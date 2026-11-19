#pragma once

// The engine's messages to its objects (block 168, BMX XXX PS2 word for
// word; the GameCube and Xbox builds the same layout with the box at +0x3C,
// DM2 at +0x44): a message names its target object, the sender, a type (an
// event's name hash, or an engine message number) and a word of data. Sent
// synchronously it goes to the target's handlers at once; else a copy waits
// in the target's queue and the target in the set of objects with messages
// waiting (deliverObjectMessages and deliverQueuedMessages deliver them; no
// caller in this build).
//
// An object's message box: the queue, then the subscriptions, a search tree
// (zge/container/tree.h) keyed by the message type. A subscription has one
// handler or a list of them; a handler returns 1 when it took the message
// (the rest of the list is not asked).
//
// The engine's object list (g_objectList) holds the objects that get the
// frame's update message (kUpdateMessage); updateObjects sends it to each.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"
#include "zge/memory/pool.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/message.h.h"

namespace zge
{
    struct EngineMessage
    {
        ListNode link;        // +0x00 in the target's queue (a waiting copy)
        ee::Address target;   // +0x08 the receiving object (its message box at +0x48)
        uint32_t sender;      // +0x0C 0: none
        uint32_t type;        // +0x10 the event's name hash, or an engine message number
        ee::Address data;     // +0x14 the event's word (a record's address or a number)
        uint32_t flags;       // +0x18 kMessageNow, kMessageLatestOnly
    };
    static_assert(sizeof(EngineMessage) == 0x1C && offsetof(EngineMessage, type) == 0x10);
    constexpr uint32_t kMessageNow = 1u << 0; // delivered at once (else queued)
    // Queued: dropped undelivered when another of its type waits behind it.
    constexpr uint32_t kMessageLatestOnly = 1u << 1;

    // The update message updateObjects sends every object on the list; its
    // data is the address of the float its caller passes (the frame's time
    // step [inferred]). Scripts never report it handled
    // (aggressive/script/script.h kUnreportedMessage).
    constexpr uint32_t kUpdateMessage = 0x7000u;
    // Sent to a level object as it is removed.
    constexpr uint32_t kRemovedMessage = 0x7001u;
    // Asks an object its velocity: the data points to a Vec4, (0, 0, 0, 1)
    // until the object writes it (objectVelocity; the NPCs and the traffic
    // cars answer it).
    constexpr uint32_t kVelocityMessage = 0x7002u;
    // The level objects' save (objects.cpp): how many bytes of state the
    // object keeps (the data points to a word, 0 until the object answers),
    // its state handed back to it (restore: the data points to it), and its
    // state written (save: the data points to where it goes). Nothing in AI
    // answers them.
    constexpr uint32_t kSaveSizeMessage = 0x7003u;
    constexpr uint32_t kRestoreMessage = 0x7004u;
    constexpr uint32_t kSaveMessage = 0x7005u;
    // Sent at once to an object being deleted (releaseMessages), before its
    // subscriptions go: an animator is freed on it.
    constexpr uint32_t kDeletingMessage = 1u;
    // ZGE4 (BMX XXX): its unsubscribe leaves a listed handler's link with no handler and sends the object this
    // message (the type in its word); every message box subscribes dropUnsubscribedHandlers to it, which frees
    // such links (and the subscription or the box once nothing is left). ZGE2 and ZGE3 free the link at once.
    constexpr uint32_t kHandlersUnsubscribedMessage = 2u;

    struct MessageBox
    {
        List queue;               // +0x00 EngineMessage copies waiting
        SearchTree subscriptions; // +0x0C Subscription values, by type
    };
    static_assert(offsetof(MessageBox, subscriptions) == 0x0C);

    using MessageHandler = ee::FunctionPtr<int32_t(ee::Ptr<EngineMessage> message)>;

    // A subscriptions tree node's value (at the node's +0x08).
    struct Subscription
    {
        uint32_t type;    // +0x00 the key
        MessageHandler handler; // +0x04 -> result; 0: the list's handlers instead
        List handlers;    // +0x08 HandlerLink
    };
    static_assert(offsetof(Subscription, handlers) == 0x08);
    struct HandlerLink
    {
        ListNode link;    // +0x00
        MessageHandler handler; // +0x08 -> 1 when it took the message
    };

    // An object as the messages see it.
    struct MessageTarget
    {
        uint8_t _00[game::readable::perGame(0x48, 0x44)];
        ee::Ptr<MessageBox> messages; // +0x48 (DM2: +0x44) 0: takes none
    };
    static_assert(offsetof(MessageTarget, messages) == game::readable::perGame(0x48u, 0x44u));

    // The engine's object list's nodes: links, then the object.
    struct ObjectListNode
    {
        ListNode link;   // +0x00
        uint32_t object; // +0x08
    };
    inline ee::Global<List, sym::g_objectList> g_objectList;

    // A record of the set of objects with messages waiting.
    struct WaitingObjectNode
    {
        SearchTreeNode node; // +0x00
        ee::Ptr<MessageTarget> object; // +0x08 the key
    };
    static_assert(offsetof(WaitingObjectNode, object) == kSearchTreeValue && sizeof(WaitingObjectNode) == 0x0C);

    // The messages' state (initMessages): the set of objects with messages
    // waiting, then the pools of the records.
    struct MessageSystem
    {
        SearchTree waitingObjects;       // +0x00 WaitingObjectNode, by object
        ee::Ptr<Pool> waitingObjectPool; // +0x0C WaitingObjectNode
        ee::Ptr<Pool> messageBoxPool;    // +0x10 MessageBox
        ee::Ptr<Pool> queuedMessagePool; // +0x14 EngineMessage copies
        ee::Ptr<Pool> handlerLinkPool;   // +0x18 HandlerLink
        ee::Ptr<Pool> subscriptionPool;  // +0x1C a subscriptions tree's nodes (the Subscription at +0x08)
    };
    static_assert(offsetof(MessageSystem, waitingObjectPool) == 0x0C && offsetof(MessageSystem, subscriptionPool) == 0x1C);
    inline ee::Global<MessageSystem, sym::g_messages> g_messages;

    int32_t deliverMessage(ee::Ptr<EngineMessage> message);
    int32_t sendEngineMessage(ee::Ptr<EngineMessage> message);              // kSendEngineMessage
    int32_t sendUpdateMessage(uint32_t object, uint32_t data);
    void forEachObject(uint32_t visitFunction, ee::Address data);
    void updateObjects(ee::Float step);
}

namespace zge
{
    // (message.cpp)
    uint32_t subscribe(ee::Address objectAddress, uint32_t type, MessageHandler handler);
    uint32_t unsubscribe(ee::Address objectAddress, uint32_t type, MessageHandler handler);
    // Whether the object has a subscription to the type.
    uint32_t findSubscription(ee::Ptr<MessageTarget> object, uint32_t type);
    int32_t sendNewMessage(ee::Address sender, ee::Address target, uint32_t type, ee::Address data, uint32_t flags);
    // deliverObjectMessages (message.cpp): the object's waiting messages delivered.
    void deliverObjectMessages(uint32_t object);
    uint32_t initMessages();
    uint32_t shutdownMessages();
    void releaseMessages(ee::Address object);
}
