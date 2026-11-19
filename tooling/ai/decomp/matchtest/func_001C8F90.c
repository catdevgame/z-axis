#include "common.h"
extern char D_002796A0;
extern float D_0027748C;
extern QVec4 D_002796B0;

/* ZGE-style 128-bit copy through $t0 (inline asm, as used by the VU0 helpers) */
#define QCOPY(dst, src) \
    __asm__ __volatile__("lq $8, 0(%1)\n\tsq $8, 0(%0)" : : "r"(dst), "r"(src) : "$8", "memory")

void func_001C8F90(QVec4 *v)
{
    D_002796A0 = 3;
    D_0027748C = 1.0f;
    QCOPY(&D_002796B0, v);
    D_002796B0.y += 3.0f;
}
