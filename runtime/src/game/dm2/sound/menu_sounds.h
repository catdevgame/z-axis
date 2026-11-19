#pragma once

// The menus' sounds (menu_sounds.cpp): zgui's five sound hooks, each a
// sound event, and the soundtrack page's script functions registered.

#include <cstdint>

namespace game::dm2
{
    // zgui's sound hooks (Zgui::selectSound .. blockedSound; the UI's set-up
    // stores them): a widget selected, a menu cancelled, the cursor moved, a
    // value changed, a move that cannot be made. Each answers 0.
    uint32_t playSelectSound();
    uint32_t playCancelSound();
    uint32_t playMoveSound();
    uint32_t playChangeSound();
    uint32_t playBlockedSound();
    // The soundtrack page's script functions (bmxsoundUIUpdate,
    // bmxsoundChangedTrack, bmxsoundChangedSoundtrack) registered. Always 1.
    uint32_t registerSoundMenuFunctions();
    // The options menu's volume script functions: "BmxUpdateVolumeInfo" (the
    // menu's volumes into the game's settings and the sound), and
    // "BmxPlayMusicVolume" (the music volume previewed: the change sound at
    // it, then the SFX volume put back). Each answers the last sound event's
    // result.
    uint32_t bmxUpdateVolumeInfo();
    uint32_t bmxPlayMusicVolume();
}
