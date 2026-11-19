// The game's sound mode and play phase, and the sound system's end as the game
// calls it (sound_mode.h): moved from zge/sound/channels.cpp, where they sat
// as engine code; the blocks say they are AI's own (layer2). DM2 (PS2) has
// them word for word in its own code.

#include "aggressive/sound/sound_mode.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/sound/sound.h"
#include "aggressive/sound/events.h"

#include <cstdint>
#include "sym/aggressive/sound/sound_mode.cpp.h"

namespace aggressive
{
    namespace
    {
        // The game's sound mode (its sound events' setGameMode): -1 before
        // the game runs and after its end, no sound.
        inline ee::Global<int32_t, sym::g_gameMode> g_gameMode;
        // The play phase (the game's setPlayPhase: 0 .. 3 the music's, then
        // the stops and the fly-through), the word after the sound mode.
        inline ee::Global<int32_t, sym::g_playPhase> g_playPhase;
        constexpr uint16_t kEveryBank = 0xFFFE;
    }

    // shutdownSound: the sound system's end, as the game calls it: its sound
    // mode left (0), every bank freed, the emitters, sources and channels
    // stopped; no sound mode (-1, which it returns). DM2 (PS2) has it word
    // for word.
    int32_t shutdownSound()
    {
        if (*g_gameMode != 0)
            setGameMode(0);
        zge::unloadSoundBank(kEveryBank);
        zge::resetSoundSources();
        zge::releaseAllSourceVoices();
        zge::shutdownSoundChannels();
        *g_gameMode = -1;
        return -1;
    }
    READABLE_FUNCTION_WAITS(shutdownSound, Verified);

    // soundUnusedHook: an empty hook (no caller in AI or DM2).
    void soundUnusedHook()
    {
    }
    READABLE_FUNCTION(soundUnusedHook, Verified);

    // gameModeOf: the game's sound mode (-1 before the game). DM2 (PS2) has
    // it word for word, on the word its shutdownSound reads.
    int32_t gameModeOf()
    {
        return *g_gameMode;
    }
    READABLE_FUNCTION(gameModeOf, Verified);

    // playPhase: the play phase. DM2 (PS2) has it word for word, on the word
    // after its sound mode, as in AI.
    int32_t playPhase()
    {
        return *g_playPhase;
    }
    READABLE_FUNCTION(playPhase, Verified);
}
