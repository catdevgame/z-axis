// The park editor's tower pieces (class "EditTowerObject"): the raised ground
// of a park. Four cells make one tower; the first of them (corner 1) holds the
// display they share, and each corner raises its own column of the display's
// vertices to its height. A tower moves only up and down between its limits.
// BMX XXX's own part of it; what both games have is zge/editor/edit_tower.cpp.

#include "bmx/editor/edit_tower.h"
#include "zge/editor/edit_tower.h"
#include "bmx/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/scene/node.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/edit_object.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_messages.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_display.h"
#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/edit_tower.cpp.h"

namespace zge { ee::Ptr<CollisionFrame> nodeCollisionFrame(ee::Ptr<Node>); }
namespace zge { ee::Ptr<ee::Ptr<uint8_t>> frameTree(ee::Ptr<CollisionFrame>); }

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditObjectClass = sym::g_editObjectName;           // "EditObject"
        constexpr uint32_t kEditTowerObjectClass = sym::g_editTowerObjectName; // "EditTowerObject"
        constexpr int32_t kNoCorner = -1;
    }

    // Made: a tower's methods, no corner yet, and its flags: its height counts, and it neither
    // goes with the remove-flagged message, shows a box nor snaps. (Aggressive Inline also gives
    // it a list with one anchor at its origin here.)
    void constructEditTowerObject(uint32_t object)
    {
        const ee::Ptr<EditObjectPart> part(editObjectPart(object, kEditObjectClass));
        const ee::Ptr<EditTowerObjectPart> tower(editObjectPart(object, kEditTowerObjectClass));
        setEditObjectMethods(part, sym::g_editTowerObjectMethods);
        tower->corner = kNoCorner;
        part->flags[kObjectHasHeight] = 1;
        part->flags[kObjectSnaps] = 0;
        part->flags[kObjectRemovable] = 0;
        part->flags[kObjectShowsBox] = 0;
    }
    READABLE_FUNCTION_WAITS(constructEditTowerObject, Verified);
}
