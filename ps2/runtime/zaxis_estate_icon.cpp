// Estate's level-select icon, made at build time from its own files: a
// picture of the level drawn from TUT.GEO, TUT.INS and TUT.TXR, put in AI's
// oval badge, on a page of the level font's format (ICONS.UFNT). See
// runtime/zaxis_estate.h.
//
// Compiled without contracting a*b+c into one FMA (ZX_FP_NO_CONTRACT), and
// with only + - * / and sqrt (correctly rounded everywhere), so the Mac and
// Windows builds make the same bytes.

#include "runtime/zaxis_estate.h"

#include "runtime/zaxis_xbox.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>

namespace zx::zaxis::estate
{
    namespace
    {
        uint32_t rd32(const uint8_t *p)
        {
            uint32_t v = 0;
            std::memcpy(&v, p, 4);
            return v;
        }
        uint16_t rd16(const uint8_t *p)
        {
            uint16_t v = 0;
            std::memcpy(&v, p, 2);
            return v;
        }
        float rdf(const uint8_t *p)
        {
            float v = 0.0f;
            std::memcpy(&v, p, 4);
            return v;
        }
        void put16(std::vector<uint8_t> &out, uint16_t v)
        {
            out.push_back(static_cast<uint8_t>(v));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }
        void put32(std::vector<uint8_t> &out, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }

        struct Span
        {
            const uint8_t *tag = nullptr;
            const uint8_t *body = nullptr;
            uint32_t size = 0;
            bool is(const char *t) const { return std::memcmp(tag, t, 4) == 0; }
        };
        // The chunks of [p, p + size): tag, u32 size, body.
        std::vector<Span> spans(const uint8_t *p, size_t size)
        {
            std::vector<Span> out;
            for (size_t at = 0; at + 8u <= size;)
            {
                const uint32_t n = rd32(p + at + 4u);
                if (n > size - at - 8u)
                    break;
                out.push_back({p + at, p + at + 8u, n});
                at += 8u + n;
            }
            return out;
        }
        std::vector<Span> spans(const Span &s) { return spans(s.body, s.size); }

        // The engine's name hash (case-insensitive ELF hash).
        uint32_t nameHash(const std::string &text)
        {
            uint32_t h = 0;
            for (char ch : text)
            {
                uint32_t c = static_cast<uint8_t>(ch);
                if (c >= 'a' && c <= 'z')
                    c -= 'a' - 'A';
                h = (h << 4) + c;
                const uint32_t g = h & 0xF0000000u;
                if (g)
                    h ^= g >> 24;
                h &= ~g;
            }
            return h;
        }

        // ---- the level's textures ------------------------------------------------

        struct Texture
        {
            int width = 0, height = 0;
            bool alpha = false;          // a TZMP: its palette's alpha tested
            std::vector<uint32_t> rgba;  // top row first, R in the low byte
        };

        // An 8- or 4-bit BMP's level 0 with its palette (BGRA quads).
        bool decodeBmp(const uint8_t *p, size_t size, bool keepAlpha, Texture &out)
        {
            if (size < 54u || p[0] != 'B' || p[1] != 'M')
                return false;
            const uint32_t dataOffset = rd32(p + 10), headerSize = rd32(p + 14);
            int32_t w = 0, h = 0;
            std::memcpy(&w, p + 18, 4);
            std::memcpy(&h, p + 22, 4);
            const uint16_t bpp = rd16(p + 28);
            uint32_t colours = rd32(p + 46);
            if ((bpp != 8 && bpp != 4) || rd32(p + 30) != 0u || w <= 0 || h == 0 || w > 4096 || h > 4096 || h < -4096)
                return false;
            const bool bottomUp = h > 0;
            const int height = h < 0 ? -h : h;
            if (!colours)
                colours = 1u << bpp;
            const size_t palette = 14u + headerSize;
            const size_t stride = ((static_cast<size_t>(w) * bpp + 31u) / 32u) * 4u;
            if (palette + colours * 4u > size || dataOffset + stride * static_cast<size_t>(height) > size)
                return false;
            out.width = w;
            out.height = height;
            out.alpha = keepAlpha;
            out.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(height), 0u);
            for (int y = 0; y < height; ++y)
            {
                const uint8_t *row = p + dataOffset + static_cast<size_t>(bottomUp ? height - 1 - y : y) * stride;
                for (int x = 0; x < w; ++x)
                {
                    const uint32_t index = bpp == 8 ? row[x] : (x & 1 ? row[x / 2] & 0x0Fu : row[x / 2] >> 4u);
                    const uint8_t *c = index < colours ? p + palette + index * 4u : p + palette;
                    const uint32_t a = keepAlpha ? c[3] : 255u;
                    out.rgba[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                        static_cast<uint32_t>(c[2]) | (static_cast<uint32_t>(c[1]) << 8) | (static_cast<uint32_t>(c[0]) << 16) |
                        (a << 24);
                }
            }
            return true;
        }

