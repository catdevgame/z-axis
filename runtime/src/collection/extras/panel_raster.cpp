// The panel's map of a level (panel_maps_parts.h): the level proper's box,
// the picture of its faces from above as a relief map, what the skater can
// reach, walls and outlines, rails, and the level file around it. A
// span-filling rasteriser with a height buffer: the highest surface at a
// pixel wins whatever order the faces come in.
#include "panel_maps_parts.h"

#include <algorithm>

namespace zx::app::extras::panel_maps
{
    namespace
    {
        constexpr double kPixelsPerUnit = 3.0;
        constexpr double kMaxSide = 2400;
        constexpr double kMargin = 12.0; // world units around the skateable area
        constexpr double kCell = 8.0;    // world units, for finding the level proper
        constexpr uint8_t kBackground[3] = {10, 13, 19};
        constexpr uint8_t kWaterColour[3] = {44, 86, 128};
        constexpr uint8_t kWallColour[3] = {52, 56, 66};
        constexpr uint8_t kWaterCode = 255;
        constexpr uint8_t kWallHeight = 240;
        constexpr double kGapUnits = 12.0; // what a building's wall can be thick

        // The height buffer: 0 no ground, else 2 + kHeightSteps over the
        // level's span (the lowest first).
        constexpr int64_t kHeightSteps = 60000;
        constexpr int64_t kHeightNone = 0, kHeightFirst = 2;
        constexpr uint8_t kKindGround = 0, kKindWater = 1;
        constexpr double kLightFlat = 0.62; // the lowest flat ground's light; the highest gets + kLightRise
        constexpr double kLightRise = 0.38;
        constexpr double kLightSlope = 0.32; // a 45 degree slope to the upper left brightens by this much

        constexpr double kCellUnits = 1.0;  // the walk's grid
        constexpr double kStepUnits = 3.5;  // what the skater rolls or hops up
        constexpr double kSeedUnits = 6.0;  // how far over its surface a thing can float

        // What no level comes near (a few thousand cells, lines of a few
        // thousand pixels): a file past them does not hold a level.
        constexpr size_t kMostCells = 1000000;
        constexpr int64_t kMostLineSteps = 10000000;

        // ---- the cells, in the order the reference's set keeps them ---------

        // panel_map.py finds the level proper of a level with no start
        // points (DM2's, BMX XXX's) from the cell of the densest
        // neighbourhood: the first such cell its Python set yields. So the
        // cells are kept as CPython's set keeps (x, z) tuples (setobject.c:
        // the tuple's xxHash, linear probes then perturbed ones, the table
        // grown at 3/5 full), for the same first cell.
        class CellSet
        {
        public:
            CellSet() : m_table(8) {}

            void add(int64_t x, int64_t z)
            {
                const uint64_t hash = tupleHash(x, z);
                size_t mask = m_table.size() - 1;
                size_t i = size_t(hash) & mask;
                uint64_t perturb = hash;
                while (true)
                {
                    const size_t probes = i + kLinearProbes <= mask ? kLinearProbes : 0;
                    for (size_t k = 0; k <= probes; ++k)
                    {
                        Entry &e = m_table[i + k];
                        if (!e.used)
                        {
                            e = {true, hash, x, z};
                            ++m_used;
                            if (m_used * 5 < mask * 3)
                                return;
                            grow(m_used > 50000 ? m_used * 2 : m_used * 4);
                            return;
                        }
                        if (e.hash == hash && e.x == x && e.z == z)
                            return;
                    }
                    perturb >>= 5;
                    i = size_t(i * 5 + 1 + perturb) & mask;
                }
            }

            bool contains(int64_t x, int64_t z) const
            {
                const uint64_t hash = tupleHash(x, z);
                const size_t mask = m_table.size() - 1;
                size_t i = size_t(hash) & mask;
                uint64_t perturb = hash;
                while (true)
                {
                    const size_t probes = i + kLinearProbes <= mask ? kLinearProbes : 0;
                    for (size_t k = 0; k <= probes; ++k)
                    {
                        const Entry &e = m_table[i + k];
                        if (!e.used)
                            return false;
                        if (e.hash == hash && e.x == x && e.z == z)
                            return true;
                    }
                    perturb >>= 5;
                    i = size_t(i * 5 + 1 + perturb) & mask;
                }
            }

