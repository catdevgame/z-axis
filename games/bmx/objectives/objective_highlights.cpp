// BMX XXX's objective highlights: a face, an edge or a level object an
// objective asks for, drawn glowing for a rider near it. The types'
// highlight callbacks call these (the manager's drawObjectiveHighlights
// has the engine visit every objective that is in progress). BMX XXX's own
// code (its game block); no build names these functions: the names are
// from what each one's code does.

#include "objective_calls.h"

#include "bmx/rider/collision/collision.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/render/material.h"
#include "zge/render/primitives.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/objective_highlights.cpp.h"

namespace game::bmx::objectives
{
    namespace collision = rider::collision;

    namespace
    {
        // The material the highlights are drawn with, and the frames drawn
        // so far (objectives.cpp makes the one and counts the other).
        inline ee::Global<ee::Ptr<zge::Material>, sym::g_highlightMaterial> g_highlightMaterial;
        inline ee::Global<uint32_t, sym::g_highlightFrame> g_highlightFrame;

        // A highlight shows within 25 units of the rider.
        constexpr ee::Float kNearSquared = 625.0f;
        // A highlighted face is drawn this far off the surface, along its normal.
        constexpr ee::Float kLift = 0.01f;
        // The primitive API's kind the highlights are drawn as (the engine
        // has no name for it yet).
        constexpr uint32_t kDrawKindTwo = 2;

        // The frames' fade: every 64 frames the highlight starts at 252
        // and loses 4 a frame, never under 64.
        constexpr uint32_t kFadeFrames = 0x3F;
        constexpr uint32_t kFadeLeast = 0x40;

        // The colour (0x00RRGGBB) at the fade's strength now, as a
        // vertex's four bytes: red, green, blue, and the strength.
        void fadedColour(uint8_t *rgba, uint32_t colour)
        {
            uint32_t strength = (kFadeFrames - (*g_highlightFrame & kFadeFrames)) << 2;
            if (!(kFadeFrames < strength))
                strength = kFadeLeast;
            strength &= 0xFF;
            rgba[3] = static_cast<uint8_t>(strength);
            rgba[0] = static_cast<uint8_t>((((colour >> 16) & 0xFF) * strength) >> 8);
            rgba[1] = static_cast<uint8_t>((((colour >> 8) & 0xFF) * strength) >> 8);
            rgba[2] = static_cast<uint8_t>(((colour & 0xFF) * strength) >> 8);
        }

        // A vertex's position as the quadword the original stores (x, y, z
        // and the word after them).
        zge::Vec4 &positionOf(ee::Ptr<zge::PrimitiveVertex> vertex)
        {
            return *vertex.cast<zge::Vec4>();
        }

        // ---- An edge's highlight ----
        //
        // An edge is drawn as a bar along it: four quads round the edge,
        // each from one end to the other. The bar's width and its strength
        // swell and shrink with the frames.
        constexpr uint32_t kBarSides = 4;
        constexpr uint32_t kQuadCorners = 4;
        constexpr uint32_t kSwellAnglePerFrame = 0x444; // a turn every 60 frames
        constexpr ee::Float kSwellStrength = 64.0f;    // the strength goes 64 .. 192
        constexpr uint32_t kSwellLeast = 0x40;
        constexpr ee::Float kSwellWidth = 0.0175f;     // the bar's half-width goes 0.05 .. 0.085
        constexpr ee::Float kLeastWidth = 0.05f;

        // A quad as drawPrimitiveQuads' callers hand it in.
        struct HighlightQuad
        {
            uint32_t texture;                                   // +0x00
            ee::Ptr<zge::PrimitiveVertex> corners[kQuadCorners]; // +0x04
        };
        static_assert(sizeof(HighlightQuad) == 0x14);
        // One line of the bar's wire frame: the function fills these and
        // draws none of them.
        struct WireLine
        {
            uint32_t texture;                   // +0x00
            ee::Ptr<zge::PrimitiveVertex> from; // +0x04
            ee::Ptr<zge::PrimitiveVertex> to;   // +0x08
        };
        static_assert(sizeof(WireLine) == 0x0C);
    }

