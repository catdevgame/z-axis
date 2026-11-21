#pragma once

// The engine's binary search trees (zge/container/tree.cpp): the
// named-object tables, the message subscriptions (zge/scene/message.h).
// Each node keeps its value inline after its two links; the tree's
// comparison is a game function less(a, b) on two values.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/container/tree.h.h"

namespace zge
{
    struct SearchTreeNode
    {
        ee::Ptr<SearchTreeNode> lower;  // +0x00 the node of the values that sort before this one, 0: none
        ee::Ptr<SearchTreeNode> higher; // +0x04 the values after it
        // +0x08 the value (its size is the tree's)
    };
    constexpr uint32_t kSearchTreeValue = 0x08u;

    // A tree node's value, as what the tree holds.
    template <typename Value>
    Value &searchTreeValue(uint32_t node)
    {
        return *ee::Ptr<Value>(node + kSearchTreeValue);
    }
    // Where a node's value is.
    template <typename Value>
    ee::Ptr<Value> searchTreeValueOf(ee::Ptr<SearchTreeNode> node)
    {
        return (node + 1).cast<Value>();
    }
    template <typename Value>
    Value &searchTreeValue(ee::Ptr<SearchTreeNode> node)
    {
        return *searchTreeValueOf<Value>(node);
    }

    // A node as the record it heads: a tree's records begin with their two
    // links and keep the value after them (the class registry's entries, the
    // keyed values, the animators, the double rails' edges).
    template <typename Entry>
    ee::Ptr<Entry> searchTreeEntry(ee::Ptr<SearchTreeNode> node)
    {
        return node.cast<Entry>();
    }

    struct SearchTree
    {
        ee::Ptr<SearchTreeNode> root; // +0x00 the top node, 0: empty
        uint32_t less;                // +0x04 (a, b) -> non-zero if value a sorts before value b
        int32_t count;                // +0x08 the nodes in the tree
    };
    static_assert(offsetof(SearchTree, less) == 0x04 && offsetof(SearchTree, count) == 0x08);

    // A tree's node as the record it is the start of (a typed view of the same address; 0 stays 0).
    template <typename Record>
    ee::Ptr<Record> treeRecordOf(ee::Ptr<SearchTreeNode> node)
    {
        return node.template cast<Record>();
    }

    // searchTreeFind: the node whose value equals the one at `key`, or none.
    ee::Ptr<SearchTreeNode> searchTreeFind(ee::Ptr<SearchTree> tree, ee::Address key);
    // initSearchTree: an empty tree that sorts by less.
    void initSearchTree(ee::Ptr<SearchTree> tree, uint32_t less);
    // searchTreeInsert: the node put in the tree, 1; 0 when its value is there
    // already (the node is not put in).
    constexpr uint32_t kSearchTreeInsert = sym::searchTreeInsert;
    int32_t searchTreeInsert(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode> node);
    // searchTreeRemove: the node whose value equals the one at `key` taken out
    // of the tree (the node), or none.
    ee::Ptr<SearchTreeNode> searchTreeRemove(ee::Ptr<SearchTree> tree, ee::Address key);
    // searchTreeForEach: visit(node, context) for every node in value order: 1
    // when the walk finished, 0 when a visit stopped it or the tree is empty.
    // searchTreeClear: the tree emptied, every node handed to visit first when
    // there is one.
    // A walk's visit: (node, the walk's context word), non-zero to go on.
    using SearchTreeVisit = ee::FunctionPtr<int32_t(ee::Ptr<SearchTreeNode> node, uint32_t context)>;
    int32_t searchTreeForEach(ee::Ptr<SearchTree> tree, SearchTreeVisit visit, ee::Address context);
    void searchTreeClear(ee::Ptr<SearchTree> tree, ee::FunctionPtr<int32_t(ee::Ptr<SearchTreeNode> node, uint32_t context)> visit, uint32_t context);
}
