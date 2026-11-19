// The lightning ("lightning"; inlineFx.c, were the sweep's drafts of
// loadLightningTextures .. spawnLightning and drawLightning): one to four bolts at random around
// the camera, each a chain of segments from high up to the ground, every
// point but the last pushed aside at random. Its textures, start, spawn
// and draw (its move is the classes' shared one).
//

#include "aggressive/camera.h"
#include "aggressive/fx/billboard.h"
#include "aggressive/fx/library.h"
#include "aggressive/fx/particles.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/container/array.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/vector.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "zge/assets/asset_library.h"
#include "zge/render/draw.h"
#include "aggressive/fx/effects_clock.h"
#include "zge/render/primitives.h"
#include "zge/scene/particle_types.h"
#include "zge/math/random.h"
#include "sce/libgcc.h"

#include <cstring>
#include "sym/aggressive/fx/lightning.cpp.h"

namespace zge
{
}

namespace aggressive::fx
{

    namespace
    {
        constexpr uint32_t kAllocateSized = sym::allocateSized;     // (bytes) -> a block of the smallest size-class pool that fits
        constexpr uint32_t kLoadMatFxTextures = sym::loadFxMaterial; // (library, texture id name, material name, flags, &texture)
        constexpr uint32_t kFxLibrary = sym::fxLibrary;         // "assets\fx.zal"
        constexpr uint32_t kBoltTextureId = sym::boltTextureId;     // "kFxID_ImgLightningBolt"
        constexpr uint32_t kForkTextureId = sym::forkTextureId;     // "kFxID_ImgLightningFork"
        constexpr uint32_t kBoltMaterial = sym::boltMaterial;      // "LightningBolt"
        constexpr uint32_t kForkMaterial = sym::forkMaterial;      // "LightningFork"
        constexpr uint32_t kLightningTextureFlags = 0x08000002u;

        // The bolts' distance from the camera (near .. far, .data) and the
        // degrees-to-angle factor (.sdata).
        inline ee::Global<ee::Float, sym::g_boltFarthest> g_boltFarthest;  // 70
        inline ee::Global<ee::Float, sym::g_boltNearest> g_boltNearest;   // 10
        inline ee::Global<ee::Float, sym::g_boltDegreesToAngle> g_boltDegreesToAngle;
        inline ee::Global<ee::Float, sym::g_boltReachStep> g_boltReachStep; // 0.4: the ground point's offset a step of the random
        inline ee::Global<ee::Float, sym::g_boltWidth> g_boltWidth;     // 0.2
        inline constexpr ee::Float kBoltJitter = 4.5f;
        inline constexpr ee::Float kBoltHeight = -40.0f;    // y down: up in the sky
        inline constexpr ee::Float kBoltHeightSpread = 8.0f; // 0 .. 15 less this
        inline constexpr ee::Float kReachCentre = 32.0f;    // 0 .. 63 less this
        constexpr uint32_t kSegmentLifetime = 500;          // milliseconds, and 15 more a segment down the bolt
        constexpr uint32_t kLightningFork = 0x4;            // a segment's flags: every fourth from the third (the fork's) [inferred]
        inline constexpr ee::Float kRandomByteStep = 0.0078125f;
        inline constexpr ee::Float kRandomByteCentre = 128.0f;

        // A bolt (the named effect's parameters; the start's, or with none
        // the defaults at lightningDefaults: from the origin to 1 below it, 8
        // segments, 0.01 wide, pushed aside up to 10).
        struct LightningParameters
        {
            ee::Float fromX, fromY, fromZ; // +0x00
            uint32_t _0c;
            ee::Float toX, toY, toZ;       // +0x10
            uint32_t _1c;
            int32_t segments;              // +0x20
            ee::Float width;               // +0x24 (the segments' size)
            ee::Float jitter;              // +0x28 a point pushed aside up to this either way
            uint32_t texture;              // +0x2C
        };
        static_assert(offsetof(LightningParameters, toX) == 0x10 && offsetof(LightningParameters, segments) == 0x20);
        static_assert(offsetof(LightningParameters, texture) == 0x2C);
        constexpr uint32_t kLightningDefaults = sym::lightningDefaults;