            size_t size() const { return m_used; }
            // Each cell in the set's order.
            template <typename Visit>
            void each(Visit visit) const
            {
                for (const Entry &e : m_table)
                    if (e.used)
                        visit(e.x, e.z);
            }

        private:
            static constexpr size_t kLinearProbes = 9;
            struct Entry
            {
                bool used = false;
                uint64_t hash = 0;
                int64_t x = 0, z = 0;
            };

            static uint64_t intHash(int64_t v) { return uint64_t(v == -1 ? -2 : v); }
            static uint64_t tupleHash(int64_t x, int64_t z)
            {
                constexpr uint64_t kPrime1 = 11400714785074694791ull, kPrime2 = 14029467366897019727ull,
                                   kPrime5 = 2870177450012600261ull;
                uint64_t acc = kPrime5;
                for (const int64_t item : {x, z})
                {
                    acc += intHash(item) * kPrime2;
                    acc = (acc << 31) | (acc >> 33);
                    acc *= kPrime1;
                }
                acc += 2 ^ (kPrime5 ^ 3527539ull);
                return acc == uint64_t(-1) ? 1546275796ull : acc;
            }

            void grow(size_t least)
            {
                size_t size = 8;
                while (size <= least)
                    size <<= 1;
                std::vector<Entry> old = std::move(m_table);
                m_table.assign(size, Entry{});
                const size_t mask = size - 1;
                for (const Entry &e : old)
                {
                    if (!e.used)
                        continue;
                    size_t i = size_t(e.hash) & mask;
                    uint64_t perturb = e.hash;
                    while (true)
                    {
                        if (!m_table[i].used)
                        {
                            m_table[i] = e;
                            break;
                        }
                        bool placed = false;
                        if (i + kLinearProbes <= mask)
                            for (size_t k = 1; k <= kLinearProbes; ++k)
                                if (!m_table[i + k].used)
                                {
                                    m_table[i + k] = e;
                                    placed = true;
                                    break;
                                }
                        if (placed)
                            break;
                        perturb >>= 5;
                        i = size_t(i * 5 + 1 + perturb) & mask;
                    }
                }
            }

            std::vector<Entry> m_table;
            size_t m_used = 0;
        };

        // `count` codes from c0 to c1, the last one c1.
        void ramp(int64_t c0, int64_t c1, int64_t count, std::vector<int64_t> &out)
        {
            out.resize(size_t(std::max<int64_t>(count, 0)));
            if (count < 2 || c0 == c1)
            {
                std::fill(out.begin(), out.end(), c0);
                return;
            }
            const int64_t run = count - 1;
            for (int64_t k = 0; k < count; ++k)
                out[size_t(k)] = c0 + floorDiv((c1 - c0) * k, run);
        }

        // A face's corner on the picture: x, y and the values blended along
        // its edges (a height code, then a colour's three channels).
        struct SpanPoint
        {
            double x, y;
            std::array<double, 4> v;
        };

