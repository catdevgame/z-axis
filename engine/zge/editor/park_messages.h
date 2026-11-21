#pragma once

// The park editor's messages (park_messages.cpp): sent breadth first through an
// object or anchor and what hangs off it.

#include "zge/editor/edit_records.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "zge/math/types.h"
#include <cstddef>
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    // (park_messages.cpp)
    int32_t broadcastEditMessage(ee::Address start, uint32_t makeMessage, uint32_t argument);
    uint32_t messageSender(uint32_t message);
    void queueEditMessageTarget(ee::Address address, ee::Address target);
    int32_t runMessageAnchorHandler(uint32_t address, uint32_t anchor);
    int32_t runMessageObjectHandler(uint32_t address, uint32_t object);

    constexpr uint32_t kQueueCapacity = 0x400;

    struct EditMessageQueue
    {
        uint32_t target[kQueueCapacity]; // +0x0000 0 past the tail
        uint32_t head;        // +0x1000
        uint32_t tail;        // +0x1004
        uint8_t _1008[8];
    };
    static_assert(sizeof(EditMessageQueue) == 0x1010);

    // What a message does to one target.
    using TargetHandler = ee::FunctionPtr<int32_t(uint32_t, uint32_t)>;

    struct EditMessage
    {
        uint32_t queue;       // +0x00
        TargetHandler objectHandler; // +0x04 (message, the object's EditObject part)
        TargetHandler anchorHandler; // +0x08 (message, anchor)
        uint32_t argument;    // +0x0C
    };
    static_assert(sizeof(EditMessage) == 0x10);

    // (park_messages.cpp, for the games' own parts)
    uint32_t makeDeleteMessage(uint32_t queue, uint32_t message);
    uint32_t makeMovedMessage(uint32_t queue, uint32_t message);
    void initEditMessageQueue(ee::Address address);
}
