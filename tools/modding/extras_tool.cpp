// zx-extras: the collection's extras (collection/extras) on the command
// line, for the checks against the tooling's Python tools (the reference:
// tools/modding/xbox_files.py, tools/modding/gc_disc.py and the converters).
//
//   zx-extras info <image>                  what the image is
//   zx-extras list <image> [PATTERN ...]    each file's size and path
//   zx-extras extract <image> --out <dir> [PATTERN ...]
//                                            the files into <dir>/<disc path>
//   zx-extras xbox-pack --ps2 <dir> --xbox <image|dir> --out <dir> [--no-movies] [--archives]
//                                            a game's Xbox texture pack (Aggressive Inline's:
//                                            tools/modding/xbox_pack.py's; DM2's --no-movies, BMX XXX's
//                                            --no-movies --archives, as the extras' setup makes them)
//   zx-extras bmx-releases --ps2 <dir> [--xbox <image|dir>] [--gc <image|dir>] --out <dir>
//                                            BMX XXX's Xbox and GameCube content
//                                            (tools/modding/bmx_releases.py's)
//   zx-extras dm2-xbox [--ps2 <dir>] --xbox <image|dir> --out <dir>
//                                            DM2's Xbox levels, themes, demo replays and
//                                            career (tools/modding/dm2_xbox.py's)
//   zx-extras ai-xbox --ps2 <dir> --xbox <image|dir> --out <dir>
//                                            AI's Xbox files (<out>/xbox), Zoo and Ape
//                                            (<out>/xbox-converted) and texture pack (<out>/xbox-pack)
//   zx-extras ai-gc --gc <image|dir> --out <dir>   AI's Estate (<out>/estate)
//   zx-extras setup <game> <game folder> [<extras disc image> ...]
//                                            the extras' setup as the app runs it: each image
//                                            checked, the game's extras made from them and its
//                                            installed PS2 disc (<game folder>/disc) into
//                                            <game folder>/extras
//   zx-extras panel ai|dm2|bmx --ps2 <disc folder> --out <dir> [--zoo <dir>] [--estate <dir>]
//                    [--xbox <dir>] [--dm2-xbox <dir>]  the bottom screen's maps (tools/modding/panel_map.py's)
//   zx-extras movie <file> [<width> <height>]
//                                            the pictures the games' movie player decodes of a
//                                            movie file (runtime/pss_movie.h FileVideo), scaled
//                                            as a host movie is (default 512x256), counted
//   zx-extras dump <image>                  the whole disc's bytes to stdout (an RVZ
//                                            expanded: its SHA-1 is Redump's)
//
// An image is an Xbox disc image (XISO or full) or a GameCube one (GCM/ISO
// or RVZ). PATTERN: a disc path, a folder, or a glob (collection/extras/
// disc_image.h pathMatches); none: every file.

#include "collection/extras/ai_converted.h"
#include "collection/extras/ai_xbox_files.h"
#include "collection/extras/ai_xbox_skaters.h"
#include "collection/extras/bmx_releases.h"
#include "collection/extras/dm2_xbox.h"
#include "collection/extras/gamecube_disc.h"
#include "collection/extras/panel_maps.h"
#include "collection/extras/setup.h"
#include "collection/extras/source_files.h"
#include "collection/extras/xbox_disc.h"
#include "collection/extras/xbox_texture_pack.h"
#include "machine.h"
#include "runtime/pss_movie.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <vector>

using namespace zx::app::extras;

// The engine's native readers link the runtime, whose function table a tool
// that runs no game leaves empty (as zx-native-assets does).
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00000000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x00000004u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1u;
PS2Machine::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

namespace
{
    std::unique_ptr<DiscFiles> openDisc(const std::string &path, std::string *what)
    {
        std::string xboxError, gcError;
        auto gc = std::make_unique<GameCubeDisc>();
        if (gc->open(path, &gcError))
        {
            *what = "GameCube disc " + gc->gameId() + " \"" + gc->title() + "\", " + gc->image().describe();
            return gc;
        }
        auto xbox = std::make_unique<XboxDisc>();
        if (xbox->open(path, &xboxError))
        {
            *what = "Xbox disc, partition at " + std::to_string(xbox->partitionOffset()) + ", " + xbox->image().describe();
            return xbox;
        }
        std::cerr << "zx-extras: " << gcError << "\nzx-extras: " << xboxError << "\n";
        return nullptr;
    }
}

namespace
{
    // A release's assets from an extracted folder or a disc image (kept in `disc`).
    std::unique_ptr<SourceFiles> openSource(const std::string &path, std::unique_ptr<DiscFiles> &disc,
                                            const std::vector<std::string> &markers)
    {
        if (std::filesystem::is_directory(path))
        {
            auto folder = std::make_unique<FolderFiles>();
            if (folder->open(path, markers))
                return folder;
            std::cerr << "zx-extras: no assets folder in " << path << "\n";
            return nullptr;
        }
        std::string what;
        disc = openDisc(path, &what);
        if (!disc)
            return nullptr;
        auto files = std::make_unique<ImageFiles>();
        if (files->open(*disc, dynamic_cast<XboxDisc *>(disc.get()) ? "media/assets/" : "assets/"))
            return files;
        std::cerr << "zx-extras: no assets folder on " << path << "\n";
        return nullptr;
    }

