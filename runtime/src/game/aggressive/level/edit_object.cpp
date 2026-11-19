// A park editor object (class "EditObject"): a piece of the park with its
// piece definition, where it stands (its own matrix under the anchor it hangs
// from, and in the world), its anchors that other pieces hang from, its display,
// and its kind's methods (a plain piece's or a tower's): moving, saving and
// loading what is its own, its world matrix, its flags and its display.
// The park's cost (what its pieces add up to, against a limit) is kept here too.
// Aggressive Inline's own part of it; what both games have is zge/editor/edit_object.cpp.

#include "aggressive/level/edit_object.h"
#include "zge/editor/edit_object.h"
#include "aggressive/level/edit_records.h"
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
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_display.h"
#include "aggressive/level/park_messages.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_ui.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/edit_object.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kEditObjectClass = sym::g_editObjectName;           // "EditObject"

        ee::Ptr<EditObjectMethods> methodsOf(ee::Ptr<EditObjectPart> part)
        {
            return ee::Ptr<EditObjectMethods>(static_cast<uint32_t>(part->methods));
        }
    }

    void removeParkCost(ee::Ptr<EditObjectPart> part);

    // The object's next anchor after the one at *index that accepts the piece,
    // round to the first that does; *index becomes its place. 0 if none does.
    uint32_t nextAnchorFor(ee::Ptr<EditObjectPart> part, ee::Ptr<EditObjectPart> piece, ee::Ptr<uint32_t> index)
    {
        uint32_t first = 0;
        uint32_t firstIndex = 0;
        uint32_t place = 0;
        for (uint32_t anchor = firstAnchor(part->anchors); anchor != 0; anchor = nextAnchor(anchor), ++place)
        {
            if (anchorAccepts(anchor, piece) == 0)
                continue;
            if (*index < place)
            {
                *index = place;
                return anchor;
            }
            if (first == 0)
            {
                firstIndex = place;
                first = anchor;
            }
        }
        if (first == 0)
            return 0;
        *index = firstIndex;
        return first;
    }
    READABLE_FUNCTION(nextAnchorFor, Verified);

    ee::Ptr<EditObjectPart> newEditObjectOfDef(ee::Address pieceDef)
    {
        const ee::Ptr<EditObjectPart> object(newEditObject(pieceDefClassName(pieceDef), pieceDef));
        makeEditObjectDisplay(object, 0);
        return object;
    }
    READABLE_FUNCTION_WAITS(newEditObjectOfDef, Verified);

    // Its kind's empty slot 1 (no caller).
    void callEditObjectSpareMethod(ee::Ptr<EditObjectPart> part)
    {
        methodsOf(part)->spare();
    }
    READABLE_FUNCTION(callEditObjectSpareMethod, Draft);

    // Turned to these angles (in its own matrix), its world matrix made again.
    void setEditObjectAngles(uint32_t object, uint32_t angles)
    {
        zge::matrixClearRotation(ee::Ptr<zge::Matrix>(object));
        zge::matrixRotateXYZ(ee::Ptr<zge::Matrix>(object), ee::Ptr<zge::Matrix>(object), ee::Ptr<const zge::EulerAngles>(angles));
        game::tail(updateEditObjectMatrix, ee::Ptr<EditObjectPart>(object));
    }
    READABLE_FUNCTION_WAITS(setEditObjectAngles, Draft);

    void setEditObjectFlag(ee::Ptr<EditObjectPart> part, uint32_t index, int32_t value)
    {
        part->flags[index] = value;
    }
    READABLE_FUNCTION(setEditObjectFlag, Draft);

    void destructEditObject(uint32_t object)
    {
        const ee::Ptr<EditObjectPart> part(editObjectPart(object, kEditObjectClass));
        if (part->display != 0)
            freeShownDisplay(part->display);
        if (part->anchors != 0)
            freeAnchorList(part->anchors);
        game::tail(removeParkCost, part);
    }
    READABLE_FUNCTION_WAITS(destructEditObject, Verified);

    void editObjectEmpty()
    {
    }
    READABLE_FUNCTION(editObjectEmpty, Draft);
}
