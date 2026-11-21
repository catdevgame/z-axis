// The park editor's messages: a message goes breadth first from one object or
// anchor through what hangs off it (level/park_classes.cpp and park_anchors.cpp
// queue the next ones), each class running its own handler with the message.
// BMX XXX's own part of it; what both games have is zge/editor/park_messages.cpp.

#include "bmx/editor/park_messages.h"
#include "zge/editor/park_messages.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "bmx/editor/edit_object.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/park_messages.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kReleaseEditObjectDisplay = sym::releaseEditObjectDisplay;
        constexpr uint32_t kUpdateEditObjectMatrix = sym::updateEditObjectMatrix;
        constexpr uint32_t kDestroyEditPiece = sym::destroyEditPiece;

        // What the find message looks for, and where it says what it found.
        struct PieceSearch
        {
            int32_t piece;
            ee::Ptr<EditObjectPart> found;
        };
    }
}