        // For each picture row the face covers: (y, x0, x1, the values at
        // the span's two ends); the first `N` values are blended.
        template <size_t N, typename Visit>
        void faceSpans(std::array<SpanPoint, 3> points, int64_t width, int64_t height, Visit visit)
        {
            std::stable_sort(points.begin(), points.end(), [](const SpanPoint &p, const SpanPoint &q) { return p.y < q.y; });
            const SpanPoint &a = points[0], &b = points[1], &c = points[2];
            const int64_t y0 = std::max<int64_t>(0, truncated(a.y));
            const int64_t y1 = std::min<int64_t>(height - 1, truncated(c.y));
            std::array<double, 4> va{}, vb{};
            for (int64_t y = y0; y <= y1; ++y)
            {
                const double yy = pyMin(c.y, pyMax(a.y, double(y) + 0.5));
                const double tLong = (yy - a.y) / (c.y - a.y + 1e-9);
                double xa = a.x + (c.x - a.x) * tLong;
                for (size_t k = 0; k < N; ++k)
                    va[k] = a.v[k] + (c.v[k] - a.v[k]) * tLong;
                double xb;
                if (yy < b.y)
                {
                    const double tShort = (yy - a.y) / (b.y - a.y + 1e-9);
                    xb = a.x + (b.x - a.x) * tShort;
                    for (size_t k = 0; k < N; ++k)
                        vb[k] = a.v[k] + (b.v[k] - a.v[k]) * tShort;
                }
                else
                {
                    const double tShort = (yy - b.y) / (c.y - b.y + 1e-9);
                    xb = b.x + (c.x - b.x) * tShort;
                    for (size_t k = 0; k < N; ++k)
                        vb[k] = b.v[k] + (c.v[k] - b.v[k]) * tShort;
                }
                const std::array<double, 4> *v0 = &va, *v1 = &vb;
                if (xa > xb)
                {
                    std::swap(xa, xb);
                    std::swap(v0, v1);
                }
                const int64_t i = std::max<int64_t>(0, truncated(xa));
                const int64_t j = std::min<int64_t>(width, truncated(xb) + 1);
                if (j > i)
                    visit(y, i, j, *v0, *v1);
            }
        }

        // A face's own colour as the picture keeps it before the light:
        // bright vertex-lit roofs roll off instead of clipping.
        std::array<uint8_t, 3> albedo(const Rgb &colour)
        {
            std::array<uint8_t, 3> out{};
            for (size_t k = 0; k < 3; ++k)
            {
                const double v = colour[k];
                const int64_t level = truncated(v < 165 ? v : 165 + (v - 165) * 60 / (60 + v - 165));
                out[k] = uint8_t(std::max<int64_t>(0, std::min<int64_t>(255, level)));
            }
            return out;
        }

        // A pixel's light (a byte): flat ground by its height, a slope to the
        // upper left brighter and away from it darker, as a printed relief map.
        int64_t lightOf(int64_t code, int64_t dx, int64_t dy, uint8_t kind, double slopeGain)
        {
            if (code == kHeightNone || kind == kKindWater)
                return 255;
            double light = kLightFlat + kLightRise * double(code - kHeightFirst) / double(kHeightSteps);
            double slope = double(-(dx + dy)) * slopeGain;
            if (slope > 1.0)
                slope = 1.0;
            else if (slope < -1.0)
                slope = -1.0;
            light *= 1.0 + kLightSlope * slope;
            return std::max<int64_t>(24, std::min<int64_t>(255, truncated(light * 255)));
        }
    }

    // ---- the level proper -----------------------------------------------------

