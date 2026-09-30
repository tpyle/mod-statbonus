/*
 * mod-statbonus - the parts that are just rules, with no server behind them.
 *
 * Deliberately includes nothing but the standard library, so tests/ can build
 * and run this without AzerothCore present. The store is keyed on a character's
 * GUID counter rather than an ObjectGuid for the same reason.
 */

#ifndef MOD_STATBONUS_STORE_H
#define MOD_STATBONUS_STORE_H

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace StatBonus
{
    // Matches the core's MAX_STATS: strength, agility, stamina, intellect,
    // spirit, in that order, which is also the order of the Stats enum.
    constexpr std::size_t STAT_COUNT = 5;

    using Bonuses = std::array<std::int32_t, STAT_COUNT>;

    inline char const* StatName(std::size_t stat)
    {
        switch (stat)
        {
            case 0: return "strength";
            case 1: return "agility";
            case 2: return "stamina";
            case 3: return "intellect";
            case 4: return "spirit";
            default: return "unknown";
        }
    }

    // Accepts the full name, the three-letter abbreviation the game itself uses
    // on the character sheet, or the raw index. Returns nothing for anything
    // else, because a command that guessed would write a bonus to the wrong
    // stat and look like it had worked.
    inline std::optional<std::size_t> ParseStat(std::string_view name)
    {
        std::string key;
        key.reserve(name.size());
        for (char const c : name)
            key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

        if (key == "strength"  || key == "str") return 0;
        if (key == "agility"   || key == "agi") return 1;
        if (key == "stamina"   || key == "sta" || key == "stam") return 2;
        if (key == "intellect" || key == "int") return 3;
        if (key == "spirit"    || key == "spi") return 4;

        if (key.size() == 1 && key[0] >= '0' && key[0] <= '4')
            return static_cast<std::size_t>(key[0] - '0');

        return std::nullopt;
    }

    // A bonus may be negative - taking a point away is as reasonable a use as
    // granting one - but the stat the client is shown must not go below zero,
    // and the field itself is unsigned.
    inline float Apply(float value, std::int32_t bonus)
    {
        return std::max(0.0f, value + static_cast<float>(bonus));
    }

    // limit 0 means no limit. Symmetric, so a limit of 50 permits -50 as well.
    inline std::int32_t ClampToLimit(std::int32_t amount, std::int32_t limit)
    {
        if (limit <= 0)
            return amount;

        return std::clamp(amount, -limit, limit);
    }

    class Store
    {
    public:
        // The hot path: called once per stat recalculation, for every stat, for
        // every character. Empty() is checked first by the caller so an unused
        // module costs one branch.
        std::int32_t Get(std::uint32_t guid, std::size_t stat) const
        {
            if (stat >= STAT_COUNT)
                return 0;

            auto const it = _bonuses.find(guid);
            return it == _bonuses.end() ? 0 : it->second[stat];
        }

        Bonuses const* Find(std::uint32_t guid) const
        {
            auto const it = _bonuses.find(guid);
            return it == _bonuses.end() ? nullptr : &it->second;
        }

        // Returns the new total for that stat.
        std::int32_t Add(std::uint32_t guid, std::size_t stat, std::int32_t delta)
        {
            if (stat >= STAT_COUNT)
                return 0;

            return Set(guid, stat, Get(guid, stat) + delta);
        }

        std::int32_t Set(std::uint32_t guid, std::size_t stat, std::int32_t amount)
        {
            if (stat >= STAT_COUNT)
                return 0;

            if (amount == 0)
            {
                Clear(guid, stat);
                return 0;
            }

            _bonuses[guid][stat] = amount;
            return amount;
        }

        bool Clear(std::uint32_t guid, std::size_t stat)
        {
            if (stat >= STAT_COUNT)
                return false;

            auto const it = _bonuses.find(guid);
            if (it == _bonuses.end() || it->second[stat] == 0)
                return false;

            it->second[stat] = 0;

            // Drop the row entirely once nothing is left in it, so Empty()
            // stays an honest answer to "is this module doing anything".
            if (std::all_of(it->second.begin(), it->second.end(), [](std::int32_t v) { return v == 0; }))
                _bonuses.erase(it);

            return true;
        }

        bool Clear(std::uint32_t guid)
        {
            return _bonuses.erase(guid) != 0;
        }

        void Reset() { _bonuses.clear(); }

        bool Empty() const { return _bonuses.empty(); }
        std::size_t Size() const { return _bonuses.size(); }

        // Sorted by GUID so ".statbonus list" prints in a stable order rather
        // than in whatever order the hash table happens to hold.
        std::vector<std::pair<std::uint32_t, Bonuses>> All() const
        {
            std::vector<std::pair<std::uint32_t, Bonuses>> out(_bonuses.begin(), _bonuses.end());
            std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) { return a.first < b.first; });
            return out;
        }

    private:
        std::unordered_map<std::uint32_t, Bonuses> _bonuses;
    };
}

#endif // MOD_STATBONUS_STORE_H