        std::map<uint32_t, Texture> readTextures(const std::vector<uint8_t> &txr)
        {
            std::map<uint32_t, Texture> out;
            if (txr.size() < 8u || std::memcmp(txr.data(), "FORM", 4) != 0)
                return out;
            for (const Span &entry : spans(txr.data() + 8, txr.size() - 8u))
            {
                if (!entry.is("TXRC"))
                    continue;
                uint32_t hash = 0;
                bool named = false;
                for (const Span &c : spans(entry))
                {
                    if (c.is("TXRH") && c.size >= 4u)
                    {
                        hash = rd32(c.body);
                        named = true;
                    }
                    else if (named && (c.is("TBMP") || c.is("TZMP")))
                    {
                        Texture t;
                        if (decodeBmp(c.body, c.size, c.is("TZMP"), t))
                            out[hash] = std::move(t);
                        break; // level 0
                    }
                }
            }
            return out;
        }

        // ---- the level's geometry -----------------------------------------------

        struct Vertex
        {
            float p[3];
            float uv[2];
            float rgb[3];
        };
        struct Triangle
        {
            uint32_t v[3];
            uint32_t texture; // its material's texture hash, 0 none
        };
        struct Mesh
        {
            std::vector<Vertex> vertices;
            std::vector<Triangle> triangles;
        };

        // An indexed stream: its TIDA table and the per-vertex indices.
        bool indexed(const Span &s, const uint8_t *&table, size_t &tableBytes, std::vector<uint32_t> &indices)
        {
            table = nullptr;
            for (const Span &k : spans(s))
            {
                if (k.is("TIDA"))
                {
                    table = k.body;
                    tableBytes = k.size;
                }
                else if (k.is("TII8"))
                    for (uint32_t i = 0; i < k.size; ++i)
                        indices.push_back(k.body[i]);
                else if (k.is("TII6"))
                    for (uint32_t i = 0; i + 2u <= k.size; i += 2u)
                        indices.push_back(rd16(k.body + i));
            }
            return table != nullptr;
        }

        // A TGPT part's vertices and triangles (strip groups: an entry's
        // 0x8000 starts a strip, its two first vertices make no triangle;
        // mode 1 a plain list), in object space.
        void readPart(const Span &tgpt, const std::vector<uint32_t> &materials, Mesh &mesh)
        {
            uint16_t count = 0;
            std::vector<Span> streams = spans(tgpt);
            for (const Span &c : streams)
                if (c.is("TGPH") && c.size >= 20u)
                    count = rd16(c.body + 4);
            if (!count)
                return;
            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            std::vector<Vertex> v(count);
            for (Vertex &x : v)
            {
                x.uv[0] = x.uv[1] = 0.0f;
                x.rgb[0] = x.rgb[1] = x.rgb[2] = 1.0f;
            }
            bool positions = false;
            for (const Span &c : streams)
            {
                if (c.is("TGVP") && c.size >= count * 12u)
                {
                    positions = true;
                    for (uint16_t i = 0; i < count; ++i)
                        for (int k = 0; k < 3; ++k)
                            v[i].p[k] = rdf(c.body + i * 12u + k * 4u);
                }
                else if (c.is("TGVU") && c.size >= count * 8u)
                {
                    for (uint16_t i = 0; i < count; ++i)
                        for (int k = 0; k < 2; ++k)
                            v[i].uv[k] = rdf(c.body + i * 8u + k * 4u);
                }
                else if (c.is("TGVC") && c.size >= count * 4u)
                {
                    for (uint16_t i = 0; i < count; ++i)
                        for (int k = 0; k < 3; ++k)
                            v[i].rgb[k] = static_cast<float>(c.body[i * 4u + k]) / 128.0f;
                }
                else if (c.is("TIVU") || c.is("TIVC"))
                {
                    const uint8_t *table = nullptr;
                    size_t bytes = 0;
                    std::vector<uint32_t> ix;
                    if (!indexed(c, table, bytes, ix) || ix.size() < count)
                        continue;
                    for (uint16_t i = 0; i < count; ++i)
                    {
                        if (c.is("TIVU") && (ix[i] + 1u) * 8u <= bytes)
                            for (int k = 0; k < 2; ++k)
                                v[i].uv[k] = rdf(table + ix[i] * 8u + k * 4u);
                        if (c.is("TIVC") && (ix[i] + 1u) * 4u <= bytes)
                            for (int k = 0; k < 3; ++k)
                                v[i].rgb[k] = static_cast<float>(table[ix[i] * 4u + k]) / 128.0f;
                    }
                }
            }
            if (!positions)
                return;
            mesh.vertices.insert(mesh.vertices.end(), v.begin(), v.end());
            for (const Span &c : streams)
            {
                if (!c.is("TGFM"))
                    continue;
                for (size_t at = 0; at + 4u <= c.size;)
                {
                    const uint8_t mode = c.body[at] & 3u, material = c.body[at + 1];
                    const uint16_t n = rd16(c.body + at + 2);
                    if (at + 4u + n * 2u > c.size)
                        break;
                    const uint32_t texture = material < materials.size() ? materials[material] : 0u;
                    (void)mode; // lists and strips alike: the GS's drawing kick bit
                    // An entry ends a triangle (the two before it and it)
                    // unless it has 0x8000; the first two never do (as the
                    // game's packets draw them; zge/native/geo_meshes.cpp).
                    for (uint16_t i = 2; i < n; ++i)
                    {
                        const uint16_t e = rd16(c.body + at + 4u + i * 2u);
                        const uint32_t a0 = rd16(c.body + at + 4u + (i - 2u) * 2u) & 0x3FFFu,
                                       a1 = rd16(c.body + at + 4u + (i - 1u) * 2u) & 0x3FFFu, a2 = e & 0x3FFFu;
                        if ((e & 0x8000u) || a0 >= count || a1 >= count || a2 >= count)
                            continue;
                        mesh.triangles.push_back({{base + a0, base + a1, base + a2}, texture});
                    }
                    at += (n * 2u + 7u) & ~size_t(3);
                }
            }
        }

