/*
 * mod-statbonus - permanent flat additions to a character's primary stats
 *
 * The question this answers is "can a character be given +1 strength for good,
 * without an item and without a buff". Everything the server already offers
 * says no in some way:
 *
 *   - Gear and enchants are the intended route, and they leave when the item
 *     does.
 *   - The 410 passive SPELL_AURA_MOD_STAT spells in Spell.dbc do grant flat
 *     stats permanently, but they show as a buff in the frame, they are fixed
 *     amounts (the ladders run +1..+46 per stat), and two grants of the same
 *     spell refresh rather than stack because StackAmount is 0.
 *   - Writing the create stats directly does present as base stat, and is then
 *     thrown away: InitStatsForLevel rewrites them from player_class_stats and
 *     player_race_stats at every login and every level-up.
 *
 * So the bonus is not stored on the character at all. It is applied at the last
 * moment of the calculation, through a core hook added for it:
 *
 *   value = ((BASE_VALUE * BASE_PCT) + TOTAL_VALUE) * TOTAL_PCT
 *   -> OnPlayerCalculateStat(player, stat, value)      <- here
 *   -> SetStat(stat, int32(value))
 *
 * That fires on every recalculation - login, level-up, equipping, any aura
 * change - so the module answers with the character's current bonus each time
 * and never has to re-apply anything. Because it lands before SetStat and after
 * everything else, the client shows it as base stat, white text, with no buff
 * icon, and the derived stats that read the stat field (health from stamina,
 * spell power from intellect, and so on) pick it up for free.
 */

#include "StatBonusStore.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Language.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "StringFormat.h"

#include <string>

using namespace Acore::ChatCommands;

namespace
{
    struct Config
    {
        bool   Enable = true;
        int32  Limit  = 0;   // 0 = no limit
    };

    Config cfg;
    StatBonus::Store store;

    void LoadConfig()
    {
        cfg.Enable = sConfigMgr->GetOption<bool>("StatBonus.Enable", true);
        cfg.Limit  = sConfigMgr->GetOption<int32>("StatBonus.Limit", 0);
    }

    // Returns the number of rows kept.
    uint32 LoadBonuses()
    {
        store.Reset();

        QueryResult result = CharacterDatabase.Query("SELECT `Guid`, `Stat`, `Amount` FROM `character_stat_bonus`");
        if (!result)
        {
            LOG_INFO("module", "mod-statbonus: no stat bonuses are granted.");
            return 0;
        }

        uint32 kept = 0;
        uint32 skipped = 0;

        do
        {
            Field* fields = result->Fetch();

            uint32 const guid   = fields[0].Get<uint32>();
            uint8  const stat   = fields[1].Get<uint8>();
            int32  const amount = fields[2].Get<int32>();

            if (stat >= StatBonus::STAT_COUNT)
            {
                LOG_ERROR("module", "mod-statbonus: character {} has a bonus for stat {}, which is not a stat; skipped.", guid, stat);
                ++skipped;
                continue;
            }

            if (amount == 0)
                continue;

            store.Set(guid, stat, amount);
            ++kept;
        } while (result->NextRow());

        LOG_INFO("module", "mod-statbonus: loaded {} stat bonus(es) for {} character(s){}.",
            kept, store.Size(), skipped ? Acore::StringFormat(", skipped {} bad row(s)", skipped) : "");

        return kept;
    }

    void Persist(uint32 guid, std::size_t stat, int32 amount)
    {
        if (amount == 0)
        {
            CharacterDatabase.Execute("DELETE FROM `character_stat_bonus` WHERE `Guid` = {} AND `Stat` = {}", guid, uint32(stat));
            return;
        }

        CharacterDatabase.Execute("REPLACE INTO `character_stat_bonus` (`Guid`, `Stat`, `Amount`) VALUES ({}, {}, {})",
            guid, uint32(stat), amount);
    }

