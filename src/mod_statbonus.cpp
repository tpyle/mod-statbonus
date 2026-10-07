/*
 * mod-statbonus - permanent flat additions to a character's stats and ratings
 *
 * The question this answers is "can a character be given +1 strength, or 20 hit
 * rating, for good, without an item and without a buff". Everything the server
 * already offers says no in some way:
 *
 *   - Gear and enchants are the intended route, and they leave when the item
 *     does.
 *   - The 410 passive SPELL_AURA_MOD_STAT spells in Spell.dbc do grant flat
 *     stats permanently, but they show as a buff in the frame, they are fixed
 *     amounts (the ladders run +1..+46 per stat), and two grants of the same
 *     spell refresh rather than stack because StackAmount is 0.
 *   - Writing the create stats directly does present as base stat, and is then
 *     thrown away: InitStatsForLevel rewrites them from player_class_stats and
 *     player_race_stats at every login and on every level-up.
 *
 * The two kinds of bonus reach the character by different routes, because the
 * server holds them differently.
 *
 * PRIMARY STATS go through a hook added to the core for it, at the last moment
 * of the calculation:
 *
 *     value = ((BASE_VALUE * BASE_PCT) + TOTAL_VALUE) * TOTAL_PCT
 *     -> OnPlayerCalculateStat(player, stat, value)     <- here
 *     -> SetStat(stat, int32(value))
 *
 * That fires on every recalculation - login, level-up, equipping, any aura
 * change - so the module answers with the character's current bonus each time
 * and never has to re-apply anything. Landing there is what makes it read as
 * base stat: white text on the character sheet, no buff icon, and everything
 * derived from the stat field (health from stamina, spell power from intellect,
 * attack power from strength) picks it up for free.
 *
 * COMBAT RATINGS go through Player::ApplyRatingMod, which is where gear puts
 * them, rather than through a second hook of the same shape. The reason is
 * haste: CR_HASTE_MELEE, _RANGED and _SPELL only affect attack and cast speed
 * because ApplyRatingMod reaches into ApplyAttackTimePercentMod and
 * ApplyCastTimePercentMod as it changes m_baseRatingValue. A hook inside
 * UpdateRating - which is where the equivalent last-moment hook would have to
 * go - would put the number on the sheet and leave the character no faster.
 *
 * RESISTANCES, and armor with them, go through Unit::HandleStatFlatModifier on
 * TOTAL_VALUE, which is again where gear puts them: Player::UpdateResistances
 * and Player::UpdateArmor both read GetFlatModifierValue(unitMod, TOTAL_VALUE)
 * as part of their sum. UNIT_MOD_RESISTANCE_START is UNIT_MOD_ARMOR, so all
 * seven schools are one uniform call, and HandleStatFlatModifier calls
 * UpdateUnitMod itself, so nothing else has to be poked afterwards.
 *
 * Note that its neighbour SetStatFlatModifier carries a comment warning that
 * use outside an aura handler loses the value when auras change. That warning
 * is about *setting* the modifier, which overwrites whatever auras had put
 * there. HandleStatFlatModifier accumulates, exactly as every item does, so a
 * contribution pushed in this way is not disturbed by auras coming and going.
 *
 * Both of those make ratings and resistances applied state, which is exactly
 * what was avoided for the stats, so it is worth saying why it is safe here:
 * nothing resets m_baseRatingValue or m_auraFlatModifiersGroup mid-session.
 * Item mods add and subtract their own deltas rather than rebuilding either,
 * and there is no equivalent of InitStatsForLevel for them. So the module
 * applies them once at login and reconciles on a change, keeping a record of
 * what it has actually pushed in so a re-grant or a reload moves by the
 * difference rather than stacking.
 */