        // Every object of the GEO by its id (TGHD +0x18).
        std::map<uint32_t, Mesh> readObjects(const std::vector<uint8_t> &geo)
        {
            std::map<uint32_t, Mesh> out;
            if (geo.size() < 8u || std::memcmp(geo.data(), "FORM", 4) != 0)
                return out;
            for (const Span &object : spans(geo.data() + 8, geo.size() - 8u))
            {
                if (!object.is("TGEO"))
                    continue;
                const std::vector<Span> kids = spans(object);
                uint32_t id = 0;
                std::vector<uint32_t> materials;
                for (const Span &k : kids)
                {
                    if (k.is("TGHD") && k.size >= 0x1Cu)
                        id = rd32(k.body + 0x18);
                    if (k.is("TMTB"))
                        for (const Span &m : spans(k))
                            if (m.is("MATI") && m.size >= 8u)
                                // its first layer's texture (+0x40; the layer count at +0x2C)
                                materials.push_back(m.size >= 0x44u && rd32(m.body + 0x2C) ? rd32(m.body + 0x40) : 0u);
                }
                Mesh &mesh = out[id];
                for (const Span &k : kids)
                    if (k.is("TGPT"))
                        readPart(k, materials, mesh);
            }
            return out;
        }

        struct Instance
        {
            uint32_t flags = 0, object = 0, parent = 0, id = 0;
            float rotation[4] = {}; // x, y, z, w
            float translation[3] = {};
            std::string name;
        };

        std::vector<Instance> readInstances(const std::vector<uint8_t> &ins)
        {
            std::vector<Instance> out;
            if (ins.size() < 8u || std::memcmp(ins.data(), "FORM", 4) != 0)
                return out;
            for (const Span &tgin : spans(ins.data() + 8, ins.size() - 8u))
                for (const Span &h : spans(tgin))
                {
                    if (!h.is("TGIH") || h.size < 0x70u)
                        continue;
                    Instance i;
                    i.flags = rd32(h.body);
                    for (int k = 0; k < 4; ++k)
                        i.rotation[k] = rdf(h.body + 8 + k * 4);
                    for (int k = 0; k < 3; ++k)
                        i.translation[k] = rdf(h.body + 0x18 + k * 4);
                    i.object = rd32(h.body + 0x40);
                    i.parent = rd32(h.body + 0x44);
                    i.id = rd32(h.body + 0x48);
                    const char *name = reinterpret_cast<const char *>(h.body + 0x50);
                    i.name.assign(name, strnlen(name, 0x20));
                    out.push_back(i);
                }
            return out;
        }

