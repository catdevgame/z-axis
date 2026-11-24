// The engine's binary search tree's lookup (block 168, zprofile; DM2 has it
// word for word, but for the walks: below): the named-object tables (objects by name hash,
// games/aggressive/objects.cpp's kHashFind, the message subscriptions) are
// such trees. Each node keeps its value inline after its two links; the
// tree's comparison is a game function less(a, b) on two values.

#include "tree.h"

#include "zge/memory/pool.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/function_ptr.h"
#include "sym/zge/container/tree.cpp.h"

namespace zge
{
    namespace
    {
        using SearchTreeLess = ee::FunctionPtr<int32_t(ee::Address a, ee::Address b)>;
        // A node's value sits right after its two links.
        ee::Address valueOf(ee::Ptr<SearchTreeNode> node) { return node + 1; }
    }

    namespace
    {
        // searchTreeFind's walk of a tree in lessWord's order, ZGE4's: the key's word read once and the words
        // compared here, no comparison called.
        inline ee::Ptr<SearchTreeNode> findWordValue(ee::Ptr<SearchTreeNode> root, uint32_t key)
        {
            auto word = [](ee::Ptr<SearchTreeNode> node) { return *ee::Ptr<uint32_t>(valueOf(node)); };
            ee::Ptr<SearchTreeNode> found;
            for (ee::Ptr<SearchTreeNode> node = root; node;)
            {
                if (word(node) < key)
                    node = node->higher;
                else
                {
                    found = node;
                    node = node->lower;
                }
            }
            if (found && !(key < word(found)))
                return found;
            return {};
        }
    }

    // searchTreeFind: the node whose value equals key (neither sorts before
    // the other), or none: down the tree to the first value not less than the
    // key, then that one compared the other way round. (ZGE4 walks a tree of
    // lessWord's itself: findWordValue.) An empty tree is none at once, its
    // comparison not even read, as in every game's code.
    ee::Ptr<SearchTreeNode> searchTreeFind(ee::Ptr<SearchTree> tree, ee::Address key)
    {
        const ee::Ptr<SearchTreeNode> root = tree->root;
        if (!root)
            return {};
        const uint32_t lessFunction = tree->less;
        if constexpr (game::readable::kIsBmx)
        {
            if (lessFunction == sym::lessWord)
                return findWordValue(root, *ee::Ptr<const uint32_t>(key));
        }
        const SearchTreeLess less(lessFunction);
        ee::Ptr<SearchTreeNode> found;
        for (ee::Ptr<SearchTreeNode> node = root; node;)
        {
            if (less(valueOf(node), key) != 0)
                node = node->higher;
            else
            {
                found = node;
                node = node->lower;
            }
        }
        if (found && less(key, valueOf(found)) == 0)
            return found;
        return {};
    }
    READABLE_FUNCTION(searchTreeFind, Verified);

    // lessWord: the comparison of trees keyed by a 32-bit word (the
    // name hashes): a's word below b's, unsigned.
    bool lessWord(const uint32_t *a, const uint32_t *b)
    {
        return *a < *b;
    }
    READABLE_FUNCTION(lessWord, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_tokenPoolReady> g_tokenPoolReady;
        inline ee::Global<ee::Ptr<Pool>, sym::g_searchTreePool> g_searchTreePool;
    }

    uint32_t shutdownSearchTrees()
    {
        int32_t result = 0;
        if (*g_tokenPoolReady == 0)
        {
            return 1;
        }
        destroyPool(*g_searchTreePool);
        *g_searchTreePool = 0;
        result = 1;
        *g_tokenPoolReady = 0;
        return result;
        return result;
    }
    READABLE_FUNCTION_WAITS(shutdownSearchTrees, Verified);

    void initSearchTree(ee::Ptr<SearchTree> tree, uint32_t less)
    {
        tree->less = less;
        tree->root = {};
        tree->count = 0;
    }
    READABLE_FUNCTION(initSearchTree, Verified);

    // Puts a node in the tree by its value: down to the empty link it sorts
    // to, then the value it would follow compared the other way round, which
    // tells an equal value (see searchTreeFind).
    int32_t searchTreeInsert(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode> node)
    {
        ee::Ptr<SearchTreeNode> at = tree->root;
        if (!at)
        {
            tree->root = node;
            tree->count = 1;
            node->higher = {};
            node->lower = {};
            return 1;
        }

        const SearchTreeLess less(tree->less);
        const ee::Address value = valueOf(node);
        ee::Ptr<SearchTreeNode> before; // the last node the value sorted after
        bool goesLower = false;
        for (;;)
        {
            goesLower = less(value, valueOf(at)) != 0;
            const ee::Ptr<SearchTreeNode> next = goesLower ? at->lower : at->higher;
            if (!goesLower)
                before = at;
            if (!next)
                break;
            at = next;
        }
        if (before && less(valueOf(before), value) == 0)
            return 0;

        if (goesLower)
            at->lower = node;
        else
            at->higher = node;
        node->higher = {};
        node->lower = {};
        tree->count = tree->count + 1;
        return 1;
    }
    READABLE_FUNCTION(searchTreeInsert, Verified);

