#pragma once

#include <cstdint>

class PS2Machine;
struct R5900Context;

namespace aggressive_inline::photo
{
    // Photo mode (aggressive_inline_photo.cpp): the game paused, a free
    // camera, no HUD or menus, picture filters, screenshots through the
    // Screenshot key. Installed with the settings menus (the app); the
    // developer runner without them is unchanged.
    void install(PS2Machine &runtime);
    // The game's widgets are not drawn (zgui's widget draw, wrapped in
    // aggressive_inline_gameplay.cpp): photo mode is on. zgui also runs
    // widget actions and key sniffers while it draws, so the pause menu
    // stands still meanwhile.
    bool hidesUi();
}

namespace aggressive_inline
{
    // Pause menu native "pcPhotoMode" (registered with the settings
    // natives): photo mode from the pause menu, back to it after.
    void photoModeNative(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
