// zx-gc-convert: what Aggressive Inline takes from its GameCube release and
// needs in the PS2's formats, made at build time from the files
// tools/modding/gc_disc.py extracted from the user's GameCube disc: TUT/Estate, the
// leftover earlier Tutorial (runtime/zaxis_estate.h). The packaging step
// runs it (platform/mac/make-app.sh, platform/windows/make-windows.ps1) and bundles the output
// folder as Resources/estate (Windows: estate\ next to disc\); the game
// override serves those files (ZX_ESTATE_DIR) and converts nothing.
//
//   zx-gc-convert --gc <dir> --out <dir>
//
//   --gc <dir>    the GameCube files, extracted from your own disc
//                 (tools/modding/gc_disc.py extract --out <dir> assets/levels/TUT/Estate):
//                 the folder with assets/, or the Estate folder itself. Read only.
//   --out <dir>   where the files go (created; files of the same name replaced).
//   --picture <file.bmp>  also the picture of the level its icon is made
//                 from (256 x 256, 24-bit), for the record; not bundled.
//
// Never commit what this makes: it is your disc's data.

#include "runtime/zaxis_estate.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    int usage(const char *why)
    {
        std::cerr << "zx-gc-convert: " << why << "\n"
                  << "usage: zx-gc-convert --gc <GameCube files> --out <folder> [--picture <file.bmp>]\n";
        return 2;
    }

    std::string lower(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    }

    // The entry of `dir` named `name` in any case, or empty.
    fs::path child(const fs::path &dir, const std::string &name)
    {
        std::error_code ec;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (lower(it->path().filename().string()) == lower(name))
                return it->path();
        return {};
    }

    fs::path findEstate(const fs::path &root)
    {
        if (!child(root, "tut.GEO").empty() && !child(root, "TUTT.bin").empty())
            return root;
        fs::path at = root;
        for (const char *part : {"assets", "levels", "TUT", "Estate"})
        {
            at = child(at, part);
            if (at.empty())
                return {};
        }
        return at;
    }

    bool readFile(const fs::path &path, std::vector<uint8_t> &out)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
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
    std::string gcDir, outDir, pictureFile;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (i + 1 >= argc)
            return usage(("bad argument " + arg).c_str());
        if (arg == "--gc")
            gcDir = argv[++i];
        else if (arg == "--out")
            outDir = argv[++i];
        else if (arg == "--picture")
            pictureFile = argv[++i];
        else
            return usage(("bad argument " + arg).c_str());
    }
    if (gcDir.empty() || outDir.empty())
        return usage("--gc and --out are needed");
    const auto start = std::chrono::steady_clock::now();
    const fs::path estate = findEstate(gcDir);
    if (estate.empty())
    {
        std::cerr << "zx-gc-convert: no assets/levels/TUT/Estate in " << gcDir << std::endl;
        return 1;
    }
    const zx::zaxis::estate::Reader reader = [&estate](const std::string &name, std::vector<uint8_t> &out)
    {
        const fs::path file = child(estate, name);
        return !file.empty() && readFile(file, out);
    };
    zx::zaxis::estate::Made made;
    std::string error;
    if (!zx::zaxis::estate::build(reader, made, std::cout, &error))
    {
        std::cerr << "zx-gc-convert: " << error << std::endl;
        return 1;
    }
    for (const auto &[name, contents] : made.files)
        if (!writeFile(fs::path(outDir) / name, contents))
        {
            std::cerr << "zx-gc-convert: cannot write " << (fs::path(outDir) / name).string() << std::endl;
            return 1;
        }
    if (!pictureFile.empty())
    {
        zx::zaxis::estate::Picture picture;
        if (!zx::zaxis::estate::renderPicture(made.files["TUT.GEO"], made.files["TUT.INS"], made.files["TUT.TXR"], 256,
                                                picture, nullptr, &error) ||
            !writeFile(pictureFile, zx::zaxis::estate::pictureBmp(picture)))
        {
            std::cerr << "zx-gc-convert: the picture: " << (error.empty() ? "cannot write " + pictureFile : error) << std::endl;
            return 1;
        }
    }
    std::cout << "[gc-convert] TUT/Estate: " << made.files.size() << " files into " << outDir << " in "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << " s" << std::endl;
    return 0;
}
