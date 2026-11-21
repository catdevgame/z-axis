#pragma once

// Disc files as the game sees them, changed in memory: a game override
// registers a patch for a file name (its UI scripts, say) and the IOP's
// host file layer serves the patched contents instead of the file. The
// disc folder or image is never written. Patches are the boot's: a game
// booted after another in the same process starts with none
// (runtime/ps2_boot_config.h).

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zx::disc_patch
{
    // Changes the file's bytes in place; false leaves the file as it is.
    using Patch = std::function<bool(std::vector<uint8_t> &bytes)>;

    // For a disc file with this name (case-insensitive, without folders).
    // Several patches for one name run in registration order, each on the
    // previous one's result.
    void registerPatch(const std::string &fileName, Patch patch);
    // As above, for an owner that takes its patches back with
    // removePatches (a game's session when it ends).
    void registerPatch(const std::string &fileName, Patch patch, std::string owner);
    void removePatches(const std::string &owner);
    void clearPatches();
    bool hasPatch(std::string_view fileName);

    // The patched contents of a file with that name: made from the
    // original (readOriginal fills it) on first use and kept. Null if no
    // patch is registered, the original cannot be read or the patch left
    // it unchanged.
    std::shared_ptr<const std::vector<uint8_t>> contents(std::string_view fileName,
                                                         const std::function<bool(std::vector<uint8_t> &)> &readOriginal);

}
