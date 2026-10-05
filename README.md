# mod-statbonus

Permanent flat additions to a character's five primary stats, its twenty-five
combat ratings and its seven resistance schools: strength through spirit; hit,
expertise, dodge, parry, block, crit, haste, defense, armor penetration and the
rest; and armor, fire, frost, nature, shadow, arcane and holy resistance.

Each of the three reaches the character by a different road, because the server
holds them differently, and only the first of them needed a change to the
core.

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

The ratings take another road: `Player::ApplyRatingMod`, which is where gear
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

## And the resistances neither

Resistances - and armor with them - go through
`Unit::HandleStatFlatModifier(unitMod, TOTAL_VALUE, amount, apply)`, which is
once again where gear puts them: `Player::UpdateResistances` and
`Player::UpdateArmor` both read `GetFlatModifierValue(unitMod, TOTAL_VALUE)` as
part of their sum. `UNIT_MOD_RESISTANCE_START` *is* `UNIT_MOD_ARMOR`, so all
seven schools are one uniform call with no special case for armor, and
`HandleStatFlatModifier` calls `UpdateUnitMod` itself, so nothing else has to be
poked afterwards.

Its neighbour `SetStatFlatModifier` carries a comment in the core warning that
"usage outside of AuraEffect Handlers is discouraged as the value will be lost
when auras change". That warning is about *setting* the modifier, which
overwrites whatever auras had put there. `HandleStatFlatModifier` accumulates,
exactly as every item does, so a contribution pushed in this way is not
disturbed by auras coming and going - and it is why the same reasoning that
makes the ratings safe covers these too.

## And movement is a percentage

Movement is the one kind here that is not a flat addition, and the reason is
arithmetic rather than taste. A speed is a *rate*: 1.0 is normal, and the yards
per second come from the core's `baseMoveSpeed` table - 7.0 running, 4.722222
swimming, and a mount is a rate of about 2.0 in the same number. A flat 1 there
would mean double speed. So the stored integer is read as a percentage: 1 is
1%, 25 is a quarter again.

It is **added to the rate** rather than multiplied into it, which is what makes
a grant mean the same thing everywhere. On foot at rate 1.0, +10 gives 1.10. On
a mount at 2.0 it gives 2.10 - still a tenth of normal speed, not a tenth of
the mount's. Multiplying would have made the same grant worth twice as much
while mounted, which is not what a number called "10%" should do.

A negative grant slows, and the result is floored at a tenth of normal rather
than at zero. A rate of zero cannot be walked out of: there is no buff to
remove and no way to reach anybody, so the character would simply be stuck.

Movement has its own ceiling, `StatBonus.SpeedLimit`, because `StatBonus.Limit`
is tuned for combat ratings where a useful grant is in the hundreds - the same
number applied to a percentage would allow twenty times normal speed.

Like the primary stats, and unlike the ratings and resistances, movement is
*not* applied state. `Player::UpdateSpeed` recomputes the rate from the auras
whenever any of them changes, mounting included, so there is nowhere a lasting
adjustment could be written - which is exactly why it needs a hook of its own,
`PLAYERHOOK_ON_CALCULATE_SPEED` /
`PlayerScript::OnPlayerCalculateSpeed(Player*, UnitMoveType, float&)`, called
immediately before `SetSpeed`. Going through `SetSpeed` is also what tells the
client (`SMSG_FORCE_RUN_SPEED_CHANGE` and its siblings), which is the same path
every speed aura takes, so movement validation is satisfied.

All nine `UnitMoveType` rates are addressable. `speed`, `run`, `swim` and `fly`
are the names worth having; the backwards rates, the turn rate and the pitch
rate have no aliases and are reached as `movement:<n>` by anyone who wants
them.

## The table

`character_stat_bonus` in the characters database, one row per character per
bonus: `Guid`, `Kind` (0 primary stat, 1 combat rating, 2 resistance, 3
movement), `Id` (an index into whichever enum `Kind` names), `Amount` (a flat
addition for kinds 0 to 2 and a percentage for kind 3, and may be negative),
and a `Comment` for the GM. The four enums are kept as the core's own rather
than flattened into one numbering, so the rows stay readable against `Stats`,
`CombatRating`, `SpellSchools` and `UnitMoveType` if the size of any of them
ever changes. A
bonus of zero is deleted rather than stored, so the table is a list of what has
actually been granted.

It ships with the module as `data/sql/db-characters/base/`, and the core's
database updater applies it at startup:
`UpdateFetcher::ReceiveIncludedDirectories` walks
`modules/<name>/data/sql/db-characters`. Nothing has to be applied by hand. The
file is `CREATE TABLE IF NOT EXISTS` and never `DROP` on purpose - a `MODULE`
file is re-applied whenever its hash changes, so a `DROP` would revoke every
grant on the realm the next time somebody fixed a typo in its comments.

Bonuses are read into memory at startup and kept there, so the stat hook costs
one hash lookup; when nothing is granted it costs one branch, and when only
primary stats are granted the per-login reconciliation is skipped entirely.

## Earning them

