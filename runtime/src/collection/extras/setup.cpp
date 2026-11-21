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

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

namespace zx::app::extras
{
    namespace
    {
        namespace fs = std::filesystem;

        // The record of a game's extras discs: "# z-axis extras 2" (each part keyed, extras/<part>.key), then a line a
        // disc, its release and its source. 1.1.0's was "# z-axis extras 1", its parts unkeyed.
        constexpr char kManifestHead[] = "# z-axis extras 2";

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

    namespace
    {
        // Each maker's version. It moves whenever the maker's output changes (measured on the discs, never assumed:
        // the batch remakes each part and compares it with the one recorded for its version), and an update then
        // remakes the parts that maker makes, and only those. 0 is the output of 1.1.0, the first release with
        // extras; lanecheck's maker-versions.py fails a change to a maker's files that leaves its line here as it was.
        struct MakerVersion
        {
            const char *maker; // a maker shared by the games has a line for each ("xbox_texture_pack ai")
            int version;
        };
        constexpr MakerVersion kMakers[] = {
            {"ai_xbox_files", 0},
            {"ai_zoo_ape", 0},
            {"ai_estate", 0},
            {"ai_xbox_skaters", 0},
            {"xbox_texture_pack ai", 0},
            {"xbox_texture_pack dm2", 0},
            {"xbox_texture_pack bmx", 1}, // 1: the props' textures paired by picture (bmxprops1, 1.2.0)
            {"dm2_xbox", 0},
            {"bmx_releases", 1}, // 1: the create-a-rider libraries, the Xbox songs, the Turok and Vexx trailers (1.2.0)
            {"panel_maps_ai", 0},
            {"panel_maps_dm2", 1}, // 1: each level's NPCs' goals with their givers (dm2panel1, 1.2.0)
            {"panel_maps_bmx", 0},
        };

        int makerVersion(const std::string &maker, bool asBefore)
        {
            if (asBefore)
                return 0;
            for (const MakerVersion &m : kMakers)
                if (maker == m.maker)
                    return m.version;
            return 0;
        }

        // A part of a game's extras: the folder its makers make, from the discs it reads and the parts before it
        // that it reads.
        struct Part
        {
            std::string folder;
            std::string name;                  // for the progress line
            std::vector<std::string> makers;
            std::vector<std::string> releases; // "xbox", "gamecube"
            std::vector<std::string> after;    // folders of the parts it reads
        };

        bool has(const std::vector<std::string> &list, const std::string &item)
        {
            return std::find(list.begin(), list.end(), item) != list.end();
        }

        // The game's parts for the releases it has discs of, in the order they are made (a part after those it
        // reads).
        std::vector<Part> partsOf(const std::string &game, const std::vector<std::string> &releases, bool models)
        {
            const bool xbox = has(releases, "xbox"), gc = has(releases, "gamecube");
            std::vector<Part> parts;
            std::vector<std::string> panelAfter;
            if (game == "ai")
            {
                if (xbox)
                {
                    parts.push_back({"xbox", "Copying the Xbox files", {"ai_xbox_files"}, {"xbox"}, {}});
                    parts.push_back({"xbox-converted", "Making the Zoo and the Ape", {"ai_zoo_ape"}, {"xbox"}, {}});
                    Part pack{"xbox-pack", "Making the Xbox textures and movies", {"xbox_texture_pack ai"}, {"xbox"}, {}};
                    if (models)
                        pack.makers.push_back("ai_xbox_skaters");
                    parts.push_back(pack);
                    panelAfter = {"xbox-converted", "xbox"};
                }
                if (gc)
                {
                    parts.push_back({"estate", "Making the Estate level", {"ai_estate"}, {"gamecube"}, {}});
                    panelAfter.push_back("estate");
                }
            }
            else if (game == "dm2" && xbox)
            {
                parts.push_back({"xbox-levels", "Making the Xbox levels and themes", {"dm2_xbox"}, {"xbox"}, {}});
                parts.push_back({"xbox-pack", "Making the Xbox textures", {"xbox_texture_pack dm2"}, {"xbox"}, {}});
                panelAfter = {"xbox-levels"};
            }
            else if (game == "bmx" && (xbox || gc))
            {
                Part made{"releases", "Making the Xbox and GameCube content", {"bmx_releases"}, {}, {}};
                for (const char *release : {"xbox", "gamecube"})
                    if (has(releases, release))
                        made.releases.push_back(release);
                parts.push_back(made);
                if (xbox)
                    parts.push_back({"xbox-pack", "Making the Xbox textures", {"xbox_texture_pack bmx"}, {"xbox"}, {}});
            }
            // The bottom screen's maps and goals, from the PS2 disc and what was made above (the Zoo, Estate, the
            // Xbox scripts, DM2's Xbox levels).
            parts.push_back({"panel", "Making the bottom screen's maps", {"panel_maps_" + game}, {}, panelAfter});
            return parts;
        }

