// The park editor's terrain tool: the select button, held, drags a rectangle of
// cells out from the cursor; let go, the rectangle is chosen and the left and
// right buttons raise and lower its ground a step at a time, a tape measure
// showing its height in feet. Reselect goes back to moving the cursor.

#include "zge/editor/edit_tool_terrain.h"
#include "zge/editor/edit_records.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"
#include "zge/editor/edit_tools.h"
#include "zge/editor/park_selection.h"
#include "zge/editor/park_ui.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_camera.h"
#include "zge/editor/park_classes.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/edit_tool_terrain.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditTerrainToolClass = sym::g_editTerrainToolName; // "EditTerrainTool"
        constexpr uint32_t kEditToolClass = sym::g_terrainToolEditToolName;   // "EditTool"

        // The tool's icon in the tool bar's font.
        constexpr int8_t kTerrainToolIcon = game::readable::perGame<int8_t>('R', 'R', 'F'); // BMX XXX's icon is 'F'

        // EditTerrainToolPart::mode
        constexpr int32_t kTerrainMoving = 0, kTerrainSelecting = 1, kTerrainChosen = 2;
    }

    int32_t registerEditTerrainToolClass()
    {
        return game::tail(registerEditClass, kEditTerrainToolClass, sizeof(EditTerrainToolPart), kEditToolClass, sym::constructEditTerrainTool, sym::destructEditTerrainTool, sym::editToolMessage);
    }
    READABLE_FUNCTION(registerEditTerrainToolClass, Verified);

    void constructEditTerrainTool(uint32_t data)
    {
        const ee::Ptr<EditTerrainToolPart> terrain(editObjectPart(data, kEditTerrainToolClass));
        const ee::Ptr<EditToolPart> tool(editObjectPart(data, kEditToolClass));
        setEditToolMethods(tool, sym::g_terrainToolMethods);
        setEditToolName(tool, sym::g_textTerrainTool);
        tool->iconChar = kTerrainToolIcon;
        terrain->mode = kTerrainMoving;
    }
    READABLE_FUNCTION(constructEditTerrainTool, Verified);

    void destructEditTerrainTool()
    {
    }
    READABLE_FUNCTION(destructEditTerrainTool, Verified);

    void terrainToolUpdate(ee::Ptr<EditToolPart> tool)
    {
        game::tail(updateSelectionCursor, tool->selection);
    }
    READABLE_FUNCTION_WAITS(terrainToolUpdate, Verified);

    int32_t terrainToolDraw(ee::Ptr<EditToolPart> tool)
    {
        return game::tail(drawSelectionInColour, tool->selection);
    }
    READABLE_FUNCTION_WAITS(terrainToolDraw, Verified);
}