    Box levelBox(const std::vector<MapFace> &faces, const std::vector<Seed> &seeds)
    {
        CellSet cells;
        for (const MapFace &t : faces)
        {
            if (t.water)
                continue; // the sea goes on past the level
            const double area = std::fabs((t.b[0] - t.a[0]) * (t.c[2] - t.a[2]) - (t.b[2] - t.a[2]) * (t.c[0] - t.a[0]));
            const double minX = std::min({t.a[0], t.b[0], t.c[0]}), maxX = std::max({t.a[0], t.b[0], t.c[0]});
            const double minZ = std::min({t.a[2], t.b[2], t.c[2]}), maxZ = std::max({t.a[2], t.b[2], t.c[2]});
            const int64_t cx0 = floorByPowerOfTwo(minX, kCell), cx1 = floorByPowerOfTwo(maxX, kCell);
            const int64_t cz0 = floorByPowerOfTwo(minZ, kCell), cz1 = floorByPowerOfTwo(maxZ, kCell);
            // (the spans as doubles: exact while small, and far cells do not overflow)
            const double spanX = double(cx1) - double(cx0), spanZ = double(cz1) - double(cz0);
            if (area < 2 && spanX + spanZ == 0)
                continue;
            if ((spanX + 1) * (spanZ + 1) > 6400)
                continue; // a far panorama's sheet
            for (int64_t cx = cx0; cx <= cx1; ++cx)
                for (int64_t cz = cz0; cz <= cz1; ++cz)
                    cells.add(cx, cz);
            if (cells.size() > kMostCells)
                throw PanelError("a level whose faces spread too far to map");
        }
        if (cells.size() == 0)
            throw PanelError("a level with no faces to map");
        std::vector<std::pair<int64_t, int64_t>> todo;
        for (const Seed &p : seeds)
        {
            const int64_t cx = floorByPowerOfTwo(p.x, kCell), cz = floorByPowerOfTwo(p.z, kCell);
            if (cells.contains(cx, cz))
                todo.emplace_back(cx, cz);
        }
        if (todo.empty())
        {
            // the cell with the most cells around it, the set's first of those
            int best = -1;
            std::pair<int64_t, int64_t> bestCell;
            cells.each([&](int64_t x, int64_t z) {
                int around = 0;
                for (int i = -4; i <= 4; ++i)
                    for (int j = -4; j <= 4; ++j)
                        around += cells.contains(x + i, z + j) ? 1 : 0;
                if (around > best)
                {
                    best = around;
                    bestCell = {x, z};
                }
            });
            todo.push_back(bestCell);
        }
        CellSet seen;
        for (const auto &c : todo)
            seen.add(c.first, c.second);
        while (!todo.empty())
        {
            const auto [cx, cz] = todo.back();
            todo.pop_back();
            for (int i = -1; i <= 1; ++i)
                for (int j = -1; j <= 1; ++j)
                    if (cells.contains(cx + i, cz + j) && !seen.contains(cx + i, cz + j))
                    {
                        seen.add(cx + i, cz + j);
                        todo.emplace_back(cx + i, cz + j);
                    }
        }
        int64_t x0 = INT64_MAX, x1 = INT64_MIN, z0 = INT64_MAX, z1 = INT64_MIN;
        seen.each([&](int64_t x, int64_t z) {
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            z0 = std::min(z0, z);
            z1 = std::max(z1, z);
        });
        return {double(x0) * kCell - kMargin, double(x1 + 1) * kCell + kMargin, double(z0) * kCell - kMargin,
                double(z1 + 1) * kCell + kMargin};
    }

    double pixelsPerUnit(const Box &box)
    {
        return pyMin(kPixelsPerUnit, kMaxSide / pyMax(box.x1 - box.x0, box.z1 - box.z0));
    }

    Frame::Frame(const Box &b, double pixels) : box(b), ppu(pixels)
    {
        turned = box.x1 - box.x0 > box.z1 - box.z0;
        const double across = turned ? box.z1 - box.z0 : box.x1 - box.x0;
        const double down = turned ? box.x1 - box.x0 : box.z1 - box.z0;
        width = truncated(across * ppu) + 1;
        height = truncated(down * ppu) + 1;
        if (width > 65536 || height > 65536)
            throw PanelError("a level too large to map");
    }

    std::pair<double, double> Frame::point(double x, double z) const
    {
        if (turned)
            return {(z - box.z0) * ppu, (x - box.x0) * ppu};
        return {(x - box.x0) * ppu, (box.z1 - z) * ppu};
    }

    // ---- the picture ------------------------------------------------------------

