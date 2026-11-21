// Aggressive Inline's TUT/Estate made into the files the PS2 game loads, at
// build time. See runtime/zaxis_estate.h.

#include "runtime/zaxis_estate.h"

#include "runtime/texture_replace.h"
#include "runtime/zaxis_xbox.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

namespace zx::zaxis::estate
{
    namespace
    {
        namespace xb = zx::zaxis::xbox;

        uint32_t u32(const uint8_t *p)
        {
            uint32_t v = 0;
            std::memcpy(&v, p, 4);
            return v;
        }

        uint16_t u16(const uint8_t *p)
        {
            uint16_t v = 0;
            std::memcpy(&v, p, 2);
            return v;
        }

        void put32(std::vector<uint8_t> &out, uint32_t v)
        {
            for (int i = 0; i < 4; ++i)
                out.push_back(static_cast<uint8_t>(v >> (8 * i)));
        }

        bool fail(std::string *error, const std::string &why)
        {
            if (error)
                *error = why;
            return false;
        }

        struct Chunk
        {
            size_t at = 0;   // the tag's offset
            uint32_t size = 0; // the payload's
            const uint8_t *payload = nullptr;
            std::string tag;
        };

        // The chunks of [begin, end): tag, u32 size, payload (sizes are
        // already multiples of 4 in these files).
        bool chunks(const std::vector<uint8_t> &d, size_t begin, size_t end, std::vector<Chunk> &out)
        {
            out.clear();
            for (size_t at = begin; at < end;)
            {
                if (at + 8u > end)
                    return false;
                Chunk c;
                c.at = at;
                c.tag.assign(reinterpret_cast<const char *>(&d[at]), 4);
                c.size = u32(&d[at + 4u]);
                if (c.size > end - at - 8u)
                    return false;
                c.payload = &d[at + 8u];
                out.push_back(c);
                at += 8u + c.size;
            }
            return true;
        }

        struct Bmp
        {
            int width = 0, height = 0, bpp = 0;
            bool bottomUp = true;
            uint32_t dataOffset = 0, headerSize = 0, colours = 0;
        };

        bool readBmpHeader(const uint8_t *p, size_t size, Bmp &bmp)
        {
            if (size < 54u || p[0] != 'B' || p[1] != 'M')
                return false;
            bmp.dataOffset = u32(p + 10);
            bmp.headerSize = u32(p + 14);
            int32_t w = 0, h = 0;
            std::memcpy(&w, p + 18, 4);
            std::memcpy(&h, p + 22, 4);
            bmp.bpp = u16(p + 28);
            const uint32_t compression = u32(p + 30);
            bmp.colours = u32(p + 46);
            bmp.width = w;
            bmp.bottomUp = h > 0;
            bmp.height = h < 0 ? -h : h;
            if (compression != 0u || bmp.width <= 0 || bmp.height <= 0 || bmp.width > 4096 || bmp.height > 4096)
                return false;
            const size_t stride = ((static_cast<size_t>(bmp.width) * static_cast<size_t>(bmp.bpp) + 31u) / 32u) * 4u;
            return bmp.dataOffset <= size && stride * static_cast<size_t>(bmp.height) <= size - bmp.dataOffset;
        }

        size_t strideOf(const Bmp &bmp)
        {
            return ((static_cast<size_t>(bmp.width) * static_cast<size_t>(bmp.bpp) + 31u) / 32u) * 4u;
        }

        // A 24- or 32-bit BMP as RGBA, top row first (32-bit: BGRA, its
        // alpha kept; 24-bit: opaque).
        bool readTrueColour(const uint8_t *p, size_t size, const Bmp &bmp, xb::Picture &out)
        {
            if (bmp.bpp != 24 && bmp.bpp != 32)
                return false;
            const size_t stride = strideOf(bmp), step = static_cast<size_t>(bmp.bpp / 8);
            out.width = bmp.width;
            out.height = bmp.height;
            out.rgba.assign(static_cast<size_t>(bmp.width) * static_cast<size_t>(bmp.height), 0u);
            for (int y = 0; y < bmp.height; ++y)
            {
                const int row = bmp.bottomUp ? bmp.height - 1 - y : y;
                const uint8_t *src = p + bmp.dataOffset + static_cast<size_t>(row) * stride;
                if (src + static_cast<size_t>(bmp.width) * step > p + size)
                    return false;
                for (int x = 0; x < bmp.width; ++x)
                {
                    const uint8_t *px = src + static_cast<size_t>(x) * step;
                    const uint32_t a = step == 4u ? px[3] : 255u;
                    out.rgba[static_cast<size_t>(y) * static_cast<size_t>(bmp.width) + static_cast<size_t>(x)] =
                        static_cast<uint32_t>(px[2]) | (static_cast<uint32_t>(px[1]) << 8) |
                        (static_cast<uint32_t>(px[0]) << 16) | (a << 24);
                }
            }
            return true;
        }

