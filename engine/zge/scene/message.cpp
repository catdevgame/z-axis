// The engine's messages (zge/scene/message.h): delivery to an object's
// handlers, the send (at once or queued), and the frame's update message to
// every object on the engine's list.
//

#include "message.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/container/tree.h"
#include "zge/text/cstring.h"
#include "sym/zge/scene/message.cpp.h"

namespace zge
{
    namespace
    {
        // The send is hooked by game overrides (the restart's and the estate's
        // ACTION watch): it is called by its address, through them.
        constexpr uint32_t kSendUpdateMessage = sym::sendUpdateMessage;
    }

    // deliverMessage: the message to its target's handlers for its type: the
    // subscription's handler, or its list's in order until one takes it.
    // The handler's result (0 when there is none or no one took it, 1 when
    // one of the list's did).
    int32_t deliverMessage(ee::Ptr<EngineMessage> message)
    {
        const ee::Ptr<MessageBox> box = ee::Ptr<MessageTarget>(message->target)->messages;
        if (!box)
            return 0;
        // ZGE2 looks the type up from a copy of it on its frame; ZGE3 hands the message's word itself.
        ee::Local<uint32_t> key;
        *key = message->type;
        const ee::Ptr<SearchTreeNode> found = game::readable::kIsDm2 ? searchTreeFind(&box->subscriptions, key)
                                                                      : searchTreeFind(&box->subscriptions, &message->type);
        if (!found)
            return 0;
        const Subscription &subscription = searchTreeValue<Subscription>(found);
        if (subscription.handler)
            return subscription.handler(message);
        // The next link is read after the call: a handler may leave the list.
        for (ee::Ptr<HandlerLink> link = recordOfNode<HandlerLink>(subscription.handlers.head); link;
             link = recordOfNode<HandlerLink>(link->link.next))
        {
            if (game::readable::kIsBmx && !link->handler) // (ZGE4 skips a link without a handler)
                continue;
            if (link->handler(message) == 1)
                return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(deliverMessage, Verified);

    // sendEngineMessage: a message delivered at once (the delivery's result),
    // or a copy queued for its target and the target put in the set of
    // objects with messages waiting (1).
    int32_t sendEngineMessage(ee::Ptr<EngineMessage> message)
    {
        if (message->flags & kMessageNow)
            return deliverMessage(message);
        const uint32_t copy = poolAlloc(g_messages->queuedMessagePool);
        memoryCopy(copy, message, sizeof(EngineMessage));
        listPush(ee::Ptr<MessageTarget>(message->target)->messages, copy);
        const uint32_t waiting = poolAlloc(g_messages->waitingObjectPool);
        searchTreeValue<uint32_t>(waiting) = message->target;
        // Already waiting: the record goes back.
        if (!searchTreeInsert(&g_messages->waitingObjects, ee::Ptr<SearchTreeNode>(waiting)))
            poolFree(g_messages->waitingObjectPool, waiting);
        return 1;
    }
    READABLE_FUNCTION_WAITS(sendEngineMessage, Verified);

    // sendUpdateMessage: forEachObject's visit for updateObjects: the update
    // message, at once, with the caller's data. Goes on to the next object (1).
    int32_t sendUpdateMessage(uint32_t object, uint32_t data)
    {
        ee::Local<EngineMessage> message;
        message->data = data;
        message->type = kUpdateMessage;
        message->target = object;
        message->flags = kMessageNow;
        message->sender = 0;
        sendEngineMessage(message);
        return 1;
    }
    READABLE_FUNCTION_WAITS(sendUpdateMessage, Verified);

    // forEachObject: visit(object, data) for each object on the engine's list,
    // in order, while it answers 1.
    void forEachObject(uint32_t visitFunction, ee::Address data)
    {
        const ee::FunctionPtr<int32_t(uint32_t object, uint32_t data)> visit(visitFunction);
        ee::Ptr<ObjectListNode> node = recordOfNode<ObjectListNode>(g_objectList->head);
        while (node && visit(node->object, data) == 1)
            node = recordOfNode<ObjectListNode>(node->link.next);
    }
    READABLE_FUNCTION_WAITS(forEachObject, Verified);

    // updateObjects: the update message to every object on the list, its data
    // the address of `step` (on the stack).
    void updateObjects(ee::Float step)
    {
        ee::Local<ee::Float> data;
        *data = step;
        forEachObject(kSendUpdateMessage, data);
    }

    // updateEachObject: ZGE2's updateObjects walks the list itself and sends
    // each object the update message there (no forEachObject, no visit). The
    // step the message's data points at sits past the message with a word
    // between (readable_locals_dm2.tsv), as DM2 keeps it.
    void updateEachObject(ee::Float step)
    {
        ee::Local<EngineMessage> message;
        ee::Local<ee::Float> stepSlot;
        *stepSlot = step;
        for (ee::Ptr<ObjectListNode> node = recordOfNode<ObjectListNode>(g_objectList->head); node;
             node = recordOfNode<ObjectListNode>(node->link.next))
        {
            message->data = stepSlot.address();
            message->type = kUpdateMessage;
            message->target = node->object;
            message->flags = kMessageNow;
            message->sender = 0;
            sendEngineMessage(message);
        }
    }
    READABLE_FUNCTION_WAITS_OWN(updateObjects, Verified, dm2, updateEachObject);

    // An object's subscriptions: the message boxes, the subscriptions tree's
    // nodes and the links of a type's handler list, each from its pool.
    namespace
    {
        struct SubscriptionNode
        {
            SearchTreeNode node;       // +0x00
            Subscription subscription; // +0x08 the tree's value
        };
        static_assert(offsetof(SubscriptionNode, subscription) == kSearchTreeValue && sizeof(SubscriptionNode) == 0x1C);

        // A subscription seen with its handler list typed: the links' next
        // pointers lead to the next link (each link starts with its ListNode).
        struct HandlerChainLink
        {
            ee::Ptr<HandlerChainLink> next; // +0x00
            ee::Ptr<HandlerChainLink> prev; // +0x04
            MessageHandler handler;         // +0x08
        };
        struct HandlerChain
        {
            ee::Ptr<HandlerChainLink> head; // +0x00
            ee::Ptr<HandlerChainLink> tail; // +0x04
            int32_t count;                  // +0x08
        };
        struct SubscriptionChain
        {
            uint32_t type;
            MessageHandler handler;
            HandlerChain handlers;
        };
        static_assert(offsetof(HandlerChainLink, handler) == offsetof(HandlerLink, handler) && sizeof(HandlerChain) == sizeof(List) &&
                      offsetof(SubscriptionChain, handlers) == offsetof(Subscription, handlers));

        // A subscription's handler word holding what unsubscribe's bug puts
        // there: the address of a handler, not the handler.
        struct SubscriptionHandlerWord
        {
            uint32_t type;
            ee::Ptr<MessageHandler> handler;
        };
        static_assert(offsetof(SubscriptionHandlerWord, handler) == offsetof(Subscription, handler));

        // The send by its address (game overrides hook it).
        uint32_t insertSubscription(ee::Ptr<SearchTree> tree, ee::Ptr<SubscriptionNode> node)
        {
            return ee::game<uint32_t>(kSearchTreeInsert, tree, node);
        }
        ee::Ptr<SubscriptionNode> removeSubscription(ee::Ptr<SearchTree> tree, ee::Address type)
        {
            return treeRecordOf<SubscriptionNode>(searchTreeRemove(tree, type));
        }
    }

    // freeSubscriberRecord: the object's message box given back (it has no
    // subscriptions left).
    void freeSubscriberRecord(ee::Ptr<MessageTarget> object)
    {
        if (!object->messages)
            return;
        if constexpr (game::readable::kIsBmx) // (ZGE4's subscription made with the box)
            unsubscribe(object, kHandlersUnsubscribedMessage, sym::bmx::dropUnsubscribedHandlers);
        poolFree(g_messages->messageBoxPool, object->messages);
        object->messages = ee::Ptr<MessageBox>();
    }
    READABLE_FUNCTION(freeSubscriberRecord, Verified);

    // objectSubscriptions: the object's message box, made the first time: an
    // empty queue and an empty tree of subscriptions sorted by type.
    void objectSubscriptions(ee::Ptr<MessageTarget> object)
    {
        if (object->messages)
            return;
        object->messages = ee::Ptr<MessageBox>(poolAlloc(g_messages->messageBoxPool));
        initList(&object->messages->queue);
        if constexpr (game::readable::kIsBmx) // ZGE4's box is subscribed to its own clean-up (kHandlersUnsubscribedMessage)
        {
            initSearchTree(&object->messages->subscriptions, sym::lessWord);
            game::tail(subscribe, object, kHandlersUnsubscribedMessage, sym::bmx::dropUnsubscribedHandlers);
            return;
        }
        game::tail(initSearchTree, &object->messages->subscriptions, sym::lessWord);
    }
    READABLE_FUNCTION_WAITS(objectSubscriptions, Verified);

    // subscribe: handler to get the object's messages of this type. A type's
    // first handler is the subscription's own; a second turns it into a list
    // (the first moved into a link). 1, or 0 when the handler has it already.
    uint32_t subscribe(ee::Address objectAddress, uint32_t type, MessageHandler handler)
    {
        const ee::Ptr<MessageTarget> object(objectAddress); // any object, as the messages see it
        ee::Local<uint32_t> key;
        *key = type;
        if (!object->messages)
            objectSubscriptions(object);
        const ee::Ptr<SearchTreeNode> found = searchTreeFind(&object->messages->subscriptions, key);
        if (!found)
        {
            const ee::Ptr<SubscriptionNode> node(poolAlloc(g_messages->subscriptionPool));
            node->subscription.handler = handler;
            node->subscription.type = *key;
            initList(&node->subscription.handlers);
            insertSubscription(&object->messages->subscriptions, node);
            return 1;
        }
        SubscriptionChain &subscription = searchTreeValue<SubscriptionChain>(found);
        if (!subscription.handlers.head)
        {
            if (handler == subscription.handler)
                return 0;
            const ee::Ptr<HandlerChainLink> first(poolAlloc(g_messages->handlerLinkPool));
            first->handler = subscription.handler;
            subscription.handler = MessageHandler();
            listPush(&subscription.handlers, first);
        }
        else
        {
            for (ee::Ptr<HandlerChainLink> link = subscription.handlers.head; link; link = link->next)
                if (link->handler == handler)
                    return 0;
        }
        const ee::Ptr<HandlerChainLink> added(poolAlloc(g_messages->handlerLinkPool));
        added->handler = handler;
        listPush(&subscription.handlers, added);
        return 1;
    }
    READABLE_FUNCTION_WAITS(subscribe, Verified);

    // findSubscription (no caller in this build): whether the object has a
    // subscription to the type.
    uint32_t findSubscription(ee::Ptr<MessageTarget> object, uint32_t type)
    {
        ee::Local<uint32_t> key;
        *key = type;
        if (!object->messages)
            return 0;
        return searchTreeFind(&object->messages->subscriptions, key) ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(findSubscription, Draft);

    // unsubscribe: handler no longer gets the object's messages of this type.
    // 1, or 0 when it did not. The last subscription gone, the message box
    // goes too. When a list is left with one handler it goes back to the
    // subscription's own, and there the original's bug is kept: what it
    // stores is the address of that handler's word in its freed link, not
    // the handler (a message of the type then calls into the link pool).
    uint32_t unsubscribe(ee::Address objectAddress, uint32_t type, MessageHandler handler)
    {
        const ee::Ptr<MessageTarget> object(objectAddress); // any object, as the messages see it
        ee::Local<uint32_t> key;
        *key = type;
        if (!object->messages)
            return 0;
        const ee::Ptr<SearchTreeNode> found = searchTreeFind(&object->messages->subscriptions, key);
        if (!found)
            return 0;
        SubscriptionChain &subscription = searchTreeValue<SubscriptionChain>(found);
        if (subscription.handler)
        {
            if (subscription.handler != handler)
                return 0;
            poolFree(g_messages->subscriptionPool, removeSubscription(&object->messages->subscriptions, key));
            if (object->messages->subscriptions.count == 0)
                freeSubscriberRecord(object);
            return 1;
        }
        ee::Ptr<HandlerChainLink> link = subscription.handlers.head;
        while (link && link->handler != handler)
            link = link->next;
        if (!link)
            return 0;
        if constexpr (game::readable::kIsBmx) // ZGE4 frees the link later (kHandlersUnsubscribedMessage)
        {
            link->handler = MessageHandler();
            sendNewMessage(0, objectAddress, kHandlersUnsubscribedMessage, type, 0);
            return 1;
        }
        listRemove(&subscription.handlers, link);
        poolFree(g_messages->handlerLinkPool, link);
        if (subscription.handlers.count != 1)
            return 1;
        const ee::Ptr<HandlerChainLink> last = subscription.handlers.head;
        listRemove(&subscription.handlers, last);
        poolFree(g_messages->handlerLinkPool, last);
        searchTreeValue<SubscriptionHandlerWord>(found).handler = ee::Ptr<MessageHandler>(&last->handler);
        return 1;
    }
    READABLE_FUNCTION_WAITS(unsubscribe, Verified);

    // sendNewMessage: a message from sender to target sent (sendEngineMessage:
    // flags kMessageNow delivers it at once); the send's result.
    int32_t sendNewMessage(ee::Address sender, ee::Address target, uint32_t type, ee::Address data, uint32_t flags)
    {
        ee::Local<EngineMessage> message;
        message->target = target;
        message->sender = sender;
        message->type = type;
        message->flags = flags;
        message->data = data;
        return sendEngineMessage(message);
    }
    READABLE_FUNCTION_WAITS(sendNewMessage, Verified);

    // initMessages: the messages' state set up: no object waiting, and the
    // records' pools (the waiting objects' 64 a block, the handler links' 16,
    // the rest 256). 1.
    uint32_t initMessages()
    {
        MessageSystem &messages = *g_messages;
        initSearchTree(&messages.waitingObjects, sym::lessWord);
        messages.waitingObjectPool = ee::Ptr<Pool>(createPool(sizeof(WaitingObjectNode), 0x40, 0));
        messages.messageBoxPool = ee::Ptr<Pool>(createPool(sizeof(MessageBox), 0x100, 0));
        messages.queuedMessagePool = ee::Ptr<Pool>(createPool(sizeof(EngineMessage), 0x100, 0));
        messages.handlerLinkPool = ee::Ptr<Pool>(createPool(sizeof(HandlerLink), 0x10, 0));
        messages.subscriptionPool = ee::Ptr<Pool>(createPool(sizeof(SubscriptionNode), game::readable::perGame(0x100u, 0x100u, 0x180u), 0)); // (ZGE4 keeps more a block)
        return 1;
    }
    READABLE_FUNCTION_WAITS(initMessages, Verified);

    // shutdownMessages: the pools destroyed (what they hold with them). 1.
    uint32_t shutdownMessages()
    {
        MessageSystem &messages = *g_messages;
        destroyPool(messages.waitingObjectPool);
        destroyPool(messages.messageBoxPool);
        destroyPool(messages.queuedMessagePool);
        destroyPool(messages.handlerLinkPool);
        destroyPool(messages.subscriptionPool);
        messages.subscriptionPool = ee::Ptr<Pool>();
        messages.waitingObjectPool = ee::Ptr<Pool>();
        messages.messageBoxPool = ee::Ptr<Pool>();
        messages.queuedMessagePool = ee::Ptr<Pool>();
        messages.handlerLinkPool = ee::Ptr<Pool>();
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownMessages, Verified);

    namespace
    {
        // The object's record taken out of the set of objects with messages
        // waiting (0: it was not there).
        ee::Ptr<WaitingObjectNode> removeWaitingObject(ee::Address object)
        {
            const ee::Ptr<SearchTree> waitingObjects = &g_messages->waitingObjects;
            return treeRecordOf<WaitingObjectNode>(searchTreeRemove(waitingObjects, object));
        }

        // A search tree's root node, as the record its nodes are.
        template <typename Record>
        ee::Ptr<Record> rootRecord(const SearchTree &tree)
        {
            return tree.root.cast<Record>();
        }

        // Whether a message of the type waits in the queue.
        bool queued(const MessageBox &box, uint32_t type)
        {
            for (ee::Ptr<EngineMessage> other = box.queue.head.cast<EngineMessage>(); other; other = other->link.next.cast<EngineMessage>())
                if (other->type == type)
                    return true;
            return false;
        }

        // The box's queued messages delivered, oldest first, each taken out
        // of the queue before it goes and given back to the pool after. One
        // flagged kMessageLatestOnly is dropped when a newer one of its type
        // waits behind it.
        void deliverQueue(ee::Ptr<MessageBox> box)
        {
            for (ee::Ptr<EngineMessage> message = box->queue.head.cast<EngineMessage>(); message; message = box->queue.head.cast<EngineMessage>())
            {
                listRemove(&box->queue, &message->link);
                const bool superseded = (message->flags & kMessageLatestOnly) && queued(*box, message->type);
                if (!superseded)
                    deliverMessage(message);
                poolFree(g_messages->queuedMessagePool, message);
            }
        }
    }

    namespace
    {
        // The target's message box emptied: its queued messages dropped undelivered, each subscription freed
        // with its handler list (releaseMessages; box is the target's).
        void dropMessageBoxContents(ee::Ptr<MessageTarget> target, ee::Ptr<MessageBox> box)
        {
            for (ee::Ptr<ListNode> dropped = box->queue.head; dropped; dropped = box->queue.head)
            {
                listRemove(&box->queue, dropped);
                poolFree(g_messages->queuedMessagePool, dropped);
            }
            SearchTree &subscriptions = target->messages->subscriptions;
            while (subscriptions.count != 0)
            {
                const ee::Ptr<SubscriptionNode> node = rootRecord<SubscriptionNode>(subscriptions);
                List &handlers = node->subscription.handlers;
                for (ee::Ptr<ListNode> link = handlers.head; link; link = handlers.head)
                {
                    listRemove(&handlers, link);
                    poolFree(g_messages->handlerLinkPool, link);
                }
                poolFree(g_messages->subscriptionPool, removeSubscription(&subscriptions, &node->subscription.type));
            }
        }
    }

    // releaseMessages: a deleted object's messages (deleteSceneObject): the
    // object told at once (kDeletingMessage), taken out of the set of objects
    // with messages waiting, its queued messages dropped undelivered, each
    // subscription freed with its handler list, and its message box given
    // back. Nothing for an object with no box.
    namespace
    {
        // ZGE2's key for the waiting set in releaseMessages, its frame's first quadword: it writes the object to
        // the third word and hands the tree the fourth, which it never writes (an engine bug kept: the set is
        // searched for the word that follows; it is empty on every recorded call, so nothing is compared).
        struct Zge2WaitingKey
        {
            uint32_t _unused[2];
            uint32_t object;
            uint32_t searched;
        };
    }

    void releaseMessages(ee::Address object)
    {
        const ee::Ptr<MessageTarget> target(object);
        if (!target->messages)
            return;
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 sends the object no kDeletingMessage, so its box is still there below.
            ee::Local<Zge2WaitingKey> key;
            key->object = object;
            if (const ee::Ptr<WaitingObjectNode> waiting = removeWaitingObject(ee::Address(&key->searched)))
                poolFree(g_messages->waitingObjectPool, waiting);
            dropMessageBoxContents(target, target->messages);
            freeSubscriberRecord(target);
            return;
        }
        ee::Local<EngineMessage> message;
        message->data = 0;
        message->flags = kMessageNow;
        message->type = kDeletingMessage;
        message->target = object;
        message->sender = 0;
        sendEngineMessage(message);

        ee::Local<uint32_t> key;
        *key = object;
        if (const ee::Ptr<WaitingObjectNode> waiting = removeWaitingObject(key))
            poolFree(g_messages->waitingObjectPool, waiting);

        // The message's handlers may have given the box back already.
        ee::Ptr<MessageBox> box = target->messages;
        if (box)
        {
            for (ee::Ptr<ListNode> dropped = box->queue.head; dropped; dropped = box->queue.head)
            {
                listRemove(&box->queue, dropped);
                poolFree(g_messages->queuedMessagePool, dropped);
            }
            SearchTree &subscriptions = target->messages->subscriptions;
            while (subscriptions.count != 0)
            {
                const ee::Ptr<SubscriptionNode> node = rootRecord<SubscriptionNode>(subscriptions);
                List &handlers = node->subscription.handlers;
                for (ee::Ptr<ListNode> link = handlers.head; link; link = handlers.head)
                {
                    listRemove(&handlers, link);
                    poolFree(g_messages->handlerLinkPool, link);
                }
                poolFree(g_messages->subscriptionPool, removeSubscription(&subscriptions, &node->subscription.type));
            }
        }
        freeSubscriberRecord(target);
    }
    READABLE_FUNCTION_WAITS(releaseMessages, Verified);

    // deliverObjectMessages (no caller in this build): the object's queued
    // messages delivered (it must have a message box) and the object taken
    // out of the set of objects waiting; its box given back if it has no
    // subscriptions left.
    void deliverObjectMessages(uint32_t object)
    {
        ee::Local<uint32_t> key;
        *key = object;
        const ee::Ptr<MessageTarget> target(object);
        const ee::Ptr<MessageBox> box = target->messages;
        if (box->queue.head)
        {
            deliverQueue(box);
            if (const ee::Ptr<WaitingObjectNode> waiting = removeWaitingObject(key))
                poolFree(g_messages->waitingObjectPool, waiting);
        }
        if (target->messages->subscriptions.count == 0)
            freeSubscriberRecord(target);
    }
    READABLE_FUNCTION_WAITS(deliverObjectMessages, Draft);

    // deliverQueuedMessages (no caller in this build): every waiting object's
    // queued messages delivered, the object taken out of the set first; an
    // object left with no subscriptions has its box given back.
    void deliverQueuedMessages()
    {
        SearchTree &waitingObjects = g_messages->waitingObjects;
        while (waitingObjects.count != 0)
        {
            const ee::Ptr<WaitingObjectNode> first = rootRecord<WaitingObjectNode>(waitingObjects);
            const ee::Ptr<MessageTarget> object = first->object;
            poolFree(g_messages->waitingObjectPool, removeWaitingObject(&first->object));
            const ee::Ptr<MessageBox> box = object->messages;
            if (box->queue.head)
                deliverQueue(box);
            if (object->messages->subscriptions.count == 0)
                freeSubscriberRecord(object);
        }
    }
    READABLE_FUNCTION_WAITS(deliverQueuedMessages, Draft);
}
