// The park editor's park and its tool set's buttons: a park is a grid of
// cells read from a theme's map file, each with an anchor pieces sit on;
// breakables are the pieces a test run can knock down, kept in a pool of their
// own so that they can be put back. A saved park is a byte stream: a word of
// header, then per cell a mark and what its anchor or tower holds, an end mark.
// BMX XXX's own part of it; what both games have is zge/editor/park_cells.cpp.

#include "bmx/editor/park_cells.h"
#include "zge/editor/park_cells.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "bmx/editor/breakables.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/text/cstring.h"
#include "zge/text/token_reader.h"
#include "zge/scene/node.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_display.h"
#include "bmx/editor/edit_tools.h"
#include "bmx/editor/park_camera.h"
#include "bmx/editor/park_ui.h"
#include "bmx/editor/edit_core.h"
#include "bmx/editor/edit_object.h"
#include "bmx/editor/edit_tower.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_messages.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/editor/park_cells.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kSetEditObjectDisplay = sym::setEditObjectDisplay; // (object, display)
        constexpr uint32_t kForEachParkCell = sym::forEachParkCell;    // (park, visit, context)
        constexpr uint32_t kSaveEditCamera = sym::saveEditCamera;

        // What findParkPiece asks of the cells: the record of the piece, and
        // the anchor that answers (the find message's handler fills it).
        struct PieceSearch
        {
            uint32_t piece;
            ee::Ptr<EditObjectPart> found;
        };
    }

    namespace
    {
        namespace tail
        {
        }
    }

    uint32_t cellAnchor(uint32_t cell);
    void forEachParkCell(uint32_t address, uint32_t visit, ee::Address context);

    // A cell's dynamics shown again: each of the catalog's dynamics standing on an anchor of
    // the cell's piece gets its display made anew.
    void remakeCellDynamics(uint32_t /*park*/, uint32_t cell)
    {
        constexpr uint32_t kDynamicsCategory = 12;
        const ee::Ptr<EditObjectPart> piece = anchorAttachedObject(cellAnchor(cell));
        if (piece == 0)
            return;
        for (uint32_t anchor = firstAnchor(editObjectAnchors(piece)); anchor != 0; anchor = nextAnchor(anchor))
        {
            const ee::Ptr<EditObjectPart> standing = anchorAttachedObject(anchor);
            if (standing != 0 && pieceDefCategory(editObjectDef(standing)) == kDynamicsCategory)
                makeEditObjectDisplay(standing, 0);
        }
    }
    READABLE_FUNCTION_WAITS(remakeCellDynamics, Verified);

    // The park shown for the editor again (BMX XXX; where Aggressive Inline puts its knocked-down
    // breakables back): the park keeps the catalog, and every cell's dynamics get their displays anew.
    void remakeParkDynamics(uint32_t park, uint32_t catalog)
    {
        ee::Ptr<EditPark>(park)->catalog = catalog;
        game::tail(forEachParkCell, park, sym::remakeCellDynamics, catalog);
    }
    READABLE_FUNCTION_WAITS(remakeParkDynamics, Verified);

    void forEachParkCell(uint32_t address, uint32_t visit, ee::Address context);

    void deleteParkPieces(uint32_t address);

    // The park emptied: every piece deleted, and its cells built again from the catalog it was built with.
    // (Aggressive Inline clears cell by cell.)
    void clearParkPieces(uint32_t park)
    {
        deleteParkPieces(park);
        game::tail(buildParkCells, park, ee::Ptr<EditPark>(park)->catalog);
    }
    READABLE_FUNCTION_WAITS(clearParkPieces, Verified);

    uint32_t cellAnchor(uint32_t cell);

    namespace
    {
        namespace tail
        {
        int32_t setEditObjectDisplay(ee::Ptr<EditObjectPart> object, uint32_t display) { return ee::tailCall<int32_t>(kSetEditObjectDisplay, object, display); }
        }
    }
}