        // The PS2 disc a game's extras are made from, by its boot ELF's SHA-256 (the setup's record of it,
        // game_setup.cpp: "sha256 <hex>"); "by-hand" for files put there by hand.
        std::string ps2Identity(const fs::path &gameDir)
        {
            std::ifstream in(gameDir / "disc.manifest", std::ios::binary);
            for (std::string line; std::getline(in, line);)
                if (line.rfind("sha256 ", 0) == 0)
                    return line.substr(7);
            return "by-hand";
        }

        // A release's disc, by its executable's SHA-256 (the one disc of it the app makes extras from).
        std::string releaseIdentity(const std::string &game, const std::string &release)
        {
            for (const ExtrasDisc *disc : extrasDiscsOf(game))
                if (release == disc->release)
                    return disc->sha256;
            return "unknown";
        }

        constexpr char kKeyHead[] = "# z-axis extras part 1";

        // The key a part has when made by these makers' versions (now's, or 1.1.0's: every maker at 0) from the
        // game's discs, after the parts it reads, whose keys are `keys` (by folder).
        std::string partKey(const std::string &game, const Part &part, const std::string &ps2, bool asBefore,
                            const std::map<std::string, std::string> &keys)
        {
            std::string key = std::string(kKeyHead) + "\npart " + part.folder + "\n";
            for (const std::string &maker : part.makers)
                key += "maker " + maker + " " + std::to_string(makerVersion(maker, asBefore)) + "\n";
            key += "input ps2 " + ps2 + "\n";
            for (const std::string &release : part.releases)
                key += "input " + release + " " + releaseIdentity(game, release) + "\n";
            for (const std::string &folder : part.after)
                if (const auto it = keys.find(folder); it != keys.end())
                    key += "after " + folder + " " +
                           sha256Hex(reinterpret_cast<const uint8_t *>(it->second.data()), it->second.size()).substr(0, 16) + "\n";
            return key;
        }

        fs::path keyFile(const fs::path &gameDir, const std::string &folder)
        {
            return extrasFolder(gameDir) / (folder + ".key");
        }

