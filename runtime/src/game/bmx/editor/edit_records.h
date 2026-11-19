#pragma once

// Records the adopted functions read: their layout and names are the work database's (readable_names_layout.tsv);
// each has only the fields code reads so far.
// BMX XXX's own part of it; what both games have is zge/editor/edit_records.h.

#include "ee/ee.h"
#include "zge/editor/edit_records.h"
#include "zge/container/list.h"
#include "zge/math/box.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge { struct Task; } // (zge/platform/tasks.h)
namespace zge::editor { struct EditCamera; } // (zge/editor/park_camera.h)

namespace zge::editor
{
    // The rail tool's own part (level/edit_tool_rail.cpp): the object under the
    // cursor, which of its anchors takes the rail, the rail chosen and a preview
    // of it hanging there.
    struct EditRailToolPart
    {
        int32_t targetAnchorIndex;  // +0x00 the target anchor's place in the target object's list of anchors
        uint32_t place;             // +0x04 the RailPlace the rail goes on, as it lies in the park (whatever way the target is turned)
        int32_t placesRow;          // +0x08 the target's row of the rail places table, -1 for none
        uint32_t targetFree;        // +0x0C 1 while the target anchor holds no rail yet
        uint32_t targetAnchor;      // +0x10 the anchor the rail goes on, 0 for none
        ee::Ptr<EditObjectPart> preview;           // +0x14 the edit object of the rail shown there
        ee::Ptr<EditObjectPart> targetObject;      // +0x18 the object under the cursor, 0 for none
        uint32_t catalog;           // +0x1C the tool set's catalog (BMX XXX keeps it)
        uint32_t category;          // +0x20 the rails of the catalog
        ee::Ptr<PieceDef> pieceDef; // +0x24 the rail chosen
        uint32_t previewAnchor;     // +0x28 an EditAnchor of the tool's own: the preview hangs from it
    };
    static_assert(sizeof(EditRailToolPart) == 0x2C && offsetof(EditRailToolPart, targetFree) == 0xC && offsetof(EditRailToolPart, targetAnchor) == 0x10);
    static_assert(offsetof(EditRailToolPart, targetObject) == 0x18 && offsetof(EditRailToolPart, previewAnchor) == 0x28);
    static_assert(offsetof(EditRailToolPart, place) == 0x4 && offsetof(EditRailToolPart, placesRow) == 0x8);
    // Where a rail can go on a piece (assets\editor\anchor.txt names a piece's anchor for each, or "none", in this
    // order: its anchors are named rail_PIECE_top, _right, _bottom, _left, _centerhorizontal, _centervertical,
    // _diagonalONE and _diagonalTWO).
    enum RailPlace : uint32_t
    {
        kRailTop = 0,
        kRailRight = 1,
        kRailBottom = 2,
        kRailLeft = 3,
        kRailCentreHorizontal = 4,
        kRailCentreVertical = 5,
        kRailDiagonalOne = 6,
        kRailDiagonalTwo = 7,
        kRailPlaces = 8,
    };

    // A row of the rail places table: a piece that takes rails, and which of its anchors is at each place.
    struct RailPlacesRow
    {
        uint32_t pieceIndex;              // +0x00 the piece's index in the catalog
        int8_t anchorIndex[kRailPlaces];  // +0x04 by RailPlace: the anchor's place in the piece's list, -1 for none
    };
    static_assert(sizeof(RailPlacesRow) == 0xC);

    // The dynamics tool's own part (edit_tool_dynamics.cpp).
    struct EditDynamicsToolPart
    {
        int32_t targetAnchorIndex;                 // +0x00 the target anchor's place in the target object's list of anchors
        uint32_t madeZero[2];                      // +0x04, +0x08: made 0 with the tool, and nothing of the tool's reads them (the rail tool keeps its place and its table row there)
        uint32_t targetFree;                       // +0x0C 1 while the target anchor holds nothing yet
        uint32_t targetAnchor;                     // +0x10 the anchor the piece goes on, 0 for none
        ee::Ptr<EditObjectPart> trial;             // +0x14 an edit object of the category's first piece: anchors are tried with it
        ee::Ptr<EditObjectPart> preview;           // +0x18 the edit object of the piece shown at the target anchor
        ee::Ptr<EditObjectPart> targetObject;      // +0x1C the object under the cursor, 0 for none
        uint32_t catalog;                          // +0x20 the tool set's catalog
        uint32_t category;                         // +0x24 the dynamics of the catalog
        ee::Ptr<PieceDef> pieceDef;                // +0x28 the piece chosen
        uint32_t previewAnchor;                    // +0x2C an EditAnchor of the tool's own: the preview hangs from it
    };
    static_assert(sizeof(EditDynamicsToolPart) == 0x30 && offsetof(EditDynamicsToolPart, targetFree) == 0xC);
    static_assert(offsetof(EditDynamicsToolPart, trial) == 0x14 && offsetof(EditDynamicsToolPart, targetObject) == 0x1C);
    static_assert(offsetof(EditDynamicsToolPart, pieceDef) == 0x28 && offsetof(EditDynamicsToolPart, previewAnchor) == 0x2C);
}

