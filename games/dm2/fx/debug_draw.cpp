// DM2's debug draws (debug_draw.h). DM2's own code (fx.c).

#include "debug_draw.h"

#include "particles.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/vec.h"
#include "zge/render/primitives.h"

#include <cstdint>
#include "sym/dm2/fx/debug_draw.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The marker's four points (one down, three level above it).
        inline ee::Global<zge::Vec4[4], sym::g_markerPoints> g_markerPoints;
        // Frames the effects' second task has run.
        inline ee::Global<int32_t, sym::g_effectsFrames> g_effectsFrames;
        // The marker's turn a frame, in turns (a 120th), and an angle's units a turn.
        constexpr ee::Float kMarkerTurnAFrame = 0.008333334f;
        constexpr ee::Float kAngleUnitsPerTurn = 65536.0f;
        // The primitive API's kind: in the world.
        constexpr uint32_t kWorldPrimitive = 2;

        // A triangle of the primitive API: its material and corners.
        struct Triangle
        {
            uint32_t material;
            ee::Ptr<QuadVertex> corners[3];
        };
        static_assert(sizeof(Triangle) == 0x10);
    }

    void debugDrawAxes(ee::Ptr<zge::Vec4> position, ee::Ptr<uint8_t> colour, ee::Float size)
    {
        ee::Local<Triangle[4]> sides;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<zge::Matrix> place;
        ee::Local<zge::Vec4[4]> points;
        for (int i = 0; i < 4; ++i)
            points[i] = (*g_markerPoints)[i];
        // Each side: three of the four points, from its own on.
        for (uint32_t side = 0; side < 4; ++side)
        {
            for (uint32_t corner = 0; corner < 3; ++corner)
                sides[side].corners[corner] = ee::Ptr<QuadVertex>(vertices) + ((side + corner) & 3);
            sides[side].material = 0;
        }
        for (uint32_t i = 0; i < 4; ++i)
        {
            QuadVertex &vertex = vertices[i];
            for (int byte = 0; byte < 4; ++byte)
                vertex.colour[byte] = colour[byte];
            vertex.u = 0.0f;
            vertex.v = 0.0f;
            // Scaffolding (t0-leftover): the factor reaches VU0 through a register whose
            // upper half is still the caller's (nothing here is called before).
            vertex.position = zge::scaledCarrying(points[i], size, zge::scaleLeftover(zge::LeftBy::caller));
        }
        zge::matrixIdentity(place);
        const ee::Float turns = ee::toFloat(*g_effectsFrames) * kMarkerTurnAFrame;
        zge::matrixRotateY(place, place, static_cast<zge::Angle>(ee::toInt(turns * kAngleUnitsPerTurn)));
        place->translation() = zge::aligned(position);
        zge::drawPrimitiveStrips(kWorldPrimitive, sides, 4, place);
    }
    READABLE_FUNCTION_WAITS(debugDrawAxes, Draft);

    void inlineFxNoopD(ee::Float, const ee::Float *, const uint8_t *)
    {
    }
    READABLE_FUNCTION(inlineFxNoopD, Draft);

    void fxStub()
    {
    }
    READABLE_FUNCTION(fxStub, Draft);

    void fxStubB()
    {
    }
    READABLE_FUNCTION(fxStubB, Draft);
}
