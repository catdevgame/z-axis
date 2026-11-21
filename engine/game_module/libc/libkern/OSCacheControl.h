#pragma once
// Declarations only (README.txt): sse2neon.h's _mm_clflush on Apple targets.
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void sys_dcache_flush(void *start, size_t length);
void sys_icache_invalidate(void *start, size_t length);
#ifdef __cplusplus
}
#endif
