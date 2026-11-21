// Mods (collection/mods.h): the manifest, the mods of the collection's Mods
// folder and of each game's own, their order, what each game reads, and
// which mod's file wins where two have the same.
#include "collection/mods.h"

#include "collection/maps.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_zip_archive.h"
#include "zx/zal_archive.h"
#include "strings.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>
#include <system_error>

namespace zx::app
{
    namespace
    {
        namespace gf = zx::game_files;

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        const char *const kGameIds[] = {"ai", "dm2", "bmx"};
        // The games that read a mod's tuning.ini: Aggressive Inline's developer
        // tuning table (aggressive_inline_devtools.cpp) and BMX XXX's, its
        // GameCube release's on the PS2's values (bmx_xxx_tuning.cpp). Dave
        // Mirra BMX 2 has no tuning table.
        const char *const kTuningGames[] = {"ai", "bmx"};
        bool knownGame(std::string_view id)
        {
            return std::find(std::begin(kGameIds), std::end(kGameIds), id) != std::end(kGameIds);
        }

        // "a, b c" -> {"a", "b", "c"} (commas or spaces between).
        std::vector<std::string> listOf(std::string_view text, bool lowerCase)
        {
            std::vector<std::string> out;
            std::string word;
            auto flush = [&]()
            {
                if (!word.empty())
                    out.push_back(lowerCase ? lower(word) : word);
                word.clear();
            };
            for (char c : text)
            {
                if (c == ',' || c == ' ' || c == '\t')
                    flush();
                else
                    word += c;
            }
            flush();
            return out;
        }

        // A file of a mod's folder or zip, any case: its text.
        bool readModText(const std::filesystem::path &root, const char *name, std::string &text)
        {
            std::vector<uint8_t> bytes;
            if (!gf::readFileBelow(root, name, bytes))
                return false;
            text.assign(bytes.begin(), bytes.end());
            return true;
        }

        // The top of a mod: its files and folders there, upper case.
        struct Top
        {
            std::set<std::string> files, folders;
        };
        Top topOf(const std::filesystem::path &root)
        {
            Top top;
            std::error_code ec;
            if (std::filesystem::is_directory(root, ec))
            {
                for (std::filesystem::directory_iterator it(root, ec), last; !ec && it != last; it.increment(ec))
                    (it->is_directory(ec) ? top.folders : top.files).insert(upper(it->path().filename().string()));
                return top;
            }
            if (const gf::ZipPath zip = gf::zipPathOf(root); zip.zip)
                for (const std::string &file : zip.zip->filesBelow(zip.inside))
                {
                    const size_t slash = file.find('/');
                    if (slash == std::string::npos)
                        top.files.insert(upper(file));
                    else
                        top.folders.insert(upper(file.substr(0, slash)));
                }
            return top;
        }

        // Files at a mod's top that are about the mod, not the game's.
        bool aboutTheMod(const std::string &name)
        {
            static const char *const exact[] = {"MOD.INI", "MAP.INI", "TUNING.INI", "TUNING-REFERENCE.INI", ".DS_STORE"};
            if (std::find_if(std::begin(exact), std::end(exact), [&](const char *e) { return name == e; }) != std::end(exact))
                return true;
            if (name.compare(0, 6, "README") == 0 || name.compare(0, 7, "LICENSE") == 0)
                return true;
            const size_t dot = name.rfind('.');
            const std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
            return ext == ".TXT" || ext == ".MD" || ext == ".BMP" || ext == ".PNG" || ext == ".JPG";
        }

        const char *gameTitle(std::string_view game)
        {
            if (game == "ai")
                return "Aggressive Inline";
            if (game == "dm2")
                return "Dave Mirra BMX 2";
            if (game == "bmx")
                return "BMX XXX";
            return "that game";
        }

        // The texture folder names a game's pictures are under in a mod.
        std::vector<std::string> textureNames(std::string_view game)
        {
            std::vector<std::string> names = {upper(std::string(game))};
            if (*gameSerial(game))
                names.push_back(gameSerial(game));
            return names;
        }

        // A folder's name as it is on the host (any case), "" for none.
        std::string hostName(const std::filesystem::path &folder, const std::string &upperName)
        {
            std::error_code ec;
            for (std::filesystem::directory_iterator it(folder, ec), last; !ec && it != last; it.increment(ec))
                if (upper(it->path().filename().string()) == upperName)
                    return it->path().filename().string();
            return {};
        }

