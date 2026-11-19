#pragma once

// The park editor's camera (park_camera.cpp): it orbits a look-at point inside
// the park and slides from one view to another.
// BMX XXX's own part of it; what both games have is zge/editor/park_camera.h.

#include "bmx/editor/edit_records.h"
#include "zge/editor/park_camera.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge::editor
{
    void editCameraUseUiCamera(ee::Ptr<EditCamera> camera);
}

