#pragma once

// The order the collection shows its games in, everywhere: release order,
// Dave Mirra Freestyle BMX 2 (2001), Aggressive Inline (2002), BMX XXX (late
// 2002). Every list of the games the collection draws or walks reads this
// one list (the menu's tiles and cards, then Custom Freeplay; the Thor's
// bottom screen; Settings > Games; the Mods page's games; Custom Freeplay's
// players and maps), whatever order the build lists them in. What is saved
// stays keyed by the game's id, never by its place here.

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace ps2x::collection
{
    inline constexpr std::array<std::string_view, 3> kGameOrder = {"dm2", "ai", "bmx"};

    // A game's place in kGameOrder; a game the collection does not know
    // comes after them (kGameOrder.size()).
    constexpr size_t gameRank(std::string_view game)
    {
        for (size_t i = 0; i < kGameOrder.size(); ++i)
            if (kGameOrder[i] == game)
                return i;
        return kGameOrder.size();
    }

    // The indices of `ids` (a list in any order, e.g. the build's) in the
    // collection's order; games it does not know keep their own order after
    // the known ones.
    std::vector<size_t> inGameOrder(const std::vector<std::string_view> &ids);
}
