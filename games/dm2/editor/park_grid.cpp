// DM2's park editor: the park's grid (park_grid.h). DM2's own code (its
// editor block).

#include "park_grid.h"
#include "start_tool.h"
#include "edit_core.h"
#include "rail_tool.h"
#include "edit_themes.h"
#include "edit_objects.h"
#include "edit_lights.h"
#include "edit_display.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ground_towers.h"
#include "park_scores.h"
#include "dm2/rider/collision/collision.h"
#include "edit_grid.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/platform/platform.h"
#include "zge/scene/query.h"
#include "zge/scene/node.h"
#include "zge/scene/scene_database.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"

#include <cstdint>
#include "sym/dm2/editor/park_grid.cpp.h"

namespace game::dm2
{
    namespace
    {
        // Scaffolding (address-call): the theme's kind of a cell (the
        // editor's themes) and the cursor's state (the editor core) are not
        // readable in this branch.

        // The editor's scene is entered (setUpParkTheme enters it once).
        inline ee::Global<uint32_t, sym::g_editSceneEntered> g_editSceneEntered;
        // The hidden "Editobj_towerdef" node: the sloped towers' vertices
        // are set from its mesh's.
        inline ee::Global<ee::Ptr<zge::Node>, sym::g_defaultTower> g_defaultTower;
        // 1 while clearParkObjects runs.
        inline ee::Global<uint32_t, sym::g_parkClearing> g_parkClearing;
        // The engine's flag (ZGE2): a geometry file loaded while it is set
        // keeps every part's normals; the loader clears it after the file.
        inline ee::Global<uint32_t, sym::g_keepAllNormals> g_keepAllNormals;
        // The copy tool's copied rows and columns (copy_tool.cpp): none
        // once a theme is set up.
        inline ee::Global<uint32_t, sym::g_copiedRows> g_copiedRows;
        inline ee::Global<uint32_t, sym::g_copiedColumns> g_copiedColumns;

        // The cell's rails removed, and its object if this is the object's
        // own cell: its lights, then the object itself.
        void removeCellContents(ee::Ptr<ParkEditor> editor, int32_t column, int32_t row, uint32_t rowByte)
        {
            removeCellRails(editor, static_cast<uint8_t>(column), rowByte);
            const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
            const ee::Ptr<PlacedObject> object = cell->object;
            if (object && object->column == column && object->row == row)
            {
                removeNodeLights(object->node);
                freePlacedObject(&cell->object);
                cell->object = ee::Ptr<PlacedObject>();
            }
        }

        void bareCell(ee::Ptr<ParkEditor> editor, int32_t column, int32_t row)
        {
            const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
            cell->level = kClearedCellLevel;
            cell->object = ee::Ptr<PlacedObject>();
            cell->column = static_cast<uint8_t>(column);
            cell->row = static_cast<uint8_t>(row);
        }
    }

    void setUpParkTheme(ee::Ptr<ParkEditor> editor, uint8_t theme)
    {
        if (!*g_editSceneEntered)
        {
            *g_keepAllNormals = 1;
            zge::enterScene(ee::Address(themeFile(theme)));
            *g_editSceneEntered = 1;
            zge::initTokenReader();
        }
        editor->rows = themeHeight(theme);
        const uint8_t columns = themeWidth(theme);
        editor->theme = theme;
        editor->columns = columns;
        editor->themeSetUp = 1;
        takeParkScores(editor);
        for (int32_t row = 0; row < editor->rows; ++row)
        {
            for (int32_t column = 0; column < editor->columns; ++column)
            {
                const int8_t level = themeCellHeight(theme, static_cast<uint8_t>(column), static_cast<uint8_t>(row));
                const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
                cell->level = level;
                cell->object = ee::Ptr<PlacedObject>();
                cell->rails = ee::Ptr<CellRails>();
                cell->column = static_cast<uint8_t>(column);
                cell->row = static_cast<uint8_t>(row);
                cell->tower = ee::Ptr<zge::Node>();
            }
        }
        if (!*g_defaultTower)
        {
            const ee::Ptr<zge::Node> tower = newWorldObjectInstance(zge::nameHash(ee::Ptr<const char>(sym::textEditobjTowerDef)));
            *g_defaultTower = tower;
            zge::setNodeShown(tower, 0, 0);
        }
        fitThemeTowers(editor);
        clearStartMarkers(editor);
        *g_copiedRows = 0;
        *g_copiedColumns = 0;
        collectThemeLights();
        game::tail(setCurrentTheme, theme);
    }
    READABLE_FUNCTION(setUpParkTheme, Verified);

    void clearParkCells(ee::Ptr<ParkEditor> editor)
    {
        // The original keeps the row's low byte in its frame for the rails' call.
        ee::Local<uint32_t> rowByte;

        for (int32_t row = 0; row < editor->rows; ++row)
        {
            if (editor->columns != 0)
                *rowByte = static_cast<uint8_t>(row);
            for (int32_t column = 0; column < editor->columns; ++column)
            {
                removeCellContents(editor, column, row, *rowByte);
                const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
                if (cell->tower)
                {
                    // The tower's own cell releases it; the other three only forget it.
                    if ((column & 1) != 0 && (row & 1) == 0)
                        zge::releaseNode(cell->tower, 1);
                    cell->tower = ee::Ptr<zge::Node>();
                }
                bareCell(editor, column, row);
            }
        }
        editor->themeSetUp = 0;
        editor->rows = 0;
        editor->columns = 0;
        editor->theme = 0;
        releaseEditNode(ee::Ptr<ee::Ptr<zge::Node>>(sym::g_defaultTower));
        zge::leaveScene();
        zge::shutDownTokenReader();
        *g_editSceneEntered = 0;
    }
    READABLE_FUNCTION(clearParkCells, Verified);

