#pragma once

// ZGE4's script blocks (script_blocks.cpp; BMX XXX only): a script made of nested blocks, each between a begin
// and an end keyword with keys inside it (OBJECTIVE_BEGIN .. OBJECTIVE_END with NAME and TYPE; PARAMETERS_BEGIN
// .. with TIMEOUT, ACTIVE, ..). A block class says what a block is: its two keywords, the block it sits in, its
// keys with the kind of value each holds, and what to call when the block begins and ends. The reader keeps each
// key's value (script_values.h) for the handlers to ask for.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "zge/container/tree.h"
#include "zge/text/script_values.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    constexpr uint32_t kBlockKeywordBytes = 0x80;
    constexpr uint32_t kBlockClassChildren = 16;

    // (the block's begin keyword) -> 1 to go on; anything else ends the reading with that result.
    using BlockHandler = ee::FunctionPtr<int32_t(uint32_t beginKeyword)>;

    // What registerBlockClass takes (0x20 bytes).
    struct BlockClassSetup
    {
        ee::Ptr<const char> begin;                  // +0x00 the keyword a block of this class starts with
        ee::Ptr<const char> end;                    // +0x04 the keyword that ends it
        ee::Ptr<const char> parentBegin;            // +0x08 the begin keyword of the class it sits in, 0: none
        ee::Ptr<ee::Ptr<const char>> keyNames;      // +0x0C keyCount names
        ee::Ptr<uint32_t> keyKinds;                 // +0x10 keyCount ScriptValueKind words
        uint32_t keyCount;                          // +0x14
        BlockHandler onBegin;                       // +0x18 as the block's reading starts, 0: none
        BlockHandler onEnd;                         // +0x1C once its end keyword is read, 0: none
    };
    static_assert(sizeof(BlockClassSetup) == 0x20);

    // A registered class (0x174 bytes), a node of the classes' tree by its begin keyword's hash.
    struct BlockClass
    {
        SearchTreeNode node;                                 // +0x000
        uint32_t beginHash;                                  // +0x008 nameHash(begin)
        char begin[kBlockKeywordBytes];                      // +0x00C
        char end[kBlockKeywordBytes];                        // +0x08C
        ee::Ptr<BlockClass> parent;                          // +0x10C
        ee::Ptr<BlockClass> children[kBlockClassChildren];   // +0x110 0: a free place
        uint32_t keyCount;                                   // +0x150
        ee::Ptr<uint32_t> keyKinds;                          // +0x154 keyCount words (the heap's)
        ee::Ptr<char> keyNames;                              // +0x158 keyCount names, kBlockKeywordBytes each
        ee::Ptr<uint32_t> keysRead;                          // +0x15C keyCount words: 1 once the block being read has given the key
        uint32_t reading;                                    // +0x160 1 while a block of the class is being read
        ee::Ptr<ScriptValueSet> values;                      // +0x164 the last block's values, 0: none yet
        BlockHandler onBegin;                                // +0x168
        BlockHandler onEnd;                                  // +0x16C
        uint32_t owner;                                      // +0x170 whose block is being read (setBlockClassOwner)
    };
    static_assert(sizeof(BlockClass) == 0x174 && offsetof(BlockClass, parent) == 0x10C && offsetof(BlockClass, keyCount) == 0x150 &&
                  offsetof(BlockClass, values) == 0x164 && offsetof(BlockClass, owner) == 0x170);

    // findBlockClass: the class of that begin keyword, or 0.
    ee::Ptr<BlockClass> findBlockClass(ee::Ptr<const char> begin);
    // registerBlockClass: a class made from the set-up and put in its parent's children. A keyword registered
    // already is left as it was.
    void registerBlockClass(ee::Ptr<BlockClassSetup> setup);
    // unregisterBlockClass: the class of that begin keyword and its children gone.
    void unregisterBlockClass(ee::Ptr<const char> begin);
    // blockClassValues: the values of the class of that begin keyword (an empty set made when it has none).
    ee::Ptr<ScriptValueSet> blockClassValues(ee::Ptr<const char> begin);
    // blockClassParentName: the begin keyword of the class's parent into out, 1; 0 with no parent.
    uint32_t blockClassParentName(ee::Ptr<const char> begin, ee::Ptr<char> out);
    // blockClassOwner, setBlockClassOwner: the word a handler keeps with the class of that begin keyword.
    uint32_t blockClassOwner(ee::Ptr<const char> begin);
    void setBlockClassOwner(ee::Ptr<const char> begin, ee::Address owner);
    // readScriptBlocks: a file of blocks read, each top-level block by its class; 1 when the whole file was read.
    uint32_t readScriptBlocks(uint32_t file);
}
