#pragma once

// BMX XXX (as DM2) hands its vectors round as their four floats (the PS2 SDK's
// sceVu0FVECTOR: a quadword-aligned float[4]), the same bytes as a
// zge::Vec4: vec4() is that vector, for zge/math/vec.h's operations. Its
// matrices are sixteen floats (sceVu0FMATRIX: rows, the last the position),
// a zge::Matrix (matrix()), and its angles three 16-bit turns, a
// zge::EulerAngles (eulerAngles()).

#include "ee/ee.h"
#include "zge/math/types.h"

namespace game::bmx::rider
{
    inline zge::Vec4 *vec4(ee::Float *lanes) { return reinterpret_cast<zge::Vec4 *>(lanes); }
    inline const zge::Vec4 *vec4(const ee::Float *lanes) { return reinterpret_cast<const zge::Vec4 *>(lanes); }
    inline zge::Matrix *matrix(ee::Float *rows) { return reinterpret_cast<zge::Matrix *>(rows); }
    inline const zge::Matrix *matrix(const ee::Float *rows) { return reinterpret_cast<const zge::Matrix *>(rows); }
    inline const zge::EulerAngles *eulerAngles(const int16_t *angles) { return reinterpret_cast<const zge::EulerAngles *>(angles); }
    inline zge::EulerAngles *eulerAngles(int16_t *angles) { return reinterpret_cast<zge::EulerAngles *>(angles); }
    inline zge::Quaternion *quaternion(ee::Float *lanes) { return reinterpret_cast<zge::Quaternion *>(lanes); }
}
