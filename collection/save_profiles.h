#pragma once

// A game's save profiles in the collection: each a career of its own, the
// game's settings.ini naming the one its boots play ([saves] profile; the
// boot hands it to the game as ZX_SAVE_PROFILE, engine/host/save_files.h).
// A profile's files are in the game's Saves folder, by its name made safe
// for a file (zx::saves::safeFileName):
//
//   <saves>/<profile>.sav        the game's save (the engine's)
//   <saves>/<profile>.playtime   how long it has been played (the app's):
//                                "seconds = N", the game's own running time
//                                (its vsyncs: a pause of the app's, in the
//                                background, does not count), added to at
//                                the end of each boot that played it
//   <saves>/<profile>.<kind>     a game's own files of the career (the game's)

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace zx::collection
{
    // The name every game's boot plays when settings.ini names none.
    constexpr const char *kDefaultProfile = "Career";
    // The game save of before save profiles, Dave Mirra BMX 2's Game.sav
    // (its first boot with profiles copies it to Career's and leaves it for
    // an older app: games/dm2/overrides/dave_mirra_saves.cpp): in a saves
    // folder, but no profile.
    constexpr const char *kBeforeProfilesSave = "Game";

    // The profile's files in the saves folder.
    std::filesystem::path profileSave(const std::filesystem::path &savesDir, const std::string &profile);
    std::filesystem::path profilePlaytime(const std::filesystem::path &savesDir, const std::string &profile);

    // The profiles a game has: each <profile>.sav of its saves folder (but
    // kBeforeProfilesSave's) and
    // each of `named` (profiles chosen with no save yet: the one settings.ini
    // names, the one the game runs), each once (by its file's name, letters'
    // case aside: the saves' file systems), Career first, the others in
    // name order.
    std::vector<std::string> listProfiles(const std::filesystem::path &savesDir, const std::vector<std::string> &named);
    // `base` when no profile of `profiles` has its file name, else the
    // first free of "<base> 2", "<base> 3", ...
    std::string freeProfileName(const std::vector<std::string> &profiles, const std::string &base);
    // The profile's files copied to a new profile, "<profile> copy"
    // (freeProfileName): its save, its time played and each other
    // <profile>.<kind> a game keeps beside them (Dave Mirra BMX 2's
    // .XboxLevels.career, Aggressive Inline's .estaterecords), not their
    // .bak. Its name; "" and why in `error` when the profile has no save yet
    // or a copy is not written (then none is).
    std::string copyProfile(const std::filesystem::path &savesDir, const std::string &profile, std::string *error = nullptr);

    // A playtime file's seconds: 0 for none (or one that does not read).
    uint64_t readPlaytime(const std::filesystem::path &file);
    // Adds `seconds` to it (made when missing). False when it is not written.
    bool addPlaytime(const std::filesystem::path &file, uint64_t seconds);
    // The time as the menu writes it after "Last played": "45 min",
    // "3 h 20 min", "12 h"; "" under a minute.
    std::string playtimeText(uint64_t seconds);
}
