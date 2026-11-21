#include "runtime/zaxis_zoo.h"

#include "runtime/texture_replace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_set>

namespace zx::zaxis::zoo
{
    namespace
    {
        uint32_t u32(const std::vector<uint8_t> &d, size_t at)
        {
            uint32_t v = 0;
            std::memcpy(&v, &d[at], 4);
            return v;
        }

        // A geometry file's objects (TGEO) by name (TGHD +0x1C).
        struct GeoObject
        {
            std::string name;
            size_t at = 0, size = 0; // the chunk, header included
        };
        std::vector<GeoObject> geoObjects(const std::vector<uint8_t> &geo)
        {
            std::vector<GeoObject> out;
            for (size_t at = 8u; at + 8u <= geo.size();)
            {
                const uint32_t size = u32(geo, at + 4);
                if (at + 8u + size > geo.size())
                    break;
                if (std::memcmp(&geo[at], "TGEO", 4) == 0 && size >= 8u + 0x40u && std::memcmp(&geo[at + 8], "TGHD", 4) == 0)
                {
                    const char *raw = reinterpret_cast<const char *>(&geo[at + 16u + 0x1Cu]);
                    out.push_back({std::string(raw, strnlen(raw, 0x40u - 0x1Cu)), at, 8u + size});
                }
                at += 8u + size;
            }
            return out;
        }

        // The texture ids an object's materials use.
        void materialTextures(const uint8_t *object, size_t size, std::unordered_set<uint32_t> &ids)
        {
            for (size_t k = 0; k + 8u + 0x30u <= size; ++k)
            {
                if (std::memcmp(object + k, "MATI", 4) != 0)
                    continue;
                uint32_t mSize = 0, layers = 0;
                std::memcpy(&mSize, object + k + 4, 4);
                std::memcpy(&layers, object + k + 8 + 0x2C, 4);
                for (uint32_t l = 0; l < std::max<uint32_t>(1u, layers) && 0x30u + 28u * (l + 1u) <= mSize &&
                                     k + 8u + 0x30u + 28u * (l + 1u) <= size;
                     ++l)
                {
                    uint32_t id = 0;
                    std::memcpy(&id, object + k + 8 + 0x30 + 28 * l + 0x10, 4);
                    if (id)
                        ids.insert(id);
                }
            }
        }

        double secondsSince(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        }
    }

    // Every PS2 split-screen level's geometry has the same 38 prop
    // definitions the single-player levels lack (or some of): Egg Hunt's
    // eggs (func_1C64B8 picks the level's: the Movie Lot's beaker, the
    // Museum's dinosaur egg, ... and for index 8 the big egg,
    // PropDef_EggBig), Twenty One's cards, the other levels' eggs, icons,
    // power juice and twister letters.
    const char *const kSplitScreenProps[38] = {
        "PropDef_Balloon", "PropDef_Beaker", "PropDef_BubbleRXL", "PropDef_CandyCane", "PropDef_CardAce",
        "PropDef_CardEight", "PropDef_CardFive", "PropDef_CardFour", "PropDef_CardJack", "PropDef_CardKing",
        "PropDef_CardNine", "PropDef_CardQueen", "PropDef_CardSeven", "PropDef_CardSix", "PropDef_CardTen",
        "PropDef_CardThree", "PropDef_CardTwo", "PropDef_Debris", "PropDef_DinoEgg", "PropDef_EggBig",
        "PropDef_EggSmall", "PropDef_IconBreak", "PropDef_IconEditor", "PropDef_IconGrab", "PropDef_IconGrind",
        "PropDef_IconPlant", "PropDef_IconRide", "PropDef_IconScore", "PropDef_IconTime", "PropDef_IconTrick",
        "PropDef_PowerJuiceExtend", "PropDef_PowerJuiceHalffill", "PropDef_PowerJuiceRefill", "PropDef_TwisterB",
        "PropDef_TwisterC", "PropDef_TwisterG", "PropDef_TwisterM", "PropDef_TwisterO",
    };

