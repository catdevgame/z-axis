// The park editor's tools (the base part every tool class has) and anchors. An
// anchor is an edit object a piece can be attached to: one sits on each cell,
// and on a piece each of its "EditAnchor" locators makes one, listing the pieces
// that may sit there. Anchors carry position and angles, work out their matrix
// through an owner's, and pass messages on to what they hold.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_anchors.cpp.

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
#include "aggressive/level/edit_core.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_messages.h"
#include "aggressive/level/park_catalog.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/level/park_anchors.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kAngleSin = sym::angleSin;           // (a game override hooks it: by address)
        constexpr uint32_t kListPush = sym::listPush;           // (list, node) to the front
        constexpr uint32_t kEditObjectDef = sym::editObjectDef;
        constexpr uint32_t kEditObjectMatrix = sym::editObjectMatrix;
        constexpr uint32_t kNewAcceptList = sym::newAcceptList;
        constexpr uint32_t kFreeAcceptList = sym::freeAcceptList;
        constexpr uint32_t kAddAcceptEntry = sym::addAcceptEntry;
        constexpr uint32_t kSetAnchorOwner = sym::setAnchorOwner;
        constexpr uint32_t kRunMessageAnchorHandler = sym::runMessageAnchorHandler;
        constexpr uint32_t kMessageSender = sym::messageSender;
        constexpr uint32_t kBroadcastEditMessage = sym::broadcastEditMessage;
        constexpr uint32_t kQueueEditMessageTarget = sym::queueEditMessageTarget;
        constexpr uint32_t kMovedMessage = sym::makeMovedMessage;
        constexpr uint32_t kCellNamesPage = ee::high(sym::g_editAnchorName);
        constexpr ee::Float kAngleToQuarterTurns = 6.1035156e-05f;
        constexpr ee::Float kQuarterTurn = 16384.0f;

        ee::Ptr<EditAnchorPart> anchorPart(uint32_t anchor)
        {
            return ee::Ptr<EditAnchorPart>(anchor);
        }
    }

    void editToolNoop()
    {
    }
    READABLE_FUNCTION(editToolNoop, Verified);

    int32_t editToolName(uint32_t part)
    {
        return static_cast<int32_t>(part + offsetof(EditToolPart, name));
    }
    READABLE_FUNCTION(editToolName, Draft);

    int32_t setAnchorOwner(uint32_t anchor, ee::Ptr<EditObjectPart> owner);

    namespace
    {
        namespace tail
        {
        int32_t listPush(uint32_t list, uint32_t node) { return ee::tailCall<int32_t>(kListPush, list, node); }
        }
    }

    uint32_t newAcceptList();

    int32_t freeAcceptList(uint32_t list);

    // moveAnchor: no caller reaches it on a route: Draft.
    int32_t moveAnchor(uint32_t anchor, ee::Ptr<zge::Vec4> offset)
    {
        zge::Vec4 &position = anchorPart(anchor)->position;
        position = position + *offset;
        return game::tail(broadcastEditMessage, anchor, kMovedMessage, 0);
    }
    READABLE_FUNCTION_WAITS(moveAnchor, Draft);

    int32_t anchorLocalPosition(uint32_t anchor)
    {
        return static_cast<int32_t>(anchor + offsetof(EditAnchorPart, position));
    }
    READABLE_FUNCTION(anchorLocalPosition, Verified);

    // saveAnchor: into a saved park: a cell's own anchor's turn, in quarter
    // turns, then what it holds.
    void saveAnchor(uint32_t anchor, uint32_t stream)
    {
        ee::Local<uint8_t> bytes;
        const ee::Ptr<EditAnchorPart> part = anchorPart(anchor);
        if (part->copied == 0)
        {
            *bytes = static_cast<uint8_t>(ee::toInt(ee::toFloat(part->angles.y) * kAngleToQuarterTurns));
            parkStreamWrite(stream, bytes, 1);
        }
        const ee::Ptr<EditObjectPart> held = part->attached;
        if (held != 0)
            saveEditObject(held, stream);
    }
    READABLE_FUNCTION_WAITS(saveAnchor, Verified);

    int32_t loadAnchor(uint32_t anchor, uint32_t stream)
    {
        ee::Local<uint8_t> bytes;
        const ee::Ptr<EditAnchorPart> part = anchorPart(anchor);
        if (part->copied == 0)
        {
            parkStreamRead(stream, bytes, 1);
            part->angles.y = static_cast<int16_t>(ee::toInt(ee::toFloat(static_cast<int32_t>(*bytes)) * kQuarterTurn));
        }
        broadcastEditMessage(anchor, kMovedMessage, 0);
        readEditPiece(anchor, stream);
        return broadcastEditMessage(anchor, kMovedMessage, 0);
    }
    READABLE_FUNCTION_WAITS(loadAnchor, Verified);
}
