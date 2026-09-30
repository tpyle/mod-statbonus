# mod-statbonus

Permanent flat additions to a character's five primary stats, shown as base
stat.

## Why it needs a core hook

"Give this character +1 strength for good, without an item and without a buff"
has no answer in the stock server:

* **Gear and enchants** are the intended route and leave when the item does.
  Item stats land in `BASE_VALUE`, enchants in `TOTAL_VALUE`, both through
  `m_auraFlatModifiersGroup`, which is a plain accumulator - they stack without
  a cap, but they are tied to the item.
* **Passive stat auras** do grant flat stats permanently: 410 spells in
  Spell.dbc apply `SPELL_AURA_MOD_STAT`, none of them scale with level, and
  spell 7464 is exactly +1 strength. But they show in the buff frame, they come
  in fixed ladders (+1..+46 per stat, with Stamina also offering +50 and +75),
  and a second cast of the same spell refreshes rather than stacks because
  `StackAmount` is 0.
* **Writing the create stats** does present as base stat, and is then thrown
  away. `InitStatsForLevel` rewrites them from `player_class_stats` and
  `player_race_stats` at every login and every level-up, so a module that wrote
  them would have to chase both.

So the bonus is never stored on the character. It is applied at the last moment
of the calculation, through a hook added to the core for it:

    value = ((BASE_VALUE * BASE_PCT) + TOTAL_VALUE) * TOTAL_PCT
    -> OnPlayerCalculateStat(player, stat, value)      <- here
    -> SetStat(stat, int32(value))

That fires on every recalculation - login, level-up, equipping, any aura change
- which is the point of it: the module answers with the character's current
bonus each time rather than writing a derived value somewhere and re-applying
it afterwards. Landing before `SetStat` and after everything else is what makes
it read as base stat: white text on the character sheet, no buff icon, and
everything derived from the stat field (health from stamina, spell power from
intellect, attack power from strength) picks it up for free.

The hook is `PLAYERHOOK_ON_CALCULATE_STAT` /
`PlayerScript::OnPlayerCalculateStat(Player*, Stats, float&)`, and it is called
from both `Player::UpdateStats` and `Player::UpdateAllStats` - the latter
inlines the same calculation rather than calling the former, so both sites need
it.

## The table

`character_stat_bonus` in the characters database, one row per character per
stat: `Guid`, `Stat` (0 strength, 1 agility, 2 stamina, 3 intellect, 4 spirit),
`Amount` (may be negative), and a `Comment` for the GM. A bonus of zero is
deleted rather than stored, so the table is a list of what has actually been
granted.

It ships with the module as `data/sql/db-characters/base/`, and the core's
database updater applies it at startup:
`UpdateFetcher::ReceiveIncludedDirectories` walks
`modules/<name>/data/sql/db-characters`. Nothing has to be applied by hand. The
file is `CREATE TABLE IF NOT EXISTS` and never `DROP` on purpose - a `MODULE`
file is re-applied whenever its hash changes, so a `DROP` would revoke every
grant on the realm the next time somebody fixed a typo in its comments.

Bonuses are read into memory at startup and kept there, so the hook costs one
hash lookup; when nothing is granted it costs one branch.

## Commands

    .statbonus add <player> <stat> <amount>     add to what they already have
    .statbonus set <player> <stat> <amount>     set the total outright
    .statbonus clear <player> [stat]            one stat, or all five
    .statbonus list [player]                    the realm, or one character
    .statbonus reload                           re-read the table

`<stat>` is `strength`, `agility`, `stamina`, `intellect` or `spirit`;
`str`/`agi`/`sta`/`int`/`spi` and the raw index `0`-`4` also work, and anything
else is an error rather than a guess. `<amount>` may be negative; the stat shown
to the client is floored at zero however negative the bonus goes, because
`SetStat` writes an unsigned field.

`<player>` may be omitted to use the selected character, and may name an offline
one - the grant is written and applies at their next login, which the command
says. After any change the module calls `UpdateAllStats()` on a connected
character, since the hook will not fire on its own for someone standing still.

`.statbonus list` with no argument prints the whole realm. That is the useful
view when a realm has a handful of grants, and it is the one that works from the
console, where there is no selection to fall back to.

## Configuration (`mod_statbonus.conf`)

`StatBonus.Enable` stops grants counting without deleting them, which is the
quickest way to tell whether something odd on a character comes from this
module. `StatBonus.Limit` is the largest absolute amount the commands will write
to one stat, symmetric, clamping rather than refusing and reporting the clamp;
it guards against a mistyped amount, not against the feature - a bonus is flat
and stacks with gear without any cap of its own, so a stray extra digit is
otherwise a character with 3000 strength. Both are read in `OnAfterConfigLoad`,
so `reload config` applies them live.

## Tests

`src/StatBonusStore.h` holds the parts that are just rules - stat-name parsing,
the floor at zero, the limit clamp, and the store - and includes nothing but the
standard library, so `tests/` builds and runs without AzerothCore:

    cmake -S tests -B build-tests -DCMAKE_CXX_COMPILER=g++-14
    cmake --build build-tests && ctest --test-dir build-tests

Inside a full tree, `mod-statbonus.cmake` registers the same sources with the
core's `unit_tests` target instead.

## Licence

GNU Affero General Public License v3.0, the licence AzerothCore and its modules
use. See [LICENSE](LICENSE).
