// The engine's lens flare: "LensFlare", a full-screen effect of the effect
// manager (fx_manager.h). A run is started with a LensFlareSettings record:
// a light's place in the world, a colour and brightness, and up to 16
// elements (a glow, rings, a star) set out along the line from the light
// through the centre of the view. Each frame it is drawn the flare brightens
// as the light nears the centre of the view, and fades out while scenery
// stands between the camera and the light (an occlusion test along that
// line, all at once or a piece a frame).
//
// Registered at the effect manager's start in all three games, but nothing
// in Aggressive Inline starts it: the game draws its own lens flare
// (aggressive/fx/lens_flare.cpp).

#include "fx_manager.h"

#include "zge/collision/collision.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/render/primitives.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/zge/render/lens_flare.cpp.h"

namespace zge
{
    namespace
    {
        struct Rgba
        {
            uint8_t red, green, blue, alpha;
        };
        static_assert(sizeof(Rgba) == 4);

        // One element of the flare (0x14 bytes): a textured square somewhere
        // on the line from the light through the centre of the view.
        struct LensFlareElement
        {
            uint32_t material;  // +0x00
            ee::Float size;     // +0x04 times the settings' width and height
            // +0x08 where on the line: 0 at the light, 1 at the centre of the
            // view, 2 as far past it as the light is before it
            ee::Float along;
            Rgba colour;        // +0x0C scaled by the settings' colour; alpha by the flare's strength
            uint32_t flags;     // +0x10 LensFlareElementFlags
        };
        static_assert(offsetof(LensFlareElement, along) == 0x08 && offsetof(LensFlareElement, flags) == 0x10 &&
                      sizeof(LensFlareElement) == 0x14);

        enum LensFlareElementFlags : uint32_t
        {
            // Sized by the factor that grows as the light nears the centre of
            // the view (else by the one that grows as it moves away).
            kElementGrowsAtCentre = 1u << 0,
            // Its alpha times the flare's strength (else times 1 - strength:
            // it fades as the flare brightens).
            kElementFadesIn = 1u << 1,
            // Drawn as four quads, each with the whole texture: the texture
            // holds a quarter of the shape, mirrored into the other three.
            kElementMirrored = 1u << 2,
            // At the light's depth in the camera's space (else at depth 128).
            kElementAtLightDepth = 1u << 5,
        };
        // Bits 3 and 4: how the element is turned. 0 not at all; 1 (and 3)
        // pointing along the line to the light; 2 the other way.
        constexpr uint32_t kElementTurnShift = 3, kElementTurnMask = 3u;
        constexpr uint32_t kElementUnturned = 0, kElementTurnedBack = 2;

        constexpr uint32_t kMaxFlareElements = 16;

        // What a run of the flare is started with and set to (0x190 bytes:
        // the start of its LensFlare).
        struct LensFlareSettings
        {
            uint8_t _00[0x10];
            Vec4 position;        // +0x10 the light's, in the world
            // +0x20 with kLensFlareFacing, the flare is dimmed by minus the
            // dot product of this with the view matrix's z row (none below 0)
            Vec4 direction;
            Rgba colour;          // +0x30 the elements' red, green and blue are scaled by it (its alpha is not read)
            ee::Float brightness; // +0x34
            // +0x38 the width and height of an element of size 1 (at depth
            // 128, the depth elements are drawn at)
            uint32_t width;
            uint32_t height;
            // +0x40 the occlusion test's length in frames (lensFlareBlocked);
            // kNoOcclusionTest: never hidden
            int32_t testPeriod;
            uint32_t elementCount; // +0x44
            uint32_t flags;        // +0x48 LensFlareFlags
            LensFlareElement elements[kMaxFlareElements]; // +0x4C
            uint32_t _18c;
        };
        static_assert(offsetof(LensFlareSettings, position) == 0x10 && offsetof(LensFlareSettings, colour) == 0x30);
        static_assert(offsetof(LensFlareSettings, testPeriod) == 0x40 && offsetof(LensFlareSettings, elements) == 0x4C);
        static_assert(sizeof(LensFlareSettings) == 0x190);

        enum LensFlareFlags : uint32_t
        {
            kLensFlareFacing = 1u << 0,
            // The occlusion test split over the test's frames, a piece of the
            // line a frame (else the whole line, in ten pieces, on its first
            // frame).
            kLensFlareTestsOverFrames = 1u << 1,
        };
        constexpr int32_t kNoOcclusionTest = -1;

