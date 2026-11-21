// Triangle strips for the converter's parts (ai_level_convert.py stripify,
// pack_strips, face_normal).
#include "converter.h"

#include "collection/levels/arithmetic.h"

#include <cmath>
#include <unordered_map>

namespace zx::app::levels
{
    namespace
    {
        uint64_t edgeKey(int a, int b) { return uint64_t(uint32_t(a)) << 32 | uint32_t(b); }
    }

    std::vector<std::vector<int>> stripify(const std::vector<std::array<int, 3>> &triangles)
    {
        // Each directed edge's triangles: a strip goes on across an edge to the
        // triangle that has it the other way round.
        std::unordered_map<uint64_t, std::vector<int>> byEdge;
        for (size_t i = 0; i < triangles.size(); ++i)
        {
            const auto &[a, b, c] = triangles[i];
            for (const auto &[p, q] : {std::pair{a, b}, std::pair{b, c}, std::pair{c, a}})
                byEdge[edgeKey(p, q)].push_back(static_cast<int>(i));
        }
        std::vector<bool> used(triangles.size(), false);
        auto take = [&](int p, int q) -> int
        {
            auto it = byEdge.find(edgeKey(p, q));
            if (it != byEdge.end())
                for (int i : it->second)
                    if (!used[static_cast<size_t>(i)])
                        return i;
            return -1;
        };
        std::vector<std::vector<int>> strips;
        for (size_t start = 0; start < triangles.size(); ++start)
        {
            if (used[start])
                continue;
            used[start] = true;
            auto [a, b, c] = triangles[start];
            // the start turned so the strip can go on along an edge with an unused neighbour
            const std::array<std::array<int, 3>, 3> turns = {{{a, b, c}, {b, c, a}, {c, a, b}}};
            for (const auto &t : turns)
                if (take(t[2], t[1]) >= 0)
                {
                    a = t[0];
                    b = t[1];
                    c = t[2];
                    break;
                }
            std::vector<int> strip = {a, b, c};
            while (true)
            {
                const size_t k = strip.size() - 2; // the next triangle's index in the strip
                const int v = strip[strip.size() - 2], w = strip[strip.size() - 1];
                // triangle k is (v, w, x) when k is even, (w, v, x) when odd
                const int i = k % 2 == 0 ? take(v, w) : take(w, v);
                if (i < 0)
                    break;
                used[static_cast<size_t>(i)] = true;
                for (int q : triangles[static_cast<size_t>(i)])
                    if (q != v && q != w)
                    {
                        strip.push_back(q);
                        break;
                    }
            }
            strips.push_back(std::move(strip));
        }
        return strips;
    }

    std::vector<StripGroup> packStrips(int submesh, const std::vector<std::vector<int>> &strips)
    {
        const size_t maxEntries = StripGroup::kGroupEntries;
        std::vector<StripGroup> groups;
        std::vector<uint16_t> current;
        auto flush = [&]()
        {
            if (current.empty())
                return;
            StripGroup g;
            g.submesh = static_cast<uint8_t>(submesh);
            g.entries = current;
            groups.push_back(std::move(g));
            current.clear();
        };
        for (const std::vector<int> &strip : strips)
        {
            std::vector<uint16_t> entries;
            for (size_t k = 0; k < strip.size(); ++k)
            {
                const int v = strip[k];
                if (k < 2 || strip[k - 2] == strip[k - 1] || strip[k - 1] == v || strip[k - 2] == v)
                    entries.push_back(static_cast<uint16_t>(v | StripGroup::kNoTriangle));
                else
                    entries.push_back(static_cast<uint16_t>(v | ((k & 1) ? StripGroup::kFlipped : 0)));
            }
            if (entries.size() > maxEntries)
            {
                flush();
                for (StripGroup &g : StripGroup::fromStrip(submesh, strip, 0, maxEntries))
                    groups.push_back(std::move(g));
                continue;
            }
            if (current.size() + entries.size() > maxEntries)
                flush();
            current.insert(current.end(), entries.begin(), entries.end());
        }
        flush();
        return groups;
    }

    Vec3 faceNormal(const Vec3 &a, const Vec3 &b, const Vec3 &c)
    {
        const Vec3 u = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const Vec3 v = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const Vec3 n = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        double length = std::sqrt(sumOf({n[0] * n[0], n[1] * n[1], n[2] * n[2]}));
        if (length == 0.0)
            length = 1.0;
        return {n[0] / length, n[1] / length, n[2] / length};
    }
}
