#pragma once

#include <cstdint>

class PS2Runtime;

namespace aggressive_inline
{
    // Gameplay settings and shortcuts: the Camera, Assists and Cheats
    // pages of the Settings screen, Unlock Everything, and the Restart,
    // Hide HUD and Screenshot keys. On with the settings screen
    // (PS2X_GAME_SETTINGS_MENUS=1); with every setting at its default the
    // game's memory is never written.
    void installGameplay(PS2Runtime &runtime);
}
