// DM2's park editor themes the Xbox disc alone has, Desert and Highway, and
// the attract demo's replays, as tools/dm2_xbox.py convert_themes and
// demo_replays make them (collection/extras/dm2_xbox.h).
//
// Desert and Highway (editor/leDesert, leHighWa) are alpha exports already
// in the PS2's formats, which every release's SCENE.TXT declares and no
// THEMES.TXT lists. What a theme has that they lack is the stock theme
// Beach's (LEBEACH): the 32 x 32 outdoor theme whose frame is theirs. Their
// ring ground lies at y 0, every shipped theme's at level 10 or 15 (+y is
// down), and a MAP floor cell needs a level over 0: raised to Beach's 15, so
// Beach's MAP fits them cell for cell, with Beach's light and Globals points.
// Each gets a theme select picture drawn from its files (themePicture).
#include "dm2_xbox_parts.h"
#include "script_text.h"

#include <cmath>
#include <limits>
#include <optional>

namespace ps2x::app::extras::dm2
{
    namespace
    {
        namespace text = script_text;

        struct ThemeInfo
        {
            const char *folder;     // the output folder ("LEDESERT")
            const char *xboxFolder; // on the Xbox disc ("leDesert")
            const char *name;       // the theme select's
        };
        const ThemeInfo kThemes[] = {{"LEDESERT", "leDesert", "Desert"}, {"LEHIGHWA", "leHighWa", "Highway"}};
        constexpr const char kDonor[] = "LEBEACH";
        constexpr const char kDonorFile[] = "LEBeach"; // its FILE in THEMES.TXT
        constexpr double kRaise = 15.0;

        // The theme select's picture (UILIB ParkMenu.zif "Themes.<value>", 256
        // x 256, 8 bits, as the stock nine): the theme drawn from above one
        // corner of its floor, its floor of towers at the MAP's level, its
        // textures lit by its AMBIANT and brightness 5, its clear colour behind.
        constexpr int kPictureSide = 256;
        constexpr int kPictureScale = 2; // drawn at twice the side, averaged down
        constexpr double kPictureEye[3] = {-62.0, -52.0, -62.0};
        constexpr double kPictureTarget[3] = {8.0, -15.0, 8.0};
        constexpr double kPictureFov = 62.0;
        constexpr int kBrightness = 5; // edit_lights.cpp kDefaultBrightness

        bool isLightPointClass(uint32_t id)
        {
            return id == nameHash("EditLightCone") || id == nameHash("EditLightOmni") || id == nameHash("Globals");
        }

        // Every instance (TGIH: the position at +0x18) moved dy along y.
        Bytes raiseIns(const Bytes &data, double dy, long &raised)
        {
            Chunks tree = parse(data);
            walk(tree, [&](Chunk &c)
            {
                if (c.tag == "TGIH")
                {
                    putF32(c.data, 0x1C, f32At(c.data, 0x1C) + dy);
                    ++raised;
                }
            });
            return serialize(tree);
        }

        // The theme's scene points moved dy along y, then the donor's light and
        // Globals points as they are.
        Bytes themeDat(const Bytes &data, const Bytes &donor, double dy, long &raised, long &added)
        {
            Chunks tree = parse(data);
            if (tree.empty())
                throw Dm2Error("an empty theme DAT");
            std::vector<Chunk> &kids = tree[0].kids;
            std::optional<size_t> last;
            for (size_t i = 0; i < kids.size(); ++i)
                if (kids[i].tag == "SDPT")
                {
                    putF32(kids[i].data, 0x14, f32At(kids[i].data, 0x14) + dy);
                    ++raised;
                    if (isLightPointClass(u32At(kids[i].data, 4)))
                        throw Dm2Error("the theme has light or Globals points of its own");
                    last = i;
                }
            const Chunks donorTree = parse(donor);
            std::vector<Chunk> points;
            if (!donorTree.empty())
                for (const Chunk &c : donorTree[0].kids)
                    if (c.tag == "SDPT" && isLightPointClass(u32At(c.data, 4)))
                        points.push_back(c);
            added = long(points.size());
            kids.insert(kids.begin() + long(last ? *last + 1 : 0), points.begin(), points.end());
            return serialize(tree);
        }

        // ---- THEMES.TXT ------------------------------------------------------------

        size_t skipSpace(const std::string &s, size_t at)
        {
            while (at < s.size() && text::isSpace(static_cast<unsigned char>(s[at])))
                ++at;
            return at;
        }

