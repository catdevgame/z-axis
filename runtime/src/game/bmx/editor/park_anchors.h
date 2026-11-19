#pragma once

// The park editor's anchors and the base part of its tools (park_anchors.cpp).
// BMX XXX's own part of it; what both games have is zge/editor/park_anchors.h.

#include "ee/ee.h"
#include "zge/editor/park_anchors.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "bmx/editor/edit_records.h"
#include "bmx/editor/park_camera.h"

#include <cstdint>

namespace zge::editor
{
    uint32_t anchorNameHash(uint32_t anchor);
    int32_t loadAnchor(uint32_t anchor, uint32_t stream);
    void saveAnchor(uint32_t anchor, uint32_t stream);
    void setAnchorNameHash(uint32_t anchor, uint32_t nameHash);
}