#include "StatBonusStore.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Creature.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "LFGMgr.h"
#include "Map.h"
#include "Item.h"
#include "Mail.h"
#include "Language.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "StringConvert.h"
#include "Unit.h"
#include "WorldSession.h"

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    struct Config
    {
        bool  Enable     = true;
        int32 Limit      = 0;   // 0 = no limit
        int32 SpeedLimit = 0;   // 0 = no limit, in percentage points

        // Earning them, rather than being given them by a GM.
        bool        QuestRewards   = true;
        uint32      BrokerEntry    = 0;    // 0 = summon nobody
        uint32      BrokerSeconds  = 120;
        uint32      TokenItem      = 0;    // 0 = hand out nothing
        uint32      TokenCount     = 1;
        // Drop chances, as a percent rolled per player. 0 switches a source
        // off, so there is one dial per source and no separate enable.
        uint32      TokenChance        = 100;  // the named creatures below
        uint32      TokenDungeonChance = 0;    // final boss of a normal dungeon
        uint32      TokenHeroicChance  = 0;    // final boss of a heroic dungeon
        uint32      TokenRaidChance    = 0;    // final boss of a raid
        bool        TokenSkipBots      = true;
        std::string TokenCreatures;        // comma separated creature entries
    };

    Config cfg;
    StatBonus::Store store;

    // Calls the broker up next to the player. Shared by the two ways of asking
    // for one: taking the quest from the token, and clicking the token again
    // afterwards.
    bool SummonBroker(Player* player)
    {
        if (!cfg.BrokerEntry || !player)
            return false;

        if (player->IsInCombat())
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "Not while you are fighting. Try again when it is quiet.");
            return false;
        }

        float x, y, z;
        player->GetClosePoint(x, y, z, player->GetCombatReach() / 3.0f, 2.0f);

        return player->SummonCreature(cfg.BrokerEntry, x, y, z, player->GetOrientation(),
            TEMPSUMMON_TIMED_DESPAWN, cfg.BrokerSeconds * IN_MILLISECONDS) != nullptr;
    }

    constexpr char const* MAIL_SUBJECT_TOKEN = "Your share";
    constexpr char const* MAIL_BODY_TOKEN =
        "Your bags were full when this was handed out, so it was sent on instead.";

    // What this module has actually pushed into each online character's
    // m_baseRatingValue and m_auraFlatModifiersGroup, so a change moves by the
    // difference. Cleared on logout; those contributions go with the session.
    // Indexed by slot, and only the applied slots are ever used.
    std::unordered_map<uint32, StatBonus::Bonuses> applied;

    // questId -> what it can grant. Read at startup and on reload, so the pool
    // is tuned with an UPDATE and a ".statbonus reload" rather than a build.
    std::unordered_map<uint32, std::vector<StatBonus::QuestReward>> questRewards;

    // The creatures that hand out a token when they die.
    std::unordered_set<uint32> tokenCreatures;

    // The roll that has already been shown to a player, by character guid and
    // then quest id, as an index into that quest's pool.
    //
    // It exists because the turn-in box names the stat, which means the roll
    // has to happen while that box is being built - one step before the reward
    // is actually granted. Keeping it here also settles what happens when the
    // player closes the box and reopens it: they see the same answer, rather
    // than being able to re-roll by clicking away.
    std::unordered_map<uint32, std::unordered_map<uint32, std::size_t>> shownRoll;

    void LoadConfig()
    {
        cfg.Enable = sConfigMgr->GetOption<bool>("StatBonus.Enable", true);
        cfg.Limit  = sConfigMgr->GetOption<int32>("StatBonus.Limit", 0);

        // Movement has its own ceiling because it is the one kind measured in
        // percentage points. A limit tuned for combat ratings, where a useful
        // grant is hundreds, would allow a character twenty times normal speed.
        cfg.SpeedLimit = sConfigMgr->GetOption<int32>("StatBonus.SpeedLimit", 0);

        cfg.QuestRewards  = sConfigMgr->GetOption<bool>("StatBonus.QuestRewards", true);
        cfg.TokenChance        = sConfigMgr->GetOption<uint32>("StatBonus.Token.Chance", 100);
        cfg.TokenDungeonChance = sConfigMgr->GetOption<uint32>("StatBonus.Token.DungeonChance", 0);
        cfg.TokenHeroicChance  = sConfigMgr->GetOption<uint32>("StatBonus.Token.HeroicChance", 0);
        cfg.TokenRaidChance    = sConfigMgr->GetOption<uint32>("StatBonus.Token.RaidChance", 0);
        cfg.TokenSkipBots      = sConfigMgr->GetOption<bool>("StatBonus.Token.SkipBots", true);
        cfg.BrokerEntry   = sConfigMgr->GetOption<uint32>("StatBonus.Broker.Entry", 0);
        cfg.BrokerSeconds = sConfigMgr->GetOption<uint32>("StatBonus.Broker.DespawnSeconds", 120);
        cfg.TokenItem     = sConfigMgr->GetOption<uint32>("StatBonus.Token.Item", 0);
        cfg.TokenCount    = sConfigMgr->GetOption<uint32>("StatBonus.Token.Count", 1);
        cfg.TokenCreatures = sConfigMgr->GetOption<std::string>("StatBonus.Token.Creatures", "");

        tokenCreatures.clear();
        for (std::string_view const field : Acore::Tokenize(cfg.TokenCreatures, ',', false))
            if (Optional<uint32> const entry = Acore::StringTo<uint32>(field))
                tokenCreatures.insert(*entry);
            else
                LOG_ERROR("module", "mod-statbonus: '{}' in StatBonus.Token.Creatures is not a creature entry", field);
    }

    // Bring a connected character's ratings and resistances in line with the
    // store, by the difference. Also the revert path: with the module disabled,
    // or every grant cleared, the desired amount is zero and this takes the
    // contributions back out again.
    void ReconcileApplied(Player* player)
    {
        if (!player)
            return;

        uint32 const guid = player->GetGUID().GetCounter();

        StatBonus::Bonuses const* bonuses = cfg.Enable ? store.Find(guid) : nullptr;
        auto const it = applied.find(guid);

        if (!bonuses && it == applied.end())
            return;

        StatBonus::Bonuses& have = it != applied.end() ? it->second : applied[guid];

        bool anyLeft = false;

        for (std::size_t slot = StatBonus::RATING_FIRST; slot < StatBonus::SLOT_COUNT; ++slot)
        {
            int32 const want  = bonuses ? (*bonuses)[slot] : 0;
            int32 const delta = want - have[slot];

            if (delta)
            {
                if (StatBonus::IsRating(slot))
                {
                    // A signed value with apply=true is how ApplyRatingMod
                    // takes a decrease as well; the haste branch reads the old
                    // and new totals either way round.
                    player->ApplyRatingMod(CombatRating(StatBonus::RatingIndex(slot)), delta, true);
                }
                else
                {
                    // UNIT_MOD_RESISTANCE_START is UNIT_MOD_ARMOR, so school 0
                    // is armor and needs no special case. This accumulates and
                    // calls UpdateUnitMod itself.
                    uint32 const school = StatBonus::ResistanceSchool(slot);
                    player->HandleStatFlatModifier(UnitMods(UNIT_MOD_RESISTANCE_START + school),
                        TOTAL_VALUE, float(delta), true);
                }

                have[slot] = want;
            }

            if (want)
                anyLeft = true;
        }

        if (!anyLeft)
            applied.erase(guid);
    }

    // Returns the number of rows kept.
    uint32 LoadBonuses()
    {
        store.Reset();

        QueryResult result = CharacterDatabase.Query("SELECT `Guid`, `Kind`, `Id`, `Amount` FROM `character_stat_bonus`");
        if (!result)
        {
            LOG_INFO("module", "mod-statbonus: no bonuses are granted.");
            return 0;
        }

        uint32 kept = 0;
        uint32 skipped = 0;

        do
        {
            Field* fields = result->Fetch();

            uint32 const guid   = fields[0].Get<uint32>();
            uint8  const kind   = fields[1].Get<uint8>();
            uint8  const id     = fields[2].Get<uint8>();
            int32  const amount = fields[3].Get<int32>();

            auto const slot = StatBonus::SlotOf(kind, id);
            if (!slot)
            {
                LOG_ERROR("module", "mod-statbonus: character {} has a bonus for kind {} id {}, which is not a stat, a rating or a resistance; skipped.",
                    guid, kind, id);
                ++skipped;
                continue;
            }

            if (amount == 0)
                continue;

            store.Set(guid, *slot, amount);
            ++kept;
        } while (result->NextRow());

        LOG_INFO("module", "mod-statbonus: loaded {} bonus(es) for {} character(s){}.",
            kept, store.Size(), skipped ? Acore::StringFormat(", skipped {} bad row(s)", skipped) : "");

        return kept;
    }

    bool IsBot(Player const* player)
    {
        // IsHeadless and not IsBot: upstream's headless session work renamed
        // the idea, and the predicate is the same one - a session with no
        // socket behind it, which is what mod-playerbots fabricates.
        return player && player->GetSession() && player->GetSession()->IsHeadless();
    }

    void GiveToken(Player* player)
    {
        if (!player)
            return;

        ItemPosCountVec dest;
        InventoryResult const canStore = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, cfg.TokenItem, cfg.TokenCount);

        if (canStore != EQUIP_ERR_OK)
        {
            // Mailed rather than dropped on the floor, so a full bag is an
            // inconvenience and not a lost reward.
            Item* item = Item::CreateItem(cfg.TokenItem, cfg.TokenCount, player);
            if (!item)
                return;

            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            item->SaveToDB(trans);
            MailDraft(MAIL_SUBJECT_TOKEN, MAIL_BODY_TOKEN).AddItem(item)
                .SendMailTo(trans, MailReceiver(player), MailSender(MAIL_CREATURE, cfg.BrokerEntry));
            CharacterDatabase.CommitTransaction(trans);
            return;
        }

        if (Item* item = player->StoreNewItem(dest, cfg.TokenItem, true))
            player->SendNewItem(item, cfg.TokenCount, true, false);
    }

    // One player's roll for one kill.
    //
    // Rolled per player rather than once for the group, so a chance below 100
    // means each person's own luck rather than everybody sharing one result.
    // At 100 this is the old behaviour exactly.
    void MaybeGiveToken(Player* player, uint32 chance)
    {
        if (!player || !chance)
            return;

        if (cfg.TokenSkipBots && IsBot(player))
            return;             // it would never be turned in

        if (chance < 100 && urand(1, 100) > chance)
            return;

        GiveToken(player);
    }

    uint32 LoadQuestRewards()
    {
        questRewards.clear();

        QueryResult result = WorldDatabase.Query(
            "SELECT `QuestId`, `Kind`, `Id`, `Amount`, `Weight` FROM `statbonus_quest_reward`");

        if (!result)
        {
            LOG_INFO("module", "mod-statbonus: no quest reward pools are configured.");
            return 0;
        }

        uint32 rows = 0;
        uint32 skipped = 0;

        do
        {
            Field* fields = result->Fetch();

            uint32 const questId = fields[0].Get<uint32>();
            uint8  const kind    = fields[1].Get<uint8>();
            uint8  const id      = fields[2].Get<uint8>();
            int32  const amount  = fields[3].Get<int32>();
            uint32 const weight  = fields[4].Get<uint32>();

            auto const slot = StatBonus::SlotOf(kind, id);
            if (!slot)
            {
                LOG_ERROR("module", "mod-statbonus: quest {} rewards kind {} id {}, which is not a stat, "
                    "rating, resistance or movement type; skipped.", questId, kind, id);
                ++skipped;
                continue;
            }

            if (StatBonus::IsMovement(*slot) && !StatBonus::IsAdjustableMovement(*slot))
            {
                LOG_ERROR("module", "mod-statbonus: quest {} rewards {}, which the core does not recalculate, "
                    "so it could never take effect; skipped.", questId, StatBonus::SlotName(*slot));
                ++skipped;
                continue;
            }

            questRewards[questId].push_back({ *slot, amount, weight });
            ++rows;
        } while (result->NextRow());

        for (auto const& [questId, pool] : questRewards)
            if (!StatBonus::TotalWeight(pool))
                LOG_ERROR("module", "mod-statbonus: quest {} has {} reward row(s) and they all have weight 0, "
                    "so it will grant nothing.", questId, pool.size());

        LOG_INFO("module", "mod-statbonus: loaded {} reward row(s) across {} quest(s){}.",
            rows, questRewards.size(), skipped ? Acore::StringFormat(", skipped {} bad row(s)", skipped) : "");

        return rows;
    }

    void Persist(uint32 guid, std::size_t slot, int32 amount)
    {
        uint32 const kind = StatBonus::KindOf(slot);
        uint32 const id   = StatBonus::IndexOf(slot);

        if (amount == 0)
        {
            CharacterDatabase.Execute("DELETE FROM `character_stat_bonus` WHERE `Guid` = {} AND `Kind` = {} AND `Id` = {}",
                guid, kind, id);
            return;
        }

        CharacterDatabase.Execute("REPLACE INTO `character_stat_bonus` (`Guid`, `Kind`, `Id`, `Amount`) VALUES ({}, {}, {}, {})",
            guid, kind, id, amount);
    }

    // Give a slot a new total, persist it, and make it visible. The command
    // path and the quest path both end here, so they cannot drift.
    int32 GrantSlot(Player* player, uint32 guid, std::size_t slot, int32 total)
    {
        store.Set(guid, slot, total);
        Persist(guid, slot, total);
        return total;
    }

    // The character sheet only changes when the numbers are recalculated, and
    // neither route fires on its own for a character standing still.
    void Refresh(Player* player)
    {
        if (!player)
            return;

        ReconcileApplied(player);
        player->UpdateAllStats();

        // Nothing recalculates a speed on its own for a character standing
        // still, and the hook only runs when something asks. Skipping the two
        // that UpdateSpeed rejects, which otherwise logged an error apiece on
        // every grant.
        for (std::size_t type = 0; type < StatBonus::MOVEMENT_COUNT; ++type)
            if (StatBonus::IsAdjustableMovement(StatBonus::MovementSlot(type)))
                player->UpdateSpeed(UnitMoveType(type), true);
    }
}

