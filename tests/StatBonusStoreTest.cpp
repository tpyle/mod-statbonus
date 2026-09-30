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

TEST(StatBonusSlots, TheSlotSpaceIsTheTwoCoreEnumsBackToBack)
{
    EXPECT_EQ(STAT_COUNT, 5u);                  // MAX_STATS
    EXPECT_EQ(RATING_COUNT, 25u);               // MAX_COMBAT_RATING
    EXPECT_EQ(SLOT_COUNT, STAT_COUNT + RATING_COUNT);

    for (std::size_t slot = 0; slot < STAT_COUNT; ++slot)
        EXPECT_FALSE(IsRating(slot));

    for (std::size_t slot = STAT_COUNT; slot < SLOT_COUNT; ++slot)
        EXPECT_TRUE(IsRating(slot));

    EXPECT_FALSE(IsRating(SLOT_COUNT));
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
    EXPECT_FALSE(SlotOf(2, 0).has_value());
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
    EXPECT_FALSE(ParseSlot("armor").has_value());      // not a rating; a real thing to ask for and not this
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
    EXPECT_FALSE(store.AnyRatings());
    EXPECT_EQ(store.Size(), 0u);
    EXPECT_EQ(store.Get(42, 0), 0);
    EXPECT_EQ(store.Find(42), nullptr);
}

TEST(StatBonusStore, StatsAndRatingsAreIndependent)
{
    Store store;

    store.Set(1, *ParseSlot("strength"), 5);
    store.Set(1, *ParseSlot("hit"), 20);

    EXPECT_EQ(store.Get(1, *ParseSlot("strength")), 5);
    EXPECT_EQ(store.Get(1, *ParseSlot("hit")), 20);
    EXPECT_EQ(store.Get(1, *ParseSlot("hit_spell")), 0);
    EXPECT_EQ(store.Size(), 1u);
}

TEST(StatBonusStore, AnyRatingsIsFalseWhileOnlyStatsAreGranted)
{
    // It gates the per-login reconciliation, so a realm that grants nothing but
    // primary stats must not pay for the rating path at every login.
    Store store;

    store.Set(1, *ParseSlot("stamina"), 10);
    EXPECT_FALSE(store.Empty());
    EXPECT_FALSE(store.AnyRatings());

    store.Set(2, *ParseSlot("dodge"), 10);
    EXPECT_TRUE(store.AnyRatings());

    store.Clear(2);
    EXPECT_FALSE(store.AnyRatings());
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
