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

namespace zx::app
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
        // (its devstart override reads ZX_DEV_START_LEVEL); a game's first
        // that the setup starts takes the maps converted for it.
        // - AI's Tutorial (folder TUT, level 7) is reached in free skate
        //   only through the Estate's entry (aggressive_inline_estate.cpp:
        //   on with ZX_ESTATE=1 and the Estate made from the GameCube
        //   disc; a mod's TUT files are read before the Estate's provider).
        //   Its free skate runs the map's TUTT.BIN (the Estate's has its
        //   lessons). The Movie Lot (MOVIE, free skate's first entry) is
        //   open from the start and needs nothing: the slot without the
        //   GameCube disc. A level's records (top speed and the rest) are
        //   in the career save: the start keeps the slot level's as they
        //   were (ZX_AI_KEEP_LEVEL_RECORDS; the Estate's own records file
        //   too, as its row is the one in RAM).
        // - DM2's FREERIDE reaches every level row (level select values 0 to
        //   7); the devstart opens the row for the pick and puts the
        //   riders' progress back once the level has begun. Woodward Camp
        //   (WOODWARD, value 0) is first: the one row open from the start,
        //   with the fewest extras of its own in the executable's tables. A
        //   FREERIDE level has no NPCs (those are ProQuest's: npcsInit) and
        //   no goals (ProQuest's); its gaps are the row's table's, found by
        //   its faces' tag words (a map's faces carry those its maker gave
        //   them); the rows differ in the tree fade (setupTreeFade: session
        //   levels 0 and 6, Swampy Trails and Galloon) and the far clip
        //   (g_levelFarClips: Woodward 187 m, Commercial 331 m). The
        //   level-point camera takes the map's own camera points
        //   (ZX_DM2_LEVEL_CAMERA_POINTS), not the row's in the
        //   executable's table. A FREERIDE ride sets none of the level's
        //   saved records (its bests: measured with them zeroed first); the
        //   start keeps the row's as they were all the same, should one ever
        //   be set (ZX_DM2_KEEP_LEVEL_RECORDS). The park slot (FREERIDE's value 8) is a
        //   park saved in the park editor over its theme
        //   (ZX_DM2_PARK_FILE): a converted level there would also hold
        //   the theme's editor objects, 4.6 MB of geometry (Woodward with
        //   them ran DM2's heap out), so it takes DM2's own parks only.
        // - BMX XXX's HARDCORE TOUR reaches every level row (the devstart
        //   opens the row for the pick and puts the bits back); The Bronx
        //   (CHL_MULY, the first row, open from the start) is first. A
        //   challenge row is ridden with no clock, a competition row's run
        //   (CMP_RMPG, CMP_ROOT) is two minutes. The tour reads the slot
        //   folder's OBJCTV.TXT (the map's own goals; none in a converted
        //   map's) and counts and marks its goals into the player's record
        //   of the row's level: the start keeps that record and puts it
        //   back when the ride is over (ZX_BMX_KEEP_LEVEL_GOALS). A row's
        //   NPC riders are the level number's, not its folder's: they stay
        //   out unless the map keeps them (ZX_BMX_LEVEL_NPCS).
        constexpr const char *kDm2Camera = "ZX_DM2_LEVEL_CAMERA_POINTS";
        constexpr const char *kBmxGoals = "ZX_BMX_KEEP_LEVEL_GOALS";
        constexpr const char *kBmxNpcs = "ZX_BMX_LEVEL_NPCS";
        constexpr const char *kAiRecords = "ZX_AI_KEEP_LEVEL_RECORDS";
        constexpr const char *kDm2Records = "ZX_DM2_KEEP_LEVEL_RECORDS";
        const std::vector<MapSlot> kMapSlots = {
            {"ai", "TUT", "ASSETS/LEVELS", "ESTATE", "ZX_ESTATE", kAiRecords, "", "ESTATE", Need::Estate},
            {"ai", "MOVIE", "ASSETS/LEVELS", "0", "", kAiRecords, "", "MOVIE"},
            {"dm2", "WOODWARD", "ASSETS/LEVELS", "0", kDm2Camera, kDm2Records, "", "WOODWARD"},
            {"dm2", "TRAINYAR", "ASSETS/LEVELS", "1", kDm2Camera, kDm2Records, "", "TRAINYAR"},
            {"dm2", "SWAMPYTR", "ASSETS/LEVELS", "2", kDm2Camera, kDm2Records, "", "SWAMPYTR"},
            {"dm2", "COMMERCI", "ASSETS/LEVELS", "3", kDm2Camera, kDm2Records, "", "COMMERCI"},
            {"dm2", "GALLOON", "ASSETS/LEVELS", "4", kDm2Camera, kDm2Records, "", "GALLOON"},
            {"dm2", "CLOVERLE", "ASSETS/LEVELS", "5", kDm2Camera, kDm2Records, "", "CLOVERLE"},
            {"dm2", "DEVILSPE", "ASSETS/LEVELS", "6", kDm2Camera, kDm2Records, "", "DEVILSPE"},
            {"dm2", "AIRPORT", "ASSETS/LEVELS", "7", kDm2Camera, kDm2Records, "", "AIRPORT"},
            {"dm2", "PARK", "ASSETS/EDITOR", "PARK", "", "", "", "", Need::Nothing, true},
            {"bmx", "CHL_MULY", "ASSETS/LEVELS", "CHL_MULY", "", kBmxGoals, kBmxNpcs, "CHL_MULY"},
            {"bmx", "CHL_DAM", "ASSETS/LEVELS", "CHL_DAM", "", kBmxGoals, kBmxNpcs, "CHL_DAM"},
            {"bmx", "CHL_MNCO", "ASSETS/LEVELS", "CHL_MNCO", "", kBmxGoals, kBmxNpcs, "CHL_MNCO"},
            {"bmx", "CHL_NASA", "ASSETS/LEVELS", "CHL_NASA", "", kBmxGoals, kBmxNpcs, "CHL_NASA"},
            {"bmx", "CHL_SHEP", "ASSETS/LEVELS", "CHL_SHEP", "", kBmxGoals, kBmxNpcs, "CHL_SHEP"},
            {"bmx", "CHL_SYRA", "ASSETS/LEVELS", "CHL_SYRA", "", kBmxGoals, kBmxNpcs, "CHL_SYRA"},
            {"bmx", "CMP_RMPG", "ASSETS/LEVELS", "CMP_RMPG", "", kBmxGoals, kBmxNpcs, "CMP_RMPG"},
            {"bmx", "CMP_ROOT", "ASSETS/LEVELS", "CMP_ROOT", "", kBmxGoals, kBmxNpcs, "CMP_ROOT"},
        };
        constexpr const char *kParkFile = "park.dat";

        levels::Engine engineOf(std::string_view game)
        {
            if (game == "dm2")
                return levels::Engine::Zge2;
            if (game == "bmx")
                return levels::Engine::Zge4;
            return levels::Engine::Zge3;
        }
    }

    bool parseMapDescription(std::string_view text, CustomMap &out, std::string *error, std::string_view game)
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
            else if (key.rfind("slot.", 0) == 0 && key.size() > 5)
                map.targets[key.substr(5)] = upper(value);
            else if (key == "park")
                map.park = value;
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
        if (map.game.empty())
            map.game = lower(std::string(game));
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
        if (map.park.find('/') != std::string::npos || map.park.find('\\') != std::string::npos || map.park == "..")
            return fail("[map] park must be a file in the map's folder");
        if (const MapSlot *own = mapSlot(map.game, map.slot); own && own->park && map.park.empty())
            map.park = kParkFile;
        out = std::move(map);
        return true;
    }

    bool isMapFolder(const std::filesystem::path &folder)
    {
        if (const zx::game_files::ZipPath zip = zx::game_files::zipPathOf(folder); zip.zip)
            return zip.zip->hasFile(zip.inside.empty() ? std::string("MAP.INI") : zip.inside + "/MAP.INI");
        std::error_code ec;
        return std::filesystem::is_regular_file(folder / kMapDescriptionFile, ec);
    }

    bool CustomMap::readPreview(std::vector<uint8_t> &out) const
    {
        return !preview.empty() && zx::game_files::readFileBelow(folder, preview, out);
    }

    std::vector<CustomMap> listAllMaps(const std::vector<Mod> &sharedMods, const std::filesystem::path &mapsRoot,
                                       const std::vector<CustomMap> &gameMaps)
    {
        std::vector<CustomMap> maps;
        for (const Mod &mod : sharedMods)
            if (mod.isMap() && mod.problem.empty())
            {
                CustomMap map = mod.map;
                map.where = mod.orderKey();
                maps.push_back(std::move(map));
            }
        maps.insert(maps.end(), gameMaps.begin(), gameMaps.end());
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

    std::vector<const MapSlot *> mapSlotsOf(std::string_view game)
    {
        std::vector<const MapSlot *> slots;
        for (const MapSlot &lent : kMapSlots)
            if (game == lent.game)
                slots.push_back(&lent);
        return slots;
    }

    const MapSlot *mapSlotOf(std::string_view game)
    {
        const std::vector<const MapSlot *> slots = mapSlotsOf(game);
        return slots.empty() ? nullptr : slots.front();
    }

    const MapSlot *mapSlotFor(const CustomMap &map, std::string_view player, const SlotStartable &startable)
    {
        const auto starts = [&startable](const MapSlot *slot) { return slot && (!startable || startable(*slot)); };
        const MapSlot *own = mapSlot(map.game, map.slot);
        if (!own)
            return nullptr;
        if (own->park)
            return map.game == player && starts(own) ? own : nullptr; // a park is built by DM2's own park code
        if (map.game == player)
        {
            if (starts(own))
                return own;
            for (const MapSlot *slot : mapSlotsOf(player))
                if (!slot->park && starts(slot))
                    return slot; // its own game's other slot, the map converted with its goals
            return nullptr;
        }
        if (const auto named = map.targets.find(std::string(player)); named != map.targets.end())
            if (const MapSlot *slot = mapSlot(player, named->second); slot && !slot->park && starts(slot))
                return slot;
        for (const MapSlot *slot : mapSlotsOf(player))
            if (!slot->park && starts(slot))
                return slot;
        return nullptr;
    }

    bool mapPlayableIn(const CustomMap &map, std::string_view player, const SlotStartable &startable)
    {
        return mapSlotFor(map, player, startable) != nullptr;
    }

    bool mapNeedsConversion(const CustomMap &map, const MapSlot &slot) { return &slot != mapSlot(map.game, map.slot); }

    namespace
    {
        // A map moved into another slot of its own game keeps the files its
        // goals are in (convertMap), each read from its own slot's folder
        // and written under the target slot's name, in place of the
        // converter's empty one; a file the map lacks stays the converter's.
        void keepGoalFiles(const CustomMap &map, const MapSlot &target, levels::Converted &converted)
        {
            const MapSlot *own = mapSlot(map.game, map.slot);
            if (!own)
                return;
            // {the file in the map's slot folder, its name in the target's} ("*": the slot's name)
            std::vector<std::pair<std::string, std::string>> files;
            if (map.game == "ai")
                files = {{"*.BIN", "*.BIN"}, {"*T.BIN", "*T.BIN"}, {"MOTIONS.ZAL", "MOTIONS.ZAL"}};
            else if (map.game == "bmx")
                files = {{"OBJCTV.TXT", "OBJCTV.TXT"}, {"ANIMS.ZAL", "ANIMS.ZAL"}, {"CUTCOMP.ZAL", "CUTCOMP.ZAL"},
                         {"CUTSCENE.ZAL", "CUTSCENE.ZAL"}, {"MODELS.ZAL", "MODELS.ZAL"}};
            const auto named = [](const std::string &pattern, const char *slot)
            { return pattern[0] == '*' ? std::string(slot) + pattern.substr(1) : pattern; };
            zx::game_files::GameFileRoots roots;
            roots.mods = {map.folder};
            zx::game_files::GameFileSystem folder(std::move(roots));
            for (const auto &[from, to] : files)
            {
                const std::string source = std::string(own->folder) + "/" + own->slot + "/" + named(from, own->slot);
                levels::Bytes bytes;
                std::shared_ptr<zx::game_files::GameFile> file = folder.open(source);
                if (!file || !zx::game_files::readAll(*file, bytes))
                    continue;
                const std::string path = std::string(target.folder) + "/" + target.slot + "/" + named(to, target.slot);
                converted.files[path] = std::move(bytes);
                converted.report.push_back("its own " + named(from, own->slot) + " kept as " + named(to, target.slot) + " (its goals)");
            }
        }
    }

    levels::LevelFiles readMapLevelFiles(const CustomMap &map)
    {
        const MapSlot *own = mapSlot(map.game, map.slot);
        if (!own || own->park)
            throw levels::LevelError("the map's files are in no level slot of " + map.game);
        // The map's level files where its own game's slot has them (any
        // case; the folder read as a mod's, so a zip is read in place).
        zx::game_files::GameFileRoots roots;
        roots.mods = {map.folder};
        zx::game_files::GameFileSystem folder(std::move(roots));
        levels::LevelFiles files;
        for (const char *ext : {"GEO", "INS", "DAT", "TXR"})
        {
            const std::string path = std::string(own->folder) + "/" + own->slot + "/" + own->slot + "." + ext;
            std::shared_ptr<zx::game_files::GameFile> file = folder.open(path);
            if (!file || !zx::game_files::readAll(*file, files[ext]))
                throw levels::LevelError("the map has no " + path);
        }
        return files;
    }

    ConvertedMap convertMap(const CustomMap &map, const MapSlot &target)
    {
        const auto start = std::chrono::steady_clock::now();
        const levels::LevelFiles files = readMapLevelFiles(map);
        const levels::Model model = levels::exportLevel(engineOf(map.game), files);
        levels::Converted converted = levels::convertModel(model, engineOf(target.game), target.slot);
        if (map.game == target.game)
            keepGoalFiles(map, target, converted);
        ConvertedMap out;
        out.slot = &target;
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