A GM command is one way in. The other is a quest, which is what the realm
actually uses: Bazil Thredd hands every member of the killing party a token,
using the token starts a repeatable quest and calls a broker, and the turn-in
grants one bonus rolled from a pool.

Three hooks, no core change:

| Step | Hook | Why there |
| --- | --- | --- |
| token to the party | `OnPlayerCreatureKill` on a configured creature, then a walk of the killer's group | loot cannot do it - see below |
| broker appears | `OnPlayerQuestAccept`, for any quest with a reward pool | no spell needed, so nothing to ship to the client |
| bonus granted | `OnPlayerCompleteQuest` | it is called at the END of `Player::RewardQuest`, so the token is already taken |

That last one is worth knowing: the name suggests the moment the objectives are
met, and it is not - `Player::CompleteQuest` does not call it. Granting at the
objectives-met moment would have handed out the bonus while the player still
held the token.

**Why the token is not loot.** The requirement was "everyone in the party, every
time", and `creature_loot_template` cannot express it. A quest item only drops
for somebody already on the quest, and an ordinary item drops once, for one
looter. So the module hands it over on the kill, to every group member in the
same map, and mails it to anybody whose bags are full rather than dropping it on
the floor.

**Why the broker is summoned on accept.** `Archmage Vargoth's Staff` (28455) is
the shipped precedent - a quest item whose use-spell summons the NPC who takes
it - but that needs a spell, and a new spell needs a row in `Spell.dbc` and so a
client patch. `item_template.StartQuest` makes the token start the quest by
itself, the way the Darkmoon decks do, and the broker is summoned when the quest
is accepted. One click either way.

### The pool

`statbonus_quest_reward` in the world database: `QuestId`, then the same
`Kind`/`Id`/`Amount` vocabulary as everything else here, plus a `Weight`.
Several rows sharing a `QuestId` make the quest random; one row makes it fixed,
which is how "pick your own stat" would be built on the same machinery - several
quests, one row each, sharing a token.

The point is that what a quest can grant is **data**. A row can award a primary
stat, any of the 25 combat ratings, a resistance or a movement percentage with
nothing rebuilt, and `Weight` 0 retires a row without deleting it. A pool whose
weights are all 0 grants nothing and says so at load time, rather than quietly
falling back to the first row.

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
* **Resistances** - `armor`, `resist_holy`, `resist_fire`, `resist_nature`,
  `resist_frost`, `resist_shadow`, `resist_arcane`. The bare school name works
  (`fire`), as do `fire_resistance`, `resistance_fire` and `fire_res`. `armor`
  is resistance school 0 and is a different thing from `armor_penetration`,
  which is a rating; the names are matched exactly so the two cannot be
  confused.
* **By index** - `stat:<n>`, `rating:<n>` and `resist:<n>` address `Stats`,
  `CombatRating` and `SpellSchools` directly.

`.statbonus names` prints the whole vocabulary, which is the fastest way to find
the name for a row you are looking at.

`hit`, `crit` and `haste` on their own are the **melee** ratings. Each of the
three has its own row per paper doll tab, so the Spells tab reads `hit_spell`
and `haste_spell` - asking for `haste` and then watching the Spells tab shows
nothing moving, which is a mistake worth making only once.

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
says. After any change the module reconciles that character's ratings and
resistances and calls `UpdateAllStats()`, since none of the three routes fires
on its own for someone standing still.

`.statbonus list` with no argument prints the whole realm. That is the useful
view when a realm has a handful of grants, and it is the one that works from the
console, where there is no selection to fall back to. `.statbonus reload` walks
the characters already online and moves their ratings and resistances to
whatever the table now says, because that state is applied rather than
recomputed.

## Configuration (`mod_statbonus.conf`)

`StatBonus.Enable` stops grants counting without deleting them, which is the
quickest way to tell whether something odd on a character comes from this
module; switching it off also takes the applied ratings back out at the next
reconciliation. `StatBonus.Limit` is the largest absolute amount the commands
will write to one stat, rating or resistance, symmetric, clamping rather than refusing and
reporting the clamp; it guards against a mistyped amount, not against the
feature - a bonus is flat and stacks with gear without any cap of its own, so a
stray extra digit is otherwise a character with 3000 strength, or one who never
misses. One number covers both kinds and they are not on the same scale, a
rating worth having being tens or hundreds of points where a stat is single
digits, so set it for the ratings. Both options are read in `OnAfterConfigLoad`,
so `reload config` applies them live.

## Tests

`src/StatBonusStore.h` holds the parts that are just rules - the slot space that
holds all three enums, name parsing, the floor at zero, the limit clamp, and the
store - and includes nothing but the standard library, so `tests/` builds and
runs without AzerothCore:

    cmake -S tests -B build-tests -DCMAKE_CXX_COMPILER=g++-14
    cmake --build build-tests && ctest --test-dir build-tests

Inside a full tree, `mod-statbonus.cmake` registers the same sources with the
core's `unit_tests` target instead.

## Licence

GNU Affero General Public License v3.0, the licence AzerothCore and its modules
use. See [LICENSE](LICENSE).
