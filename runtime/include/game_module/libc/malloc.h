#pragma once
// Declarations only (README.txt): mm_malloc.h's on Windows.
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *_aligned_malloc(size_t size, size_t alignment);
void _aligned_free(void *pointer);
#ifdef __cplusplus
}
#endif
