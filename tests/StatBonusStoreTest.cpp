/*
 * Tests for mod-statbonus' rules (src/StatBonusStore.h).
 *
 * What is worth pinning down here is not arithmetic for its own sake. A bonus
 * that silently lands on the wrong stat, a name that prints one way and parses
 * another, a zero left in the table so the module believes it is doing work, or
 * a negative grant that pushes the unsigned stat field below zero are all
 * invisible in play until a character looks wrong on the sheet, which is a long
 * way from the cause.
 */

#include "StatBonusStore.h"

#include <gtest/gtest.h>

using namespace StatBonus;

TEST(StatBonusSlots, TheSlotSpaceIsTheFourCoreEnumsBackToBack)
{
    EXPECT_EQ(STAT_COUNT, 5u);                  // MAX_STATS
    EXPECT_EQ(RATING_COUNT, 25u);               // MAX_COMBAT_RATING
    EXPECT_EQ(RESISTANCE_COUNT, 7u);            // MAX_SPELL_SCHOOL
    EXPECT_EQ(MOVEMENT_COUNT, 9u);              // MAX_MOVE_TYPE
    EXPECT_EQ(SLOT_COUNT, STAT_COUNT + RATING_COUNT + RESISTANCE_COUNT + MOVEMENT_COUNT);

    // Exactly one predicate is true for every slot, and none past the end.
    for (std::size_t slot = 0; slot < SLOT_COUNT; ++slot)
        EXPECT_EQ(int(IsStat(slot)) + int(IsRating(slot)) + int(IsResistance(slot)) + int(IsMovement(slot)), 1)
            << "slot " << slot;

    EXPECT_FALSE(IsStat(SLOT_COUNT));
    EXPECT_FALSE(IsRating(SLOT_COUNT));
    EXPECT_FALSE(IsResistance(SLOT_COUNT));
    EXPECT_FALSE(IsMovement(SLOT_COUNT));
}

TEST(StatBonusSlots, OnlyRatingsAndResistancesArePushedIn)
{
    // IsApplied decides what gets reconciled at login. Anything answered by a
    // hook instead must stay out of it, or it is counted twice: once by the
    // hook and once by the reconciliation. That is the stats, and now the
    // movement rates as well, which is why this stops at MOVEMENT_FIRST
    // rather than running to the end of the slot space.
    for (std::size_t slot = 0; slot < STAT_COUNT; ++slot)
        EXPECT_FALSE(IsApplied(slot)) << SlotName(slot);

    for (std::size_t slot = RATING_FIRST; slot < MOVEMENT_FIRST; ++slot)
        EXPECT_TRUE(IsApplied(slot)) << SlotName(slot);

    for (std::size_t slot = MOVEMENT_FIRST; slot < SLOT_COUNT; ++slot)
        EXPECT_FALSE(IsApplied(slot)) << SlotName(slot);
}

TEST(StatBonusSlots, SlotAndKindIndexRoundTrip)
{
    // Kind and Id are what goes in the table, so a slot has to survive the
    // trip out to the database and back unchanged.
    for (std::size_t slot = 0; slot < SLOT_COUNT; ++slot)
    {
        auto const back = SlotOf(KindOf(slot), IndexOf(slot));
        ASSERT_TRUE(back.has_value()) << "slot " << slot;
        EXPECT_EQ(*back, slot);
    }
}

TEST(StatBonusSlots, OutOfRangeKindsAndIndicesHaveNoSlot)
{
    // These come out of a tinyint column that nothing stops a GM editing by
    // hand, so a bad row has to be rejected rather than land somewhere.
    EXPECT_FALSE(SlotOf(KIND_STAT, STAT_COUNT).has_value());
    EXPECT_FALSE(SlotOf(KIND_RATING, RATING_COUNT).has_value());
    EXPECT_FALSE(SlotOf(KIND_RESISTANCE, RESISTANCE_COUNT).has_value());
    EXPECT_FALSE(SlotOf(KIND_MOVEMENT, MOVEMENT_COUNT).has_value());
    EXPECT_FALSE(SlotOf(4, 0).has_value());
    EXPECT_FALSE(SlotOf(KIND_STAT, 200).has_value());
}

