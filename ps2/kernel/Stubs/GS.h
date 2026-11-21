#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void sceGifPkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkAddGsData(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkCnt(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkEnd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkRef(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkRefLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkReset(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkReserve(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGifPkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsExecLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsExecStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsGetGParam(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsPutDispEnv(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsPutDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsResetGraph(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsResetPath(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSetDefClear(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSetDefDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSetDefDBuff(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSetDefDispEnv(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSetDefDrawEnv(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSetDefDrawEnv2(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSetDefLoadImage(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSetDefStoreImage(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSwapDBuffDc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSwapDBuff(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSyncPath(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSyncV(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGsSyncVCallback(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceGszbufaddr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void Ps2SwapDBuff(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkAddGsAD(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkAlign(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkCall(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkCloseDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkCloseGifTag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkCnt(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkEnd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkOpenDirectCode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkOpenGifTag(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkReset(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkReserve(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceVif1PkTerminate(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
