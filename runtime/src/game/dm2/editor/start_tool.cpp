// DM2's park editor: the start tool (start_tool.h): the park's start and
// its two markers. DM2's own code.

#include "start_tool.h"
#include "editor_state.h"
#include "edit_core.h"
#include "edit_display.h"
#include "edit_grid.h"
#include "park_grid.h"
#include "rail_tool.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/scene/locators.h"
#include "zge/scene/node.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/editor/start_tool.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The markers' nodes: the cursor's, then the placed start's.
        constexpr uint32_t kCursorMarker = 0;
        constexpr uint32_t kPlacedMarker = 1;
        inline ee::Global<ee::Ptr<zge::Node>[2], sym::g_startMarkers> g_startMarkers;
        // Scaffolding (unnamed-field): a byte the start tool clears and
        // nothing of the module sets; while it is not 0 placeStart does
        // nothing.
        inline ee::Global<uint8_t, sym::g_startPlacingOff> g_startPlacingOff;
        // The cursor's marker's heading in quarter turns, 0 .. 3.
        inline ee::Global<uint8_t, sym::g_startMarkerTurns> g_startMarkerTurns;

        constexpr ee::Ptr<const char> kMarkerObject{sym::textStartMarkerObject}; // "EditObj_Player1_Icon"
        constexpr ee::Ptr<const char> kSpawnName{sym::textStartSpawnName};       // "PlayerOne"

        constexpr ee::Float kQuarterTurnDegrees = 90.0f;
        constexpr ee::Float kQuarterTurnAngle = 16384.0f;
        // Objects 148 .. 179 of the catalog: a start cannot stand on them.
        constexpr uint8_t kFirstNoStartObject = 148;
        constexpr uint8_t kNoStartObjects = 32;
        // Scaffolding (value-named): the piece category whose entry, when
        // current, takes the placed start off; which category that is, is
        // not settled.
        constexpr uint8_t kCategoryThatUnplacesStart = 14;
        // The park's edges for pointInLevel.
        constexpr ee::Float kHalfCellSize = 2.5f;
        constexpr ee::Float kEdgeMargin = 15.0f;

        struct MarkerPlaceFrame
        {
            zge::Vec4 position; // +0x00
            uint8_t column;     // +0x10
            uint8_t row;        // +0x11
        };
        static_assert(offsetof(MarkerPlaceFrame, column) == 0x10);

        struct CursorCellFrame
        {
            uint8_t column, row;
        };

        struct SpawnFrame
        {
            zge::Matrix matrix; // +0x00
            zge::Vec4 position; // +0x40
        };
        static_assert(offsetof(SpawnFrame, position) == 0x40);


        ee::Ptr<zge::Node> newMarker()
        {
            return newWorldObjectInstance(zge::nameHash(kMarkerObject));
        }
    }

    void clearStartMarkers(ee::Ptr<ParkEditor> editor)
    {
        (*g_startMarkers)[kPlacedMarker] = ee::Ptr<zge::Node>();
        (*g_startMarkers)[kCursorMarker] = ee::Ptr<zge::Node>();
        *g_startPlacingOff = 0;
        *g_startMarkerTurns = 0;
        editor->startPlaced = 0;
    }
    READABLE_FUNCTION(clearStartMarkers, Verified);

    void freeStartMarkers()
    {
        game::tail(releaseStartMarkers);
    }
    READABLE_FUNCTION(freeStartMarkers, Draft);

    void removePlacedStart(ee::Ptr<ParkEditor> editor)
    {
        if ((*g_startMarkers)[kPlacedMarker])
            releaseEditNode(&(*g_startMarkers)[kPlacedMarker]);
        editor->startPlaced = 0;
    }
    READABLE_FUNCTION(removePlacedStart, Verified);

    void moveStartMarkerToCursor(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<MarkerPlaceFrame> frame;
        ee::Ptr<zge::Node> &marker = (*g_startMarkers)[kCursorMarker];
        if (!marker)
            return;
        cursorCell(editor, &frame->column, &frame->row);
        cellMarkerPlace(editor, static_cast<int8_t>(frame->column), static_cast<int8_t>(frame->row), marker, &frame->position);
        setNodePosition(marker, &frame->position);
        setNodeHeading(marker, ee::toFloat(*g_startMarkerTurns) * kQuarterTurnDegrees);
        if (currentToolCategory() == kCategoryThatUnplacesStart)
            editor->startPlaced = 0;
    }
    READABLE_FUNCTION(moveStartMarkerToCursor, Verified);

    void newStartMarkers(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::Vec4> position;
        cursorWorldPosition(editor, position);
        *g_startPlacingOff = 0;
        const ee::Ptr<zge::Node> cursorMarker = newMarker();
        (*g_startMarkers)[kCursorMarker] = cursorMarker;
        setNodePosition(cursorMarker, position);
        if (editor->startPlaced)
        {
            cellWorldPosition(editor, static_cast<uint32_t>(static_cast<int32_t>(editor->startColumn)),
                              static_cast<uint32_t>(static_cast<int32_t>(editor->startRow)), position);
            const ee::Ptr<zge::Node> placedMarker = newMarker();
            (*g_startMarkers)[kPlacedMarker] = placedMarker;
            setNodePosition(placedMarker, position);
        }
    }
    READABLE_FUNCTION(newStartMarkers, Verified);

    void releaseStartMarkers()
    {
        for (uint32_t marker = kCursorMarker; marker <= kPlacedMarker; ++marker)
        {
            if ((*g_startMarkers)[marker])
                releaseEditNode(&(*g_startMarkers)[marker]);
        }
    }
    READABLE_FUNCTION(releaseStartMarkers, Verified);

    void turnStartMarker()
    {
        *g_startMarkerTurns = static_cast<uint8_t>((*g_startMarkerTurns + 1) & 3);
    }
    READABLE_FUNCTION(turnStartMarker, Verified);

    uint32_t pointInLevel(ee::Ptr<const zge::Vec4> point)
    {
        uint32_t inside = 1;
        const ee::Ptr<ParkEditor> editor = parkEditor();
        const ee::Float halfWidth = ee::toFloat(editor->columns) * kHalfCellSize - kEdgeMargin;
        const ee::Float halfDepth = ee::toFloat(editor->rows) * kHalfCellSize - kEdgeMargin;
        const ee::Float x = point->x;
        if (halfWidth < x || x < -halfWidth)
            inside = 0;
        const ee::Float z = point->z;
        if (halfDepth < z || z < -halfDepth)
            inside = 0;
        return inside;
    }
    READABLE_FUNCTION(pointInLevel, Verified);

    uint32_t startFitsAtCursor(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<CursorCellFrame> cell;
        cursorCell(editor, &cell->column, &cell->row);
        const ee::Ptr<PlacedObject> object = parkCell(editor, cell->column, cell->row)->object;
        if (object && static_cast<uint8_t>(object->object - kFirstNoStartObject) < kNoStartObjects)
            return 0;
        return cellHasRail(editor, cell->column, cell->row) == 0;
    }
    READABLE_FUNCTION(startFitsAtCursor, Verified);

    void placeStart(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<MarkerPlaceFrame> frame;
        cursorCell(editor, &frame->column, &frame->row);
        if (*g_startPlacingOff)
            return;
        if ((*g_startMarkers)[kPlacedMarker])
            releaseEditNode(&(*g_startMarkers)[kPlacedMarker]);
        editor->startColumn = static_cast<int8_t>(frame->column);
        editor->startRow = static_cast<int8_t>(frame->row);
        editor->startPlaced = 1;
        editor->startTurns = static_cast<int8_t>(*g_startMarkerTurns);
        cellMarkerPlace(editor, static_cast<int8_t>(frame->column), static_cast<int8_t>(frame->row), (*g_startMarkers)[kCursorMarker],
                        &frame->position);
        const ee::Ptr<zge::Node> placedMarker = newMarker();
        (*g_startMarkers)[kPlacedMarker] = placedMarker;
        setNodePosition(placedMarker, &frame->position);
        setNodeHeading((*g_startMarkers)[kPlacedMarker], ee::toFloat(editor->startTurns) * kQuarterTurnDegrees);
    }
    READABLE_FUNCTION(placeStart, Verified);

    void renewStartMarker(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::Vec4> position;
        ee::Ptr<zge::Node> &marker = (*g_startMarkers)[kCursorMarker];
        if (marker)
            releaseEditNode(&marker);
        *g_startPlacingOff = 0;
        marker = newMarker();
        cursorWorldPosition(editor, position);
        setNodePosition(marker, position);
        setNodeHeading(marker, ee::toFloat(*g_startMarkerTurns) * kQuarterTurnDegrees);
    }
    READABLE_FUNCTION(renewStartMarker, Verified);

    void setParkSpawn(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<SpawnFrame> frame;
        zge::matrixIdentity(&frame->matrix);
        const zge::Angle heading = static_cast<zge::Angle>(ee::toInt(ee::toFloat(editor->startTurns) * kQuarterTurnAngle));
        zge::matrixRotateY(&frame->matrix, &frame->matrix, heading);
        cellMarkerPlace(editor, editor->startColumn, editor->startRow, ee::Ptr<zge::Node>(), &frame->position);
        zge::matrixTranslated(&frame->matrix, &frame->matrix, &frame->position.xyz());
        setSpawnPosition(&frame->matrix, kSpawnName);
    }
    READABLE_FUNCTION(setParkSpawn, Verified);

    void saveParkStart(ee::Ptr<ParkEditor> editor, ee::Address stream)
    {
        parkStreamWrite(stream, &editor->startColumn, 1);
        parkStreamWrite(stream, &editor->startRow, 1);
        parkStreamWrite(stream, &editor->startTurns, 1);
        game::tail(parkStreamWrite, stream, ee::Address(&editor->startPlaced), 1);
    }
    READABLE_FUNCTION(saveParkStart, Verified);

    void loadParkStart(ee::Ptr<ParkEditor> editor, ee::Address stream)
    {
        parkStreamRead(stream, &editor->startColumn, 1);
        parkStreamRead(stream, &editor->startRow, 1);
        parkStreamRead(stream, &editor->startTurns, 1);
        game::tail(parkStreamRead, stream, ee::Address(&editor->startPlaced), 1);
    }
    READABLE_FUNCTION(loadParkStart, Verified);
}

