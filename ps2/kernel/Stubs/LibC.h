#pragma once

#include "stubs.h"

namespace ps2_stubs
{
    void malloc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void memalign(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void free(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void calloc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void realloc(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void memcpy(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void memset(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void memclr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void memmove(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void memcmp(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strcpy(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strncpy(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strlen(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strcmp(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strncmp(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strcat(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strncat(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strchr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strrchr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strstr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void printf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sprintf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void snprintf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void puts(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fopen(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fclose(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fread(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fwrite(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fprintf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fseek(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ftell(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fflush(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sqrt(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sin(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void __kernel_sinf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void cos(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void __kernel_cosf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void __ieee754_rem_pio2f(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void tan(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void atan2(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void pow(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void exp(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void log(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void log10(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void ceil(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void floor(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void fabs(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void abs(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void atan(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void memchr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void rand(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void srand(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void strcasecmp(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void vfprintf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void vsprintf(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void __divdi3(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