class StatBonus_WorldScript : public WorldScript
{
public:
    StatBonus_WorldScript() : WorldScript("StatBonus_WorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        LoadConfig();

        // The characters database is not connected yet at startup config load,
        // so the table is read from OnStartup instead.
        if (reload)
        {
            LoadBonuses();
            LoadQuestRewards();
        }
    }

    void OnStartup() override
    {
        LoadBonuses();
        LoadQuestRewards();
    }
};

class StatBonus_PlayerScript : public PlayerScript
{
public:
    StatBonus_PlayerScript() : PlayerScript("StatBonus_PlayerScript",
        {
            PLAYERHOOK_ON_CALCULATE_STAT,
            PLAYERHOOK_ON_CALCULATE_SPEED,
            PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
            PLAYERHOOK_ON_QUEST_OFFER_REWARD_TEXT,
            PLAYERHOOK_ON_PLAYER_QUEST_ACCEPT,
            PLAYERHOOK_ON_CREATURE_KILL,
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT
        }) { }

    void OnPlayerCalculateStat(Player* player, Stats stat, float& value) override
    {
        // This runs five times per recalculation for every character on the
        // realm, so the common case - nobody has been granted anything - has to
        // be one branch.
        if (!cfg.Enable || store.Empty() || !player)
            return;

        int32 const bonus = store.Get(player->GetGUID().GetCounter(), std::size_t(stat));
        if (!bonus)
            return;

        value = StatBonus::Apply(value, bonus);
    }

