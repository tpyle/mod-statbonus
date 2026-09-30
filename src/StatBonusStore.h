/*
 * mod-statbonus - the parts that are just rules, with no server behind them.
 *
 * Deliberately includes nothing but the standard library, so tests/ can build
 * and run this without AzerothCore present. The store is keyed on a character's
 * GUID counter rather than an ObjectGuid for the same reason.
 *
 * The two kinds of bonus share one flat slot space - the five primary stats
 * first, then the twenty-five combat ratings - because everything above the
 * point of application treats them alike: one name to parse, one number to
 * store, one row in the table. They part company only where they are handed to
 * the server, and that is the module's business rather than this header's.
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
    // Matches the core's MAX_STATS and MAX_COMBAT_RATING. Slots 0-4 are the
    // Stats enum in its own order, slots 5-29 the CombatRating enum in its own
    // order, so a slot converts to either by adding or subtracting STAT_COUNT.
    constexpr std::size_t STAT_COUNT   = 5;
    constexpr std::size_t RATING_COUNT = 25;
    constexpr std::size_t SLOT_COUNT   = STAT_COUNT + RATING_COUNT;

    // What the table's Kind column holds. Stored per kind rather than as the
    // flat slot so the rows stay readable against the core's enums if the
    // number of either ever changes.
    enum Kind : std::uint8_t
    {
        KIND_STAT   = 0,
        KIND_RATING = 1
    };

    using Bonuses = std::array<std::int32_t, SLOT_COUNT>;

    inline bool IsRating(std::size_t slot) { return slot >= STAT_COUNT && slot < SLOT_COUNT; }

    inline std::size_t RatingSlot(std::size_t rating) { return STAT_COUNT + rating; }
    inline std::size_t RatingIndex(std::size_t slot)  { return slot - STAT_COUNT; }

    inline std::optional<std::size_t> SlotOf(std::uint8_t kind, std::size_t index)
    {
        if (kind == KIND_STAT && index < STAT_COUNT)
            return index;

        if (kind == KIND_RATING && index < RATING_COUNT)
            return RatingSlot(index);

        return std::nullopt;
    }

    inline std::uint8_t KindOf(std::size_t slot) { return IsRating(slot) ? KIND_RATING : KIND_STAT; }
    inline std::size_t IndexOf(std::size_t slot) { return IsRating(slot) ? RatingIndex(slot) : slot; }

    // The canonical name of every slot, which is also the name the commands
    // print. Each one has to parse back to its own slot; a test holds that.
    inline char const* SlotName(std::size_t slot)
    {
        switch (slot)
        {
            case  0: return "strength";
            case  1: return "agility";
            case  2: return "stamina";
            case  3: return "intellect";
            case  4: return "spirit";

            case  5: return "weapon_skill";           // CR_WEAPON_SKILL
            case  6: return "defense";                // CR_DEFENSE_SKILL
            case  7: return "dodge";                  // CR_DODGE
            case  8: return "parry";                  // CR_PARRY
            case  9: return "block";                  // CR_BLOCK
            case 10: return "hit";                    // CR_HIT_MELEE
            case 11: return "hit_ranged";             // CR_HIT_RANGED
            case 12: return "hit_spell";              // CR_HIT_SPELL
            case 13: return "crit";                   // CR_CRIT_MELEE
            case 14: return "crit_ranged";            // CR_CRIT_RANGED
            case 15: return "crit_spell";             // CR_CRIT_SPELL
            case 16: return "hit_taken";              // CR_HIT_TAKEN_MELEE
            case 17: return "hit_taken_ranged";       // CR_HIT_TAKEN_RANGED
            case 18: return "hit_taken_spell";        // CR_HIT_TAKEN_SPELL
            case 19: return "crit_taken";             // CR_CRIT_TAKEN_MELEE
            case 20: return "crit_taken_ranged";      // CR_CRIT_TAKEN_RANGED
            case 21: return "crit_taken_spell";       // CR_CRIT_TAKEN_SPELL
            case 22: return "haste";                  // CR_HASTE_MELEE
            case 23: return "haste_ranged";           // CR_HASTE_RANGED
            case 24: return "haste_spell";            // CR_HASTE_SPELL
            case 25: return "weapon_skill_mainhand";  // CR_WEAPON_SKILL_MAINHAND
            case 26: return "weapon_skill_offhand";   // CR_WEAPON_SKILL_OFFHAND
            case 27: return "weapon_skill_ranged";    // CR_WEAPON_SKILL_RANGED
            case 28: return "expertise";              // CR_EXPERTISE
            case 29: return "armor_penetration";      // CR_ARMOR_PENETRATION

            default: return "unknown";
        }
    }

    // True where the bonus is a rating the server reads as a number of points
    // rather than a percentage, which is every rating except the two skills.
    // Only used to word the command's confirmation.
    inline bool IsSkillRating(std::size_t slot)
    {
        return slot == 5 || slot == 6 || (slot >= 25 && slot <= 27);
    }

    // Accepts the canonical name above, the abbreviations the character sheet
    // itself uses, the melee aliases spelled out, and an explicit "stat:<n>" or
    // "rating:<n>" for addressing the core's enums by index. A bare number is
    // deliberately NOT accepted: it would have to mean a stat or a rating, and
    // a bonus landing on the wrong one of those is invisible until the sheet
    // looks wrong, which is a long way from the cause.
    inline std::optional<std::size_t> ParseSlot(std::string_view name)
    {
        // Trim the edges, then treat a hyphen or an interior space as the
        // underscore it was meant to be: "armor penetration" and "armor-pen"
        // are both what somebody types for armor_penetration.
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front())))
            name.remove_prefix(1);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())))
            name.remove_suffix(1);

        std::string key;
        key.reserve(name.size());
        for (char const c : name)
        {
            if (c == '-' || c == ' ')
                key.push_back('_');
            else
                key.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }

        // stat:<n> / rating:<n>, by index into the core's own enum
        auto const colon = key.find(':');
        if (colon != std::string::npos)
        {
            std::string const kind = key.substr(0, colon);
            std::string const rest = key.substr(colon + 1);

            if (rest.empty() || rest.size() > 2 || !std::all_of(rest.begin(), rest.end(),
                [](char c) { return c >= '0' && c <= '9'; }))
                return std::nullopt;

            std::size_t const index = static_cast<std::size_t>(std::stoul(rest));

            if (kind == "stat")
                return SlotOf(KIND_STAT, index);
            if (kind == "rating" || kind == "cr")
                return SlotOf(KIND_RATING, index);

            return std::nullopt;
        }

        // Primary stats.
        if (key == "strength"  || key == "str") return 0;
        if (key == "agility"   || key == "agi") return 1;
        if (key == "stamina"   || key == "sta" || key == "stam") return 2;
        if (key == "intellect" || key == "int") return 3;
        if (key == "spirit"    || key == "spi") return 4;

        // Canonical rating names.
        for (std::size_t slot = STAT_COUNT; slot < SLOT_COUNT; ++slot)
            if (key == SlotName(slot))
                return slot;

        // Aliases. "hit", "crit" and "haste" already mean the melee rating,
        // because that is what the sheet calls them, but the spelled-out forms
        // read better in a command and both orders get typed.
        if (key == "defense_skill" || key == "def")           return 6;
        if (key == "hit_melee"     || key == "melee_hit")     return 10;
        if (key == "ranged_hit")                              return 11;
        if (key == "spell_hit")                               return 12;
        if (key == "crit_melee"    || key == "melee_crit")    return 13;
        if (key == "ranged_crit")                             return 14;
        if (key == "spell_crit")                              return 15;
        if (key == "hit_taken_melee")                         return 16;
        // No bare "resilience": the item stat of that name feeds all three of
        // these at once, so a single name for one of them would promise more
        // than it grants.
        if (key == "crit_taken_melee" || key == "resilience_melee")  return 19;
        if (key == "resilience_ranged")                       return 20;
        if (key == "resilience_spell")                        return 21;
        if (key == "haste_melee"   || key == "melee_haste")   return 22;
        if (key == "ranged_haste")                            return 23;
        if (key == "spell_haste")                             return 24;
        if (key == "armorpen" || key == "arp" || key == "armor_pen") return 29;

        return std::nullopt;
    }

    // A bonus may be negative - taking a point away is as reasonable a use as
    // granting one - but the stat the client is shown must not go below zero,
    // and the field itself is unsigned. Ratings are clamped by the core's own
    // UpdateRating, so this is only used for the five primary stats.
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
        std::int32_t Get(std::uint32_t guid, std::size_t slot) const
        {
            if (slot >= SLOT_COUNT)
                return 0;

            auto const it = _bonuses.find(guid);
            return it == _bonuses.end() ? 0 : it->second[slot];
        }

        Bonuses const* Find(std::uint32_t guid) const
        {
            auto const it = _bonuses.find(guid);
            return it == _bonuses.end() ? nullptr : &it->second;
        }

        // Returns the new total for that slot.
        std::int32_t Add(std::uint32_t guid, std::size_t slot, std::int32_t delta)
        {
            if (slot >= SLOT_COUNT)
                return 0;

            return Set(guid, slot, Get(guid, slot) + delta);
        }

        std::int32_t Set(std::uint32_t guid, std::size_t slot, std::int32_t amount)
        {
            if (slot >= SLOT_COUNT)
                return 0;

            if (amount == 0)
            {
                Clear(guid, slot);
                return 0;
            }

            _bonuses[guid][slot] = amount;
            return amount;
        }

        bool Clear(std::uint32_t guid, std::size_t slot)
        {
            if (slot >= SLOT_COUNT)
                return false;

            auto const it = _bonuses.find(guid);
            if (it == _bonuses.end() || it->second[slot] == 0)
                return false;

            it->second[slot] = 0;

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

        // True when no character has a rating bonus, which lets the module skip
        // the per-login reconciliation entirely on a realm that only grants
        // primary stats.
        bool AnyRatings() const
        {
            for (auto const& [guid, bonuses] : _bonuses)
                for (std::size_t slot = STAT_COUNT; slot < SLOT_COUNT; ++slot)
                    if (bonuses[slot])
                        return true;

            return false;
        }

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
