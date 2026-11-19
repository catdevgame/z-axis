// The park editor's test run tool: while it is on, the editor's camera is off
// and the park is skated; its one button pauses the run.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_tool_test_run.cpp.

#include "aggressive/level/edit_tool_test_run.h"
#include "zge/editor/edit_tool_test_run.h"
#include "aggressive/level/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "aggressive/level/park_ui.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_classes.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_tool_test_run.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kEditTestRunToolClass = sym::g_editTestRunToolName; // "EditTestRunTool"

        // A button row's state (park_ui.cpp): pressed.
        constexpr uint32_t kOnPress = 1;

        // setToolSetCameraActive, and what it leaves in the result register (the tool's methods return it).
        int32_t setToolSetCameraActiveKeepingResult(ee::Ptr<EditToolSet> toolSet, uint32_t active) { return ee::tailCall<int32_t>(sym::setToolSetCameraActive, toolSet, active); }
    }

    int32_t testRunToolActivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        const uint32_t testRun = editObjectPart(tool, kEditTestRunToolClass);
        pushUiButtonPage(ui);
        setUiButton(ui, 0, kOnPress, sym::testRunToolPause, testRun, 10, ee::Float(0.0f));
        return setToolSetCameraActiveKeepingResult(tool->toolSet, 0);
    }
    READABLE_FUNCTION_WAITS(testRunToolActivate, Verified);
}
