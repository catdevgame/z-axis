#pragma once

// DM2's memory card frame as the host's native saves run it: what its two
// files share (game_save.cpp: the game save, the auto load, the start check
// and a park from a host file; slot_saves.cpp: the replays and parks).

#include <cstdint>

namespace game::dm2::host
{
    // The frame's work done by the host: half a second from now the frame's
    // run (uiFrameUpdate's host form) tells the request that the work is done
    // (its onDone, with the frame then over) or that the frame closed (its
    // onClosed).
    void tellRequestLater(bool workDone);
    // Two frames from now the frame moves on to `step` (the game's
    // setUiFrameStep), its menu up by then to show the step's message.
    void showStepLater(uint32_t step);
}
