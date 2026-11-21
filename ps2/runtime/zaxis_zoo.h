#pragma once

// Aggressive Inline's Zoo (the level cut from the PS2 release; the Xbox
// release has it) made into PS2 files at build time, from the user's Xbox
// files and PS2 disc. The packaging step (platform/mac/make-app.sh,
// platform/windows/make-windows.ps1) runs zx-xbox-convert (tools/modding/xbox_convert.cpp),
// which writes what build() makes into the app (Resources/xbox-converted;
// Windows: xbox-converted\ next to disc\). The game override
// (game_overrides/aggressive_inline_zoo.cpp) only serves those files
// (ZX_XBOX_CONVERTED); it converts nothing.
//
// What is made (normalized game paths, under the output folder):
//   ASSETS/LEVELS/ZOO/ZOO.TXR   the Xbox textures as 8-bit BMPs, at most 128
//                               a side and 2560 KB in all (Options;
//                               runtime/zaxis_xbox.h)
//   ASSETS/LEVELS/ZOO/ZOO.GEO   strips cut into the PS2 renderer's groups,
//                               materials in the PS2 layout (by the
//                               textures' alpha)
//   ASSETS/LEVELS/ZOO/ZOO.INS, ZOO.DAT, ZOO.ZIT, ZOOT.BIN, ZOO.BIN,
//   MOTIONS.ZAL                 the Xbox files as they are (the PS2's formats)
//   ASSETS/LEVELS/MP/ZOO/       split screen: ZOO.TXR at most 64 a side with
//                               the split-screen games' props' textures from
//                               the PS2 disc's MP\MOVIE; ZOO.GEO the level's
//                               with those 38 props; ZOO.DAT the level's with
//                               player two's start, eight egg spots and each
//                               willow once; ZOO.BIN the free skate script
//                               without its intro camera call. Other names:
//                               the level's.
//   ASSETS/NPC/ZOO/*.ZIT        the six ambient NPC models (kNpcModels),
//                               converted as the level is
//   textures/<path>.xtp         for each converted texture library (the two
//                               ZOO.TXR and the models): the Xbox originals,
//                               keyed by the converted textures, for the GPU
//                               renderer (runtime/texture_replace.h
//                               pack format)

#include "runtime/zaxis_xbox.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <ostream>
#include <string>
#include <vector>

namespace zx::zaxis::zoo
{
    // The engine's name hash (0x148888): case-insensitive ELF hash.
    constexpr uint32_t engineHash(const char *name)
    {
        uint32_t h = 0;
        for (; *name; ++name)
        {
            uint32_t c = static_cast<uint8_t>(*name);
            if (c >= 'a' && c <= 'z')
                c -= 'a' - 'A';
            h = (h << 4) + c;
            const uint32_t high = h & 0xF0000000u;
            if (high)
                h ^= high >> 24;
            h &= ~high;
        }
        return h;
    }

    constexpr const char kLevelFolder[] = "ASSETS/LEVELS/ZOO/";
    constexpr const char kSplitScreenFolder[] = "ASSETS/LEVELS/MP/ZOO/";
    constexpr const char kNpcFolder[] = "ASSETS/NPC/ZOO/";
    constexpr const char kPictureFolder[] = "textures/"; // + game path + ".xtp"

    // The Xbox level's files used as they are (Xbox name under levels/ZOO,
    // the PS2 name).
    struct LevelFile
    {
        const char *xbox;
        const char *ps2;
    };
    inline constexpr LevelFile kLevelFilesAsTheyAre[] = {
        {"zoo.INS", "ZOO.INS"}, {"zoo.DAT", "ZOO.DAT"}, {"zoo.ZIT", "ZOO.ZIT"},
        {"zoot.bin", "ZOOT.BIN"}, {"ZOO.bin", "ZOO.BIN"}, {"motions.zal", "MOTIONS.ZAL"},
    };