    size_t hideObjects(std::vector<uint8_t> &geo, const std::string &list)
    {
        std::vector<std::string> names;
        for (size_t start = 0; start <= list.size();)
        {
            size_t end = list.find(',', start);
            if (end == std::string::npos)
                end = list.size();
            if (end > start)
                names.push_back(list.substr(start, end - start));
            start = end + 1;
        }
        size_t at = 8u, hidden = 0;
        while (at + 8u <= geo.size())
        {
            const uint32_t size = u32(geo, at + 4);
            if (std::memcmp(&geo[at], "TGEO", 4) == 0 && at + 8u + 64u <= geo.size())
            {
                const char *raw = reinterpret_cast<const char *>(&geo[at + 16u + 36u]);
                const std::string name(raw, strnlen(raw, 28));
                bool match = false;
                for (const std::string &n : names)
                    match = match || name == n;
                for (size_t sub = at + 8u; match && sub + 8u <= at + 8u + size;)
                {
                    const uint32_t subSize = u32(geo, sub + 4);
                    if (std::memcmp(&geo[sub], "TGPT", 4) == 0)
                    {
                        const uint32_t zero = 0;
                        std::memcpy(&geo[sub + 16u + 8u], &zero, 4); // TGPH group count
                        ++hidden;
                    }
                    sub += 8u + subSize;
                }
            }
            at += 8u + size;
        }
        return hidden;
    }

    // The Zoo's geometry (single player, as on the Xbox) has none of the
    // props, so Egg Hunt had nothing to hide. Split screen's ZOO.GEO and
    // ZOO.TXR get them from the PS2 disc's split-screen Movie Lot (MP\MOVIE:
    // the objects as they are, and the textures their materials use).
    size_t addSplitScreenProps(std::vector<uint8_t> &file, bool geometry, const std::vector<uint8_t> &levelGeo,
                               const std::vector<uint8_t> &donorGeo, const std::vector<uint8_t> *donorTxr)
    {
        if ((!geometry && !donorTxr) || file.size() < 8u || std::memcmp(file.data(), "FORM", 4) != 0)
            return 0;
        // The props the Zoo lacks (its own geometry, for both files).
        std::unordered_set<std::string> have;
        for (const GeoObject &o : geoObjects(levelGeo))
            have.insert(o.name);
        std::vector<GeoObject> props;
        std::unordered_set<uint32_t> ids;
        for (const GeoObject &o : geoObjects(donorGeo))
            for (const char *wanted : kSplitScreenProps)
                if (o.name == wanted && !have.count(o.name))
                {
                    props.push_back(o);
                    materialTextures(donorGeo.data() + o.at, o.size, ids);
                }
        size_t added = 0;
        if (geometry)
        {
            for (const GeoObject &o : props)
                file.insert(file.end(), donorGeo.begin() + static_cast<std::ptrdiff_t>(o.at),
                            donorGeo.begin() + static_cast<std::ptrdiff_t>(o.at + o.size));
            added = props.size();
        }
        else
        {
            // TXRC entries: TXRH id at +16.
            std::unordered_set<uint32_t> present;
            for (size_t at = 8u; at + 20u <= file.size();)
            {
                const uint32_t size = u32(file, at + 4), id = u32(file, at + 16);
                present.insert(id);
                at += 8u + size;
            }
            const std::vector<uint8_t> &txr = *donorTxr;
            for (size_t at = 8u; at + 20u <= txr.size();)
            {
                const uint32_t size = u32(txr, at + 4), id = u32(txr, at + 16);
                if (at + 8u + size > txr.size())
                    break;
                if (ids.count(id) && !present.count(id))
                {
                    file.insert(file.end(), txr.begin() + static_cast<std::ptrdiff_t>(at),
                                txr.begin() + static_cast<std::ptrdiff_t>(at + 8u + size));
                    ++added;
                }
                at += 8u + size;
            }
        }
        const uint32_t form = static_cast<uint32_t>(file.size() - 8u);
        std::memcpy(file.data() + 4, &form, 4);
        return added;
    }

