// A park editor object (class "EditObject"): a piece of the park with its
// piece definition, where it stands (its own matrix under the anchor it hangs
// from, and in the world), its anchors that other pieces hang from, its display,
// and its kind's methods (a plain piece's or a tower's): moving, saving and
// loading what is its own, its world matrix, its flags and its display.
// The park's cost (what its pieces add up to, against a limit) is kept here too.
// BMX XXX's own part of it; what both games have is zge/editor/edit_object.cpp.

#include "bmx/editor/edit_object.h"
#include "zge/editor/edit_object.h"
#include "bmx/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_display.h"
#include "bmx/editor/park_messages.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_ui.h"
#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/edit_object.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditObjectClass = sym::g_editObjectName;           // "EditObject"
    }

    void removeParkCost(ee::Ptr<EditObjectPart> part);

    // Freed: its anchors no longer name it as their owner (BMX XXX; Aggressive Inline leaves
    // the owner), then its display and its anchor list go, and its cost comes off the park.
    void destructEditObject(uint32_t object)
    {
        const ee::Ptr<EditObjectPart> part(editObjectPart(object, kEditObjectClass));
        setAnchorListOwner(part->anchors, 0);
        if (part->display != 0)
            freeShownDisplay(part->display);
        if (part->anchors != 0)
            freeAnchorList(part->anchors);
        game::tail(removeParkCost, part);
    }
    READABLE_FUNCTION_WAITS(destructEditObject, Verified);
}
