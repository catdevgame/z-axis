// The level's pause buttons (readable round gamehand2): each frame of play
// a player's START (or a pad gone: its options word without bit 0x20)
// stops play into the pause menu with that player's number in the level
// state; in the career, SELECT on pad 0 (but in level 7) stops it with
// reason 13. Names past the offsets are inferred from what the code does.

#include "aggressive/level/pause.h"
#include "aggressive/game_state.h"
#include "aggressive/skater/skater.h"

#include "aggressive/level/level_object.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/pause.cpp.h"

namespace aggressive
{
    namespace
    {
        inline ee::Global<uint8_t, sym::g_gameTypeByte> g_gameTypeByte;  // 1 the career, 4 split screen
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelWord; // the level index's byte and the next three

        constexpr uint32_t kStart = 0x4000;  // the game's button bits
        constexpr uint32_t kSelect = 0x8000;
        constexpr uint32_t kPadConnected = 0x20; // PadState::options [inferred]
        constexpr uint64_t kStopSelect = 13;  // why play stopped: SELECT, the objectives page
        constexpr uint32_t kCareer = 1, kSplitScreen = 4, kTutorialLevel = 7;
        constexpr uint32_t kStoppedMode = 3;
        constexpr int32_t kStopped = 3;      // the level state's record +0x04's state: play already stopped

        // The pad a player of the level plays with.
        uint32_t padOf(uint32_t player) { return aggressive::currentLevel()->players[player].controls.port; }
        // A port's options word (skater.h's PadState; the port is not bounded, as in the original).
        uint32_t padOptions(uint32_t port)
        {
            const ee::Ptr<skater::PadState> pads = *skater::g_pads;
            return pads[port].options;
        }
    }

    // checkPauseButtons.
    void checkPauseButtons()
    {
        const ee::Ptr<LevelObject> level = parts::currentLevel();
        if (*ee::Ptr<int32_t>(level->mode) != kStopped)
        {
            // The players (two in split screen) in turn: the first whose
            // START was pressed, or whose pad is gone, pauses.
            const uint32_t players = *g_gameTypeByte == kSplitScreen ? 2 : 1;
            for (uint32_t player = 0; player < players; ++player)
            {
                bool pause = skater::padHeld(kStart, padOf(player)) && skater::padPressed(kStart, padOf(player));
                if (!pause)
                    pause = !(padOptions(padOf(player)) & kPadConnected);
                if (pause)
                {
                    level->flags &= ~kLevelStopReason;
                    level->flags = (level->flags & ~kLevelStoppedBy) | static_cast<uint64_t>(player & 3u) << 14;
                    enterGameMode(kStoppedMode);
                    return;
                }
            }
        }
        if (*g_gameTypeByte == kCareer && *g_levelWord != kTutorialLevel && skater::padHeld(kSelect, 0) && skater::padPressed(kSelect, 0))
        {
            parts::setLevelState(kStopSelect);
            enterGameMode(kStoppedMode);
            level->flags &= ~kLevelStoppedBy;
        }
    }
    READABLE_FUNCTION(checkPauseButtons, Verified);
}
