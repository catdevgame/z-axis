// The park editor's tower pieces (class "EditTowerObject"): the raised ground
// of a park. Four cells make one tower; the first of them (corner 1) holds the
// display they share, and each corner raises its own column of the display's
// vertices to its height. A tower moves only up and down between its limits.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_tower.cpp.

#include "aggressive/level/edit_tower.h"
#include "zge/editor/edit_tower.h"
#include "aggressive/level/edit_records.h"
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
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_messages.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_display.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_tower.cpp.h"

namespace zge { ee::Ptr<CollisionFrame> nodeCollisionFrame(ee::Ptr<Node>); }
namespace zge { ee::Ptr<ee::Ptr<uint8_t>> frameTree(ee::Ptr<CollisionFrame>); }

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kEditObjectClass = sym::g_editObjectName;           // "EditObject"
        constexpr uint32_t kEditTowerObjectClass = sym::g_editTowerObjectName; // "EditTowerObject"
        constexpr uint32_t kEditAnchorClass = sym::g_towerEditAnchorName;      // "EditAnchor"

        // The corner that holds the shared display, and a tower's corner when made.
        constexpr int32_t kDisplayCorner = 1, kNoCorner = -1;
    }

    // Made: the tower's methods, one anchor of its own at its origin (in place of
    // its piece's), no corner yet, and the flags of a tower.
    void constructEditTowerObject(uint32_t object)
    {
        const ee::Ptr<EditObjectPart> part(editObjectPart(object, kEditObjectClass));
        const ee::Ptr<EditTowerObjectPart> tower(editObjectPart(object, kEditTowerObjectClass));
        ee::Local<zge::Vec3> origin;
        setEditObjectMethods(part, sym::g_editTowerObjectMethods);
        freeAnchorList(part->anchors);
        const uint32_t anchors = newEditList();
        const uint32_t anchor = newEditObject(kEditAnchorClass, 0);
        origin->x = ee::Float(0.0f);
        origin->y = ee::Float(0.0f);
        origin->z = ee::Float(0.0f);
        setAnchorPosition(anchor, origin);
        appendAnchor(anchors, anchor);
        setEditObjectAnchors(part, anchors);
        tower->corner = kNoCorner;
        part->flags[kObjectHasHeight] = 1;
        part->flags[kObjectSnaps] = 0;
        part->flags[kObjectRemovable] = 0;
        part->flags[kObjectShowsBox] = 0;
    }
    READABLE_FUNCTION_WAITS(constructEditTowerObject, Verified);
}