    MapPicture rasterise(const std::vector<MapFace> &faces, const Frame &frame)
    {
        const int64_t width = frame.width, height = frame.height;
        const size_t pixels = size_t(width * height);
        MapPicture out;
        out.width = width;
        out.height = height;
        out.rgb.resize(pixels * 3);
        for (size_t p = 0; p < pixels; ++p)
            std::memcpy(out.rgb.data() + 3 * p, kBackground, 3);
        std::vector<uint16_t> tops(pixels, 0);
        Bytes kinds(pixels, kKindGround);

        std::vector<double> ys;
        for (const MapFace &t : faces)
            if (!t.water)
                ys.push_back(t.top);
        if (ys.empty())
            throw PanelError("a level with no ground to map");
        std::sort(ys.begin(), ys.end());
        const int64_t n = int64_t(ys.size());
        const int64_t high = n + floorDiv(-n, 200) - 1; // ys[-len(ys) // 200 - 1]
        if (high < 0)
            throw PanelError("a level with too few faces to map");
        const double ymin = ys[size_t(n / 200)], ymax = ys[size_t(high)];
        const double span = pyMax(1e-3, ymax - ymin);
        const double perUnit = double(kHeightSteps) / span;

        struct Face
        {
            std::array<SpanPoint, 3> points;
            bool blend;
            std::array<std::array<uint8_t, 3>, 3> paint; // [0] alone unless blend
            uint8_t kind;
        };
        std::vector<Face> painted;
        painted.reserve(faces.size());
        for (const MapFace &t : faces)
        {
            if (std::fabs(t.normal[1]) < 0.1)
                continue; // a wall: nothing of it is seen from above
            Face f{};
            const Vec3 *corners[3] = {&t.a, &t.b, &t.c};
            for (size_t k = 0; k < 3; ++k)
            {
                const auto [x, y] = frame.point((*corners[k])[0], (*corners[k])[2]);
                const double depth = (ymax - (*corners[k])[1]) * perUnit;
                const int64_t code = depth >= double(kHeightSteps) ? kHeightSteps : depth <= 0 ? 0 : truncated(depth);
                f.points[k] = {x, y, {double(kHeightFirst + code), 0, 0, 0}};
            }
            f.blend = false;
            if (t.water)
                f.paint[0] = {kWaterColour[0], kWaterColour[1], kWaterColour[2]};
            else if (t.cornerColours)
            {
                for (size_t k = 0; k < 3; ++k)
                    f.paint[k] = albedo(t.colour[k]);
                f.blend = !(f.paint[0] == f.paint[1] && f.paint[1] == f.paint[2]);
            }
            else
                f.paint[0] = albedo(t.colour[0]);
            f.kind = t.water ? kKindWater : kKindGround;
            painted.push_back(f);
        }

        // the top surface at each pixel
        std::vector<int64_t> codes;
        for (const Face &f : painted)
            faceSpans<1>(f.points, width, height,
                         [&](int64_t y, int64_t i, int64_t j, const std::array<double, 4> &c0, const std::array<double, 4> &c1) {
                             ramp(truncated(c0[0]), truncated(c1[0]), j - i, codes);
                             uint16_t *row = tops.data() + size_t(y * width);
                             for (int64_t x = i; x < j; ++x)
                                 row[x] = uint16_t(std::max<int64_t>(row[x], codes[size_t(x - i)]));
                         });
        // each pixel's colour and kind: the face that holds the top
        std::vector<int64_t> channel[3];
        for (Face &f : painted)
        {
            if (f.blend)
                for (size_t k = 0; k < 3; ++k)
                    for (size_t ch = 0; ch < 3; ++ch)
                        f.points[k].v[1 + ch] = f.paint[k][ch];
            auto paintSpan = [&](int64_t y, int64_t i, int64_t j, const std::array<double, 4> &c0, const std::array<double, 4> &c1) {
                const int64_t count = j - i;
                ramp(truncated(c0[0]), truncated(c1[0]), count, codes);
                if (f.blend)
                    for (size_t ch = 0; ch < 3; ++ch)
                        ramp(truncated(c0[ch + 1] + 0.5), truncated(c1[ch + 1] + 0.5), count, channel[ch]);
                const uint16_t *top = tops.data() + size_t(y * width + i);
                bool anyWin = false, allWin = true;
                for (int64_t x = 0; x < count; ++x)
                {
                    const bool win = codes[size_t(x)] == top[x];
                    anyWin = anyWin || win;
                    allWin = allWin && win;
                }
                if (!anyWin)
                    return;
                uint8_t *rgb = out.pixel(i, y);
                uint8_t *kind = kinds.data() + size_t(y * width + i);
                for (int64_t x = 0; x < count; ++x)
                {
                    if (!allWin && codes[size_t(x)] != top[x])
                        continue;
                    for (size_t ch = 0; ch < 3; ++ch)
                        rgb[3 * x + ch] = f.blend ? uint8_t(channel[ch][size_t(x)]) : f.paint[0][ch];
                    // (a ground face that holds part of a span leaves the kind as it was)
                    if (allWin || f.kind != kKindGround)
                        kind[x] = f.kind;
                }
            };
            if (f.blend)
                faceSpans<4>(f.points, width, height, paintSpan);
            else
                faceSpans<1>(f.points, width, height, paintSpan);
        }

        // the light: the height, and the slope from the height buffer itself
        const double slopeGain = frame.ppu / perUnit / 2.0; // codes over two pixels to units a unit: 45 degrees is 1
        out.heights.resize(pixels);
        for (int64_t y = 0; y < height; ++y)
        {
            const uint16_t *row = tops.data() + size_t(y * width);
            const uint16_t *above = y > 0 ? row - width : row;
            const uint16_t *below = y + 1 < height ? row + width : row;
            const uint8_t *kind = kinds.data() + size_t(y * width);
            uint8_t *rgb = out.pixel(0, y);
            uint8_t *code = out.heights.data() + size_t(y * width);
            for (int64_t x = 0; x < width; ++x)
            {
                // the slope over two pixels each way: a facet's edge is softened, a wall's step stays a step
                const int64_t dx = (x == 0 || x == width - 1) ? 0 : int64_t(row[x + 1]) - int64_t(row[x - 1]);
                const int64_t dy = int64_t(below[x]) - int64_t(above[x]);
                const int64_t light = lightOf(row[x], dx, dy, kind[x], slopeGain);
                for (size_t ch = 0; ch < 3; ++ch)
                    rgb[3 * x + ch] = uint8_t((rgb[3 * x + ch] * light + 127) / 255);
                if (kind[x] == kKindWater)
                    code[x] = kWaterCode;
                else
                    code[x] = row[x] == kHeightNone ? 0 : uint8_t(kHeightFirst + (row[x] - kHeightFirst) * 251 / kHeightSteps);
            }
        }
        out.codesPerUnit = 253.0 / span;
        out.highest = ymax;
        return out;
    }