        // What the bolts read of the emitter object's data (zge's
        // EmitterObjectData): the point they are made from.
        struct EmitterDataPosition
        {
            uint8_t _00[0x10];
            ee::Float x, y, z; // +0x10
        };
        // A particle emitter object (a SceneObject) as the bolts see it: its
        // data where other objects have their animator.
        struct EmitterObject
        {
            uint8_t _00[0x3C];
            ee::Ptr<EmitterDataPosition> data; // +0x3C SceneObject::animator
        };
        static_assert(offsetof(EmitterObject, data) == 0x3C);

        // A random number modulo n (DIVU by 0 leaves the dividend).
        uint32_t randomModulo(uint32_t r, uint32_t n)
        {
            return n ? r % n : r;
        }
    }

    // loadLightningTextures: the bolt's and the fork's textures loaded.
    uint32_t loadLightningTextures()
    {
        const uint32_t library = zge::openAssetLibrary(kFxLibrary);
        loadFxMaterial(library, kBoltTextureId, kBoltMaterial, kLightningTextureFlags, g_boltTexture.get());
        loadFxMaterial(library, kForkTextureId, kForkMaterial, kLightningTextureFlags, g_forkTexture.get());
        return game::tail(zge::closeAssetLibrary, library);
    }
    READABLE_FUNCTION_WAITS(loadLightningTextures, Verified);

    // startLightning: lightning (the named effect's start): 1 to 4 bolts, each
    // at a random distance from the camera in a random direction within its
    // field of view either way of where it looks, from high up (y -48 ..
    // -33) to the ground (y 0) a little aside, 6 to 9 segments.
    uint32_t startLightning(ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<zge::Vec4> eye;
        ee::Local<zge::Vec4> lookAt;
        ee::Local<zge::Vec4> point;
        ee::Local<LightningParameters> bolt;
        const FollowCamera &camera = *g_followCamera;
        *eye = camera.eye;
        *lookAt = camera.lookAt;
        const int32_t heading = static_cast<short>(zge::angleAtan2(lookAt->x - eye->x, lookAt->z - eye->z));
        const uint32_t count = (zge::randomNumber() & 3u) + 1;
        uint32_t started = 0;
        for (uint32_t bolts = 0; bolts < count; ++bolts)
        {
            // Where it strikes: a random distance from the camera, within
            // its field of view either way of where it looks.
            const uint32_t r = zge::randomNumber();
            const ee::Float nearest = *g_boltNearest;
            const uint32_t span = static_cast<uint32_t>(ee::toInt(*g_boltFarthest - nearest));
            const ee::Float distance = nearest + ee::toFloat(static_cast<int32_t>(randomModulo(r, span)));
            const int32_t fov = ee::toInt(camera.fov);
            const uint32_t r2 = zge::randomNumber();
            const ee::Float degreesToAngle = *g_boltDegreesToAngle;
            const uint32_t degrees = randomModulo(r2, static_cast<uint32_t>(fov << 1));
            point->z = distance;
            point->x = 0.0f;
            point->y = 0.0f;
            point->w = 0.0f;
            const int32_t turn = -(heading + ee::toInt(ee::toFloat(static_cast<int32_t>(degrees - fov)) * degreesToAngle));
            zge::vec3RotateY(asVec3(*point), asVec3(*point), static_cast<zge::Angle>(turn));
            *point = *point + *eye;

            // From high up there, to the ground a little aside.
            const uint32_t r3 = zge::randomNumber();
            const ee::Float x = point->x;
            const ee::Float z = point->z;
            bolt->toX = x;
            const ee::Float height = ee::toFloat(static_cast<int32_t>(r3 & 0xFu)) - kBoltHeightSpread + kBoltHeight;
            bolt->fromZ = z;
            bolt->fromX = x;
            bolt->fromY = height;
            point->y = height;
            const uint32_t r4 = zge::randomNumber();
            const ee::Float reachStep = *g_boltReachStep;
            const ee::Float toX = bolt->toX;
            bolt->toZ = bolt->fromZ;
            bolt->toX = toX + (ee::toFloat(static_cast<int32_t>(r4 & 0x3Fu)) - kReachCentre) * reachStep;
            const uint32_t r5 = zge::randomNumber();
            bolt->toY = 0.0f;
            bolt->toZ = bolt->toZ + (ee::toFloat(static_cast<int32_t>(r5 & 0x3Fu)) - kReachCentre) * reachStep;

            const uint32_t r6 = zge::randomNumber();
            const uint32_t texture = *g_boltTexture;
            const ee::Float width = *g_boltWidth;
            bolt->segments = static_cast<int32_t>((r6 & 3u) + 6);
            bolt->width = width;
            bolt->jitter = kBoltJitter;
            bolt->texture = texture;
            started = zge::startEffect(object, *g_lightningClass, 1, 0, bolt);
        }
        return started;
    }
    READABLE_FUNCTION_WAITS(startLightning, Verified);

