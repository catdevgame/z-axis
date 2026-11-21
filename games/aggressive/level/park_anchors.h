#pragma once

// The park editor's anchors and the base part of its tools (park_anchors.cpp).
// Aggressive Inline's own part of it; what both games have is zge/editor/park_anchors.h.

#include "ee/ee.h"
#include "zge/editor/park_anchors.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "aggressive/level/edit_records.h"
#include "aggressive/level/park_camera.h"

#include <cstdint>

namespace zge::editor
{
    int32_t anchorLocalPosition(uint32_t anchor);
    int32_t loadAnchor(uint32_t anchor, uint32_t stream);
    void saveAnchor(uint32_t anchor, uint32_t stream);
}