    void drawFaceHighlight(collision::FaceRef face, uint32_t colour);
    void drawEdgeHighlight(collision::EdgeRef edge, uint32_t colour);
    void drawObjectHighlight(ee::Ptr<zge::SceneObject> object, uint32_t colour);
    uint32_t drawObjectFaceHighlight(uint32_t node, ee::Ptr<collision::NodeFace> face, ee::Ptr<uint8_t> rgba);

    // highlightFace: the face drawn in the colour for a rider at `from`,
    // when its node is drawn and its centre is within 25 units.
    void highlightFace(uint64_t face, ee::Address from, uint32_t colour)
    {
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::Vec4> offset;
        if (zge::nodeNotDrawn(ee::Ptr<zge::Node>(static_cast<uint32_t>(collision::refNode(face)))) != 0)
            return;
        collision::faceCentre(face, &centre->x);
        *offset = *centre - *ee::Ptr<zge::Vec4>(from);
        if (zge::squaredLength(*offset) < kNearSquared)
            drawFaceHighlight(face, colour);
    }
    READABLE_FUNCTION_WAITS(highlightFace, Draft);

    // highlightEdge: the edge drawn in the colour for a rider at `from`,
    // when its node is drawn and its middle is within 25 units.
    void highlightEdge(uint64_t edge, ee::Address from, uint32_t colour)
    {
        ee::Local<zge::Vec4> middle;
        ee::Local<zge::Vec4> offset;
        if (zge::nodeNotDrawn(ee::Ptr<zge::Node>(static_cast<uint32_t>(collision::refNode(edge)))) != 0)
            return;
        collision::edgePoint(edge, 0, &middle->x);
        collision::edgePoint(edge, 1, &offset->x);
        const ee::Float halfX = (offset->x - middle->x) * ee::Float(0.5f);
        const ee::Float halfY = (offset->y - middle->y) * ee::Float(0.5f);
        const ee::Float halfZ = (offset->z - middle->z) * ee::Float(0.5f);
        middle->x = middle->x + halfX;
        middle->y = middle->y + halfY;
        middle->z = middle->z + halfZ;
        *offset = *middle - *ee::Ptr<zge::Vec4>(from);
        if (zge::squaredLength(*offset) < kNearSquared)
            drawEdgeHighlight(edge, colour);
    }
    READABLE_FUNCTION_WAITS(highlightEdge, Verified);

    // highlightObject: the level object drawn in the colour for a rider at
    // `from`, when its scene is drawn and it is within 25 units.
    void highlightObject(ee::Ptr<zge::SceneObject> object, ee::Address from, uint32_t colour)
    {
        ee::Local<zge::Vec4> at;
        ee::Local<zge::Vec4> offset;
        if (zge::nodeNotDrawn(zge::sceneRootNode(zge::sceneOf(object))) != 0)
            return;
        *at = *zge::objectPosition(object);
        *offset = *at - *ee::Ptr<zge::Vec4>(from);
        if (zge::squaredLength(*offset) < kNearSquared)
            drawObjectHighlight(object, colour);
    }
    READABLE_FUNCTION_WAITS(highlightObject, Draft);