    // ---- in play or not -----------------------------------------------------------

    double muteOutOfPlay(MapPicture &picture, const Frame &frame, const std::vector<Seed> &seeds,
                         const std::vector<std::array<Vec3, 3>> &barriers)
    {
        const double perUnit = picture.codesPerUnit, ymax = picture.highest;
        const int64_t step = std::max<int64_t>(1, truncated(kCellUnits * frame.ppu));
        const int64_t across = frame.width / step, down = frame.height / step;
        auto at = [&](int64_t x, int64_t y) { return size_t(y * across + x); };
        Bytes grid(size_t(across * down));
        for (int64_t y = 0; y < down; ++y)
            for (int64_t x = 0; x < across; ++x)
                grid[at(x, y)] = picture.heights[size_t(y * step * picture.width + x * step)];
        const int64_t limit = std::max<int64_t>(1, truncated(kStepUnits * perUnit));
        Bytes seen(grid.size(), 0);
        // the invisible walls stop the walk where they stand
        Bytes blocked(grid.size(), 0);
        for (const auto &tri : barriers)
            for (int e = 0; e < 3; ++e)
            {
                const Vec3 &p = tri[size_t(e)], &q = tri[size_t((e + 1) % 3)];
                const auto [ax, ay] = frame.point(p[0], p[2]);
                const auto [bx, by] = frame.point(q[0], q[2]);
                const int64_t steps = truncated(pyMax(std::fabs(bx - ax), std::fabs(by - ay)) / double(step)) + 1;
                if (steps > kMostLineSteps)
                    throw PanelError("a level with a wall too far from it to map");
                for (int64_t k = 0; k <= steps; ++k)
                {
                    const int64_t cx = floorDiv(truncated(ax + (bx - ax) * double(k) / double(steps)), step);
                    const int64_t cy = floorDiv(truncated(ay + (by - ay) * double(k) / double(steps)), step);
                    if (0 <= cx && cx < across && 0 <= cy && cy < down)
                        blocked[at(cx, cy)] = 1;
                }
            }
        std::vector<std::pair<int64_t, int64_t>> todo;
        const int64_t near = truncated(kSeedUnits * perUnit);
        for (const Seed &s : seeds)
        {
            const auto [mx, my] = frame.point(s.x, s.z);
            const int64_t cx = floorDiv(truncated(mx), step), cy = floorDiv(truncated(my), step);
            const std::optional<double> wanted =
                s.y ? std::optional<double>(2 + (ymax - *s.y) * perUnit) : std::nullopt;
            // a seed sits on what is near it (a prop hangs over its surface)
            for (int64_t j = -2; j <= 2; ++j)
                for (int64_t i = -2; i <= 2; ++i)
                {
                    const int64_t xx = cx + i, yy = cy + j;
                    if (!(0 <= xx && xx < across && 0 <= yy && yy < down))
                        continue;
                    const uint8_t g = grid[at(xx, yy)];
                    if (0 < g && g < kWaterCode && !seen[at(xx, yy)])
                        if (!wanted || std::fabs(double(g) - *wanted) <= double(near))
                        {
                            seen[at(xx, yy)] = 1;
                            todo.emplace_back(xx, yy);
                        }
                }
        }
        while (!todo.empty())
        {
            const auto [x, y] = todo.back();
            todo.pop_back();
            const int here = grid[at(x, y)];
            const std::pair<int64_t, int64_t> next[4] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
            for (const auto &[xx, yy] : next)
                if (0 <= xx && xx < across && 0 <= yy && yy < down && !seen[at(xx, yy)])
                {
                    const int there = grid[at(xx, yy)];
                    if (0 < there && there < kWaterCode && std::abs(there - here) <= limit && !blocked[at(xx, yy)])
                    {
                        seen[at(xx, yy)] = 1;
                        todo.emplace_back(xx, yy);
                    }
                }
        }

        // what the skater cannot reach steps back: the buildings the
        // streets run between, the ground behind them
        int64_t kept = 0, total = 0;
        for (int64_t y = 0; y < down; ++y)
        {
            int64_t x = 0;
            while (x < across)
            {
                const uint8_t g = grid[at(x, y)];
                if (g == 0 || g == kWaterCode || seen[at(x, y)])
                {
                    kept += seen[at(x, y)];
                    total += (g && g != kWaterCode) ? 1 : 0;
                    ++x;
                    continue;
                }
                int64_t end = x;
                while (end < across && grid[at(end, y)] && grid[at(end, y)] != kWaterCode && !seen[at(end, y)])
                    ++end;
                total += end - x;
                for (int64_t row = y * step; row < (y + 1) * step && row < picture.height; ++row)
                {
                    uint8_t *rgb = picture.pixel(0, row);
                    const int64_t from = x * step * 3, to = std::min(end * step * 3, picture.width * 3);
                    for (int64_t b = from; b < to; ++b)
                        rgb[b] = uint8_t(14 + rgb[b] * 40 / 100);
                }
                x = end;
            }
        }
        return double(kept) / double(std::max<int64_t>(1, total));
    }