        // v turned by the quaternion q.
        void rotate(const float *q, const float *v, float *out)
        {
            const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]), ty = 2.0f * (q[2] * v[0] - q[0] * v[2]),
                        tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
            out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
            out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
            out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
        }

        // What the level shows when it starts: not the lesson props (named
        // *_L<n> or *_Lcess, hidden until their lesson) and not the
        // panorama (the far skyline ring); the roofs left off too (seen from
        // above they would hide the courtyard).
        bool drawn(const Instance &i)
        {
            std::string n = i.name;
            for (char &c : n)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n.rfind("panorama", 0) == 0 || n.find("roof") != std::string::npos)
                return false;
            const size_t l = n.rfind("_l");
            return !(l != std::string::npos && l + 2u < n.size() &&
                     ((n[l + 2u] >= '0' && n[l + 2u] <= '9') || n.compare(l + 2u, 4, "cess") == 0));
        }

        // ---- the picture ------------------------------------------------------------

        struct View
        {
            float centre[3], right[3], up[3], forward[3];
            float scale; // pixels per unit
            int size;
        };

        void normalize(float *v)
        {
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            for (int k = 0; k < 3; ++k)
                v[k] = v[k] / l;
        }
        void cross(const float *a, const float *b, float *out)
        {
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        }
        float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    }

    bool renderPicture(const std::vector<uint8_t> &geo, const std::vector<uint8_t> &ins, const std::vector<uint8_t> &txr,
                       int size, Picture &out, PictureReport *report, std::string *error)
    {
        return renderPicture(geo, ins, txr, size, size, 1.0f, out, report, error);
    }

    bool renderPicture(const std::vector<uint8_t> &geo, const std::vector<uint8_t> &ins, const std::vector<uint8_t> &txr,
                       int width, int height, float shownAspect, Picture &out, PictureReport *report, std::string *error)
    {
        const std::map<uint32_t, Texture> textures = readTextures(txr);
        const std::map<uint32_t, Mesh> objects = readObjects(geo);
        const std::vector<Instance> instances = readInstances(ins);
        if (objects.empty() || instances.empty())
        {
            if (error)
                *error = "no objects or instances";
            return false;
        }
        std::map<uint32_t, const Instance *> byId;
        for (const Instance &i : instances)
            byId[i.id] = &i;
        // Every drawn instance's triangles in world space (a child in its
        // parent's space, as the game attaches it).
        struct WorldTriangle
        {
            Vertex v[3];
            uint32_t texture;
        };
        std::vector<WorldTriangle> world;
        float groundMin[3] = {1e30f, 1e30f, 1e30f}, groundMax[3] = {-1e30f, -1e30f, -1e30f};
        size_t drawnInstances = 0;
        for (const Instance &i : instances)
        {
            auto o = objects.find(i.object);
            if (!drawn(i) || o == objects.end())
                continue;
            ++drawnInstances;
            std::string lower = i.name;
            for (char &c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            const bool ground = lower.rfind("terrain_ground", 0) == 0;
            std::vector<Vertex> placed = o->second.vertices;
            for (Vertex &v : placed)
            {
                float p[3] = {v.p[0], v.p[1], v.p[2]};
                const Instance *at = &i;
                for (int depth = 0; at && depth < 16; ++depth)
                {
                    float r[3];
                    rotate(at->rotation, p, r);
                    for (int k = 0; k < 3; ++k)
                        p[k] = r[k] + at->translation[k];
                    auto parent = at->parent ? byId.find(at->parent) : byId.end();
                    at = parent == byId.end() ? nullptr : parent->second;
                }
                std::memcpy(v.p, p, sizeof(p));
                if (ground)
                    for (int k = 0; k < 3; ++k)
                    {
                        groundMin[k] = std::min(groundMin[k], p[k]);
                        groundMax[k] = std::max(groundMax[k], p[k]);
                    }
            }
            for (const Triangle &t : o->second.triangles)
                world.push_back({{placed[t.v[0]], placed[t.v[1]], placed[t.v[2]]}, t.texture});
        }
        if (world.empty() || groundMin[0] > groundMax[0])
        {
            if (error)
                *error = "no ground to frame";
            return false;
        }
        // The view: looking down at the grounds from the south-east, 50
        // degrees down (+y is down), orthographic, the grounds' middle in the
        // middle and their extent across the picture.
        View view{};
        for (int k = 0; k < 3; ++k)
            view.centre[k] = (groundMin[k] + groundMax[k]) * 0.5f;
        view.centre[1] = groundMax[1];
        view.forward[0] = -0.6f;
        view.forward[1] = 1.6f;
        view.forward[2] = -0.8f;
        normalize(view.forward);
        const float worldUp[3] = {0.0f, -1.0f, 0.0f};
        cross(view.forward, worldUp, view.right);
        normalize(view.right);
        cross(view.right, view.forward, view.up);
        float half = 0.0f;
        for (int cx = 0; cx < 2; ++cx)
            for (int cz = 0; cz < 2; ++cz)
            {
                const float corner[3] = {(cx ? groundMax[0] : groundMin[0]) - view.centre[0], 0.0f,
                                         (cz ? groundMax[2] : groundMin[2]) - view.centre[2]};
                half = std::max(half, std::max(std::fabs(dot(corner, view.right)), std::fabs(dot(corner, view.up))));
            }
        constexpr int kSuper = 3; // samples a side per pixel
        const int sideX = width * kSuper, sideY = height * kSuper;
        // The view's half width in units (a little closer than the whole
        // grounds), its half height as the picture is shown (shownAspect:
        // its width over its height on screen).
        const float halfX = half * 0.65f, halfY = halfX / shownAspect;
        const float scaleX = static_cast<float>(sideX) * 0.5f / halfX, scaleY = static_cast<float>(sideY) * 0.5f / halfY;
        std::vector<float> depth(static_cast<size_t>(sideX) * static_cast<size_t>(sideY), 1e30f);
        std::vector<uint32_t> colour(static_cast<size_t>(sideX) * static_cast<size_t>(sideY));
        for (int y = 0; y < sideY; ++y)
        {
            // the sky behind: AI's level select blue toward the top
            const uint32_t r = static_cast<uint32_t>(40 + 30 * y / sideY), g = static_cast<uint32_t>(70 + 40 * y / sideY),
                           b = static_cast<uint32_t>(120 + 40 * y / sideY);
            for (int x = 0; x < sideX; ++x)
                colour[static_cast<size_t>(y) * sideX + x] = r | (g << 8) | (b << 16);
        }
        size_t covered = 0, textured = 0;
        for (const WorldTriangle &t : world)
        {
            float sx[3], sy[3], sz[3];
            for (int k = 0; k < 3; ++k)
            {
                const float d[3] = {t.v[k].p[0] - view.centre[0], t.v[k].p[1] - view.centre[1], t.v[k].p[2] - view.centre[2]};
                sx[k] = static_cast<float>(sideX) * 0.5f + dot(d, view.right) * scaleX;
                sy[k] = static_cast<float>(sideY) * 0.5f - dot(d, view.up) * scaleY;
                sz[k] = dot(d, view.forward);
            }
            const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (area == 0.0f || !(std::fabs(area) < 1e12f))
                continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({sx[0], sx[1], sx[2]})))),
                      x1 = std::min(sideX - 1, static_cast<int>(std::ceil(std::max({sx[0], sx[1], sx[2]})))),
                      y0 = std::max(0, static_cast<int>(std::floor(std::min({sy[0], sy[1], sy[2]})))),
                      y1 = std::min(sideY - 1, static_cast<int>(std::ceil(std::max({sy[0], sy[1], sy[2]}))));
            if (x0 > x1 || y0 > y1)
                continue;
            auto tex = t.texture ? textures.find(t.texture) : textures.end();
            const Texture *texture = tex == textures.end() ? nullptr : &tex->second;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
                    float w[3];
                    w[0] = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) / area;
                    w[1] = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) / area;
                    w[2] = 1.0f - w[0] - w[1];
                    if (w[0] < 0.0f || w[1] < 0.0f || w[2] < 0.0f)
                        continue;
                    const float z = w[0] * sz[0] + w[1] * sz[1] + w[2] * sz[2];
                    float &zb = depth[static_cast<size_t>(y) * sideX + x];
                    if (!(z < zb))
                        continue;
                    float rgb[3] = {1.0f, 1.0f, 1.0f};
                    if (texture)
                    {
                        const float u = w[0] * t.v[0].uv[0] + w[1] * t.v[1].uv[0] + w[2] * t.v[2].uv[0];
                        const float v = w[0] * t.v[0].uv[1] + w[1] * t.v[1].uv[1] + w[2] * t.v[2].uv[1];
                        const float fu = u - std::floor(u), fv = v - std::floor(v);
                        const int tx = std::min(texture->width - 1, static_cast<int>(fu * static_cast<float>(texture->width)));
                        const int ty = std::min(texture->height - 1, static_cast<int>(fv * static_cast<float>(texture->height)));
                        const uint32_t texel = texture->rgba[static_cast<size_t>(ty) * texture->width + tx];
                        if (texture->alpha && (texel >> 24) < 0x40u)
                            continue; // alpha-tested out (leaves, fences)
                        if ((texel & 0xFFFFFFu) == 0xFF00FFu)
                            continue; // the exporter's see-through magenta (Transparent_Pink: invisible walls)
                        rgb[0] = static_cast<float>(texel & 0xFFu) / 255.0f;
                        rgb[1] = static_cast<float>((texel >> 8) & 0xFFu) / 255.0f;
                        rgb[2] = static_cast<float>((texel >> 16) & 0xFFu) / 255.0f;
                    }
                    else
                        rgb[0] = rgb[1] = rgb[2] = 0.6f;
                    zb = z;
                    uint32_t packed = 0;
                    for (int k = 0; k < 3; ++k)
                    {
                        // the prelit colour (128 = 1.0, as the GS modulates)
                        const float lit = rgb[k] * (w[0] * t.v[0].rgb[k] + w[1] * t.v[1].rgb[k] + w[2] * t.v[2].rgb[k]);
                        const float c = std::min(255.0f, std::max(0.0f, lit * 255.0f + 0.5f));
                        packed |= static_cast<uint32_t>(c) << (8 * k);
                    }
                    colour[static_cast<size_t>(y) * sideX + x] = packed;
                }
            ++covered;
            textured += texture != nullptr;
        }
        // Down to size x size, each pixel the mean of its samples.
        out.width = width;
        out.height = height;
        out.rgba.assign(static_cast<size_t>(width) * height, 0u);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                uint32_t sum[3] = {};
                for (int j = 0; j < kSuper; ++j)
                    for (int i = 0; i < kSuper; ++i)
                    {
                        const uint32_t c = colour[static_cast<size_t>(y * kSuper + j) * sideX + x * kSuper + i];
                        for (int k = 0; k < 3; ++k)
                            sum[k] += (c >> (8 * k)) & 0xFFu;
                    }
                uint32_t packed = 0xFF000000u;
                for (int k = 0; k < 3; ++k)
                    packed |= ((sum[k] + kSuper * kSuper / 2) / (kSuper * kSuper)) << (8 * k);
                out.rgba[static_cast<size_t>(y) * width + x] = packed;
            }
        if (report)
        {
            report->instances = drawnInstances;
            report->triangles = world.size();
            report->drawnTriangles = covered;
            report->texturedTriangles = textured;
            report->textures = textures.size();
        }
        return true;
    }

    // ---- the icon page --------------------------------------------------------------

    namespace
    {
        constexpr int kCell = 64, kIconSize = 63;
        constexpr double kBadge[4] = {33.0, 32.8, 36.0, 24.7}; // centre x, y, half axes (turned 45 degrees)
        constexpr int kGlow[3] = {103, 168, 241};              // AI's selected icons' blue

        // The share of pixel (x, y) inside the badge (4x4 samples) and the
        // distance of its centre outside the badge's edge (pixels, < 0
        // inside).
        void badgeCover(int x, int y, double &inside, double &outside)
        {
            const double cs = std::sqrt(0.5), sn = -std::sqrt(0.5);
            auto norm = [&](double px, double py)
            {
                const double dx = px - kBadge[0], dy = py - kBadge[1];
                const double u = dx * cs + dy * sn, v = -dx * sn + dy * cs;
                return std::sqrt((u / kBadge[2]) * (u / kBadge[2]) + (v / kBadge[3]) * (v / kBadge[3]));
            };
            int in = 0;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    in += norm(x + (i + 0.5) / 4.0, y + (j + 0.5) / 4.0) <= 1.0;
            inside = in / 16.0;
            outside = (norm(x + 0.5, y + 0.5) - 1.0) * kBadge[3];
        }

        // A 64x64 RGBA cell, the picture in the badge; unselected dimmed and
        // greyer with a light rim, selected in full colour with a blue glow.
        std::vector<uint32_t> makeIcon(const Picture &picture, bool selected)
        {
            std::vector<uint32_t> cell(kCell * kCell, 0u);
            const int w = picture.width, h = picture.height, x0 = 2, x1 = 64, y0 = 2, y1 = 64;
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                {
                    if (x >= kIconSize + 1 || y >= kIconSize + 1 || x == 0 || y == 0)
                        continue;
                    double cover = 0.0, outside = 0.0;
                    badgeCover(x, y, cover, outside);
                    int sx0 = (x - x0) * w / (x1 - x0), sx1 = std::max((x - x0 + 1) * w / (x1 - x0), sx0 + 1);
                    int sy0 = (y - y0) * h / (y1 - y0), sy1 = std::max((y - y0 + 1) * h / (y1 - y0), sy0 + 1);
                    sx0 = std::max(0, sx0), sx1 = std::min(w, sx1), sy0 = std::max(0, sy0), sy1 = std::min(h, sy1);
                    double c[3] = {0.0, 0.0, 0.0};
                    if (sx1 > sx0 && sy1 > sy0)
                    {
                        const double n = static_cast<double>((sx1 - sx0) * (sy1 - sy0));
                        for (int k = 0; k < 3; ++k)
                        {
                            uint64_t sum = 0;
                            for (int j = sy0; j < sy1; ++j)
                                for (int i = sx0; i < sx1; ++i)
                                    sum += (picture.rgba[static_cast<size_t>(j) * w + i] >> (8 * k)) & 0xFFu;
                            c[k] = static_cast<double>(sum) / n;
                        }
                    }
                    double rim = 0.0, rimWidth = 0.0, rimColour[3];
                    if (selected)
                    {
                        for (double &v : c)
                            v = std::min(255.0, v * 1.1);
                        rim = 1.0, rimWidth = 3.0;
                        for (int k = 0; k < 3; ++k)
                            rimColour[k] = kGlow[k];
                    }
                    else
                    {
                        const double grey = 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2];
                        for (double &v : c)
                            v = (0.45 * v + 0.55 * grey) * 0.55;
                        rim = 0.8, rimWidth = 1.5;
                        rimColour[0] = rimColour[1] = rimColour[2] = 200.0;
                    }
                    const double t = std::max(0.0, 1.0 - std::fabs(outside + rimWidth / 2.0) / (rimWidth / 2.0 + 0.5)) * rim;
                    for (int k = 0; k < 3; ++k)
                        c[k] = c[k] * (1.0 - t) + rimColour[k] * t;
                    double alpha = 255.0 * cover;
                    if (selected && outside > 0.0)
                    {
                        const double glow = std::max(0.0, 1.0 - outside / 3.0) * 0.8;
                        if (glow > cover)
                        {
                            for (int k = 0; k < 3; ++k)
                                c[k] = kGlow[k];
                            alpha = 255.0 * glow;
                        }
                    }
                    uint32_t packed = static_cast<uint32_t>(alpha + 0.5) << 24;
                    for (int k = 0; k < 3; ++k)
                        packed |= static_cast<uint32_t>(c[k] + 0.5) << (8 * k);
                    cell[static_cast<size_t>(y) * kCell + x] = (packed >> 24) ? packed : 0u;
                }
            return cell;
        }

        using Rgba = std::tuple<int, int, int, int>;
        int channel(const Rgba &c, int k) { return k == 0 ? std::get<0>(c) : k == 1 ? std::get<1>(c) : k == 2 ? std::get<2>(c) : std::get<3>(c); }

        // Median cut (dm2_level.py median_cut): at most `boxes` colours, each colour's index.
        void medianCut(const std::map<Rgba, uint64_t> &counts, size_t boxes, std::vector<Rgba> &palette,
                       std::map<Rgba, int> &index)
        {
            std::vector<std::vector<Rgba>> work(1);
            for (const auto &[c, n] : counts)
                work[0].push_back(c);
            while (work.size() < boxes)
            {
                int best = -1, bestChannel = 0;
                uint64_t bestScore = 0;
                for (size_t i = 0; i < work.size(); ++i)
                {
                    const std::vector<Rgba> &box = work[i];
                    if (box.size() < 2u)
                        continue;
                    int ch = 0, widest = -1;
                    for (int k = 0; k < 4; ++k)
                    {
                        int lo = 255, hi = 0;
                        for (const Rgba &c : box)
                            lo = std::min(lo, channel(c, k)), hi = std::max(hi, channel(c, k));
                        if (hi - lo > widest)
                            widest = hi - lo, ch = k;
                    }
                    uint64_t total = 0;
                    for (const Rgba &c : box)
                        total += counts.at(c);
                    const uint64_t score = static_cast<uint64_t>(widest) * total;
                    if (score > bestScore)
                        best = static_cast<int>(i), bestScore = score, bestChannel = ch;
                }
                if (best < 0)
                    break;
                std::vector<Rgba> box = work[static_cast<size_t>(best)];
                std::sort(box.begin(), box.end(), [&](const Rgba &a, const Rgba &b)
                          { return std::make_pair(channel(a, bestChannel), a) < std::make_pair(channel(b, bestChannel), b); });
                uint64_t total = 0, run = 0;
                for (const Rgba &c : box)
                    total += counts.at(c);
                size_t cut = 1;
                for (size_t k = 0; k + 1 < box.size(); ++k)
                {
                    run += counts.at(box[k]);
                    if (run * 2u >= total)
                    {
                        cut = k + 1;
                        break;
                    }
                }
                work[static_cast<size_t>(best)] = std::vector<Rgba>(box.begin(), box.begin() + static_cast<std::ptrdiff_t>(cut));
                work.insert(work.begin() + best + 1, std::vector<Rgba>(box.begin() + static_cast<std::ptrdiff_t>(cut), box.end()));
            }
            for (const std::vector<Rgba> &box : work)
            {
                uint64_t n = 0, s[4] = {};
                for (const Rgba &c : box)
                {
                    n += counts.at(c);
                    for (int k = 0; k < 4; ++k)
                        s[k] += static_cast<uint64_t>(channel(c, k)) * counts.at(c);
                }
                int mean[4];
                for (int k = 0; k < 4; ++k)
                    mean[k] = static_cast<int>((s[k] * 2u + n) / (2u * n)); // rounded
                palette.emplace_back(mean[0], mean[1], mean[2], mean[3]);
                for (const Rgba &c : box)
                    index[c] = static_cast<int>(palette.size()) - 1;
            }
        }

        void chunk(std::vector<uint8_t> &out, const char *tag, const std::vector<uint8_t> &body)
        {
            out.insert(out.end(), tag, tag + 4);
            put32(out, static_cast<uint32_t>(body.size() + (-body.size() & 3u)));
            out.insert(out.end(), body.begin(), body.end());
            out.resize(out.size() + (-body.size() & 3u), 0u);
        }
    }

    std::vector<uint8_t> iconFont(const Picture &picture, char unselected, char selected, const std::string &pageName)
    {
        // One page of 256 x 64: the two cells side by side.
        constexpr int kPageWidth = 256, kPageHeight = 64;
        const std::vector<uint32_t> cells[2] = {makeIcon(picture, false), makeIcon(picture, true)};
        std::map<Rgba, uint64_t> counts;
        for (const std::vector<uint32_t> &cell : cells)
            for (uint32_t c : cell)
                if (c >> 24)
                    ++counts[Rgba(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24)];
        std::vector<Rgba> palette{Rgba(0, 0, 0, 0)}; // index 0: clear
        std::vector<Rgba> found;
        std::map<Rgba, int> index;
        medianCut(counts, 255u, found, index);
        palette.insert(palette.end(), found.begin(), found.end());
        std::vector<uint8_t> page(kPageWidth * kPageHeight, 0u);
        for (int j = 0; j < 2; ++j)
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                {
                    const uint32_t c = cells[j][static_cast<size_t>(y) * kCell + x];
                    if (c >> 24)
                        page[static_cast<size_t>(y) * kPageWidth + j * kCell + x] =
                            static_cast<uint8_t>(index.at(Rgba(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24)) + 1);
                }
        // The page as an 8-bit BMP (bottom-up, a BGRA palette of 256).
        std::vector<uint8_t> bmp{'B', 'M'};
        const uint32_t pixels = kPageWidth * kPageHeight, offset = 14u + 40u + 1024u;
        put32(bmp, offset + pixels);
        put32(bmp, 0u);
        put32(bmp, offset);
        put32(bmp, 40u);
        put32(bmp, kPageWidth);
        put32(bmp, kPageHeight);
        put16(bmp, 1u);
        put16(bmp, 8u);
        put32(bmp, 0u);
        put32(bmp, pixels);
        put32(bmp, 0u);
        put32(bmp, 0u);
        put32(bmp, 256u);
        put32(bmp, 0u);
        for (size_t i = 0; i < 256u; ++i)
        {
            const Rgba c = i < palette.size() ? palette[i] : Rgba(0, 0, 0, 0);
            bmp.push_back(static_cast<uint8_t>(std::get<2>(c)));
            bmp.push_back(static_cast<uint8_t>(std::get<1>(c)));
            bmp.push_back(static_cast<uint8_t>(std::get<0>(c)));
            bmp.push_back(static_cast<uint8_t>(std::get<3>(c)));
        }
        for (int y = kPageHeight - 1; y >= 0; --y)
            bmp.insert(bmp.end(), page.begin() + static_cast<std::ptrdiff_t>(y) * kPageWidth,
                       page.begin() + static_cast<std::ptrdiff_t>(y + 1) * kPageWidth);
        // HFNT: line height 11, 2 glyphs, 1 page {256, 64, name, 0}, then the
        // glyphs {char, page, 0, x, y, w, h}.
        std::vector<uint8_t> name(32, 0u);
        std::memcpy(name.data(), pageName.data(), std::min<size_t>(pageName.size(), 31u));
        std::vector<uint8_t> hfnt{11, 0};
        put16(hfnt, 2u);
        put16(hfnt, 1u);
        put16(hfnt, kPageWidth);
        put16(hfnt, kPageHeight);
        hfnt.insert(hfnt.end(), name.begin(), name.end());
        put32(hfnt, 0u);
        const char letters[2] = {unselected, selected};
        for (int j = 0; j < 2; ++j)
        {
            hfnt.push_back(static_cast<uint8_t>(letters[j]));
            hfnt.push_back(0u);
            put16(hfnt, 0u);
            put16(hfnt, static_cast<uint16_t>(j * kCell + 1));
            put16(hfnt, 1u);
            put16(hfnt, kIconSize);
            put16(hfnt, kIconSize);
        }
        std::vector<uint8_t> txrh;
        put32(txrh, nameHash(pageName));
        put16(txrh, kPageWidth);
        put16(txrh, kPageHeight);
        put32(txrh, 0x20u);
        txrh.resize(txrh.size() + 8u, 0u);
        txrh.insert(txrh.end(), name.begin(), name.end());
        std::vector<uint8_t> txrc, hmtl, body, font;
        chunk(txrc, "TXRH", txrh);
        chunk(txrc, "TZMP", bmp);
        put32(hmtl, 0x111u);
        chunk(body, "HFNT", hfnt);
        chunk(body, "HMTL", hmtl);
        chunk(body, "TXRC", txrc);
        chunk(font, "UFNT", body);
        return font;
    }

    std::vector<uint8_t> stillLibrary(const Picture &picture, const std::string &name)
    {
        // The picture in 256 colours, as the build makes the level's one
        // 24-bit texture 8-bit (ps2_zaxis_xbox palettize: median cut, k-means).
        zx::zaxis::xbox::Picture rgba;
        rgba.width = picture.width;
        rgba.height = picture.height;
        rgba.rgba = picture.rgba;
        zx::zaxis::xbox::Palettized image;
        zx::zaxis::xbox::palettize(rgba, false, image);
        const std::vector<uint8_t> bmp = zx::zaxis::xbox::writeBmp8(picture.width, picture.height, image, false);
        const uint32_t w = static_cast<uint32_t>(picture.width), h = static_cast<uint32_t>(picture.height);
        // UAST { HAST source path, UMTL { HMTL, TXRC { TXRH, TBMP } } } in a ULIB,
        // as the FrontEnd library's still_bkg_01 asset is made.
        std::vector<uint8_t> nameBytes(32, 0u);
        std::memcpy(nameBytes.data(), name.data(), std::min<size_t>(name.size(), 31u));
        std::vector<uint8_t> txrh;
        put32(txrh, nameHash(name));
        put16(txrh, static_cast<uint16_t>(w));
        put16(txrh, static_cast<uint16_t>(h));
        put32(txrh, 0x20u);
        txrh.resize(txrh.size() + 8u, 0u);
        txrh.insert(txrh.end(), nameBytes.begin(), nameBytes.end());
        const std::string source = "S:\\inline\\Art\\Interface\\FrontEnd\\background image\\" + name + ".bmp";
        // HAST: the source path in 128 bytes, the asset's name (what a script
        // names it by: "FrontEnd.<name>") in 32, a word.
        std::vector<uint8_t> hast(164u, 0u);
        std::memcpy(hast.data(), source.data(), std::min<size_t>(source.size(), 127u));
        std::memcpy(hast.data() + 128, nameBytes.data(), 32u);
        std::vector<uint8_t> txrc, hmtl{0x11, 0xC1, 0xCC, 0xCC}, umtl, uast, ulib;
        chunk(txrc, "TXRH", txrh);
        chunk(txrc, "TBMP", bmp);
        chunk(umtl, "HMTL", hmtl);
        chunk(umtl, "TXRC", txrc);
        chunk(uast, "HAST", hast);
        chunk(uast, "UMTL", umtl);
        std::vector<uint8_t> body;
        chunk(body, "UAST", uast);
        chunk(ulib, "ULIB", body);
        return ulib;
    }

    std::vector<uint8_t> pictureBmp(const Picture &picture)
    {
        std::vector<uint8_t> bmp{'B', 'M'};
        const uint32_t stride = (static_cast<uint32_t>(picture.width) * 3u + 3u) & ~3u;
        const uint32_t pixels = stride * static_cast<uint32_t>(picture.height);
        put32(bmp, 54u + pixels);
        put32(bmp, 0u);
        put32(bmp, 54u);
        put32(bmp, 40u);
        put32(bmp, static_cast<uint32_t>(picture.width));
        put32(bmp, static_cast<uint32_t>(picture.height));
        put16(bmp, 1u);
        put16(bmp, 24u);
        put32(bmp, 0u);
        put32(bmp, pixels);
        for (int i = 0; i < 4; ++i)
            put32(bmp, 0u);
        for (int y = picture.height - 1; y >= 0; --y)
        {
            for (int x = 0; x < picture.width; ++x)
            {
                const uint32_t c = picture.rgba[static_cast<size_t>(y) * picture.width + x];
                bmp.push_back(static_cast<uint8_t>(c >> 16));
                bmp.push_back(static_cast<uint8_t>(c >> 8));
                bmp.push_back(static_cast<uint8_t>(c));
            }
            bmp.resize(bmp.size() + (stride - static_cast<uint32_t>(picture.width) * 3u), 0u);
        }
        return bmp;
    }
}