        std::string readText(const fs::path &path)
        {
            std::ifstream in(path, std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            return in ? text.str() : std::string();
        }

        // Extras made before parts had keys (1.1.0's: its record "# z-axis extras 1", or none for a game set up with
        // no extras discs): a part there with no key was made by every maker's version 0.
        bool madeBeforeKeys(const fs::path &gameDir)
        {
            std::error_code ec;
            std::ifstream in(gameDir / "extras.manifest", std::ios::binary);
            std::string head;
            if (!std::getline(in, head))
                return fs::is_directory(extrasFolder(gameDir), ec);
            return head != kManifestHead;
        }

        // Each part as it is and as it would be made now.
        struct PartState
        {
            Part part;
            std::string want;  // its key now
            std::string has;   // the key of what is there ("" none)
            bool keyed = false; // `has` is its key file's (else 1.1.0's, worked out)
            bool stale() const { return has != want; }
        };

        // The parts of `game` for these releases, each with the key it would have now and the one it has. A part
        // that reads another reads it as it will be once made, or as it is when it waits for its disc (one of the
        // `unopened` releases), so its key follows that part's.
        std::vector<PartState> partStates(const std::string &game, const fs::path &gameDir,
                                          const std::vector<std::string> &releases, bool models,
                                          const std::vector<std::string> &unopened)
        {
            const std::string ps2 = ps2Identity(gameDir);
            const bool before = madeBeforeKeys(gameDir);
            std::map<std::string, std::string> beforeKeys, readKeys;
            std::vector<PartState> out;
            std::error_code ec;
            for (const Part &part : partsOf(game, releases, models))
            {
                PartState state{part, partKey(game, part, ps2, false, readKeys), {}, false};
                const std::string beforeKey = partKey(game, part, ps2, true, beforeKeys);
                if (fs::is_regular_file(keyFile(gameDir, part.folder), ec))
                {
                    state.has = readText(keyFile(gameDir, part.folder));
                    state.keyed = true;
                }
                else if (before && fs::is_directory(extrasFolder(gameDir) / part.folder, ec))
                    state.has = beforeKey;
                beforeKeys[part.folder] = state.has.empty() ? beforeKey : state.has;
                // (a part left as it is for want of its disc is read as it is)
                const bool waits = state.stale() && std::any_of(part.releases.begin(), part.releases.end(),
                                                                [&](const std::string &r) { return has(unopened, r); });
                readKeys[part.folder] = waits ? state.has : state.want;
                out.push_back(std::move(state));
            }
            return out;
        }

        // A part's files put in place of its earlier ones, then its key.
        bool placePart(const fs::path &gameDir, const std::string &folder, const fs::path &made, const std::string &key,
                       std::string *error)
        {
            std::error_code ec;
            const fs::path there = extrasFolder(gameDir) / folder, old = extrasFolder(gameDir) / (folder + ".old");
            fs::remove(keyFile(gameDir, folder), ec);
            fs::remove_all(old, ec);
            if (fs::exists(there, ec))
                fs::rename(there, old, ec);
            if (!ec)
                fs::rename(made, there, ec);
            if (ec)
            {
                *error = "The extras could not be put in place: " + ec.message() + ".";
                return false;
            }
            fs::remove_all(old, ec);
            return writeFile(keyFile(gameDir, folder), key, error);
        }
    }

    StaleExtras staleExtras(const std::string &game, const fs::path &gameDir, bool models)
    {
        StaleExtras out;
        std::error_code ec;
        if (!fs::is_directory(gameDir / "disc", ec))
            return out;
        const std::vector<RecordedDisc> recorded = recordedExtras(gameDir);
        std::vector<std::string> releases;
        for (const RecordedDisc &disc : recorded)
            releases.push_back(disc.release);
        out.unkeyed = madeBeforeKeys(gameDir);
        for (const PartState &state : partStates(game, gameDir, releases, models, {}))
        {
            if (!state.stale())
                continue;
            out.parts.push_back(state.part.folder);
            for (const RecordedDisc &disc : recorded)
                if (has(state.part.releases, disc.release) &&
                    std::none_of(out.discs.begin(), out.discs.end(), [&](const RecordedDisc &d) { return d.source == disc.source; }))
                    out.discs.push_back(disc);
        }
        return out;
    }

    ExtrasResult makeExtras(const std::string &game, const fs::path &gameDir, std::vector<ExtrasSource> &sources,
                            const XboxModelMaker &models, const ExtrasProgressFn &progress,
                            const std::vector<RecordedDisc> &unopened)
    {
        ExtrasResult result;
        std::error_code ec;
        const fs::path made = extrasFolder(gameDir);
        const SourceFiles *xbox = nullptr, *gc = nullptr;
        ImageFiles xboxAssets, gcAssets;
        DiscFiles *xboxDisc = nullptr;
        std::vector<std::string> releases, unopenedReleases;
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
            releases.push_back(s.disc->release);
        }
        for (const RecordedDisc &disc : unopened)
            if (!has(releases, disc.release))
            {
                releases.push_back(disc.release);
                unopenedReleases.push_back(disc.release);
            }
        const fs::path ps2Disc = gameDir / "disc";
        FolderFiles ps2;
        if (!ps2.open(ps2Disc, {"levels"}))
        {
            result.message = titleOf(game) + " is not set up: its extras need its PS2 disc's files first.";
            return result;
        }