    // zx-extras <converter> --ps2 <dir> --xbox <image|dir> --gc <image|dir> --out <dir> [flags]
    int convert(const std::string &command, int argc, char **argv)
    {
        std::map<std::string, std::string> args;
        std::set<std::string> flags;
        for (int i = 2; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (i + 1 < argc && (a == "--ps2" || a == "--xbox" || a == "--gc" || a == "--out"))
                args[a] = argv[++i];
            else
                flags.insert(a);
        }
        if (!args.count("--out"))
        {
            std::cerr << "zx-extras " << command << ": --out is needed\n";
            return 2;
        }
        std::unique_ptr<DiscFiles> xboxDisc, gcDisc, ps2Disc;
        std::unique_ptr<SourceFiles> ps2, xbox, gc;
        if (args.count("--ps2") && !(ps2 = openSource(args["--ps2"], ps2Disc, {"levels"})))
            return 1;
        if (args.count("--xbox") && !(xbox = openSource(args["--xbox"], xboxDisc, {"levels"})))
            return 1;
        if (args.count("--gc") && !(gc = openSource(args["--gc"], gcDisc, {"levels"})))
            return 1;
        std::vector<std::string> log;
        std::string error;
        bool ok = false;
        if (command == "xbox-pack" && ps2 && xbox)
        {
            XboxTexturePackOptions options;
            options.movies = !flags.count("--no-movies");
            options.archives = flags.count("--archives") != 0;
            ok = makeXboxTexturePack(*ps2, *xbox, args["--out"], options, log, &error);
        }
        else if (command == "ai-xbox" && ps2 && xbox)
        {
            const std::filesystem::path out = args["--out"];
            ok = makeAiXboxFiles(*xbox, out / "xbox", log, &error) &&
                 makeAiZooAndApe(*xbox, args["--ps2"], out / "xbox-converted", log, &error) &&
                 makeXboxTexturePack(*ps2, *xbox, out / "xbox-pack", XboxTexturePackOptions(), log, &error) &&
                 makeAiXboxSkaters(*ps2, *xbox, engineXboxModelMaker(), out / "xbox-pack", log, &error);
        }
        else if (command == "ai-gc" && gc)
            ok = makeAiEstate(*gc, std::filesystem::path(args["--out"]) / "estate", log, &error);
        else if (command == "dm2-xbox" && xbox)
        {
            // default.xbe: on the image, or two folders above an extracted media/assets.
            Bytes xbe;
            if (xboxDisc)
            {
                const DiscFile *file = xboxDisc->find("default.xbe");
                if (!file || !xboxDisc->readFile(*file, xbe))
                    xbe.clear();
            }
            else if (auto *folder = dynamic_cast<FolderFiles *>(xbox.get()))
            {
                std::ifstream in(folder->assets().parent_path().parent_path() / "default.xbe", std::ios::binary);
                xbe.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            ok = makeDm2XboxExtras(ps2.get(), *xbox, xbe, args["--out"], Dm2XboxOptions(), log, &error);
        }
        else if (command == "bmx-releases" && ps2 && (xbox || gc))
            ok = makeBmxReleases(*ps2, xbox.get(), gc.get(), args["--out"], log, &error);
        else
        {
            std::cerr << "zx-extras " << command << ": missing --ps2/--xbox/--gc\n";
            return 2;
        }
        for (const std::string &line : log)
            std::cout << line << "\n";
        if (!ok)
            std::cerr << "zx-extras: " << error << "\n";
        return ok ? 0 : 1;
    }
}

namespace
{
    int setup(int argc, char **argv)
    {
        if (argc < 4)
        {
            std::cerr << "usage: zx-extras setup <game> <game folder> [<extras disc image> ...]\n";
            return 2;
        }
        const std::string game = argv[2];
        std::vector<ExtrasSource> sources;
        for (int i = 4; i < argc; ++i)
        {
            ExtrasSource s = checkExtrasSource(argv[i], game, pathOpener());
            std::cout << argv[i] << ": " << s.message << "\n";
            if (!s.disc)
                return 1;
            sources.push_back(std::move(s));
        }
        const ExtrasResult r = makeExtras(game, argv[3], sources, engineXboxModelMaker(),
                                          [](const std::string &step, size_t done, size_t total)
                                          {
                                              std::cout << "[" << done << "/" << total << "] " << step << "\n";
                                              return true;
                                          });
        for (const std::string &line : r.log)
            std::cout << line << "\n";
        std::cout << r.message << "\n";
        return r.ok ? 0 : 1;
    }
}

namespace
{
    int countMovie(int argc, char **argv)
    {
        if (argc < 3)
            return 2;
        zx::movie::FileVideo video{std::filesystem::path(argv[2])};
        if (!video.ok())
        {
            std::cerr << "zx-extras: " << argv[2] << ": no decoder plays it\n";
            return 1;
        }
        const int width = argc >= 5 ? std::atoi(argv[3]) : 512, height = argc >= 5 ? std::atoi(argv[4]) : 256;
        video.setScaledSize(width, height);
        zx::movie::Picture picture;
        size_t pictures = 0;
        bool sized = true;
        while (video.next(picture))
        {
            ++pictures;
            sized = sized && picture.width == width && picture.height == height;
        }
        std::printf("%s: %dx%d at %.3f a second, %zu pictures (%s %dx%d for the game), %.2f s\n", argv[2], video.width(),
                    video.height(), video.framesPerSecond(), pictures, sized ? "all" : "NOT all", width, height,
                    double(pictures) / video.framesPerSecond());
        return pictures ? 0 : 1;
    }
}

namespace
{
    int panel(int argc, char **argv)
    {
        if (argc < 3)
            return 2;
        const std::string game = argv[2];
        std::map<std::string, std::string> args;
        for (int i = 3; i + 1 < argc; i += 2)
            args[argv[i]] = argv[i + 1];
        FolderFiles disc, zoo, estate, scripts, dm2Xbox;
        if (!args.count("--out") || !disc.open(args["--ps2"], {"ASSETS"}))
        {
            std::cerr << "zx-extras panel: --ps2 <disc folder with ASSETS> and --out are needed\n";
            return 2;
        }
        PanelExtras extras;
        if (args.count("--zoo") && zoo.open(args["--zoo"], {}))
            extras.zoo = &zoo;
        if (args.count("--estate") && estate.open(args["--estate"], {}))
            extras.estate = &estate;
        if (args.count("--xbox") && scripts.open(args["--xbox"], {"levels"}))
            extras.xboxScripts = &scripts;
        if (args.count("--dm2-xbox") && dm2Xbox.open(args["--dm2-xbox"], {}))
            extras.dm2Xbox = &dm2Xbox;
        const PanelGame which = game == "ai" ? PanelGame::AggressiveInline : game == "dm2" ? PanelGame::DaveMirra2 : PanelGame::BmxXxx;
        std::vector<std::string> log;
        std::string error;
        const bool ok = makePanelMaps(which, disc, extras, args["--out"], log, &error);
        for (const std::string &line : log)
            std::cout << line << "\n";
        if (!ok)
            std::cerr << "zx-extras: " << error << "\n";
        return ok ? 0 : 1;
    }
}

int main(int argc, char **argv)
{
    if (argc >= 2 && std::string(argv[1]) == "panel")
        return panel(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "movie")
        return countMovie(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "setup")
        return setup(argc, argv);
    if (argc >= 2 && (std::string(argv[1]) == "xbox-pack" || std::string(argv[1]) == "bmx-releases" ||
                      std::string(argv[1]) == "dm2-xbox" || std::string(argv[1]) == "ai-xbox" ||
                      std::string(argv[1]) == "ai-gc"))
        return convert(argv[1], argc, argv);
    if (argc < 3)
    {
        std::cerr << "usage: zx-extras info|list|extract <image> [--out <dir>] [PATTERN ...]\n";
        return 2;
    }
    const std::string command = argv[1], imagePath = argv[2];
    std::string out;
    std::vector<std::string> patterns;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)
            out = argv[++i];
        else
            patterns.push_back(a);
    }
    std::string what;
    std::unique_ptr<DiscFiles> disc = openDisc(imagePath, &what);
    if (!disc)
        return 1;
    std::vector<const DiscFile *> chosen;
    if (patterns.empty())
        for (const DiscFile &file : disc->files())
            chosen.push_back(&file);
    else
        chosen = disc->select(patterns);
    uint64_t bytes = 0;
    for (const DiscFile *file : chosen)
        bytes += file->size;
    if (command == "info")
    {
        std::cout << what << "\n" << disc->files().size() << " files\n";
        return 0;
    }
    if (command == "list")
    {
        for (const DiscFile *file : chosen)
            std::printf("%10llu  %s\n", static_cast<unsigned long long>(file->size), file->path.c_str());
        std::fprintf(stderr, "%zu files, %llu bytes\n", chosen.size(), static_cast<unsigned long long>(bytes));
        return 0;
    }
    if (command == "dump")
    {
        DiscImage &image = disc->image();
        std::vector<uint8_t> piece(4u << 20);
        for (uint64_t done = 0; done < image.size();)
        {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(piece.size(), image.size() - done));
            if (!image.read(done, n, piece.data()))
            {
                std::cerr << "zx-extras: the image cannot be read at " << done << "\n";
                return 1;
            }
            std::fwrite(piece.data(), 1, n, stdout);
            done += n;
        }
        return 0;
    }
    if (command == "extract" && !out.empty())
    {
        std::string error;
        if (!extractFiles(*disc, chosen, out, &error))
        {
            std::cerr << "zx-extras: " << error << "\n";
            return 1;
        }
        std::cout << chosen.size() << " files, " << bytes << " bytes into " << out << "\n";
        return 0;
    }
    std::cerr << "zx-extras: unknown command " << command << " (extract needs --out)\n";
    return 2;
}
