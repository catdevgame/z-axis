// A vector turned by a matrix's rows, AI's own (its game block; no other game
// has it): moved from zge/math/matrix.cpp, where it sat as engine code
// (layer2). Nothing calls it.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"

#include <cstdint>
#include "sym/aggressive/matrix_rows.cpp.h"

namespace aggressive
{
    using namespace zge;

    // transformVecByRows: v's x, y and z replaced by the matrix's first three
    // rows scaled by them and summed (v turned by the matrix's rotation, as a
    // row vector). Nothing calls it.
    // Each row is scaled in a stack copy; the factor reaches VU0 through an
    // integer register that last held the third row's upper half (its z and
    // w), which so lands in VU0's work register as in the original.
    void transformVecByRows(ee::Ptr<const Matrix> matrix, ee::Ptr<Vec3> v)
    {
        ee::Local<Vec4[3]> scaled;
        const Vec4 &third = matrix->row(2);
        const uint64_t carried = static_cast<uint64_t>(third.w.bits()) << 32 | third.z.bits();
        scaled[0] = scaledCarrying(matrix->row(0), v->x, carried);
        scaled[1] = scaledCarrying(matrix->row(1), v->y, carried);
        scaled[2] = scaledCarrying(third, v->z, carried);
        const ee::Float y = scaled[0].y + scaled[1].y + scaled[2].y;
        const ee::Float z = scaled[0].z + scaled[1].z + scaled[2].z;
        v->x = scaled[0].x + scaled[1].x + scaled[2].x;
        v->y = y;
        v->z = z;
    }
    READABLE_FUNCTION(transformVecByRows, Draft);
}