        // THEMES.TXT's THEME blocks (the regex THEME\s*\{.*?\n\s*\}): [(FILE, block)].
        std::vector<std::pair<std::string, std::string>> themeBlocks(const std::string &file)
        {
            std::vector<std::pair<std::string, std::string>> out;
            size_t from = 0;
            while (true)
            {
                const size_t begin = file.find("THEME", from);
                if (begin == std::string::npos)
                    return out;
                const size_t brace = skipSpace(file, begin + 5);
                if (brace >= file.size() || file[brace] != '{')
                {
                    from = begin + 1;
                    continue;
                }
                // The first newline after the brace followed by white space and a }.
                size_t end = std::string::npos;
                for (size_t k = brace + 1; k < file.size(); ++k)
                    if (file[k] == '\n')
                    {
                        const size_t close = skipSpace(file, k + 1);
                        if (close < file.size() && file[close] == '}')
                        {
                            end = close + 1;
                            break;
                        }
                    }
                if (end == std::string::npos)
                {
                    from = begin + 1;
                    continue;
                }
                const std::string block = file.substr(begin, end - begin);
                std::string name;
                for (size_t at = block.find("FILE"); at != std::string::npos; at = block.find("FILE", at + 1))
                {
                    // FILE\s+"([^"]*)"
                    const size_t quote = skipSpace(block, at + 4);
                    if (quote == at + 4 || quote >= block.size() || block[quote] != '"')
                        continue;
                    const size_t close = block.find('"', quote + 1);
                    if (close == std::string::npos)
                        continue;
                    name = block.substr(quote + 1, close - quote - 1);
                    break;
                }
                out.push_back({name, block});
                from = end;
            }
        }

        // The first KEY\s+"[^"]*" in `block` made `KEY\t"value"` (re.sub, count=1).
        std::string replaceQuoted(const std::string &block, const char *key, const std::string &value)
        {
            const size_t keyLength = std::strlen(key);
            for (size_t at = block.find(key); at != std::string::npos; at = block.find(key, at + 1))
            {
                const size_t quote = skipSpace(block, at + keyLength);
                if (quote == at + keyLength || quote >= block.size() || block[quote] != '"')
                    continue;
                const size_t close = block.find('"', quote + 1);
                if (close == std::string::npos)
                    continue;
                return block.substr(0, at) + key + "\t\"" + value + "\"" + block.substr(close + 1);
            }
            return block;
        }

        // The donor's THEME block with the theme's NAME and FILE.
        std::string themeBlock(const std::string &donor, const std::string &name, const std::string &folder)
        {
            return replaceQuoted(replaceQuoted(donor, "NAME", name), "FILE", folder);
        }

        // The digits after the first KEY\s+ in `block` (each further number with
        // its own \s+ before it), or none.
        std::optional<std::vector<long>> numbersAfter(const std::string &block, const char *key, int count)
        {
            const size_t keyLength = std::strlen(key);
            for (size_t at = block.find(key); at != std::string::npos; at = block.find(key, at + 1))
            {
                std::vector<long> numbers;
                size_t p = at + keyLength;
                for (int i = 0; i < count; ++i)
                {
                    const size_t digits = skipSpace(block, p);
                    size_t end = digits;
                    while (end < block.size() && block[end] >= '0' && block[end] <= '9')
                        ++end;
                    if (digits == p || end == digits)
                        break;
                    numbers.push_back(std::stol(block.substr(digits, end - digits)));
                    p = end;
                }
                if (int(numbers.size()) == count)
                    return numbers;
            }
            return std::nullopt;
        }

        // The MAP's cells: the numbers of the run of '-', digits and white
        // space after the first MAP\s+ (the regex MAP\s+([-\d\s]+)).
        std::vector<long> mapCells(const std::string &block, size_t count)
        {
            for (size_t at = block.find("MAP"); at != std::string::npos; at = block.find("MAP", at + 1))
            {
                const size_t after = at + 3;
                if (after >= block.size() || !text::isSpace(static_cast<unsigned char>(block[after])))
                    continue;
                size_t end = after;
                while (end < block.size() && (block[end] == '-' || (block[end] >= '0' && block[end] <= '9') ||
                                              text::isSpace(static_cast<unsigned char>(block[end]))))
                    ++end;
                if (end - after < 2) // \s+ takes one, the group at least one more
                    continue;
                std::vector<long> cells;
                for (const std::string &word : text::splitWords(block.substr(after, end - after)))
                {
                    if (cells.size() == count)
                        break;
                    cells.push_back(std::stol(word));
                }
                return cells;
            }
            throw Dm2Error("a THEME block with no MAP");
        }