    void OnPlayerCalculateSpeed(Player* player, UnitMoveType type, float& rate) override
    {
        // Same shape as the stat hook, and for the same reason: UpdateSpeed
        // recomputes the rate from the auras whenever any of them changes -
        // mounting included - so there is nowhere a lasting adjustment could
        // be written and this has to answer afresh each time.
        if (!cfg.Enable || store.Empty() || !player)
            return;

        int32 const percent = store.Get(player->GetGUID().GetCounter(), StatBonus::MovementSlot(std::size_t(type)));
        if (!percent)
            return;

        rate = StatBonus::ApplyMovement(rate, percent);
    }

    // Rolls the pool for a quest, or returns the roll already shown to this
    // player for it. Nothing is granted here.
    std::optional<std::size_t> RollFor(Player* player, uint32 questId)
    {
        auto const pool = questRewards.find(questId);
        if (pool == questRewards.end())
            return std::nullopt;

        uint32 const guid = player->GetGUID().GetCounter();

        auto const byQuest = shownRoll.find(guid);
        if (byQuest != shownRoll.end())
        {
            auto const already = byQuest->second.find(questId);
            if (already != byQuest->second.end())
                return already->second;
        }

        uint32 const total = StatBonus::TotalWeight(pool->second);
        if (!total)
            return std::nullopt;        // already shouted about at load time

        auto const picked = StatBonus::PickByWeight(pool->second, urand(0, total - 1));
        if (picked)
            shownRoll[guid][questId] = *picked;

        return picked;
    }

