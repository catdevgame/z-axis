#pragma once

// Strips for the PS2 renderer, made from triangles (build time: the
// packaging step's model converter, zge/native/xbox_models.h; header-only).
//
// A PS2 strip group (TGFM, zge/render/part.h StripGroup) sends VU1 one
// vertex per entry: an entry ends the triangle of the previous two and
// itself unless it carries 0x8000 (a strip start), and 0x4000 turns that
// triangle's winding. Every entry is one vertex unpacked into the packet
// (24 or 28 bytes, zge/render/ps2/packet_size.h), so a mesh costs as many
// entries as its strips have vertices, plus two for every restart.
//
// The Xbox release's skaters are one long D3D strip per material, joined by
// degenerate triangles; converted entry for entry
// (runtime/zaxis_xbox.h restrip) every degenerate triangle stays an
// entry that draws nothing, about four entries a triangle. makeStrips
// strips the triangles again: greedily, each strip walking on to a
// neighbour across its last edge and back across its first (the winding
// flag lets any neighbour follow), starting from the triangle with the
// fewest free neighbours.

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

namespace zge::native::strips
{
    constexpr uint16_t kVertexMask = 0x3FFFu;
    constexpr uint16_t kFlipped = 0x4000u;
    constexpr uint16_t kNoTriangle = 0x8000u;

    // A triangle as drawn: its vertices in the winding order the entries
    // give (an entry with kFlipped draws (b, a, c) for (a, b, c)).
    using Triangle = std::array<uint16_t, 3>;

    // The same triangle starting at its smallest vertex (rotation kept).
    inline Triangle canonical(Triangle t)
    {
        while (t[0] > t[1] || t[0] > t[2])
            t = {t[1], t[2], t[0]};
        return t;
    }

    // The triangles a group's entries draw (a group is its own strip: its
    // first two entries draw nothing).
    inline void trianglesOf(const std::vector<uint16_t> &entries, std::vector<Triangle> &out)
    {
        for (size_t k = 2; k < entries.size(); ++k)
        {
            if (entries[k] & kNoTriangle)
                continue;
            const uint16_t a = entries[k - 2] & kVertexMask, b = entries[k - 1] & kVertexMask,
                           c = entries[k] & kVertexMask;
            if (a == b || b == c || a == c)
                continue; // degenerate: nothing drawn
            out.push_back((entries[k] & kFlipped) ? Triangle{b, a, c} : Triangle{a, b, c});
        }
    }

