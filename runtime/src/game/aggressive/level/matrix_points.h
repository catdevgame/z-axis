#pragma once

// A point or a direction through a matrix, as the level's objects use them
// (zge/math/vec.h's transformed and rotated: the result's w is what VU0's
// last result held there, not a product).

#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"

namespace aggressive::level
{
    inline zge::Vec4 throughMatrix(const zge::Matrix &matrix, const zge::Vec4 &point)
    {
        return zge::transformed(matrix, point);
    }

    // A vector's length, its w ignored.
    inline ee::Float lengthOf(ee::Ptr<zge::Vec4> vector)
    {
        return zge::vec3Length(&vector->xyz());
    }

    // The rotation alone, no translation.
    inline zge::Vec4 throughRotation(const zge::Matrix &matrix, const zge::Vec4 &direction)
    {
        return zge::rotated(matrix, direction);
    }
}