    // Puts the rolled stat into the turn-in box.
    //
    // This is the only place it can go. quest_offer_reward.RewardText is one
    // static string shared by everybody, and the packet carrying it is the
    // last thing sent before the reward is handed over, so the roll has to be
    // decided here rather than in OnPlayerCompleteQuest, which runs after the
    // player has already read the text.
    //
    // %stat is substituted; a pool quest whose text forgot the token gets the
    // answer appended rather than losing it silently.
    void OnPlayerQuestOfferRewardText(Player* player, Quest const* quest, std::string& text) override
    {
        if (!cfg.Enable || !cfg.QuestRewards || !player || !quest)
            return;

        auto const picked = RollFor(player, quest->GetQuestId());
        if (!picked)
            return;

        StatBonus::QuestReward const& reward = questRewards[quest->GetQuestId()][*picked];
        bool const isSpeed = StatBonus::IsMovement(reward.slot);

        std::string const named = Acore::StringFormat("{:+d}{} {}",
            reward.amount, isSpeed ? "%" : "", StatBonus::SlotName(reward.slot));

        std::string::size_type const token = text.find("%stat");
        if (token != std::string::npos)
            text.replace(token, 5, named);
        else
            text.append("$B$BWhat it was carrying: ").append(named).append(".");
    }

    // Earned, rather than granted by a GM: the quest is turned in and the roll
    // the player was shown in the turn-in box is paid out.
    //
    // This is the end of Player::RewardQuest rather than the moment the
    // objectives were met, which is what makes it the right hook - by now the
    // token has been taken and the quest's own rewards handed over, so there is
    // no window where somebody holds both the bonus and the item.
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (!cfg.Enable || !cfg.QuestRewards || !player || !quest)
            return;

        // Normally this is the roll the player was just shown. It rolls fresh
        // only for a turn-in that never drew the box at all.
        auto const picked = RollFor(player, quest->GetQuestId());
        if (!picked)
            return;

        StatBonus::QuestReward const& reward = questRewards[quest->GetQuestId()][*picked];

        uint32 const guid    = player->GetGUID().GetCounter();
        bool   const isSpeed = StatBonus::IsMovement(reward.slot);
        int32  const limit   = isSpeed ? cfg.SpeedLimit : cfg.Limit;
        int32  const wanted  = store.Get(guid, reward.slot) + reward.amount;
        int32  const capped  = StatBonus::ClampToLimit(wanted, limit);

        shownRoll[guid].erase(quest->GetQuestId());   // spent; the next one rolls again

        if (capped == store.Get(guid, reward.slot))
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "You are already at the limit for {}, so nothing was added.", StatBonus::SlotName(reward.slot));
            return;
        }

        GrantSlot(player, guid, reward.slot, capped);
        Refresh(player);

        ChatHandler(player->GetSession()).PSendSysMessage("{}{:+d}{} {}|r - now {:+d}{} in total.",
            "|cff40ff40", reward.amount, isSpeed ? "%" : "", StatBonus::SlotName(reward.slot),
            capped, isSpeed ? "%" : "");
    }

    // The token, to everybody who was there.
    //
    // Loot cannot do this: a quest item only drops for somebody already on the
    // quest, and an ordinary item drops once for one looter. Handing it over
    // here gives every group member in the instance a copy whether or not they
    // have the quest, which is what makes the thing repeatable in company.
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!cfg.Enable || !cfg.TokenItem || !killer || !killed)
            return;

        if (!tokenCreatures.count(killed->GetEntry()))
            return;

        Group* group = killer->GetGroup();
        if (!group)
        {
            MaybeGiveToken(killer, cfg.TokenChance);
            return;
        }

        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* member = itr->GetSource())
                if (member->GetMap() == killed->GetMap())
                    MaybeGiveToken(member, cfg.TokenChance);
    }

    // The broker, called up when the quest is taken.
    //
    // The token starts the quest by itself - item_template.StartQuest, the way
    // the Darkmoon decks do it - so accepting is the moment the player has
    // asked for somebody to hand it to. Summoning here rather than from the
    // item's own use means no spell has to exist for it, and so nothing has to
    // be added to Spell.dbc or shipped to the client.
    void OnPlayerQuestAccept(Player* player, Quest const* quest) override
    {
        if (!cfg.Enable || !player || !quest)
            return;

        if (!questRewards.count(quest->GetQuestId()))
            return;

        SummonBroker(player);
    }

    void OnPlayerLogin(Player* player) override
    {
        // Ratings and resistances are applied state and go with the session, so
        // they are put back on at every login. Stats need nothing here - their
        // hook has already fired several times by this point.
        if (store.AnyApplied())
            ReconcileApplied(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        applied.erase(player->GetGUID().GetCounter());
        shownRoll.erase(player->GetGUID().GetCounter());
    }
};

