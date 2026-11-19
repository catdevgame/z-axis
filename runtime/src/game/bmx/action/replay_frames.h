#pragma once
// BMX XXX's camera states' frame functions for the replays (action.c,
// replay_frames.cpp).

#include <cstdint>

namespace game::bmx
{
    // The frame of a watched replay (camera states 2, 3 and 5). Always 0.
    int32_t replayStateFrame();
    // The frame of the demo's replay (camera state 4). 1 on the frame it
    // leaves for the feature screen or the first game state.
    int32_t replayDemoStateFrame();
}