    void fillGaps(MapPicture &picture, double ppu)
    {
        const int64_t most = truncated(kGapUnits * ppu);
        const int64_t width = picture.width, height = picture.height;
        struct Mark
        {
            int64_t y, x0, x1;
        };
        std::vector<Mark> marks;
        for (int64_t y = 0; y < height; ++y)
        {
            const uint8_t *h = picture.heights.data() + size_t(y * width);
            for (int64_t x = 0; x < width;)
            {
                if (h[x])
                {
                    ++x;
                    continue;
                }
                int64_t end = x;
                while (end < width && !h[end])
                    ++end;
                if (0 < x && end < width && end - x <= most)
                    marks.push_back({y, x, end});
                x = end;
            }
        }
        for (int64_t x = 0; x < width; ++x)
            for (int64_t y = 0; y < height;)
            {
                if (picture.heights[size_t(y * width + x)])
                {
                    ++y;
                    continue;
                }
                int64_t end = y;
                while (end < height && !picture.heights[size_t(end * width + x)])
                    ++end;
                if (0 < y && end < height && end - y <= most)
                    for (int64_t row = y; row < end; ++row)
                        marks.push_back({row, x, x + 1});
                y = end;
            }
        for (const Mark &m : marks)
            for (int64_t x = m.x0; x < m.x1; ++x)
            {
                std::memcpy(picture.pixel(x, m.y), kWallColour, 3);
                picture.heights[size_t(m.y * width + x)] = kWallHeight;
            }
    }