        // What a mod holds, from its layout; its manifest and map read.
        void describe(Mod &mod)
        {
            const Top top = topOf(mod.folder);
            std::string text;
            if (top.files.count("MOD.INI") && readModText(mod.folder, kModManifestFile, text))
            {
                ModManifest manifest;
                std::string error;
                if (parseModManifest(text, manifest, &error))
                {
                    mod.manifest = true;
                    mod.name = manifest.id;
                    mod.title = manifest.name.empty() ? manifest.id : manifest.name;
                    mod.version = manifest.version;
                    mod.author = manifest.author;
                    mod.games = manifest.games;
                    mod.needs = manifest.needs;
                    mod.conflicts = manifest.conflicts;
                }
                else if (!error.empty())
                    mod.problem = "mod.ini: " + error;
                CustomMap map;
                if (parseMapDescription(text, map, nullptr, mod.ownGame))
                {
                    mod.map = map;
                    mod.contents |= kModMap;
                }
            }
            if (!mod.isMap() && top.files.count("MAP.INI") && readModText(mod.folder, kMapDescriptionFile, text))
            {
                CustomMap map;
                std::string error;
                if (parseMapDescription(text, map, &error, mod.ownGame))
                {
                    mod.map = map;
                    mod.contents |= kModMap;
                }
                else
                    mod.problem = "map.ini: " + error;
            }
            if (mod.isMap())
            {
                mod.map.folder = mod.folder;
                mod.map.folderName = mod.folder.filename().string();
                if (mod.title.empty() || !mod.manifest)
                    mod.title = mod.map.name;
                if (mod.author.empty())
                    mod.author = mod.map.author;
            }
            if (top.files.count("TUNING.INI"))
                mod.contents |= kModTuning;
            for (const std::string &folder : top.folders)
            {
                if (folder == "MUSIC")
                    mod.contents |= kModMusic;
                else if (folder == "TEXTURES")
                    mod.contents |= kModTextures;
                else if (folder != "__MACOSX")
                    mod.contents |= kModFiles;
            }
            for (const std::string &file : top.files)
                if (!aboutTheMod(file))
                    mod.contents |= kModFiles;
            // Textures for a game the mod is not for.
            if (mod.contents & kModTextures)
            {
                const Top textures = topOf(mod.folder / (mod.zip ? std::string("Textures") : hostName(mod.folder, "TEXTURES")));
                for (const char *game : kGameIds)
                {
                    if (mod.forGame(game))
                        continue;
                    for (const std::string &name : textureNames(game))
                        if (textures.folders.count(name))
                            mod.notes.push_back("Textures/" + name + " not read, the mod is not for " + gameTitle(game) + ".");
                }
            }
            // tuning.ini for games none of which reads one: not tuning, and no logic changed by it.
            if ((mod.contents & kModTuning) && (!mod.ownGame.empty() || !mod.games.empty()) &&
                std::none_of(std::begin(kTuningGames), std::end(kTuningGames), [&](const char *game) { return mod.forGame(game); }))
            {
                mod.contents &= ~kModTuning;
                mod.notes.push_back("tuning.ini not read, only Aggressive Inline and BMX XXX have a tuning table.");
            }
        }

        bool isZipName(const std::filesystem::path &path)
        {
            return upper(path.extension().string()) == ".ZIP";
        }

        std::vector<std::filesystem::path> entriesOf(const std::filesystem::path &root)
        {
            std::vector<std::filesystem::path> entries;
            std::error_code ec;
            for (std::filesystem::directory_iterator it(root, ec), last; !ec && it != last; it.increment(ec))
                if (it->is_directory(ec) || (it->is_regular_file(ec) && isZipName(it->path())))
                    entries.push_back(it->path());
            std::sort(entries.begin(), entries.end(), [](const std::filesystem::path &a, const std::filesystem::path &b)
                      { return a.filename().string() < b.filename().string(); });
            return entries;
        }

        bool contains(const std::vector<std::string> &list, const std::string &item)
        {
            return std::find(list.begin(), list.end(), item) != list.end();
        }