    // Takes out the node whose value equals key: the node, or 0 if there is
    // none. A node with two children gives way to the first node after it, the
    // leftmost of its higher side. The node's own links are left as they were.
    // An empty tree is none at once, its comparison not read (as searchTreeFind).
    ee::Ptr<SearchTreeNode> searchTreeRemove(ee::Ptr<SearchTree> tree, ee::Address key)
    {
        const ee::Ptr<SearchTreeNode> root = tree->root;
        if (!root)
            return {};
        ee::Ptr<SearchTreeNode> found;       // the first node not less than the key
        ee::Ptr<SearchTreeNode> foundParent;
        ee::Ptr<SearchTreeNode> parent;
        const SearchTreeLess less(tree->less);
        for (ee::Ptr<SearchTreeNode> node = root; node;)
        {
            if (less(valueOf(node), key) != 0)
            {
                parent = node;
                node = node->higher;
            }
            else
            {
                found = node;
                foundParent = parent;
                parent = node;
                node = node->lower;
            }
        }
        if (!found || less(key, valueOf(found)) != 0)
            return {};

        ee::Ptr<SearchTreeNode> replacement = found->lower;
        if (!replacement)
            replacement = found->higher;
        else if (found->higher)
        {
            const ee::Ptr<SearchTreeNode> lowerSide = replacement;
            ee::Ptr<SearchTreeNode> nextParent = found;
            replacement = found->higher;
            while (replacement->lower)
            {
                nextParent = replacement;
                replacement = replacement->lower;
            }
            replacement->lower = lowerSide;
            if (nextParent != found)
            {
                nextParent->lower = replacement->higher;
                replacement->higher = found->higher;
            }
        }

        if (found == tree->root)
            tree->root = replacement;
        else if (foundParent->lower == found)
            foundParent->lower = replacement;
        else
            foundParent->higher = replacement;
        tree->count = tree->count - 1;
        return found;
    }
    READABLE_FUNCTION(searchTreeRemove, Verified);