TEST(StatBonusNames, EverySlotHasANameThatParsesBackToIt)
{
    for (std::size_t slot = 0; slot < SLOT_COUNT; ++slot)
    {
        EXPECT_STRNE(SlotName(slot), "unknown") << "slot " << slot;
        // The commands print these, so a name that would not be accepted back
        // would make the output something the GM cannot act on.
        EXPECT_EQ(ParseSlot(SlotName(slot)), slot) << SlotName(slot);
    }

    EXPECT_STREQ(SlotName(SLOT_COUNT), "unknown");
}

TEST(StatBonusNames, EveryNameIsDistinct)
{
    for (std::size_t a = 0; a < SLOT_COUNT; ++a)
        for (std::size_t b = a + 1; b < SLOT_COUNT; ++b)
            EXPECT_STRNE(SlotName(a), SlotName(b)) << "slots " << a << " and " << b;
}

TEST(StatBonusParse, PrimaryStatsByNameAndAbbreviation)
{
    EXPECT_EQ(ParseSlot("strength"), 0u);
    EXPECT_EQ(ParseSlot("STRENGTH"), 0u);
    EXPECT_EQ(ParseSlot("str"), 0u);
    EXPECT_EQ(ParseSlot("agi"), 1u);
    EXPECT_EQ(ParseSlot("stam"), 2u);
    EXPECT_EQ(ParseSlot("intellect"), 3u);
    EXPECT_EQ(ParseSlot("spi"), 4u);
}

TEST(StatBonusParse, TheRatingsTheUserAskedForLandOnTheRightCombatRating)
{
    // Checked against the CombatRating enum in Unit.h, because everything else
    // here only proves the header is consistent with itself.
    EXPECT_EQ(ParseSlot("dodge"),             RatingSlot(2));   // CR_DODGE
    EXPECT_EQ(ParseSlot("parry"),             RatingSlot(3));   // CR_PARRY
    EXPECT_EQ(ParseSlot("block"),             RatingSlot(4));   // CR_BLOCK
    EXPECT_EQ(ParseSlot("hit"),               RatingSlot(5));   // CR_HIT_MELEE
    EXPECT_EQ(ParseSlot("expertise"),         RatingSlot(23));  // CR_EXPERTISE
    EXPECT_EQ(ParseSlot("armor_penetration"), RatingSlot(24));  // CR_ARMOR_PENETRATION
    EXPECT_EQ(ParseSlot("defense"),           RatingSlot(1));   // CR_DEFENSE_SKILL
    EXPECT_EQ(ParseSlot("haste"),             RatingSlot(17));  // CR_HASTE_MELEE
    EXPECT_EQ(ParseSlot("crit"),              RatingSlot(8));   // CR_CRIT_MELEE
}

TEST(StatBonusParse, BareNamesMeanTheMeleeRatingBecauseThatIsWhatTheSheetCallsThem)
{
    EXPECT_EQ(ParseSlot("hit"),   ParseSlot("hit_melee"));
    EXPECT_EQ(ParseSlot("crit"),  ParseSlot("crit_melee"));
    EXPECT_EQ(ParseSlot("haste"), ParseSlot("haste_melee"));

    EXPECT_NE(ParseSlot("hit"),   ParseSlot("hit_spell"));
    EXPECT_NE(ParseSlot("crit"),  ParseSlot("crit_ranged"));
}

TEST(StatBonusParse, AliasesAndSeparatorsAndBothWordOrders)
{
    EXPECT_EQ(ParseSlot("melee_hit"),  ParseSlot("hit_melee"));
    EXPECT_EQ(ParseSlot("spell_crit"), ParseSlot("crit_spell"));
    EXPECT_EQ(ParseSlot("ranged_haste"), ParseSlot("haste_ranged"));
    EXPECT_EQ(ParseSlot("arp"), ParseSlot("armor_penetration"));
    EXPECT_EQ(ParseSlot("armorpen"), ParseSlot("armor_penetration"));

    // A hyphen or a space where the underscore goes is a typo worth accepting,
    // and so is a stray space at either end.
    EXPECT_EQ(ParseSlot("hit-spell"), ParseSlot("hit_spell"));
    EXPECT_EQ(ParseSlot("armor penetration"), ParseSlot("armor_penetration"));
    EXPECT_EQ(ParseSlot("  strength  "), 0u);
    EXPECT_EQ(ParseSlot(" rating:2 "), RatingSlot(2));
}

