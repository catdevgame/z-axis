// What the park editor shows of a piece and where its camera looks: a display
// wraps the node of a world object, a tower's display stretches the shared
// "Editobj_towerdef" model to the tower's height; a view is a saved camera place.
// BMX XXX's own part of it; what both games have is zge/editor/park_display.cpp.

#include "bmx/editor/park_display.h"
#include "zge/editor/park_display.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/text/strings.h"
#include "bmx/editor/conversions.h"
#include "bmx/editor/edit_records.h"
#include "zge/assets/geo.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/scene.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/park_display.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kSetNodeLocalMatrix = sym::setNodeLocalMatrix; // (node, matrix)
        constexpr uint32_t kCopyFlags = 0x000D0007u;          // the geometry copy, with the caller's own
    }

    // A node showing a copy of a world object's geometry. (BMX XXX: the node gets a copy of
    // the object's tags too.)
    uint32_t newWorldObjectInstance(uint32_t hash, uint32_t copyFlags)
    {
        const ee::Ptr<zge::GeoObject> object(zge::findWorldObjectByHash(hash));
        const ee::Ptr<zge::GeoObject> geometry = zge::copyGeoObject(object, copyFlags | kCopyFlags);
        const uint32_t node = zge::newAppInstanceNode(geometry, 1);
        if (node != 0 && object->tags != 0)
        {
            const ee::Ptr<zge::TagTableHeader> tags = zge::copyTagTable(object->tags);
            zge::setNodeTags(ee::Ptr<zge::Node>(node), tags);
            if (tags != 0)
                zge::releaseTagTable(tags);
        }
        zge::releaseGeoObject(geometry);
        return node;
    }
    READABLE_FUNCTION_WAITS(newWorldObjectInstance, Verified);
}
