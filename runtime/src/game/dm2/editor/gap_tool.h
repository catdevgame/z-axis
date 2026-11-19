// DM2's park editor: the gap tool's gaps. A gap is two cells of the park, its
// start and its end, each shown by a marker, with a name and the points it
// scores; the faces of the two cells carry a tag of the gap's own, which the
// game matches when the rider leaves one and lands on the other. DM2's own
// code (it links after the editor's block).
#pragma once

#include "park_editor.h"

#include "ee/ee.h"
#include "zge/container/list.h"
#include "zge/math/vec.h"
#include "zge/scene/node.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // No cell: an end of the gap that is not placed yet.
    constexpr int8_t kGapNoCell = -1;
    // The points the gap menu offers: 500, 1000, 2500, 5000, 7500, 10000.
    constexpr uint32_t kGapScoreChoices = 6;

    struct ParkGap
    {
        zge::ListNode link;             // +0x00
        ee::Ptr<zge::Node> startMarker; // +0x08 the markers' nodes, 0 while they are not shown
        ee::Ptr<zge::Node> endMarker;   // +0x0C
        int8_t startColumn, startRow;   // +0x10 kGapNoCell: not placed
        int8_t endColumn, endRow;       // +0x12
        char name[0x20];                // +0x14 "Gap 1", or what the player typed
        uint32_t scoreChoice;           // +0x34 the gap menu's selected entry: which of the points
        uint16_t id;                    // +0x38
        uint8_t _3a[2];
        uint32_t startTag;              // +0x3C the tags of the two cells' faces
        uint32_t endTag;                // +0x40
    };
    static_assert(offsetof(ParkGap, startColumn) == 0x10 && offsetof(ParkGap, name) == 0x14);
    static_assert(offsetof(ParkGap, scoreChoice) == 0x34 && offsetof(ParkGap, id) == 0x38);
    static_assert(offsetof(ParkGap, startTag) == 0x3C && sizeof(ParkGap) == 0x44);

    // The gaps' pool, list, points and line colours set up; every gap
    // removed.
    void setUpGaps();
    void clearGaps(ee::Ptr<ParkEditor> editor);
    // A second entry that only goes on to clearGaps.
    void clearGapsEntry(ee::Ptr<ParkEditor> editor);
    // The gap tool taken up: a new gap begun, and every gap's markers shown.
    void beginNewGap(ee::Ptr<ParkEditor> editor);
    void resetGapTool(ee::Ptr<ParkEditor> editor);
    // Every marker taken off the scene.
    void hideGapMarkers();
    void showGapMarkers(ee::Ptr<ParkEditor> editor);
    void releaseGapMarkers();

    // The new gap's markers: made at the cursor, released, and the one
    // being placed moved with the cursor (it spins as it goes).
    void newGapStartMarker(ee::Ptr<ParkEditor> editor);
    void newGapEndMarker(ee::Ptr<ParkEditor> editor);
    void releaseNewGapStartMarker();
    void releaseNewGapEndMarker();
    void moveGapMarkerToCursor(ee::Ptr<ParkEditor> editor);
    // An empty function of the module.
    void unusedGapStep();

    // The new gap's cells, name and points.
    void setNewGapStart(int8_t column, int8_t row);
    void setNewGapEnd(int8_t column, int8_t row);
    void clearNewGapEnd();
    void moveCursorToNewGapStart(ee::Ptr<ParkEditor> editor);
    void setNewGapScoreChoice(uint32_t choice);
    ee::Ptr<char> newGapName();
    void setNewGapName(ee::Ptr<const char> name);
    // The new gap joins the park's gaps, and another is begun.
    void addNewGap(ee::Ptr<ParkEditor> editor);

    // The id of the first gap with an end on the cell (after the gap `after`
    // when that is not -1), or -1.
    int32_t gapAtCell(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row, int32_t after);
    void removeGap(ee::Ptr<ParkEditor> editor, uint16_t id);
    void removeGapsAtCursor(ee::Ptr<ParkEditor> editor);
    // The tag the cell's faces carry for a gap: the gap's own where one
    // ends on it, else the tag such faces have before any gap.
    uint32_t gapTagAtCell(ee::Ptr<ParkEditor> editor, uint8_t column, uint8_t row);
    // The hash of a new name, "EDITGAP0" onwards.
    uint32_t newGapTag();
    // The gap's tags taken off its cells' faces again, unless another gap
    // uses the same tag.
    void restoreGapFaceTags(ee::Ptr<ParkEditor> editor, ee::Ptr<ParkGap> gap);

    void drawGapTool(ee::Ptr<ParkEditor> editor);
    void drawGapFaces(ee::Ptr<ParkEditor> editor, int8_t column, int8_t row);
    void drawGapLines();

    // The gaps in the park's stream.
    void saveGaps(ee::Ptr<ParkEditor> editor, ee::Address stream);
    void loadGaps(ee::Ptr<ParkEditor> editor, ee::Address stream);

    // The gaps told to the game (its table of 32), the new gap alone, and
    // the table emptied.
    void registerGaps();
    void registerNewGap(ee::Ptr<const char> name);
    void clearRegisteredGaps();
}