class StatBonus_CommandScript : public CommandScript
{
public:
    StatBonus_CommandScript() : CommandScript("StatBonus_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable statBonusCommandTable =
        {
            { "add",    HandleStatBonusAddCommand,    SEC_ADMINISTRATOR, Console::Yes },
            { "set",    HandleStatBonusSetCommand,    SEC_ADMINISTRATOR, Console::Yes },
            { "clear",  HandleStatBonusClearCommand,  SEC_ADMINISTRATOR, Console::Yes },
            { "list",   HandleStatBonusListCommand,   SEC_GAMEMASTER,    Console::Yes },
            { "names",  HandleStatBonusNamesCommand,  SEC_GAMEMASTER,    Console::Yes },
            { "reload", HandleStatBonusReloadCommand, SEC_ADMINISTRATOR, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "statbonus", statBonusCommandTable }
        };

        return commandTable;
    }

    static bool HandleStatBonusReloadCommand(ChatHandler* handler)
    {
        uint32 const count = LoadBonuses();
        LoadQuestRewards();

        // Ratings and resistances are applied state, so a reload has to walk
        // the characters already on and move them to whatever the table says.
        //
        // ObjectAccessor rather than WorldSessionMgr::GetAllSessions(), which
        // looks like the obvious choice and is wrong on a playerbot realm:
        // mod-playerbots never calls AddSession for its fabricated sessions, so
        // that map holds only the real clients - "Connection peak: 0" with 500
        // bots in the world - and this loop reported refreshing nobody.
        uint32 reconciled = 0;
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (!player || !player->IsInWorld())
                continue;

            ReconcileApplied(player);
            player->UpdateAllStats();
            ++reconciled;
        }

        handler->PSendSysMessage("mod-statbonus: reloaded {} bonus(es) for {} character(s); refreshed {} online.",
            count, store.Size(), reconciled);
        return true;
    }

    static bool HandleStatBonusNamesCommand(ChatHandler* handler)
    {
        PrintNames(handler, "Primary stats:", 0, StatBonus::RATING_FIRST);
        PrintNames(handler, "Combat ratings:", StatBonus::RATING_FIRST, StatBonus::RESISTANCE_FIRST);
        PrintNames(handler, "Resistances:", StatBonus::RESISTANCE_FIRST, StatBonus::MOVEMENT_FIRST);
        // Only the ones that can actually be adjusted; turn rate and pitch
        // rate have slots for the sake of the enum and are refused on grant.
        {
            handler->PSendSysMessage("Movement (an amount here is a PERCENTAGE):");
            std::string line;
            for (std::size_t slot = StatBonus::MOVEMENT_FIRST; slot < StatBonus::SLOT_COUNT; ++slot)
                if (StatBonus::IsAdjustableMovement(slot))
                    line += Acore::StringFormat("{}{}", line.empty() ? "  " : ", ", StatBonus::SlotName(slot));

            handler->PSendSysMessage("{}", line);
        }

        handler->PSendSysMessage("Also str/agi/sta/int/spi, melee_hit, spell_crit, arp, bare school names like fire,");
        handler->PSendSysMessage("speed for run_speed, swim, fly, or stat:<n> / rating:<n> / resist:<n> / movement:<n>.");
        return true;
    }

    static bool HandleStatBonusAddCommand(ChatHandler* handler, Optional<PlayerIdentifier> target, std::string what, int32 amount)
    {
        return Grant(handler, std::move(target), what, amount, true);
    }

    static bool HandleStatBonusSetCommand(ChatHandler* handler, Optional<PlayerIdentifier> target, std::string what, int32 amount)
    {
        return Grant(handler, std::move(target), what, amount, false);
    }

    static bool HandleStatBonusClearCommand(ChatHandler* handler, Optional<PlayerIdentifier> target, Optional<std::string> what)
    {
        if (!Resolve(handler, target))
            return false;

        uint32 const guid = target->GetGUID().GetCounter();

        if (what)
        {
            auto const slot = StatBonus::ParseSlot(*what);
            if (!slot)
                return Unknown(handler, *what);

            if (!store.Clear(guid, *slot))
            {
                handler->PSendSysMessage("{} has no {} bonus to clear.", target->GetName(), StatBonus::SlotName(*slot));
                return true;
            }

            Persist(guid, *slot, 0);
            Refresh(target->GetConnectedPlayer());
            handler->PSendSysMessage("Cleared {}'s {} bonus.", target->GetName(), StatBonus::SlotName(*slot));
            return true;
        }

        if (!store.Clear(guid))
        {
            handler->PSendSysMessage("{} has no bonuses to clear.", target->GetName());
            return true;
        }

        CharacterDatabase.Execute("DELETE FROM `character_stat_bonus` WHERE `Guid` = {}", guid);
        Refresh(target->GetConnectedPlayer());
        handler->PSendSysMessage("Cleared every bonus on {}.", target->GetName());
        return true;
    }

    static bool HandleStatBonusListCommand(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        // No argument lists the whole realm rather than falling back to the
        // selected character: a realm has a handful of grants at most, so that
        // is the view worth having, and it is the one that works from the
        // console, where there is no selection to fall back to.
        if (!target)
        {
            if (store.Empty())
            {
                handler->PSendSysMessage("No bonuses are granted.");
                return true;
            }

            for (auto const& [guid, bonuses] : store.All())
                handler->PSendSysMessage("  guid {}: {}", guid, Describe(bonuses));

            handler->PSendSysMessage("{} character(s) with bonuses.", store.Size());
            return true;
        }

        StatBonus::Bonuses const* bonuses = store.Find(target->GetGUID().GetCounter());
        if (!bonuses)
        {
            handler->PSendSysMessage("{} has no bonuses.", target->GetName());
            return true;
        }

        handler->PSendSysMessage("Bonuses for {}: {}", target->GetName(), Describe(*bonuses));
        return true;
    }

