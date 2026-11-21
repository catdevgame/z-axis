// A function the recompiler wrote as two entry_ chunks, built as game.cmake
// builds a traced original of several chunks (ee/fncheck_traced.h,
// FNCHECK_TRACED_CHUNKS), for the recorder's test in readable_tests.cpp.
//
// twoChunks (0x4E00 .. 0x4E28), the shape of AI's lowerRegionTop: its first
// chunk (0x4E00 .. 0x4E18) makes a frame, keeps ra, calls 0x6100 with a0
// (return point 0x4E14, a resume entry of the chunk), and goes on to the
// second chunk either way: by a branch when v0 is not 0, else by falling
// through after writing 1 to 0x5600. The second (0x4E18 .. 0x4E28) writes
// v0 + 1 to 0x5604, takes the frame down and returns (jr ra). In the game
// each chunk returns with pc at the second, which the scheduler dispatches.
#include "ee/fncheck_traced.h"

namespace fncheck_test
{
    void entry_00004e00_0x4e00(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        switch (ctx->pc)
        {
        case 0x4e14u: goto label_4e14;
        default: break;
        }
        // addiu $sp, $sp, -0x10; sw $ra, 0x0($sp)
        SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 4294967280u));
        WRITE32(ADD32(GPR_U32(ctx, 29), 0), GPR_U32(ctx, 31));
        // jal 0x6100 (at 0x4E0C)
        SET_GPR_U32(ctx, 31, 0x4E14u);
        ctx->pc = 0x6100u;
        if (!runtime->dispatchGuestBranch(rdram, ctx, 0x6100u, 0x4E0Cu, 0x4E14u, PS2Runtime::GuestBranchKind::DirectCall,
                                          "JAL"))
            return;
    label_4e14:
        // bnez $v0, 0x4E18
        if (GPR_U32(ctx, 2) != 0u)
        {
            ctx->pc = 0x4E18u;
            return;
        }
        // addiu $v1, $zero, 1; sw $v1, 0x5600($zero)
        SET_GPR_S32(ctx, 3, 1);
        WRITE32(0x5600u, GPR_U32(ctx, 3));
        ctx->pc = 0x4e18u;
    }

    void entry_00004e18_0x4e18(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // addiu $v1, $v0, 1; sw $v1, 0x5604($zero)
        SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 2), 1u));
        WRITE32(0x5604u, GPR_U32(ctx, 3));
        // lw $ra, 0x0($sp); addiu $sp, $sp, 0x10; jr $ra
        SET_GPR_S32(ctx, 31, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 0)));
        SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 16u));
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        (void)runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x4E20u, 0u, PS2Runtime::GuestBranchKind::Return,
                                           "JR $ra");
    }

    FNCHECK_TRACED_CHUNKS(twoChunksTraced,
                          {0x4E00u, 0x4E18u, &entry_00004e00_0x4e00},
                          {0x4E18u, 0x4E28u, &entry_00004e18_0x4e18})

    // The whole function's traced original, and its first chunk alone (the
    // traced original before chunks were followed). (PS2Runtime is the traced
    // runtime in this file, fncheck_traced.h: the type by its base.)
    ::fncheck::TracedRuntime::RecompiledFunction twoChunksWhole() { return &::fncheck::tracedEntry<&twoChunksTraced>; }
    ::fncheck::TracedRuntime::RecompiledFunction twoChunksFirstChunk() { return &::fncheck::tracedEntry<&entry_00004e00_0x4e00>; }
    // The second chunk alone: what the game's table has at its address.
    ::fncheck::TracedRuntime::RecompiledFunction twoChunksSecondChunk() { return &::fncheck::tracedEntry<&entry_00004e18_0x4e18>; }
}
