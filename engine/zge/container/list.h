#pragma once

// The engine's doubly linked lists: a head, a tail and a count, and nodes
// whose first two words are the links (the rest is the owner's). The
// record pools (zge/memory/pool.h) keep their records on them. The
// functions are in zge/utilities.cpp.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/container/list.h.h"

namespace zge
{
    struct ListNode
    {
        ee::Ptr<ListNode> next; // +0x00
        ee::Ptr<ListNode> prev; // +0x04
    };

    struct List
    {
        ee::Ptr<ListNode> head; // +0x00
        ee::Ptr<ListNode> tail; // +0x04
        int32_t count;          // +0x08
    };
    static_assert(sizeof(List) == 0x0C && offsetof(List, count) == 8);

    // A list's node as the record it is the start of (a typed view of the
    // same address).
    template <typename Record>
    ee::Ptr<Record> recordOfNode(ee::Ptr<ListNode> node)
    {
        return node.template cast<Record>();
    }

    // listPushFront: node put at the front of the list.
    void listPushFront(ee::Address list, ee::Address node);

    // listRemove: node taken out of the list (its own links are left as
    // they were).
    void listRemove(ee::Address list, ee::Address node);

    // The functions take any list (a record that starts with a List) and
    // any node (a record that starts with a ListNode).

    // initList: the list made empty (utilities.cpp).
    void initList(ee::Address list);

    // listPush: node put at the end of the list (utilities.cpp).
    void listPush(ee::Address list, ee::Address node);

    // listInsertAfter: node put after `after`, or at the front with no
    // `after` (utilities.cpp; nothing calls it in AI).
    void listInsertAfter(ee::Address list, ee::Address node, ee::Address after);

    // listContains: 1 if node is on the list, else 0 (utilities.cpp).
    uint32_t listContains(ee::Address list, ee::Address node);

    // listPush by address.
    constexpr uint32_t kListPushBack = sym::listPush;
}
