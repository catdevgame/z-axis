// BMX XXX's spinning marker (marker.h). BMX XXX's own code (fx.c's block);
// DM2 has the same function.

#include "marker.h"
#include "particles.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/render/primitives.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/marker.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The marker's four points: the tip, a unit below the middle, and
        // three above it.
        inline ee::Global<zge::Vec4[4], sym::markerPoints> g_markerPoints;
        // Frames the effects' second task has run.
        inline ee::Global<int32_t, sym::g_effectsFrames> g_effectsFrames;
        // A turn in 120 frames.
        constexpr ee::Float kTurnsPerFrame = 1.0f / 120.0f;
        constexpr ee::Float kAnglePerTurn = 65536.0f;
        // drawPrimitiveStrips' flags for triangles in the world, through a matrix.
        constexpr uint32_t kWorldTriangles = 2;
    }

    // drawSpinningMarker: see marker.h. Four untextured triangles, each on
    // three of the four points in turn.
    void drawSpinningMarker(ee::Ptr<zge::Vec4> position, ee::Ptr<uint8_t> colour, ee::Float size)
    {
        ee::Local<zge::PrimitiveTriangle[4]> triangles;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<zge::Matrix> placing;
        ee::Local<zge::Vec4[4]> points;
        std::memcpy(&points[0], &(*g_markerPoints)[0], sizeof(zge::Vec4[4]));
        for (uint32_t triangle = 0; triangle < 4; ++triangle)
        {
            for (uint32_t corner = 0; corner < 3; ++corner)
                triangles[triangle].vertices[corner] = primitiveVertexOf(&vertices[(triangle + corner) & 3]);
            triangles[triangle].texture = 0;
        }
        for (uint32_t point = 0; point < 4; ++point)
        {
            QuadVertex &vertex = vertices[point];
            std::memcpy(vertex.colour, colour.get(), sizeof(vertex.colour));
            vertex.u = 0.0f;
            vertex.v = 0.0f;
            vertex.position = points[point] * size;
        }
        zge::matrixIdentity(placing);
        const ee::Float turns = ee::toFloat(*g_effectsFrames) * kTurnsPerFrame;
        zge::matrixRotateY(placing, placing, static_cast<zge::Angle>(ee::toInt(turns * kAnglePerTurn)));
        placing->translation() = zge::aligned(position);
        zge::drawPrimitiveStrips(kWorldTriangles, triangles.address(), 4, placing.address());
    }
    READABLE_FUNCTION_WAITS(drawSpinningMarker, Draft);
}