        // The indices of an 8- or 4-bit BMP, a byte a texel, rows top down
        // (the BMP's bottom-up rows reversed, as the game loads them; 4-bit:
        // the first texel in the high nibble).
        bool readIndices(const uint8_t *p, size_t size, const Bmp &bmp, std::vector<uint8_t> &out)
        {
            if (bmp.bpp != 8 && bmp.bpp != 4)
                return false;
            const size_t stride = strideOf(bmp);
            out.assign(static_cast<size_t>(bmp.width) * static_cast<size_t>(bmp.height), 0u);
            for (int y = 0; y < bmp.height; ++y)
            {
                const int row = bmp.bottomUp ? bmp.height - 1 - y : y;
                const uint8_t *src = p + bmp.dataOffset + static_cast<size_t>(row) * stride;
                if (src + stride > p + size)
                    return false;
                uint8_t *dst = &out[static_cast<size_t>(y) * static_cast<size_t>(bmp.width)];
                for (int x = 0; x < bmp.width; ++x)
                    dst[x] = bmp.bpp == 8 ? src[x] : static_cast<uint8_t>(x & 1 ? src[x / 2] & 0x0Fu : src[x / 2] >> 4);
            }
            return true;
        }

        // The TXRC entries of a library: (offset of the entry's tag, its chunks).
        bool libraryEntries(const std::vector<uint8_t> &txr, std::vector<std::pair<Chunk, std::vector<Chunk>>> &out)
        {
            out.clear();
            if (txr.size() < 8u || std::memcmp(txr.data(), "FORM", 4) != 0 || u32(&txr[4]) != txr.size() - 8u)
                return false;
            std::vector<Chunk> top;
            if (!chunks(txr, 8u, txr.size(), top))
                return false;
            for (const Chunk &entry : top)
            {
                if (entry.tag != "TXRC")
                    return false;
                std::vector<Chunk> inner;
                if (!chunks(txr, entry.at + 8u, entry.at + 8u + entry.size, inner))
                    return false;
                out.emplace_back(entry, std::move(inner));
            }
            return true;
        }

        std::string headerName(const Chunk &header)
        {
            if (header.tag != "TXRH" || header.size < 0x15u)
                return {};
            const char *name = reinterpret_cast<const char *>(header.payload + 0x14);
            return std::string(name, strnlen(name, header.size - 0x14u));
        }

        std::string upper(std::string text)
        {
            for (char &c : text)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return text;
        }
    }

