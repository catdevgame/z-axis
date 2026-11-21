#pragma once

// Free-form deformation lattices, in EE RAM (zge/scene/ffd.cpp).

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // A lattice: countX by countY by countZ points, x fastest, then y, then
    // z: point (x, y, z) is points[x + y * countX + z * countX * countY].
    // A point keeps x, y and z (w is 0 from the clear, and nothing reads it
    // but the deformation's quadword loads).
    struct FfdLattice
    {
        uint32_t countX;        // +0x00
        uint32_t countY;        // +0x04
        uint32_t countZ;        // +0x08
        ee::Ptr<Vec4> points;   // +0x0C
    };
    static_assert(sizeof(FfdLattice) == 0x10 && offsetof(FfdLattice, points) == 0x0C);

    // A lattice cell's eight corners, numbered x + 2 y + 4 z (each 0 or 1).
    constexpr uint32_t kFfdCellCorners = 8;

    // A vertex's place in a lattice: the eight points of the cell it lies
    // in (by corner number) and how much each moves it.
    struct FfdVertexCell
    {
        uint32_t point[kFfdCellCorners];    // +0x00 the lattice's point index
        ee::Float weight[kFfdCellCorners];  // +0x20 summing to 1
    };
    static_assert(sizeof(FfdVertexCell) == 0x40 && offsetof(FfdVertexCell, weight) == 0x20);

    // A part's vertices bound to a lattice (bindPartToFfd).
    struct FfdVertexWeights
    {
        uint32_t vertexCount;              // +0x00
        ee::Ptr<FfdVertexCell> vertices;   // +0x04 one per vertex
    };
    static_assert(sizeof(FfdVertexWeights) == 0x08);
}

namespace zge
{
    // (ffd.cpp)
    uint32_t registerZffdChunkHandler();
}