        // ---- the theme's picture ------------------------------------------------------

        struct Texture
        {
            int width, height;
            std::vector<std::vector<Rgba>> rows;
        };

        bool isColourKey(const Rgba &c) { return c[0] >= 248 && c[1] <= 8 && c[2] >= 248; }

        // A TXR's TBMPs (the first level) by texture id, the colour key clear.
        std::map<uint32_t, Texture> themeTextures(const Bytes &txr)
        {
            std::map<uint32_t, Texture> out;
            walk(parse(txr), [&](const Chunk &c)
            {
                if (c.tag != "TXRC")
                    return;
                const Chunk *head = c.find("TXRH"), *picture = c.find("TBMP");
                if (!head || !picture)
                    return;
                Texture t;
                const Bytes &d = picture->data;
                const int bitsPerPixel = u16At(d, 28);
                if (bitsPerPixel == 24 || bitsPerPixel == 32)
                {
                    const int32_t w = s32At(d, 18), h = s32At(d, 22);
                    const int bpp = bitsPerPixel / 8;
                    const size_t at = u32At(d, 10), stride = size_t((w * bpp + 3) / 4 * 4);
                    const int rows = std::abs(h);
                    for (int i = 0; i < rows; ++i)
                    {
                        const int r = h > 0 ? rows - 1 - i : i;
                        std::vector<Rgba> line;
                        for (int x = 0; x < w; ++x)
                        {
                            const size_t p = at + size_t(r) * stride + size_t(x) * size_t(bpp);
                            line.push_back({d.at(p + 2), d.at(p + 1), d.at(p), 255});
                        }
                        t.rows.push_back(std::move(line));
                    }
                    t.width = w;
                    t.height = rows;
                }
                else
                {
                    Picture p = decodeBmp(d, true);
                    t.width = p.width;
                    t.height = p.height;
                    t.rows = std::move(p.rows);
                }
                for (auto &row : t.rows)
                    for (Rgba &p : row)
                        if (isColourKey(p))
                            p = {0, 0, 0, 0};
                out[u32At(head->data, 0)] = std::move(t);
            });
            return out;
        }

        struct Face
        {
            int i0, i1, i2;
            uint32_t texture;
        };
        struct Part
        {
            std::vector<Vec3> positions;
            std::vector<std::array<double, 2>> uvs;
            std::vector<Face> faces;
        };
        struct Object
        {
            std::string name;
            std::vector<Part> parts; // its first level of detail
        };

        // The objects in the GEO's order (an id seen again keeps its first
        // place, with the later object: a Python dict's).
        std::vector<std::pair<uint32_t, Object>> themeObjects(const Bytes &geo)
        {
            std::vector<std::pair<uint32_t, Object>> out;
            const Chunks tree = parse(geo);
            if (tree.empty())
                return out;
            for (const Chunk &object : tree[0].kids)
            {
                if (object.tag != "TGEO")
                    continue;
                const Chunk *head = object.find("TGHD");
                if (!head)
                    throw Dm2Error("an object with no TGHD");
                std::vector<std::optional<uint32_t>> materials;
                for (const Chunk &table : object.kids)
                    if (table.tag == "TMTB")
                        for (const Chunk &m : table.kids)
                            if (m.tag == "MATI")
                                materials.push_back(u32At(m.data, 0x28) ? std::optional<uint32_t>(u32At(m.data, 0x2C + 0x10))
                                                                        : std::nullopt);
                Object o;
                o.name = cText(head->data, 0x1C, 0x3C);
                for (const Chunk &part : object.kids)
                {
                    if (part.tag != "TGPT")
                        continue;
                    std::map<std::string, const Bytes *> s;
                    for (const Chunk &k : part.kids)
                        s[k.tag] = &k.data;
                    if (!s.count("TGPH") || !s.count("TGVP") || !s.count("TGFP"))
                        throw Dm2Error("a theme part with no TGPH, TGVP or TGFP");
                    const uint16_t vertices = u16At(*s["TGPH"], 4), faceCount = u16At(*s["TGPH"], 6);
                    if (!s.count("TGVU") || s["TGVU"]->size() < 8 * size_t(vertices))
                        continue;
                    Part p;
                    for (size_t i = 0; i < vertices; ++i)
                    {
                        p.positions.push_back({f32At(*s["TGVP"], 12 * i), f32At(*s["TGVP"], 12 * i + 4),
                                               f32At(*s["TGVP"], 12 * i + 8)});
                        p.uvs.push_back({f32At(*s["TGVU"], 8 * i), f32At(*s["TGVU"], 8 * i + 4)});
                    }
                    for (size_t i = 0; i < faceCount; ++i)
                    {
                        const uint16_t f0 = u16At(*s["TGFP"], 8 * i), f1 = u16At(*s["TGFP"], 8 * i + 2),
                                       f2 = u16At(*s["TGFP"], 8 * i + 4), f3 = u16At(*s["TGFP"], 8 * i + 6);
                        const size_t m = f0 & 0xFF;
                        if (std::max({f1, f2, f3}) < vertices && m < materials.size() && materials[m])
                            p.faces.push_back({f1, f2, f3, *materials[m]});
                    }
                    o.parts.push_back(std::move(p));
                    break; // the first level of detail
                }
                const uint32_t id = u32At(head->data, 0x18);
                auto same = std::find_if(out.begin(), out.end(), [id](const auto &e) { return e.first == id; });
                if (same != out.end())
                    same->second = std::move(o);
                else
                    out.push_back({id, std::move(o)});
            }
            return out;
        }

