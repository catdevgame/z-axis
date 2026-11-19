// Aggressive Inline (SLUS-20327): the Zoo, the level cut from the PS2
// release (the Xbox release has it, free skate only).
//
// The game still knows the level: index 8 of its level tables (scene and
// folder names 0x24B8B0 / 0x24BC30, "ZOO"; split screen 0x24BA70 /
// 0x24BDF0, "MPZOO" / "MP\ZOO"), "Zoo" in the level-select name list
// 0x265440 (func_1A8258 turns the menu's SelectedLevel into the index), a
// giraffe icon ('s', selected 'S') in the level-select font, SFXLEV08.ZSD.
// What is missing is the menu entry and, on the PS2 disc, a usable level:
// ASSETS/LEVELS/ZOO is a January 2002 grey box (one untextured material,
// an 8-byte ZOO.TXR, 60-byte scripts) and MP/ZOO an earlier prototype with
// seven textures.
//
// With PS2X_ZOO=1 (the app: Settings > Game > Zoo, settings.ini [extras]
// zoo):
//   - FrontEnd.zsc's free skate (and timed run) level select gains the Zoo
//     between the Museum and the Editor Level, always unlocked;
//   - the files of ASSETS/LEVELS/ZOO come from the Xbox release when the app
//     has them, made into the PS2's at build time (ps2xXboxConvert,
//     runtime/ps2_zaxis_zoo.h; served from PS2X_XBOX_CONVERTED,
//     aggressive_inline_converted.h): textures as 8-bit BMPs, at most 128 a
//     side as in the PS2 levels (the renderer's texture cache garbles a view
//     full of 256s; the GPU renderer draws the Xbox originals, registered
//     here from the build's .xtp when the textures are served); strips cut
//     into the PS2 renderer's 60-entry groups; materials' render state as
//     the PS2 exporter wrote it (the Zoo's has none: the most common PS2
//     state for its blend mode, alpha blending for textures with alpha);
//     the Xbox-only material flags dropped. The rest (objects, rails and
//     spawn points, the free skate script ZOOT.BIN, motions) as it is: the
//     formats are the same. Nothing is converted here. Without them, the
//     PS2 disc's own Zoo: its multiplayer prototype (MP/ZOO, seven
//     textures, flat grey elsewhere; the default) or with PS2X_ZOO_PS2=sp
//     the single-player grey box;
//   - loading screens: the generic ones (ASSETS/LOADING/ZOO is empty on both
//     discs); the level-select preview: the Xbox flythrough, a PSM the
//     workshop's build made of it (Movies/Flythrus/ZOO.PSM) or the Xbox
//     disc's own Bink file the movie player plays (movies/flythrus/zoo.bik,
//     game/host/movies.h setHostMovie), else the still picture the Editor
//     Level uses;
//   - the Zoo's ambient NPCs (Xbox only): kelp, sharks and the willows'
//     swaying foliage (and an octopus no spawn point uses). The level's
//     data points spawn them as NPCs of types npc_KelpA, npc_KelpB,
//     npc_KelpC, npc_shark, npc_octopus, npc_willow; the Xbox executable
//     has six more rows in the NPC type table (0x25ACC0 here, 62 rows) with
//     the level's own animations (zoo_assets.npc.*, in its MOTIONS.ZAL) in
//     every slot, and its SCENE.TXT six more models in the ZOO scene
//     (assets\NPC\zoo\*.ZIT). Here: the rows in extended memory, found by
//     the type lookup func_1E2B10 and given their animations by
//     func_1E3150 (both wrapped; missing zoo_assets animations are not
//     reported outside the Zoo, as on the Xbox), SCENE.TXT patched, and
//     the models made at build time (runtime/ps2_zaxis_xbox.h convertModel
//     in ps2xXboxConvert). The
//     NPC constructor hides NPCs outside career mode but in the Zoo, as the
//     Xbox's does (func_13BD48 wrapped for that one call). The game's own
//     NPC code does the rest (the willows sway by the free skate script);
//   - split screen: the split-screen level select gains the Zoo as a fifth
//     entry; MP\ZOO is the single-player level (the Xbox has none; the PS2
//     disc's prototype without Xbox files) as the build made it: its
//     textures at most 64 a side (both views' textures share the GS texture cache), a
//     "playertwo" start, the free skate script without its intro camera
//     call (doors and willows as in free skate; the call took the HUD and
//     timer away), the split-screen games' props from the PS2's MP\MOVIE
//     (Egg Hunt's egg, Twenty One's cards, ...) and egg spots on the ground
//     below the respawn points, each willow once, the camera FOV of the
//     Movie Lot's split screen (func_18C828 has one for four levels only, 0
//     for the rest), and its NPCs animated whether or not the second view
//     drew them (func_132860 at the animation update);
//   - extended memory (above the PS2's 32 MB): the Zoo's textures go there
//     (PS2Runtime::guestMallocExtended from the texture allocator
//     func_126900 while the level index is 8), and the game's malloc
//     overflows into it once the 32 MB heap is full: the Xbox level's
//     geometry alone is as large as the largest PS2 level's. Allocations
//     that fit in the 32 MB land where they always did.
// Nothing derived from either disc is written anywhere.

