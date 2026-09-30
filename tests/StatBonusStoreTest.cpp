/*
 * Tests for mod-statbonus' rules (src/StatBonusStore.h).
 *
 * What is worth pinning down here is not arithmetic for its own sake. A bonus
 * that silently lands on the wrong stat, a zero left in the table so the module
 * believes it is doing work, or a negative grant that pushes the unsigned stat
 * field below zero are all invisible in play until a character looks wrong on
 * the sheet, which is a long way from the cause.
 */

#include "StatBonusStore.h"

#include <gtest/gtest.h>

using namespace StatBonus;

TEST(StatBonusParse, AcceptsNamesAbbreviationsAndIndices)
{
    EXPECT_EQ(ParseStat("strength"), 0u);
    EXPECT_EQ(ParseStat("STRENGTH"), 0u);
    EXPECT_EQ(ParseStat("str"), 0u);
    EXPECT_EQ(ParseStat("agi"), 1u);
    EXPECT_EQ(ParseStat("stam"), 2u);
    EXPECT_EQ(ParseStat("intellect"), 3u);
    EXPECT_EQ(ParseStat("spi"), 4u);

    for (std::size_t i = 0; i < STAT_COUNT; ++i)
        EXPECT_EQ(ParseStat(std::to_string(i)), i);
}

TEST(StatBonusParse, RejectsAnythingElse)
{
    // A command that guessed here would write the bonus to some other stat and
    // report success, so every one of these has to come back empty.
    EXPECT_FALSE(ParseStat("").has_value());
    EXPECT_FALSE(ParseStat("s").has_value());          // ambiguous between strength, stamina, spirit
    EXPECT_EQ(ParseStat("st"), std::nullopt);          // same
    EXPECT_FALSE(ParseStat("5").has_value());          // one past the last stat
    EXPECT_FALSE(ParseStat("-1").has_value());
    EXPECT_FALSE(ParseStat("armor").has_value());
    EXPECT_FALSE(ParseStat("strength ").has_value());  // the command splits on spaces; a stray one is a typo
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
    EXPECT_EQ(store.Size(), 0u);
    EXPECT_EQ(store.Get(42, 0), 0);
    EXPECT_EQ(store.Find(42), nullptr);
}

TEST(StatBonusStore, SetAndGetAreIndependentPerStatAndPerCharacter)
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
    // Empty() gates the hook's hot path, so a character whose bonus has been
    // added and then taken away again must not leave the store looking busy.
    Store store;

    store.Add(1, 0, 3);
    EXPECT_FALSE(store.Empty());

    store.Add(1, 0, -3);
    EXPECT_TRUE(store.Empty());
    EXPECT_EQ(store.Find(1), nullptr);
}

TEST(StatBonusStore, ClearingOneStatKeepsTheOthers)
{
    Store store;

    store.Set(1, 0, 5);
    store.Set(1, 4, 2);

    EXPECT_TRUE(store.Clear(1, 0));
    EXPECT_EQ(store.Get(1, 0), 0);
    EXPECT_EQ(store.Get(1, 4), 2);
    EXPECT_FALSE(store.Empty());

    EXPECT_TRUE(store.Clear(1, 4));
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

TEST(StatBonusStore, OutOfRangeStatsAreIgnoredRatherThanWritten)
{
    // The stat comes from a tinyint column that nothing stops a GM editing by
    // hand, and from the Stats enum. Neither may write past the array.
    Store store;

    EXPECT_EQ(store.Set(1, STAT_COUNT, 5), 0);
    EXPECT_EQ(store.Add(1, 99, 5), 0);
    EXPECT_EQ(store.Get(1, STAT_COUNT), 0);
    EXPECT_FALSE(store.Clear(1, STAT_COUNT));
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

TEST(StatBonusNames, EveryStatHasAName)
{
    for (std::size_t i = 0; i < STAT_COUNT; ++i)
    {
        EXPECT_STRNE(StatName(i), "unknown");
        // A name has to round-trip, or the command would print something it
        // could not then be given back.
        EXPECT_EQ(ParseStat(StatName(i)), i);
    }

    EXPECT_STREQ(StatName(STAT_COUNT), "unknown");
}