        // Python's int(): toward zero (a value past any picture's clipped first).
        long long truncated(double v)
        {
            return (long long)std::max(-1e15, std::min(1e15, v));
        }

        // Python's a % b for b > 0: never negative.
        long long floorMod(long long a, long long b)
        {
            const long long m = a % b;
            return m < 0 ? m + b : m;
        }

        using Rgb = std::array<int, 3>;

        // The theme select's picture of a theme: rows of (r, g, b). Doubles in
        // the Python's order, so every host draws the same picture.
        std::vector<std::vector<Rgb>> themePicture(const Bytes &geo, const Bytes &ins, const Bytes &txr, const Bytes &dat,
                                                   const std::string &block)
        {
            const std::map<uint32_t, Texture> textures = themeTextures(txr);
            const std::vector<std::pair<uint32_t, Object>> objects = themeObjects(geo);
            const std::optional<std::vector<long>> ambient = numbersAfter(block, "AMBIANT", 3);
            const std::optional<std::vector<long>> widthOf = numbersAfter(block, "WIDTH", 1),
                                                   heightOf = numbersAfter(block, "HEIGHT", 1);
            if (!ambient || !widthOf || !heightOf)
                throw Dm2Error("a THEME block with no AMBIANT, WIDTH or HEIGHT");
            double light[3];
            for (int i = 0; i < 3; ++i)
                light[i] = std::min(255L, (*ambient)[size_t(i)] + kBrightness) / 128.0;
            const long width = (*widthOf)[0], height = (*heightOf)[0];
            const std::vector<long> cells = mapCells(block, size_t(width * height));
            Rgb clear = {0, 0, 0};
            {
                const Chunks tree = parse(dat);
                if (!tree.empty())
                    for (const Chunk &c : tree[0].kids)
                        if (c.tag == "SDPT" && u32At(c.data, 4) == nameHash("Globals"))
                            clear = {c.data.at(0x30), c.data.at(0x31), c.data.at(0x32)}; // the clear colour
            }
            // What is drawn: every instance's object, then a tower on every
            // tower cell of the floor (MAP > 0).
            using Place = std::pair<Quat, Vec3>;
            std::vector<std::pair<const std::vector<Part> *, std::vector<Place>>> placed;
            std::vector<const Bytes *> instances;
            const Chunks insTree = parse(ins);
            walk(insTree, [&](const Chunk &c)
            {
                if (c.tag == "TGIH")
                    instances.push_back(&c.data);
            });
            std::map<uint32_t, const Bytes *> byId;
            for (const Bytes *d : instances)
                byId[u32At(*d, 0x48)] = d;
            auto objectOf = [&](uint32_t id) -> const Object *
            {
                for (const auto &[oid, o] : objects)
                    if (oid == id)
                        return &o;
                return nullptr;
            };
            for (const Bytes *d : instances)
            {
                std::vector<Place> chain;
                const Bytes *at = d;
                for (int i = 0; i < 16; ++i)
                {
                    chain.push_back({{f32At(*at, 8), f32At(*at, 12), f32At(*at, 16), f32At(*at, 20)},
                                     {f32At(*at, 0x18), f32At(*at, 0x1C), f32At(*at, 0x20)}});
                    const auto parent = byId.find(u32At(*at, 0x44));
                    if (parent == byId.end())
                        break;
                    at = parent->second;
                }
                if (const Object *o = objectOf(u32At(*d, 0x40)))
                    placed.push_back({&o->parts, chain});
            }
            const Object *tower = nullptr;
            for (const auto &[id, o] : objects)
                if (!tower && lowerAscii(o.name) == "editobj_tower")
                    tower = &o;
            if (tower)
                for (long row = 0; row < height; row += 2)
                    for (long col = 1; col < width; col += 2)
                    {
                        const size_t cell = size_t(row * width + col);
                        const long level = cell < cells.size() ? cells[cell] : 0;
                        if (level > 0)
                        {
                            const Vec3 pos = {col * 5.0 - (width - 1) * 2.5, -double(level), row * 5.0 - (height - 1) * 2.5};
                            placed.push_back({&tower->parts, {{{0.0, 0.0, 0.0, 1.0}, pos}}});
                        }
                    }
            // The camera: +y is down; the picture's x right, y down.
            const int side = kPictureSide * kPictureScale;
            const double ex = kPictureEye[0], ey = kPictureEye[1], ez = kPictureEye[2];
            double fx = kPictureTarget[0] - ex, fy = kPictureTarget[1] - ey, fz = kPictureTarget[2] - ez;
            double n = std::sqrt(fx * fx + fy * fy + fz * fz);
            fx = fx / n;
            fy = fy / n;
            fz = fz / n;
            double rx = -fz, ry = 0.0, rz = fx; // right, level with the ground
            n = std::sqrt(rx * rx + rz * rz);
            rx = rx / n;
            rz = rz / n;
            const double ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx; // down in the picture
            const double pi = 3.141592653589793;                                                 // Python's math.pi
            const double focal = double(side) / 2 / std::tan(kPictureFov * (pi / 180.0) / 2);
            const double nearest = 0.5;
            std::vector<double> depth(size_t(side) * size_t(side), std::numeric_limits<double>::infinity());
            std::vector<Rgb> colour(size_t(side) * size_t(side), clear);
            struct Projected
            {
                bool visible;
                double x, y, z;
            };
            auto project = [&](const Vec3 &p) -> Projected
            {
                const double dx = p[0] - ex, dy = p[1] - ey, dz = p[2] - ez;
                const double z = dx * fx + dy * fy + dz * fz;
                if (!(z > nearest))
                    return {false, 0, 0, z};
                return {true, double(side) / 2 + focal * (dx * rx + dy * ry + dz * rz) / z,
                        double(side) / 2 + focal * (dx * ux + dy * uy + dz * uz) / z, z};
            };
            for (const auto &[parts, chain] : placed)
                for (const Part &part : *parts)
                {
                    std::vector<Projected> world;
                    for (Vec3 v : part.positions)
                    {
                        for (const auto &[q, t] : chain)
                        {
                            const Vec3 r = quatRotate(q, v);
                            v = {r[0] + t[0], r[1] + t[1], r[2] + t[2]};
                        }
                        world.push_back(project(v));
                    }
                    for (const Face &face : part.faces)
                    {
                        const Projected &a = world[size_t(face.i0)], &b = world[size_t(face.i1)], &c = world[size_t(face.i2)];
                        if (!a.visible || !b.visible || !c.visible)
                            continue;
                        const auto found = textures.find(face.texture);
                        if (found == textures.end())
                            continue;
                        const Texture &texture = found->second;
                        const double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
                        if (std::fabs(area) < 1e-9)
                            continue;
                        const long long x0 = std::max(0LL, truncated(std::min({a.x, b.x, c.x})));
                        const long long x1 = std::min<long long>(side - 1, truncated(std::max({a.x, b.x, c.x})) + 1);
                        const long long y0 = std::max(0LL, truncated(std::min({a.y, b.y, c.y})));
                        const long long y1 = std::min<long long>(side - 1, truncated(std::max({a.y, b.y, c.y})) + 1);
                        if (x0 > x1 || y0 > y1)
                            continue;
                        // Perspective-correct texture coordinates (u/z, v/z, 1/z across the face).
                        const double ia = 1.0 / a.z, ib = 1.0 / b.z, ic = 1.0 / c.z;
                        const auto &uv0 = part.uvs[size_t(face.i0)], &uv1 = part.uvs[size_t(face.i1)],
                                   &uv2 = part.uvs[size_t(face.i2)];
                        const double ua = uv0[0] * ia, va = uv0[1] * ia;
                        const double ub = uv1[0] * ib, vb = uv1[1] * ib;
                        const double uc = uv2[0] * ic, vc = uv2[1] * ic;
                        for (long long y = y0; y <= y1; ++y)
                        {
                            const double py = double(y) + 0.5;
                            for (long long x = x0; x <= x1; ++x)
                            {
                                const double px = double(x) + 0.5;
                                const double w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                                const double w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                                const double w2 = 1.0 - w0 - w1;
                                if (w0 < 0 || w1 < 0 || w2 < 0)
                                    continue;
                                const double iz = w0 * ia + w1 * ib + w2 * ic;
                                const double z = 1.0 / iz;
                                const size_t k = size_t(y) * size_t(side) + size_t(x);
                                if (z >= depth[k])
                                    continue;
                                const double u = (w0 * ua + w1 * ub + w2 * uc) * z;
                                const double v = (w0 * va + w1 * vb + w2 * vc) * z;
                                const Rgba &p = texture.rows[size_t(floorMod(truncated(v * texture.height), texture.height))]
                                                            [size_t(floorMod(truncated(u * texture.width), texture.width))];
                                if (p[3] == 0)
                                    continue;
                                depth[k] = z;
                                colour[k] = {int(std::min(255LL, truncated(p[0] * light[0]))),
                                             int(std::min(255LL, truncated(p[1] * light[1]))),
                                             int(std::min(255LL, truncated(p[2] * light[2])))};
                            }
                        }
                    }
                }
            std::vector<std::vector<Rgb>> rows;
            for (int y = 0; y < kPictureSide; ++y)
            {
                std::vector<Rgb> row;
                for (int x = 0; x < kPictureSide; ++x)
                {
                    Rgb sum = {0, 0, 0};
                    for (int j = 0; j < kPictureScale; ++j)
                        for (int i = 0; i < kPictureScale; ++i)
                        {
                            const Rgb &p = colour[size_t(y * kPictureScale + j) * size_t(side) + size_t(x * kPictureScale + i)];
                            for (int ch = 0; ch < 3; ++ch)
                                sum[size_t(ch)] += p[size_t(ch)];
                        }
                    for (int &ch : sum)
                        ch /= kPictureScale * kPictureScale;
                    row.push_back(sum);
                }
                rows.push_back(std::move(row));
            }
            return rows;
        }

