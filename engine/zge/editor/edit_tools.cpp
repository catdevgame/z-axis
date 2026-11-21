// The park editor's tool set: the tools a theme names (its catalog's
// categories among them, one tool each), which one is in use, stepping through
// them, and putting one aside under another; the editor's update and its end.
// Also what every tool uses: a sound at the selection, and the selection
// brought up to date.

#include "zge/editor/edit_tools.h"
#include "zge/editor/game.h"
#include "zge/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "aggressive/sound/events.h"
#include "zge/editor/park_ui.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_camera.h"
#include "zge/editor/park_catalog.h"
#include "zge/editor/park_classes.h"
#include "zge/editor/park_selection.h"
#include "zge/editor/park_cells.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/edit_tools.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditObjectToolClass = sym::g_toolMakerEditObjectToolName; // "EditObjectTool"
        constexpr uint32_t kEditToolClass = sym::g_toolMakerEditToolName;             // "EditTool"
        constexpr uint32_t kHelperEditToolClass = sym::g_toolHelperEditToolName;      // "EditTool"

        // bindEditTool, and what it leaves in the result register (setEditorTool returns it).
        int32_t bindEditToolKeepingResult(uint32_t tool, ee::Ptr<EditToolSet> toolSet, uint32_t selection, ee::Ptr<EditCamera> camera) { return ee::game<int32_t>(sym::bindEditTool, tool, toolSet, selection, camera); }
    }

    int32_t setEditorTool(ee::Ptr<EditToolSet> toolSet, uint32_t tool);
    void addToolSetTool(ee::Ptr<EditToolSet> toolSet, ee::Ptr<EditToolPart> tool);
    void makeCategoryTools(ee::Ptr<EditToolSet> toolSet);

    // The theme's tools in its order: a tool of its named class for each, and
    // where it names EditObjectTool, one for each of the catalog's categories.
    void makeThemeTools(ee::Ptr<EditToolSet> toolSet, uint32_t theme)
    {
        for (uint32_t index = 0; index < themeToolCount(theme); ++index)
        {
            const uint32_t className = static_cast<uint32_t>(themeToolName(theme, index));
            if (zge::stringCompareNoCase(className, kEditObjectToolClass) == 0)
                makeCategoryTools(toolSet);
            else
                addToolSetTool(toolSet, ee::Ptr<EditToolPart>(editObjectPart(newEditObject(className, toolSet), kEditToolClass)));
        }
    }
    READABLE_FUNCTION_WAITS(makeThemeTools, Verified);

    // Button 0's handler run as pressed (no caller in this build).
    int32_t resetToolSetButton(uint32_t ui)
    {
        return game::tail(fireUiButton, ui, 0, 1);
    }
    READABLE_FUNCTION_WAITS(resetToolSetButton, Verified);

    // The editor's end: the tool in use put away, every tool and the selection
    // deleted, the buttons, camera and UI freed, the theme's destroy script
    // played if its UI script was attached, the draw task deleted, and the tool
    // set itself freed.
    void deleteEditor(ee::Ptr<EditToolSet> toolSet)
    {
        deactivateEditTool(toolSet->currentTool, toolSet->ui);
        while (toolSet->tools.first != 0)
        {
            const uint32_t tool = toolSet->tools.first;
            zge::listRemove(&toolSet->tools, tool);
            deleteEditObject(tool);
        }
        clearUiButton(toolSet->ui, 0);
        clearUiButton(toolSet->ui, 1);
        clearUiButton(toolSet->ui, 6);
        clearUiButton(toolSet->ui, 9);
        deleteEditObject(toolSet->selection);
        clearCameraButtons(toolSet);
        freeEditCamera(toolSet->editCamera);
        freeEditUi(toolSet->ui);
        if (toolSet->hasUiScript != 0)
        {
            playDestroyParkEditorScript();
            toolSet->hasUiScript = 0;
        }
        if (toolSet->task != 0)
            zge::deleteTask(toolSet->task);
        game::tail(editFree, toolSet);
    }
    READABLE_FUNCTION_WAITS(deleteEditor, Verified);

    // Each frame: the UI's buttons, the tool in use, and the camera while it
    // follows the sticks.
    void updateEditor(ee::Ptr<EditToolSet> toolSet, ee::Float seconds)
    {
        updateEditUi(toolSet->ui);
        if (toolSet->currentTool != 0)
            updateEditTool(toolSet->currentTool, seconds);
        if (toolSet->cameraActive != 0)
            game::tail(updateEditCamera, toolSet->editCamera, seconds);
    }
    READABLE_FUNCTION_WAITS(updateEditor, Verified);

    // A tool joins the set at the end, and its key goes in front of the tool
    // roll's.
    void addToolSetTool(ee::Ptr<EditToolSet> toolSet, ee::Ptr<EditToolPart> tool)
    {
        ee::Local<char[0x20]> keys;
        zge::listPush(&toolSet->tools, tool);
        zge::formatString(keys.address(), sym::g_textToolKeyFormat, tool->iconChar, &toolSet->toolKeys[0]);
        zge::stringCopy(&toolSet->toolKeys[0], keys.address());
    }
    READABLE_FUNCTION(addToolSetTool, Verified);

    // The first tool, if it is not in use already, and the roll shown.
    int32_t selectFirstTool(ee::Ptr<EditToolSet> toolSet)
    {
        const uint32_t first = toolSet->tools.first;
        const int32_t current = toolSet->currentTool;
        if (current == static_cast<int32_t>(first))
            return current;
        setEditorTool(toolSet, first);
        return game::tail(setToolRoll, toolSet->ui, &toolSet->toolKeys[0]);
    }
    READABLE_FUNCTION_WAITS(selectFirstTool, Verified);

    // Another tool in use over the current one, which is put aside on the stack.
    void pushEditorTool(ee::Ptr<EditToolSet> toolSet, uint32_t tool)
    {
        deactivateEditTool(toolSet->currentTool, toolSet->ui);
        const ee::Ptr<ToolStackNode> aside(editAllocate(sizeof(ToolStackNode)));
        aside->tool = toolSet->currentTool;
        zge::listPush(&toolSet->toolStack, aside);
        toolSet->currentTool = static_cast<int32_t>(tool);
        bindEditTool(tool, toolSet, toolSet->selection, toolSet->editCamera);
        if (toolSet->currentTool != 0)
            game::tail(activateEditTool, toolSet->currentTool, toolSet->ui);
    }
    READABLE_FUNCTION_WAITS(pushEditorTool, Verified);

    // The tool put aside last back in use. Returns the tool that was in use
    // (its caller deletes it).
    int32_t popEditorTool(ee::Ptr<EditToolSet> toolSet)
    {
        if (toolSet->currentTool != 0)
            deactivateEditTool(toolSet->currentTool, toolSet->ui);
        const int32_t popped = toolSet->currentTool;
        const ee::Ptr<ToolStackNode> top(toolSet->toolStack.last);
        toolSet->currentTool = top->tool;
        zge::listRemove(&toolSet->toolStack, top);
        editFree(top);
        if (toolSet->currentTool != 0)
            activateEditTool(toolSet->currentTool, toolSet->ui);
        return popped;
    }
    READABLE_FUNCTION_WAITS(popEditorTool, Verified);

    // The tools in turn, round from the last to the first and back.
    int32_t selectNextTool(ee::Ptr<EditToolSet> toolSet)
    {
        playNextToolScript();
        uint32_t next = ee::Ptr<EditToolPart>(toolSet->currentTool)->next;
        if (next == 0)
            next = toolSet->tools.first;
        return game::tail(setEditorTool, toolSet, next);
    }
    READABLE_FUNCTION_WAITS(selectNextTool, Verified);

    int32_t selectPreviousTool(ee::Ptr<EditToolSet> toolSet)
    {
        playPreviousToolScript();
        uint32_t previous = ee::Ptr<EditToolPart>(toolSet->currentTool)->previous;
        if (previous == 0)
            previous = toolSet->tools.last;
        return game::tail(setEditorTool, toolSet, previous);
    }
    READABLE_FUNCTION_WAITS(selectPreviousTool, Verified);

    // A tool for each category of the catalog not flagged as the system's.
    void makeCategoryTools(ee::Ptr<EditToolSet> toolSet)
    {
        for (uint32_t index = 0; index < catalogCategoryCount(toolSet->catalog); ++index)
        {
            const uint32_t category = static_cast<uint32_t>(catalogCategory(toolSet->catalog, index));
            if (categorySystemFlag(category) == 0)
                addToolSetTool(toolSet, ee::Ptr<EditToolPart>(editObjectPart(newEditObject(kEditObjectToolClass, category), kEditToolClass)));
        }
    }
    READABLE_FUNCTION_WAITS(makeCategoryTools, Verified);

    void setToolSetCatalog(ee::Ptr<EditToolSet> toolSet, uint32_t catalog)
    {
        toolSet->catalog = static_cast<int32_t>(catalog);
    }
    READABLE_FUNCTION(setToolSetCatalog, Verified);

    void setToolSetPark(ee::Ptr<EditToolSet> toolSet, uint32_t park)
    {
        toolSet->park = static_cast<int32_t>(park);
    }
    READABLE_FUNCTION(setToolSetPark, Verified);

    // The tool in use changes: the old one put away, the new one bound to the
    // set's selection and camera and picked up.
    int32_t setEditorTool(ee::Ptr<EditToolSet> toolSet, uint32_t tool)
    {
        if (toolSet->currentTool != 0)
            deactivateEditTool(toolSet->currentTool, toolSet->ui);
        toolSet->currentTool = static_cast<int32_t>(tool);
        const int32_t bound = bindEditToolKeepingResult(tool, toolSet, toolSet->selection, toolSet->editCamera);
        if (toolSet->currentTool != 0)
            return game::tail(activateEditTool, toolSet->currentTool, toolSet->ui);
        return bound;
    }
    READABLE_FUNCTION_WAITS(setEditorTool, Verified);

    int32_t deactivateEditorTool(ee::Ptr<EditToolSet> toolSet)
    {
        const int32_t tool = toolSet->currentTool;
        if (tool != 0)
            return game::tail(deactivateEditTool, tool, toolSet->ui);
        return tool;
    }
    READABLE_FUNCTION_WAITS(deactivateEditorTool, Verified);

    int32_t activateEditorTool(ee::Ptr<EditToolSet> toolSet)
    {
        const int32_t tool = toolSet->currentTool;
        if (tool != 0)
            return game::tail(activateEditTool, tool, toolSet->ui);
        return tool;
    }
    READABLE_FUNCTION_WAITS(activateEditorTool, Verified);

    // The tool's selection moved by nothing: what follows a move (the cursor,
    // the held pieces) catches up.
    int32_t refreshToolSelection(ee::Address part)
    {
        const ee::Ptr<EditToolPart> tool(editObjectPart(part, kHelperEditToolClass));
        ee::Local<zge::Vec3> noMove;
        noMove->x = ee::Float(0.0f);
        noMove->y = ee::Float(0.0f);
        noMove->z = ee::Float(0.0f);
        return moveSelection(tool->selection, noMove.address());
    }
    READABLE_FUNCTION_WAITS(refreshToolSelection, Verified);

    namespace
    {
        // The editor's draw task: each frame the tool in use and the UI.
        constexpr uint32_t kDrawTaskPriority = 1, kDrawTaskFlags = 1;
        constexpr uint32_t kEditToolSetSourceLine = game::readable::perGame(160u, 160u, 164u); // of "../GameTools/ParkEditor/editToolSet.c", line 164 in BMX XXX's
        // The tool set's buttons (as deleteEditor clears them), pressed, with
        // a repeat period of 1 and no deadzone.
        constexpr uint32_t kButtonPause = 0, kButtonFirstTool = 1, kButtonPreviousTool = 6, kButtonNextTool = 9;
        constexpr uint32_t kOnPress = 1, kRepeatPeriod = 1;
    }

    // The park editor's tool set for a theme's park and catalog: its UI
    // (with the theme's UI script attached unless the UI is already loaded)
    // and camera (kept inside the park's corners, following the sticks), the
    // task that draws it, the selection on the park, the pause and tool
    // buttons, the theme's tools with the first one in use, their keys on the
    // UI's tool roll, and the theme's start script played.
    ee::Ptr<EditToolSet> newEditToolSet(uint32_t theme, uint32_t park, uint32_t catalog, uint32_t uiLoaded)
    {
        ee::Local<zge::Vec4> nearCorner;
        ee::Local<zge::Vec4> farCorner;
        const ee::Ptr<EditToolSet> toolSet(editAllocate(sizeof(EditToolSet)));
        toolSet->toolKeys[0] = 0;
        setToolSetPark(toolSet, park);
        setToolSetCatalog(toolSet, catalog);
        toolSet->ui = static_cast<int32_t>(newEditUi());
        toolSet->editCamera = newEditCamera();
        toolSet->cameraActive = 1;
        toolSet->hasUiScript = 0;
        if (!uiLoaded)
        {
            attachEditUiScript(toolSet->ui, themeUiScript(theme));
            toolSet->hasUiScript = 1;
        }
        showEditUi(toolSet->ui);
        parkNearCorner(park, nearCorner);
        parkFarCorner(park, farCorner);
        setEditCameraBounds(toolSet->editCamera, farCorner, nearCorner);
        setCameraButtons(toolSet);
        toolSet->task = zge::newTask(kDrawTaskPriority, sym::drawEditor, toolSet, kDrawTaskFlags, sym::g_textGameToolsParkEditorEditToolSet, kEditToolSetSourceLine);
        const uint32_t selection = newEditObject(sym::g_toolSetEditSelectionName, 0); // "EditSelection"
        toolSet->selection = static_cast<int32_t>(selection);
        attachSelectionToPark(selection, toolSet->park);
        setUiButton(toolSet->ui, kButtonPause, kOnPress, sym::toolButtonPause, toolSet, kRepeatPeriod, 0.0f);
        setUiButton(toolSet->ui, kButtonFirstTool, kOnPress, sym::toolButtonFirstTool, toolSet, kRepeatPeriod, 0.0f);
        setUiButton(toolSet->ui, kButtonPreviousTool, kOnPress, sym::toolButtonPreviousTool, toolSet, kRepeatPeriod, 0.0f);
        setUiButton(toolSet->ui, kButtonNextTool, kOnPress, sym::toolButtonNextTool, toolSet, kRepeatPeriod, 0.0f);
        setEditUiPadLostHandler(toolSet->ui, sym::resetToolSetButton);
        toolSet->currentTool = 0;
        zge::initList(&toolSet->tools);
        zge::initList(&toolSet->toolStack);
        makeThemeTools(toolSet, theme);
        selectFirstTool(toolSet);
        setToolRoll(toolSet->ui, &toolSet->toolKeys[0]);
        playEditScriptFor(toolSet->ui, themeStartScript(theme));
        toolSet->built = 1;
        return toolSet;
    }
    READABLE_FUNCTION_WAITS(newEditToolSet, Verified);
}
