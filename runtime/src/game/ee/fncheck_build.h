#pragma once

// What the build knows about the checked functions, for zx-fncheck
// (docs/READABLE.md, "How a function is proven"):
//
// - Block tables (path coverage). tools/fncheck_traced_gen.py writes each
//   traced original as a copy of its generated function with a marker at the
//   start of every basic block (the function's entry, every branch or jump
//   target inside it, the instruction after every branch or jump; the
//   addresses from the generated code's instruction comments). A marker
//   tells the active BlockSink (zx-fncheck while it replays the original);
//   in the game, with no sink, it is one thread-local load.
// - Callee stack arguments: functions whose code reads above their entry sp
//   (a ninth argument and on, which the caller stores at 0(sp) before the
//   call), [low, high) above that sp. zx-fncheck compares what a readable
//   version leaves there at such a call with what the original did.
// - Digests (sha256, the first 16 hex digits): of each generated original
//   (in its block table) and of each readable function's source
//   (tools/readable_digest.py: its definition and the same file's functions
//   it calls, comments and spacing left out), printed with every result so
//   that a pass names what it was made against.

#include "game_id.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fncheck
{
    struct BlockTable;

    class BlockSink
    {
    public:
        virtual ~BlockSink() = default;
        virtual void hit(const BlockTable &table, uint32_t block) = 0;
    };
    inline thread_local BlockSink *t_blockSink = nullptr;

    inline std::vector<const BlockTable *> &blockTables()
    {
        static std::vector<const BlockTable *> tables;
        return tables;
    }

    // One traced original's blocks (addresses in order) and its generated
    // file's digest; registers itself.
    struct BlockTable
    {
        BlockTable(uint32_t function_, game::Game game_, const uint32_t *starts_, uint32_t count_,
                   const char *original_)
            : function(function_), game(game_), starts(starts_), count(count_), original(original_)
        {
            blockTables().push_back(this);
        }
        uint32_t function;
        game::Game game;
        const uint32_t *starts;
        uint32_t count;
        const char *original; // the generated file's digest
    };

    inline void blockHit(const BlockTable &table, uint32_t block)
    {
        if (BlockSink *sink = t_blockSink)
            sink->hit(table, block);
    }

    inline const BlockTable *findBlocks(uint32_t function, game::Game game)
    {
        for (const BlockTable *t : blockTables())
            if (t->function == function && t->game == game)
                return t;
        return nullptr;
    }

    // Generated at build time (game.cmake): zx-fncheck only.
    struct StackArgs
    {
        uint8_t game;      // game::Game
        uint32_t function; // the callee
        uint16_t low, high; // bytes [low, high) above its entry sp that it reads
    };
    extern const StackArgs g_stackArgs[];
    extern const size_t g_stackArgCount;

    struct Digest
    {
        uint8_t game;
        uint32_t function;
        const char *digest;
    };
    extern const Digest g_readableDigests[];
    extern const size_t g_readableDigestCount;
}