private:
    // Wrapped by hand because a chat line is not wide enough for the
    // twenty-five rating names on one row.
    static void PrintNames(ChatHandler* handler, char const* heading, std::size_t first, std::size_t last)
    {
        handler->PSendSysMessage("{}", heading);

        std::string line;
        for (std::size_t slot = first; slot < last; ++slot)
        {
            line += Acore::StringFormat("{}{}", line.empty() ? "  " : ", ", StatBonus::SlotName(slot));
            if (line.size() > 140)
            {
                handler->PSendSysMessage("{}", line);
                line.clear();
            }
        }

        if (!line.empty())
            handler->PSendSysMessage("{}", line);
    }

    static std::string Describe(StatBonus::Bonuses const& bonuses)
    {
        std::string out;
        for (std::size_t slot = 0; slot < StatBonus::SLOT_COUNT; ++slot)
            if (bonuses[slot])
                out += Acore::StringFormat("{}{:+d}{} {}", out.empty() ? "" : ", ", bonuses[slot],
                    StatBonus::IsMovement(slot) ? "%" : "", StatBonus::SlotName(slot));

        return out;
    }

    static bool Unknown(ChatHandler* handler, std::string const& what)
    {
        handler->PSendSysMessage("Unknown stat, rating or resistance \"{}\". \".statbonus names\" lists them.", what);
        handler->SetSentErrorMessage(true);
        return false;
    }

    static bool Resolve(ChatHandler* handler, Optional<PlayerIdentifier>& target)
    {
        if (!target)
            target = PlayerIdentifier::FromTargetOrSelf(handler);

        if (!target)
        {
            handler->SendErrorMessage(LANG_PLAYER_NOT_FOUND);
            return false;
        }

        return true;
    }

    static bool Grant(ChatHandler* handler, Optional<PlayerIdentifier> target, std::string const& what, int32 amount, bool add)
    {
        if (!Resolve(handler, target))
            return false;

        auto const slot = StatBonus::ParseSlot(what);
        if (!slot)
            return Unknown(handler, what);

        if (StatBonus::IsMovement(*slot) && !StatBonus::IsAdjustableMovement(*slot))
        {
            handler->PSendSysMessage("{} is in UnitMoveType but is not a speed the core recalculates - "
                "Unit::UpdateSpeed rejects it, so a bonus there could never take effect.",
                StatBonus::SlotName(*slot));
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const guid = target->GetGUID().GetCounter();

        int32 const wanted = add ? store.Get(guid, *slot) + amount : amount;

        bool  const isSpeed = StatBonus::IsMovement(*slot);
        int32 const limit   = isSpeed ? cfg.SpeedLimit : cfg.Limit;
        int32 const total   = StatBonus::ClampToLimit(wanted, limit);

        if (total != wanted)
            handler->PSendSysMessage("Clamped to StatBonus.{} ({}).", isSpeed ? "SpeedLimit" : "Limit", limit);

        store.Set(guid, *slot, total);
        Persist(guid, *slot, total);
        Refresh(target->GetConnectedPlayer());

        if (total == 0)
            handler->PSendSysMessage("{} now has no {} bonus.", target->GetName(), StatBonus::SlotName(*slot));
        else if (isSpeed)
            handler->PSendSysMessage("{} now has {:+d}% {} - a rate, so {:+d} is {:+d}% of normal however they are travelling.",
                target->GetName(), total, StatBonus::SlotName(*slot), total, total);
        else if (StatBonus::IsRating(*slot) && !StatBonus::IsSkillRating(*slot))
            handler->PSendSysMessage("{} now has {:+d} {} rating.", target->GetName(), total, StatBonus::SlotName(*slot));
        else
            handler->PSendSysMessage("{} now has {:+d} {}.", target->GetName(), total, StatBonus::SlotName(*slot));

        if (!target->GetConnectedPlayer())
            handler->PSendSysMessage("They are offline; it applies when they next log in.");

        if (!cfg.Enable)
            handler->PSendSysMessage("StatBonus.Enable is 0, so it is recorded but not in effect.");

        return true;
    }
};