    // drawFaceHighlight: the face as one triangle of the highlight
    // material in the faded colour, lifted a little off the surface along
    // the face's normal. (The lift's fourth lane is not set: each vertex's
    // word after its position gets the corner's plus what the frame held.)
    void drawFaceHighlight(collision::FaceRef face, uint32_t colour)
    {
        ee::Local<zge::PrimitiveTriangle> triangle;
        ee::Local<zge::PrimitiveVertex[3]> vertices;
        ee::Local<zge::Vec4> lift;
        ee::Local<uint8_t[4]> rgba;
        ee::Local<zge::Vec4> normal;
        ee::Local<zge::Vec4> corner;
        fadedColour(&rgba[0], colour);
        collision::faceNormal(face, &normal->x);
        lift->x = normal->x * kLift;
        lift->y = normal->y * kLift;
        lift->z = normal->z * kLift;
        for (uint32_t i = 0; i < 3; ++i)
        {
            collision::faceVertex(face, i, &corner->x);
            const ee::Ptr<zge::PrimitiveVertex> vertex(vertices.address() + i * sizeof(zge::PrimitiveVertex));
            for (uint32_t byte = 0; byte < 4; ++byte)
                vertex->rgba[byte] = rgba[byte];
            positionOf(vertex) = *corner + *lift;
            triangle->vertices[i] = vertex;
        }
        triangle->texture = ee::Address(*g_highlightMaterial);
        zge::drawPrimitiveStrips(kDrawKindTwo, triangle, 1, ee::Address());
    }
    READABLE_FUNCTION_WAITS(drawFaceHighlight, Draft);

    // drawObjectHighlight: every face of the object's collision mesh
    // drawn in the faded colour.
    void drawObjectHighlight(ee::Ptr<zge::SceneObject> object, uint32_t colour)
    {
        ee::Local<uint8_t[4]> rgba;
        fadedColour(&rgba[0], colour);
        collision::forEachNodeFace(collision::widen(ee::Address(zge::objectLevelNode(object))), collision::NodeFaceVisit(sym::drawObjectFaceHighlight), rgba);
    }
    READABLE_FUNCTION_WAITS(drawObjectHighlight, Draft);

    // drawObjectFaceHighlight (drawObjectHighlight's visit of a face): the
    // face as one triangle of the highlight material in the colour, lifted
    // a little along its normal. Always 1: the visit goes on.
    uint32_t drawObjectFaceHighlight(uint32_t node, ee::Ptr<collision::NodeFace> face, ee::Ptr<uint8_t> rgba)
    {
        (void)node;
        ee::Local<zge::PrimitiveTriangle> triangle;
        ee::Local<zge::PrimitiveVertex[3]> vertices;
        ee::Local<zge::Vec4> lift;
        *lift = face->normal * kLift;
        for (uint32_t i = 0; i < 3; ++i)
        {
            const ee::Ptr<zge::PrimitiveVertex> vertex(vertices.address() + i * sizeof(zge::PrimitiveVertex));
            positionOf(vertex) = face->corners[i].position + *lift;
            for (uint32_t byte = 0; byte < 4; ++byte)
                vertex->rgba[byte] = rgba[byte];
            triangle->vertices[i] = vertex;
        }
        triangle->texture = ee::Address(*g_highlightMaterial);
        zge::drawPrimitiveStrips(kDrawKindTwo, triangle, 1, ee::Address());
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawObjectFaceHighlight, Draft);

