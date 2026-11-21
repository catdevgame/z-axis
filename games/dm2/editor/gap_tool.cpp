// DM2's park editor: the gap tool's gaps (gap_tool.h): the gap being made
// and its markers, the park's list of gaps, their face tags, their lines on
// the screen and their place in the park's stream. DM2's own code.

#include "gap_tool.h"
#include "edit_core.h"
#include "edit_display.h"
#include "edit_grid.h"
#include "edit_pools.h"
#include "edit_tags.h"
#include "edit_themes.h"
#include "park_grid.h"
#include "../game/gaps.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/container/list.h"
#include "zge/math/matrix.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/render/part.h"
#include "zge/render/primitives.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/editor/gap_tool.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The gap being made: its markers, cells, name and points until
        // addNewGap copies it into the park's list.
        inline ee::Global<ParkGap, sym::g_newGap> g_newGap;
        // The park's gaps, in the order they were made, and their pool (an
        // index in the editor's pool table).
        inline ee::Global<zge::List, sym::g_gaps> g_gaps;
        inline ee::Global<int8_t, sym::g_gapPool> g_gapPool;
        inline ee::Global<uint32_t, sym::g_gapCount> g_gapCount;
        // The next gap's id, and the number in the next tag's name.
        inline ee::Global<uint16_t, sym::g_nextGapId> g_nextGapId;
        inline ee::Global<uint32_t, sym::g_nextGapTagNumber> g_nextGapTagNumber;
        // The points and the line's colour for each entry of the gap menu.
        inline ee::Global<uint32_t[kGapScoreChoices], sym::g_gapPoints> g_gapPoints;
        inline ee::Global<zge::VertexColour[kGapScoreChoices], sym::g_gapColours> g_gapColours;
        // The heading, in degrees, of the new gap's markers as they spin.
        inline ee::Global<ee::Float, sym::g_gapStartMarkerHeading> g_startMarkerHeading;
        inline ee::Global<ee::Float, sym::g_gapEndMarkerHeading> g_endMarkerHeading;
        // The colour of a cell's gap faces before the pulse sets its alpha.
        inline ee::Global<zge::VertexColour, sym::gapFaceColour> g_gapFaceColour;
        // How far over the ground the gap faces are drawn.
        inline ee::Global<ee::Float, sym::gapFaceLift> g_gapFaceLift;
        inline ee::Global<uint8_t, sym::g_parkClearing> g_parkClearing;

        constexpr ee::Ptr<const char> kMarkerObject{sym::textGapMarkerObject}; // "EditObj_GapMarker"
        constexpr ee::Ptr<const char> kNameFormat{sym::textGapNameFormat};     // "Gap %d"
        constexpr ee::Ptr<const char> kTagFormat{sym::textGapTagFormat};       // "EDITGAP%d"
        constexpr ee::Ptr<const char> kNoName{sym::textGapNoName};             // ""

        constexpr uint32_t kGapPoolRecordsABlock = 0x10;
        constexpr uint32_t kGameGaps = 0x20;
        // A marker being placed turns this many degrees a frame.
        constexpr ee::Float kMarkerSpin = 3.0f;
        constexpr ee::Float kHalfTurn = 180.0f;
        constexpr ee::Float kFullTurn = 360.0f;
        // A gap's line runs this far above its markers' places.
        constexpr ee::Float kLineHeight = 5.0f;
        constexpr int32_t kPulseRate = 0x200;
        constexpr ee::Float kPulseSpan = 16.0f;
        constexpr ee::Float kLinePulseBase = 16.0f;
        constexpr ee::Float kFacePulseBase = 32.0f;
        constexpr uint32_t kTriangleCorners = 3;
        // drawPrimitiveStrips' flags for the gap faces.
        constexpr uint32_t kGapFacesFlags = 2;

        // A line of the screen between two places, as the frames below hold
        // it.
        struct LineFrame
        {
            zge::PrimitiveLine line;      // +0x00
            uint32_t _0c;
            zge::PrimitiveVertex ends[2]; // +0x10
            zge::VertexColour colour;     // +0x50
        };
        static_assert(offsetof(LineFrame, ends) == 0x10 && offsetof(LineFrame, colour) == 0x50);

        struct MarkerMoveFrame
        {
            zge::Vec4 position; // +0x00
            uint8_t column;     // +0x10
            uint8_t row;        // +0x11
        };
        static_assert(offsetof(MarkerMoveFrame, column) == 0x10);

        struct CursorCellFrame
        {
            uint8_t column, row;
        };

        struct GapFacesFrame
        {
            zge::Matrix matrix;                             // +0x00 the node's, for the faces' vertices
            zge::PrimitiveTriangle triangle;                // +0x40
            zge::PrimitiveVertex corners[kTriangleCorners]; // +0x50
            zge::VertexColour colour;                       // +0xB0
            uint8_t _b4[0x0C];
            uint8_t column, row;                            // +0xC0
            uint8_t _c2[2];
            ee::Ptr<uint16_t> faceCorners;                  // +0xC4 the face's three vertex numbers
            uint32_t tag;                                   // +0xC8
        };
        static_assert(offsetof(GapFacesFrame, triangle) == 0x40 && offsetof(GapFacesFrame, corners) == 0x50);
        static_assert(offsetof(GapFacesFrame, colour) == 0xB0 && offsetof(GapFacesFrame, column) == 0xC0);
        static_assert(offsetof(GapFacesFrame, faceCorners) == 0xC4 && offsetof(GapFacesFrame, tag) == 0xC8);

        // The park's gaps walked: the first of the list, the one after a gap (0 at the end).
        ee::Ptr<ParkGap> gapOf(ee::Ptr<zge::ListNode> node) { return zge::recordOfNode<ParkGap>(node); }
        ee::Ptr<ParkGap> firstGap() { return gapOf(g_gaps->head); }
        ee::Ptr<ParkGap> nextGap(ee::Ptr<ParkGap> gap) { return gapOf(gap->link.next); }
        // A vertex's position: its first quadword (x, y, z and the word after) as the Vec4 the position setters write.
        ee::Ptr<zge::Vec4> placeOf(ee::Ptr<zge::PrimitiveVertex> vertex)
        {
            return vertex.cast<zge::Vec4>();
        }
        // The node whose faces a cell's gap tags are on: the object's on the
        // cell, or the cell's ground tower.
        ee::Ptr<zge::Node> cellGapNode(ee::Ptr<ParkCell> cell, ee::Ptr<PlacedObject> object)
        {
            return object ? object->node : cell->tower;
        }

        ee::Ptr<zge::Node> newMarker()
        {
            return newWorldObjectInstance(zge::nameHash(kMarkerObject));
        }

        // A line from one node to another, kLineHeight above them, drawn in
        // the frame's colour.
        void drawLineBetween(ee::Ptr<LineFrame> frame, ee::Ptr<zge::Node> from, ee::Ptr<zge::Node> to)
        {
            nodePosition(from, placeOf(&frame->ends[0]));
            nodePosition(to, placeOf(frame->line.second));
            for (const ee::Ptr<zge::PrimitiveVertex> end : {frame->line.first, frame->line.second})
            {
                end->y = end->y - kLineHeight;
                end->u = 0.0f;
                end->v = 0.0f;
                zge::memoryCopy(end->rgba, &frame->colour, sizeof(zge::VertexColour));
            }
            zge::drawLines(0, ee::Address(&frame->line), 1, 0);
        }

        // A marker made for an end of a gap of the list, on the end's cell.
        void showMarker(ee::Ptr<ParkEditor> editor, ee::Ptr<ee::Ptr<zge::Node>> marker, int8_t column, int8_t row, ee::Ptr<zge::Vec4> position)
        {
            cellMarkerPlace(editor, column, row, ee::Ptr<zge::Node>(), position);
            const ee::Ptr<zge::Node> node = newMarker();
            *marker = node;
            setNodePosition(node, position);
            setNodeHeading(*marker, 0.0f);
        }

        // The cell's faces that carry `plain` tagged `tag` instead.
        void tagCellFaces(ee::Ptr<ParkEditor> editor, int8_t column, int8_t row, uint32_t tag)
        {
            const ee::Ptr<ParkCell> cell = parkCell(editor, column, row);
            const ee::Ptr<PlacedObject> object = cell->object;
            const uint32_t plain = editGapTag();
            replaceEditFaceTags(cellGapNode(cell, object), static_cast<int32_t>(plain), tag);
        }

        // Whether a gap of the list uses the tag.
        bool tagInUse(uint32_t tag)
        {
            for (ee::Ptr<ParkGap> other = firstGap(); other; other = nextGap(other))
            {
                if (other->startTag == tag || other->endTag == tag)
                    return true;
            }
            return false;
        }

        // The marker follows the cursor's cell and turns kMarkerSpin.
        void spinMarkerAtCursor(ee::Ptr<ParkEditor> editor, ee::Ptr<MarkerMoveFrame> frame, ee::Ptr<zge::Node> &marker, ee::Float &heading)
        {
            cellMarkerPlace(editor, static_cast<int8_t>(frame->column), static_cast<int8_t>(frame->row), marker, &frame->position);
            setNodePosition(marker, &frame->position);
            const ee::Float turned = heading + kMarkerSpin;
            heading = turned;
            if (kHalfTurn < turned)
                heading = turned - kFullTurn;
            setNodeHeading(marker, heading);
        }
    }

    void setUpGaps()
    {
        *g_gapPool = newEditPool(sizeof(ParkGap), kGapPoolRecordsABlock);
        zge::initList(&*g_gaps);
        uint32_t *const points = *g_gapPoints;
        points[0] = 500;
        points[5] = 10000;
        g_newGap->endMarker = ee::Ptr<zge::Node>();
        points[1] = 1000;
        points[2] = 2500;
        points[3] = 5000;
        points[4] = 7500;
        zge::VertexColour *const colours = *g_gapColours;
        colours[0] = {0xFF, 0xFF, 0xFF, 0xFF};
        colours[1] = {0x00, 0x00, 0xFF, 0xFF};
        colours[2] = {0x00, 0xFF, 0x00, 0xFF};
        colours[3] = {0x93, 0x0C, 0xD2, 0xFF};
        colours[4] = {0xFF, 0xFF, 0x00, 0xFF};
        colours[5] = {0xFF, 0x00, 0x00, 0xFF};
        g_newGap->startMarker = ee::Ptr<zge::Node>();
        *g_nextGapId = 0;
        *g_nextGapTagNumber = 0;
    }
    READABLE_FUNCTION(setUpGaps, Verified);

    void clearGaps(ee::Ptr<ParkEditor> editor)
    {
        // editPoolFree clears the record's address where it is kept.
        ee::Local<ee::Ptr<ParkGap>> gap;
        const ee::Ptr<ee::Ptr<ParkGap>> slot = gap;
        *gap = firstGap();
        while (*gap)
        {
            const ee::Ptr<ParkGap> next = nextGap(*gap);
            zge::listRemove(&*g_gaps, *gap);
            if ((*gap)->startMarker)
                releaseEditNode(&(*gap)->startMarker);
            if ((*gap)->endMarker)
                releaseEditNode(&(*gap)->endMarker);
            restoreGapFaceTags(editor, *gap);
            editPoolFree(*g_gapPool, slot);
            *gap = next;
        }
        zge::initList(&*g_gaps);
        hideGapMarkers();
        *g_nextGapId = 0;
        g_newGap->endMarker = ee::Ptr<zge::Node>();
        g_newGap->startMarker = ee::Ptr<zge::Node>();
        *g_nextGapTagNumber = 0;
        *g_gapCount = 0;
    }
    READABLE_FUNCTION(clearGaps, Verified);

    void clearGapsEntry(ee::Ptr<ParkEditor> editor)
    {
        game::tail(clearGaps, editor);
    }
    READABLE_FUNCTION(clearGapsEntry, Verified);

    void beginNewGap(ee::Ptr<ParkEditor> editor)
    {
        zge::memoryFill(&*g_newGap, 0, sizeof(ParkGap));
        newGapStartMarker(editor);
        game::tail(showGapMarkers, editor);
    }
    READABLE_FUNCTION(beginNewGap, Verified);

    void hideGapMarkers()
    {
        releaseGapMarkers();
        releaseNewGapStartMarker();
        game::tail(releaseNewGapEndMarker);
    }
    READABLE_FUNCTION(hideGapMarkers, Verified);

    void moveGapMarkerToCursor(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<MarkerMoveFrame> frame;
        cursorCell(editor, &frame->column, &frame->row);
        if (g_newGap->endMarker)
            spinMarkerAtCursor(editor, frame, g_newGap->endMarker, *g_endMarkerHeading);
        else if (g_newGap->startMarker)
            spinMarkerAtCursor(editor, frame, g_newGap->startMarker, *g_startMarkerHeading);
    }
    READABLE_FUNCTION(moveGapMarkerToCursor, Verified);

    void unusedGapStep()
    {
    }
    READABLE_FUNCTION(unusedGapStep, Draft);

    // The start's marker made at the cursor, the gap's cells not placed,
    // and its name "Gap" and the number it will have.
    void newGapStartMarker(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::Vec4> position;
        cursorWorldPosition(editor, position);
        const ee::Ptr<zge::Node> node = newMarker();
        g_newGap->startMarker = node;
        setNodePosition(node, position);
        setNodeHeading(g_newGap->startMarker, *g_startMarkerHeading);
        g_newGap->startRow = kGapNoCell;
        g_newGap->startColumn = kGapNoCell;
        zge::formatString(ee::Ptr<char>(g_newGap->name), kNameFormat, static_cast<uint32_t>(*g_nextGapId) + 1);
    }
    READABLE_FUNCTION(newGapStartMarker, Verified);

    void newGapEndMarker(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::Vec4> position;
        cursorWorldPosition(editor, position);
        const ee::Ptr<zge::Node> node = newMarker();
        g_newGap->endMarker = node;
        setNodePosition(node, position);
        setNodeHeading(g_newGap->endMarker, *g_endMarkerHeading);
        g_newGap->endRow = kGapNoCell;
        g_newGap->endColumn = kGapNoCell;
    }
    READABLE_FUNCTION(newGapEndMarker, Verified);

    void releaseNewGapStartMarker()
    {
        if (g_newGap->startMarker)
            game::tail(releaseEditNode, ee::Ptr<ee::Ptr<zge::Node>>(&g_newGap->startMarker));
    }
    READABLE_FUNCTION(releaseNewGapStartMarker, Verified);

    void releaseNewGapEndMarker()
    {
        if (g_newGap->endMarker)
            game::tail(releaseEditNode, ee::Ptr<ee::Ptr<zge::Node>>(&g_newGap->endMarker));
    }
    READABLE_FUNCTION(releaseNewGapEndMarker, Verified);

    void setNewGapStart(int8_t column, int8_t row)
    {
        g_newGap->startRow = row;
        g_newGap->startColumn = column;
    }
    READABLE_FUNCTION(setNewGapStart, Verified);

    void setNewGapEnd(int8_t column, int8_t row)
    {
        g_newGap->endRow = row;
        g_newGap->endColumn = column;
    }
    READABLE_FUNCTION(setNewGapEnd, Verified);

    // The gap tool's picture: the new gap's line once its start is placed
    // (grey, pulsing), the gap faces of the cells under its markers, and
    // every gap's line.
    void drawGapTool(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<LineFrame> frame;
        if (g_newGap->startRow != kGapNoCell)
        {
            frame->colour.alpha = 0xFF;
            const ee::Float pulse = pulseCosine(kPulseRate);
            frame->line.second = &frame->ends[1];
            frame->line.first = &frame->ends[0];
            frame->line.texture = 0;
            const uint8_t grey = static_cast<uint8_t>(ee::toInt(pulse * kPulseSpan + kLinePulseBase));
            frame->colour.red = grey;
            frame->colour.blue = grey;
            frame->colour.green = grey;
            drawLineBetween(frame, g_newGap->startMarker, g_newGap->endMarker);
        }
        if (g_newGap->startMarker)
            drawGapFaces(editor, g_newGap->startColumn, g_newGap->startRow);
        if (g_newGap->endMarker)
            drawGapFaces(editor, g_newGap->endColumn, g_newGap->endRow);
        drawGapLines();
    }
    READABLE_FUNCTION(drawGapTool, Verified);

    void addNewGap(ee::Ptr<ParkEditor> editor)
    {
        // An end on a cell no gap uses yet gets a tag of its own.
        const uint32_t startTag = gapTagAtCell(editor, static_cast<uint8_t>(g_newGap->startColumn), static_cast<uint8_t>(g_newGap->startRow));
        if (startTag == editGapTag())
        {
            g_newGap->startTag = newGapTag();
            tagCellFaces(editor, g_newGap->startColumn, g_newGap->startRow, g_newGap->startTag);
        }
        else
        {
            g_newGap->startTag = startTag;
        }
        const uint32_t endTag = gapTagAtCell(editor, static_cast<uint8_t>(g_newGap->endColumn), static_cast<uint8_t>(g_newGap->endRow));
        if (endTag == editGapTag())
        {
            g_newGap->endTag = newGapTag();
            tagCellFaces(editor, g_newGap->endColumn, g_newGap->endRow, g_newGap->endTag);
        }
        else
        {
            g_newGap->endTag = endTag;
        }

        const uint16_t id = *g_nextGapId;
        g_newGap->id = id;
        *g_nextGapId = static_cast<uint16_t>(id + 1);
        const ee::Ptr<ParkGap> gap(editPoolAllocate(*g_gapPool));
        zge::memoryCopy(gap, &*g_newGap, sizeof(ParkGap));
        gap->startMarker = ee::Ptr<zge::Node>();
        gap->endMarker = ee::Ptr<zge::Node>();
        zge::listPush(&*g_gaps, gap);
        releaseNewGapStartMarker();
        releaseNewGapEndMarker();
        newGapStartMarker(editor);
        showGapMarkers(editor);
        *g_gapCount = *g_gapCount + 1;
    }
    READABLE_FUNCTION(addNewGap, Verified);

    void removeGapsAtCursor(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<CursorCellFrame> cell;
        cursorCell(editor, &cell->column, &cell->row);
        int32_t id = gapAtCell(editor, cell->column, cell->row, -1);
        while (id != -1)
        {
            removeGap(editor, static_cast<uint16_t>(id));
            id = gapAtCell(editor, cell->column, cell->row, -1);
            *g_gapCount = *g_gapCount - 1;
        }
    }
    READABLE_FUNCTION(removeGapsAtCursor, Verified);

    void removeGap(ee::Ptr<ParkEditor> editor, uint16_t id)
    {
        // editPoolFree clears the record's address where it is kept.
        ee::Local<ee::Ptr<ParkGap>> gap;
        const ee::Ptr<ee::Ptr<ParkGap>> slot = gap;
        *gap = firstGap();
        while (*gap)
        {
            if ((*gap)->id != id)
            {
                *gap = nextGap(*gap);
                continue;
            }
            zge::listRemove(&*g_gaps, *gap);
            if ((*gap)->startMarker)
                releaseEditNode(&(*gap)->startMarker);
            if ((*gap)->endMarker)
                releaseEditNode(&(*gap)->endMarker);
            restoreGapFaceTags(editor, *gap);
            const ee::Ptr<ParkGap> next = nextGap(*gap);
            editPoolFree(*g_gapPool, slot);
            *gap = next;
        }
    }
    READABLE_FUNCTION(removeGap, Verified);

    // The cell is compared with the gaps' signed cells as an unsigned byte:
    // a cell of 128 or more matches none.
    int32_t gapAtCell(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row, int32_t after)
    {
        static_cast<void>(editor);
        bool looking = after == -1;
        for (ee::Ptr<ParkGap> gap = firstGap(); gap; gap = nextGap(gap))
        {
            if (!looking)
            {
                looking = static_cast<int32_t>(gap->id) == after;
                continue;
            }
            if (gap->startColumn == static_cast<int32_t>(column) && gap->startRow == static_cast<int32_t>(row))
                return gap->id;
            if (gap->endColumn == static_cast<int32_t>(column) && gap->endRow == static_cast<int32_t>(row))
                return gap->id;
        }
        return -1;
    }
    READABLE_FUNCTION(gapAtCell, Verified);

    void saveGaps(ee::Ptr<ParkEditor> editor, ee::Address stream)
    {
        static_cast<void>(editor);
        ee::Local<uint8_t> more;
        parkStreamWrite(stream, &*g_nextGapId, 2);
        parkStreamWrite(stream, &*g_nextGapTagNumber, 4);
        for (ee::Ptr<ParkGap> gap = firstGap(); gap; gap = nextGap(gap))
        {
            *more = 1;
            parkStreamWrite(stream, more, 1);
            parkStreamWrite(stream, &gap->startColumn, 1);
            parkStreamWrite(stream, &gap->startRow, 1);
            parkStreamWrite(stream, &gap->endColumn, 1);
            parkStreamWrite(stream, &gap->endRow, 1);
            parkStreamWrite(stream, gap->name, sizeof(gap->name));
            parkStreamWrite(stream, &gap->scoreChoice, 1);
            parkStreamWrite(stream, &gap->id, 2);
            parkStreamWrite(stream, &gap->startTag, 4);
            parkStreamWrite(stream, &gap->endTag, 4);
        }
        *more = 0;
        parkStreamWrite(stream, more, 1);
    }
    READABLE_FUNCTION(saveGaps, Verified);

    void loadGaps(ee::Ptr<ParkEditor> editor, ee::Address stream)
    {
        ee::Local<uint8_t> more;
        clearGaps(editor);
        parkStreamRead(stream, &*g_nextGapId, 2);
        parkStreamRead(stream, &*g_nextGapTagNumber, 4);
        parkStreamRead(stream, more, 1);
        while (*more)
        {
            const ee::Ptr<ParkGap> gap(editPoolAllocate(*g_gapPool));
            gap->startMarker = ee::Ptr<zge::Node>();
            gap->endMarker = ee::Ptr<zge::Node>();
            parkStreamRead(stream, &gap->startColumn, 1);
            parkStreamRead(stream, &gap->startRow, 1);
            parkStreamRead(stream, &gap->endColumn, 1);
            parkStreamRead(stream, &gap->endRow, 1);
            parkStreamRead(stream, gap->name, sizeof(gap->name));
            parkStreamRead(stream, &gap->scoreChoice, 1);
            parkStreamRead(stream, &gap->id, 2);
            parkStreamRead(stream, &gap->startTag, 4);
            parkStreamRead(stream, &gap->endTag, 4);
            zge::listPush(&*g_gaps, gap);
            tagCellFaces(editor, gap->startColumn, gap->startRow, gap->startTag);
            tagCellFaces(editor, gap->endColumn, gap->endRow, gap->endTag);
            parkStreamRead(stream, more, 1);
        }
    }
    READABLE_FUNCTION(loadGaps, Verified);

    // The faces of the cell that carry the cell's gap tag, drawn a little
    // over the ground in a pulsing blue. A cell of kGapNoCell is the
    // cursor's; a cell an object covers is the object's own. The cursor
    // turns red on a cell whose theme does not allow a gap's end.
    void drawGapFaces(ee::Ptr<ParkEditor> editor, int8_t column, int8_t row)
    {
        ee::Local<GapFacesFrame> frame;
        frame->column = static_cast<uint8_t>(column);
        frame->row = static_cast<uint8_t>(row);
        frame->colour = *g_gapFaceColour;
        frame->colour.alpha = static_cast<uint8_t>(ee::toInt(pulseCosine(kPulseRate) * kPulseSpan + kFacePulseBase));
        if (static_cast<int8_t>(frame->column) == kGapNoCell)
            cursorCell(editor, &frame->column, &frame->row);
        const ee::Ptr<ParkCell> cell = parkCell(editor, static_cast<int8_t>(frame->column), static_cast<int8_t>(frame->row));
        const ee::Ptr<PlacedObject> object = cell->object;
        ee::Ptr<ee::Ptr<zge::Node>> node;
        if (object)
        {
            frame->column = object->column;
            frame->row = cell->object->row;
            node = &object->node;
        }
        else
        {
            node = &cell->tower;
        }
        if (!(themeCellKind(editor->theme, frame->column, frame->row) & kThemeCellBit1))
        {
            setCursorState(editor, kCursorBlocked);
            return;
        }
        setCursorState(editor, kCursorFree);

        if (!hasCollisionFrame(*node))
            return;
        const ee::Ptr<zge::CollisionFrame> collision = (*node)->lods;
        zge::copyRotationAndTranslation(&frame->matrix, zge::nodeMatrix(*node, 0));
        const ee::Ptr<zge::ZgePart> mesh = collision->part;
        if (!mesh)
            return;
        frame->tag = gapTagAtCell(editor, frame->column, frame->row);
        const uint32_t faces = mesh->faceCount;
        for (uint32_t face = 0; face < faces; ++face)
        {
            if (static_cast<uint32_t>(editFaceTag(*node, face)) != frame->tag)
                continue;
            frame->triangle.texture = 0;
            zge::faceVertices(mesh, face, &frame->faceCorners);
            for (uint32_t i = 0; i < kTriangleCorners; ++i)
            {
                const ee::Ptr<zge::PrimitiveVertex> corner = &frame->corners[i];
                frame->triangle.vertices[i] = corner;
                zge::vertexPosition(mesh, frame->faceCorners[i], placeOf(corner));
                corner->v = 0.0f;
                corner->u = 0.0f;
                corner->y = corner->y - *g_gapFaceLift;
                zge::memoryCopy(corner->rgba, &frame->colour, sizeof(zge::VertexColour));
            }
            zge::drawPrimitiveStrips(kGapFacesFlags, &frame->triangle, 1, &frame->matrix);
        }
    }
    READABLE_FUNCTION(drawGapFaces, Verified);

    void showGapMarkers(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::Vec4> position;
        for (ee::Ptr<ParkGap> gap = firstGap(); gap; gap = nextGap(gap))
        {
            if (!gap->startMarker)
                showMarker(editor, &gap->startMarker, gap->startColumn, gap->startRow, position);
            if (!gap->endMarker)
                showMarker(editor, &gap->endMarker, gap->endColumn, gap->endRow, position);
        }
    }
    READABLE_FUNCTION(showGapMarkers, Verified);

    void releaseGapMarkers()
    {
        for (ee::Ptr<ParkGap> gap = firstGap(); gap; gap = nextGap(gap))
        {
            if (gap->startMarker)
                releaseEditNode(&gap->startMarker);
            if (gap->endMarker)
                releaseEditNode(&gap->endMarker);
        }
    }
    READABLE_FUNCTION(releaseGapMarkers, Verified);

    // Every gap's line from its start to its end, in the colour of the
    // points it scores.
    void drawGapLines()
    {
        ee::Local<LineFrame> frame;
        for (ee::Ptr<ParkGap> gap = firstGap(); gap; gap = nextGap(gap))
        {
            frame->colour = (*g_gapColours)[gap->scoreChoice];
            frame->line.texture = 0;
            frame->line.first = &frame->ends[0];
            frame->line.second = &frame->ends[1];
            drawLineBetween(frame, gap->startMarker, gap->endMarker);
        }
    }
    READABLE_FUNCTION(drawGapLines, Verified);

    // Bare ground has one of four tags by the cell's place (odd or even
    // column and row); a cell with an object has the plain gap tag.
    uint32_t gapTagAtCell(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row)
    {
        for (ee::Ptr<ParkGap> gap = firstGap(); gap; gap = nextGap(gap))
        {
            if (gap->startColumn == static_cast<int32_t>(column) && gap->startRow == static_cast<int32_t>(row))
                return gap->startTag;
            if (gap->endColumn == static_cast<int32_t>(column) && gap->endRow == static_cast<int32_t>(row))
                return gap->endTag;
        }
        if (parkCell(editor, column, row)->object)
            return editGapTag();
        return editGapTagOf((column & 1u) * 2 + 1 + (row & 1u));
    }
    READABLE_FUNCTION(gapTagAtCell, Verified);

    uint32_t newGapTag()
    {
        ee::Local<char[0x40]> name;
        zge::formatString(name, kTagFormat, *g_nextGapTagNumber);
        *g_nextGapTagNumber = *g_nextGapTagNumber + 1;
        return zge::nameHash(name);
    }
    READABLE_FUNCTION(newGapTag, Verified);

    // The second retagging is the original's tail call.
    void restoreGapFaceTags(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkGap> gap)
    {
        if (!tagInUse(gap->startTag))
        {
            const ee::Ptr<ParkCell> cell = parkCell(editor, gap->startColumn, gap->startRow);
            const ee::Ptr<PlacedObject> object = cell->object;
            const uint32_t plain = editGapTag();
            replaceEditFaceTags(cellGapNode(cell, object), static_cast<int32_t>(gap->startTag), plain);
        }
        if (!tagInUse(gap->endTag))
        {
            const ee::Ptr<ParkCell> cell = parkCell(editor, gap->endColumn, gap->endRow);
            const ee::Ptr<PlacedObject> object = cell->object;
            const uint32_t plain = editGapTag();
            game::tail(replaceEditFaceTags, cellGapNode(cell, object), static_cast<int32_t>(gap->endTag), plain);
        }
    }
    READABLE_FUNCTION(restoreGapFaceTags, Verified);

    void setNewGapScoreChoice(uint32_t choice)
    {
        g_newGap->scoreChoice = choice;
    }
    READABLE_FUNCTION(setNewGapScoreChoice, Verified);

    void registerGaps()
    {
        ee::Local<LevelGap> record;
        for (ee::Ptr<ParkGap> gap = firstGap(); gap; gap = nextGap(gap))
        {
            record->flags = kGapOneWay; // a park's gap counts from its start to its end
            zge::stringCopy(record->name, gap->name);
            record->from = gap->startTag;
            record->to = gap->endTag;
            record->points = (*g_gapPoints)[gap->scoreChoice];
            setLevelGap(gap->id, record);
        }
    }
    READABLE_FUNCTION(registerGaps, Verified);

    void registerNewGap(ee::Ptr<const char> name)
    {
        ee::Local<LevelGap> record;
        record->flags = kGapOneWay;
        zge::stringCopy(record->name, name);
        record->from = g_newGap->startTag;
        record->to = g_newGap->endTag;
        record->points = (*g_gapPoints)[g_newGap->scoreChoice];
        setLevelGap(g_newGap->id, record);
    }
    READABLE_FUNCTION(registerNewGap, Verified);

    void clearRegisteredGaps()
    {
        ee::Local<LevelGap> record;
        record->flags = 0;
        record->from = 0;
        record->to = 0;
        record->points = 0;
        zge::stringCopy(record->name, kNoName);
        for (uint32_t index = 0; index < kGameGaps; ++index)
            setLevelGap(index, record);
    }
    READABLE_FUNCTION(clearRegisteredGaps, Verified);

    ee::Ptr<char> newGapName()
    {
        return g_newGap->name;
    }
    READABLE_FUNCTION(newGapName, Verified);

    void setNewGapName(ee::Ptr<const char> name)
    {
        game::tail(zge::stringCopy, ee::Ptr<char>(g_newGap->name), name);
    }
    READABLE_FUNCTION(setNewGapName, Verified);

    void moveCursorToNewGapStart(ee::Ptr<ParkEditor> editor)
    {
        game::tail(moveCursorToCell, editor, static_cast<uint8_t>(g_newGap->startColumn), static_cast<uint8_t>(g_newGap->startRow));
    }
    READABLE_FUNCTION(moveCursorToNewGapStart, Verified);

    void clearNewGapEnd()
    {
        g_newGap->endRow = kGapNoCell;
        g_newGap->endColumn = kGapNoCell;
    }
    READABLE_FUNCTION(clearNewGapEnd, Verified);

    // The park's gaps dropped; with the gap tool in hand it starts over.
    void resetGapTool(ee::Ptr<ParkEditor> editor)
    {
        clearGaps(editor);
        if (editor->tool == kEditToolGap)
        {
            editor->toolStep = 0;
            game::tail(beginNewGap, editor);
        }
    }
    READABLE_FUNCTION(resetGapTool, Verified);
}
