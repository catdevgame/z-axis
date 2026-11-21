#pragma once

// The game's sound mode and play phase, and the sound system's end as DM2
// calls it (sound_mode.cpp).

#include <cstdint>

namespace game::dm2
{
    // The sound system's end: the sound mode left, every bank freed, the
    // emitters, sources and channels stopped; -1 (no sound mode).
    int32_t shutdownSound();
    // An empty hook (no caller).
    void soundUnusedHook();
    // The game's sound mode (-1 before the game).
    int32_t gameModeOf();
    // The play phase.
    int32_t playPhase();
    // The sound modes (setGameMode): none, the front end's (its music), a
    // level's, a level paused (its sources stopped, its music paused), and
    // the placed sounds' bank's (no caller in DM2).
    constexpr uint32_t kSoundModeOff = 0, kSoundModeFrontEnd = 1, kSoundModeLevel = 2, kSoundModePaused = 3,
                       kSoundModePlacedSounds = 4;
    // The sound mode set, its banks, voices, sources and music with it;
    // returns the mode.
    int32_t setGameMode(uint32_t mode);
    // The front end's script functions BmxMenuMusicOff and BmxMenuMusicOn:
    // the sound mode left (0) or the front end's (1), whose music follows
    // it. Both answer setGameMode's result.
    uint32_t bmxMenuMusicOff();
    uint32_t bmxMenuMusicOn();
    // The sound's frame (playFrame's): a level's music kept playing track
    // after track, its sound emitters, the streams' and the audio's frames.
    void musicFrame();
    // The play phase set (from a music phase the music is stopped or turned
    // down); returns it.
    int32_t setPlayPhase(int32_t phase);
    // The sound system started: channels, sources, emitters, streams, the
    // four banks and the soundtrack. Always 1.
    uint32_t initSound();
}
