#pragma once

// Records the adopted functions read: their layout and names are the work database's (readable_names_layout.tsv);
// each has only the fields code reads so far.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_records.h.

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
        uint32_t unused;            // +0x00 made 0; read nowhere (PS2, GameCube)
        uint32_t targetAnchorIndex; // +0x04 the target anchor's place among the object's anchors
        uint32_t targetFree;        // +0x08 1 while the target anchor holds no rail yet
        uint32_t targetAnchor;      // +0x0C the anchor the rail goes on, 0 for none
        ee::Ptr<EditObjectPart> preview;           // +0x10 the edit object of the rail shown there
        ee::Ptr<EditObjectPart> targetObject;      // +0x14 the object under the cursor, 0 for none
        uint32_t category;          // +0x18 the rails of the tool set's catalog
        ee::Ptr<PieceDef> pieceDef; // +0x1C the rail chosen
        uint32_t previewAnchor;     // +0x20 an EditAnchor of the tool's own: the preview hangs from it
        uint32_t pointerDisplay;    // +0x24 "EditObj_AnchorPointer", shown at the target anchor
    };
    static_assert(sizeof(EditRailToolPart) == 0x28 && offsetof(EditRailToolPart, targetAnchor) == 0xC);
    static_assert(offsetof(EditRailToolPart, targetObject) == 0x14 && offsetof(EditRailToolPart, pointerDisplay) == 0x24);
}

namespace aggressive::level
{
    // The level code's functions take these by their own namespace's names.
    using zge::editor::AngleWords;
    using zge::editor::CellIndex;
    using zge::editor::EditDisplay;
    using zge::editor::EditObjectPart;
    using zge::editor::SelectionColours;
}