namespace game::dm2
{

    namespace
    {
        // The spawn points (the level's "Spawn" scene points) by name.
        constexpr ee::Ptr<const char> kSpawnKind{sym::spawnKind}; // "Spawn"
    }

    // addSpawnPoint (matrix, name): a "Spawn" scene point of that name added
    // at the matrix's place and turn (no tags; its index left as the frame
    // has it, kept).
    void addSpawnPoint(ee::Ptr<zge::Matrix> matrix, ee::Ptr<const char> name)
    {
        ee::Local<zge::ScenePoint> point;
        point->kindHash = zge::nameHash(kSpawnKind.get());
        point->nameHash = zge::nameHash(name.get());
        point->tagBytes = 0;
        point->tags = ee::Ptr<zge::TagRecord>();
        point->position = matrix->translation();
        zge::matrixToQuaternion(&point->orientation, matrix.get());
        zge::addScenePoint(point);
    }
    READABLE_FUNCTION_WAITS(addSpawnPoint, Draft);

    // setSpawnPosition (matrix, name): the "Spawn" scene point of that name
    // moved to the matrix's place and turn, or added when the level has
    // none.
    void setSpawnPosition(ee::Ptr<zge::Matrix> matrix, ee::Ptr<const char> name)
    {
        ee::Local<zge::LocatorList> spawns;
        const uint32_t nameHash = zge::nameHash(name.get());
        zge::findLocators(spawns, zge::nameHash(kSpawnKind.get()), nameHash);
        if (spawns->head)
        {
            const ee::Ptr<zge::ScenePoint> spawn(spawns->head->point);
            spawn->position = matrix->translation();
            zge::matrixToQuaternion(&spawn->orientation, matrix.get());
        }
        else
            addSpawnPoint(matrix, name);
        zge::freeLocators(spawns);
    }
    READABLE_FUNCTION_WAITS(setSpawnPosition, Verified);
}