    // The Xbox release's NPC types the PS2 executable lacks, as its type
    // table has them (default.xbe 0x1DEC9C, after npc_Juggler): the type
    // (the model's name hash), its model file (in assets\NPC\zoo), and the
    // animation in all five slots (walk, idle, angry, crash, attack).
    struct NpcModel
    {
        const char *model; // SCENE.TXT model name; its hash is the type
        const char *file;  // in assets\NPC\zoo (Xbox case)
        const char *animation;
    };
    inline constexpr NpcModel kNpcModels[] = {
        {"NPC_KelpA", "kelpA.ZIT", "zoo_assets.npc.kelpA"},
        {"NPC_KelpB", "kelpB.ZIT", "zoo_assets.npc.kelpB"},
        {"NPC_KelpC", "kelpC.ZIT", "zoo_assets.npc.kelpC"},
        {"NPC_Shark", "Shark.ZIT", "zoo_assets.npc.shark_ambient"},
        {"NPC_Octopus", "Octopus.ZIT", "zoo_assets.npc.ocopus"}, // sic
        {"NPC_Willow", "Willow.ZIT", "zoo_assets.npc.willow_tree"},
    };
    constexpr size_t kNpcModelCount = sizeof(kNpcModels) / sizeof(kNpcModels[0]);

    struct Options
    {
        // At most this many pixels a side: the level's textures (as in the
        // PS2 levels: the game's GS texture cache garbles a view full of
        // 256s) and split screen's (two views through the one cache).
        int levelTextureSide = 128;
        int splitScreenTextureSide = 64;
        // The level's textures' texels at most this many bytes (0: no
        // budget), the largest made one level smaller first
        // (xbox::budgetSides): the game's texture cache gives a frame's
        // textures GS memory without checking its end, and the Zoo at 128 a
        // side (3,083 KB) ran its allocations over the CLUTs (the GS memory
        // audit, runtime/gs_texture_audit.h: block 16,000 with the CLUTs
        // from 15,676). 2560 KB as dm2_xbox.py's TEXTURE_BUDGET_KB (the
        // PS2 levels' size); the Xbox originals the GPU renderer draws are
        // the same, keyed by the converted textures.
        size_t levelTextureBudget = 2560u * 1024u;
        size_t groupEntries = 60;     // the level's strip entries per PS2 group (the VU1 buffer's; the NPC models keep 60)
        unsigned threads = 0;         // texture conversion workers (0: the machine's)
        // Developers: objects (TGHD names, comma separated) that draw
        // nothing (their TGPH group counts 0).
        std::string hide;
        // Developers: split screen's egg spots, "x,y,z;x,y,z..." in place of
        // the port's.
        std::string splitScreenEggs;
        // Developers: split screen keeps both willow NPCs on each tree.
        bool keepWillowTwins = false;
    };

    // A file of the inputs by path, false if there is none: Xbox paths are
    // relative to media/assets, any case ("levels/ZOO/zoo.TXR"); PS2 paths
    // are disc paths ("ASSETS/LEVELS/MP/MOVIE/MOVIE.GEO").
    using Reader = std::function<bool(const std::string &path, std::vector<uint8_t> &out)>;

    struct Made
    {
        std::map<std::string, std::vector<uint8_t>> files; // output path (see above) -> contents
    };

    // Everything above. False (with a reason) when an Xbox file is missing
    // or does not convert; then nothing should be written (the game falls
    // back to the PS2 disc's prototype).
    bool build(const Reader &xbox, const Reader &ps2, const Options &options, Made &out, std::ostream &log,
               std::string *error = nullptr);

    // ---- the pieces (tests) ------------------------------------------------

    // The objects named in a comma-separated list draw nothing.
    size_t hideObjects(std::vector<uint8_t> &geo, const std::string &list);

    // The split-screen games' props (kSplitScreenProps) the level lacks,
    // from a donor level's geometry (and their textures from its texture
    // library): appended to `file`, a GEO (geometry) or a TXR. Returns the
    // number of objects or textures added.
    extern const char *const kSplitScreenProps[38];
    size_t addSplitScreenProps(std::vector<uint8_t> &file, bool geometry, const std::vector<uint8_t> &levelGeo,
                               const std::vector<uint8_t> &donorGeo, const std::vector<uint8_t> *donorTxr);

    struct SplitScreenDataReport
    {
        bool playerTwo = false; // a "playertwo" start added
        size_t eggs = 0;        // type 2 pickup points added
        size_t twins = 0;       // static willow NPCs left out
    };
    std::vector<uint8_t> splitScreenData(const std::vector<uint8_t> &level, const Options &options,
                                         SplitScreenDataReport *report = nullptr);

    // The free skate script with its playCameraAnim calls made
    // getTheVisibleTimer; the number changed.
    size_t splitScreenScript(std::vector<uint8_t> &script);

    // The .xtp of a converted library's Xbox originals.
    std::vector<uint8_t> picturePack(const std::vector<xbox::HostTexture> &host);
}