        // A stock theme's picture asset in UILIB.ZAL's ParkMenu.zif (its
        // assets named "0" on): the form the made ones take.
        Chunk stockThemePicture(const SourceFiles &ps2)
        {
            Bytes uilib;
            if (!ps2.readAny("UILIB.ZAL", uilib))
                throw Dm2Error("the PS2 files have no UILIB.ZAL");
            for (const Bytes &entry : zalEntries(uilib))
            {
                if (entry.size() < 4 || std::memcmp(entry.data(), "ULIB", 4) != 0)
                    continue;
                Chunks library;
                try
                {
                    library = parseUi(entry);
                }
                catch (const Dm2Error &)
                {
                    continue;
                }
                for (const Chunk &c : library)
                    if (c.tag == "ULIB")
                        for (const Chunk &asset : c.kids)
                        {
                            if (asset.tag != "UAST" || asset.kids.empty() || asset.kids[0].tag != "HAST")
                                continue;
                            const Bytes &hast = asset.kids[0].data;
                            const Bytes path = sliceOf(hast, 0, 0x80);
                            const std::string pathText(path.begin(), path.end());
                            if (cText(hast, 0x80, 0xA0) == "0" && pathText.find("Themes") != std::string::npos)
                                return asset;
                        }
            }
            throw Dm2Error("UILIB.ZAL has no theme pictures (ParkMenu.zif)");
        }

