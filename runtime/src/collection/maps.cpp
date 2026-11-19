// Custom maps (collection/maps.h): a map folder's description, the maps of
// the maps folder, the slot each game lends, a map converted for another
// game and the folders a boot into a map reads.
#include "collection/maps.h"

#include "collection/levels/export.h"
#include "collection/mods.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_zip_archive.h"
#include "strings.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace ps2x::app
{
    namespace
    {
        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        // The slots lent, by game, each with the game's own start into it
        // (its devstart override reads PS2X_DEV_START_LEVEL):
        // - AI's free skate reaches the Tutorial's level 7 (folder TUT,
        //   always unlocked) through the Estate's entry
        //   (aggressive_inline_estate.cpp: on with PS2X_ESTATE=1 and the
        //   bundled estate folder; a mod's TUT files are read before the
        //   Estate's provider).
        // - DM2's FREERIDE reaches every level row (level select values 0 to
        //   7); the map takes Woodward Camp's (WOODWARD, value 0): the one
        //   row open from the start (the others need ProQuest), with the
        //   fewest extras of its own in the executable's tables. A FREERIDE
        //   level has no NPCs (those are ProQuest's: npcsInit); its gaps are
        //   its own faces' tags (a map's faces carry none, so none fires);
        //   no tree fade (setupTreeFade: session levels 0 and 6, Swampy
        //   Trails and Galloon); its far clip is the table's, 187 m
        //   (g_levelFarClips). Its level-point camera takes the map's own
        //   camera points (PS2X_DM2_LEVEL_CAMERA_POINTS), not Woodward
        //   Camp's in the executable's table. The park slot (FREERIDE's value 8, a park
        //   theme's files and a saved park) also holds the theme's editor
        //   objects, 4.6 MB of geometry: Woodward with them ran DM2's heap
        //   out (29.1 MB in use) and its load never ended.
        // - BMX XXX's HARDCORE TOUR reaches every level row; the map takes
        //   The Bronx's (CHL_MULY, the first row, open from the start): a
        //   challenge level, ridden with no clock (a competition row's run
        //   is two minutes). The map's goals file (OBJCTV.TXT, none in
        //   bmx_level_convert.py's) gives the level no goals, and the
        //   game counts a level's goals into the player's record of it as
        //   it loads: the start keeps The Bronx's counts and puts them back
        //   when the ride is over (PS2X_BMX_KEEP_LEVEL_GOALS). The Bronx's
        //   NPC riders are the level number's, not its folder's: they stay
        //   out unless the map keeps them (PS2X_BMX_LEVEL_NPCS).
        // AI's Tutorial has its lesson givers from its scripts (TUT.BIN and
        // TUTT.BIN, the map's own) and DM2's FREERIDE has no NPCs: no NPCs of
        // those slots come from outside the map's files.
        const std::vector<MapSlot> kMapSlots = {
            {"ai", "TUT", "ASSETS/LEVELS", "ESTATE", "PS2X_ESTATE", "", Need::Estate},
            {"dm2", "WOODWARD", "ASSETS/LEVELS", "0", "PS2X_DM2_LEVEL_CAMERA_POINTS", ""},
            {"bmx", "CHL_MULY", "ASSETS/LEVELS", "CHL_MULY", "PS2X_BMX_KEEP_LEVEL_GOALS", "PS2X_BMX_LEVEL_NPCS"},
        };

        levels::Engine engineOf(std::string_view game)
        {
            if (game == "dm2")
                return levels::Engine::Zge2;
            if (game == "bmx")
                return levels::Engine::Zge4;
            return levels::Engine::Zge3;
        }
    }

    bool parseMapDescription(std::string_view text, CustomMap &out, std::string *error)
    {
        CustomMap map;
        std::string section;
        std::string npcs;
        bool inMap = false;
        std::istringstream in{std::string(text)};
        std::string line;
        while (std::getline(in, line))
        {
            std::string content = trim(line);
            // A comment after the value too (; ...).
            if (const size_t semicolon = content.find(';'); semicolon != std::string::npos)
                content = trim(std::string_view(content).substr(0, semicolon));
            if (content.empty() || content[0] == '#')
                continue;
            if (content.front() == '[' && content.back() == ']')
            {
                section = lower(trim(std::string_view(content).substr(1, content.size() - 2)));
                inMap = inMap || section == "map";
                continue;
            }
            if (section != "map")
                continue;
            const size_t equals = content.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = lower(trim(std::string_view(content).substr(0, equals)));
            const std::string value = trim(std::string_view(content).substr(equals + 1));
            if (key == "name")
                map.name = value;
            else if (key == "game")
                map.game = lower(value);
            else if (key == "slot")
                map.slot = upper(value);
            else if (key == "preview")
                map.preview = value;
            else if (key == "author")
                map.author = value;
            else if (key == "npcs")
            {
                npcs = lower(value);
                map.slotNpcs = npcs == "slot";
            }
        }
        auto fail = [&](const char *why)
        {
            if (error)
                *error = why;
            return false;
        };
        if (!inMap)
            return fail("no [map] section");
        if (map.name.empty())
            return fail("[map] has no name");
        if (map.game.empty())
            return fail("[map] has no game (ai, dm2, bmx)");
        if (map.slot.empty())
            return fail("[map] has no slot (the level folder its files replace)");
        if (map.preview.find('/') != std::string::npos || map.preview.find('\\') != std::string::npos || map.preview == "..")
            return fail("[map] preview must be a file in the map's folder");
        if (!npcs.empty() && npcs != "none" && npcs != "slot")
            return fail("[map] npcs must be none or slot");
        out = std::move(map);
        return true;
    }

    bool isMapFolder(const std::filesystem::path &folder)
    {
        if (const ps2x::game_files::ZipPath zip = ps2x::game_files::zipPathOf(folder); zip.zip)
            return zip.zip->hasFile(zip.inside.empty() ? std::string("MAP.INI") : zip.inside + "/MAP.INI");
        std::error_code ec;
        return std::filesystem::is_regular_file(folder / kMapDescriptionFile, ec);
    }

    bool CustomMap::readPreview(std::vector<uint8_t> &out) const
    {
        return !preview.empty() && ps2x::game_files::readFileBelow(folder, preview, out);
    }

    std::vector<CustomMap> listAllMaps(const std::vector<Mod> &sharedMods, const std::filesystem::path &mapsRoot)
    {
        std::vector<CustomMap> maps;
        for (const Mod &mod : sharedMods)
            if (mod.isMap() && mod.problem.empty())
            {
                CustomMap map = mod.map;
                map.where = mod.orderKey();
                maps.push_back(std::move(map));
            }
        for (CustomMap &map : listCustomMaps(mapsRoot))
            maps.push_back(std::move(map));
        return maps;
    }

    std::vector<CustomMap> listCustomMaps(const std::filesystem::path &mapsRoot)
    {
        std::vector<CustomMap> maps;
        std::vector<std::filesystem::path> folders;
        std::error_code ec;
        if (!mapsRoot.empty() && std::filesystem::is_directory(mapsRoot, ec))
            for (std::filesystem::directory_iterator it(mapsRoot, ec), last; !ec && it != last; it.increment(ec))
                if (it->is_directory(ec) && isMapFolder(it->path()))
                    folders.push_back(it->path());
        std::sort(folders.begin(), folders.end(),
                  [](const std::filesystem::path &a, const std::filesystem::path &b) { return a.filename().string() < b.filename().string(); });
        for (const std::filesystem::path &folder : folders)
        {
            std::ifstream in(folder / kMapDescriptionFile, std::ios::binary);
            if (!in)
                continue;
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            CustomMap map;
            if (!parseMapDescription(text, map))
                continue;
            map.folder = folder;
            map.folderName = folder.filename().string();
            map.where = std::string(kMapsFolder) + "/" + map.folderName;
            maps.push_back(std::move(map));
        }
        return maps;
    }

    const std::vector<MapSlot> &mapSlots()
    {
        return kMapSlots;
    }

    const MapSlot *mapSlot(std::string_view game, std::string_view slot)
    {
        for (const MapSlot &lent : kMapSlots)
            if (game == lent.game && slot == lent.slot)
                return &lent;
        return nullptr;
    }

    const MapSlot *mapSlotOf(std::string_view game)
    {
        for (const MapSlot &lent : kMapSlots)
            if (game == lent.game)
                return &lent;
        return nullptr;
    }

    bool mapPlayableIn(const CustomMap &map, std::string_view player)
    {
        return mapSlot(map.game, map.slot) != nullptr && mapSlotOf(player) != nullptr;
    }

    bool mapNeedsConversion(const CustomMap &map, std::string_view player) { return map.game != player; }

    ConvertedMap convertMap(const CustomMap &map, std::string_view player)
    {
        const MapSlot *own = mapSlot(map.game, map.slot);
        const MapSlot *target = mapSlotOf(player);
        if (!own || !target)
            throw levels::LevelError("no slot for the map in " + std::string(own ? player : std::string_view(map.game)));
        const auto start = std::chrono::steady_clock::now();
        // The map's level files where its own game's slot has them (any
        // case; the folder read as a mod's, so a zip is read in place).
        ps2x::game_files::GameFileRoots roots;
        roots.mods = {map.folder};
        ps2x::game_files::GameFileSystem folder(std::move(roots));
        levels::LevelFiles files;
        for (const char *ext : {"GEO", "INS", "DAT", "TXR"})
        {
            const std::string path = std::string(own->folder) + "/" + own->slot + "/" + own->slot + "." + ext;
            std::shared_ptr<ps2x::game_files::GameFile> file = folder.open(path);
            if (!file || !ps2x::game_files::readAll(*file, files[ext]))
                throw levels::LevelError("the map has no " + path);
        }
        const levels::Model model = levels::exportLevel(engineOf(map.game), files);
        levels::Converted converted = levels::convertModel(model, engineOf(player));
        ConvertedMap out;
        out.files = std::move(converted.files);
        out.report = std::move(converted.report);
        for (const auto &[path, data] : out.files)
            out.bytes += data.size();
        out.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return out;
    }

    std::vector<std::filesystem::path> freeplayModsRoots(const std::filesystem::path &modsRoot, const std::vector<Mod> &mods,
                                                         const CustomMap &map, bool converted)
    {
        std::vector<std::filesystem::path> roots;
        roots.push_back(modsRoot);
        for (const Mod &mod : mods)
            if (mod.read && !mod.isMap())
                roots.push_back(mod.folder);
        if (!converted)
            roots.push_back(map.folder);
        return roots;
    }
}