// The token, clicked again once the quest is already taken.
//
// It carries a spell purely so the client offers it. Whether an item is usable
// at all - the "Use:" line on the tooltip, and whether a right-click sends
// CMSG_USE_ITEM - is decided by the client alone, out of its own Spell.dbc.
// The spell this feature actually wants lives in the spell_dbc world table,
// which is a server-side overlay the client never sees, so a token carrying it
// showed no Use: line and right-clicked into nothing. Confirmed rather than
// assumed: a probe item identical to the token but carrying a stock spell id
// did show the line.
//
// So the item's spell is a doorbell. OnUse returns true, which stops
// WorldSession::HandleUseItemOpcode before CastItemUseSpell, and whatever that
// spell would really have done never happens. Nothing has to be added to the
// client's Spell.dbc and nothing has to be shipped to anybody.
class StatBonus_TokenItemScript : public ItemScript
{
public:
    StatBonus_TokenItemScript() : ItemScript("item_statbonus_token") { }

    bool OnUse(Player* player, Item* item, SpellCastTargets const& /*targets*/) override
    {
        if (!player || !item)
            return false;

        // The handler's own warning: a script that stops the cast has to tell
        // the client, or the item sits greyed out as though still being used.
        player->SendEquipError(EQUIP_ERR_NONE, item, nullptr);

        if (!cfg.Enable || !cfg.QuestRewards || !cfg.BrokerEntry)
            return true;

        if (player->FindNearestCreature(cfg.BrokerEntry, 30.0f))
        {
            ChatHandler(player->GetSession()).PSendSysMessage("Already here, and waiting.");
            return true;
        }

        if (!SummonBroker(player))
            return true;              // it said why

        // The cooldown CastItemUseSpell would have applied. Read off the item
        // rather than named here, so spellcooldown_1 in item_template stays
        // the one place it is set.
        if (SpellInfo const* doorbell = sSpellMgr->GetSpellInfo(item->GetTemplate()->Spells[0].SpellId))
            player->SendCooldownEvent(doorbell, item->GetEntry());

        return true;
    }
};

// The token, from the final boss of a dungeon.
//
// Reuses the core's own idea of "last boss", rather than a list of creature
// entries that would have to be kept by hand: instance_encounters carries a
// lastEncounterDungeon column naming the LFG dungeon an encounter finishes,
// and Map::UpdateEncounterState is where that is read - it is what tells the
// dungeon finder to pay out the random-dungeon bag. The hook fires right
// beside that payout with the same dungeon id, so anything the finder would
// call a completed dungeon is exactly what is caught here.
//
// 127 encounters are marked final in this database, which is why the type is
// checked rather than assumed: 60 are normal dungeons, 32 are heroics, and 35
// are raids with Naxxramas and Icecrown among them. Each bucket has its own
// chance, and 0 for raids keeps Arthas out of it until asked otherwise.
//
// Deliberately not gated on the hook's `updated` flag. That is only ever set
// when the boss sat under an InstanceScript, and most of the classic dungeons
// have none - gating on it would have quietly excluded the older half of the
// list, which is the half most of these bosses are in.
class StatBonus_GlobalScript : public GlobalScript
{
public:
    StatBonus_GlobalScript() : GlobalScript("StatBonus_GlobalScript",
        {
            GLOBALHOOK_ON_AFTER_UPDATE_ENCOUNTER_STATE
        }) { }

    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType /*type*/, uint32 /*creditEntry*/,
        Unit* /*source*/, Difficulty /*difficulty*/, DungeonEncounterList const* /*encounters*/,
        uint32 dungeonCompleted, bool /*updated*/) override
    {
        if (!cfg.Enable || !cfg.TokenItem || !map || !dungeonCompleted)
            return;

        lfg::LFGDungeonData const* dungeon = sLFGMgr->GetLFGDungeon(dungeonCompleted);
        if (!dungeon)
            return;

        uint32 chance = 0;
        switch (dungeon->type)
        {
            case lfg::LFG_TYPE_DUNGEON:
                chance = cfg.TokenDungeonChance;
                break;
            case lfg::LFG_TYPE_HEROIC:
                chance = cfg.TokenHeroicChance;
                break;
            case lfg::LFG_TYPE_RAID:
                chance = cfg.TokenRaidChance;
                break;
            default:
                return;
        }

        if (!chance)
            return;

        // Everybody on the map, not everybody in the group: inside an instance
        // those are the same people, and this way somebody who came in ungrouped
        // is not skipped.
        Map::PlayerList const& players = map->GetPlayers();
        for (Map::PlayerList::const_iterator itr = players.begin(); itr != players.end(); ++itr)
            if (Player* player = itr->GetSource())
                MaybeGiveToken(player, chance);
    }
};

void AddStatBonusScripts()
{
    new StatBonus_WorldScript();
    new StatBonus_PlayerScript();
    new StatBonus_CommandScript();
    new StatBonus_TokenItemScript();
    new StatBonus_GlobalScript();
}