    // spawnLightning ("lightning" +0x18): `count` bolts from the parameters
    // (the defaults when none), each its segments from the start to the
    // end, every point but the last pushed aside (x and y) at random up to
    // the jitter; the segments white at alpha 0x80, at the effect's
    // position, each 15 milliseconds longer lived down the bolt.
    void spawnLightning(uint32_t effectClass, ee::Ptr<EmitterObject> object, ee::Ptr<Emitter> emitter, uint32_t count, uint32_t, ee::Ptr<LightningParameters> given)
    {
        const uint32_t wanted = count & 0xFFu;
        const int32_t clock = effectsClock();
        const ee::Ptr<EmitterDataPosition> origin = object->data;
        const ee::Ptr<LightningParameters> bolt = given ? given : ee::Ptr<LightningParameters>(kLightningDefaults);
        if (!effectClass || !emitter || !wanted)
            return;
        // A step toward the end on one axis: what is left over the segments left.
        const auto stepToward = [](ee::Float to, ee::Float from, int32_t left)
        {
            const ee::Float gap = to - from;
            return gap / unsignedToFloat(static_cast<uint32_t>(left));
        };
        // A random byte (a point is pushed aside by it less 128, in steps
        // of the jitter over 128).
        const auto randomByte = []
        {
            return ee::toFloat(static_cast<int32_t>(zge::randomNumber() & 0xFFu));
        };
        for (uint32_t bolts = 0; bolts < wanted; ++bolts)
        {
            ee::Float fromX = bolt->fromX, fromY = bolt->fromY, fromZ = bolt->fromZ;
            for (uint32_t segment = 0; static_cast<int32_t>(segment) < bolt->segments; ++segment)
            {
                const ee::Ptr<Particle> piece = newParticle(*emitter);
                if (!piece)
                    return;
                ee::Float toX, toY, toZ;
                bool fork = false;
                if (static_cast<int32_t>(segment + 1) != bolt->segments)
                {
                    const int32_t left = bolt->segments - static_cast<int32_t>(segment);
                    const ee::Float rx = randomByte();
                    toX = fromX + stepToward(bolt->toX, fromX, left) + (rx - kRandomByteCentre) * (bolt->jitter * kRandomByteStep);
                    const ee::Float ry = randomByte();
                    toY = fromY + stepToward(bolt->toY, fromY, left) + (ry - kRandomByteCentre) * (bolt->jitter * kRandomByteStep);
                    toZ = fromZ + stepToward(bolt->toZ, fromZ, left);
                    fork = (segment & 3u) == 2u;
                }
                else
                {
                    toX = bolt->toX;
                    toY = bolt->toY;
                    toZ = bolt->toZ;
                }
                piece->flags = fork ? kLightningFork : 0u;
                piece->born = static_cast<uint32_t>(clock);
                piece->colour[3] = 0x80;
                piece->colour[0] = 255;
                piece->colour[1] = 255;
                piece->colour[2] = 255;
                piece->tail.x = origin->x + fromX;
                piece->tail.y = origin->y + fromY;
                piece->tail.z = origin->z + fromZ;
                piece->position.x = origin->x + toX;
                piece->position.y = origin->y + toY;
                piece->position.z = origin->z + toZ;
                piece->lifetime = segment * 15 + kSegmentLifetime;
                piece->size = bolt->width;
                piece->texture = bolt->texture;
                zge::pointerArrayAdd(emitter->particles, piece);
                fromX = toX;
                fromY = toY;
                fromZ = toZ;
            }
        }
    }
    READABLE_FUNCTION_WAITS(spawnLightning, Verified);

    namespace
    {

        // The draw's records for the primitive API: a ribbon's {texture, its
        // four vertices}, a line's {texture, its two}.
        struct RibbonRecord
        {
            uint32_t texture;
            ee::Ptr<QuadVertex> vertices[4];
        };
        struct LineRecord
        {
            uint32_t texture;
            ee::Ptr<QuadVertex> vertices[2];
        };

