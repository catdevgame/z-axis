#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void sceeFontInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceeFontLoadFont(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceeFontGenerateString(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceeFontPrintfAt(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceeFontPrintfAt2(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceeFontClose(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceeFontSetColour(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceeFontSetMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceeFontSetFont(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceeFontSetScale(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
