# mod-statbonus

Permanent flat additions to a character's five primary stats and its twenty-five
combat ratings: strength through spirit, and hit, expertise, dodge, parry,
block, crit, haste, defense, armor penetration and the rest.

## Why the stats need a core hook

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

## Why the ratings do not

The ratings take the other road: `Player::ApplyRatingMod`, which is where gear
puts them, rather than a second hook of the same shape.

The reason is haste. A last-moment hook for ratings would have to go inside
`Player::UpdateRating`, just before the combat rating field is written - the
exact mirror of the stat hook. But `CR_HASTE_MELEE`, `_RANGED` and `_SPELL`
only make a character faster because `ApplyRatingMod` reaches into
`ApplyAttackTimePercentMod` and `ApplyCastTimePercentMod` as it changes
`m_baseRatingValue`; nothing downstream of `UpdateRating` re-derives attack or
cast speed from the field. A hook there would have put a haste number on the
sheet and left the character swinging at the same speed, which is worse than
not offering haste at all.

Going through `ApplyRatingMod` makes ratings *applied* state - the thing
deliberately avoided for the stats - so it is worth saying why that is safe
here. Nothing resets `m_baseRatingValue` mid-session: item mods add and subtract
their own deltas rather than rebuilding the array, and there is no rating
equivalent of `InitStatsForLevel`. So the module applies its ratings once at
login and reconciles on a change, keeping a record of what it has actually
pushed in, so a re-grant or a `.statbonus reload` moves by the difference rather
than stacking on top of itself.

The upshot for the ratings is that they behave exactly as the same number off an
item would, including the derived percentages: `UpdateRating` calls
`UpdateDodgePercentage`, `UpdateParryPercentage`, `UpdateExpertise`,
`UpdateArmorPenetration` and the hit and crit updates itself.

## The table

`character_stat_bonus` in the characters database, one row per character per
bonus: `Guid`, `Kind` (0 primary stat, 1 combat rating), `Id` (an index into
whichever enum `Kind` names), `Amount` (may be negative), and a `Comment` for
the GM. The two enums are kept as the core's own rather than flattened into one
numbering, so the rows stay readable against `Stats` and `CombatRating` if the
size of either ever changes. A bonus of zero is deleted rather than stored, so
the table is a list of what has actually been granted.

It ships with the module as `data/sql/db-characters/base/`, and the core's
database updater applies it at startup:
`UpdateFetcher::ReceiveIncludedDirectories` walks
`modules/<name>/data/sql/db-characters`. Nothing has to be applied by hand. The
file is `CREATE TABLE IF NOT EXISTS` and never `DROP` on purpose - a `MODULE`
file is re-applied whenever its hash changes, so a `DROP` would revoke every
grant on the realm the next time somebody fixed a typo in its comments.

Bonuses are read into memory at startup and kept there, so the stat hook costs
one hash lookup; when nothing is granted it costs one branch, and when only
primary stats are granted the per-login rating pass is skipped entirely.

## Commands

    .statbonus add <player> <what> <amount>     add to what they already have
    .statbonus set <player> <what> <amount>     set the total outright
    .statbonus clear <player> [what]            one bonus, or all of them
    .statbonus list [player]                    the realm, or one character
    .statbonus names                            everything <what> accepts
    .statbonus reload                           re-read the table

`<what>` is one name covering both kinds:

* **Stats** - `strength`, `agility`, `stamina`, `intellect`, `spirit`, or
  `str`/`agi`/`sta`/`int`/`spi`.
* **Ratings** - `dodge`, `parry`, `block`, `hit`, `crit`, `haste`, `expertise`,
  `defense`, `armor_penetration`, the `_ranged` and `_spell` forms of hit, crit
  and haste, `hit_taken` and `crit_taken` and their forms, and the four weapon
  skills. `hit`, `crit` and `haste` on their own mean the melee rating, because
  that is what the character sheet calls them. Aliases like `melee_hit`,
  `spell_crit` and `arp` work, and a hyphen or a space stands in for the
  underscore.
* **By index** - `stat:<n>` and `rating:<n>` address `Stats` and `CombatRating`
  directly.

A bare number is refused: it would have to mean either a stat or a rating, and
either reading is wrong half the time. There is also no bare `resilience`,
because the item stat of that name feeds `CR_CRIT_TAKEN_MELEE`, `_RANGED` and
`_SPELL` at once and one name for one of them would look like it had done the
job - ask for `resilience_melee`, `resilience_ranged` and `resilience_spell`.

`<amount>` may be negative; a shown stat is floored at zero however negative the
bonus goes, because `SetStat` writes an unsigned field, and the core's own
`UpdateRating` floors the ratings the same way.

`<player>` may be omitted to use the selected character, and may name an offline
one - the grant is written and applies at their next login, which the command
says. After any change the module reconciles that character's ratings and calls
`UpdateAllStats()`, since neither route fires on its own for someone standing
still.

`.statbonus list` with no argument prints the whole realm. That is the useful
view when a realm has a handful of grants, and it is the one that works from the
console, where there is no selection to fall back to. `.statbonus reload` walks
the characters already online and moves their ratings to whatever the table now
says, because that state is applied rather than recomputed.

## Configuration (`mod_statbonus.conf`)

`StatBonus.Enable` stops grants counting without deleting them, which is the
quickest way to tell whether something odd on a character comes from this
module; switching it off also takes the applied ratings back out at the next
reconciliation. `StatBonus.Limit` is the largest absolute amount the commands
will write to one stat or rating, symmetric, clamping rather than refusing and
reporting the clamp; it guards against a mistyped amount, not against the
feature - a bonus is flat and stacks with gear without any cap of its own, so a
stray extra digit is otherwise a character with 3000 strength, or one who never
misses. One number covers both kinds and they are not on the same scale, a
rating worth having being tens or hundreds of points where a stat is single
digits, so set it for the ratings. Both options are read in `OnAfterConfigLoad`,
so `reload config` applies them live.

## Tests

`src/StatBonusStore.h` holds the parts that are just rules - the slot space that
holds both enums, name parsing, the floor at zero, the limit clamp, and the
store - and includes nothing but the standard library, so `tests/` builds and
runs without AzerothCore:

    cmake -S tests -B build-tests -DCMAKE_CXX_COMPILER=g++-14
    cmake --build build-tests && ctest --test-dir build-tests

Inside a full tree, `mod-statbonus.cmake` registers the same sources with the
core's `unit_tests` target instead.

## Licence

GNU Affero General Public License v3.0, the licence AzerothCore and its modules
use. See [LICENSE](LICENSE).
