#pragma once
// BMX XXX's sound modes (bmx.c, sound_mode.cpp): what the sound has loaded
// and plays for where the game is, and play's music phases. BMX XXX's own
// code; AI's games/aggressive/sound is the same design one game earlier.

#include <cstdint>

namespace game::bmx
{
    // The sound modes. Entering the front end loads its bank and starts its
    // music; entering play from there loads the level's banks and opens the
    // music's stream and the speech channel; the pause stops the level's
    // sources and holds the music and the speech until play comes back.
    // (kNoSoundMode: before the sound is set up and after it is shut down;
    // nothing sounds. kPlacedSoundsMode is tested and never set.)
    constexpr int32_t kNoSoundMode = -1, kSoundModeNone = 0, kFrontEndMode = 1, kPlayMode = 2, kPausedMode = 3, kPlacedSoundsMode = 4;
    // The sound moved to `mode` (nothing when it is there already); the
    // sound's task then runs once. Returns the mode the sound is in.
    int32_t setGameMode(uint32_t mode);

    // Play's music phases (the game's round sets them): 1 and 3 play the
    // playlist, 2 fades it out for a cutscene's sound, 4 stops it.
    // Returns the phase set.
    int32_t setPlayPhase(int32_t phase);

    // The sound shut down: its mode left, every bank unloaded, the streams,
    // sources and channels closed, the track table freed.
    void shutdownSound();
}