        // The stock picture's asset with this picture, named `name` (its HAST
        // name and its texture's, the hash with it: what "Themes.<value>"
        // finds), the palette by median cut.
        Chunk themePictureAsset(const Chunk &stock, const std::vector<std::vector<Rgb>> &rows, const std::string &name)
        {
            std::map<Rgba, long long> counts;
            for (const auto &row : rows)
                for (const Rgb &c : row)
                    ++counts[{c[0] & 0xFC, c[1] & 0xFC, c[2] & 0xFC, 255}];
            const Palette palette = medianCut(counts, 256);
            std::vector<std::vector<uint8_t>> indices;
            for (const auto &row : rows)
            {
                std::vector<uint8_t> line;
                for (const Rgb &c : row)
                    line.push_back(uint8_t(palette.index.at({c[0] & 0xFC, c[1] & 0xFC, c[2] & 0xFC, 255})));
                indices.push_back(std::move(line));
            }
            std::vector<Rgba> colours;
            for (const Rgba &c : palette.colours)
                colours.push_back({c[0], c[1], c[2], 0});
            const Bytes bmp = bmp8(indices, colours);
            Chunk asset = stock;
            const Bytes hast = asset.kids[0].data;
            Bytes newHast;
            appendPadded(newHast, "dm2_xbox.py\\" + name + ".bmp", 0x80);
            appendPadded(newHast, name, 32);
            appendBytes(newHast, sliceOf(hast, 0xA0));
            asset.kids[0].data = std::move(newHast);
            walk(asset.kids, [&](Chunk &c)
            {
                if (c.tag == "TXRH")
                {
                    Bytes head;
                    appendU32(head, nameHash(name));
                    appendU16(head, kPictureSide);
                    appendU16(head, kPictureSide);
                    appendBytes(head, sliceOf(c.data, 8, 0x14));
                    appendPadded(head, name, 32);
                    appendBytes(head, sliceOf(c.data, 0x34));
                    c.data = std::move(head);
                }
                else if (c.tag == "TBMP")
                    c.data = bmp;
            });
            return asset;
        }

