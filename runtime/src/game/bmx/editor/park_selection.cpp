// The park editor's selection (class EditSelection): the cursor that eases to its
// cell, the rectangle dragged out from it, and the pieces it holds, each on an
// anchor of its own while it moves with the cursor.
// BMX XXX's own part of it; what both games have is zge/editor/park_selection.cpp.

#include "zge/memory/regions.h"
#include "zge/editor/park_selection.h"
#include "zge/text/token_reader.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "bmx/editor/edit_records.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "bmx/editor/matrix_points.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_ui.h"
#include "bmx/editor/edit_object.h"
#include "bmx/editor/park_selection.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_messages.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/editor/park_selection.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kPoolFree = sym::poolFree;
        // Registered at the addresses of other functions of identical code (game::callAt).
        constexpr uint32_t kTranslateEditObject = sym::translateEditObject;
        constexpr uint32_t kNameEditTowerObject = sym::g_selectionEditTowerObjectName; // "EditTowerObject"

        // editObjectFlag's questions, by what this file does with the answer.
        constexpr uint32_t kFlagHasHeight = 0, kFlagRemovable = 1, kFlagShowsBox = 2, kFlagSnaps = 3;
        constexpr ee::Float kLevelTolerance = 0.01f;          // a piece's cells at the height of its anchor
    }

    namespace
    {
        // Registered at the addresses of other functions of identical code.
        ee::Ptr<EditObjectPart> attachedObject(uint32_t anchor) { return anchorAttachedObject(anchor); }
    }

    namespace
    {
        namespace tail
        {
        int32_t poolFree(uint32_t pool, uint32_t record) { return ee::tailCall<int32_t>(kPoolFree, pool, record); }
        }
    }

    namespace
    {
        namespace tail
        {
        }
    }

    // A selected cell's piece going away: a removable piece's record is copied to the cell
    // pool and its other cells collected. A cell with no piece (BMX XXX): while the selection
    // goes through towers, everything standing on the anchors of the cell's tower is taken
    // off and deleted.
    void collectSelectedPiece(uint32_t address, uint32_t recordAddress)
    {
        const ee::Ptr<EditSelection> selection(address);
        const ee::Ptr<SelectionRecord> record(recordAddress);
        const ee::Ptr<EditObjectPart> piece = record->object;
        if (piece != 0)
        {
            if (editObjectFlag(piece, kFlagRemovable) != 0)
            {
                const uint32_t copy = zge::poolAlloc(ee::Ptr<zge::Pool>(selection->cellPool));
                *ee::Ptr<SelectionRecord>(copy) = *record;
                game::tail(collectCellPiece, address, recordAddress);
            }
            return;
        }
        const uint32_t cellAnchor = parkCellAnchor(selection->park, &record->cell);
        if (selection->throughTowers != 1)
            return;
        const ee::Ptr<EditObjectPart> tower = attachedObject(cellAnchor);
        if (tower == 0 || zge::compareStrings(editObjectClassName(tower), kNameEditTowerObject) != 0)
            return;
        for (uint32_t anchor = firstAnchor(editObjectAnchors(tower)); anchor != 0; anchor = nextAnchor(anchor))
        {
            const ee::Ptr<EditObjectPart> standing = attachedObject(anchor);
            if (standing != 0)
            {
                detachFromAnchor(anchor);
                broadcastEditMessage(standing, sym::makeDeleteMessage, 0);
            }
        }
    }
    READABLE_FUNCTION_WAITS(collectSelectedPiece, Verified);

    uint32_t cellAcceptsPiece(uint32_t address, ee::Ptr<const CellIndex> cell, uint32_t recordAddress);

    // Whether a cell takes the held piece: the cell free, nothing on the selection's anchor
    // there, nothing standing on the cell's tower (BMX XXX), and the anchor level with the
    // piece's own.
    uint32_t cellAcceptsPiece(uint32_t address, ee::Ptr<const CellIndex> cell, uint32_t recordAddress)
    {
        const ee::Ptr<EditSelection> selection(address);
        if (parkCellFree(selection->park, cell) == 0)
            return 0;
        const uint32_t anchor = selectionCellAt(address, cell);
        if (attachedObject(anchor) != 0)
            return 0;
        const ee::Ptr<EditObjectPart> tower = attachedObject(parkCellAnchor(selection->park, cell));
        if (tower != 0 && zge::compareStrings(editObjectClassName(tower), kNameEditTowerObject) == 0)
        {
            for (uint32_t towerAnchor = firstAnchor(editObjectAnchors(tower)); towerAnchor != 0; towerAnchor = nextAnchor(towerAnchor))
            {
                if (attachedObject(towerAnchor) != 0)
                    return 0;
            }
        }
        const ee::Ptr<zge::Vec4> position(static_cast<uint32_t>(anchorWorldPosition(anchor)));
        const uint32_t parent = editObjectParentAnchor(ee::Ptr<SelectionRecord>(recordAddress)->object);
        const ee::Ptr<zge::Vec4> parentPosition(static_cast<uint32_t>(anchorWorldPosition(parent)));
        const bool level = !(kLevelTolerance < ee::abs(position->y - parentPosition->y));
        return level ? 1u : 0u;
    }
    READABLE_FUNCTION_WAITS(cellAcceptsPiece, Verified);
}
