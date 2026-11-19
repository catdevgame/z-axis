/* Minimal types for the feasibility functions. */
typedef struct { float x, y, z, w; } Vec4;
/* 16-byte aligned vector (copied with lq/sq), like sceVu0FVECTOR */
typedef struct { float x, y, z, w; } __attribute__((aligned(16))) QVec4;