    // The split-screen level's data points (ZOO.DAT: a FORM of "SDPT"
    // records, each a 0x2C-byte header (class hash's low 16 bits at +4, the
    // point's name hash at +8, id, position at +0x10) and the class's own
    // fields, then ZPVS and ZLIT). The level's with a "playertwo" spawn point
    // (class "spawn"), as the PS2's split-screen levels have: the Zoo has
    // only "playerone" (where player one starts in split screen; player two
    // started in the pond's fountain, at the origin). Player two starts at
    // the free skate start (the first "respawn"), facing as player one does,
    // as the Movie Lot's pair stands. Added after the last point, so no
    // point's index moves. And the willows once each: the level has two NPCs
    // on each tree, a static one of a shared name ("willow", ids 0-9) and
    // one of its own name that the free skate script makes sway (the Xbox
    // draws both); split screen keeps the swaying one, as the willow is the
    // costliest NPC (868 skinned vertices) and drawn in both views. The
    // other's point's class is cleared (0: no object).
    std::vector<uint8_t> splitScreenData(const std::vector<uint8_t> &level, const Options &options,
                                         SplitScreenDataReport *report)
    {
        constexpr uint16_t kSpawnClass = 0x46BEu; // "spawn"
        constexpr uint32_t kPlayerOne = engineHash("playerone");
        constexpr uint32_t kPlayerTwo = engineHash("playertwo");
        constexpr uint32_t kRespawn = engineHash("respawn");
        constexpr uint16_t kNpcClass = 0x5343u; // "NPC"
        constexpr uint32_t kWillow = engineHash("NPC_Willow");
        static_assert(kPlayerOne == 0x06DA4725u && kPlayerTwo == 0x06DA2CBFu && kRespawn == 0x06A846EEu,
                      "the spawn points' names");
        std::vector<uint8_t> dat = level;
        if (report)
            *report = {};
        if (dat.size() < 8u || std::memcmp(dat.data(), "FORM", 4) != 0)
            return dat;
        size_t playerOne = 0, respawn = 0, lastPoint = 0, twins = 0;
        std::vector<std::array<size_t, 3>> willows; // record, name, how many share it
        bool playerTwo = false;
        for (size_t at = 8u; at + 8u <= dat.size();)
        {
            const uint32_t size = u32(dat, at + 4);
            if (at + 8u + size > dat.size())
                break;
            if (std::memcmp(&dat[at], "SDPT", 4) == 0 && size >= 0x2Cu)
            {
                const size_t p = at + 8u;
                const uint16_t kind = static_cast<uint16_t>(u32(dat, p + 4));
                const uint32_t name = u32(dat, p + 8);
                if (kind == kSpawnClass && size == 0x30u)
                {
                    playerOne = name == kPlayerOne && !playerOne ? at : playerOne;
                    playerTwo = playerTwo || name == kPlayerTwo;
                    respawn = name == kRespawn && u32(dat, p + 12) == 0u && !respawn ? at : respawn;
                }
                if (kind == kNpcClass && size >= 0x30u && u32(dat, p + 0x2C) == kWillow)
                    willows.push_back({p, name, 0u});
                lastPoint = at + 8u + size;
            }
            at += 8u + size;
        }
        for (auto &w : willows)
            for (const auto &other : willows)
                w[2] += other[1] == w[1] ? 1u : 0u;
        for (const auto &w : willows)
        {
            // A shared name, and a willow of its own name on its spot.
            const size_t p = w[0];
            const bool twin = std::any_of(willows.begin(), willows.end(), [&](const std::array<size_t, 3> &other)
                                          { return other[2] == 1u && std::memcmp(&dat[other[0] + 0x10u], &dat[p + 0x10u], 12) == 0; });
            if (!options.keepWillowTwins && w[2] > 1u && twin)
            {
                const uint16_t none = 0u;
                std::memcpy(&dat[p + 4], &none, 2);
                ++twins;
            }
        }
        // Egg Hunt's eggs (and Twenty One's cards) appear at the level's
        // pickup points of type 2 (class "bS" 0x5362, param byte +0x44;
        // func_1C5D40 swaps the item for the game's: index 8's
        // PropDef_EggBig). The Zoo has none (its 14 "bS" points are its door
        // triggers, type 0xFF): one on the ground below each respawn point
        // that is not a start (the level's own safe spots, spread over it:
        // the east boulevard, the monkey pit, the west lawn, the aquarium
        // floor). Pickup points sit on the ground (the box reaches 3.05 up),
        // respawn points 4 to 10 above it: the ground heights below them, as
        // a skater dropped there lands (measured in the game, x and z
        // unchanged; a respawn point not listed gets no egg). Added after
        // the last point.
        struct Ground
        {
            float x, z, y;
        };
        static constexpr Ground kGroundBelowRespawns[] = {
            {162.3f, -3.0f, 4.61f},  {-180.5f, -33.8f, 13.84f}, {-145.7f, 127.8f, 1.53f},
            {79.1f, 297.2f, 12.30f}, {-145.2f, 278.1f, 12.30f}, {-24.4f, 278.1f, 12.30f},
            {165.0f, 195.2f, 4.61f}, {152.4f, 64.2f, 4.61f},
        };
        std::vector<std::array<float, 3>> spots;
        std::array<float, 3> starts[2] = {};
        size_t startCount = 0;
        for (size_t at = 8u; at + 8u <= dat.size();)
        {
            const uint32_t size = u32(dat, at + 4);
            if (at + 8u + size > dat.size())
                break;
            if (std::memcmp(&dat[at], "SDPT", 4) == 0 && size == 0x30u &&
                static_cast<uint16_t>(u32(dat, at + 12)) == kSpawnClass)
            {
                std::array<float, 3> pos{};
                std::memcpy(pos.data(), &dat[at + 8u + 0x10u], 12);
                const uint32_t name = u32(dat, at + 16);
                if ((name == kPlayerOne || at == respawn) && startCount < 2u)
                    starts[startCount++] = pos;
                else if (name == kRespawn)
                    spots.push_back(pos);
            }
            at += 8u + size;
        }
        auto near = [](const std::array<float, 3> &a, const std::array<float, 3> &b, float d)
        { return (a[0] - b[0]) * (a[0] - b[0]) + (a[2] - b[2]) * (a[2] - b[2]) < d * d; };
        std::vector<std::array<float, 3>> eggs;
        for (const auto &spot : spots)
        {
            bool skip = false;
            for (size_t k = 0; k < startCount; ++k)
                skip = skip || near(spot, starts[k], 20.0f);
            for (const auto &egg : eggs)
                skip = skip || near(spot, egg, 10.0f);
            const Ground *ground = nullptr;
            for (const Ground &g : kGroundBelowRespawns)
                if (std::fabs(g.x - spot[0]) < 1.0f && std::fabs(g.z - spot[2]) < 1.0f)
                    ground = &g;
            if (!skip && ground)
                eggs.push_back({spot[0], ground->y, spot[2]});
        }
        if (!options.splitScreenEggs.empty())
        {
            eggs.clear();
            const std::string &text = options.splitScreenEggs;
            for (size_t start = 0; start < text.size();)
            {
                size_t end = text.find(';', start);
                if (end == std::string::npos)
                    end = text.size();
                std::array<float, 3> p{};
                if (std::sscanf(text.substr(start, end - start).c_str(), "%f,%f,%f", &p[0], &p[1], &p[2]) == 3)
                    eggs.push_back(p);
                start = end + 1;
            }
        }
        std::vector<uint8_t> pickups;
        for (size_t i = 0; i < eggs.size(); ++i)
        {
            // As the split-screen Movie Lot's type 2 points.
            const float box[6] = {-1.52f, -3.05f, -1.52f, 1.52f, 0.0f, 1.52f};
            const float pos[3] = {eggs[i][0], eggs[i][1], eggs[i][2]};
            const float turn[4] = {0.0f, 0.0f, 0.0f, -1.0f};
            const uint32_t header[4] = {0x2Cu, 0x5362u, engineHash("zooEggSpot") + static_cast<uint32_t>(i), 0u};
            const uint32_t type[2] = {2u, 0u};
            const size_t at = pickups.size();
            pickups.resize(at + 8u + 76u, 0u);
            std::memcpy(&pickups[at], "SDPT", 4);
            const uint32_t size = 76u;
            std::memcpy(&pickups[at + 4], &size, 4);
            std::memcpy(&pickups[at + 8], header, sizeof(header));
            std::memcpy(&pickups[at + 8 + 0x10], pos, sizeof(pos));
            std::memcpy(&pickups[at + 8 + 0x1C], turn, sizeof(turn));
            std::memcpy(&pickups[at + 8 + 0x2C], box, sizeof(box));
            std::memcpy(&pickups[at + 8 + 0x44], type, sizeof(type));
        }
        dat.insert(dat.begin() + static_cast<std::ptrdiff_t>(lastPoint), pickups.begin(), pickups.end());
        const bool addTwo = playerOne && respawn && !playerTwo;
        if (addTwo)
        {
            std::vector<uint8_t> point(dat.begin() + static_cast<std::ptrdiff_t>(playerOne),
                                       dat.begin() + static_cast<std::ptrdiff_t>(playerOne + 8u + 0x30u));
            std::memcpy(&point[8u + 8u], &kPlayerTwo, 4);
            std::memcpy(&point[8u + 0x10u], &dat[respawn + 8u + 0x10u], 12);
            dat.insert(dat.begin() + static_cast<std::ptrdiff_t>(lastPoint), point.begin(), point.end());
            const uint32_t form = static_cast<uint32_t>(dat.size() - 8u);
            std::memcpy(&dat[4], &form, 4);
        }
        if (report)
        {
            report->playerTwo = addTwo;
            report->eggs = eggs.size();
            report->twins = twins;
        }
        return dat;
    }

