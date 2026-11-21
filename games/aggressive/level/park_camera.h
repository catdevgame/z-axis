#pragma once

// The park editor's camera (park_camera.cpp): it orbits a look-at point inside
// the park and slides from one view to another.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_camera.h.

#include "aggressive/level/edit_records.h"
#include "zge/editor/park_camera.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge::editor
{
    void editCameraView(ee::Ptr<const EditCamera> camera, ee::Ptr<EditCameraView> view);
    uint64_t startEditCameraTransition(ee::Ptr<EditCamera> camera, ee::Ptr<const EditCameraView> fromView, ee::Ptr<const EditCameraView> toView, ee::Float seconds);
}