#include "zge_sites.h"
#include "aggressive_inline_npcs.h"
#include "aggressive_inline_converted.h"
#include "aggressive_inline_disc.h"
#include "../game/host/movies.h"
#include "ps2_runtime.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_xbox_disc.h"
#include "runtime/ps2_zaxis_ui.h"
#include "runtime/ps2_zaxis_zoo.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace aggressive_inline::zoo
{
    namespace
    {
        namespace gf = ps2x::game_files;
        namespace zx = ps2x::zaxis;
        namespace made_ = aggressive_inline::converted;

        enum class Source
        {
            Converted, // the Xbox release's level, made into the PS2's at build time
            Ps2,       // the PS2 disc's grey box (ASSETS/LEVELS/ZOO as it is)
            Ps2Mp,     // the PS2 disc's multiplayer prototype (ASSETS/LEVELS/MP/ZOO)
        };

        // The level's texture memory (func_126900(texture, bytes): pixels
        // at +0x10, palette at +0x1C; from the 512 KB scratch pool when the
        // texture's flag 2 is set and it has room, else malloc; freed by
        // 0x126940, the pool's free passing blocks not its own to free())
        // and the level index.
        constexpr uint32_t kTextureAlloc = 0x00126900u;
        constexpr uint32_t kLevelIndex = 0x0024C9B4u;
        constexpr uint32_t kZooIndex = 8u;
        struct Zoo
        {
            PS2Runtime::RecompiledFunction textureAlloc = nullptr;
            bool extended = false;

            Source source = Source::Ps2Mp;
            bool flythrough = false;      // the Xbox flythrough
            bool flythroughBink = false;  // as the Xbox disc's Bink file (else a PSM made of it)
            bool earned = false; // the Xbox's unlock: the Museum's eight challenges (else always open)
            std::mutex mutex;
            std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> made; // normalized path -> contents
            std::map<std::string, bool> missing;
            uint32_t extendedInUse = 0; // the extended heap's bytes last shown
        };
        Zoo &level() { return ps2x::session::state<Zoo>(); }

        const std::string kLevelFolder = zx::zoo::kLevelFolder;
        const std::string kNpcFolder = zx::zoo::kNpcFolder;
        const std::string kMpLevelFolder = zx::zoo::kSplitScreenFolder;
        const char kLoadingFolder[] = "ASSETS/LOADING/ZOO/";
        const char kFlythrough[] = "ASSETS/MOVIES/FLYTHRUS/ZOO.PSM";
        const char kXboxFlythroughPsm[] = "Movies/Flythrus/ZOO.PSM"; // (the workshop's build made it)
        const char kXboxFlythroughBink[] = "movies/flythrus/zoo.bik";

        std::shared_ptr<const std::vector<uint8_t>> readGameFile(const std::string &path)
        {
            std::shared_ptr<gf::GameFile> file = openGameFile(path);
            if (!file)
                return nullptr;
            auto bytes = std::make_shared<std::vector<uint8_t>>();
            if (!gf::readAll(*file, *bytes))
                return nullptr;
            return bytes;
        }

        std::shared_ptr<const std::vector<uint8_t>> readXbox(const std::string &path)
        {
            auto bytes = std::make_shared<std::vector<uint8_t>>();
            if (!ps2_xbox_disc::readFile(path, *bytes))
                return nullptr;
            return bytes;
        }

        // The Xbox originals of a made file's converted textures, for the
        // GPU renderer (runtime/ps2_texture_replace.h; display.xbox_textures
        // switches them, as the pack's): one picture per Xbox texture, also
        // when two conversions (the level's and split screen's smaller one)
        // or models key it.
        void registerHostPictures(const std::string &path)
        {
            const made_::Pictures p = made_::registerPictures(path, "zoo:" + path, true);
            if (p.ok)
                std::cout << "[zoo] " << path << ": " << p.count << " Xbox originals for the GPU renderer (" << p.larger
                          << " larger than drawn by the PS2 renderer), " << (p.newBytes >> 10) << " KB new" << std::endl;
        }

        std::shared_ptr<const std::vector<uint8_t>> made(const std::string &path);

        // A file made at build time; a texture library's (or a model's)
        // Xbox originals registered with it.
        std::shared_ptr<const std::vector<uint8_t>> readMade(const std::string &path, bool pictures = false)
        {
            auto bytes = made_::read(path);
            if (bytes && pictures)
                registerHostPictures(path);
            return bytes;
        }

        // ---- the ambient NPCs -------------------------------------------

        // The Xbox release's NPC types the PS2 executable lacks
        // (runtime/ps2_zaxis_zoo.h kNpcModels: the model, its file and its
        // animation in all five slots).
        using NpcType = zx::zoo::NpcModel;
        constexpr const NpcType (&kNpcTypes)[zx::zoo::kNpcModelCount] = zx::zoo::kNpcModels;
        constexpr size_t kNpcTypeCount = zx::zoo::kNpcModelCount;
        struct Npcs
        {
            bool available = false; // the Xbox NPC models are there
        };
        Npcs &npcs() { return ps2x::session::state<Npcs>(); }

        // The split-screen level's script (MP\ZOO\ZOO.BIN): neither disc
        // has one for the Zoo (the PS2's MP\ZOO has an empty one); the build
        // makes it from the free skate script ZOOT.BIN without its intro
        // camera call, so its doors open and its willows sway.
        // PS2X_ZOO_MP_SCRIPT=empty (developers): the empty one; =free:
        // ZOOT.BIN as it is (the intro camera takes the HUD and the timer
        // away: no game).
        std::string mpScriptChoice()
        {
            const char *which = std::getenv("PS2X_ZOO_MP_SCRIPT");
            return which ? std::string(which) : std::string();
        }

        std::shared_ptr<const std::vector<uint8_t>> make(const std::string &path)
        {
            if (path.rfind(kNpcFolder, 0) == 0)
            {
                if (!npcs().available)
                    return nullptr;
                const std::string name = path.substr(kNpcFolder.size());
                for (const NpcType &type : kNpcTypes)
                    if (gf::normalizePath(type.file) == name)
                        return readMade(path, true);
                return nullptr;
            }
            if (path.rfind(kMpLevelFolder, 0) == 0)
            {
                // Split screen (MP\ZOO): the build's split-screen files, else
                // the single-player level's. The PS2 disc's prototype
                // otherwise (its own folder).
                const std::string name = path.substr(kMpLevelFolder.size());
                if (level().source != Source::Converted || name.find('/') != std::string::npos)
                    return nullptr;
                if (name == "ZOO.BIN" && mpScriptChoice() == "empty")
                    return made(kLevelFolder + "ZOO.BIN");
                if (name == "ZOO.BIN" && mpScriptChoice() == "free")
                    return made(kLevelFolder + "ZOOT.BIN");
                // The level's textures (and their Xbox originals) first, as
                // when they were converted here.
                if (name == "ZOO.TXR" || name == "ZOO.GEO")
                    made(kLevelFolder + "ZOO.TXR");
                if (made_::exists(path))
                    return readMade(path, name == "ZOO.TXR");
                return made(kLevelFolder + name);
            }
            if (path.rfind(kLevelFolder, 0) == 0)
            {
                const std::string name = path.substr(kLevelFolder.size());
                if (name.find('/') != std::string::npos)
                    return nullptr;
                switch (level().source)
                {
                case Source::Converted:
                    if (name == "ZOO.GEO")
                        made(kLevelFolder + "ZOO.TXR");
                    return made_::exists(path) ? readMade(path, name == "ZOO.TXR") : nullptr;
                case Source::Ps2Mp:
                    // The free skate script is the grey box's stub (the
                    // prototype has none).
                    if (name == "ZOOT.BIN" || name == "ZOO.BIN")
                        return nullptr;
                    return readGameFile("ASSETS/LEVELS/MP/ZOO/" + name);
                case Source::Ps2:
                    return nullptr;
                }
            }
            if (path.rfind(kLoadingFolder, 0) == 0)
            {
                // ZOO0.RAW .. ZOO3.RAW (shown once the career has unlocked
                // the level's loading screens): the generic ones.
                const std::string name = path.substr(sizeof(kLoadingFolder) - 1u);
                if (name.size() == 8u && name.rfind("ZOO", 0) == 0 && name.compare(4, 4, ".RAW") == 0 &&
                    name[3] >= '0' && name[3] <= '3')
                    return readGameFile(std::string("ASSETS/LOADING/BASIC/BASIC0") + name[3] + ".RAW");
                return nullptr;
            }
            if (path == kFlythrough && level().flythrough)
                return readXbox(level().flythroughBink ? kXboxFlythroughBink : kXboxFlythroughPsm);
            return nullptr;
        }

        // A file made once and kept (g_mutex held).
        std::shared_ptr<const std::vector<uint8_t>> made(const std::string &path)
        {
            if (auto it = level().made.find(path); it != level().made.end())
                return it->second;
            if (level().missing.count(path))
                return nullptr;
            std::shared_ptr<const std::vector<uint8_t>> bytes = make(path);
            if (bytes)
                level().made[path] = bytes;
            else
                level().missing[path] = true;
            return bytes;
        }

        std::shared_ptr<const std::vector<uint8_t>> provide(const std::string &path)
        {
            std::lock_guard<std::mutex> lock(level().mutex);
            return made(path);
        }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        // In the Zoo, textures (8.7 MB of them from the Xbox level, three
        // times a PS2 level's) live in extended memory; everything else
        // stays where the game's heap puts it.
        void textureAllocHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t texture = getRegU32(ctx, 4);
            const uint32_t bytes = getRegU32(ctx, 5);
            (void)texture; // pooled or not: the pool's free hands foreign blocks to free()
            static const bool trace = std::getenv("PS2X_TRACE_ZOO") != nullptr; // every texture allocation
            if (trace)
                std::cout << "[zoo] texture memory " << bytes << " bytes, level " << readWord(rdram, kLevelIndex)
                          << ", texture flags 0x" << std::hex << readWord(rdram, texture) << std::dec << std::endl;
            if (level().extended && bytes >= 0x400u && readWord(rdram, kLevelIndex) == kZooIndex)
            {
                if (const uint32_t address = runtime->guestMallocExtended(bytes, 64u))
                {
                    setReturnU32(ctx, address);
                    ctx->pc = getRegU32(ctx, 31);
                    return;
                }
            }
            level().textureAlloc(rdram, ctx, runtime);
        }

        // The NPC type table (func_1E2B10 looks a type up in its 62 rows of
        // 0x2C bytes: the type hash, then five pairs of an animation name and
        // the animation func_1E3150 found for it at each level load).
        constexpr uint32_t kNpcTypeTable = 0x0025ACC0u;
        constexpr uint32_t kNpcTypeRows = 62u;
        constexpr uint32_t kNpcTypeRowBytes = 0x2Cu;
        constexpr uint32_t kNpcTypeLookup = 0x001E2B10u;
        constexpr uint32_t kNpcAnimations = 0x001E3150u;
        constexpr uint32_t kFindAnimation = 0x001ADA98u; // (name, &animation): nonzero if found
        struct NpcTypes
        {
            PS2Runtime::RecompiledFunction npcAnimations = nullptr;
            uint32_t extraNpcTypes = 0; // the extra rows in guest memory (0: none)
            std::map<uint64_t, unsigned> lookups; // PS2X_TRACE_ZOO_NPCS: by type and caller
            std::unordered_set<uint64_t> traced;  // PS2X_TRACE_ZOO: type and answer pairs shown
        };
        NpcTypes &npcTypes() { return ps2x::session::state<NpcTypes>(); }

        using zx::zoo::engineHash; // the engine's name hash (0x148888)
        static_assert(engineHash("NPC_KelpA") == 0x093FD201u, "the Xbox table's kelp type");
        static_assert(engineHash("npc_willow") == 0x04B945D7u, "the Xbox table's willow type");

        void writeWord(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        // The rows and their strings, once, in extended memory (it survives
        // heap resets): a row is the type and five (name, animation) pairs.
        uint32_t extraNpcTypes(uint8_t *rdram, PS2Runtime *runtime)
        {
            if (npcTypes().extraNpcTypes || !npcs().available)
                return npcTypes().extraNpcTypes;
            uint32_t bytes = kNpcTypeCount * kNpcTypeRowBytes;
            for (const NpcType &type : kNpcTypes)
                bytes += static_cast<uint32_t>(std::strlen(type.animation)) + 1u;
            const uint32_t block = runtime->guestMallocExtended((bytes + 15u) & ~15u, 16u);
            if (!block)
            {
                std::cerr << "[zoo] no room for the NPC types, no ambient NPCs" << std::endl;
                npcs().available = false;
                return 0u;
            }
            uint32_t text = block + kNpcTypeCount * kNpcTypeRowBytes;
            for (size_t i = 0; i < kNpcTypeCount; ++i)
            {
                const uint32_t row = block + static_cast<uint32_t>(i) * kNpcTypeRowBytes;
                writeWord(rdram, row, engineHash(kNpcTypes[i].model));
                for (uint32_t slot = 0; slot < 5u; ++slot)
                {
                    writeWord(rdram, row + 4u + 8u * slot, text);
                    writeWord(rdram, row + 8u + 8u * slot, 0u);
                }
                const size_t length = std::strlen(kNpcTypes[i].animation) + 1u;
                if (uint8_t *p = getMemPtr(rdram, text))
                    std::memcpy(p, kNpcTypes[i].animation, length);
                text += static_cast<uint32_t>(length);
            }
            npcTypes().extraNpcTypes = block;
            return block;
        }

        // func_1E2B10(type): the table's row for an NPC type, or 0; the
        // Xbox's rows after the PS2's.
        void npcTypeLookupHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t type = getRegU32(ctx, 4);
            uint32_t found = 0;
            for (uint32_t i = 0; i < kNpcTypeRows && !found; ++i)
                if (readWord(rdram, kNpcTypeTable + i * kNpcTypeRowBytes) == type)
                    found = kNpcTypeTable + i * kNpcTypeRowBytes;
            if (const uint32_t extra = found ? 0u : extraNpcTypes(rdram, runtime))
                for (uint32_t i = 0; i < kNpcTypeCount && !found; ++i)
                    if (readWord(rdram, extra + i * kNpcTypeRowBytes) == type)
                        found = extra + i * kNpcTypeRowBytes;
            // PS2X_TRACE_ZOO_NPCS=1 (developers): each lookup of an Xbox
            // type, counted per type and caller (the NPCs a level makes).
            static const bool countNpcs = std::getenv("PS2X_TRACE_ZOO_NPCS") != nullptr;
            if (countNpcs && found && npcTypes().extraNpcTypes && found >= npcTypes().extraNpcTypes &&
                found < npcTypes().extraNpcTypes + kNpcTypeCount * kNpcTypeRowBytes)
            {
                const uint32_t ra = getRegU32(ctx, 31); // ra: diagnostics only
                const unsigned n = ++npcTypes().lookups[(uint64_t(type) << 32) | ra];
                const uint8_t *gameType = getConstMemPtr(rdram, 0x0024C9B3u);
                std::cout << "[zoo-npc] " << kNpcTypes[(found - npcTypes().extraNpcTypes) / kNpcTypeRowBytes].model << " ra 0x"
                          << std::hex << ra << std::dec << " n " << n << " level " << readWord(rdram, kLevelIndex)
                          << " gametype " << unsigned(gameType ? *gameType : 0u) << std::endl;
            }
            static const bool trace = std::getenv("PS2X_TRACE_ZOO") != nullptr;
            if (trace)
            {
                if (npcTypes().traced.insert((uint64_t(type) << 32) | found).second)
                    std::cout << "[zoo] NPC type 0x" << std::hex << type << " -> 0x" << found << " from 0x"
                              << getRegU32(ctx, 31) << std::dec << std::endl;
            }
            setReturnU32(ctx, found);
            ctx->pc = getRegU32(ctx, 31);
        }

        // func_1E3150 (each level load): the extra rows' animations, then
        // the PS2's. They are the Zoo's own, so elsewhere none is found
        // (the Xbox reports a missing zoo_assets animation only in the Zoo).
        void npcAnimationsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (const uint32_t extra = extraNpcTypes(rdram, runtime))
            {
                const bool zoo = readWord(rdram, kLevelIndex) == kZooIndex;
                size_t found = 0;
                for (uint32_t i = 0; i < kNpcTypeCount; ++i)
                {
                    const uint32_t row = extra + i * kNpcTypeRowBytes;
                    for (uint32_t slot = 0; slot < 5u; ++slot)
                    {
                        uint32_t ok = 0;
                        writeWord(rdram, row + 8u + 8u * slot, 0u);
                        runtime->callGuestFunction(rdram, ctx, kFindAnimation,
                                                   {readWord(rdram, row + 4u + 8u * slot), row + 8u + 8u * slot},
                                                   getRegU32(ctx, 29), &ok);
                        found += ok ? 1u : 0u;
                        if (!ok && zoo && slot == 0u)
                            std::cerr << "[zoo] the NPC animation " << kNpcTypes[i].animation << " was not found" << std::endl;
                    }
                }
                if (zoo)
                    std::cout << "[zoo] ambient NPC animations: " << found << " of " << kNpcTypeCount * 5u << " found"
                              << std::endl;
            }
            npcTypes().npcAnimations(rdram, ctx, runtime);
        }

        // func_13BD48(object, on) sets or clears an object's flag 0x10
        // (shown). The NPC constructor func_1E3500 hides every NPC outside
        // career mode (game type byte 0x24C9B3 not 1) but the Movie Lot's
        // and Boardwalk's trees (at 0x1E36AC); the Xbox's skips that in the
        // Zoo (default.xbe 0x1A80F: level index 8), so its kelp, sharks and
        // willows show in free skate. The same here: that one call does
        // nothing in the Zoo.
        constexpr uint32_t kShowObject = 0x0013BD48u;
        struct Hooks
        {
            PS2Runtime::RecompiledFunction showObject = nullptr;
            PS2Runtime::RecompiledFunction drawnLastFrame = nullptr;
            PS2Runtime::RecompiledFunction fovGetter = nullptr;
            PS2Runtime::RecompiledFunction careerCheck = nullptr;
        };
        Hooks &hooks() { return ps2x::session::state<Hooks>(); }

        void showObjectHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (readWord(rdram, kLevelIndex) == kZooIndex && npcs::isNpcStartHide(rdram, ctx, runtime))
            {
                ctx->pc = getRegU32(ctx, 31);
                return;
            }
            hooks().showObject(rdram, ctx, runtime);
        }

        // func_132860(model): 1 unless the model was drawn in the frame the
        // renderer last finished (its stamp at model +0x1D4 equals
        // *0x27396C +0x54, and +0x1D8 bit 0). The animation update
        // func_1AD230 (at 0x1AD26C) leaves NPCs that answer 1 where they are:
        // NPCs off screen do not animate. In split screen that test fails
        // for nearly every NPC (383 of 13,490 in a run passed, two models),
        // so the Zoo's NPCs stood still; split screen's levels on the disc
        // have none (their NPC models never get the stamp there). In the
        // Zoo's split screen that call answers 0 for the NPCs within 200 of
        // either player's camera: they animate as in the single-player Zoo
        // (all 81 at once cost the EE 2.5 to 4 ms a frame on the Mac).
        // PS2X_ZOO_MP_NPC_ANIMATION=0 (developers): as the game does.
        constexpr uint32_t kDrawnLastFrame = 0x00132860u;
        // (the update is wrapped: its record while it runs; never known by
        // the call's return address or the caller's registers, which a
        // readable caller does not keep)
        constexpr uint32_t kUpdateAnimator = 0x001AD230u;
        struct AnimatorScope
        {
            PS2Runtime::RecompiledFunction updateAnimator = nullptr;
            uint32_t record = 0;
        };
        AnimatorScope &animatorScope() { return ps2x::session::state<AnimatorScope>(); }
        void updateAnimatorHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            AnimatorScope &a = animatorScope();
            const uint32_t outer = a.record;
            a.record = getRegU32(ctx, 4);
            a.updateAnimator(rdram, ctx, runtime);
            a.record = outer;
        }

        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            const uint32_t bits = readWord(rdram, address);
            float value = 0.0f;
            std::memcpy(&value, &bits, 4);
            return value;
        }

        // Within kNpcAnimationRange (horizontally) of either player's camera
        // (the camera controllers 0x24C9D0 and 0x24CA50, position at +0x10).
        constexpr float kNpcAnimationRange = 200.0f;
        constexpr uint32_t kCameraPositions[2] = {0x0024C9E0u, 0x0024CA60u};

        void drawnLastFrameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool off = [] { const char *v = std::getenv("PS2X_ZOO_MP_NPC_ANIMATION"); return v && v[0] == '0'; }();
            const uint8_t *type = getConstMemPtr(rdram, 0x0024C9B3u);
            if (!off && animatorScope().record && type && *type == 4u && readWord(rdram, kLevelIndex) == kZooIndex)
            {
                // The animation record's object at +8, the object's position at +0x10.
                zge_host::sites::seen("ai.zoo.npc_animation");
                const uint32_t object = readWord(rdram, animatorScope().record + 8u);
                const float x = readFloat(rdram, object + 0x10u), z = readFloat(rdram, object + 0x18u);
                for (uint32_t camera : kCameraPositions)
                {
                    const float dx = x - readFloat(rdram, camera), dz = z - readFloat(rdram, camera + 8u);
                    if (dx * dx + dz * dz < kNpcAnimationRange * kNpcAnimationRange)
                    {
                        setReturnU32(ctx, 0u);
                        ctx->pc = getRegU32(ctx, 31);
                        return;
                    }
                }
            }
            hooks().drawnLastFrame(rdram, ctx, runtime);
        }

        // SCENE.TXT: the ZOO scene's NPC models, as the Xbox's (and the
        // split-screen MPZOO's, which loads the same level).
        bool patchScene(std::vector<uint8_t> &bytes)
        {
            std::string text(bytes.begin(), bytes.end());
            std::string models;
            for (const NpcType &type : kNpcTypes)
                models += std::string("\tMODEL\r\n\t\t{\r\n\t\tNAME\t\t\"") + type.model + "\"\r\n" +
                          "\t\tTYPE\t\t$kModelTypeNPC\r\n\t\tSUBTYPE\t0\r\n\t\tFILE\t\t\"assets\\NPC\\zoo\\" +
                          type.file + "\"\r\n\t\t}\r\n";
            size_t patched = 0;
            for (const char *scene : {"\"ZOO\"", "\"MPZOO\""})
            {
                const size_t name = text.find(std::string("NAME\t") + scene);
                const size_t close = name == std::string::npos ? name : text.find("\n}", name);
                if (close == std::string::npos)
                    continue;
                text.insert(close + 1u, models);
                ++patched;
            }
            if (!patched)
            {
                std::cerr << "[zoo] SCENE.TXT: the ZOO scene was not found, no ambient NPCs" << std::endl;
                return false;
            }
            bytes.assign(text.begin(), text.end());
            std::cout << "[zoo] SCENE.TXT: " << kNpcTypeCount << " NPC models in the ZOO and MPZOO scenes" << std::endl;
            return true;
        }

        // ---- split screen ---------------------------------------------------

        // The camera FOV getter func_18C828: in split screen (game type 4,
        // byte 0x24C9B3) the level's own FOV from the camera table
        // (*0x24AD78 +0x200 Boardwalk, +0x230 Civic Center, +0x260 Movie
        // Lot, +0x290 Museum), 0 for any other level: the Zoo's split
        // screen drew from a point (an endless zoom). Here the Movie Lot's,
        // the other open outdoor level (the getter is asked as for it; the
        // FOV setting's scaling, a wrapper installed before this one,
        // applies as for any level).
        constexpr uint32_t kFovGetter = 0x0018C828u;
        constexpr uint32_t kGameType = 0x0024C9B3u; // byte
        constexpr uint8_t kGameTypeSplitScreen = 4u;
        constexpr uint32_t kMovieLotIndex = 5u;

        void fovGetterHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint8_t *type = getConstMemPtr(rdram, kGameType);
            if (!type || *type != kGameTypeSplitScreen || readWord(rdram, kLevelIndex) != kZooIndex)
            {
                hooks().fovGetter(rdram, ctx, runtime);
                return;
            }
            writeWord(rdram, kLevelIndex, kMovieLotIndex);
            hooks().fovGetter(rdram, ctx, runtime);
            writeWord(rdram, kLevelIndex, kZooIndex);
        }

        // ---- the Xbox's unlock -------------------------------------------

        // The career's progress check func_1BD438(career), called by the
        // challenge completion func_1BE1A8 (not every frame). Its argument
        // is the player's career object (*(*0x24C9BC + 0x10) + 0x10, on the
        // heap), not the 0x510-byte skater record: the career works on a
        // copy of that record at +0x5E0 (written back when the level is
        // left), and per level from +0x68C + level * 0x24 (challenge points
        // there, func_1BE1A8 adds each completed challenge's). A level's
        // eighth point unlocks the next level (table 0x2541A0: the record's
        // +1 count and +4 level bits, the career's 0x24CBBC get its bit,
        // "New level unlocked!"), once (flag 2 of +0x69C + level * 0x24),
        // but not for the Museum (level 6, whose entry is 8, the Zoo). The
        // Xbox's skips level 8 instead (default.xbe 0x54270), so the Museum
        // unlocks the Zoo for that skater. Here the same, done just before
        // the check (which then sets the flag and skips as before). Seen in
        // a run: the eighth point in the Museum, "New level unlocked!", the
        // Zoo open in the free skate select after leaving the level.
        constexpr uint32_t kCareerCheck = 0x001BD438u;
        constexpr uint32_t kCareerLevel = 0x002795C8u; // u32: the level index in the career
        constexpr uint32_t kUnlockThreshold = 0x00276E68u; // u32: challenges for the next level (8)
        constexpr uint32_t kLevelUnlocks = 0x0024CBBCu;  // u16: the career's level bits
        constexpr uint32_t kShowMessage = 0x001AFEE8u;   // (text, f12 seconds)
        constexpr uint32_t kNewLevelText = 0x002687D8u;  // "New level unlocked!"
        constexpr uint32_t kMuseumIndex = 6u;

        void careerUnlockHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t skater = getRegU32(ctx, 4);
            const uint32_t level = readWord(rdram, kCareerLevel);
            const uint32_t record = skater + 0x24u * level;
            if (level == kMuseumIndex && readWord(rdram, 0x0024C9B4u) != 7u &&
                readWord(rdram, record + 0x68Cu) >= readWord(rdram, kUnlockThreshold) &&
                !(readWord(rdram, record + 0x69Cu) & 2u) && !(readWord(rdram, skater + 0x5E4u) & (1u << kZooIndex)))
            {
                if (uint8_t *count = getMemPtr(rdram, skater + 0x5E1u))
                    ++*count;
                writeWord(rdram, skater + 0x5E4u, readWord(rdram, skater + 0x5E4u) | (1u << kZooIndex));
                if (uint8_t *bits = getMemPtr(rdram, kLevelUnlocks))
                {
                    uint16_t v = 0;
                    std::memcpy(&v, bits, 2);
                    v = static_cast<uint16_t>(v | (1u << kZooIndex));
                    std::memcpy(bits, &v, 2);
                }
                const float f12 = ctx->f[12];
                ctx->f[12] = 3.5f;
                runtime->callGuestFunction(rdram, ctx, kShowMessage, {kNewLevelText}, getRegU32(ctx, 29));
                ctx->f[12] = f12;
                std::cout << "[zoo] the Museum's eighth challenge: the Zoo unlocked for this skater, as on the Xbox" << std::endl;
            }
            hooks().careerCheck(rdram, ctx, runtime);
        }

        // ---- the level select ---------------------------------------------

        bool patchLevelSelect(std::string &text)
        {
            zx::Span menu = zx::findObject(text, "FrontEnd/Background/LevelSelect/Menu");
            zx::Span script = zx::findObject(text, "FreeskateOnChange", menu);
            if (!menu.found() || !script.found())
            {
                std::cerr << "[zoo] FrontEnd.zsc: the free skate level select was not found" << std::endl;
                return false;
            }
            std::string block = text.substr(script.begin, script.end - script.begin);
            const int depth = zx::indentAt(text, script.begin) + 2; // the script lines
            const std::string tabs(static_cast<size_t>(depth), '\t');
            // Every level's icon row gets the giraffe before the editor's
            // icon; the editor level becomes entry 8.
            size_t replaced = 0;
            for (const char *editor : {"\\ni\"}", "\\nI\"}"})
            {
                const std::string to = std::string("\\ns") + editor;
                for (size_t at = block.find(editor); at != std::string::npos; at = block.find(editor, at + to.size()))
                {
                    block.replace(at, std::strlen(editor), to);
                    ++replaced;
                }
            }
            const std::string editorLabel = tabs + ":7\r\n";
            const size_t editorCase = block.find(editorLabel);
            const std::string populate = tabs + "CALL inlineMenuPopulateLevelData\r\n";
            const std::string parkCheck = tabs + "IF .SelectedLevel == {\"PEMovieLot\"} GOTO ParkEditorLevel\r\n";
            const std::string complete = tabs + ":Complete\r\n";
            if (replaced != 8u || editorCase == std::string::npos || block.find(parkCheck) == std::string::npos ||
                block.find(complete) == std::string::npos || block.find(populate) == std::string::npos)
            {
                std::cerr << "[zoo] FrontEnd.zsc: FreeskateOnChange is not the expected script (" << replaced
                          << " icon rows), no Zoo" << std::endl;
                return false;
            }
            block.replace(editorCase, editorLabel.size(),
                          tabs + ":7\r\n" + tabs + "SET .SelectedLevel {\"Zoo\"}\r\n" + tabs +
                              "SET .LevelData.LevelName.TextLabel.Text {\"Zoo\"}\r\n" + tabs +
                              "SET Items {\"o\\nr\\nu\\nw\\ne\\nq\\np\\nS\\ni\"}\r\n" + tabs + "GOTO Done\r\n" + tabs +
                              ":8\r\n");
            const size_t check = block.find(parkCheck);
            block.insert(check + parkCheck.size(), tabs + "IF .SelectedLevel == {\"Zoo\"} GOTO ZooLevel\r\n");
            // Always: unlocked whatever the career says (the native check
            // reads the skater's level bit 8, which no PS2 career sets).
            // Museum (the Xbox's rule): that check, the bit set by the
            // Museum's eight challenges (careerUnlockHook).
            std::string zoo = tabs + "GOTO Complete\r\n" + tabs + ":ZooLevel\r\n";
            if (!level().earned)
                zoo += tabs + "SET .LevelData.Data.isLocked {0}\r\n" + tabs + "PLAY .LevelData.Data.Refresh\r\n";
            if (level().flythrough)
                zoo += tabs + "SET Movie {\"assets\\Movies\\Flythrus\\Zoo.PSM\"}\r\n" + tabs + "PLAY StartFlythruMovie\r\n";
            else
                zoo += tabs + "CALL inlineMenuStopMovie\r\n" + tabs +
                       "SET Root.FrontEnd.Movie.Material {\"FrontEnd.still_bkg_01\"}\r\n";
            block.insert(block.find(complete), zoo);
            text.replace(script.begin, script.end - script.begin, block);
            // Nine icons: the row is 64 pixels a step from x 64, so it ends
            // at the screen's right edge.
            menu = zx::findObject(text, "FrontEnd/Background/LevelSelect/Menu");
            const std::string menuTabs(static_cast<size_t>(zx::indentAt(text, menu.begin) + 1), '\t');
            if (!zx::replaceIn(text, menu, menuTabs + "VisibleCount\t8\r\n", menuTabs + "VisibleCount\t9\r\n"))
                std::cerr << "[zoo] FrontEnd.zsc: level select VisibleCount not found" << std::endl;
            return true;
        }

        // The split-screen level select (MultiplayerOnChange: Movie Lot,
        // Civic Center, Boardwalk, Museum) gains the Zoo as a fifth entry,
        // unlocked, its row centred (x 160 instead of 192).
        bool patchMultiplayerSelect(std::string &text)
        {
            zx::Span menu = zx::findObject(text, "FrontEnd/Background/LevelSelect/Menu");
            zx::Span script = zx::findObject(text, "MultiplayerOnChange", menu);
            zx::Span levelSelect = zx::findObject(text, "FrontEnd/Background/LevelSelect");
            zx::Span setup = zx::findObject(text, "SetupForMultiplayer", levelSelect);
            if (!menu.found() || !script.found() || !setup.found())
            {
                std::cerr << "[zoo] FrontEnd.zsc: the split-screen level select was not found" << std::endl;
                return false;
            }
            std::string block = text.substr(script.begin, script.end - script.begin);
            const int depth = zx::indentAt(text, script.begin) + 2;
            const std::string tabs(static_cast<size_t>(depth), '\t');
            // Each row's icons get the giraffe last.
            size_t rows = 0;
            for (const char *row : {"{\"O\\nr\\nw\\np\"}", "{\"o\\nR\\nw\\np\"}", "{\"o\\nr\\nW\\np\"}",
                                    "{\"o\\nr\\nw\\nP\"}"})
            {
                const size_t at = block.find(row);
                if (at == std::string::npos)
                    break;
                block.insert(at + std::strlen(row) - 2u, "\\ns");
                ++rows;
            }
            const std::string done = tabs + ":Done\r\n";
            const std::string abortMovie = tabs + "ABORT StartFlythruMovie \r\n";
            const size_t doneAt = block.find(done);
            if (rows != 4u || doneAt == std::string::npos || block.find(abortMovie) == std::string::npos)
            {
                std::cerr << "[zoo] FrontEnd.zsc: MultiplayerOnChange is not the expected script (" << rows
                          << " icon rows), no split-screen Zoo" << std::endl;
                return false;
            }
            block.insert(doneAt, tabs + ":4\r\n" + tabs + "SET .SelectedLevel {\"Zoo\"}\r\n" + tabs +
                                     "SET .LevelData.LevelName.TextLabel.Text {\"Zoo\"}\r\n" + tabs +
                                     "SET Items {\"o\\nr\\nw\\np\\nS\"}\r\n" + tabs + "GOTO Done\r\n");
            const size_t abortAt = block.find(abortMovie);
            block.insert(abortAt + abortMovie.size(), tabs + "IF .SelectedLevel == {\"Zoo\"} GOTO ZooLevel\r\n");
            // The end of the script (the closing brace of its Script block).
            const size_t close = block.rfind("}", block.rfind("}") - 1u);
            const size_t lineStart = block.rfind('\n', close) + 1u;
            std::string zoo = tabs + "GOTO ZooDone\r\n" + tabs + ":ZooLevel\r\n" + tabs +
                              "SET .LevelData.Data.isLocked {0}\r\n" + tabs + "PLAY .LevelData.Data.Refresh\r\n";
            if (level().flythrough)
                zoo += tabs + "SET Movie {\"assets\\Movies\\Flythrus\\Zoo.PSM\"}\r\n" + tabs + "PLAY StartFlythruMovie\r\n";
            else
                zoo += tabs + "CALL inlineMenuStopMovie\r\n" + tabs +
                       "SET Root.FrontEnd.Movie.Material {\"FrontEnd.still_bkg_01\"}\r\n";
            zoo += tabs + ":ZooDone\r\n";
            block.insert(lineStart, zoo);
            text.replace(script.begin, script.end - script.begin, block);
            // Five icons, 64 pixels apart, centred.
            levelSelect = zx::findObject(text, "FrontEnd/Background/LevelSelect");
            setup = zx::findObject(text, "SetupForMultiplayer", levelSelect);
            if (!zx::replaceIn(text, setup, "SET Menu.Position.x {192}", "SET Menu.Position.x {160}"))
                std::cerr << "[zoo] FrontEnd.zsc: the split-screen level row's position was not found" << std::endl;
            return true;
        }

        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::ArchiveEntry> entries;
            std::string error;
            if (!zx::readArchive(bytes, entries, &error))
            {
                std::cerr << "[zoo] UILIB.ZAL: " << error << std::endl;
                return false;
            }
            bool changed = false;
            for (zx::ArchiveEntry &entry : entries)
            {
                if (zx::scriptName(entry) != "FrontEnd")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                // The FrontEnd the game loads (the other is an older one).
                if (text.find("\"FreeskateOnChange\"") == std::string::npos)
                    continue;
                bool patched = patchLevelSelect(text);
                patched = patchMultiplayerSelect(text) || patched;
                if (patched)
                {
                    entry.data.assign(text.begin(), text.end());
                    changed = true;
                }
            }
            if (changed)
            {
                bytes = zx::writeArchive(entries);
                std::cout << "[zoo] the Zoo is in the free skate and split-screen level selects" << std::endl;
            }
            return changed;
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *on = ps2x::boot::text("PS2X_ZOO");
        if (!on || on[0] != '1')
        {
            std::cout << "[zoo] off (PS2X_ZOO=1 adds the Zoo to free skate)" << std::endl;
            return;
        }
        if (made_::exists(kLevelFolder + "ZOO.GEO") && made_::exists(kLevelFolder + "ZOO.TXR") &&
            made_::exists(kLevelFolder + "ZOOT.BIN"))
        {
            level().source = Source::Converted;
            level().flythrough = ps2_xbox_disc::init() && ps2_xbox_disc::exists(kXboxFlythroughPsm);
            if (!level().flythrough && ps2_xbox_disc::available() && game::host::movies::playedOnHost())
                if (const std::string bink = ps2_xbox_disc::hostPath(kXboxFlythroughBink); !bink.empty())
                {
                    game::host::movies::setHostMovie(kFlythrough, bink, game::host::movies::kPs2MovieWidth,
                                                     game::host::movies::kPs2MovieHeight);
                    level().flythrough = level().flythroughBink = true;
                }
        }
        else
        {
            const char *which = std::getenv("PS2X_ZOO_PS2");
            // The multiplayer prototype by default: textured trees and
            // shading; the grey box draws as a dithered fog.
            level().source = which && std::string(which) == "sp" ? Source::Ps2 : Source::Ps2Mp;
        }
        std::cout << "[zoo] on: "
                  << (level().source == Source::Converted ? "the Xbox release's level, made at build time (" + made_::description() + ")"
                      : level().source == Source::Ps2Mp ? std::string("the PS2 disc's multiplayer prototype (MP/ZOO; ") + made_::description() + ")"
                                                  : std::string("the PS2 disc's grey box (") + made_::description() + ")")
                  << (level().flythrough ? ", flythrough" : "") << std::endl;
        gf::registerProvider(kLevelFolder, &provide, "zoo");
        ps2x::session::onEnd([]() { gf::removeProviders("zoo"); });
        gf::registerProvider(kLoadingFolder, &provide, "zoo");
        if (!(hooks().fovGetter = runtime.lookupFunction(kFovGetter)) || !runtime.replaceFunction(kFovGetter, &fovGetterHook))
            std::cerr << "[zoo] the split-screen camera's FOV getter was not wrapped" << std::endl;
        if (level().source == Source::Converted)
            gf::registerProvider(kMpLevelFolder, &provide, "zoo");
        gf::registerProvider("ASSETS/MOVIES/FLYTHRUS/", &provide, "zoo");
        // The ambient NPCs: with the Xbox level, its models, and room for
        // the extra NPC types (extended memory).
        ps2x::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary, "ai");
        // PS2X_ZOO_UNLOCK=museum (the app: Settings > Game > Zoo Unlock): the
        // Xbox's rule; else always open.
        if (const char *unlock = ps2x::boot::text("PS2X_ZOO_UNLOCK"); unlock && std::string(unlock) == "museum")
        {
            if ((hooks().careerCheck = runtime.lookupFunction(kCareerCheck)) &&
                runtime.replaceFunction(kCareerCheck, &careerUnlockHook))
                level().earned = true;
            else
                std::cerr << "[zoo] the career's progress check was not wrapped: the Zoo stays open" << std::endl;
        }
        std::cout << "[zoo] " << (level().earned ? "unlocked by the Museum's eight challenges, as on the Xbox"
                                           : "always unlocked in free skate")
                  << std::endl;
        if (const char *ext = std::getenv("PS2X_ZOO_EXTMEM"); ext && ext[0] == '0')
            std::cout << "[zoo] no extended memory (PS2X_ZOO_EXTMEM=0)" << std::endl;
        else if (!runtime.addGuestHeapRegion(PS2_LEGACY_RAM_SIZE, PS2_RAM_SIZE) ||
                 !(level().textureAlloc = runtime.lookupFunction(kTextureAlloc)) ||
                 !runtime.replaceFunction(kTextureAlloc, &textureAllocHook))
            std::cerr << "[zoo] extended memory not set up" << std::endl;
        else
        {
            level().extended = true;
            bool models = level().source == Source::Converted;
            for (const NpcType &type : kNpcTypes)
                models = models && made_::exists(kNpcFolder + gf::normalizePath(type.file));
            if (const char *npcs = std::getenv("PS2X_ZOO_NPCS"); npcs && npcs[0] == '0')
                models = false; // developers: the Zoo without its ambient NPCs
            if (models && npcs::installNpcStart(runtime) && (npcTypes().npcAnimations = runtime.lookupFunction(kNpcAnimations)) &&
                (hooks().showObject = runtime.lookupFunction(kShowObject)) &&
                (hooks().drawnLastFrame = runtime.lookupFunction(kDrawnLastFrame)) &&
                (animatorScope().updateAnimator = runtime.lookupFunction(kUpdateAnimator)) &&
                runtime.replaceFunction(kUpdateAnimator, &updateAnimatorHook) &&
                runtime.replaceFunction(kDrawnLastFrame, &drawnLastFrameHook) &&
                runtime.replaceFunction(kShowObject, &showObjectHook) &&
                runtime.replaceFunction(kNpcTypeLookup, &npcTypeLookupHook) &&
                runtime.replaceFunction(kNpcAnimations, &npcAnimationsHook))
            {
                npcs().available = true;
                gf::registerProvider(kNpcFolder, &provide, "zoo");
                ps2x::disc_patch::registerPatch("SCENE.TXT", &patchScene, "ai");
                std::cout << "[zoo] ambient NPCs: " << kNpcTypeCount << " Xbox NPC types" << std::endl;
            }
            std::cout << "[zoo] the Zoo's textures may use " << (PS2_RAM_SIZE - PS2_LEGACY_RAM_SIZE) / (1024u * 1024u)
                      << " MB above the PS2's 32 MB" << std::endl;
            // How much of it is in use, when that changes by a megabyte.
            runtime.addVSyncHook([&runtime](uint8_t *, uint64_t)
                                 {
                                     const uint32_t used = runtime.extendedHeapInUse();
                                     if (used / (1024u * 1024u) != level().extendedInUse / (1024u * 1024u))
                                         std::cout << "[zoo] extended memory in use: " << used / 1024u << " KB" << std::endl;
                                     level().extendedInUse = used;
                                 });
        }
    }
}

namespace aggressive_inline
{
    void installZoo(PS2Runtime &runtime)
    {
        zoo::install(runtime);
    }
}
