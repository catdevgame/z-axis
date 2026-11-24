#pragma once

// The Xbox textures, shared by the games built on the Z-Axis engine: the
// pack the extras' setup makes from the player's Xbox disc
// (collection/extras/xbox_texture_pack.h; ZX_XBOX_PACK names its folder,
// the app sets it to the game's extras/xbox-pack or the bundle's
// Resources/xbox-pack). When the game opens a PS2 file the pack has
// pictures for (a level's TXR, a skater's ZIT, a BMX XXX level's MODELS.ZAL
// ...), a worker reads them and hands them to the GPU renderer
// (runtime/texture_replace.h), keyed by the PS2 textures' contents.
// The renderer is told which texture object each GS texture is by the
// game's texture cache (its placeTextureLevel is wrapped), so it draws a
// picture by identity, also those other overrides register (AI's Zoo, DM2's
// Xbox levels). ZX_XBOX_TEXTURES=0/1 (the app sets it from settings.ini
// [display] xbox_textures) starts with them off or on; default on.

#include "ee/game_id.h"
#include "runtime/settings_registry.h"

#include <cstdint>
#include <functional>
#include <string>

class PS2Machine;

namespace zge_host::xbox_textures
{
    // The pack's pictures and the texture cache's notes, in `game`.
    void install(PS2Machine &runtime, game::Game game);

    // Only the texture cache's notes, for pictures an override registers
    // itself (DM2's Xbox levels); install() makes them too. Once a boot.
    void installTextureNotes(PS2Machine &runtime, game::Game game);

    // The game opened a disc file (any form of its path); from the native
    // disc layer (zge_host::DiscAdditions::fileOpened).
    void noteFileOpened(const std::string &path);

    // The Display page's row (key display.xbox_textures): switches the
    // pictures on and off at once; `save` (may be null) keeps the choice.
    inline constexpr const char *kSettingKey = "display.xbox_textures";
    zx::settings::Setting setting(std::function<void(bool on)> save);

    // The pack folder, or empty (no ZX_XBOX_PACK or not a folder).
    std::string packFolder();

    // Textures the pack must leave alone: a texture object whose key (its
    // level 0's size and index hash) `keepOff` answers true for is told to
    // the renderer with no key, so it draws the game's own texture (a level
    // served in place of one the pack covers: AI's TUT/Estate as the
    // Tutorial). Asked as each texture level gets its GS address. One filter.
    void keepTexturesOff(std::function<bool(uint32_t width, uint32_t height, uint64_t indexHash)> keepOff);
}
