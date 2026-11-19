#pragma once

// A game's code made on the player's machine from their own disc (the collection's first run): the recompiler the
// app bundles turns the disc's boot ELF into generated C++, the compiler and linker it bundles (LLVM's clang and lld,
// runtime/toolchain/llvm.lock) build that into a game module (runtime/game_module.h), and the module is kept in the
// support folder until the app or the disc changes. Nothing of the game comes with the app.
//
// The app's tools for it, one folder (the packaging's game-code-tools, runtime/toolchain/stage-game-code-tools.sh):
//   bin/            the compiler and linker (clang++, ld64.lld | lld-link | ld.lld: the one llvm program's names;
//                   another platform's modules through the host's linker with -flavor)
//                   and the recompiler (ps2_recomp)
//   lib/clang/      the compiler's own headers (intrinsics, stdint.h)
//   sdk/            the game module's headers (include/: ps2_generated.h, game_module/, ...), its entry
//                   (module_entry.cpp), the compile's flags (game_module_flags.txt) and the folder's key (key.txt);
//                   vu/: VU1's step and core, which the game's VU1 microprograms are compiled against: the recompiler
//                   writes the micro memory image the game's renderer uploads (<hash>.vu1code), the app's own
//                   generator (runtime/ps2_vu1_aot_generator.h) its code, and the module carries it
//   games/<id>/     each game's recompiler config (config.toml) and function list (ghidra-functions.csv)
//   toolchain.txt   the compiler's version (llvm.lock's)
//
// On Android an app may run no program it has written or unpacked itself: the programs are the APK's native libraries
// (lib/arm64-v8a/libclang.so, liblld.so, libps2_recomp.so; Android unpacks them into the app's native library folder,
// the one place it runs an app's files from), and the rest of the folder comes from the APK's assets
// (android_app.cpp unpacks it into the internal storage). The modules are kept in the internal storage too: Android
// loads no code from the external files folder.

#include "collection/game_setup.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace ps2x::app
{
    struct GameCodeTools
    {
        std::filesystem::path root;
        std::filesystem::path programs; // the compiler's, linker's and recompiler's folder: root/bin (Android: above)
        // The tools of `root` when they are all there; else false with what is missing in *error.
        bool open(const std::filesystem::path &folder, std::string *error);
    };

    struct GameCodeRequest
    {
        std::string id;              // "ai", "dm2", "bmx"
        std::filesystem::path elf;   // the disc's boot ELF (already checked)
        std::filesystem::path cache; // where modules are kept (<support>/game-code)
        unsigned jobs = 0;           // compiles at once (0: the host's cores, one left free)
        // What the module is for: "" this app's own platform; or another's, made here for a device that does not
        // build its own (the Thor: "android-arm64"; also macos-arm64, macos-x86_64, windows-x86_64, linux-arm64,
        // linux-x86_64), with the same tools.
        std::string target;
    };

    // The tools' name of a game (games/<id>): `id` when they have it, else the game whose recompiler config names `elf`
    // as its input (a single-game app's setup knows its game by its ELF, not by the collection's id); `id` if none.
    std::string gameCodeGame(const GameCodeTools &tools, const std::string &id, const std::filesystem::path &elf);
    // The module's key: what it was made from and with (the ELF, the game's recompiler inputs, the recompiler, the
    // module's headers and flags, the compiler, the boundary's version, the target). A module is used only under its key.
    std::string gameCodeKey(const GameCodeTools &tools, const GameCodeRequest &request);
    // <cache>/<id>-<key>.dylib (.dll, .so): the module made for this request.
    std::filesystem::path gameCodeFile(const GameCodeTools &tools, const GameCodeRequest &request);

    // A step of the build: its words for the player and how far it is (done of total).
    struct GameCodeProgress
    {
        const char *step = ""; // "Recompiling", "Compiling", "Linking"
        uint64_t done = 0;
        uint64_t total = 0;
    };
    using GameCodeProgressFn = std::function<bool(const GameCodeProgress &)>; // false: stop

    // Makes the module (gameCodeFile), through a folder of its own beside it that is removed after; any other module
    // of this game in the cache is removed once the new one is in place. False with *error in plain words, or
    // cancelled (progress said stop: *error empty).
    bool makeGameCode(const GameCodeTools &tools, const GameCodeRequest &request, const GameCodeProgressFn &progress,
                      std::string *error);

    // The setup's code step (game_setup.h) for an app that makes each game's code: the module for the installed disc's
    // ELF, made with the app's tools in `toolsFolder` into the game's folder (gameCodeCache), the compile shown on the
    // setup's screens. Ready when that module is there.
    class BuiltGameCode final : public GameCodeStep
    {
    public:
        explicit BuiltGameCode(std::filesystem::path toolsFolder) : m_tools(std::move(toolsFolder)) {}
        std::string name() const override { return "Preparing the game's code"; }
        bool ready(const SetupGame &game, const std::filesystem::path &discFolder,
                   const std::filesystem::path &gameDir) const override;
        bool make(const SetupGame &game, const std::filesystem::path &discFolder, const std::filesystem::path &gameDir,
                  const SetupProgressFn &progress, std::string *error) override;

    private:
        std::filesystem::path m_tools;
    };

    // Where a game's modules are kept: <the game's folder>/code (the setup's and the boot's); on Android
    // <internal storage>/game-code/<the game's folder's name>.
    std::filesystem::path gameCodeCache(const std::filesystem::path &gameDir);
}
