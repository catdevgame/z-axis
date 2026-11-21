#pragma once

// Files made at build time from the user's Xbox files (zx-xbox-convert,
// runtime/ps2_zaxis_zoo.h and ps2_zaxis_ape.h): the Zoo and the Ape in the
// PS2's formats. The app bundles them as Resources/xbox-converted (Windows:
// xbox-converted\ next to disc\) and sets ZX_XBOX_CONVERTED to that
// folder; developer runs point it at zx-xbox-convert's output. The game
// overrides only read them: nothing is converted at run time.
//
// Layout: <folder>/<normalized game path> ("ASSETS/LEVELS/ZOO/ZOO.TXR"),
// and <folder>/textures/<game path>.xtp, the Xbox originals of a file's
// converted textures for the GPU renderer (runtime/ps2_texture_replace.h).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aggressive_inline::converted
{
    // ZX_XBOX_CONVERTED is a folder.
    bool available();
    // For logs: the folder, or why there is none.
    std::string description();
    // A made file by its normalized game path.
    bool exists(const std::string &gamePath);
    std::shared_ptr<const std::vector<uint8_t>> read(const std::string &gamePath);

    // Registers textures/<gamePath>.xtp with the texture replacement as
    // `source`. shareAlike: a picture of the same name, size and data size
    // as one registered before by a shareAlike call is that picture (one
    // picture per Xbox texture when several conversions key it: the Zoo's
    // level and split screen, its models).
    struct Pictures
    {
        bool ok = false;     // the pack was there and parsed
        size_t count = 0;    // entries registered
        size_t larger = 0;   // pictures larger than the texture they replace
        size_t newBytes = 0; // picture data not shared with earlier calls
    };
    Pictures registerPictures(const std::string &gamePath, const std::string &source, bool shareAlike);
}