        // What each part is now, and what it would be. A part with nothing to make it from now (its disc did not
        // open) stays as it is, its key with it.
        const std::vector<PartState> states = partStates(game, gameDir, releases, bool(models), unopenedReleases);
        fs::create_directories(made, ec);
        // (1.1.0's parts that are as they would be made now: their keys, so they count as made)
        for (const PartState &state : states)
            if (!state.keyed && !state.has.empty() && !state.stale())
            {
                std::string error;
                if (!writeFile(keyFile(gameDir, state.part.folder), state.want, &error))
                {
                    result.message = error;
                    return result;
                }
            }
        // The record of the discs, before anything is made: what is there is then always the record's or
        // being made from it (a part made but not yet keyed is made again).
        std::string manifest = std::string(kManifestHead) + "\n";
        for (const ExtrasSource &s : sources)
            manifest += std::string(s.disc->release) + "\t" + s.source + "\n";
        for (const RecordedDisc &disc : unopened)
            if (has(unopenedReleases, disc.release))
                manifest += disc.release + "\t" + disc.source + "\n";
        if (std::string error; !writeFile(gameDir / "extras.manifest", manifest, &error))
        {
            result.message = error;
            return result;
        }
        // What no disc makes now (a disc taken away), and what a stopped make left: 1.1.0's whole folder being made,
        // a part's .partial and .old.
        fs::remove_all(gameDir / "extras.partial", ec);
        for (const auto &entry : fs::directory_iterator(made, ec))
        {
            const fs::path path = entry.path();
            std::string folder = path.filename().string();
            if (path.extension() == ".key")
                folder = path.stem().string();
            const bool kept = std::any_of(states.begin(), states.end(), [&](const PartState &s) { return s.part.folder == folder; });
            if (!kept || path.extension() == ".partial" || path.extension() == ".old")
            {
                std::error_code removeEc;
                fs::remove_all(path, removeEc);
            }
        }