    // Entries (strips, restarts flagged) that draw exactly `triangles`, in
    // their winding.
    inline std::vector<uint16_t> makeStrips(const std::vector<Triangle> &triangles)
    {
        const size_t n = triangles.size();
        auto edgeKey = [](uint16_t a, uint16_t b) { return a < b ? (uint32_t(a) << 16 | b) : (uint32_t(b) << 16 | a); };
        std::unordered_map<uint32_t, std::vector<uint32_t>> byEdge;
        for (uint32_t t = 0; t < n; ++t)
            for (int e = 0; e < 3; ++e)
                byEdge[edgeKey(triangles[t][e], triangles[t][(e + 1) % 3])].push_back(t);
        std::vector<bool> used(n, false);
        auto freeNeighbours = [&](uint32_t t)
        {
            int count = 0;
            for (int e = 0; e < 3; ++e)
                for (uint32_t u : byEdge[edgeKey(triangles[t][e], triangles[t][(e + 1) % 3])])
                    count += u != t && !used[u];
            return count;
        };
        // The free triangle across edge (a, b) with the fewest free
        // neighbours of its own, or n.
        auto next = [&](uint16_t a, uint16_t b)
        {
            uint32_t best = static_cast<uint32_t>(n);
            int bestCount = INT_MAX;
            for (uint32_t u : byEdge[edgeKey(a, b)])
                if (!used[u])
                {
                    const int c = freeNeighbours(u);
                    if (c < bestCount)
                        best = u, bestCount = c;
                }
            return best;
        };
        auto thirdOf = [&](uint32_t t, uint16_t a, uint16_t b)
        {
            for (uint16_t v : triangles[t])
                if (v != a && v != b)
                    return v;
            return triangles[t][0];
        };
        // An entry drawing triangle t as (a, b, v): flipped unless t winds
        // a, b, v.
        auto entryFor = [&](uint32_t t, uint16_t a, uint16_t b, uint16_t v)
        {
            const bool same = canonical({a, b, v}) == canonical(triangles[t]);
            return static_cast<uint16_t>(v | (same ? 0u : kFlipped));
        };

        std::vector<uint16_t> out;
        for (size_t remaining = n; remaining > 0;)
        {
            // Start where the fewest free neighbours are (the mesh's edges
            // first, so strips do not strand single triangles).
            uint32_t start = static_cast<uint32_t>(n);
            int startCount = INT_MAX;
            for (uint32_t t = 0; t < n && startCount > 0; ++t)
                if (!used[t])
                {
                    const int c = freeNeighbours(t);
                    if (c < startCount)
                        start = t, startCount = c;
                }
            // Of its three rotations, the one whose strip runs longest: on
            // across its last edge, then back across its first.
            std::deque<uint16_t> bestStrip; // vertices; every three in a row a triangle
            std::deque<uint32_t> bestTriangles;
            for (int r = 0; r < 3; ++r)
            {
                std::deque<uint16_t> strip{triangles[start][r], triangles[start][(r + 1) % 3],
                                           triangles[start][(r + 2) % 3]};
                std::deque<uint32_t> taken{start};
                used[start] = true;
                for (uint32_t t = next(strip[strip.size() - 2], strip.back()); t < n;
                     t = next(strip[strip.size() - 2], strip.back()))
                {
                    strip.push_back(thirdOf(t, strip[strip.size() - 2], strip.back()));
                    used[t] = true;
                    taken.push_back(t);
                }
                for (uint32_t t = next(strip[0], strip[1]); t < n; t = next(strip[0], strip[1]))
                {
                    strip.push_front(thirdOf(t, strip[0], strip[1]));
                    used[t] = true;
                    taken.push_front(t);
                }
                for (uint32_t t : taken)
                    used[t] = false;
                if (taken.size() > bestTriangles.size())
                    bestStrip = std::move(strip), bestTriangles = std::move(taken);
            }
            out.push_back(static_cast<uint16_t>(bestStrip[0] | kNoTriangle));
            out.push_back(static_cast<uint16_t>(bestStrip[1] | kNoTriangle));
            for (size_t k = 2; k < bestStrip.size(); ++k)
                out.push_back(entryFor(bestTriangles[k - 2], bestStrip[k - 2], bestStrip[k - 1], bestStrip[k]));
            for (uint32_t t : bestTriangles)
                used[t] = true;
            remaining -= bestTriangles.size();
        }
        return out;
    }

    // Entries cut into groups of at most maxEntries: a strip that goes on
    // past a group's end starts the next group again from its last two
    // vertices (drawing nothing), as the converter's restrip does.
    inline std::vector<std::vector<uint16_t>> cutGroups(const std::vector<uint16_t> &entries,
                                                        size_t maxEntries = 60u)
    {
        std::vector<std::vector<uint16_t>> out;
        maxEntries = std::max<size_t>(maxEntries, 4u);
        size_t k = 0;
        while (k < entries.size())
        {
            std::vector<uint16_t> group;
            const bool starts = (entries[k] & kNoTriangle) != 0u &&
                                (k + 1u >= entries.size() || (entries[k + 1] & kNoTriangle) != 0u);
            if (k >= 2u && !starts)
            {
                group.push_back(static_cast<uint16_t>((entries[k - 2] & kVertexMask) | kNoTriangle));
                group.push_back(static_cast<uint16_t>((entries[k - 1] & kVertexMask) | kNoTriangle));
            }
            while (k < entries.size() && group.size() < maxEntries)
            {
                // A strip start that would leave only its two entries in this
                // group (they draw nothing) goes to the next group.
                if (group.size() + 2u >= maxEntries && (entries[k] & kNoTriangle) &&
                    k + 1u < entries.size() && (entries[k + 1] & kNoTriangle))
                    break;
                group.push_back(entries[k++]);
            }
            out.push_back(std::move(group));
        }
        return out;
    }
}