    // A level script (.BIN, compiled): a header (version float, the code
    // size in words, ..., the string table's size at +0x10), the strings,
    // then the code: an object table, then per object its states and events
    // and their code, where 0x0A pushes a value and 0x1E calls a native by
    // its name hash. ZOOT.BIN calls playCameraAnim once (the intro flight,
    // func_1B5950): it plays the camera animation on player one's camera
    // and sets the HUD's camera bits, which split screen never clears (no
    // timer, no HUD). The call made one of getTheVisibleTimer (func_1B7658:
    // reads a value, returns 1 as playCameraAnim does).
    size_t splitScreenScript(std::vector<uint8_t> &bin)
    {
        constexpr uint32_t kCall = 0x1Eu;
        constexpr uint32_t kCameraAnimation = engineHash("playCameraAnim");
        constexpr uint32_t kHarmless = engineHash("getTheVisibleTimer");
        if (bin.size() < 0x14u)
            return 0;
        const uint32_t strings = u32(bin, 0x10);
        size_t calls = 0;
        for (size_t at = 0x14u + strings; at + 8u <= bin.size(); at += 4u)
        {
            if (u32(bin, at) == kCall && u32(bin, at + 4u) == kCameraAnimation)
            {
                std::memcpy(&bin[at + 4u], &kHarmless, 4);
                ++calls;
            }
        }
        return calls;
    }

