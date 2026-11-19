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

    // The field of view a level's view camera is drawn with for the game's
    // own (degrees): Camera Xbox's, then the Field of View setting's scale
    // (zge_view.h Widescreen::drawnFieldOfView). The game's own at the
    // settings' defaults.
    float drawnFieldOfView(const uint8_t *rdram, float gameDegrees);

    // Camera Distance and Height change the game's camera tuning (the follow
    // camera's Distance .. Grind Height): the hidden follow camera
    // (aggressive_inline_slus20327.cpp) runs with the game's own values,
    // which holdGameCameraTuning(rdram, true) puts in the table until
    // holdGameCameraTuning(rdram, false) puts the settings' back.
    bool cameraTuningChanged();
    void holdGameCameraTuning(uint8_t *rdram, bool game);
    // Camera / Distance: the game's own value and the table's now.
    void cameraDistance(const uint8_t *rdram, float &game, float &now);
}