TEST(StatBonusParse, ResistancesByBareSchoolAndBothWordOrders)
{
    // Checked against SpellSchools, and school 0 is armor: the core indexes
    // armor as a resistance and UNIT_MOD_RESISTANCE_START is UNIT_MOD_ARMOR.
    EXPECT_EQ(ParseSlot("armor"),         ResistanceSlot(0));   // SPELL_SCHOOL_NORMAL
    EXPECT_EQ(ParseSlot("resist_holy"),   ResistanceSlot(1));   // SPELL_SCHOOL_HOLY
    EXPECT_EQ(ParseSlot("fire"),          ResistanceSlot(2));   // SPELL_SCHOOL_FIRE
    EXPECT_EQ(ParseSlot("nature"),        ResistanceSlot(3));
    EXPECT_EQ(ParseSlot("frost"),         ResistanceSlot(4));
    EXPECT_EQ(ParseSlot("shadow"),        ResistanceSlot(5));
    EXPECT_EQ(ParseSlot("arcane"),        ResistanceSlot(6));   // SPELL_SCHOOL_ARCANE

    EXPECT_EQ(ParseSlot("fire_resistance"), ParseSlot("resist_fire"));
    EXPECT_EQ(ParseSlot("resistance_fire"), ParseSlot("resist_fire"));
    EXPECT_EQ(ParseSlot("frost_res"),       ParseSlot("resist_frost"));
    EXPECT_EQ(ParseSlot("resist:4"),        ParseSlot("resist_frost"));
    EXPECT_EQ(ParseSlot("school:0"),        ParseSlot("armor"));
    EXPECT_FALSE(ParseSlot("resist:7").has_value());
}

TEST(StatBonusParse, ArmorAndArmorPenetrationDoNotCollide)
{
    // One is a resistance school and the other a combat rating. A prefix match
    // on "armor" would have quietly sent armor penetration to the wrong place.
    EXPECT_EQ(ParseSlot("armor"), ResistanceSlot(0));
    EXPECT_EQ(ParseSlot("armor_penetration"), RatingSlot(24));
    EXPECT_EQ(ParseSlot("armor_pen"), RatingSlot(24));
    EXPECT_EQ(ParseSlot("armorpen"), RatingSlot(24));
    EXPECT_EQ(ParseSlot("arp"), RatingSlot(24));
    EXPECT_NE(ParseSlot("armor"), ParseSlot("armor_pen"));
}

TEST(StatBonusParse, NoBareResilienceBecauseItWouldPromiseAllThree)
{
    // The resilience item stat feeds CR_CRIT_TAKEN_MELEE, _RANGED and _SPELL at
    // once. One name for one of them would look like it had done the job.
    EXPECT_FALSE(ParseSlot("resilience").has_value());
    EXPECT_EQ(ParseSlot("resilience_melee"), RatingSlot(14));
    EXPECT_EQ(ParseSlot("crit_taken"), RatingSlot(14));
}

TEST(StatBonusParse, EnumIndicesNeedTheirNamespaceSpelledOut)
{
    EXPECT_EQ(ParseSlot("stat:0"), 0u);
    EXPECT_EQ(ParseSlot("stat:4"), 4u);
    EXPECT_EQ(ParseSlot("rating:0"), RatingSlot(0));
    EXPECT_EQ(ParseSlot("rating:24"), RatingSlot(24));
    EXPECT_EQ(ParseSlot("cr:5"), RatingSlot(5));

    EXPECT_FALSE(ParseSlot("stat:5").has_value());
    EXPECT_FALSE(ParseSlot("rating:25").has_value());
    EXPECT_FALSE(ParseSlot("stat:").has_value());
    EXPECT_FALSE(ParseSlot("stat:x").has_value());
    EXPECT_FALSE(ParseSlot("stat:-1").has_value());
    EXPECT_FALSE(ParseSlot("armor:1").has_value());
}