    void clearParkObjects(ee::Ptr<ParkEditor> editor)
    {
        *g_parkClearing = 1;
        for (int32_t row = 0; row < editor->rows; ++row)
        {
            for (int32_t column = 0; column < editor->columns; ++column)
            {
                removeCellContents(editor, column, row, static_cast<uint8_t>(row));
                bareCell(editor, column, row);
            }
        }
        editor->themeSetUp = 0;
        *g_parkClearing = 0;
        releaseEditNode(ee::Ptr<ee::Ptr<zge::Node>>(sym::g_defaultTower));
        zge::shutDownTokenReader();
        *g_editSceneEntered = 0;
    }
    READABLE_FUNCTION(clearParkObjects, Verified);

    uint32_t cellInPark(ee::Ptr<ParkEditor> editor, int8_t column, int8_t row)
    {
        if (column < 0 || row < 0)
            return 0;
        if (!(column < editor->columns))
            return 0;
        return row < editor->rows;
    }
    READABLE_FUNCTION(cellInPark, Verified);

    uint32_t cellCanBeBuiltOn(ee::Ptr<ParkEditor> editor, int8_t column, int8_t row)
    {
        if (!(column < editor->columns) || column < 0)
            return 0;
        if (!(row < editor->rows) || row < 0)
            return 0;
        const uint8_t cellColumn = static_cast<uint8_t>(column);
        const uint8_t cellRow = static_cast<uint8_t>(row);
        if (!(themeCellKind(editor->theme, cellColumn, cellRow) & kThemeCellBuildable))
            return 0;
        if (editor->tool != kEditToolTerrain)
            return 1;
        if (themeCellKind(editor->theme, cellColumn, cellRow) & kThemeCellBit1)
            setCursorState(editor, kCursorFree);
        else
            setCursorState(editor, kCursorBlocked);
        return 1;
    }
    READABLE_FUNCTION(cellCanBeBuiltOn, Verified);

    namespace
    {
        // The engine's query: the nodes and faces a segment touches, less
        // the nodes of a list that ends with 0.
        constexpr int32_t kQuerySegment = 1;
        struct MarkerRayFrame
        {
            zge::QuerySegment ray;         // +0x00
            rider::collision::QueryHits hits; // +0x20
            uint8_t _28[8];
            zge::Matrix hitMatrix;         // +0x30
            ee::Ptr<zge::Node> skipped[2]; // +0x70
        };
        static_assert(offsetof(MarkerRayFrame, hits) == 0x20 && offsetof(MarkerRayFrame, hitMatrix) == 0x30);
        static_assert(offsetof(MarkerRayFrame, skipped) == 0x70);
        int32_t sceneQuery(int32_t kind, ee::Ptr<zge::QuerySegment> shape, uint32_t flags, uint32_t filter, uint32_t filterContext,
                           ee::Ptr<ee::Ptr<zge::Node>> nodes, ee::Ptr<rider::collision::QueryHits> hits)
        {
            return ee::game<int32_t>(sym::sceneQuery, kind, shape, flags, filter, filterContext, nodes, hits);
        }
        // The ray starts this far over the cell's ground (y grows downwards).
        constexpr ee::Float kMarkerRayHeight = 60.0f;
        constexpr ee::Float kMarkerSink = 0.5f;
        constexpr ee::Float kMarkerCellSize = 5.0f;
        constexpr ee::Float kMarkerHalfCell = 2.5f;
    }

    void cellMarkerPlace(ee::Ptr<ParkEditor> editor, int8_t column, int8_t row, ee::Ptr<zge::Node> node, ee::Ptr<zge::Vec4> position)
    {
        ee::Local<MarkerRayFrame> frame;
        const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
        const ee::Ptr<PlacedObject> object = cell->object;
        if (!object)
        {
            const ee::Float x = ee::toFloat(column) * kMarkerCellSize;
            const ee::Float z = ee::toFloat(row) * kMarkerCellSize;
            position->y = -ee::toFloat(cell->level);
            position->x = x - ee::toFloat(editor->columns - 1) * kMarkerHalfCell;
            position->z = z - ee::toFloat(editor->rows - 1) * kMarkerHalfCell;
            return;
        }
        frame->skipped[0] = node;
        frame->skipped[1] = ee::Ptr<zge::Node>();
        cellWorldPosition(editor, static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(object->column))),
                          static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(object->row))), position);
        frame->ray.start = *position;
        frame->ray.end = *position;
        frame->ray.start.y = frame->ray.start.y - kMarkerRayHeight;
        if (sceneQuery(kQuerySegment, &frame->ray, zge::kQueryExcludeListed, 0, 0, frame->skipped, &frame->hits))
        {
            const ee::Ptr<rider::collision::QueryNode> hit = frame->hits.nodes;
            const ee::Float *const point = hit->faces->point;
            position->x = point[0];
            position->y = point[1];
            position->z = point[2];
            position->w = point[3];
            zge::copyRotationAndTranslation(&frame->hitMatrix, zge::nodeMatrix(ee::Ptr<zge::Node>(hit->node), 0));
            zge::transformPoint(position, &frame->hitMatrix, position);
            position->y = position->y - kMarkerSink;
        }
    }
    READABLE_FUNCTION(cellMarkerPlace, Verified);
}
