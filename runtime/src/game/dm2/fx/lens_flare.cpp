// DM2's lens flare (lens_flare.h). DM2's own code (SpecialFX/zfxMgr.c), the
// same as Aggressive Inline's (game/aggressive/fx/lens_flare.cpp): newLensFlare,
// setupLensFlareElements and drawLensFlareElement word for word. Two differ
// with the engine: the sight test asks ZGE2's scene query, and the frame
// transforms the sun with the engine's point transform, a call in ZGE2.

#include "lens_flare.h"

#include "dm2/rider/collision/collision.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/render/primitives.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/dm2/fx/lens_flare.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The sun's line of sight is tested every 15th frame; between tests the
        // last answer holds.
        inline ee::Global<uint32_t, sym::g_lensFlareFrame> g_lensFlareFrame;
        inline ee::Global<int32_t, sym::g_lensFlareHidden> g_lensFlareHidden;
        constexpr uint32_t kSightTestInterval = 15;
        constexpr int32_t kSightSteps = 10;

        // Scaffolding (address-call): ZGE2's scene query (a shape against the
        // scene: kind 1 a segment) is DM2's engine's own and has no readable
        // body.
        constexpr uint32_t kSegmentQuery = 1;
        int32_t sceneQuery(uint32_t kind, ee::Ptr<zge::QuerySegment> shape, uint32_t flags)
        {
            return ee::game<int32_t>(sym::sceneQuery, kind, shape, flags, 0u, 0u, 0u, 0u);
        }

        // A vertex of the primitive API's quads.
        struct Vertex
        {
            ee::Float x, y, z;
            uint32_t _0c;
            ee::Float u, v;
            uint8_t colour[4];
            uint32_t _1c;
        };
        static_assert(sizeof(Vertex) == 0x20 && offsetof(Vertex, colour) == 0x18);

        // A textured quad: its texture and its corners.
        struct QuadRecord
        {
            uint32_t texture;
            ee::Ptr<Vertex> corners[4];
        };
        static_assert(sizeof(QuadRecord) == 0x14);

        // An element's half width and half height on screen.
        struct FlareExtent
        {
            ee::Float x, y;
        };

        constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }

        // A size field (a whole number) halved, as a float.
        ee::Float half(uint32_t size) { return ee::toFloat(static_cast<int32_t>(size >> 1)); }
    }

    ee::Ptr<LensFlare> newLensFlare()
    {
        const ee::Ptr<LensFlare> flare(zge::heapAllocate(sizeof(LensFlare)));
        flare->elementCount = 9;
        flare->elementScaleY = 0x100;
        flare->glow = 0;
        flare->ring1 = 0;
        flare->ring2 = 0;
        flare->ray = 0;
        flare->star = 0;
        flare->unusedSlot = 0;
        flare->elementScaleX = 0x100;
        return flare;
    }
    READABLE_FUNCTION_WAITS(newLensFlare, Verified);

    namespace
    {
        // The nine elements: where, how big, what colour, and glow or ring.
        struct ElementLook
        {
            float distance, scale;
            uint8_t colour[4];
            bool ring;
        };
        constexpr ElementLook kElementLooks[9] = {
            {1.6f, 1.6f, {205, 205, 205, 145}, false},
            {0.7f, 1.2f, {155, 200, 200, 55}, false},
            {0.65f, 0.8f, {205, 205, 235, 155}, true},
            {0.2f, 0.2f, {205, 205, 255, 100}, false},
            {-0.3f, 0.56f, {115, 105, 105, 100}, true},
            {-0.25f, 1.0f, {105, 105, 255, 100}, true},
            {-0.45f, 0.48f, {205, 215, 255, 50}, false},
            {-0.9f, 0.56f, {215, 235, 255, 100}, true},
            {-1.0f, 0.92f, {255, 215, 235, 100}, false},
        };
    }

    void setupLensFlareElements(ee::Ptr<LensFlare> flare)
    {
        for (int i = 0; i < 9; ++i)
        {
            const ElementLook &look = kElementLooks[i];
            LensFlareElement &element = flare->elements[i];
            element.distance = ee::Float(look.distance);
            element.scale = ee::Float(look.scale);
            std::memcpy(element.colour, look.colour, sizeof(element.colour));
            element.texture = look.ring ? flare->ring2 : flare->glow;
        }
    }
    READABLE_FUNCTION(setupLensFlareElements, Verified);

    // Every 15th frame the line from the eye to the sun is tested in ten
    // pieces, from the eye, until one hits something.
    int32_t lensFlareHidden(ee::Ptr<zge::Vec4> sun, ee::Ptr<zge::Vec4> eye)
    {
        if (*g_lensFlareFrame % kSightTestInterval == 0)
        {
            ee::Local<zge::QuerySegment> piece;
            piece->end = *sun;
            piece->start = *eye;
            const zge::Vec4 step = (piece->end - piece->start) * ee::Float(0.1f);
            piece->end = piece->start + step;
            *g_lensFlareHidden = 0;
            for (int32_t left = kSightSteps; left > 0; --left)
            {
                *g_lensFlareHidden = sceneQuery(kSegmentQuery, piece, zge::kQueryWorldPoints);
                piece->start = piece->start + step;
                piece->end = piece->start + step;
                if (*g_lensFlareHidden != 0)
                    break;
            }
        }
        *g_lensFlareFrame = *g_lensFlareFrame + 1;
        return *g_lensFlareHidden;
    }
    READABLE_FUNCTION_WAITS(lensFlareHidden, Verified);

    // One element: a quad of four, `extent` out from `centre` each way, the
    // texture mirrored about the middle (its middle at the quad's middle, its
    // corner at the quad's edges), in the camera's space.
    void drawLensFlareElement(ee::Ptr<zge::Vec4> centre, ee::Ptr<FlareExtent> extent, uint32_t texture, uint32_t colour, ee::Ptr<zge::Matrix> camera)
    {
        ee::Local<uint32_t> colourWord;
        ee::Local<Vertex[9]> grid; // three rows of three, from the low y
        ee::Local<QuadRecord> quad;
        *colourWord = colour;
        const ee::Float xs[3] = {centre->x - extent->x, centre->x, centre->x + extent->x};
        const ee::Float ys[3] = {centre->y - extent->y, centre->y, centre->y + extent->y};
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                Vertex &vertex = grid[row * 3 + column];
                vertex.x = xs[column];
                vertex.y = ys[row];
                vertex.z = centre->z;
                vertex.u = ee::Float(column == 1 ? 1.0f : 0.0f);
                vertex.v = ee::Float(row == 1 ? 1.0f : 0.0f);
                std::memcpy(vertex.colour, &*colourWord, sizeof(vertex.colour));
            }
        }
        quad->texture = texture;
        const ee::Ptr<Vertex> vertices = grid;
        // The four cells, each corner from the row above: low left, low right, high right, high left.
        const auto drawCell = [&](int row, int column)
        {
            quad->corners[0] = vertices + ((row + 1) * 3 + column);
            quad->corners[1] = vertices + ((row + 1) * 3 + column + 1);
            quad->corners[2] = vertices + (row * 3 + column + 1);
            quad->corners[3] = vertices + (row * 3 + column);
            zge::drawPrimitiveQuads(0, quad, 1, camera);
        };
        drawCell(0, 0);
        drawCell(0, 1);
        drawCell(1, 1);
        drawCell(1, 0);
    }
    READABLE_FUNCTION_WAITS(drawLensFlareElement, Verified);

    // Where the sun is in front of the camera and in sight: its star, ray and
    // glow at the sun's direction, bigger the straighter the camera looks at
    // it; then the elements on the line from the sun through the middle of
    // the screen, smaller and fainter the straighter it looks.
    void drawLensFlare(ee::Ptr<LensFlare> flare)
    {
        ee::Local<zge::Vec4> sunward;   // the sun's direction from the eye, 128 long
        ee::Local<zge::Vec4> elementAt;
        ee::Local<zge::Matrix> view;    // the camera's view matrix
        ee::Local<zge::Matrix> camera;  // the camera's matrix in the world
        ee::Local<FlareExtent> extent;
        ee::Local<zge::Vec4> screenMiddle; // half a unit ahead of the eye
        // The original keeps these in its frame too, and what they hold at the return stays there below the
        // caller's stack pointer, where later code reads it as stale stack.
        ee::Local<zge::Vec4> fromMiddle;    // the sun's place from the middle
        ee::Local<uint32_t> sunColour;      // the star's, the ray's and the glow's colour in turn
        ee::Local<uint8_t[4]> elementColour; // an element's colour, its alpha faded
        const ee::Ptr<zge::Vec4> sun = &flare->position;
        *view = *zge::cachedMatrix(ee::Ptr<zge::MatrixCache>(*zge::g_currentCamera), 0);
        *camera = *zge::cachedMatrix(ee::Ptr<zge::MatrixCache>(*zge::g_currentCamera), 1);
        if (lensFlareHidden(sun, &camera->translation()) != 0)
            return;

        rider::collision::engine::transformPoint(&sunward->x, view, &sun->x);
        // (The original writes the middle's x, y and z only: its w is what the stack held, and the difference's w goes with it.)
        screenMiddle->x = 0.0f;
        screenMiddle->y = 0.0f;
        screenMiddle->z = 0.5f;
        *fromMiddle = *sunward - *screenMiddle;
        if (fromMiddle->z < ee::Float(0.0f))
            return; // behind the camera
        // (Worked out where the elements' places go later.)
        zge::Vec4 &direction = *elementAt;
        direction = zge::unit(*fromMiddle);
        // How straight the camera looks at the sun (0.72 dead ahead).
        const ee::Float facing = direction.z * direction.z * ee::Float(0.72f);
        const ee::Float facingSquared = facing * facing;
        const ee::Float sunSize = facingSquared * facingSquared * (facing * ee::Float(8.0f) * facingSquared) * facing * facing * ee::Float(0.5f);
        *sunward = zge::scaledCarrying(direction, ee::Float(128.0f), zge::scaleLeftover(zge::LeftBy::lastCall));

        extent->x = half(flare->elementScaleX) * sunSize;
        extent->y = half(flare->elementScaleY) * sunSize;
        *sunColour = rgba(255, 255, 255, 0x80);
        drawLensFlareElement(sunward, extent, flare->star, *sunColour, camera);
        *sunColour = rgba(255, 255, 255, 100);
        drawLensFlareElement(sunward, extent, flare->ray, *sunColour, camera);
        *sunColour = rgba(0xF0, 0xF0, 100, 250);
        drawLensFlareElement(sunward, extent, flare->glow, *sunColour, camera);

        const ee::Float elementSize = (ee::Float(1.0f) - facing) * ee::Float(0.125f);
        for (uint32_t i = 0; i < flare->elementCount; ++i)
        {
            const LensFlareElement &element = flare->elements[i];
            extent->x = half(flare->elementScaleX) * element.scale * elementSize;
            extent->y = half(flare->elementScaleY) * element.scale * elementSize;
            *elementAt = zge::scaledCarrying(*sunward, element.distance, zge::scaleLeftover(zge::LeftBy::lastCall));
            elementAt->z = ee::Float(128.0f);
            std::memcpy(&elementColour[0], element.colour, sizeof(element.colour));
            elementColour[3] = static_cast<uint8_t>(ee::toInt(ee::toFloat(elementColour[3]) * facing));
            uint32_t colourWord;
            std::memcpy(&colourWord, &elementColour[0], sizeof(colourWord));
            drawLensFlareElement(elementAt, extent, element.texture, colourWord, camera);
        }
    }
    READABLE_FUNCTION_WAITS(drawLensFlare, Verified);
}