    // drawEdgeHighlight: the edge as a bar of the highlight material in
    // the colour. The bar's cross-section is a square round the edge, in
    // the frame of the edge's direction and its plane's normal (straight
    // along the third axis when the edge has no plane); each of its four
    // sides is a quad from the edge's first end to its second, with the
    // texture running once across it.
    void drawEdgeHighlight(collision::EdgeRef edge, uint32_t colour)
    {
        ee::Local<zge::Vec4> corner;
        ee::Local<zge::Vec4> turned;
        ee::Local<HighlightQuad[kBarSides]> quads;
        ee::Local<WireLine[kBarSides * kQuadCorners]> wire;
        ee::Local<zge::PrimitiveVertex[kBarSides * kQuadCorners]> vertices;
        ee::Local<zge::Matrix> frame;
        ee::Local<uint8_t[4]> rgba;
        ee::Local<zge::Vec4> plane;
        ee::Local<zge::Vec4> end;

        const zge::Angle swell = static_cast<zge::Angle>(*g_highlightFrame * kSwellAnglePerFrame);
        const uint32_t strength = static_cast<uint32_t>(ee::toInt((zge::angleSin(swell) + ee::Float(1.0f)) * kSwellStrength) + static_cast<int32_t>(kSwellLeast)) & 0xFF;
        rgba[3] = static_cast<uint8_t>(strength);
        rgba[0] = static_cast<uint8_t>((((colour >> 16) & 0xFF) * strength) >> 8);
        rgba[1] = static_cast<uint8_t>((((colour >> 8) & 0xFF) * strength) >> 8);
        rgba[2] = static_cast<uint8_t>(((colour & 0xFF) * strength) >> 8);

        zge::Vec4 &across = zge::matrixRow(*frame, 0);
        zge::Vec4 &normal = zge::matrixRow(*frame, 1);
        zge::Vec4 &along = zge::matrixRow(*frame, 2);
        collision::edgeDirection(edge, &along.x);
        if (collision::edgePlane(edge, &plane->x) != 0)
            normal = *plane;
        else
        {
            normal.x = 0.0f;
            normal.y = 0.0f;
            normal.z = 1.0f;
        }
        across = zge::cross(normal, along);
        zge::matrixRow(*frame, 3) = zge::kZero;

        const ee::Float halfWidth = (zge::angleSin(swell) + ee::Float(1.0f)) * kSwellWidth + kLeastWidth;
        const uint32_t material = ee::Address(*g_highlightMaterial);
        for (uint32_t side = 0; side < kBarSides; ++side)
        {
            for (uint32_t i = 0; i < kQuadCorners; ++i)
            {
                // The corner's place in the square, and which end it is at:
                // corners 0 and 1 at the first end, 2 and 3 at the second;
                // corners 1 and 2 on the side's other edge.
                const bool atSecondEnd = (i & 2) != 0;
                const bool otherEdge = ((i + 1) & 2) != 0;
                const bool farSide = (side & 2) != 0;
                corner->z = atSecondEnd ? halfWidth : -halfWidth;
                if ((side & 1) == 0)
                {
                    corner->y = otherEdge ? -halfWidth : halfWidth;
                    corner->x = farSide ? halfWidth : -halfWidth;
                }
                else
                {
                    corner->y = farSide ? halfWidth : -halfWidth;
                    corner->x = otherEdge ? -halfWidth : halfWidth;
                }
                collision::edgePoint(edge, atSecondEnd ? 1 : 0, &end->x);
                *turned = zge::transformed(*frame, *corner);
                zge::PrimitiveVertex &vertex = vertices[side * kQuadCorners + i];
                positionOf(ee::Ptr<zge::PrimitiveVertex>(vertices.address() + (side * kQuadCorners + i) * sizeof(zge::PrimitiveVertex))) = *turned + *end;
                for (uint32_t byte = 0; byte < 4; ++byte)
                    vertex.rgba[byte] = rgba[byte];
                vertex.u = otherEdge ? 1.0f : 0.0f;
                vertex.v = atSecondEnd ? 1.0f : 0.0f;
                const ee::Ptr<zge::PrimitiveVertex> here(vertices.address() + (side * kQuadCorners + i) * sizeof(zge::PrimitiveVertex));
                const ee::Ptr<zge::PrimitiveVertex> next(vertices.address() + (side * kQuadCorners + ((i + 1) & 3)) * sizeof(zge::PrimitiveVertex));
                WireLine &line = wire[side * kQuadCorners + i];
                line.from = here;
                line.to = next;
                line.texture = material;
                quads[side].corners[i] = here;
            }
            quads[side].texture = material;
        }
        zge::drawPrimitiveQuads(kDrawKindTwo, quads, kBarSides, ee::Address());
    }
    READABLE_FUNCTION_WAITS(drawEdgeHighlight, Verified);
}
