// The park editor's test run tool: while it is on, the editor's camera is off
// and the park is skated; its one button pauses the run.

#include "zge/editor/edit_tool_test_run.h"
#include "zge/editor/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/editor/park_ui.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_classes.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/edit_tool_test_run.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditTestRunToolClass = sym::g_editTestRunToolName; // "EditTestRunTool"
        constexpr uint32_t kEditToolClass = sym::g_testRunToolEditToolName;    // "EditTool"

        // setToolSetCameraActive, and what it leaves in the result register (the tool's methods return it).
        int32_t setToolSetCameraActiveKeepingResult(ee::Ptr<EditToolSet> toolSet, uint32_t active) { return ee::tailCall<int32_t>(sym::setToolSetCameraActive, toolSet, active); }
    }

    int32_t registerEditTestRunToolClass()
    {
        return game::tail(registerEditClass, kEditTestRunToolClass, 4, kEditToolClass, sym::constructEditTestRunTool, sym::destructEditTestRunTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditTestRunToolClass, Verified);

    // The tool's own part is looked up and not used.
    uint32_t constructEditTestRunTool(uint32_t data)
    {
        editObjectPart(data, kEditTestRunToolClass);
        const ee::Ptr<EditToolPart> tool(editObjectPart(data, kEditToolClass));
        setEditToolMethods(tool, sym::g_testRunToolMethods);
        return game::tail(setEditToolName, tool, sym::g_textTestRunTool);
    }
    READABLE_FUNCTION(constructEditTestRunTool, Verified);

    void destructEditTestRunTool()
    {
    }
    READABLE_FUNCTION(destructEditTestRunTool, Verified);

    void testRunToolUpdate()
    {
    }
    READABLE_FUNCTION(testRunToolUpdate, Verified);

    void testRunToolDraw()
    {
    }
    READABLE_FUNCTION(testRunToolDraw, Verified);

    int32_t testRunToolDeactivate(ee::Ptr<EditToolPart> tool, uint32_t ui)
    {
        clearUiButton(ui, 0);
        popUiButtonPage(ui);
        return setToolSetCameraActiveKeepingResult(tool->toolSet, 1);
    }
    READABLE_FUNCTION_WAITS(testRunToolDeactivate, Verified);

    int32_t testRunToolPause(ee::Ptr<EditUiButton> button)
    {
        return game::tail(playTestPauseScript, button->ui);
    }
    READABLE_FUNCTION_WAITS(testRunToolPause, Verified);
}