TEST(StatBonusParse, ABareNumberIsRejected)
{
    // It would have to mean a stat or a rating, and either reading is wrong
    // half the time. "3" as intellect and "3" as parry are both plausible.
    for (char c = '0'; c <= '9'; ++c)
        EXPECT_FALSE(ParseSlot(std::string(1, c)).has_value()) << c;

    EXPECT_FALSE(ParseSlot("23").has_value());
}

TEST(StatBonusParse, RejectsAnythingElse)
{
    EXPECT_FALSE(ParseSlot("").has_value());
    EXPECT_FALSE(ParseSlot("s").has_value());          // strength, stamina or spirit
    EXPECT_FALSE(ParseSlot("st").has_value());         // strength or stamina
    EXPECT_FALSE(ParseSlot("spellpower").has_value());
    EXPECT_FALSE(ParseSlot("mp5").has_value());
    EXPECT_FALSE(ParseSlot("_").has_value());
    EXPECT_FALSE(ParseSlot("hit_").has_value());
}

TEST(StatBonusApply, AddsTheBonusToTheCalculatedValue)
{
    EXPECT_FLOAT_EQ(Apply(100.0f, 1), 101.0f);
    EXPECT_FLOAT_EQ(Apply(100.0f, -1), 99.0f);
    EXPECT_FLOAT_EQ(Apply(100.0f, 0), 100.0f);
}

TEST(StatBonusApply, NeverGoesBelowZero)
{
    // SetStat writes into an unsigned field, so a bonus more negative than the
    // character's whole stat must floor rather than wrap to four billion.
    EXPECT_FLOAT_EQ(Apply(10.0f, -10), 0.0f);
    EXPECT_FLOAT_EQ(Apply(10.0f, -11), 0.0f);
    EXPECT_FLOAT_EQ(Apply(10.0f, -100000), 0.0f);
}

TEST(StatBonusLimit, ClampsSymmetricallyAndTreatsZeroAsNoLimit)
{
    EXPECT_EQ(ClampToLimit(60, 50), 50);
    EXPECT_EQ(ClampToLimit(-60, 50), -50);
    EXPECT_EQ(ClampToLimit(40, 50), 40);

    EXPECT_EQ(ClampToLimit(100000, 0), 100000);
    EXPECT_EQ(ClampToLimit(-100000, 0), -100000);

    // A negative limit is a misconfiguration; it must not invert the clamp.
    EXPECT_EQ(ClampToLimit(60, -50), 60);
}

TEST(StatBonusStore, StartsEmptyAndReportsUnknownCharactersAsZero)
{
    Store store;

    EXPECT_TRUE(store.Empty());
    EXPECT_FALSE(store.AnyApplied());
    EXPECT_EQ(store.Size(), 0u);
    EXPECT_EQ(store.Get(42, 0), 0);
    EXPECT_EQ(store.Find(42), nullptr);
}

TEST(StatBonusStore, TheThreeKindsAreIndependent)
{
    Store store;

    store.Set(1, *ParseSlot("strength"), 5);
    store.Set(1, *ParseSlot("hit"), 20);
    store.Set(1, *ParseSlot("resist_fire"), 30);

    EXPECT_EQ(store.Get(1, *ParseSlot("strength")), 5);
    EXPECT_EQ(store.Get(1, *ParseSlot("hit")), 20);
    EXPECT_EQ(store.Get(1, *ParseSlot("resist_fire")), 30);
    EXPECT_EQ(store.Get(1, *ParseSlot("hit_spell")), 0);
    EXPECT_EQ(store.Get(1, *ParseSlot("resist_frost")), 0);
    EXPECT_EQ(store.Get(1, *ParseSlot("armor")), 0);
    EXPECT_EQ(store.Size(), 1u);
}

