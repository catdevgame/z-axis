#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void resetMpegStubState();
    void enqueueMpegDecodedFrameForTesting(uint32_t mpegAddr);
    size_t mpegDecodedFrameCountForTesting(uint32_t mpegAddr);
    void notifyMpegCdStreamStart(PS2Machine *runtime = nullptr);
    void notifyMpegCdStreamDataProduced(uint32_t byteCount, bool endOfStream);
    void notifyMpegCdStreamEof(PS2Machine *runtime = nullptr);
    void sceMpegFlush(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegAddBs(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegAddCallback(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegAddStrCallback(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegClearRefBuff(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegCreate(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegDelete(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegDemuxPss(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegDemuxPssRing(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegDispCenterOffX(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegDispCenterOffY(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegDispHeight(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegDispWidth(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegGetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegGetPicture(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegGetPictureRAW8(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegGetPictureRAW8xy(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegIsEnd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegIsRefBuffEmpty(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegReset(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegResetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegSetDecodeMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegSetDefaultPtsGap(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMpegSetImageBuff(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
