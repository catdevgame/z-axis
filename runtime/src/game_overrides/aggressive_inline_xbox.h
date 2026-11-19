#pragma once

#include "runtime/ps2_settings.h"

#include <functional>
#include <string>

class PS2Runtime;

namespace aggressive_inline
{
    // Xbox textures: the pack made at build time from the user's Xbox disc
    // (tools/xbox_pack.py; PS2X_XBOX_PACK names its folder, the app sets it
    // to the bundle's Resources/xbox-pack). When the game opens a PS2 file
    // the pack has pictures for (a level's TXR, a skater's ZIT ...), a worker
    // reads them and hands them to the GPU renderer
    // (runtime/ps2_texture_replace.h), keyed by the PS2 textures' contents.
    // The renderer is told which texture object each GS texture is by the
    // game's texture cache (its level allocator is wrapped), so it draws a
    // picture by identity, also the Zoo's (registered by the Zoo override).
    // PS2X_XBOX_TEXTURES=0/1 (the app sets it from settings.ini [display]
    // xbox_textures) starts with them off or on; default on.
    void installXboxTextures(PS2Runtime &runtime);

    // The game opened a disc file (any form of its path); from the native
    // disc layer.
    void xboxNoteFileOpened(const std::string &path);

    // The Display page's row (key display.xbox_textures): switches the
    // pictures on and off at once; `save` (may be null) keeps the choice.
    inline constexpr const char *kXboxTexturesKey = "display.xbox_textures";
    ps2x::settings::Setting xboxTexturesSetting(std::function<void(bool on)> save);

    // The pack folder, or empty (no PS2X_XBOX_PACK or not a folder).
    std::string xboxPackFolder();

    // Textures the pack must leave alone: a texture object whose key (its
    // level 0's size and index hash) `keepOff` answers true for is told to
    // the renderer with no key, so it draws the game's own texture (a level
    // served in place of one the pack covers: TUT/Estate as the Tutorial).
    // Asked as each texture level gets its GS address. One filter.
    void xboxKeepTexturesOff(std::function<bool(uint32_t width, uint32_t height, uint64_t indexHash)> keepOff);
}