TEST(StatBonusStore, AnyAppliedIsFalseWhileOnlyStatsAreGranted)
{
    // It gates the per-login reconciliation, so a realm that grants nothing but
    // primary stats must not pay for that path at every login.
    Store store;

    store.Set(1, *ParseSlot("stamina"), 10);
    EXPECT_FALSE(store.Empty());
    EXPECT_FALSE(store.AnyApplied());

    store.Set(2, *ParseSlot("dodge"), 10);
    EXPECT_TRUE(store.AnyApplied());

    store.Clear(2);
    EXPECT_FALSE(store.AnyApplied());

    // A resistance counts too, and it is the one that would be missed by a
    // check written when ratings were the only applied kind.
    store.Set(3, *ParseSlot("resist_fire"), 10);
    EXPECT_TRUE(store.AnyApplied());
}

TEST(StatBonusStore, SetAndGetAreIndependentPerSlotAndPerCharacter)
{
    Store store;

    store.Set(1, 0, 5);
    store.Set(1, 2, -3);
    store.Set(2, 0, 7);

    EXPECT_EQ(store.Get(1, 0), 5);
    EXPECT_EQ(store.Get(1, 1), 0);
    EXPECT_EQ(store.Get(1, 2), -3);
    EXPECT_EQ(store.Get(2, 0), 7);
    EXPECT_EQ(store.Size(), 2u);
}

TEST(StatBonusStore, AddAccumulatesAndReturnsTheNewTotal)
{
    Store store;

    EXPECT_EQ(store.Add(1, 0, 1), 1);
    EXPECT_EQ(store.Add(1, 0, 1), 2);
    EXPECT_EQ(store.Add(1, 0, -5), -3);
    EXPECT_EQ(store.Get(1, 0), -3);
}

TEST(StatBonusStore, ADeltaBackToZeroRemovesTheEntry)
{
    // Empty() gates the stat hook's hot path, so a character whose bonus has
    // been added and then taken away again must not leave the store busy.
    Store store;

    store.Add(1, 0, 3);
    EXPECT_FALSE(store.Empty());

    store.Add(1, 0, -3);
    EXPECT_TRUE(store.Empty());
    EXPECT_EQ(store.Find(1), nullptr);
}

TEST(StatBonusStore, ClearingOneSlotKeepsTheOthers)
{
    Store store;

    store.Set(1, 0, 5);
    store.Set(1, *ParseSlot("parry"), 2);

    EXPECT_TRUE(store.Clear(1, 0));
    EXPECT_EQ(store.Get(1, 0), 0);
    EXPECT_EQ(store.Get(1, *ParseSlot("parry")), 2);
    EXPECT_FALSE(store.Empty());

    EXPECT_TRUE(store.Clear(1, *ParseSlot("parry")));
    EXPECT_TRUE(store.Empty());
}

TEST(StatBonusStore, ClearingReportsWhetherAnythingWasThere)
{
    // The command prints "has no bonus to clear" off this, so a clear of
    // nothing must not claim to have revoked something.
    Store store;

    EXPECT_FALSE(store.Clear(1, 0));
    EXPECT_FALSE(store.Clear(1));

    store.Set(1, 0, 5);
    EXPECT_TRUE(store.Clear(1));
    EXPECT_FALSE(store.Clear(1));
}

TEST(StatBonusStore, OutOfRangeSlotsAreIgnoredRatherThanWritten)
{
    Store store;

    EXPECT_EQ(store.Set(1, SLOT_COUNT, 5), 0);
    EXPECT_EQ(store.Add(1, 99, 5), 0);
    EXPECT_EQ(store.Get(1, SLOT_COUNT), 0);
    EXPECT_FALSE(store.Clear(1, SLOT_COUNT));
    EXPECT_TRUE(store.Empty());
}

TEST(StatBonusStore, ResetDropsEverything)
{
    Store store;

    store.Set(1, 0, 5);
    store.Set(2, 1, 5);
    store.Reset();

    EXPECT_TRUE(store.Empty());
}

TEST(StatBonusStore, AllIsOrderedByGuid)
{
    Store store;

    store.Set(30, 0, 1);
    store.Set(10, 0, 1);
    store.Set(20, 0, 1);

    auto const all = store.All();
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].first, 10u);
    EXPECT_EQ(all[1].first, 20u);
    EXPECT_EQ(all[2].first, 30u);
}

