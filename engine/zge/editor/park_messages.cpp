// The park editor's messages: a message goes breadth first from one object or
// anchor through what hangs off it (level/park_classes.cpp and park_anchors.cpp
// queue the next ones), each class running its own handler with the message.

#include "zge/editor/park_messages.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/editor/edit_object.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/park_messages.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kReleaseEditObjectDisplay = sym::releaseEditObjectDisplay;
        constexpr uint32_t kSaveEditObject = sym::saveEditObject;
        constexpr uint32_t kUpdateEditObjectMatrix = sym::updateEditObjectMatrix;
        constexpr uint32_t kDestroyEditPiece = sym::destroyEditPiece;

        // What the find message looks for, and where it says what it found.
        struct PieceSearch
        {
            int32_t piece;
            ee::Ptr<EditObjectPart> found;
        };

        constexpr uint32_t kMovedObjectHandler = sym::movedObjectHandler;
        constexpr uint32_t kMovedAnchorHandler = sym::movedAnchorHandler;
        constexpr uint32_t kDeleteObjectHandler = sym::deleteObjectHandler;
        constexpr uint32_t kDeleteAnchorHandler = sym::deleteAnchorHandler;
        constexpr uint32_t kHideObjectHandler = sym::hideObjectHandler;
        constexpr uint32_t kHideAnchorHandler = sym::hideAnchorHandler;
        constexpr uint32_t kFindObjectHandler = sym::findObjectHandler;
        constexpr uint32_t kFindAnchorHandler = sym::findAnchorHandler;

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

    // runMessageObjectHandler: the message's handler for objects, when it has one.
    int32_t runMessageObjectHandler(uint32_t address, uint32_t object)
    {
        const TargetHandler handler = ee::Ptr<EditMessage>(address)->objectHandler;
        return handler ? handler(address, object) : 0;
    }
    READABLE_FUNCTION(runMessageObjectHandler, Verified);

    int32_t runMessageAnchorHandler(uint32_t address, uint32_t anchor)
    {
        const TargetHandler handler = ee::Ptr<EditMessage>(address)->anchorHandler;
        return handler ? handler(address, anchor) : 0;
    }
    READABLE_FUNCTION(runMessageAnchorHandler, Verified);

    uint32_t messageSender(uint32_t message)
    {
        return ee::Ptr<EditMessage>(message)->queue;
    }
    READABLE_FUNCTION(messageSender, Verified);

    void initEditMessageQueue(ee::Address address);

    // broadcastEditMessage: `makeMessage` fills the message; the queue starts with `start`
    // and the handlers add what hangs off each target.
    int32_t broadcastEditMessage(ee::Address start, uint32_t makeMessage, uint32_t argument)
    {
        ee::Local<EditMessageQueue> queue;
        ee::Local<EditMessage> message;
        const ee::FunctionPtr<void(ee::Ptr<EditMessageQueue>, ee::Ptr<EditMessage>)> fillMessage(makeMessage);
        fillMessage(queue, message);
        message->argument = argument;
        initEditMessageQueue(queue);
        queueEditMessageTarget(queue, start);
        uint32_t result = queue->head << 2;
        uint32_t target = queue->target[queue->head];
        if (target != 0)
        {
            for (;;)
            {
                sendEditMessage(target, message);
                result = (queue->head + 1) & (kQueueCapacity - 1);
                queue->head = result;
                target = queue->target[result];
                if (target == 0)
                    break;
            }
        }
        return static_cast<int32_t>(result);
    }
    READABLE_FUNCTION_WAITS(broadcastEditMessage, Verified);

    void initEditMessageQueue(ee::Address address)
    {
        const ee::Ptr<EditMessageQueue> queue(address);
        queue->head = 0;
        queue->tail = 0;
        for (uint32_t i = 0;;)
        {
            queue->target[i] = 0;
            if (!(++i < kQueueCapacity))
                return;
        }
    }
    READABLE_FUNCTION(initEditMessageQueue, Verified);

    void queueEditMessageTarget(ee::Address address, ee::Address target)
    {
        const ee::Ptr<EditMessageQueue> queue(address);
        queue->target[queue->tail] = target;
        queue->tail = (queue->tail + 1) & (kQueueCapacity - 1);
    }
    READABLE_FUNCTION(queueEditMessageTarget, Verified);

    ee::Ptr<zge::Matrix> movedAnchorHandler(uint32_t /*message*/, uint32_t anchor)
    {
        return game::tail(updateAnchorMatrix, anchor);
    }
    READABLE_FUNCTION(movedAnchorHandler, Verified);

    namespace
    {
        namespace tail
        {
        int32_t updateEditObjectMatrix(uint32_t object) { return ee::tailCall<int32_t>(kUpdateEditObjectMatrix, object); }
        }
    }

    int32_t movedObjectHandler(uint32_t /*message*/, uint32_t object)
    {
        return tail::updateEditObjectMatrix(object);
    }
    READABLE_FUNCTION_WAITS(movedObjectHandler, Verified);

    uint32_t makeMovedMessage(uint32_t queue, uint32_t message)
    {
        return makeEditMessage(queue, message, kMovedObjectHandler, kMovedAnchorHandler);
    }
    READABLE_FUNCTION(makeMovedMessage, Verified);

    int32_t deleteAnchorHandler(uint32_t /*message*/, uint32_t anchor)
    {
        return game::tail(deleteAnchor, anchor);
    }
    READABLE_FUNCTION_WAITS(deleteAnchorHandler, Verified);

    namespace
    {
        // destroyEditPiece, and what it leaves in the result register (the handlers return it).
        int32_t destroyEditPieceKeepingResult(ee::Address object) { return ee::tailCall<int32_t>(kDestroyEditPiece, object); }
    }

    int32_t deleteObjectHandler(uint32_t /*message*/, uint32_t object)
    {
        return destroyEditPieceKeepingResult(object);
    }
    READABLE_FUNCTION_WAITS(deleteObjectHandler, Verified);

    uint32_t makeDeleteMessage(uint32_t queue, uint32_t message)
    {
        return makeEditMessage(queue, message, kDeleteObjectHandler, kDeleteAnchorHandler);
    }
    READABLE_FUNCTION(makeDeleteMessage, Verified);

    namespace
    {
        namespace tail
        {
        int32_t saveEditObject(uint32_t editObject, uint32_t parkStream) { return ee::tailCall<int32_t>(kSaveEditObject, editObject, parkStream); }
        }
    }

    namespace
    {
        namespace tail
        {
        int32_t releaseEditObjectDisplay(uint32_t object) { return ee::tailCall<int32_t>(kReleaseEditObjectDisplay, object); }
        }
    }

    int32_t hideObjectHandler(uint32_t /*message*/, uint32_t object)
    {
        return tail::releaseEditObjectDisplay(object);
    }
    READABLE_FUNCTION_WAITS(hideObjectHandler, Verified);

    void hideAnchorHandler()
    {
    }
    READABLE_FUNCTION(hideAnchorHandler, Verified);

    uint32_t makeHideMessage(uint32_t queue, uint32_t message)
    {
        return makeEditMessage(queue, message, kHideObjectHandler, kHideAnchorHandler);
    }
    READABLE_FUNCTION(makeHideMessage, Verified);

    // findObjectHandler: the message's argument is {the def looked for, the object found}.
    int32_t findObjectHandler(uint32_t message, ee::Ptr<EditObjectPart> object)
    {
        const ee::Ptr<PieceSearch> search(ee::Ptr<EditMessage>(message)->argument);
        const int32_t def = editObjectDef(object);
        if (def == search->piece)
            search->found = object;
        return def;
    }
    READABLE_FUNCTION(findObjectHandler, Verified);

    void findAnchorHandler()
    {
    }
    READABLE_FUNCTION(findAnchorHandler, Verified);

    uint32_t makeFindMessage(uint32_t queue, uint32_t message)
    {
        return makeEditMessage(queue, message, kFindObjectHandler, kFindAnchorHandler);
    }
    READABLE_FUNCTION(makeFindMessage, Verified);
}