        // Each part whose key differs, made in <folder>.partial beside it and put in its place, its key last.
        std::vector<const PartState *> toMake;
        for (const PartState &state : states)
        {
            if (!state.stale())
                continue;
            const auto missing = std::find_if(state.part.releases.begin(), state.part.releases.end(),
                                              [&](const std::string &r) { return has(unopenedReleases, r); });
            if (missing == state.part.releases.end())
            {
                toMake.push_back(&state);
                continue;
            }
            for (const RecordedDisc &disc : unopened)
                if (disc.release == *missing &&
                    std::none_of(result.missing.begin(), result.missing.end(), [&](const RecordedDisc &d) { return d.source == disc.source; }))
                    result.missing.push_back(disc);
            result.kept.push_back(state.part.folder);
        }
        std::vector<std::string> &log = result.log;
        const auto partFolder = [&](const std::string &folder) { return made / folder; };
        for (size_t i = 0; i < toMake.size(); ++i)
        {
            const Part &part = toMake[i]->part;
            if (progress && !progress(part.name, i, toMake.size()))
            {
                result.cancelled = true;
                result.message = "Stopped: what is made is kept, the rest is made the next time.";
                return result;
            }
            const fs::path out = made / (part.folder + ".partial");
            fs::remove_all(out, ec);
            std::string error;
            bool ok = true;
            // (a disc that opened with none of the files its parts read: the part is not made from it)
            for (const std::string &release : part.releases)
                if ((release == "xbox" && !xbox && !(part.folder == "releases" && gc)) ||
                    (release == "gamecube" && !gc && !(part.folder == "releases" && xbox)))
                {
                    error = "The " + std::string(release == "xbox" ? "Xbox" : "GameCube") + " disc has none of the files " +
                            titleOf(game) + "'s extras are made from.";
                    ok = false;
                }
            if (!ok)
                ;
            else if (part.folder == "xbox" && game == "ai")
                ok = makeAiXboxFiles(*xbox, out, log, &error);
            else if (part.folder == "xbox-converted")
                ok = makeAiZooAndApe(*xbox, ps2Disc, out, log, &error);
            else if (part.folder == "xbox-pack")
            {
                // DM2's and BMX XXX's Xbox textures: their levels' mostly at twice the PS2's size (their riders and
                // menus are mostly the PS2's own size on the Xbox); no movies. BMX XXX's levels' props too.
                XboxTexturePackOptions options;
                options.movies = game == "ai";
                options.archives = game == "bmx";
                ok = makeXboxTexturePack(ps2, *xbox, out, options, log, &error);
                if (ok && game == "ai" && models)
                    ok = makeAiXboxSkaters(ps2, *xbox, models, out, log, &error);
            }
            else if (part.folder == "estate")
                ok = makeAiEstate(*gc, out, log, &error);
            else if (part.folder == "xbox-levels")
            {
                Bytes xbe;
                const DiscFile *file = xboxDisc->find("default.xbe");
                if (!file || !xboxDisc->readFile(*file, xbe))
                {
                    error = "The Xbox disc's default.xbe cannot be read.";
                    ok = false;
                }
                else
                    ok = makeDm2XboxExtras(&ps2, *xbox, xbe, out, Dm2XboxOptions(), log, &error);
            }
            else if (part.folder == "releases")
                ok = makeBmxReleases(ps2, xbox, gc, out, log, &error);
            else if (part.folder == "panel")
            {
                FolderFiles disc, zoo, estate, scripts, dm2Xbox;
                if (!disc.open(ps2Disc, {"ASSETS"}))
                {
                    error = titleOf(game) + "'s PS2 disc files have no ASSETS folder.";
                    ok = false;
                }
                else
                {
                    PanelExtras extras;
                    if (has(part.after, "xbox-converted") && zoo.open(partFolder("xbox-converted") / "ASSETS" / "LEVELS" / "ZOO", {}))
                        extras.zoo = &zoo;
                    if (has(part.after, "estate") && estate.open(partFolder("estate"), {}))
                        extras.estate = &estate;
                    if (has(part.after, "xbox") && scripts.open(partFolder("xbox"), {"levels"}))
                        extras.xboxScripts = &scripts;
                    if (has(part.after, "xbox-levels") && dm2Xbox.open(partFolder("xbox-levels"), {}))
                        extras.dm2Xbox = &dm2Xbox;
                    const PanelGame which = game == "ai"    ? PanelGame::AggressiveInline
                                            : game == "dm2" ? PanelGame::DaveMirra2
                                                            : PanelGame::BmxXxx;
                    ok = makePanelMaps(which, disc, extras, out, log, &error);
                }
            }
            if (ok)
            {
                fs::create_directories(out, ec);
                ok = placePart(gameDir, part.folder, out, toMake[i]->want, &error);
            }
            if (!ok)
            {
                fs::remove_all(out, ec);
                result.message = error;
                return result;
            }
            result.made.push_back(part.folder);
        }
        if (progress)
            progress("Done", toMake.size(), toMake.size());
        result.ok = true;
        std::string names;
        for (const ExtrasSource &s : sources)
            names += std::string(names.empty() ? "" : " and ") + s.disc->title;
        if (result.made.empty() && !result.kept.empty())
            result.message = "The extras of " + titleOf(game) + " made from a disc that is not there are kept as they are.";
        else if (result.made.empty())
            result.message = "The extras of " + titleOf(game) + " are up to date.";
        else if (sources.empty() && unopened.empty())
            result.message = "The bottom screen's maps of " + titleOf(game) + " are made (no extras discs).";
        else
            result.message = names.empty() ? "The extras of " + titleOf(game) + " are made."
                                           : "The extras from " + names + " are made.";
        return result;
    }
}