    std::vector<uint8_t> picturePack(const std::vector<xbox::HostTexture> &host)
    {
        namespace tr = zx::texture_replace;
        std::vector<tr::Entry> entries;
        entries.reserve(host.size());
        for (const xbox::HostTexture &h : host)
        {
            auto picture = std::make_shared<tr::Picture>();
            picture->width = static_cast<uint32_t>(h.pictureWidth);
            picture->height = static_cast<uint32_t>(h.pictureHeight);
            picture->mips = static_cast<uint32_t>(h.mips);
            picture->format = h.dxt3 ? tr::Format::BC2 : tr::Format::RGBA8;
            picture->name = h.name;
            picture->data = h.data;
            tr::Entry e;
            e.width = static_cast<uint32_t>(h.width);
            e.height = static_cast<uint32_t>(h.height);
            e.indexHash = h.indexHash;
            e.picture = std::move(picture);
            entries.push_back(std::move(e));
        }
        return tr::writePack(entries, 8u);
    }

    namespace
    {
        // A texture library converted, with its report lines (as the game
        // override logged them when it converted at run time).
        bool convertTextures(const std::vector<uint8_t> &xboxTxr, int maxSide, unsigned threads,
                             std::vector<uint8_t> &out, std::vector<xbox::TextureReport> &report,
                             std::vector<xbox::HostTexture> &host, std::ostream &log, std::string &error,
                             size_t budget = 0)
        {
            const auto start = std::chrono::steady_clock::now();
            if (!xbox::convertTextureLibrary(xboxTxr, out, &report, &error, threads, maxSide, &host, budget))
                return false;
            size_t alpha = 0, failed = 0, shrunk = 0, budgeted = 0, texels = 0;
            for (const auto &texture : report)
            {
                alpha += texture.alpha ? 1u : 0u;
                shrunk += texture.width < texture.sourceWidth || texture.height < texture.sourceHeight ? 1u : 0u;
                budgeted += std::max(texture.width, texture.height) < std::min(maxSide, std::max(texture.sourceWidth,
                                                                                                 texture.sourceHeight))
                                ? 1u
                                : 0u;
                if (texture.error.empty())
                    texels += static_cast<size_t>(texture.width) * static_cast<size_t>(texture.height);
                if (!texture.error.empty())
                {
                    ++failed;
                    log << "[zoo] texture " << texture.name << " left out: " << texture.error << "\n";
                }
            }
            log << "[zoo] zoo.TXR: " << report.size() << " Xbox textures as 8-bit BMPs (" << shrunk << " halved to "
                << maxSide << " a side, " << alpha << " with alpha, " << failed << " left out), " << out.size() / 1024u
                << " KB, " << secondsSince(start) << " s\n";
            if (budget)
                log << "[zoo] zoo.TXR: texels " << texels / 1024u << " KB of the " << budget / 1024u << " KB budget ("
                    << budgeted << " textures smaller for it)\n";
            return true;
        }