        // A run's flare (0x1E0 bytes from the heap, RunningEffect::data).
        // The start copies in the settings and leaves the rest as the heap
        // block held it.
        struct LensFlare
        {
            LensFlareSettings settings; // +0x000
            // +0x190 how hidden the flare is: 0 clear to 255 hidden; up 20 a
            // frame while the light is blocked, down 20 while it is not
            uint8_t hidden;
            uint8_t blocked;            // +0x191 bit 0: the last test found the light blocked
            uint8_t _192[0x0E];
            // +0x1A0 with kLensFlareTestsOverFrames: the piece of the line
            // from the camera to the light tested next, the step from one
            // piece to the next, whether a piece has hit yet this test, and
            // how many pieces are left
            QuerySegment piece;
            Vec4 pieceStep;             // +0x1C0
            int32_t hit;                // +0x1D0
            uint8_t piecesLeft;         // +0x1D4
            uint8_t _1d5[0x0B];
        };
        static_assert(offsetof(LensFlare, hidden) == 0x190 && offsetof(LensFlare, piece) == 0x1A0);
        static_assert(offsetof(LensFlare, pieceStep) == 0x1C0 && offsetof(LensFlare, hit) == 0x1D0);
        static_assert(offsetof(LensFlare, piecesLeft) == 0x1D4 && sizeof(LensFlare) == 0x1E0);

        constexpr uint8_t kBlocked = 1u;
        constexpr uint8_t kFullyHidden = 255;
        constexpr uint8_t kFadeStep = 20;
        // At or past this the next blocked frame hides the flare fully.
        constexpr uint8_t kNearlyHidden = 230;

        // A vertex and a quad as the primitive API takes them
        // (drawPrimitiveQuads: render/ps2/primitive_packets.cpp).
        struct alignas(16) Vertex
        {
            Vec4 position; // +0x00 (w not read)
            ee::Float u, v; // +0x10 texture coordinates
            Rgba colour;   // +0x18
            uint32_t _1c;
        };
        static_assert(offsetof(Vertex, u) == 0x10 && offsetof(Vertex, colour) == 0x18 && sizeof(Vertex) == 0x20);

        struct Quad
        {
            uint32_t material;          // +0x00
            ee::Ptr<Vertex> corners[4]; // +0x04
        };
        static_assert(sizeof(Quad) == 0x14);

        // The camera's matrices (cachedMatrix): world to camera, and camera to world.
        constexpr uint32_t kViewMatrix = 0, kCameraMatrix = 1;
        // An element's size is given at this depth: it is scaled by its depth
        // over this, so it covers the same part of the screen at any depth.
        constexpr ee::Float kElementDepth = 128.0f;
        constexpr ee::Float kPerElementDepth = std::bit_cast<float>(0x3C000000u); // 1 / 128
        // The point half a unit in front of the camera the flare's line runs
        // through (the centre of the view).
        constexpr ee::Float kViewCentreDepth = 0.5f;
        constexpr ee::Float kPerSquaredByte = std::bit_cast<float>(0x37810182u); // 1 / (255 * 255)
        constexpr ee::Float kFaintest = std::bit_cast<float>(0x3A83126Fu);       // 0.001: not drawn below this
        constexpr ee::Float kGrowthRange = std::bit_cast<float>(0x3DCCCCCDu);    // 0.1
        constexpr ee::Float kGrowthBase = std::bit_cast<float>(0x3CCCCCCDu);     // 0.025
        // The whole line is tested in this many pieces when not over frames.
        constexpr int32_t kWholeLinePieces = 10;
        constexpr ee::Float kWholeLinePiece = std::bit_cast<float>(0x3DCCCCCDu); // 0.1

        // An unsigned number as a float, as the compiled conversion does it
        // (halved with its low bit kept, converted and doubled when past
        // the signed range).
        ee::Float unsignedToFloat(uint32_t n)
        {
            if (static_cast<int32_t>(n) >= 0)
                return ee::toFloat(static_cast<int32_t>(n));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((n >> 1) | (n & 1u)));
            return half + half;
        }

        // Whether the segment hits the scenery (its hits in world space).
        int32_t segmentHits(ee::Ptr<QuerySegment> segment)
        {
            return collisionQuery(static_cast<uint32_t>(CollisionKind::Segment), segment, kQueryWorldPoints, 0, 0, 0, 0, 0);
        }

