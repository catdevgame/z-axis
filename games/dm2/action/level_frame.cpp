// The level state's frame (action.h): DM2's own code (action.c).

#include "action.h"
#include "../input/pads.h"
#include "../rider/rider.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/input/pads.h"

#include <cstdint>
#include "sym/dm2/action/level_frame.cpp.h"

namespace game::dm2
{
    namespace rider
    {
        PadInput *padInput(uint32_t port); // input.cpp
    }

    namespace
    {
        // The action runs once every `interval` frames (1: every frame);
        // the countdown to its next run.
        inline ee::Global<int32_t, sym::g_actionFrameInterval> g_actionFrameInterval;
        inline ee::Global<int32_t, sym::g_actionFrameCountdown> g_actionFrameCountdown;
        // An interval of -1 runs the action once more and holds R2 down on
        // the first pad from then on (nothing in the game sets it).
        constexpr int32_t kRunOnce = -1;

        // What readPads answered this frame (bit 0: a pad was lost, bit 1: a pad came back).
        inline ee::Global<uint32_t, sym::g_padReport> g_padReport;
        // The action modes' frame functions (actionMode: play, then the
        // overlay's and the replay's).
        constexpr uint32_t kActionModes = 8;
        inline ee::Global<ee::FunctionPtr<void()>[kActionModes], sym::g_actionModeFrames> g_actionModeFrames;
    }

    uint32_t levelStateFrame()
    {
        const ee::Ptr<zge::Pad> firstPad(rider::padInput(0)->pad);
        --*g_actionFrameCountdown;
        if (*g_actionFrameCountdown == 0)
        {
            *g_actionFrameCountdown = *g_actionFrameInterval;
            *g_padReport = readPads();
            (*g_actionModeFrames)[actionMode()]();
            if (*g_actionFrameInterval == kRunOnce)
            {
                firstPad->buttons |= zge::kPadR2;
                firstPad->changed |= zge::kPadR2;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(levelStateFrame, Verified);
}
