#ifndef COMMON_H
#define COMMON_H

typedef struct { float x, y, z, w; } Vec4;
/* 16-byte aligned vector (copied with lq/sq), like sceVu0FVECTOR */
typedef struct { float x, y, z, w; } __attribute__((aligned(16))) QVec4;

#endif
