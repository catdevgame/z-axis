// The collection's order of its games (collection/game_order.h).
#include "collection/game_order.h"

#include <algorithm>
#include <numeric>

namespace ps2x::collection
{
    std::vector<size_t> inGameOrder(const std::vector<std::string_view> &ids)
    {
        std::vector<size_t> order(ids.size());
        std::iota(order.begin(), order.end(), size_t{0});
        std::stable_sort(order.begin(), order.end(), [&ids](size_t a, size_t b) { return gameRank(ids[a]) < gameRank(ids[b]); });
        return order;
    }
}
