// ZGE4's script blocks (script_blocks.h; BMX XXX only): the block classes' registry. The reader of a script's
// blocks and the unregistering are the original's.

#include "script_blocks.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/container/tree.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/text/token_reader.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/text/script_blocks.cpp.h"

namespace zge
{
    uint32_t poolTakeCreating(ee::Ptr<ee::Ptr<Pool>> slot, uint32_t recordBytes); // (pool.cpp)
    void poolGiveDestroying(ee::Ptr<ee::Ptr<Pool>> slot, ee::Address record);

    namespace
    {
        // The classes by their begin keyword's hash; the tree is set up when the first class is registered.
        inline ee::Global<uint32_t, sym::bmx::g_blockClassesReady> g_blockClassesReady;
        constexpr uint32_t kBlockClasses = sym::bmx::g_blockClasses;       // a SearchTree
        constexpr uint32_t kBlockClassPool = sym::bmx::g_blockClassPool;   // the pool's word (made with the first class)
        constexpr uint32_t kValueSetPool = sym::bmx::g_scriptValueSetPool; // the value sets' pool word
    }

    // newScriptValues: an empty set of values.
    inline ee::Ptr<ScriptValueSet> newScriptValues()
    {
        const ee::Ptr<ScriptValueSet> values(poolTakeCreating(ee::Ptr<ee::Ptr<Pool>>(kValueSetPool), sizeof(ScriptValueSet)));
        initSearchTree(valuesTree(values), sym::lessWord);
        values->watcher = 0;
        values->watcherOwner = 0;
        return values;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newScriptValues);

    inline ee::Ptr<BlockClass> findBlockClass(ee::Ptr<const char> begin)
    {
        ee::Local<uint32_t> hash;
        *hash = nameHash(begin);
        return treeRecordOf<BlockClass>(searchTreeFind(ee::Ptr<SearchTree>(kBlockClasses), hash));
    }
    READABLE_FUNCTION_ONLY(bmx, findBlockClass);

    // addChildBlockClass: the child in the parent's first free place (none free: nothing is done).
    inline void addChildBlockClass(ee::Ptr<BlockClass> parent, ee::Ptr<BlockClass> child)
    {
        for (uint32_t i = 0; i < kBlockClassChildren; ++i)
        {
            if (!parent->children[i])
            {
                parent->children[i] = child;
                child->parent = parent;
                return;
            }
        }
    }
    READABLE_FUNCTION_ONLY(bmx, addChildBlockClass);

    // setBlockKeyName, blockKeyName: a key's name in the class's names.
    inline uint32_t setBlockKeyName(ee::Ptr<BlockClass> blockClass, uint32_t key, ee::Ptr<const char> name)
    {
        return game::tail(stringCopy, blockClass->keyNames + key * kBlockKeywordBytes, name);
    }
    READABLE_FUNCTION_ONLY(bmx, setBlockKeyName);
    inline ee::Ptr<char> blockKeyName(ee::Ptr<BlockClass> blockClass, uint32_t key)
    {
        return blockClass->keyNames + key * kBlockKeywordBytes;
    }
    READABLE_FUNCTION_ONLY(bmx, blockKeyName);

    inline void registerBlockClass(ee::Ptr<BlockClassSetup> setup)
    {
        if (*g_blockClassesReady == 0)
        {
            initSearchTree(ee::Ptr<SearchTree>(kBlockClasses), sym::lessWord);
            *g_blockClassesReady = 1;
        }
        const ee::Ptr<BlockClass> blockClass(poolTakeCreating(ee::Ptr<ee::Ptr<Pool>>(kBlockClassPool), sizeof(BlockClass)));
        memoryFill(blockClass, 0, sizeof(BlockClass));
        blockClass->beginHash = nameHash(setup->begin);
        if (searchTreeInsert(ee::Ptr<SearchTree>(kBlockClasses), &blockClass->node) == 0)
        {
            poolGiveDestroying(ee::Ptr<ee::Ptr<Pool>>(kBlockClassPool), blockClass);
            return;
        }
        stringCopy(blockClass->begin, setup->begin);
        stringCopy(blockClass->end, setup->end);
        blockClass->reading = 0;
        blockClass->values = ee::Ptr<ScriptValueSet>();
        memoryFill(&blockClass->children, 0, sizeof(blockClass->children));
        blockClass->onBegin = setup->onBegin;
        blockClass->owner = 0;
        blockClass->onEnd = setup->onEnd;
        blockClass->keyCount = setup->keyCount;
        if (setup->keyCount != 0)
        {
            blockClass->keyKinds = ee::Ptr<uint32_t>(heapAllocate(setup->keyCount * sizeof(uint32_t)));
            blockClass->keyNames = ee::Ptr<char>(heapAllocate(setup->keyCount * kBlockKeywordBytes));
            blockClass->keysRead = ee::Ptr<uint32_t>(heapAllocate(setup->keyCount * sizeof(uint32_t)));
            memoryFill(blockClass->keysRead, 0, blockClass->keyCount * sizeof(uint32_t));
            for (uint32_t key = 0; key < setup->keyCount; ++key)
            {
                blockClass->keyKinds[key] = setup->keyKinds[key];
                setBlockKeyName(blockClass, key, setup->keyNames[key]);
            }
        }
        else
        {
            blockClass->keyNames = ee::Ptr<char>();
            blockClass->keyKinds = ee::Ptr<uint32_t>();
            blockClass->keysRead = ee::Ptr<uint32_t>();
        }
        if (!setup->parentBegin)
        {
            blockClass->parent = ee::Ptr<BlockClass>();
            return;
        }
        addChildBlockClass(findBlockClass(setup->parentBegin), blockClass);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, registerBlockClass);