    // The character sheet only changes when the stats are recalculated, and the
    // hook is not going to fire on its own for a character standing still.
    void Refresh(Player* player)
    {
        if (player)
            player->UpdateAllStats();
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
        { PLAYERHOOK_ON_CALCULATE_STAT }) { }

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
        handler->PSendSysMessage("mod-statbonus: reloaded {} bonus(es) for {} character(s). Online characters refresh on their next stat update.",
            count, store.Size());
        return true;
    }

    static bool HandleStatBonusAddCommand(ChatHandler* handler, Optional<PlayerIdentifier> target, std::string statName, int32 amount)
    {
        return Grant(handler, std::move(target), statName, amount, true);
    }

    static bool HandleStatBonusSetCommand(ChatHandler* handler, Optional<PlayerIdentifier> target, std::string statName, int32 amount)
    {
        return Grant(handler, std::move(target), statName, amount, false);
    }

    static bool HandleStatBonusClearCommand(ChatHandler* handler, Optional<PlayerIdentifier> target, Optional<std::string> statName)
    {
        if (!Resolve(handler, target))
            return false;

        uint32 const guid = target->GetGUID().GetCounter();

        if (statName)
        {
            auto const stat = StatBonus::ParseStat(*statName);
            if (!stat)
            {
                handler->PSendSysMessage("Unknown stat \"{}\". Use strength, agility, stamina, intellect or spirit.", *statName);
                handler->SetSentErrorMessage(true);
                return false;
            }

            if (!store.Clear(guid, *stat))
            {
                handler->PSendSysMessage("{} has no {} bonus to clear.", target->GetName(), StatBonus::StatName(*stat));
                return true;
            }

            Persist(guid, *stat, 0);
            Refresh(target->GetConnectedPlayer());
            handler->PSendSysMessage("Cleared {}'s {} bonus.", target->GetName(), StatBonus::StatName(*stat));
            return true;
        }

        if (!store.Clear(guid))
        {
            handler->PSendSysMessage("{} has no stat bonuses to clear.", target->GetName());
            return true;
        }

        CharacterDatabase.Execute("DELETE FROM `character_stat_bonus` WHERE `Guid` = {}", guid);
        Refresh(target->GetConnectedPlayer());
        handler->PSendSysMessage("Cleared every stat bonus on {}.", target->GetName());
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
                handler->PSendSysMessage("No stat bonuses are granted.");
                return true;
            }

            for (auto const& [guid, bonuses] : store.All())
            {
                std::string line;
                for (std::size_t i = 0; i < StatBonus::STAT_COUNT; ++i)
                    if (bonuses[i])
                        line += Acore::StringFormat("{}{:+d} {}", line.empty() ? "" : ", ", bonuses[i], StatBonus::StatName(i));

                handler->PSendSysMessage("  guid {}: {}", guid, line);
            }

            handler->PSendSysMessage("{} character(s) with bonuses.", store.Size());
            return true;
        }

        StatBonus::Bonuses const* bonuses = store.Find(target->GetGUID().GetCounter());
        if (!bonuses)
        {
            handler->PSendSysMessage("{} has no stat bonuses.", target->GetName());
            return true;
        }

        handler->PSendSysMessage("Stat bonuses for {}:", target->GetName());
        for (std::size_t i = 0; i < StatBonus::STAT_COUNT; ++i)
            if ((*bonuses)[i])
                handler->PSendSysMessage("  {:+d} {}", (*bonuses)[i], StatBonus::StatName(i));

        return true;
    }

private:
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

    static bool Grant(ChatHandler* handler, Optional<PlayerIdentifier> target, std::string const& statName, int32 amount, bool add)
    {
        if (!Resolve(handler, target))
            return false;

        auto const stat = StatBonus::ParseStat(statName);
        if (!stat)
        {
            handler->PSendSysMessage("Unknown stat \"{}\". Use strength, agility, stamina, intellect or spirit.", statName);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const guid = target->GetGUID().GetCounter();

        int32 const wanted = add ? store.Get(guid, *stat) + amount : amount;
        int32 const total  = StatBonus::ClampToLimit(wanted, cfg.Limit);

        if (total != wanted)
            handler->PSendSysMessage("Clamped to StatBonus.Limit ({}).", cfg.Limit);

        store.Set(guid, *stat, total);
        Persist(guid, *stat, total);
        Refresh(target->GetConnectedPlayer());

        if (total == 0)
            handler->PSendSysMessage("{} now has no {} bonus.", target->GetName(), StatBonus::StatName(*stat));
        else
            handler->PSendSysMessage("{} now has {:+d} {}.", target->GetName(), total, StatBonus::StatName(*stat));

        if (!target->GetConnectedPlayer())
            handler->PSendSysMessage("They are offline; it applies when they next log in.");

        return true;
    }
};

void AddStatBonusScripts()
{
    new StatBonus_WorldScript();
    new StatBonus_PlayerScript();
    new StatBonus_CommandScript();
}