    // The subtree under node in value order, each node handed to visit with
    // context; 1 at the end, 0 as soon as a visit returns 0. Nothing reads
    // `parent`: the original hands each call the node above it.
    int32_t searchTreeWalk(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode> parent, ee::Ptr<SearchTreeNode> node, SearchTreeVisit visit, uint32_t context);
    int32_t searchTreeWalk(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode>, ee::Ptr<SearchTreeNode> node, SearchTreeVisit visit, uint32_t context)
    {
        if (node->lower && searchTreeWalk(tree, node, node->lower, visit, context) == 0)
            return 0;
        if (visit(node, context) == 0)
            return 0;
        if (node->higher && searchTreeWalk(tree, node, node->higher, visit, context) == 0)
            return 0;
        return 1;
    }
    // ZGE2's walk: the same without the parent argument (ZGE3 added it).
    int32_t walkSearchTreeInOrder(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode> node, SearchTreeVisit visit, uint32_t context);
    int32_t walkSearchTreeInOrder(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode> node, SearchTreeVisit visit, uint32_t context)
    {
        if (node->lower && walkSearchTreeInOrder(tree, node->lower, visit, context) == 0)
            return 0;
        if (visit(node, context) == 0)
            return 0;
        if (node->higher && walkSearchTreeInOrder(tree, node->higher, visit, context) == 0)
            return 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(searchTreeWalk, Verified, dm2, walkSearchTreeInOrder);

    // Every node in value order: 1 when the walk finished, 0 when a visit
    // stopped it or the tree is empty (ZGE2 answers an empty tree with the
    // visit's own address: the register it was moved to).
    int32_t searchTreeForEach(ee::Ptr<SearchTree> tree, SearchTreeVisit visit, ee::Address context)
    {
        const ee::Ptr<SearchTreeNode> root = tree->root;
        if constexpr (game::readable::kIsDm2)
        {
            if (!root)
                return static_cast<int32_t>(ee::Abi<SearchTreeVisit>::toReg(visit));
            return game::tail(walkSearchTreeInOrder, tree, root, visit, context);
        }
        else
        {
            if (!root)
                return 0;
            return game::tail(searchTreeWalk, tree, ee::Ptr<SearchTreeNode>(), root, visit, context);
        }
    }
    READABLE_FUNCTION_WAITS(searchTreeForEach, Verified);

    // The subtree under node, children before their parent, so that visit can
    // free each node it is handed: 1 at the end, 0 as soon as a visit returns 0.
    int32_t searchTreeVisitPostOrder(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode> node, SearchTreeVisit visit, uint32_t context);
    int32_t searchTreeVisitPostOrder(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode> node, SearchTreeVisit visit, uint32_t context)
    {
        if (node->lower && searchTreeVisitPostOrder(tree, node->lower, visit, context) == 0)
            return 0;
        if (node->higher && searchTreeVisitPostOrder(tree, node->higher, visit, context) == 0)
            return 0;
        return visit(node, context) != 0;
    }
    // ZGE2's: each side's subtree through the in-order walk (so a parent there
    // goes before its children), then the node itself; ZGE3 recurses into
    // itself, every child before its parent.
    int32_t visitSearchTreeNodeLast(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode> node, SearchTreeVisit visit, uint32_t context)
    {
        if (node->lower && walkSearchTreeInOrder(tree, node->lower, visit, context) == 0)
            return 0;
        if (node->higher && walkSearchTreeInOrder(tree, node->higher, visit, context) == 0)
            return 0;
        return visit(node, context) != 0;
    }
    READABLE_FUNCTION_WAITS_OWN(searchTreeVisitPostOrder, Verified, dm2, visitSearchTreeNodeLast);

    // Empties the tree, handing every node to visit first when there is one
    // (the shutdowns' free).
    void searchTreeClear(ee::Ptr<SearchTree> tree, SearchTreeVisit visit, uint32_t context)
    {
        const ee::Ptr<SearchTreeNode> root = tree->root;
        if (!root)
            return;
        if (visit)
            searchTreeVisitPostOrder(tree, root, visit, context);
        game::tail(initSearchTree, tree, tree->less);
    }
    READABLE_FUNCTION_WAITS(searchTreeClear, Verified);

    // The subtree under node in reverse value order (the higher side first),
    // each node handed to visit with context: 1 at the end, 0 as soon as a
    // visit returns 0. Nothing reads `parent` (as searchTreeWalk's).
    int32_t searchTreeWalkDescending(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode> parent, ee::Ptr<SearchTreeNode> node, SearchTreeVisit visit, uint32_t context);
    int32_t searchTreeWalkDescending(ee::Ptr<SearchTree> tree, ee::Ptr<SearchTreeNode>, ee::Ptr<SearchTreeNode> node, SearchTreeVisit visit, uint32_t context)
    {
        if (node->higher && searchTreeWalkDescending(tree, node, node->higher, visit, context) == 0)
            return 0;
        if (visit(node, context) == 0)
            return 0;
        if (node->lower && searchTreeWalkDescending(tree, node, node->lower, visit, context) == 0)
            return 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(searchTreeWalkDescending, Draft);

    // searchTreeForEachDescending (nothing calls it in AI): every node in
    // reverse value order: 1 when the walk finished, 0 when a visit stopped
    // it or the tree is empty.
    int32_t searchTreeForEachDescending(ee::Ptr<SearchTree> tree, SearchTreeVisit visit, uint32_t context)
    {
        const ee::Ptr<SearchTreeNode> root = tree->root;
        if (!root)
            return 0;
        return game::tail(searchTreeWalkDescending, tree, ee::Ptr<SearchTreeNode>(), root, visit, context);
    }
    READABLE_FUNCTION_WAITS(searchTreeForEachDescending, Draft);

    // searchTreeLastBefore (nothing calls it in AI): the node with the
    // greatest value that sorts before the one at key, or 0.
    ee::Ptr<SearchTreeNode> searchTreeLastBefore(ee::Ptr<SearchTree> tree, ee::Address key)
    {
        ee::Ptr<SearchTreeNode> node = tree->root;
        if (!node)
            return ee::Ptr<SearchTreeNode>();
        const SearchTreeLess less(tree->less);
        ee::Ptr<SearchTreeNode> found;
        while (node)
        {
            if (less(valueOf(node), key) != 0)
            {
                found = node;
                node = node->higher;
            }
            else
                node = node->lower;
        }
        return found;
    }
    READABLE_FUNCTION(searchTreeLastBefore, Draft);

    // searchTreeFirstNotBefore (nothing calls it in AI): the node with the
    // least value that does not sort before the one at key, or 0.
    ee::Ptr<SearchTreeNode> searchTreeFirstNotBefore(ee::Ptr<SearchTree> tree, ee::Address key)
    {
        ee::Ptr<SearchTreeNode> node = tree->root;
        if (!node)
            return ee::Ptr<SearchTreeNode>();
        const SearchTreeLess less(tree->less);
        ee::Ptr<SearchTreeNode> found;
        while (node)
        {
            if (less(valueOf(node), key) == 0)
            {
                found = node;
                node = node->lower;
            }
            else
                node = node->higher;
        }
        return found;
    }
    READABLE_FUNCTION(searchTreeFirstNotBefore, Draft);

    // freeSearchTreeRecord (a searchTreeClear visit; nothing uses it in AI):
    // the node handed back to the pool the context points to. 1.
    uint32_t freeSearchTreeRecord(ee::Ptr<SearchTreeNode> node, ee::Ptr<ee::Ptr<Pool>> pool)
    {
        poolFree(*pool, node);
        return 1;
    }
    READABLE_FUNCTION(freeSearchTreeRecord, Draft);

}

// The fields this source uses, at the offsets each game's machine code uses (AI's: its headers').
#include "sym/zge/container/tree.cpp.layout.h"
