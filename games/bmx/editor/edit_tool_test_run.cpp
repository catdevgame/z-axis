// The park editor's test run tool: while it is on, the editor's camera is off
// and the park is skated; its one button pauses the run.
// BMX XXX's own part of it; what both games have is zge/editor/edit_tool_test_run.cpp.

#include "bmx/editor/edit_tool_test_run.h"
#include "zge/editor/edit_tool_test_run.h"
#include "bmx/editor/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "bmx/editor/park_ui.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_cells.h"
#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/edit_tool_test_run.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditTestRunToolClass = sym::g_editTestRunToolName; // "EditTestRunTool"
        constexpr uint32_t kTextEmpty = sym::textEmpty2;                        // ""
        constexpr uint32_t kOnPress = 1;
    }

    // Switched to: its one button pauses the run, and the editor's camera goes off. BMX XXX
    // then clears the selection and help texts (Aggressive Inline leaves them).
    int32_t testRunToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const uint32_t testRun = editObjectPart(tool, kEditTestRunToolClass);
        pushUiButtonPage(ui);
        setUiButton(ui, 0, kOnPress, sym::testRunToolPause, testRun, 10, ee::Float(0.0f));
        setToolSetCameraActive(tool->toolSet, 0);
        setObjectSelectionText(ui, ee::Ptr<const char>(kTextEmpty));
        return game::tail(setInfoHelpText, ui, kTextEmpty);
    }
    READABLE_FUNCTION_WAITS(testRunToolActivate, Verified);
}
