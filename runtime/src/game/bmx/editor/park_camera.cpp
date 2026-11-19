// The park editor's camera: it orbits a look-at point (a selection's, when it
// has a target) at a heading, a tilt from the vertical and a distance, kept
// inside the park's corners, and slides from one view to another.
// BMX XXX's own part of it; what both games have is zge/editor/park_camera.cpp.

#include "ee/abi.h"
#include "zge/editor/park_camera.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "bmx/editor/edit_records.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "bmx/editor/park_camera.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/editor/park_camera.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kLookFromAt = sym::lookFromAt;         // (node, eye *, look-at *, 0)

        inline ee::Global<uint32_t, sym::g_editCameraObject> g_uiCamera;
    }

    // The edit camera drives the UI's scene camera as it is now (BMX XXX: the game calls this
    // through the edit core when its camera object changed).
    void editCameraUseUiCamera(ee::Ptr<EditCamera> camera)
    {
        camera->camera = *g_uiCamera;
    }
    READABLE_FUNCTION(editCameraUseUiCamera, Verified);
}