    bool palettizeLibrary(std::vector<uint8_t> &txr, TextureReport *report, std::string *error)
    {
        std::vector<std::pair<Chunk, std::vector<Chunk>>> entries;
        if (!libraryEntries(txr, entries))
            return fail(error, "not a texture library (FORM of TXRC)");
        TextureReport r;
        std::vector<uint8_t> out;
        out.insert(out.end(), txr.begin(), txr.begin() + 8);
        for (const auto &[entry, inner] : entries)
        {
            ++r.textures;
            std::vector<uint8_t> body;
            std::string name;
            for (const Chunk &c : inner)
            {
                if (c.tag == "TXRH")
                    name = headerName(c);
                const bool picture = c.tag == "TBMP" || c.tag == "TZMP";
                Bmp bmp;
                if (picture)
                {
                    ++r.pictures;
                    if (!readBmpHeader(c.payload, c.size, bmp))
                        return fail(error, "texture " + name + ": an unreadable BMP");
                }
                if (!picture || bmp.bpp == 8 || bmp.bpp == 4)
                {
                    body.insert(body.end(), txr.begin() + static_cast<std::ptrdiff_t>(c.at),
                                txr.begin() + static_cast<std::ptrdiff_t>(c.at + 8u + c.size));
                    continue;
                }
                xb::Picture rgba;
                if (!readTrueColour(c.payload, c.size, bmp, rgba))
                    return fail(error, "texture " + name + ": a " + std::to_string(bmp.bpp) + "-bit BMP");
                // A TZMP keeps its alpha (the palette's); a TBMP is opaque.
                const bool alpha = c.tag == "TZMP";
                xb::Palettized image;
                xb::palettize(rgba, alpha, image);
                std::vector<uint8_t> written = xb::writeBmp8(rgba.width, rgba.height, image, alpha);
                written.resize((written.size() + 3u) & ~size_t(3));
                body.insert(body.end(), c.tag.begin(), c.tag.end());
                put32(body, static_cast<uint32_t>(written.size()));
                body.insert(body.end(), written.begin(), written.end());
                ++r.converted;
                r.convertedNames.push_back(name + " (" + std::to_string(bmp.width) + "x" + std::to_string(bmp.height) +
                                           ", " + std::to_string(bmp.bpp) + "-bit)");
            }
            out.insert(out.end(), {'T', 'X', 'R', 'C'});
            put32(out, static_cast<uint32_t>(body.size()));
            out.insert(out.end(), body.begin(), body.end());
        }
        const uint32_t formSize = static_cast<uint32_t>(out.size() - 8u);
        std::memcpy(&out[4], &formSize, 4);
        txr = std::move(out);
        if (report)
            *report = std::move(r);
        return true;
    }

    std::vector<TextureKey> textureKeys(const std::vector<uint8_t> &txr)
    {
        std::vector<TextureKey> keys;
        std::vector<std::pair<Chunk, std::vector<Chunk>>> entries;
        if (!libraryEntries(txr, entries))
            return keys;
        for (const auto &[entry, inner] : entries)
        {
            std::string name;
            for (const Chunk &c : inner)
            {
                if (c.tag == "TXRH")
                    name = headerName(c);
                if (c.tag != "TBMP" && c.tag != "TZMP")
                    continue;
                Bmp bmp;
                std::vector<uint8_t> indices;
                if (readBmpHeader(c.payload, c.size, bmp) && readIndices(c.payload, c.size, bmp, indices))
                    keys.push_back({static_cast<uint32_t>(bmp.width), static_cast<uint32_t>(bmp.height),
                                    zx::texture_replace::indexHash(indices.data(), indices.size()), name});
                break; // level 0 only (the key of the texture object)
            }
        }
        return keys;
    }

