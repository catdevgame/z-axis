// Aggressive Inline's Xbox skaters from the player's discs
// (collection/extras/ai_xbox_skaters.h).
#include "collection/extras/ai_xbox_skaters.h"

#include "runtime/ps2_game_files.h"

namespace zx::app::extras
{
    bool makeAiXboxSkaters(const SourceFiles &ps2, const SourceFiles &xbox, const XboxModelMaker &maker,
                           const std::filesystem::path &out, std::vector<std::string> &log, std::string *error)
    {
        size_t made = 0, left = 0, same = 0;
        for (const std::string &file : ps2.files())
        {
            const std::string upper = upperAscii(file);
            if (upper.compare(0, 8, "SKATERS/") != 0 || upper.size() < 4 || upper.compare(upper.size() - 4, 4, ".ZIT") != 0)
                continue;
            Bytes ps2Model, xboxModel, converted;
            if (!xbox.readAny(file, xboxModel))
                continue;
            if (!ps2.read(file, ps2Model))
            {
                *error = "Could not read " + file + " from the PS2 disc.";
                return false;
            }
            const XboxModelMade result = maker(file, ps2Model, xboxModel, converted);
            if (!result.made)
            {
                ++left;
                log.push_back(file + ": left out (" + result.why + ")");
                continue;
            }
            if (result.sameGeometry)
            {
                ++same;
                continue;
            }
            if (!writeFile(out / "models" / zx::game_files::normalizePath("ASSETS/" + file), converted, error))
                return false;
            ++made;
            if (!result.summary.empty())
                log.push_back(file + ": " + result.summary);
        }
        log.push_back("xbox models: " + std::to_string(made) + " made, " + std::to_string(same) +
                      " the same geometry on both, " + std::to_string(left) + " left out");
        return true;
    }
}