        // Latin-1 text as the UTF-8 Python's text files write.
        std::string utf8OfLatin1(const std::string &s)
        {
            std::string out;
            for (unsigned char c : s)
            {
                if (c < 0x80)
                    out += char(c);
                else
                {
                    out += char(0xC0 | (c >> 6));
                    out += char(0x80 | (c & 0x3F));
                }
            }
            return out;
        }

        // ---- the attract demo's replays ------------------------------------------

        // REPLAY.ZAL (an uncompressed zfile library: a header, counts at +0x10,
        // then a directory of groups {u32 id, u32 offset from byte 0x10, u32
        // length}): groups 0-15 are the attract demo's, two a level.
        constexpr int kDemoGroups = 16;
        constexpr uint32_t kZfileMagic = 0x2A81511C;

        struct Group
        {
            uint32_t id, offset, length;
        };

        std::vector<Group> libraryGroups(const Bytes &data)
        {
            if (u32At(data, 0) != kZfileMagic)
                throw Dm2Error("REPLAY.ZAL is not a zfile library");
            const uint16_t assets = u16At(data, 0x10), groups = u16At(data, 0x12);
            if ((assets & 0x1FFF) != 1 || (assets >> 15) || (groups >> 14))
                throw Dm2Error("REPLAY.ZAL is not an uncompressed one-asset-a-group library");
            std::vector<Group> out;
            for (size_t g = 0; g < (groups & 0x3FFFu); ++g)
                out.push_back({u32At(data, 0x14 + 12 * g), u32At(data, 0x18 + 12 * g), u32At(data, 0x1C + 12 * g) & 0xFFFFFF});
            return out;
        }

        // A demo replay of that level (its saved part: the controller layout,
        // the modes and the level, the rider record's name "Rider00").
        bool demoReplayOk(const Bytes &data, const Group &g, int level)
        {
            const Bytes body = sliceOf(data, 0x10 + size_t(g.offset), 0x10 + size_t(g.offset) + g.length);
            return body.size() > 0x20 && body[1] == 0 && body[3] == level && std::memcmp(body.data() + 0x0C, "Rider", 5) == 0;
        }

        std::string listText(const std::vector<int> &values)
        {
            std::string out = "[";
            for (size_t i = 0; i < values.size(); ++i)
                out += (i ? ", " : "") + std::to_string(values[i]);
            return out + "]";
        }
    }

