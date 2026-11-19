// The action mode (action.h): DM2's own code (action.c).

#include "action.h"

#include "ee/abi.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/dm2/action/action_mode.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The action mode: actionStart sets it; the action's frame runs the
        // mode's frame function (a table of eight: play, then the overlay's
        // and the replay's).
        inline ee::Global<uint8_t, sym::g_actionMode> g_actionMode;
    }

    uint8_t actionMode()
    {
        return *g_actionMode;
    }
    READABLE_FUNCTION(actionMode, Verified);
}
