#pragma once

// The blocks of the traced originals that end with a JR through a jump table
// the recompiler turned into a switch (tools/fncheck_traced_gen.py, in the
// generated fncheck_stack_args.cpp): the whole function runs the case in
// place, but from an entry block a target outside it is dispatchGuestBranch's
// IndirectJump, 8 cycles more than the scheduler's dispatch of the target
// (zx-fncheck's SliceEmu; the readable side is ee::tableJump, ee/dispatch.h;
// drafter23). Kept out of fncheck_tables.h, which readable code includes.

#include <cstddef>
#include <cstdint>

namespace fncheck
{
    struct TableJump
    {
        uint8_t game;
        uint32_t function;
        uint32_t block; // the start of the block that ends with the JR
    };
    extern const TableJump g_tableJumps[];
    extern const size_t g_tableJumpCount;
}
