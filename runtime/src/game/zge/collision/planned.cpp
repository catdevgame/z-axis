// Collision functions not ported yet, registered so PS2X_FNREC can record
// them (game/ee/readable.h READABLE_PLANNED).

#include "ee/readable.h"
#include "sym/zge/collision/planned.cpp.h"

namespace zge
{
    READABLE_PLANNED(sym::forEachCellInBox, "forEachCellInBox");
    READABLE_PLANNED(sym::forEachCellInOrientedBox, "forEachCellInOrientedBox");
    READABLE_PLANNED(sym::boxFaceTree, "boxFaceTree");
    READABLE_PLANNED(sym::boxExtentAlong, "boxExtentAlong");
}
