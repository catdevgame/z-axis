#pragma once
// Declarations only (README.txt).
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t size);
void free(void *pointer);
void abort(void);
int posix_memalign(void **pointer, size_t alignment, size_t size);
#ifdef __cplusplus
}
#endif