        // The glow over a ribbon: pale blue, 3 either side (its offsets
        // along the segment worked out in doubles, the literal's type).
        constexpr uint8_t kGlowRed = 0xAF, kGlowGreen = 0xAF, kGlowBlue = 0xFF;
        inline constexpr ee::Float kGlowHalfWidth = 3.0f;
        constexpr uint64_t kGlowHalfWidthDouble = 0x4008000000000000ull; // 3.0
        // A segment shows in the last of its life only, fading out: its
        // alpha the milliseconds it has left, under 256 (else nothing).
        constexpr uint32_t kShownFor = 0x100;
    }

    // drawLightning ("lightning" +0x20): the bolts' segments drawn. One
    // without a texture is a line from its bottom to its top; else a ribbon
    // facing the camera from the top along the segment (as the camera sees
    // it), `size` either side; over it a glow 3 either side, and on a fork's
    // segment the glow again mirrored across the top, with the fork's
    // texture. Returns how many segments it drew.
    int32_t drawLightning(uint32_t /*effectClass*/, ee::Ptr<Emitter> emitter)
    {
        ee::Local<RibbonRecord> ribbon;
        ee::Local<LineRecord> line;
        ee::Local<QuadVertex[4]> corners; // 0, 1 the bottom; 2, 3 the top
        ee::Local<zge::Matrix> camera;    // the camera's matrix, its translation the segment's top
        ee::Local<zge::Matrix> toCamera;  // its rotation transposed (into the camera's frame)
        ee::Local<uint8_t[4]> colour;
        ee::Local<zge::Vec4> along;       // the segment, top to bottom, in the camera's frame
        ee::Local<zge::Vec4> bottom;
        ee::Local<zge::Vec4> top;
        // (The original keeps these in its frame.)
        ee::Local<ee::Ptr<Emitter>> emitterKept;
        ee::Local<int32_t> drawn;
        ee::Local<int32_t> nowKept;
        ee::Local<ee::Ptr<zge::Vec4>> cameraTopAt;
        ee::Local<ee::Ptr<zge::Vec4>> toCameraOriginAt;
        ee::Local<ee::Ptr<QuadVertex>> corner2At;
        ee::Local<ee::Ptr<QuadVertex>> corner3At;
        QuadVertex &corner0 = corners[0];
        QuadVertex &corner1 = corners[1];
        QuadVertex &corner2 = corners[2];
        QuadVertex &corner3 = corners[3];
        *emitterKept = emitter;
        *drawn = 0;

        const int32_t now = effectsClock();
        *nowKept = now;
        for (uint32_t i = 0; i < 4; ++i)
            ribbon->vertices[i] = &corners[i];
        line->vertices[0] = &corner0;
        line->vertices[1] = &corner1;
        *corner2At = &corner2;
        *corner3At = &corner3;
        *camera = currentCameraMatrix();
        *toCamera = *camera;
        zge::matrixTranspose3(toCamera, toCamera);

        uint32_t segment = first(*emitter->particles);
        if (!segment)
            return *drawn;
        *toCameraOriginAt = &zge::matrixRow(*toCamera, 3);
        *cameraTopAt = &zge::matrixRow(*camera, 3);
        for (; segment; segment = next(*emitter->particles))
        {
            const Particle &p = *ee::Ptr<Particle>(segment);
            zge::matrixRow(*toCamera, 3) = zge::kZero;
            std::memcpy(&colour[0], p.colour, 4);
            const uint32_t left = p.lifetime - (static_cast<uint32_t>(now) - p.born);
            colour[3] = left < kShownFor ? static_cast<uint8_t>(left) : 0;
            const auto vertex = [&](QuadVertex &v, ee::Float x, ee::Float y, ee::Float z)
            {
                std::memcpy(v.colour, &colour[0], 4);
                v.position.x = x;
                v.position.y = y;
                v.position.z = z;
            };
            if (!p.texture)
            {
                // A line, bottom (the head) to top (the tail).
                zge::matrixRow(*camera, 3) = p.position;
                vertex(corner0, p.tail.x, p.tail.y, p.tail.z);
                corner0.u = 0.0f;
                corner0.v = 0.0f;
                vertex(corner1, p.position.x, p.position.y, p.position.z);
                corner1.u = 0.0f;
                corner1.v = 0.0f;
                line->texture = p.texture;
                zge::drawLines(kWorldPrimitive, line.address(), 1, 0);
            }
            else
            {
                // The camera's matrix moved to the top; the segment in the
                // camera's frame.
                zge::matrixRow(*camera, 3) = p.tail;
                bottom->x = p.position.x;
                bottom->y = p.position.y;
                bottom->z = p.position.z;
                top->x = p.tail.x;
                top->y = p.tail.y;
                top->z = p.tail.z;
                *bottom = zge::transformed(*toCamera, *bottom);
                *top = zge::transformed(*toCamera, *top);
                along->x = bottom->x - top->x;
                along->y = bottom->y - top->y;
                along->z = bottom->z - top->z;
                // The corners in turn, so the ribbon faces the camera either way.
                for (uint32_t i = 0; i < 4; ++i)
                    ribbon->vertices[i] = ee::Float(0.0f) < along->x ? &corners[i] : &corners[3 - i];
                // The ribbon: `size` either side of the segment (y in the
                // camera's frame), to the world by the camera's matrix.
                const auto place = [&](QuadVertex &v, ee::Float x, ee::Float y, ee::Float z)
                {
                    vertex(v, x, y, z);
                    v.position = zge::transformed(*camera, v.position);
                };
                const ee::Float one = 1.0f;
                place(corner3, 0.0f, -p.size, 0.0f);
                corner3.v = 0.0f;
                corner3.u = one;
                place(corner2, 0.0f, p.size, 0.0f);
                corner2.u = 0.0f;
                corner2.v = 0.0f;
                place(corner1, along->x, along->y + p.size, along->z);
                corner1.u = 0.0f;
                corner1.v = one;
                place(corner0, along->x, along->y - p.size, along->z);
                corner0.u = one;
                corner0.v = one;
                ribbon->texture = p.texture;
                zge::drawPrimitiveQuads(kWorldPrimitive, ee::Ptr<uint32_t>(&ribbon->texture), 1, 0);

                // The glow over it.
                const ee::Float glowDown = -kGlowHalfWidth;
                const ee::Float glowUp = kGlowHalfWidth;
                colour[1] = kGlowGreen;
                colour[2] = kGlowBlue;
                colour[0] = kGlowRed;
                place(corner3, 0.0f, glowDown, 0.0f);
                corner3.u = one;
                corner3.v = 0.0f;
                place(corner2, 0.0f, glowUp, 0.0f);
                corner2.u = 0.0f;
                corner2.v = 0.0f;
                // y plus or minus 3 (the libgcc doubles).
                const auto offsetY = [&](ee::Float y, bool up)
                {
                    const uint64_t wide = sce::libgcc::fptodp(y);
                    const uint64_t moved = up ? sce::libgcc::dpadd(wide, kGlowHalfWidthDouble)
                                              : sce::libgcc::dpsub(wide, kGlowHalfWidthDouble);
                    return sce::libgcc::dptofp(moved);
                };
                const ee::Float upY = offsetY(along->y, true);
                place(corner1, along->x, upY, along->z);
                corner1.v = one;
                corner1.u = 0.0f;
                const ee::Float downY = offsetY(along->y, false);
                place(corner0, along->x, downY, along->z);
                corner0.u = one;
                corner0.v = one;
                zge::drawPrimitiveQuads(kWorldPrimitive, ee::Ptr<uint32_t>(&ribbon->texture), 1, 0);

                // A fork's segment: the glow mirrored across the top.
                if (p.flags & kLightningFork)
                {
                    place(corner3, 0.0f, glowDown, 0.0f);
                    corner3.u = one;
                    corner3.v = 0.0f;
                    place(corner2, 0.0f, glowUp, 0.0f);
                    corner2.u = 0.0f;
                    corner2.v = 0.0f;
                    const ee::Float forkUpY = offsetY(along->y, true);
                    place(corner1, -along->x, forkUpY, along->z);
                    corner1.v = one;
                    corner1.u = 0.0f;
                    const ee::Float forkDownY = offsetY(along->y, false);
                    place(corner0, -along->x, forkDownY, along->z);
                    ribbon->texture = *g_forkTexture;
                    corner0.v = one;
                    corner0.u = one;
                    zge::drawPrimitiveQuads(kWorldPrimitive, ee::Ptr<uint32_t>(&ribbon->texture), 1, 0);
                }
            }
            *drawn = *drawn + 1;
        }
        return *drawn;
    }
    READABLE_FUNCTION_WAITS(drawLightning, Verified);
}
