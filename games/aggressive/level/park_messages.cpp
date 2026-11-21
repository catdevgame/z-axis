// The park editor's messages: a message goes breadth first from one object or
// anchor through what hangs off it (level/park_classes.cpp and park_anchors.cpp
// queue the next ones), each class running its own handler with the message.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_messages.cpp.

#include "aggressive/level/park_messages.h"
#include "zge/editor/park_messages.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "aggressive/level/edit_object.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/park_classes.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/park_messages.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kEditObjectDef = sym::editObjectDef;
        constexpr uint32_t kEditObjectFlag = sym::editObjectFlag;
        constexpr uint32_t kReleaseEditObjectDisplay = sym::releaseEditObjectDisplay;
        constexpr uint32_t kSaveEditObject = sym::saveEditObject;
        constexpr uint32_t kLoadEditObject = sym::loadEditObject;
        constexpr uint32_t kUpdateEditObjectMatrix = sym::updateEditObjectMatrix;
        constexpr uint32_t kDestroyEditPiece = sym::destroyEditPiece;
        constexpr uint32_t kInitEditMessageQueue = sym::initEditMessageQueue;
        constexpr uint32_t kQueueEditMessageTarget = sym::queueEditMessageTarget;
        constexpr uint32_t kRemovedFlag = 1;                  // editObjectFlag's: what the remove-flagged message removes
        constexpr uint32_t kSaveObjectHandler = sym::saveObjectHandler;
        constexpr uint32_t kSaveAnchorHandler = sym::saveAnchorHandler;
        constexpr uint32_t kLoadObjectHandler = sym::loadObjectHandler;
        constexpr uint32_t kLoadAnchorHandler = sym::loadAnchorHandler;
        constexpr uint32_t kRemoveFlaggedObjectHandler = sym::removeFlaggedObjectHandler;
        constexpr uint32_t kRemoveFlaggedAnchorHandler = sym::removeFlaggedAnchorHandler;

        // make*Message's common shape; the value is the object handler.
        uint32_t makeEditMessage(uint32_t queue, uint32_t address, uint32_t objectHandler, uint32_t anchorHandler)
        {
            const ee::Ptr<EditMessage> message(address);
            message->queue = queue;
            message->objectHandler = objectHandler;
            message->anchorHandler = anchorHandler;
            return objectHandler;
        }
    }

    void initEditMessageQueue(ee::Address address);

    namespace
    {
        namespace tail
        {
        int32_t updateEditObjectMatrix(uint32_t object) { return ee::tailCall<int32_t>(kUpdateEditObjectMatrix, object); }
        }
    }

    namespace
    {
        // destroyEditPiece, and what it leaves in the result register (the handlers return it).
        int32_t destroyEditPieceKeepingResult(ee::Address object) { return ee::tailCall<int32_t>(kDestroyEditPiece, object); }
    }

    void saveAnchorHandler(uint32_t message, uint32_t anchor)
    {
        game::tail(saveAnchor, anchor, ee::Ptr<EditMessage>(message)->argument);
    }
    READABLE_FUNCTION_WAITS(saveAnchorHandler, Tested);

    namespace
    {
        namespace tail
        {
        int32_t saveEditObject(uint32_t editObject, uint32_t parkStream) { return ee::tailCall<int32_t>(kSaveEditObject, editObject, parkStream); }
        }
    }

    int32_t saveObjectHandler(uint32_t message, uint32_t object)
    {
        return tail::saveEditObject(object, ee::Ptr<EditMessage>(message)->argument);
    }
    READABLE_FUNCTION_WAITS(saveObjectHandler, Tested);

    uint32_t makeSaveMessage(uint32_t queue, uint32_t message)
    {
        return makeEditMessage(queue, message, kSaveObjectHandler, kSaveAnchorHandler);
    }
    READABLE_FUNCTION(makeSaveMessage, Verified);

    int32_t loadAnchorHandler(uint32_t message, uint32_t anchor)
    {
        return game::tail(loadAnchor, anchor, ee::Ptr<EditMessage>(message)->argument);
    }
    READABLE_FUNCTION_WAITS(loadAnchorHandler, Draft);

    int32_t loadObjectHandler(uint32_t message, ee::Ptr<EditObjectPart> object)
    {
        return game::tail(loadEditObject, object, ee::Ptr<EditMessage>(message)->argument);
    }
    READABLE_FUNCTION_WAITS(loadObjectHandler, Tested);

    uint32_t makeLoadMessage(uint32_t queue, uint32_t message)
    {
        return makeEditMessage(queue, message, kLoadObjectHandler, kLoadAnchorHandler);
    }
    READABLE_FUNCTION(makeLoadMessage, Verified);

    int32_t removeFlaggedObjectHandler(uint32_t /*message*/, ee::Ptr<EditObjectPart> object)
    {
        const int32_t flagged = editObjectFlag(object, kRemovedFlag);
        if (flagged != 0)
            return destroyEditPieceKeepingResult(object);
        return flagged;
    }
    READABLE_FUNCTION_WAITS(removeFlaggedObjectHandler, Verified);

    // removeFlaggedAnchorHandler: an anchor goes with a flagged or missing owner; one whose
    // owner stays only lets go of a flagged object on it.
    ee::Address removeFlaggedAnchorHandler(uint32_t /*message*/, uint32_t anchor)
    {
        const ee::Ptr<EditObjectPart> owner = anchorOwner(anchor);
        if (!owner)
            return game::tail(deleteAnchor, anchor);
        if (editObjectFlag(owner, kRemovedFlag) != 0)
            return game::tail(deleteAnchor, anchor);
        const ee::Ptr<EditObjectPart> attached = anchorAttachedObject(anchor);
        if (!attached)
            return 0;
        if (editObjectFlag(attached, kRemovedFlag) != 0)
            return game::tail(detachFromAnchor, anchor);
        return 0;
    }
    READABLE_FUNCTION_WAITS(removeFlaggedAnchorHandler, Verified);

    uint32_t makeRemoveFlaggedMessage(uint32_t queue, uint32_t message)
    {
        return makeEditMessage(queue, message, kRemoveFlaggedObjectHandler, kRemoveFlaggedAnchorHandler);
    }
    READABLE_FUNCTION(makeRemoveFlaggedMessage, Verified);

    namespace
    {
        namespace tail
        {
        int32_t releaseEditObjectDisplay(uint32_t object) { return ee::tailCall<int32_t>(kReleaseEditObjectDisplay, object); }
        }
    }
}
