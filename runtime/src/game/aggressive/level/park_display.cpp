// What the park editor shows of a piece and where its camera looks: a display
// wraps the node of a world object, a tower's display stretches the shared
// "Editobj_towerdef" model to the tower's height; a view is a saved camera place.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_display.cpp.

#include "aggressive/level/park_display.h"
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
#include "aggressive/conversions.h"
#include "aggressive/level/edit_records.h"
#include "zge/assets/geo.h"
#include "zge/scene/scene.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/park_display.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kReleaseGeoObject = sym::releaseGeoObject;
        constexpr uint32_t kCopyGeoObject = sym::copyGeoObject;
        constexpr uint32_t kSetNodeLocalMatrix = sym::setNodeLocalMatrix; // (node, matrix)
        constexpr uint32_t kNewWorldObjectInstance = sym::newWorldObjectInstance;
        constexpr uint32_t kCopyFlags = 0x000D0007u;          // the geometry copy, with the caller's own
    }

    namespace
    {
        int32_t setDisplayNodeMatrix(uint32_t node, ee::Address matrix) { return ee::game<int32_t>(kSetNodeLocalMatrix, node, matrix); }
    }

    int32_t setDisplayPosition(uint32_t display, uint32_t position)
    {
        ee::Local<zge::Matrix> matrix;
        zge::matrixIdentity(matrix);
        matrix->translation() = *ee::Ptr<zge::Vec4>(position);
        return setDisplayNodeMatrix(ee::Ptr<EditDisplay>(display)->node, matrix);
    }
    READABLE_FUNCTION_WAITS(setDisplayPosition, Verified);

    // newWorldObjectInstance: a shown instance of the world object of that hash; the node
    // keeps its own reference to the copied geometry.
    uint32_t newWorldObjectInstance(uint32_t hash, uint32_t copyFlags)
    {
        const ee::Ptr<zge::GeoObject> object(zge::findWorldObjectByHash(hash));
        const ee::Ptr<zge::GeoObject> geometry = zge::copyGeoObject(object, copyFlags | kCopyFlags);
        const uint32_t node = zge::newAppInstanceNode(geometry, 1);
        zge::releaseGeoObject(geometry);
        return node;
    }
    READABLE_FUNCTION_WAITS(newWorldObjectInstance, Verified);

    int32_t wrapEditDisplay(uint32_t node)
    {
        const uint32_t display = editAllocate(static_cast<uint32_t>(sizeof(EditDisplay)));
        ee::Ptr<EditDisplay>(display)->node = node;
        return static_cast<int32_t>(display);
    }
    READABLE_FUNCTION_WAITS(wrapEditDisplay, Draft);

    void editDisplayBox(uint32_t display, ee::Ptr<zge::Box> out)
    {
        *out = *zge::nodeBox(ee::Ptr<zge::Node>(ee::Ptr<EditDisplay>(display)->node));
    }
    READABLE_FUNCTION(editDisplayBox, Verified);

    int32_t setEditViewPosition(uint32_t view, uint32_t position)
    {
        ee::Ptr<EditView>(view)->position = *ee::Ptr<zge::Vec4>(position);
        ee::Ptr<EditView>(view)->set = 1;
        return 1;
    }
    READABLE_FUNCTION(setEditViewPosition, Verified);

    // (no caller in the game: nothing reads a result)
    void setEditViewAngles(ee::Ptr<EditView> view, ee::Ptr<const zge::EulerAngles> angles)
    {
        view->angles = *angles;
        view->set = 1;
    }
    READABLE_FUNCTION(setEditViewAngles, Draft);

    void editViewPosition(uint32_t view, uint32_t positionOut)
    {
        *ee::Ptr<zge::Vec4>(positionOut) = ee::Ptr<EditView>(view)->position;
    }
    READABLE_FUNCTION(editViewPosition, Draft);

    // (no caller in the game: nothing reads a result)
    void editViewAngles(ee::Ptr<const EditView> view, ee::Ptr<zge::EulerAngles> anglesOut)
    {
        *anglesOut = view->angles;
    }
    READABLE_FUNCTION(editViewAngles, Draft);

    uint32_t editViewIsSet(uint32_t view)
    {
        return ee::Ptr<EditView>(view)->set;
    }
    READABLE_FUNCTION(editViewIsSet, Verified);
}