    std::string writeTextureKeys(const std::vector<TextureKey> &keys)
    {
        std::string text;
        for (const TextureKey &k : keys)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "%u %u %016llx ", k.width, k.height,
                          static_cast<unsigned long long>(k.indexHash));
            text += line + k.name + "\n";
        }
        return text;
    }

    std::vector<TextureKey> readTextureKeys(const std::string &text)
    {
        std::vector<TextureKey> keys;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            std::istringstream fields(line);
            TextureKey k;
            std::string hash;
            if (!(fields >> k.width >> k.height >> hash) || hash.empty() || hash.size() > 16u)
                continue;
            k.indexHash = std::stoull(hash, nullptr, 16);
            std::getline(fields >> std::ws, k.name);
            while (!k.name.empty() && (k.name.back() == '\r' || k.name.back() == ' '))
                k.name.pop_back();
            keys.push_back(std::move(k));
        }
        return keys;
    }

    namespace
    {
        // setObjectVisible(object, visible, keep) as compiled: Push keep,
        // Push visible, Push object, CallNative (runtime/... script.h).
        constexpr uint32_t kPush = 0x0Au, kCallNative = 0x1Eu, kEnd = 0x1F40u;
        constexpr uint32_t kSetObjectVisible = 0x098C3505u;
        constexpr uint32_t kEnterEvent = 0x004A38A2u; // "enter"

        struct Call
        {
            uint32_t object, state, event, target, visible, keep;
        };

        // Every setObjectVisible with constant arguments in every handler.
        bool visibilityCalls(const std::vector<uint8_t> &bin, std::vector<Call> &out)
        {
            out.clear();
            if (bin.size() < 0x14u)
                return false;
            const uint32_t codeWords = u32(&bin[4]), variableWords = u32(&bin[8]), dataBytes = u32(&bin[12]),
                           stringBytes = u32(&bin[16]);
            const size_t code = 0x14u + static_cast<size_t>(stringBytes) + dataBytes + variableWords * 4u;
            if (code + static_cast<size_t>(codeWords) * 4u > bin.size())
                return false;
            auto word = [&](size_t index) -> uint32_t
            { return index < codeWords ? u32(&bin[code + index * 4u]) : 0xFFFFFFFFu; };
            for (size_t o = 0; o < codeWords && word(o) != 0xFFFFFFFFu; o += 2)
            {
                const uint32_t object = word(o), objectAt = word(o + 1);
                for (size_t st = objectAt + 2u; st < codeWords && word(st) != 0xFFFFFFFFu; st += 2)
                {
                    const uint32_t state = word(st), stateAt = objectAt + word(st + 1);
                    for (size_t ev = stateAt; ev < codeWords && word(ev) != 0xFFFFFFFFu; ev += 2)
                    {
                        const uint32_t event = word(ev);
                        std::vector<std::pair<uint32_t, uint32_t>> ins;
                        for (size_t at = stateAt + word(ev + 1); at + 1u < codeWords; at += 2)
                        {
                            ins.emplace_back(word(at), word(at + 1));
                            if (word(at) == kEnd)
                                break;
                        }
                        for (size_t i = 3; i < ins.size(); ++i)
                            if (ins[i].first == kCallNative && ins[i].second == kSetObjectVisible && ins[i - 1].first == kPush &&
                                ins[i - 2].first == kPush && ins[i - 3].first == kPush)
                                out.push_back({object, state, event, ins[i - 1].second, ins[i - 2].second, ins[i - 3].second});
                    }
                }
            }
            return true;
        }
    }

    std::vector<VisibilityRule> borrowedVisibility(const std::vector<uint8_t> &from, const std::vector<uint8_t> &into)
    {
        std::vector<VisibilityRule> rules;
        std::vector<Call> theirs, ours;
        if (!visibilityCalls(from, theirs) || !visibilityCalls(into, ours))
            return rules;
        std::set<uint32_t> handled;
        for (const Call &c : ours)
            handled.insert(c.target);
        // The objects and states the script has (a rule for a state it does
        // not have would never run).
        std::set<std::pair<uint32_t, uint32_t>> states;
        {
            const size_t code = 0x14u + static_cast<size_t>(u32(&into[16])) + u32(&into[12]) + u32(&into[8]) * 4u;
            const uint32_t codeWords = u32(&into[4]);
            auto word = [&](size_t index) -> uint32_t
            { return index < codeWords ? u32(&into[code + index * 4u]) : 0xFFFFFFFFu; };
            for (size_t o = 0; o < codeWords && word(o) != 0xFFFFFFFFu; o += 2)
                for (size_t st = word(o + 1) + 2u; st < codeWords && word(st) != 0xFFFFFFFFu; st += 2)
                    states.insert({word(o), word(st)});
        }
        for (const Call &c : theirs)
            if (c.event == kEnterEvent && !handled.count(c.target) && states.count({c.object, c.state}))
                rules.push_back({c.object, c.state, c.target, c.visible != 0u, c.keep != 0u});
        return rules;
    }

    std::string writeVisibilityRules(const std::vector<VisibilityRule> &rules)
    {
        std::string text;
        for (const VisibilityRule &r : rules)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "%08x %08x %08x %d %d\n", r.object, r.state, r.target, r.visible ? 1 : 0,
                          r.keep ? 1 : 0);
            text += line;
        }
        return text;
    }

    std::vector<VisibilityRule> readVisibilityRules(const std::string &text)
    {
        std::vector<VisibilityRule> rules;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line))
        {
            unsigned object = 0, state = 0, target = 0;
            int visible = 0, keep = 0;
            if (std::sscanf(line.c_str(), "%x %x %x %d %d", &object, &state, &target, &visible, &keep) == 5)
                rules.push_back({object, state, target, visible != 0, keep != 0});
        }
        return rules;
    }

    bool build(const Reader &gc, Made &made, std::ostream &log, std::string *error)
    {
        made.files.clear();
        // The disc's names (as the exporter wrote them) and the game's.
        struct File
        {
            const char *disc;
            const char *game;
        };
        constexpr File kFiles[] = {
            {"tut.ZIT", "TUT.ZIT"}, {"tut.GEO", "TUT.GEO"}, {"tut.INS", "TUT.INS"},
            {"tut.TXR", "TUT.TXR"}, {"tut.DAT", "TUT.DAT"}, {"TUT.bin", "TUT.BIN"},
            {"TUTT.bin", "TUTT.BIN"}, {"motions.zal", "MOTIONS.ZAL"},
        };
        for (const File &f : kFiles)
        {
            std::vector<uint8_t> bytes;
            if (!gc(f.disc, bytes) || bytes.empty())
                return fail(error, std::string("TUT/Estate/") + f.disc + " is missing");
            made.files[f.game] = std::move(bytes);
        }
        // The level's ZIT names its four files; they must be the ones above.
        const std::vector<uint8_t> &zit = made.files["TUT.ZIT"];
        for (const char *name : {"tut.TXR", "tut.GEO", "tut.INS", "tut.DAT"})
        {
            const std::string want(name);
            bool found = false;
            for (size_t i = 0; !found && i + want.size() <= zit.size(); ++i)
                found = upper(std::string(reinterpret_cast<const char *>(&zit[i]), want.size())) == upper(want);
            if (!found)
                return fail(error, "TUT.ZIT does not name " + want);
        }
        std::vector<uint8_t> &txr = made.files["TUT.TXR"];
        TextureReport report;
        if (!palettizeLibrary(txr, &report, error))
            return false;
        log << "[estate] TUT.TXR: " << report.textures << " textures, " << report.pictures << " pictures, "
            << report.converted << " made 8-bit";
        for (const std::string &name : report.convertedNames)
            log << " " << name;
        log << std::endl;
        const std::vector<TextureKey> keys = textureKeys(txr);
        if (keys.size() != report.textures)
            return fail(error, "TUT.TXR: " + std::to_string(keys.size()) + " of " + std::to_string(report.textures) +
                                   " textures have an 8- or 4-bit level 0");
        const std::string text = writeTextureKeys(keys);
        made.files["TEXTURES.TXT"].assign(text.begin(), text.end());
        // The free skate script's missing show and hide calls, from the
        // career script's same objects and states (VISIBILITY.TXT).
        const std::vector<VisibilityRule> rules = borrowedVisibility(made.files["TUT.BIN"], made.files["TUTT.BIN"]);
        const std::string visibility = writeVisibilityRules(rules);
        made.files["VISIBILITY.TXT"].assign(visibility.begin(), visibility.end());
        log << "[estate] VISIBILITY.TXT: " << rules.size()
            << " show/hide calls of TUT.BIN's for objects TUTT.BIN never shows or hides" << std::endl;
        // Its level-select icon, from a picture of the level.
        Picture picture;
        PictureReport drawn;
        std::string why;
        if (renderPicture(made.files["TUT.GEO"], made.files["TUT.INS"], txr, 256, picture, &drawn, &why))
        {
            made.files["ICONS.UFNT"] = iconFont(picture, kIconUnselected, kIconSelected, kIconPage);
            Picture still;
            if (renderPicture(made.files["TUT.GEO"], made.files["TUT.INS"], txr, 512, 256, 4.0f / 3.0f, still, nullptr, &why))
                made.files["STILL.ULIB"] = stillLibrary(still, kStillName);
            log << "[estate] ICONS.UFNT: the icon from a picture of " << drawn.instances << " instances, "
                << drawn.drawnTriangles << " of " << drawn.triangles << " triangles on screen (" << drawn.texturedTriangles
                << " textured, " << drawn.textures << " textures)" << std::endl;
        }
        else // the game shows the Tutorial's cap then
            log << "[estate] no icon: the level's picture could not be drawn (" << why << ")" << std::endl;
        uint64_t bytes = 0;
        for (const auto &[name, contents] : made.files)
            bytes += contents.size();
        log << "[estate] " << made.files.size() << " files, " << bytes << " bytes" << std::endl;
        return true;
    }
}