    void outline(MapPicture &picture)
    {
        const int64_t width = picture.width;
        for (int64_t y = 0; y + 1 < picture.height; ++y)
        {
            const uint8_t *h0 = picture.heights.data() + size_t(y * width);
            const uint8_t *h1 = h0 + width;
            uint8_t *rgb = picture.pixel(0, y);
            for (int64_t x = 0; x + 1 < width; ++x)
            {
                int d = std::abs(int(h0[x]) - int(h0[x + 1]));
                const int e = std::abs(int(h0[x]) - int(h1[x]));
                if (e > d)
                    d = e;
                if (d > 8)
                {
                    const int k = d > 24 ? 150 : 200;
                    for (int ch = 0; ch < 3; ++ch)
                        rgb[3 * x + ch] = uint8_t(rgb[3 * x + ch] * k >> 8);
                }
            }
        }
    }

    void drawRails(MapPicture &picture, const Frame &frame, const std::vector<std::array<Vec3, 2>> &rails)
    {
        for (const auto &rail : rails)
        {
            const auto [ax, ay] = frame.point(rail[0][0], rail[0][2]);
            const auto [bx, by] = frame.point(rail[1][0], rail[1][2]);
            const int64_t steps = truncated(pyMax(std::fabs(bx - ax), std::fabs(by - ay))) + 1;
            if (steps > kMostLineSteps)
                throw PanelError("a level with a rail too far from it to map");
            for (int64_t i = 0; i <= steps; ++i)
            {
                const int64_t x = truncated(ax + (bx - ax) * double(i) / double(steps));
                const int64_t y = truncated(ay + (by - ay) * double(i) / double(steps));
                if (0 <= x && x < picture.width && 0 <= y && y < picture.height)
                {
                    uint8_t *p = picture.pixel(x, y);
                    p[0] = uint8_t((p[0] + 90) >> 1);
                    p[1] = uint8_t((p[1] + 200) >> 1);
                    p[2] = uint8_t((p[2] + 240) >> 1);
                }
            }
        }
    }

    // ---- the file -------------------------------------------------------------------

    Bytes levelFile(std::string_view title, const Frame &frame, const MapPicture &picture, const Bytes &goals)
    {
        constexpr uint32_t kFileVersion = 3; // panel_file.h kFileVersion
        Bytes file;
        putText(file, "PMAP");
        putU32(file, kFileVersion);
        Bytes name;
        putText(name, title);
        name.push_back(0);
        putBytes(file, chunk("NAME", name));
        // MAPI: the transform, then each row as runs of RGB565 {u16 run, u16 colour}
        Bytes map;
        putU32(map, frame.width);
        putU32(map, frame.height);
        putF32(map, frame.worldLeft());
        putF32(map, frame.worldTop());
        putF32(map, frame.ppu);
        putU32(map, frame.turned ? 1 : 0);
        std::vector<uint16_t> colours(size_t(picture.width));
        for (int64_t y = 0; y < picture.height; ++y)
        {
            const uint8_t *row = picture.rgb.data() + size_t(y * picture.width * 3);
            for (int64_t x = 0; x < picture.width; ++x)
                colours[size_t(x)] = uint16_t(((row[3 * x] & 0xF8) << 8) | ((row[3 * x + 1] & 0xFC) << 3) | (row[3 * x + 2] >> 3));
            int64_t x = 0;
            while (x < picture.width)
            {
                const uint16_t c = colours[size_t(x)];
                int64_t run = x + 1;
                while (run < picture.width && colours[size_t(run)] == c && run - x < 0xFFFF)
                    ++run;
                putU16(map, run - x);
                putU16(map, c);
                x = run;
            }
        }
        putBytes(file, chunk("MAPI", map));
        putBytes(file, goals);
        return file;
    }
}
