// The park editor's tool set: the tools a theme names (its catalog's
// categories among them, one tool each), which one is in use, stepping through
// them, and putting one aside under another; the editor's update and its end.
// Also what every tool uses: a sound at the selection, and the selection
// brought up to date.
// BMX XXX's own part of it; what both games have is zge/editor/edit_tools.cpp.

#include "bmx/editor/edit_tools.h"
#include "zge/editor/edit_tools.h"
#include "bmx/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "bmx/editor/park_ui.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_camera.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_selection.h"
#include "bmx/editor/park_cells.h"
#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/edit_tools.cpp.h"

namespace zge::editor
{
    // Scaffolding (address-call): the original's tail jump to soundEvent (edit_tools.h).
    namespace calls
    {
        uint32_t soundEventTail(uint32_t event, ee::Address a, ee::Address b, uint32_t c, uint32_t d)
        {
            return ee::tailCall<uint32_t>(sym::soundEvent, event, a, b, c, d);
        }
    }

    namespace
    {
        // The park's cost: each piece placed adds its cost, each taken away
        // takes it off (edit_object.cpp).
        inline ee::Global<int32_t, sym::g_parkCost> g_parkCost;
    }

    // resetParkCost: a new catalog starts the park's cost again. (Aggressive
    // Inline keeps it with its skater's reset, in game/aggressive/skater; it
    // sits in the editor's code, before the tool set's, in both games.)
    void resetParkCost()
    {
        *g_parkCost = 0;
    }
    READABLE_FUNCTION(resetParkCost, Verified);

    // The pause button: the help page goes, and the UI's edit-pause script plays.
    int32_t toolButtonPause(ee::Ptr<EditUiButton> button)
    {
        dismissEditHelpPage(button->ui, 0);
        return game::tail(playEditPauseScript, button->ui);
    }
    READABLE_FUNCTION_WAITS(toolButtonPause, Verified);

    // The tool set's camera takes the UI's scene camera as it is now (BMX XXX).
    void toolSetUseUiCamera(ee::Ptr<EditToolSet> toolSet)
    {
        game::tail(editCameraUseUiCamera, toolSet->editCamera);
    }
    READABLE_FUNCTION(toolSetUseUiCamera, Verified);
}