        namespace tail
        {
            int32_t heapFree(uint32_t block) { return ee::tailCall<int32_t>(sym::heapFree, block); }
        }

        void setVertex(Vertex &vertex, ee::Float x, ee::Float y, ee::Float z, ee::Float u, ee::Float v, Rgba colour)
        {
            vertex.position.x = x;
            vertex.position.y = y;
            vertex.position.z = z;
            vertex.u = u;
            vertex.v = v;
            vertex.colour = colour;
        }
        void setCorners(Quad &quad, Vertex &a, Vertex &b, Vertex &c, Vertex &d)
        {
            quad.corners[0] = &a;
            quad.corners[1] = &b;
            quad.corners[2] = &c;
            quad.corners[3] = &d;
        }

        // An element's half width and half height scaled from depth 128 to
        // the element's depth (written back to the caller's halfSize, as the
        // original does).
        void scaleToDepth(Vec4 &halfSize, const Vec4 &centre)
        {
            halfSize.x = halfSize.x * centre.z * kPerElementDepth;
            halfSize.y = halfSize.y * centre.z * kPerElementDepth;
        }
    }

    // drawFlareElement: one element of the flare, a square (halfSize across
    // and up from its centre, scaled to its depth) turned by the angle in
    // the camera's space, in the colour, drawn through the camera's matrix.
    void drawFlareElement(ee::Ptr<Vec4> centre, ee::Ptr<Vec4> halfSize, uint32_t material, uint32_t rgba, ee::Ptr<Matrix> camera,
                           Angle turn)
    {
        ee::Local<Rgba> colour;
        ee::Local<Vertex[4]> corners;
        ee::Local<Quad> quad;
        *colour = std::bit_cast<Rgba>(rgba);
        const ee::Float sine = angleSin(turn);
        const ee::Float cosine = angleCos(turn);
        scaleToDepth(*halfSize, *centre);
        const ee::Float across = halfSize->x, back = -across;
        const ee::Float up = halfSize->y, down = -up;
        const Vec4 &at = *centre;
        // Each corner (x, y) turned: x cos - y sin, x sin + y cos.
        setVertex(corners[0], at.x + (cosine * back - sine * down), at.y + (sine * back + cosine * down), at.z, 0.0f, 0.0f, *colour);
        setVertex(corners[1], at.x + (cosine * across - sine * down), at.y + (sine * across + cosine * down), at.z, 1.0f, 0.0f, *colour);
        setVertex(corners[2], at.x + (cosine * across - sine * up), at.y + (sine * across + cosine * up), at.z, 1.0f, 1.0f, *colour);
        setVertex(corners[3], at.x + (cosine * back - sine * up), at.y + (sine * back + cosine * up), at.z, 0.0f, 1.0f, *colour);
        quad->material = material;
        setCorners(*quad, corners[3], corners[2], corners[1], corners[0]);
        drawPrimitiveQuads(0, quad, 1, camera);
    }
    READABLE_FUNCTION_WAITS(drawFlareElement, Draft);

    // drawMirroredFlareElement: an element as drawFlareElement draws it, but
    // as four quads that meet at its centre, the texture's corner (1, 1) at
    // the centre of the square and (0, 0) at each of its corners: the
    // texture is a quarter of the element, mirrored into each quarter.
    void drawMirroredFlareElement(ee::Ptr<Vec4> centre, ee::Ptr<Vec4> halfSize, uint32_t material, uint32_t rgba,
                                   ee::Ptr<Matrix> camera, Angle turn)
    {
        ee::Local<Rgba> colour;
        // A 3 x 3 grid: the bottom row (left, middle, right), the middle row,
        // the top row.
        ee::Local<Vertex[9]> grid;
        ee::Local<Quad> quad;
        *colour = std::bit_cast<Rgba>(rgba);
        const ee::Float sine = angleSin(turn);
        const ee::Float cosine = angleCos(turn);
        scaleToDepth(*halfSize, *centre);
        const ee::Float across = halfSize->x, back = -across;
        const ee::Float up = halfSize->y, down = -up;
        const Vec4 &at = *centre;
        const ee::Float cosAcross = cosine * across, sinAcross = sine * across;
        const ee::Float cosBack = cosine * back, sinBack = sine * back;
        const ee::Float cosUp = cosine * up, sinUp = sine * up;
        const ee::Float cosDown = cosine * down, sinDown = sine * down;
        setVertex(grid[0], at.x + (cosBack - sinDown), at.y + (sinBack + cosDown), at.z, 0.0f, 0.0f, *colour);
        setVertex(grid[1], at.x - sinDown, at.y + cosDown, at.z, 1.0f, 0.0f, *colour);
        setVertex(grid[2], at.x + (cosAcross - sinDown), at.y + (sinAcross + cosDown), at.z, 0.0f, 0.0f, *colour);
        setVertex(grid[3], at.x + cosBack, at.y + sinBack, at.z, 0.0f, 1.0f, *colour);
        setVertex(grid[4], at.x, at.y, at.z, 1.0f, 1.0f, *colour);
        setVertex(grid[5], at.x + cosAcross, at.y + sinAcross, at.z, 0.0f, 1.0f, *colour);
        setVertex(grid[6], at.x + (cosBack - sinUp), at.y + (sinBack + cosUp), at.z, 0.0f, 0.0f, *colour);
        setVertex(grid[7], at.x - sinUp, at.y + cosUp, at.z, 1.0f, 0.0f, *colour);
        setVertex(grid[8], at.x + (cosAcross - sinUp), at.y + (sinAcross + cosUp), at.z, 0.0f, 0.0f, *colour);
        quad->material = material;
        setCorners(*quad, grid[3], grid[4], grid[1], grid[0]); // bottom left
        drawPrimitiveQuads(0, quad, 1, camera);
        setCorners(*quad, grid[4], grid[5], grid[2], grid[1]); // bottom right
        drawPrimitiveQuads(0, quad, 1, camera);
        setCorners(*quad, grid[7], grid[8], grid[5], grid[4]); // top right
        drawPrimitiveQuads(0, quad, 1, camera);
        setCorners(*quad, grid[6], grid[7], grid[4], grid[3]); // top left
        drawPrimitiveQuads(0, quad, 1, camera);
    }
    READABLE_FUNCTION_WAITS(drawMirroredFlareElement, Draft);

    namespace
    {
        // The next piece of the line to the light tested, and the piece
        // after it made ready.
        void testNextPiece(LensFlare &flare)
        {
            flare.hit = segmentHits(&flare.piece);
            flare.piece.start = flare.piece.start + flare.pieceStep;
            flare.piece.end = flare.piece.start + flare.pieceStep;
            flare.piecesLeft = static_cast<uint8_t>(flare.piecesLeft - 1);
        }
    }

    // lensFlareBlocked: whether scenery stands between the eye and the light,
    // tested along the line between them once every `period` frames
    // (*frames counts them, one more each call). Without
    // kLensFlareTestsOverFrames the whole line is tested, in ten pieces, on
    // the period's first frame, and the answer is given then. With it, the
    // line is cut into `period` pieces, one tested a frame until a piece
    // hits, and the answer is given on the period's last frame. Between
    // answers it hands back lastAnswer.
    uint32_t lensFlareBlocked(uint32_t lastAnswer, uint32_t period, ee::Ptr<uint32_t> frames, ee::Ptr<Vec4> light, ee::Ptr<Vec4> eye,
                               ee::Ptr<LensFlare> flare)
    {
        ee::Local<QuerySegment> segment;
        ee::Local<Vec4> step;
        uint32_t answer = lastAnswer;
        const bool overFrames = (flare->settings.flags & kLensFlareTestsOverFrames) != 0;
        // (A zero period: DIVU by 0 leaves the dividend, as the recompiled
        // code has it; the original then raises BREAK 7.)
        const uint32_t frameOfPeriod = period ? *frames % period : *frames;
        if (frameOfPeriod == 0)
        {
            *frames = 0;
            if (overFrames)
            {
                flare->piece.end = *light;
                flare->piece.start = *eye;
                flare->pieceStep = flare->piece.end - flare->piece.start;
                flare->pieceStep = flare->pieceStep * (ee::Float(1.0f) / unsignedToFloat(period));
                flare->piece.end = flare->piece.start + flare->pieceStep;
                flare->hit = 0;
                flare->piecesLeft = static_cast<uint8_t>(period);
                if (flare->piecesLeft != 0)
                    testNextPiece(*flare);
            }
            else
            {
                segment->end = *light;
                segment->start = *eye;
                *step = segment->end - segment->start;
                *step = *step * kWholeLinePiece;
                segment->end = segment->start + *step;
                for (int32_t pieces = kWholeLinePieces; pieces > 0; --pieces)
                {
                    answer = static_cast<uint32_t>(segmentHits(segment));
                    segment->start = segment->start + *step;
                    segment->end = segment->start + *step;
                    if (answer != 0)
                        break;
                }
            }
        }
        else if (overFrames)
        {
            if (flare->hit == 0 && flare->piecesLeft != 0)
                testNextPiece(*flare);
            if (*frames == period - 1)
                answer = static_cast<uint32_t>(flare->hit);
        }
        *frames = *frames + 1;
        return answer;
    }
    READABLE_FUNCTION_WAITS(lensFlareBlocked, Draft);

    // startLensFlare (LensFlare's start): the run's flare made, the settings
    // copied in. The rest of the flare (how hidden it is, the occlusion
    // test's state) is not cleared: it starts as the heap block held it.
    void startLensFlare(ee::Ptr<RunningEffect> run, ee::Ptr<LensFlareSettings> settings)
    {
        run->data = heapAllocate(sizeof(LensFlare));
        memoryCopy(run->data, settings, sizeof(LensFlareSettings));
    }
    READABLE_FUNCTION_WAITS(startLensFlare, Draft);

    // setLensFlare (LensFlare's set-value method): new settings copied over
    // the flare's. Returns the flare (the copy's result).
    uint32_t setLensFlare(ee::Ptr<RunningEffect> run, ee::Ptr<LensFlareSettings> settings)
    {
        return memoryCopy(run->data, settings, sizeof(LensFlareSettings));
    }
    READABLE_FUNCTION(setLensFlare, Draft);

    // drawLensFlareEffect (LensFlare's draw): the occlusion test run (unless the
    // settings have none) and the flare faded by its answer; then, if the
    // light is in front of the camera, each element drawn on the line from
    // the light through the centre of the view, sized and faded by how near
    // the light is to that centre. Nothing is drawn while the flare is fully
    // hidden or fainter than 0.001.
    void drawLensFlareEffect(ee::Ptr<RunningEffect> run)
    {
        ee::Local<Vec4> light;      // the light in the camera's space
        ee::Local<Vec4> fromCentre; // from the centre of the view to the light
        ee::Local<Vec4> viewCentre;
        ee::Local<Vec4> place;      // the direction to the light, then each element's centre
        ee::Local<Matrix> view;
        ee::Local<Matrix> camera;
        ee::Local<Vec4> halfSize;   // each element's half width and half height
        ee::Local<Rgba> colour;
        const ee::Ptr<LensFlare> flare(run->data);
        const LensFlareSettings &settings = flare->settings;
        *view = *cachedMatrix(ee::Ptr<MatrixCache>(*g_currentCamera), kViewMatrix);
        *camera = *cachedMatrix(ee::Ptr<MatrixCache>(*g_currentCamera), kCameraMatrix);
        if (settings.testPeriod != kNoOcclusionTest)
        {
            if (lensFlareBlocked(static_cast<uint32_t>(flare->blocked & kBlocked), static_cast<uint32_t>(settings.testPeriod),
                                 &run->counter, &flare->settings.position, &camera->translation(), flare))
            {
                flare->blocked = flare->blocked | kBlocked;
                flare->hidden = flare->hidden >= kNearlyHidden ? kFullyHidden : static_cast<uint8_t>(flare->hidden + kFadeStep);
                if (flare->hidden == kFullyHidden)
                    return;
            }
            else
            {
                flare->blocked = flare->blocked & ~kBlocked;
                flare->hidden = flare->hidden <= kFadeStep ? 0 : static_cast<uint8_t>(flare->hidden - kFadeStep);
            }
        }

        *light = transformed(*view, settings.position);
        viewCentre->x = 0.0f;
        viewCentre->y = 0.0f;
        viewCentre->z = kViewCentreDepth;
        *fromCentre = *light - *viewCentre;
        if (fromCentre->z < 0.0f)
            return; // behind the camera

        ee::Float facing = 1.0f;
        if (settings.flags & kLensFlareFacing)
        {
            facing = -dot(matrixRow(*view, 2), settings.direction);
            if (facing < 0.0f)
                facing = 0.0f;
        }
        *place = unit(*fromCentre);
        // The cosine of the angle between the view's axis and the light.
        const ee::Float cosine = place->z;
        const ee::Float cosine2 = cosine * cosine;
        const ee::Float visible = ee::toFloat(kFullyHidden - flare->hidden);
        // Its strength: 0 to 1 for a brightness of 255, by the fourth power
        // of the cosine.
        const ee::Float strength = facing * visible * (settings.brightness * cosine) * (cosine * kPerSquaredByte * cosine2);
        if (strength < kFaintest)
            return;
        // The elements' two size factors: broad grows as the light moves
        // away from the centre of the view, sharp (the cosine's 14th power)
        // as it nears it; both 0.025 to 0.125.
        const ee::Float cosine4 = cosine2 * cosine2;
        const ee::Float broad = (ee::Float(1.0f) - cosine2) * kGrowthRange + kGrowthBase;
        const ee::Float sharp = cosine4 * cosine4 * cosine * cosine * cosine * cosine * cosine * cosine * kGrowthRange + kGrowthBase;
        const ee::Ptr<LensFlareElement> elements(flare->settings.elements);
        for (uint32_t i = 0; i < settings.elementCount; ++i)
        {
            const LensFlareElement &element = elements[i];
            const ee::Float size = element.size;
            const ee::Float growth = (element.flags & kElementGrowsAtCentre) ? sharp : broad;
            halfSize->x = ee::toFloat(static_cast<int32_t>(settings.width / 2)) * size * growth;
            halfSize->y = ee::toFloat(static_cast<int32_t>(settings.height / 2)) * size * growth;
            place->x = light->x - element.along * fromCentre->x;
            place->y = light->y - element.along * fromCentre->y;
            place->z = (element.flags & kElementAtLightDepth) ? light->z : kElementDepth;
            colour->red = static_cast<uint8_t>(element.colour.red * settings.colour.red >> 8);
            colour->green = static_cast<uint8_t>(element.colour.green * settings.colour.green >> 8);
            colour->blue = static_cast<uint8_t>(element.colour.blue * settings.colour.blue >> 8);
            Angle turn = 0;
            const uint32_t turnKind = element.flags >> kElementTurnShift & kElementTurnMask;
            if (turnKind != kElementUnturned)
            {
                turn = angleAtan2(fromCentre->x, fromCentre->y);
                if (turnKind == kElementTurnedBack)
                    turn = static_cast<Angle>(-turn);
            }
            const ee::Float alpha = ee::toFloat(element.colour.alpha);
            const ee::Float fade = (element.flags & kElementFadesIn) ? strength : ee::Float(1.0f) - strength;
            colour->alpha = static_cast<uint8_t>(ee::toInt(alpha * fade));
            if (element.flags & kElementMirrored)
                drawMirroredFlareElement(place, halfSize, element.material, std::bit_cast<uint32_t>(*colour), camera, turn);
            else
                drawFlareElement(place, halfSize, element.material, std::bit_cast<uint32_t>(*colour), camera, turn);
        }
    }
    READABLE_FUNCTION_WAITS(drawLensFlareEffect, Draft);

    // stopLensFlare (LensFlare's stop): the run's flare freed (the
    // original's tail call).
    int32_t stopLensFlare(ee::Ptr<RunningEffect> run)
    {
        return tail::heapFree(run->data);
    }
    READABLE_FUNCTION_WAITS(stopLensFlare, Draft);

    // registerLensFlareEffect (initEffectManager): "LensFlare" registered,
    // drawn but not updated: its start, draw, stop and set-value methods,
    // and its runs handed back to whoever starts one.
    void registerLensFlareEffect()
    {
        ee::Local<FullScreenEffect> definition;
        memoryFill(definition.address(), 0, sizeof(FullScreenEffect));
        definition->flags = ((definition->flags | kEffectHandsBackRun) & ~kEffectUpdates) | kEffectDraws;
        definition->start = sym::startLensFlare;
        definition->update = 0;
        definition->draw = sym::drawLensFlareEffect;
        definition->stop = sym::stopLensFlare;
        definition->setValue = sym::setLensFlare;
        registerFullScreenEffect(sym::g_textLensFlare, definition);
    }
    READABLE_FUNCTION_WAITS(registerLensFlareEffect, Verified);
}
