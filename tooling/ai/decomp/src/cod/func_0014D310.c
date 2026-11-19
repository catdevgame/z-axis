#include "common.h"

/* Negate xyz, keep w. Returns dst. */
Vec4 *func_0014D310(Vec4 *dst, Vec4 *src)
{
    dst->x = -src->x;
    dst->y = -src->y;
    dst->z = -src->z;
    dst->w = src->w;
    return dst;
}
