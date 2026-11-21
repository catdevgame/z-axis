// zx-xbox-convert: what Aggressive Inline takes from the Xbox release and
// needs in the PS2's formats, made from the user's Xbox files and PS2 disc
// at build time: the Zoo (runtime/ps2_zaxis_zoo.h) and the Ape
// (runtime/ps2_zaxis_ape.h). The packaging step runs it (tools/make-app.sh,
// tools/make-windows.ps1) and bundles the output folder as
// Resources/xbox-converted (Windows: xbox-converted\ next to disc\); the
// game overrides serve those files (ZX_XBOX_CONVERTED) and convert
// nothing.
//
//   zx-xbox-convert --xbox <dir> --ps2 <dir> --out <dir> [options]
//
//   --xbox <dir>   the Xbox release's files, extracted from your own disc
//                  (tools/xbox_files.py extract): its root, media or
//                  media/assets, any case. Read only.
//   --ps2 <dir>    your PS2 disc, extracted (the folder with SYSTEM.CNF and
//                  ASSETS). Read only: the split-screen games' props come
//                  from its MP\MOVIE.
//   --out <dir>    where the files go (created; files of the same name
//                  replaced).
// Developers (the Zoo):
//   --split-screen-texture-side N   split screen's textures (default 64)
//   --level-texture-budget KB       the level's textures' texels at most KB
//                                   (default 2560; 0: none; zoo::Options)
//   --group-entries N               the level's strip entries per PS2 group (default 60)
//   --hide name,name                objects (TGHD names) that draw nothing
//   --split-screen-eggs "x,y,z;..." egg spots in place of the port's
//   --keep-willow-twins             split screen keeps both willows a tree
//   --threads N                     texture workers (default: the machine's)
//
// Never commit what this makes: it is your discs' data.

#include "runtime/ps2_game_files.h"
#include "runtime/ps2_xbox_disc.h"
#include "runtime/ps2_zaxis_ape.h"
#include "runtime/ps2_zaxis_zoo.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace zoo = zx::zaxis::zoo;

    int usage(const char *why)
    {
        std::cerr << "zx-xbox-convert: " << why << "\n"
                  << "usage: zx-xbox-convert --xbox <Xbox files> --ps2 <PS2 disc folder> --out <folder>\n"
                  << "       [--split-screen-texture-side N] [--level-texture-budget KB] [--group-entries N] [--hide a,b]\n"
                  << "       [--split-screen-eggs \"x,y,z;...\"] [--keep-willow-twins] [--threads N]\n";
        return 2;
    }

    bool writeFile(const fs::path &path, const std::vector<uint8_t> &bytes)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(out);
    }
}

int main(int argc, char **argv)
{
    std::string xboxDir, ps2Dir, outDir;
    zoo::Options options;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&]() -> const char *
        { return i + 1 < argc ? argv[++i] : nullptr; };
        const char *v = nullptr;
        if (arg == "--keep-willow-twins")
        {
            options.keepWillowTwins = true;
            continue;
        }
        if (arg.rfind("--", 0) != 0 || !(v = value()))
            return usage(("bad argument " + arg).c_str());
        if (arg == "--xbox")
            xboxDir = v;
        else if (arg == "--ps2")
            ps2Dir = v;
        else if (arg == "--out")
            outDir = v;
        else if (arg == "--split-screen-texture-side" && std::atoi(v) >= 8)
            options.splitScreenTextureSide = std::atoi(v);
        else if (arg == "--level-texture-budget" && std::atoi(v) >= 0)
            options.levelTextureBudget = static_cast<size_t>(std::atoi(v)) * 1024u; // KB, 0: none
        else if (arg == "--group-entries" && std::atoi(v) >= 4)
            options.groupEntries = static_cast<size_t>(std::atoi(v));
        else if (arg == "--hide")
            options.hide = v;
        else if (arg == "--split-screen-eggs")
            options.splitScreenEggs = v;
        else if (arg == "--threads")
            options.threads = static_cast<unsigned>(std::max(0, std::atoi(v)));
        else
            return usage(("bad argument " + arg + " " + v).c_str());
    }
    if (xboxDir.empty() || ps2Dir.empty() || outDir.empty())
        return usage("--xbox, --ps2 and --out are needed");

    const auto start = std::chrono::steady_clock::now();
    if (!ps2_xbox_disc::openPath(xboxDir))
    {
        std::cerr << "zx-xbox-convert: no Xbox assets folder in " << xboxDir << std::endl;
        return 1;
    }
    std::error_code ec;
    if (!fs::is_directory(ps2Dir, ec))
    {
        std::cerr << "zx-xbox-convert: no PS2 disc folder " << ps2Dir << std::endl;
        return 1;
    }
    zx::game_files::GameFileRoots roots;
    roots.disc = ps2Dir;
    zx::game_files::GameFileSystem disc(roots);

    const zoo::Reader xbox = [](const std::string &path, std::vector<uint8_t> &out)
    { return ps2_xbox_disc::readFile(path, out); };
    const zoo::Reader ps2 = [&disc](const std::string &path, std::vector<uint8_t> &out)
    {
        std::shared_ptr<zx::game_files::GameFile> file = disc.open(path);
        return file && zx::game_files::readAll(*file, out);
    };

    zoo::Made made;
    std::string error;
    if (!zoo::build(xbox, ps2, options, made, std::cout, &error) ||
        !zx::zaxis::ape::build(xbox, made, std::cout, &error))
    {
        std::cerr << "zx-xbox-convert: " << error << std::endl;
        return 1;
    }
    uint64_t bytes = 0;
    for (const auto &[path, contents] : made.files)
    {
        if (!writeFile(fs::path(outDir) / fs::path(path), contents))
        {
            std::cerr << "zx-xbox-convert: cannot write " << (fs::path(outDir) / path).string() << std::endl;
            return 1;
        }
        bytes += contents.size();
    }
    std::cout << "[xbox-convert] " << made.files.size() << " files, " << (bytes >> 20) << " MB, into " << outDir << " in "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s" << std::endl;
    return 0;
}
