// The park editor's tools (the base part every tool class has) and anchors. An
// anchor is an edit object a piece can be attached to: one sits on each cell,
// and on a piece each of its "EditAnchor" locators makes one, listing the pieces
// that may sit there. Anchors carry position and angles, work out their matrix
// through an owner's, and pass messages on to what they hold.
// BMX XXX's own part of it; what both games have is zge/editor/park_anchors.cpp.

#include "ee/abi.h"
#include "zge/editor/park_anchors.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/angle.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/render/draw.h"
#include "zge/render/primitives.h"
#include "bmx/editor/edit_core.h"
#include "bmx/editor/edit_object.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/park_messages.h"
#include "bmx/editor/park_catalog.h"
#include "bmx/editor/park_cells.h"
#include "bmx/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/editor/park_anchors.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kListPush = sym::listPush;           // (list, node) to the front
        constexpr uint32_t kMovedMessage = sym::makeMovedMessage;
        constexpr ee::Float kAngleToQuarterTurns = 6.1035156e-05f;
        constexpr ee::Float kQuarterTurn = 16384.0f;

        ee::Ptr<EditAnchorPart> anchorPart(uint32_t anchor)
        {
            return ee::Ptr<EditAnchorPart>(anchor);
        }
    }

    // Saved: its heading in quarter turns, one byte, then the piece on it. (BMX XXX saves the
    // heading of every anchor; Aggressive Inline skips an anchor copied from a definition.)
    void saveAnchor(uint32_t anchor, uint32_t stream)
    {
        ee::Local<uint8_t> bytes;
        const ee::Ptr<EditAnchorPart> part = anchorPart(anchor);
        *bytes = static_cast<uint8_t>(ee::toInt(ee::toFloat(part->angles.y) * kAngleToQuarterTurns));
        parkStreamWrite(stream, bytes, 1);
        const ee::Ptr<EditObjectPart> held = part->attached;
        if (held != 0)
            saveEditObject(held, stream);
    }
    READABLE_FUNCTION_WAITS(saveAnchor, Verified);

    namespace
    {
        // Scaffolding (address-call): readEditPiece, and what it leaves in the result register: loadAnchor
        // hands it on, and loadParkCell returns it to the walk over the cells.
        int32_t readEditPieceKeepingResult(uint32_t anchor, uint32_t stream) { return ee::game<int32_t>(sym::readEditPiece, anchor, stream); }
    }

    // Loaded: its heading, every part told it moved, then the piece on it.
    int32_t loadAnchor(uint32_t anchor, uint32_t stream)
    {
        ee::Local<uint8_t> bytes;
        const ee::Ptr<EditAnchorPart> part = anchorPart(anchor);
        parkStreamRead(stream, bytes, 1);
        part->angles.y = static_cast<int16_t>(ee::toInt(ee::toFloat(static_cast<int32_t>(*bytes)) * kQuarterTurn));
        broadcastEditMessage(anchor, kMovedMessage, 0);
        return readEditPieceKeepingResult(anchor, stream);
    }
    READABLE_FUNCTION_WAITS(loadAnchor, Draft);

    // An anchor's name, hashed: its locator's (findPieceAnchors gives it).
    void setAnchorNameHash(uint32_t anchor, uint32_t nameHash)
    {
        anchorPart(anchor)->nameHash = nameHash;
    }
    READABLE_FUNCTION(setAnchorNameHash, Verified);

    uint32_t anchorNameHash(uint32_t anchor)
    {
        return anchorPart(anchor)->nameHash;
    }
    READABLE_FUNCTION(anchorNameHash, Verified);
}