    inline ee::Ptr<ScriptValueSet> blockClassValues(ee::Ptr<const char> begin)
    {
        const ee::Ptr<BlockClass> blockClass = findBlockClass(begin);
        if (!blockClass->values)
            blockClass->values = newScriptValues();
        return blockClass->values;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, blockClassValues);

    inline uint32_t blockClassParentName(ee::Ptr<const char> begin, ee::Ptr<char> out)
    {
        const ee::Ptr<BlockClass> parent = findBlockClass(begin)->parent;
        if (!parent)
            return 0;
        stringCopy(out, parent->begin);
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, blockClassParentName);

    inline uint32_t blockClassOwner(ee::Ptr<const char> begin) { return findBlockClass(begin)->owner; }
    READABLE_FUNCTION_ONLY(bmx, blockClassOwner);
    inline void setBlockClassOwner(ee::Ptr<const char> begin, ee::Address owner) { findBlockClass(begin)->owner = owner; }
    READABLE_FUNCTION_ONLY(bmx, setBlockClassOwner);
    // removeChildBlockClass: the class out of its parent's children (nothing with no parent, or when the parent
    // does not hold it).
    inline void removeChildBlockClass(ee::Ptr<BlockClass> blockClass)
    {
        const ee::Ptr<BlockClass> parent = blockClass->parent;
        if (!parent)
            return;
        for (uint32_t i = 0; i < kBlockClassChildren; ++i)
        {
            if (parent->children[i] == blockClass)
            {
                parent->children[i] = ee::Ptr<BlockClass>();
                blockClass->parent = ee::Ptr<BlockClass>();
                return;
            }
        }
    }
    READABLE_FUNCTION_ONLY(bmx, removeChildBlockClass);

    // unregisterBlockClass: the class of that begin keyword gone, its children first: out of its parent, its keys
    // and values freed, out of the tree and back to the pool. Nothing for a keyword not registered.
    inline void unregisterBlockClass(ee::Ptr<const char> begin)
    {
        ee::Local<uint32_t> hash;
        *hash = nameHash(begin);
        const ee::Ptr<BlockClass> blockClass = treeRecordOf<BlockClass>(searchTreeFind(ee::Ptr<SearchTree>(kBlockClasses), hash));
        if (!blockClass)
            return;
        for (uint32_t i = 0; i < kBlockClassChildren; ++i)
        {
            const ee::Ptr<BlockClass> child = blockClass->children[i];
            if (child)
                unregisterBlockClass(ee::Ptr<const char>(child->begin));
        }
        removeChildBlockClass(blockClass);
        if (blockClass->keyCount != 0)
        {
            heapFree(blockClass->keyKinds);
            heapFree(blockClass->keyNames);
            heapFree(blockClass->keysRead);
        }
        if (blockClass->values)
            freeScriptValues(blockClass->values);
        searchTreeRemove(ee::Ptr<SearchTree>(kBlockClasses), hash);
        poolGiveDestroying(ee::Ptr<ee::Ptr<Pool>>(kBlockClassPool), blockClass);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, unregisterBlockClass);
    uint32_t saveTokenReader();                 // (token_reader.cpp)
    uint32_t restoreTokenReader(uint32_t saved);
    uint32_t beginScriptFromFile(uint32_t file);

    namespace // the game functions below call: ee/plain.h
    {
        // readScriptBlock: one block of the class read, its begin keyword read already, to its end keyword: its keys'
        // values, the blocks inside it, its handlers. 1 to go on.
        int32_t readScriptBlock(ee::Ptr<BlockClass> blockClass) { return ee::game<int32_t>(sym::bmx::readScriptBlock, blockClass); }
    }

    // readScriptBlocks: a file of blocks read with the token reader (its state kept and put back): each top-level
    // block by its class. 1 when the whole file was read; 0 when no class is registered, at a keyword that begins
    // none or one that only sits inside another, or when a block's reading ends early.
    inline uint32_t readScriptBlocks(uint32_t file)
    {
        if (*g_blockClassesReady == 0)
            return 0;
        ee::Local<char[kBlockKeywordBytes]> keyword;
        uint32_t result = 1;
        const uint32_t saved = saveTokenReader();
        beginScriptFromFile(file);
        for (int32_t read = readStringToken(keyword, kBlockKeywordBytes); read == 1; read = readStringToken(keyword, kBlockKeywordBytes))
        {
            const ee::Ptr<BlockClass> blockClass = findBlockClass(keyword);
            if (!blockClass || blockClass->parent || readScriptBlock(blockClass) != 1)
            {
                result = 0;
                break;
            }
        }
        restoreTokenReader(saved);
        return result;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readScriptBlocks);
}
