// The extras' setup (collection/extras/setup.h): the discs checked and
// recorded, each game's extras made from them.
#include "collection/extras/setup.h"

#include "collection/disc_check.h"
#include "collection/extras/ai_converted.h"
#include "collection/extras/ai_xbox_files.h"
#include "collection/extras/bmx_releases.h"
#include "collection/extras/dm2_xbox.h"
#include "collection/extras/gamecube_disc.h"
#include "collection/extras/panel_maps.h"
#include "collection/extras/source_files.h"
#include "collection/extras/xbox_disc.h"
#include "collection/extras/xbox_texture_pack.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace ps2x::app::extras
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr char kManifestHead[] = "# z-axis extras 1";

        uint32_t be32(const uint8_t *p)
        {
            return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
        }

        // A GameCube disc's main.dol: its header's 18 sections (7 text, 11
        // data: offsets at 0, sizes at 0x90) end where the file does.
        bool readMainDol(GameCubeDisc &disc, Bytes &out)
        {
            uint8_t head[0x100];
            if (!disc.image().read(disc.dolOffset(), sizeof head, head))
                return false;
            uint64_t size = sizeof head;
            for (int i = 0; i < 18; ++i)
            {
                const uint32_t offset = be32(head + 4 * i), length = be32(head + 0x90 + 4 * i);
                if (length)
                    size = std::max<uint64_t>(size, uint64_t(offset) + length);
            }
            out.resize(size_t(size));
            return disc.image().read(disc.dolOffset(), out.size(), out.data());
        }

        // The disc's executable, by which it is known.
        bool executableOf(DiscFiles &files, Bytes &out)
        {
            if (auto *gc = dynamic_cast<GameCubeDisc *>(&files))
                return readMainDol(*gc, out);
            const DiscFile *xbe = files.find("default.xbe");
            return xbe && files.readFile(*xbe, out);
        }

        std::string titleOf(const std::string &game)
        {
            return game == "ai" ? "Aggressive Inline" : game == "dm2" ? "Dave Mirra Freestyle BMX 2" : "BMX XXX";
        }
    }

    const std::vector<ExtrasDisc> &extrasDiscs()
    {
        // The USA releases the extras' converters were made for (their
        // tables' places, their files' contents).
        static const std::vector<ExtrasDisc> discs = {
            {"ai", "xbox", "Aggressive Inline (Xbox, USA)", "",
             "f8cd57e2d2570038f850898c811a07f6014e1534143a267364af995a2fd8bd5f"},
            {"ai", "gamecube", "Aggressive Inline (GameCube, USA)", "GILE51",
             "f3d92f4312dafa5baa79355a03989873e100b97ae85b97280bc808edb084c27a"},
            {"dm2", "xbox", "Dave Mirra Freestyle BMX 2 (Xbox, USA)", "",
             "833a51515490a1cb0ab1600350074c1f59257937a67c0511b8ea495352c89dbc"},
            {"bmx", "xbox", "BMX XXX (Xbox, USA)", "",
             "956742821c4dcfe2661d0e7a8784bf9b6c34de913b02ab54bc0b064d89d933fe"},
            {"bmx", "gamecube", "BMX XXX (GameCube, USA)", "GB3E51",
             "2d5c58b816206b58128f1f7779914b0f75436fbb77865429196de84f8c629030"},
        };
        return discs;
    }

    std::vector<const ExtrasDisc *> extrasDiscsOf(const std::string &game)
    {
        std::vector<const ExtrasDisc *> out;
        for (const ExtrasDisc &d : extrasDiscs())
            if (game == d.game)
                out.push_back(&d);
        return out;
    }

    ImageOpener pathOpener()
    {
        return [](const std::string &source, std::string *error)
        { return ImageFile::open(fs::path(std::u8string(source.begin(), source.end())), error); };
    }

    ExtrasSource checkExtrasSource(const std::string &source, const std::string &game, const ImageOpener &open)
    {
        ExtrasSource out;
        out.source = source;
        std::string error;
        // A GameCube disc by its header's magic, else an Xbox disc by its volume.
        std::unique_ptr<ImageFile> file = open(source, &error);
        if (!file)
        {
            out.message = error;
            return out;
        }
        auto gc = std::make_unique<GameCubeDisc>();
        std::string gcError, xboxError;
        if (gc->open(std::move(file), &gcError))
            out.files = std::move(gc);
        else
        {
            file = open(source, &error);
            auto xbox = std::make_unique<XboxDisc>();
            if (file && xbox->open(std::move(file), &xboxError))
                out.files = std::move(xbox);
        }
        if (!out.files)
        {
            out.message = "This is not an Xbox or GameCube game disc image the app reads (" +
                          (xboxError.empty() ? gcError : xboxError) + ")";
            return out;
        }
        out.gameDisc = true;
        Bytes executable;
        if (!executableOf(*out.files, executable))
        {
            out.message = "The disc image cannot be read: its game's executable is damaged.";
            out.files.reset();
            return out;
        }
        const std::string sha256 = sha256Hex(executable.data(), executable.size());
        const auto *gcDisc = dynamic_cast<GameCubeDisc *>(out.files.get());
        for (const ExtrasDisc &d : extrasDiscs())
            if (sha256 == d.sha256 && (game.empty() || game == d.game))
            {
                out.disc = &d;
                out.message = std::string(d.title) + ": its extras can be made.";
                return out;
            }
        for (const ExtrasDisc &d : extrasDiscs())
            if (gcDisc && gcDisc->gameId() == d.gameCode)
            {
                out.message = std::string("This is ") + d.title + ", but not the release the extras are made from " +
                              "(its main.dol differs): a damaged dump, or another version.";
                out.files.reset();
                return out;
            }
        out.message = game.empty() ? "This disc has no extras the app makes."
                                   : "This disc is not a release of " + titleOf(game) + " the app makes extras from.";
        out.files.reset();
        return out;
    }

    fs::path extrasFolder(const fs::path &gameDir) { return gameDir / "extras"; }

    std::vector<RecordedDisc> recordedExtras(const fs::path &gameDir)
    {
        std::vector<RecordedDisc> out;
        std::ifstream in(gameDir / "extras.manifest", std::ios::binary);
        for (std::string line; std::getline(in, line);)
        {
            if (line.empty() || line[0] == '#')
                continue;
            const size_t tab = line.find('\t');
            if (tab == std::string::npos)
                continue;
            out.push_back({line.substr(0, tab), line.substr(tab + 1)});
        }
        return out;
    }

    ExtrasResult makeExtras(const std::string &game, const fs::path &gameDir, std::vector<ExtrasSource> &sources,
                            const XboxModelMaker &models, const ExtrasProgressFn &progress)
    {
        ExtrasResult result;
        std::error_code ec;
        const fs::path staging = gameDir / "extras.partial", made = extrasFolder(gameDir);
        const SourceFiles *xbox = nullptr, *gc = nullptr;
        ImageFiles xboxAssets, gcAssets;
        DiscFiles *xboxDisc = nullptr;
        for (ExtrasSource &s : sources)
        {
            if (!s.disc || !s.files || game != s.disc->game)
            {
                result.message = "An extras disc of another game was given for " + titleOf(game) + ".";
                return result;
            }
            if (std::string(s.disc->release) == "xbox" && xboxAssets.open(*s.files, "media/assets/"))
            {
                xbox = &xboxAssets;
                xboxDisc = s.files.get();
            }
            else if (std::string(s.disc->release) == "gamecube" && gcAssets.open(*s.files, "assets/"))
                gc = &gcAssets;
        }
        const fs::path ps2Disc = gameDir / "disc";
        FolderFiles ps2;
        if (!ps2.open(ps2Disc, {"levels"}))
        {
            result.message = titleOf(game) + " is not set up: its extras need its PS2 disc's files first.";
            return result;
        }
        fs::remove_all(staging, ec);

        // Each extra: its name for the progress line and how it is made.
        struct Step
        {
            std::string name;
            std::function<bool(std::string *)> make;
        };
        std::vector<Step> steps;
        std::vector<std::string> &log = result.log;
        if (game == "ai" && xbox)
        {
            steps.push_back({"Copying the Xbox files", [&](std::string *e) { return makeAiXboxFiles(*xbox, staging / "xbox", log, e); }});
            steps.push_back({"Making the Zoo and the Ape",
                             [&](std::string *e) { return makeAiZooAndApe(*xbox, ps2Disc, staging / "xbox-converted", log, e); }});
            steps.push_back({"Making the Xbox textures and movies", [&](std::string *e)
                             { return makeXboxTexturePack(ps2, *xbox, staging / "xbox-pack", XboxTexturePackOptions(), log, e); }});
            if (models)
                steps.push_back({"Making the Xbox skaters",
                                 [&](std::string *e) { return makeAiXboxSkaters(ps2, *xbox, models, staging / "xbox-pack", log, e); }});
        }
        if (game == "ai" && gc)
            steps.push_back({"Making the Estate level", [&](std::string *e) { return makeAiEstate(*gc, staging / "estate", log, e); }});
        if (game == "dm2" && xbox)
            steps.push_back({"Making the Xbox levels and themes", [&](std::string *e)
                             {
                                 Bytes xbe;
                                 const DiscFile *file = xboxDisc->find("default.xbe");
                                 if (!file || !xboxDisc->readFile(*file, xbe))
                                 {
                                     *e = "The Xbox disc's default.xbe cannot be read.";
                                     return false;
                                 }
                                 return makeDm2XboxExtras(&ps2, *xbox, xbe, staging / "xbox-levels", Dm2XboxOptions(), log, e);
                             }});
        if (game == "bmx" && (xbox || gc))
            steps.push_back({"Making the Xbox and GameCube content",
                             [&](std::string *e) { return makeBmxReleases(ps2, xbox, gc, staging / "releases", log, e); }});
        // DM2's and BMX XXX's Xbox textures: their levels' mostly at twice
        // the PS2's size (their riders and menus are mostly the PS2's own
        // size on the Xbox); no movies.
        if ((game == "dm2" || game == "bmx") && xbox)
            steps.push_back({"Making the Xbox textures", [&](std::string *e)
                             {
                                 XboxTexturePackOptions options;
                                 options.movies = false;
                                 options.archives = game == "bmx"; // its levels' props
                                 return makeXboxTexturePack(ps2, *xbox, staging / "xbox-pack", options, log, e);
                             }});
        // The bottom screen's maps and goals, from the PS2 disc and what was
        // made above (the Zoo, Estate, the Xbox scripts, DM2's Xbox levels).
        steps.push_back({"Making the bottom screen's maps", [&](std::string *e)
                         {
                             FolderFiles disc, zoo, estate, scripts, dm2Xbox;
                             if (!disc.open(ps2Disc, {"ASSETS"}))
                             {
                                 *e = titleOf(game) + "'s PS2 disc files have no ASSETS folder.";
                                 return false;
                             }
                             PanelExtras extras;
                             if (zoo.open(staging / "xbox-converted" / "ASSETS" / "LEVELS" / "ZOO", {}))
                                 extras.zoo = &zoo;
                             if (estate.open(staging / "estate", {}))
                                 extras.estate = &estate;
                             if (scripts.open(staging / "xbox", {"levels"}))
                                 extras.xboxScripts = &scripts;
                             if (dm2Xbox.open(staging / "xbox-levels", {}))
                                 extras.dm2Xbox = &dm2Xbox;
                             const PanelGame which = game == "ai"    ? PanelGame::AggressiveInline
                                                     : game == "dm2" ? PanelGame::DaveMirra2
                                                                     : PanelGame::BmxXxx;
                             return makePanelMaps(which, disc, extras, staging / "panel", log, e);
                         }});

        for (size_t i = 0; i < steps.size(); ++i)
        {
            if (progress && !progress(steps[i].name, i, steps.size()))
            {
                fs::remove_all(staging, ec);
                result.cancelled = true;
                result.message = "Stopped: the extras are as they were.";
                return result;
            }
            std::string error;
            if (!steps[i].make(&error))
            {
                fs::remove_all(staging, ec);
                result.message = error;
                return result;
            }
        }
        if (progress)
            progress("Done", steps.size(), steps.size());
        // In place of the earlier extras, the record last.
        fs::create_directories(staging, ec);
        fs::remove_all(made, ec);
        fs::rename(staging, made, ec);
        if (ec)
        {
            result.message = "The extras could not be put in place: " + ec.message() + ".";
            return result;
        }
        if (sources.empty())
        {
            fs::remove(gameDir / "extras.manifest", ec);
            result.ok = true;
            result.message = "The bottom screen's maps of " + titleOf(game) + " are made (no extras discs).";
            return result;
        }
        std::string manifest = std::string(kManifestHead) + "\n";
        for (const ExtrasSource &s : sources)
            manifest += std::string(s.disc->release) + "\t" + s.source + "\n";
        std::string error;
        if (!writeFile(gameDir / "extras.manifest", manifest, &error))
        {
            result.message = error;
            return result;
        }
        result.ok = true;
        std::string names;
        for (const ExtrasSource &s : sources)
            names += std::string(names.empty() ? "" : " and ") + s.disc->title;
        result.message = "The extras from " + names + " are made.";
        return result;
    }
}