// ---------------------------------------------------------------------------
// Movement.
//
// The one kind whose Amount is not a flat addition. A speed is a rate where
// 1.0 is normal and the yards per second come from the core's table, so a flat
// 1 would mean double speed - the integer is a percentage instead.
// ---------------------------------------------------------------------------

TEST(StatBonusMovement, TheSlotSpaceGainedAFourthKind)
{
    EXPECT_EQ(MOVEMENT_COUNT, 9u);          // MAX_MOVE_TYPE
    EXPECT_EQ(SLOT_COUNT, STAT_COUNT + RATING_COUNT + RESISTANCE_COUNT + MOVEMENT_COUNT);

    // Still exactly one predicate per slot, with the new one included.
    for (std::size_t slot = 0; slot < SLOT_COUNT; ++slot)
        EXPECT_EQ(int(IsStat(slot)) + int(IsRating(slot)) + int(IsResistance(slot)) + int(IsMovement(slot)), 1)
            << "slot " << slot;

    EXPECT_FALSE(IsMovement(SLOT_COUNT));
}

TEST(StatBonusMovement, MovementIsNotAppliedState)
{
    // This is the trap. Ratings and resistances are pushed into the character
    // and reconciled; movement is answered on demand by the hook, exactly like
    // a primary stat. Listing it as applied would have it counted twice.
    for (std::size_t slot = MOVEMENT_FIRST; slot < SLOT_COUNT; ++slot)
        EXPECT_FALSE(IsApplied(slot)) << SlotName(slot);
}

TEST(StatBonusMovement, NamesLandOnTheRightUnitMoveType)
{
    // Checked against UnitMoveType in UnitDefines.h.
    EXPECT_EQ(ParseSlot("walk_speed"),   MovementSlot(0));
    EXPECT_EQ(ParseSlot("run_speed"),    MovementSlot(1));
    EXPECT_EQ(ParseSlot("swim_speed"),   MovementSlot(3));
    EXPECT_EQ(ParseSlot("flight_speed"), MovementSlot(6));
    EXPECT_EQ(ParseSlot("pitch_rate"),   MovementSlot(8));

    // The short forms people actually type.
    EXPECT_EQ(ParseSlot("speed"), ParseSlot("run_speed"));
    EXPECT_EQ(ParseSlot("run"),   ParseSlot("run_speed"));
    EXPECT_EQ(ParseSlot("swim"),  ParseSlot("swim_speed"));
    EXPECT_EQ(ParseSlot("fly"),   ParseSlot("flight_speed"));

    EXPECT_EQ(ParseSlot("movement:1"), MovementSlot(1));
    EXPECT_EQ(ParseSlot("move:3"), MovementSlot(3));
    EXPECT_EQ(ParseSlot("speed:6"), MovementSlot(6));
    EXPECT_FALSE(ParseSlot("movement:9").has_value());
}

TEST(StatBonusMovement, SpeedDoesNotCollideWithAnythingElse)
{
    // "speed" is a bare name AND a namespace prefix, and the two must not
    // tread on each other.
    EXPECT_EQ(ParseSlot("speed"), MovementSlot(1));
    EXPECT_EQ(ParseSlot("speed:0"), MovementSlot(0));

    // Nor should it be confused with the ratings that sound like it.
    EXPECT_NE(ParseSlot("speed"), ParseSlot("haste"));
}

TEST(StatBonusMovement, APercentageIsAddedToTheRate)
{
    // On foot, rate 1.0: +10 is a tenth faster.
    EXPECT_FLOAT_EQ(ApplyMovement(1.0f, 10), 1.10f);
    EXPECT_FLOAT_EQ(ApplyMovement(1.0f, 100), 2.00f);
    EXPECT_FLOAT_EQ(ApplyMovement(1.0f, 1), 1.01f);
    EXPECT_FLOAT_EQ(ApplyMovement(1.0f, 0), 1.0f);
}

