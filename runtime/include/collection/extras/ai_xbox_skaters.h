#pragma once

// Aggressive Inline's Xbox skaters: each PS2 skater model (ASSETS/SKATERS/
// .../*.ZIT) with the Xbox release's more detailed geometry, made from the
// player's PS2 and Xbox discs as ps2xNativeAssets --xbox-models makes them,
// into <out>/models/<PS2 path> (the texture pack's folder: Settings >
// Display > Xbox Skaters, aggressive_inline_disc.cpp).
//
// The model converter is the engine's (zge/native/xbox_models.h
// makeXboxModel, game code the app links); the setup is handed it
// (XboxModelMaker), so this layer stays free of the game's code.

#include "collection/extras/source_files.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ps2x::app::extras
{
    struct XboxModelMade
    {
        bool made = false;         // false: left out (why says why)
        bool sameGeometry = false; // the same vertices on both: nothing to gain
        std::string why;
        std::string summary;       // the converter's numbers, for the log
    };
    // A PS2 model (`relative`: below ASSETS, "SKATERS/AGRUB/AG_ZIT.ZIT") and
    // the Xbox's of the same path made into `out`.
    using XboxModelMaker =
        std::function<XboxModelMade(const std::string &relative, const Bytes &ps2, const Bytes &xbox, Bytes &out)>;

    // The engine's converter (src/collection/extras/xbox_model_maker.cpp),
    // linked only where the game's code is: the app and ps2xExtras.
    XboxModelMaker engineXboxModelMaker();

    // False with *error when a file cannot be read or written.
    bool makeAiXboxSkaters(const SourceFiles &ps2, const SourceFiles &xbox, const XboxModelMaker &maker,
                           const std::filesystem::path &out, std::vector<std::string> &log, std::string *error);
}