        // The archive a mod's file is a single entry of ("ASSETS/UILIB.ZAL"
        // for ASSETS/UILIB.ZAL/CommonFonts.zif), "" for a file of no archive.
        std::string archiveOf(const std::string &normalized)
        {
            const size_t slash = normalized.rfind('/');
            return slash != std::string::npos && gf::isArchivePath(normalized.substr(0, slash)) ? normalized.substr(0, slash) : std::string();
        }

        // An archive entry's key for the winners: its archive and its id, as
        // the names a mod may give it mean it (zal::findEntry): "0x..." an
        // id, a number a zfile library's group, else a name's hash.
        std::string entryKey(const std::string &archive, std::string_view name)
        {
            namespace zal = zx::zal;
            const std::string_view stem = name.substr(0, name.find('.'));
            if (stem.size() == 10 && stem[0] == '0' && (stem[1] == 'x' || stem[1] == 'X') &&
                std::all_of(stem.begin() + 2, stem.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }))
                return archive + " entry " + zal::idName(uint32_t(std::stoul(std::string(stem.substr(2)), nullptr, 16)));
            if (!stem.empty() && std::all_of(stem.begin(), stem.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) || c == '-'; }))
                return archive + " entry " + std::string(stem);
            return archive + " entry " + zal::idName(zal::nameHash(name));
        }

        // A mod's things for the winners (key: what is shown): disc files by
        // path, archive entries by entryKey, songs by number, tuning.ini,
        // textures by file name.
        std::map<std::string, std::string> thingsOf(const Mod &mod, std::string_view game)
        {
            std::map<std::string, std::string> things;
            auto add = [&things](std::string key, std::string shown) { things.emplace(std::move(key), std::move(shown)); };
            if (mod.contents & (kModFiles | kModMusic | kModTuning))
                for (const std::string &file : gf::listFilesBelow(mod.folder))
                {
                    const std::string path = gf::normalizePath(file);
                    const size_t slash = path.find('/');
                    const std::string first = slash == std::string::npos ? path : path.substr(0, slash);
                    if (first == "TEXTURES" || first == "__MACOSX")
                        continue;
                    if (first == "MUSIC")
                    {
                        const std::string song = path.substr(slash + 1u);
                        if (song.find('/') == std::string::npos && song.size() >= 2u && std::isdigit(static_cast<unsigned char>(song[0])) &&
                            std::isdigit(static_cast<unsigned char>(song[1])) &&
                            (song.size() == 2u || !std::isdigit(static_cast<unsigned char>(song[2]))))
                            add("song " + song.substr(0, 2), "song " + song.substr(0, 2));
                        continue;
                    }
                    if (slash == std::string::npos)
                    {
                        if (path == "TUNING.INI")
                            add("tuning.ini", "tuning.ini");
                        else if (!aboutTheMod(path))
                            add(path, path);
                        continue;
                    }
                    if (const std::string archive = archiveOf(path); !archive.empty())
                    {
                        const std::string name = file.substr(file.rfind('/') + 1u);
                        if (!gf::ignoredInArchiveFolder(name))
                            add(entryKey(archive, name), archive + "/" + name);
                        continue;
                    }
                    add(path, path);
                }
            if (const std::filesystem::path textures = modTextureFolder(mod, game); !textures.empty())
                for (const std::string &file : gf::listFilesBelow(textures))
                {
                    // (a picture's key is its file name, wherever it is below)
                    const std::string path = gf::normalizePath(file);
                    const size_t slash = path.rfind('/');
                    const std::string picture = "texture " + (slash == std::string::npos ? path : path.substr(slash + 1u));
                    add(picture, picture);
                }
            return things;
        }

        // The first bytes of a file the game file system opens: its directory, when it is an archive.
        bool readArchiveDirectory(gf::GameFile &file, zx::zal::Archive &archive, std::string &error)
        {
            std::vector<uint8_t> head(20);
            if (file.read(0, head.data(), head.size()) != head.size())
            {
                error = "too short to be one";
                return false;
            }
            const uint64_t bytes = zx::zal::directoryBytes(head.data(), head.size());
            if (bytes == 0 || bytes > file.size())
            {
                error = "not a ZDIR archive or a zfile library";
                return false;
            }
            head.resize(static_cast<size_t>(bytes));
            if (file.read(0, head.data(), head.size()) != head.size())
            {
                error = "cannot be read";
                return false;
            }
            return zx::zal::readDirectory(head.data(), head.size(), archive, &error);
        }
    }

    bool parseModManifest(std::string_view text, ModManifest &out, std::string *error)
    {
        ModManifest manifest;
        std::string section;
        bool inMod = false;
        std::istringstream in{std::string(text)};
        std::string line;
        std::vector<std::string> unknownGames;
        while (std::getline(in, line))
        {
            std::string content = trim(line);
            if (const size_t semicolon = content.find(';'); semicolon != std::string::npos)
                content = trim(std::string_view(content).substr(0, semicolon));
            if (content.empty() || content[0] == '#')
                continue;
            if (content.front() == '[' && content.back() == ']')
            {
                section = lower(trim(std::string_view(content).substr(1, content.size() - 2)));
                inMod = inMod || section == "mod";
                continue;
            }
            if (section != "mod")
                continue;
            const size_t equals = content.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = lower(trim(std::string_view(content).substr(0, equals)));
            const std::string value = trim(std::string_view(content).substr(equals + 1));
            if (key == "id")
                manifest.id = value;
            else if (key == "name")
                manifest.name = value;
            else if (key == "version")
                manifest.version = value;
            else if (key == "author")
                manifest.author = value;
            else if (key == "games")
                for (const std::string &game : listOf(value, true))
                {
                    if (!knownGame(game))
                        unknownGames.push_back(game);
                    else if (!contains(manifest.games, game))
                        manifest.games.push_back(game);
                }
            else if (key == "needs")
                manifest.needs = listOf(value, false);
            else if (key == "conflicts")
                manifest.conflicts = listOf(value, false);
        }
        auto fail = [&](const std::string &why)
        {
            if (error)
                *error = why;
            return false;
        };
        if (!inMod)
            return fail("");
        if (manifest.id.empty())
            return fail("[mod] has no id");
        if (manifest.id.find_first_of("=/\\") != std::string::npos)
            return fail("[mod] id has = / or \\ in it");
        if (!unknownGames.empty())
            return fail("[mod] games: " + unknownGames.front() + " is not ai, dm2 or bmx");
        if (manifest.games.empty())
            return fail("[mod] has no games (ai, dm2, bmx)");
        out = std::move(manifest);
        return true;
    }

    std::string Mod::orderKey() const
    {
        return place + "/" + folder.filename().string();
    }

    bool Mod::forGame(std::string_view game) const
    {
        if (!ownGame.empty())
            return game == ownGame;
        return std::find(games.begin(), games.end(), game) != games.end();
    }

    const char *gameSerial(std::string_view game)
    {
        if (game == "ai")
            return "SLUS-20327";
        if (game == "dm2")
            return "SLUS-20159";
        if (game == "bmx")
            return "SLUS-20415";
        return "";
    }

    std::vector<Mod> listMods(const std::filesystem::path &modsRoot, const std::filesystem::path &discRoot,
                              const std::vector<std::string> &off, std::string_view game)
    {
        std::vector<Mod> mods;
        std::error_code ec;
        if (modsRoot.empty() || !std::filesystem::is_directory(modsRoot, ec))
            return mods;
        // The loose layout's folders: the disc's own top-level folders and the soundtrack's.
        std::vector<std::string> loose = {"MUSIC"};
        if (!discRoot.empty())
            for (std::filesystem::directory_iterator it(discRoot, ec), last; !ec && it != last; it.increment(ec))
                if (it->is_directory(ec))
                    loose.push_back(upper(it->path().filename().string()));
        for (const std::filesystem::path &entry : entriesOf(modsRoot))
        {
            const std::string name = entry.filename().string();
            if (std::find(loose.begin(), loose.end(), upper(name)) != loose.end())
                continue;
            Mod mod;
            mod.name = name;
            mod.title = name;
            mod.folder = entry;
            mod.zip = !std::filesystem::is_directory(entry, ec);
            mod.ownGame = std::string(game);
            mod.place = game.empty() ? std::string(kModsFolder) : "games/" + std::string(game) + "/" + kModsFolder;
            describe(mod);
            // A custom map is not a mod (collection/maps.h): no boot reads it.
            if (mod.isMap() || isMapFolder(entry))
                continue;
            // (its key stays its folder's name, as the settings knew it; the
            // manifest gives its name and version)
            mod.name = name;
            mod.games = game.empty() ? std::vector<std::string>{} : std::vector<std::string>{std::string(game)};
            // (a mod.ini that does not parse leaves it as it was before
            // manifests: read; a zip that cannot be read is not)
            mod.problem = mod.zip && !gf::ZipArchive::open(entry) ? "the zip cannot be opened" : "";
            mod.on = std::find(off.begin(), off.end(), name) == off.end();
            mod.read = mod.on && mod.problem.empty();
            mods.push_back(std::move(mod));
        }
        return mods;
    }

    std::vector<CustomMap> listGameMaps(const std::filesystem::path &modsRoot, std::string_view game)
    {
        std::vector<CustomMap> maps;
        std::error_code ec;
        if (modsRoot.empty() || !std::filesystem::is_directory(modsRoot, ec))
            return maps;
        for (const std::filesystem::path &entry : entriesOf(modsRoot))
        {
            Mod mod;
            mod.name = entry.filename().string();
            mod.folder = entry;
            mod.zip = !std::filesystem::is_directory(entry, ec);
            mod.ownGame = std::string(game);
            mod.place = "games/" + std::string(game) + "/" + kModsFolder;
            if (mod.zip && !gf::ZipArchive::open(entry))
                continue;
            describe(mod);
            if (!mod.isMap())
                continue;
            CustomMap map = mod.map;
            map.where = mod.orderKey();
            maps.push_back(std::move(map));
        }
        return maps;
    }

    std::vector<Mod> listSharedMods(const std::filesystem::path &sharedRoot)
    {
        std::vector<Mod> mods;
        std::error_code ec;
        if (sharedRoot.empty() || !std::filesystem::is_directory(sharedRoot, ec))
            return mods;
        std::map<std::string, std::string> seen; // key -> where
        for (const std::filesystem::path &entry : entriesOf(sharedRoot))
        {
            Mod mod;
            mod.name = entry.filename().string();
            mod.title = mod.name;
            mod.folder = entry;
            mod.zip = !std::filesystem::is_directory(entry, ec);
            mod.place = kModsFolder;
            if (mod.zip && !gf::ZipArchive::open(entry))
                mod.problem = "the zip cannot be opened";
            else
                describe(mod);
            if (mod.problem.empty() && !mod.manifest && !mod.isMap())
                mod.problem = "its mod.ini names no game";
            if (mod.problem.empty())
            {
                if (auto it = seen.find(mod.name); it != seen.end())
                    mod.problem = it->second + " has the same id";
                else
                    seen.emplace(mod.name, mod.orderKey());
            }
            mods.push_back(std::move(mod));
        }
        return mods;
    }

    void orderMods(std::vector<Mod> &mods, const std::vector<std::string> &order)
    {
        auto rank = [&order](const Mod &mod)
        {
            const auto it = std::find(order.begin(), order.end(), mod.orderKey());
            return it == order.end() ? order.size() : static_cast<size_t>(it - order.begin());
        };
        std::stable_sort(mods.begin(), mods.end(), [&](const Mod &a, const Mod &b) { return rank(a) < rank(b); });
    }

    std::vector<std::string> modOrderOf(const std::vector<Mod> &mods)
    {
        std::vector<std::string> order;
        for (const Mod &mod : mods)
            order.push_back(mod.orderKey());
        return order;
    }

    std::vector<Mod> modsForGame(std::string_view game, const std::vector<Mod> &own, const std::vector<Mod> &shared,
                                 const std::vector<std::string> &order, const std::vector<std::string> &off)
    {
        std::vector<Mod> mods;
        for (const Mod &mod : own)
            mods.push_back(mod);
        for (const Mod &mod : shared)
            if (!mod.isMap() && mod.forGame(game))
                mods.push_back(mod);
        orderMods(mods, order);
        std::map<std::string, size_t> first; // key -> the first mod with it
        for (size_t i = 0; i < mods.size(); ++i)
        {
            Mod &mod = mods[i];
            mod.on = std::find(off.begin(), off.end(), mod.name) == off.end();
            mod.read = mod.on && mod.problem.empty();
            mod.why = !mod.problem.empty() ? mod.problem : mod.on ? std::string() : std::string("Off");
            if (auto [it, fresh] = first.emplace(mod.name, i); !fresh && mod.problem.empty())
            {
                mod.read = false;
                mod.why = mods[it->second].orderKey() + " has the same name";
            }
        }
        auto readKey = [&](const std::string &key)
        {
            for (const Mod &mod : mods)
                if (mod.name == key && mod.read)
                    return true;
            return false;
        };
        auto hasKey = [&](const std::string &key)
        {
            for (const Mod &mod : mods)
                if (mod.name == key)
                    return true;
            return false;
        };
        // Needs first (a mod whose need is not read is not), then the
        // conflicts with what is read, then the needs again (a need left out
        // by a conflict).
        auto settleNeeds = [&]()
        {
            for (bool changed = true; changed;)
            {
                changed = false;
                for (Mod &mod : mods)
                    if (mod.read)
                        for (const std::string &need : mod.needs)
                            if (!readKey(need))
                            {
                                mod.read = false;
                                mod.why = "needs " + need + (hasKey(need) ? ", which is not read" : ", which is missing");
                                changed = true;
                                break;
                            }
            }
        };
        settleNeeds();
        for (Mod &mod : mods)
            if (mod.read)
                for (const std::string &other : mod.conflicts)
                    if (other != mod.name && readKey(other))
                    {
                        mod.read = false;
                        mod.why = "clashes with " + other + ", which is on";
                        break;
                    }
        settleNeeds();
        // Each mod after the mods it needs: the first whose needs are all
        // placed goes next, so the order moves only where a need asks.
        std::vector<Mod> placed;
        std::vector<bool> done(mods.size(), false);
        std::set<std::string> placedKeys;
        while (placed.size() < mods.size())
        {
            size_t next = mods.size();
            for (size_t i = 0; i < mods.size() && next == mods.size(); ++i)
            {
                if (done[i])
                    continue;
                bool ready = true;
                if (mods[i].read)
                    for (const std::string &need : mods[i].needs)
                        ready = ready && placedKeys.count(need) != 0;
                if (ready)
                    next = i;
            }
            if (next == mods.size()) // (needs in a loop: none of them read)
                for (size_t i = 0; i < mods.size(); ++i)
                    if (!done[i])
                    {
                        mods[i].read = false;
                        mods[i].why = "its needs form a loop";
                        next = i;
                        break;
                    }
            done[next] = true;
            if (mods[next].read)
                placedKeys.insert(mods[next].name);
            placed.push_back(std::move(mods[next]));
        }
        return placed;
    }

    std::vector<std::filesystem::path> modsRoots(const std::filesystem::path &modsRoot, const std::vector<Mod> &mods)
    {
        std::vector<std::filesystem::path> roots;
        roots.push_back(modsRoot);
        for (const Mod &mod : mods)
            if (mod.read && !mod.isMap())
                roots.push_back(mod.folder);
        return roots;
    }

    std::filesystem::path modTextureFolder(const Mod &mod, std::string_view game)
    {
        if (!(mod.contents & kModTextures) || !mod.forGame(game))
            return {};
        std::error_code ec;
        const bool folder = std::filesystem::is_directory(mod.folder, ec);
        const std::string texturesName = folder ? hostName(mod.folder, "TEXTURES") : std::string("Textures");
        if (texturesName.empty())
            return {};
        const std::filesystem::path textures = mod.folder / texturesName;
        const Top top = topOf(textures);
        for (const std::string &name : textureNames(game))
            if (top.folders.count(name))
                // (a zip's paths are compared without case: the game's id as it is written)
                return textures / (folder ? hostName(textures, name) : name == upper(std::string(game)) ? std::string(game) : name);
        // A mod of this game alone may have its pictures in Textures itself.
        const bool alone = !mod.ownGame.empty() || mod.games.size() == 1u;
        if (!alone)
            return {};
        bool ownFiles = !top.files.empty();
        for (const std::string &sub : top.folders)
        {
            bool someGames = false;
            for (const char *other : kGameIds)
                for (const std::string &name : textureNames(other))
                    someGames = someGames || sub == name;
            ownFiles = ownFiles || !someGames;
        }
        return ownFiles ? textures : std::filesystem::path();
    }

    std::vector<std::filesystem::path> texturePacks(const std::vector<Mod> &mods, std::string_view game)
    {
        std::vector<std::filesystem::path> packs;
        for (const Mod &mod : mods)
            if (mod.read && !mod.isMap())
                if (std::filesystem::path folder = modTextureFolder(mod, game); !folder.empty())
                    packs.push_back(std::move(folder));
        return packs;
    }

    void checkArchiveEntries(std::vector<Mod> &mods, const std::filesystem::path &modsRoot, const std::filesystem::path &discRoot)
    {
        std::vector<std::filesystem::path> below = {modsRoot}; // (as modsRoots gives them)
        for (Mod &mod : mods)
        {
            mod.entriesNotRead.clear();
            if (!mod.read || mod.isMap())
                continue;
            if (mod.contents & kModFiles)
            {
                std::map<std::string, std::vector<std::string>> entries; // archive -> its entries' names
                for (const std::string &file : gf::listFilesBelow(mod.folder))
                {
                    const std::string path = gf::normalizePath(file);
                    if (const std::string archive = archiveOf(path); !archive.empty())
                    {
                        const std::string name = file.substr(file.rfind('/') + 1u);
                        if (!gf::ignoredInArchiveFolder(name))
                            entries[archive].push_back(name);
                        continue;
                    }
                    // A file in a folder inside an archive's.
                    for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', slash + 1u))
                        if (gf::isArchivePath(path.substr(0, slash)))
                        {
                            mod.entriesNotRead.push_back(file + " is in a folder inside " + path.substr(0, slash) + "'s, where entries are not");
                            break;
                        }
                }
                gf::GameFileRoots roots;
                roots.disc = discRoot;
                roots.mods = below;
                gf::GameFileSystem files(std::move(roots));
                for (const auto &[archive, names] : entries)
                {
                    const std::shared_ptr<gf::GameFile> file = files.open(archive);
                    zx::zal::Archive directory;
                    std::string error;
                    if (!file)
                        mod.entriesNotRead.push_back(archive + " is not one of the game's files, so its " + std::to_string(names.size()) +
                                                     (names.size() == 1 ? " entry is" : " entries are") + " not read");
                    else if (!readArchiveDirectory(*file, directory, error))
                        mod.entriesNotRead.push_back(archive + " is not an archive (" + error + "), so its entries are not read");
                    else
                        for (const std::string &name : names)
                            if (!zx::zal::findEntry(directory, name))
                                mod.entriesNotRead.push_back(archive + " has no entry " + name);
                }
            }
            below.push_back(mod.folder);
        }
    }

    std::vector<ModWinners> modWinners(const std::vector<Mod> &mods, std::string_view game)
    {
        std::vector<ModWinners> winners(mods.size());
        std::map<std::string, std::vector<size_t>> holders; // thing -> the mods read that have it, in order
        std::map<std::string, std::string> shown;           // thing -> how the page says it
        for (size_t i = 0; i < mods.size(); ++i)
        {
            if (!mods[i].read || mods[i].isMap())
                continue;
            const std::map<std::string, std::string> things = thingsOf(mods[i], game);
            winners[i].things = things.size();
            for (const auto &[thing, text] : things)
            {
                holders[thing].push_back(i);
                shown.emplace(thing, text);
            }
        }
        auto count = [](std::vector<ModOverlap> &list, size_t mod, const std::string &thing)
        {
            for (ModOverlap &overlap : list)
                if (overlap.mod == mod)
                {
                    ++overlap.count;
                    return;
                }
            list.push_back({mod, 1u, thing});
        };
        for (const auto &[thing, list] : holders)
        {
            // An entry (its key "<archive> entry <id>"): a whole archive of a
            // later mod wins over it too.
            std::vector<size_t> over = list;
            if (const size_t at = thing.find(" entry "); at != std::string::npos)
                if (const auto whole = holders.find(thing.substr(0, at)); whole != holders.end())
                    for (const size_t archive : whole->second)
                        if (archive > list.back())
                        {
                            for (const size_t entry : list)
                            {
                                count(winners[archive].winsOver, entry, shown[thing]);
                                count(winners[entry].losesTo, archive, shown[thing]);
                            }
                            over.clear();
                            break;
                        }
            if (over.size() < 2u)
                continue;
            const size_t winner = over.back();
            for (size_t k = 0; k + 1u < over.size(); ++k)
            {
                count(winners[winner].winsOver, over[k], shown[thing]);
                count(winners[over[k]].losesTo, winner, shown[thing]);
            }
        }
        return winners;
    }
}