TEST(StatBonusMovement, TheSameGrantIsWorthTheSameMountedOrNot)
{
    // Added to the rate rather than multiplied into it. On a mount at 2.0 a
    // +10 grant is still a tenth of normal speed and not a tenth of the
    // mount's, which is what "10%" ought to mean.
    EXPECT_FLOAT_EQ(ApplyMovement(2.0f, 10), 2.10f);

    // NEAR and not FLOAT_EQ: these are differences taken at different
    // magnitudes, so the float error does not cancel - 0.10000002 against
    // 0.099999905. The gain is equal to well inside anything that matters at a
    // hundredth of a rate.
    EXPECT_NEAR(ApplyMovement(1.0f, 10) - 1.0f, ApplyMovement(2.0f, 10) - 2.0f, 0.0001f);
}

TEST(StatBonusMovement, NegativeSlowsButNeverFreezes)
{
    EXPECT_FLOAT_EQ(ApplyMovement(1.0f, -50), 0.50f);

    // A rate of zero cannot be walked out of - no buff to remove and no way to
    // reach anybody - so it floors well above it.
    EXPECT_FLOAT_EQ(ApplyMovement(1.0f, -100), MOVEMENT_RATE_FLOOR());
    EXPECT_FLOAT_EQ(ApplyMovement(1.0f, -500), MOVEMENT_RATE_FLOOR());
    EXPECT_GT(ApplyMovement(1.0f, -10000), 0.0f);
}

TEST(StatBonusMovement, KindAndIndexRoundTripForMovementToo)
{
    for (std::size_t slot = MOVEMENT_FIRST; slot < SLOT_COUNT; ++slot)
    {
        EXPECT_EQ(KindOf(slot), KIND_MOVEMENT);
        auto const back = SlotOf(KindOf(slot), IndexOf(slot));
        ASSERT_TRUE(back.has_value()) << "slot " << slot;
        EXPECT_EQ(*back, slot);
    }

    EXPECT_FALSE(SlotOf(KIND_MOVEMENT, MOVEMENT_COUNT).has_value());
    EXPECT_FALSE(SlotOf(4, 0).has_value());
}

TEST(StatBonusMovement, TurnRateAndPitchRateAreNotAdjustable)
{
    // They are in UnitMoveType but Unit::UpdateSpeed's switch sends them to a
    // default that logs "Unsupported move type". The hook this module reads
    // lives inside that function, so a bonus on either could never be asked
    // for - and asking UpdateSpeed for them anyway logged an error apiece on
    // every grant, which is how this was found.
    EXPECT_FALSE(IsAdjustableMovement(MovementSlot(5)));   // MOVE_TURN_RATE
    EXPECT_FALSE(IsAdjustableMovement(MovementSlot(8)));   // MOVE_PITCH_RATE

    // Everything else is a real speed.
    for (std::size_t type : { 0u, 1u, 2u, 3u, 4u, 6u, 7u })
        EXPECT_TRUE(IsAdjustableMovement(MovementSlot(type))) << SlotName(MovementSlot(type));
}

TEST(StatBonusMovement, OnlyMovementSlotsAreAdjustableMovement)
{
    // The predicate has to be false for the other three kinds rather than
    // reading their slot as a move type.
    for (std::size_t slot = 0; slot < MOVEMENT_FIRST; ++slot)
        EXPECT_FALSE(IsAdjustableMovement(slot)) << SlotName(slot);

    EXPECT_FALSE(IsAdjustableMovement(SLOT_COUNT));
}

TEST(StatBonusMovement, TheyKeepTheirSlotsSoTheEnumStillRoundTrips)
{
    // Refused on grant, but not removed: the table stores UnitMoveType ids, so
    // the numbering has to keep matching the core's.
    EXPECT_EQ(ParseSlot("turn_rate"), MovementSlot(5));
    EXPECT_EQ(ParseSlot("pitch_rate"), MovementSlot(8));
    EXPECT_EQ(ParseSlot("movement:5"), MovementSlot(5));

    for (std::size_t type : { 5u, 8u })
    {
        std::size_t const slot = MovementSlot(type);
        EXPECT_EQ(KindOf(slot), KIND_MOVEMENT);
        EXPECT_EQ(IndexOf(slot), type);
        EXPECT_EQ(SlotOf(KIND_MOVEMENT, type), slot);
    }
}