    void convertThemes(const SourceFiles &xbox, const SourceFiles &ps2, Made &made, std::vector<std::string> &log)
    {
        const Bytes themesData = need(ps2, "EDITOR/THEMES.TXT", "the PS2 files");
        const Bytes donorDat = need(ps2, std::string("EDITOR/") + kDonor + "/" + kDonor + ".DAT", "the PS2 files");
        const std::string themesText(themesData.begin(), themesData.end());
        const std::vector<std::pair<std::string, std::string>> stockBlocks = themeBlocks(themesText);
        std::optional<std::string> donor;
        for (const auto &[file, block] : stockBlocks)
            if (file == kDonorFile)
                donor = block; // (the last of that FILE, as a Python dict keeps)
        if (!donor)
            throw Dm2Error(std::string("THEMES.TXT has no ") + kDonorFile + " block");
        const Chunk stock = stockThemePicture(ps2);
        std::vector<std::string> blocks, lines;
        Chunks assets;
        for (const ThemeInfo &theme : kThemes)
        {
            std::map<std::string, Bytes> files;
            for (const char *ext : {"ZIT", "GEO", "INS", "TXR", "DAT"})
                files[ext] = need(xbox, std::string("editor/") + theme.xboxFolder + "/" + theme.xboxFolder + "." + ext,
                                  "the Xbox files");
            long raisedInstances = 0, raisedPoints = 0, added = 0;
            const Bytes zit = convertZit(files["ZIT"], theme.folder);
            const Bytes ins = raiseIns(files["INS"], -kRaise, raisedInstances);
            const Bytes dat = themeDat(files["DAT"], donorDat, -kRaise, raisedPoints, added);
            const std::string base = std::string("themes/") + theme.folder + "/" + theme.folder + ".";
            made.push_back({base + "ZIT", zit});
            made.push_back({base + "GEO", files["GEO"]}); // the PS2's chunk set, TBMP textures: as they are
            made.push_back({base + "INS", ins});
            made.push_back({base + "TXR", files["TXR"]});
            made.push_back({base + "DAT", dat});
            blocks.push_back(themeBlock(*donor, theme.name, theme.folder));
            const auto rows = themePicture(files["GEO"], ins, files["TXR"], dat, blocks.back());
            const std::string value = std::to_string(stockBlocks.size() + assets.size());
            assets.push_back(themePictureAsset(stock, rows, value));
            log.push_back(std::string("[dm2xbox] theme ") + theme.folder + ": picture Themes." + value + " drawn");
            lines.push_back(std::string(theme.folder) + " " + kDonorFile + " " + theme.name);
            log.push_back(std::string("[dm2xbox] theme ") + theme.folder + " (" + theme.name + "): " +
                          std::to_string(raisedInstances) + " instances and " + std::to_string(raisedPoints) +
                          " points raised 15 m, " + std::to_string(added) + " light points from " + kDonor);
        }
        // (written as Python's text files: UTF-8, THEMES.TXT's every \n as \r\n)
        std::string themes;
        for (size_t i = 0; i < blocks.size(); ++i)
            themes += (i ? "\n\n" : "") + blocks[i];
        themes = text::replaceAll(utf8OfLatin1(themes + "\n"), "\n", "\r\n");
        std::string names;
        for (size_t i = 0; i < lines.size(); ++i)
            names += (i ? "\n" : "") + lines[i];
        names = utf8OfLatin1(names + "\n");
        made.push_back({"themes/THEMES.TXT", Bytes(themes.begin(), themes.end())});
        made.push_back({"themes/names.txt", Bytes(names.begin(), names.end())});
        if (!assets.empty())
            made.push_back({"themes/PICTURES.ZIF", serialize({Chunk::node("ULIB", assets)})});
    }

    void demoReplays(const SourceFiles &xbox, const SourceFiles &ps2, Made &made, std::vector<std::string> &log)
    {
        const Bytes ps2Data = need(ps2, "REPLAY.ZAL", "the PS2 files"), xboxData = need(xbox, "replay.zal", "the Xbox files");
        const std::vector<Group> ours = libraryGroups(ps2Data), theirs = libraryGroups(xboxData);
        Bytes out = ps2Data;
        std::vector<int> taken, kept;
        for (int g = 0; g < kDemoGroups && size_t(g) < ours.size(); ++g)
        {
            const int level = g / 2;
            if (demoReplayOk(ps2Data, ours[size_t(g)], level))
            {
                kept.push_back(g);
                continue;
            }
            if (size_t(g) < theirs.size() && theirs[size_t(g)].id == uint32_t(g) && demoReplayOk(xboxData, theirs[size_t(g)], level))
            {
                const Group &t = theirs[size_t(g)];
                const Bytes body = sliceOf(xboxData, 0x10 + size_t(t.offset), 0x10 + size_t(t.offset) + t.length);
                out.resize((out.size() + 15) & ~size_t(15), 0);
                putU32(out, 0x14 + 12 * size_t(g) + 4, uint32_t(out.size() - 0x10));
                putU32(out, 0x14 + 12 * size_t(g) + 8, uint32_t(body.size()));
                appendBytes(out, body);
                taken.push_back(g);
            }
        }
        made.push_back({"demo/REPLAY.ZAL", std::move(out)});
        log.push_back("[dm2xbox] attract demo: demo/REPLAY.ZAL, the PS2's demo groups " + listText(kept) +
                      " kept, the Xbox's " + listText(taken) + " taken");
    }
}