        size_t largerPictures(const std::vector<xbox::HostTexture> &host)
        {
            size_t larger = 0;
            for (const xbox::HostTexture &h : host)
                larger += h.pictureWidth > h.width || h.pictureHeight > h.height ? 1u : 0u;
            return larger;
        }

        std::string upper(std::string text)
        {
            for (char &c : text)
                if (c >= 'a' && c <= 'z')
                    c = static_cast<char>(c - 'a' + 'A');
            return text;
        }
    }

    bool build(const Reader &xbox, const Reader &ps2, const Options &options, Made &out, std::ostream &log,
               std::string *error)
    {
        auto fail = [&](const std::string &why)
        {
            if (error)
                *error = why;
            return false;
        };
        const std::string level(kLevelFolder), split(kSplitScreenFolder), npc(kNpcFolder);
        auto pack = [&](const std::string &path, const std::vector<xbox::HostTexture> &host)
        {
            out.files[std::string(kPictureFolder) + path + ".xtp"] = picturePack(host);
            log << "[zoo] " << path << ": " << host.size() << " Xbox originals for the GPU renderer ("
                << largerPictures(host) << " larger than drawn by the PS2 renderer)\n";
        };

        // The level's textures, then its geometry: the materials follow
        // which textures cut out by their alpha (the Zoo's exporter wrote
        // no render state).
        std::vector<uint8_t> xboxTxr;
        if (!xbox("levels/ZOO/zoo.TXR", xboxTxr))
            return fail("no levels/ZOO/zoo.TXR in the Xbox files");
        std::unordered_set<uint32_t> alphaTextures;
        {
            std::vector<uint8_t> txr;
            std::vector<xbox::TextureReport> report;
            std::vector<xbox::HostTexture> host;
            std::string why;
            if (!convertTextures(xboxTxr, options.levelTextureSide, options.threads, txr, report, host, log, why,
                                 options.levelTextureBudget))
                return fail("zoo.TXR: " + why);
            for (const auto &texture : report)
                if (texture.cutout)
                    alphaTextures.insert(texture.id);
            out.files[level + "ZOO.TXR"] = std::move(txr);
            pack(level + "ZOO.TXR", host);
        }
        std::vector<uint8_t> geo;
        if (!xbox("levels/ZOO/zoo.GEO", geo))
            return fail("no levels/ZOO/zoo.GEO in the Xbox files");
        {
            const auto start = std::chrono::steady_clock::now();
            xbox::GeometryReport report;
            if (!xbox::convertGeometry(geo, &report, &alphaTextures, options.groupEntries))
                return fail("zoo.GEO: not a geometry file");
            log << "[zoo] zoo.GEO: " << report.objects << " objects' strips cut into " << report.groupsOut
                << " PS2 groups (from " << report.groupsIn << "), " << report.materials << " materials in the PS2 layout, "
                << geo.size() / 1024u << " KB, " << secondsSince(start) << " s\n";
            if (!options.hide.empty())
                log << "[zoo] hidden: " << hideObjects(geo, options.hide) << " objects\n";
            out.files[level + "ZOO.GEO"] = geo;
        }
        for (const LevelFile &file : kLevelFilesAsTheyAre)
        {
            std::vector<uint8_t> bytes;
            if (!xbox(std::string("levels/ZOO/") + file.xbox, bytes))
                return fail(std::string("no levels/ZOO/") + file.xbox + " in the Xbox files");
            out.files[level + file.ps2] = std::move(bytes);
        }

        // Split screen (MP\ZOO): the level with smaller textures, the games'
        // props, its own data points and script.
        std::vector<uint8_t> donorGeo, donorTxr;
        const bool donor = ps2("ASSETS/LEVELS/MP/MOVIE/MOVIE.GEO", donorGeo) &&
                           ps2("ASSETS/LEVELS/MP/MOVIE/MOVIE.TXR", donorTxr);
        if (!donor)
            log << "[zoo] split screen: no props from MP\\MOVIE (Egg Hunt without eggs)\n";
        {
            std::vector<uint8_t> txr;
            std::vector<xbox::TextureReport> report;
            std::vector<xbox::HostTexture> host;
            std::string why;
            if (!convertTextures(xboxTxr, options.splitScreenTextureSide, options.threads, txr, report, host, log, why))
                return fail("zoo.TXR (split screen): " + why);
            pack(split + "ZOO.TXR", host);
            if (donor)
                log << "[zoo] split screen's ZOO.TXR: " << addSplitScreenProps(txr, false, geo, donorGeo, &donorTxr)
                    << " of their textures from MP\\MOVIE\n";
            out.files[split + "ZOO.TXR"] = std::move(txr);
        }
        {
            std::vector<uint8_t> splitGeo = geo;
            if (donor)
                log << "[zoo] split screen's ZOO.GEO: " << addSplitScreenProps(splitGeo, true, geo, donorGeo, nullptr)
                    << " game props from MP\\MOVIE\n";
            out.files[split + "ZOO.GEO"] = std::move(splitGeo);
        }
        {
            SplitScreenDataReport report;
            out.files[split + "ZOO.DAT"] = splitScreenData(out.files[level + "ZOO.DAT"], options, &report);
            log << "[zoo] split screen's ZOO.DAT: " << (report.playerTwo ? "player two's start, " : "") << report.eggs
                << " egg spots, " << report.twins << " static willow NPCs under a swaying one left out\n";
        }
        {
            std::vector<uint8_t> script = out.files[level + "ZOOT.BIN"];
            const size_t calls = splitScreenScript(script);
            log << "[zoo] split screen's ZOO.BIN: the free skate script, " << calls << " camera animation call"
                << (calls == 1u ? "" : "s") << " left out\n";
            out.files[split + "ZOO.BIN"] = std::move(script);
        }

        // The ambient NPCs' models.
        for (const NpcModel &model : kNpcModels)
        {
            std::vector<uint8_t> bytes;
            if (!xbox(std::string("NPC/zoo/") + model.file, bytes))
                return fail(std::string("no NPC/zoo/") + model.file + " in the Xbox files");
            xbox::ModelReport report;
            std::vector<xbox::HostTexture> host;
            std::string why;
            if (!xbox::convertModel(bytes, &report, &why, options.levelTextureSide, &host))
                return fail(std::string("NPC model ") + model.file + ": " + why);
            log << "[zoo] NPC model " << model.file << ": " << report.textures.size() << " textures, "
                << report.geometry.groupsIn << " strips cut into " << report.geometry.groupsOut << " groups, "
                << report.geometry.materials << " materials, " << bytes.size() / 1024u << " KB\n";
            const std::string path = npc + upper(model.file);
            out.files[path] = std::move(bytes);
            pack(path, host);
        }
        return true;
    }
}
