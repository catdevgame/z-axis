// DM2's Xbox-only content (collection/extras/dm2_xbox.h): its parts in the
// order tools/dm2_xbox.py convert makes them, written once all are made.
#include "dm2_xbox_parts.h"

namespace ps2x::app::extras
{
    bool makeDm2XboxExtras(const SourceFiles *ps2, const SourceFiles &xbox, const Bytes &defaultXbe,
                           const std::filesystem::path &out, const Dm2XboxOptions &options,
                           std::vector<std::string> &log, std::string *error)
    {
        dm2::Made made;
        try
        {
            if (ps2)
            {
                // The park editor's Desert and Highway, with the PS2's Beach
                // theme; the attract demo's replays the PS2 disc lacks.
                dm2::convertThemes(xbox, *ps2, made, log);
                dm2::demoReplays(xbox, *ps2, made, log);
            }
            else
                log.push_back("[dm2xbox] themes: no PS2 disc: no Desert and Highway, no demo replays");
            // The gallery's Retro movie as the Xbox disc has it: the game's
            // movie player plays the Bink file for assets\movie\retro.pss.
            made.push_back({"movies/ASSETS/MOVIE/RETRO.BIK", dm2::need(xbox, "movie/retro.bik", "the Xbox files")});
            dm2::convertLevels(xbox, options, made, log);
            made.push_back({"CAREER.BIN", dm2::buildCareer(xbox, defaultXbe, log)});
        }
        catch (const dm2::Dm2Error &e)
        {
            *error = std::string("Dave Mirra BMX 2's Xbox disc could not be converted: ") + e.what() + ".";
            return false;
        }
        for (const auto &[path, data] : made)
            if (!writeFile(out / std::filesystem::path(path), data, error))
                return false;
        return true;
    }
}
