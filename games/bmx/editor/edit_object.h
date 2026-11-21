#pragma once

// A park editor object's own part (class "EditObject", edit_object.cpp): its
// piece, matrices, anchors, display and flags, and the park's cost.
// BMX XXX's own part of it; what both games have is zge/editor/edit_object.h.

#include "bmx/editor/edit_records.h"
#include "zge/editor/edit_object.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/box.h"
#include "zge/math/types.h"

#include <cstdint>

