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
#include "Config.h"
#include "DatabaseEnv.h"
#include "Language.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "Unit.h"

#include <string>
#include <unordered_map>

using namespace Acore::ChatCommands;

namespace
{
    struct Config
    {
        bool  Enable     = true;
        int32 Limit      = 0;   // 0 = no limit
        int32 SpeedLimit = 0;   // 0 = no limit, in percentage points
    };

    Config cfg;
    StatBonus::Store store;

    // What this module has actually pushed into each online character's
    // m_baseRatingValue and m_auraFlatModifiersGroup, so a change moves by the
    // difference. Cleared on logout; those contributions go with the session.
    // Indexed by slot, and only the applied slots are ever used.
    std::unordered_map<uint32, StatBonus::Bonuses> applied;

    void LoadConfig()
    {
        cfg.Enable = sConfigMgr->GetOption<bool>("StatBonus.Enable", true);
        cfg.Limit  = sConfigMgr->GetOption<int32>("StatBonus.Limit", 0);

        // Movement has its own ceiling because it is the one kind measured in
        // percentage points. A limit tuned for combat ratings, where a useful
        // grant is hundreds, would allow a character twenty times normal speed.
        cfg.SpeedLimit = sConfigMgr->GetOption<int32>("StatBonus.SpeedLimit", 0);
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
            LoadBonuses();
    }

    void OnStartup() override
    {
        LoadBonuses();
    }
};

class StatBonus_PlayerScript : public PlayerScript
{
public:
    StatBonus_PlayerScript() : PlayerScript("StatBonus_PlayerScript",
        {
            PLAYERHOOK_ON_CALCULATE_STAT,
            PLAYERHOOK_ON_CALCULATE_SPEED,
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

void AddStatBonusScripts()
{
    new StatBonus_WorldScript();
    new StatBonus_PlayerScript();
    new StatBonus_CommandScript();
}
