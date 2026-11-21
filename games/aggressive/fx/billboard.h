#pragma once

// The particles' camera-facing squares (billboards), as the effects' draws
// build them for the primitive API (zge drawPrimitives, kind 2: in the
// world): a quad record of four vertices whose positions are a corner
// offset (+-size, +-size, 0) taken to the world by the camera's matrix with
// the particle's position as its translation. The draws' frames share the
// layout: the colour at +0x00, the quad record at +0x10, the four vertices
// at +0x20, their list at +0xA0, the matrix at +0xB0, the offset at +0xF0
// (Billboard).

#include "aggressive/conversions.h"
#include "aggressive/fx/particles.h"
#include "aggressive/globals.h"
#include "ee/abi.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/render/draw.h"
#include "zge/render/primitives.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/fx/billboard.h.h"

namespace aggressive::fx
{
    // The primitive API's vertex (as zgui's quads; effects.cpp).
    struct QuadVertex
    {
        zge::Vec4 position; // w: VU0 vf9's, as SQC2 stores it
        ee::Float u, v;
        uint8_t colour[4];
        uint32_t _1c;
    };
    static_assert(sizeof(QuadVertex) == 0x20 && offsetof(QuadVertex, colour) == 0x18);

    // The draw's quad: {texture, the vertex list, 4}.
    struct QuadRecord
    {
        uint32_t texture;
        ee::Ptr<ee::Ptr<QuadVertex>> vertices; // its corners, in the API's order
        uint32_t count;
    };

    // The primitive API's kind for quads in the world.
    inline constexpr uint32_t kWorldPrimitive = 2;

    // A quad as the draws build it: its record, four corners and their list
    // (0, 1, 3, 2: the API goes round the square).
    struct Quad
    {
        QuadRecord record;           // +0x00
        uint32_t _0c;
        QuadVertex corners[4];       // +0x10
        ee::Ptr<QuadVertex> list[4]; // +0x90
    };
    static_assert(offsetof(Quad, corners) == 0x10 && offsetof(Quad, list) == 0x90 && sizeof(Quad) == 0xA0);

    // The quad's record pointed at its four corners.
    inline void listCorners(Quad &quad)
    {
        quad.record.vertices = &quad.list[0];
        quad.record.count = 4;
        quad.list[1] = &quad.corners[1];
        quad.list[2] = &quad.corners[3];
        quad.list[3] = &quad.corners[2];
        quad.list[0] = &quad.corners[0];
    }

    // A particle's camera-facing square, as the particle draws build it on
    // their stack: the colour, the quad, the camera's matrix with the
    // particle's position as its translation, and the corner being placed.
    struct Billboard
    {
        uint8_t colour[4];           // +0x00 RGBA
        uint8_t _04[0x0C];
        Quad quad;                   // +0x10
        zge::Matrix camera;          // +0xB0
        zge::Vec4 corner;            // +0xF0 (+-size, +-size, 0) in the camera's frame
    };
    static_assert(offsetof(Billboard, quad) == 0x10 && offsetof(Billboard, camera) == 0xB0);
    static_assert(offsetof(Billboard, corner) == 0xF0 && sizeof(Billboard) == 0x100);

    // The camera the scene is drawn from.
    inline ee::Global<uint32_t, sym::g_currentCamera> g_currentCamera;

    // The current camera's world matrix.
    inline zge::Matrix &currentCameraMatrix()
    {
        const ee::Ptr<zge::SceneObject> camera(zge::cameraObject(ee::Ptr<zge::Camera>(*g_currentCamera)));
        return *zge::objectMatrix(camera, 1);
    }

    // An emitter's draw method (a class's +0x20).
    using EmitterDraw = ee::FunctionPtr<int32_t(uint32_t effectClass, ee::Ptr<Emitter> emitter)>;

    // A visibility area of the level, as the camera's and the placed
    // effects' are.
    struct ViewArea
    {
        uint32_t _00;
        uint32_t mark; // +0x04 the frame's out-of-view mark when out of the camera's view
    };
    inline ee::Global<uint32_t, sym::g_outOfViewMark> g_outOfViewMark;

    // An emitter's draw (`draw`, the class's own) unless the placed effect it
    // belongs to is hidden this frame: its node was not drawn in the
    // camera's last frame, or (no node) its area is out of the camera's
    // view, or it is farther than its range from the camera (outside split
    // screen the distance is measured twice). The effect keeps whether it is
    // hidden. Returns the draw's count, 0 when hidden. The camera is the
    // game's own (zge/host/game_view.h): with a hidden follow camera its
    // eye, not the drawn one's.
    inline int32_t drawUnlessHidden(uint32_t effectClass, ee::Ptr<Emitter> emitter, EmitterDraw draw)
    {
        ee::Local<zge::Matrix> camera;
        ee::Local<zge::Vec4> apart; // the effect from the camera
        ee::Local<zge::Matrix> firstCamera;
        ee::Local<zge::Vec4> eye;
        const ee::Ptr<UserEffect> effect(emitter->owner);
        if (effect)
        {
            if (effect->node)
                effect->hidden = zge::nodeNotDrawn(effect->node); // 0: drawn in the camera's last frame
            else
            {
                const uint32_t seen = zge::cameraArea(ee::Ptr<zge::Camera>(*g_currentCamera)); // the camera's own area, or 0
                const ee::Ptr<ViewArea> area(effect->area);
                effect->hidden = seen && area && area->mark == *g_outOfViewMark ? 1u : 0u;
            }
            if (effect->hidden)
                return 0;
            if (*g_gameType != GameType::SplitScreen)
            {
                *firstCamera = currentCameraMatrix();
                *eye = zge::matrixRow(*firstCamera, 3);
                // (the game's own camera's eye, a hidden one's: zge/host/game_view.h)
                if (zge::gameCameraElsewhere(ee::Ptr<zge::Camera>(*g_currentCamera)))
                    *eye = zge::gameCameraPosition(ee::Ptr<zge::Camera>(*g_currentCamera));
                *apart = effect->position - *eye;
                if (effect->range < zge::vec3Length(asVec3(*apart)))
                    effect->hidden = 1;
            }
            if (effect->hidden)
                return 0;
            *camera = currentCameraMatrix();
            *apart = effect->position - zge::matrixRow(*camera, 3);
            if (zge::gameCameraElsewhere(ee::Ptr<zge::Camera>(*g_currentCamera))) // (the draw advances the puffs: as the game's)
                *apart = effect->position - zge::gameCameraPosition(ee::Ptr<zge::Camera>(*g_currentCamera));
            if (!(zge::vec3Length(asVec3(*apart)) <= effect->range))
                return 0;
        }
        return draw(effectClass, emitter);
    }

    // The billboard moved to `position`, facing the camera (the camera's
    // world matrix with the position as its translation).
    inline void placeBillboard(Billboard &billboard, const zge::Matrix &camera, const zge::Vec4 &position)
    {
        billboard.camera = camera;
        zge::matrixRow(billboard.camera, 3) = position;
    }

    // The billboard's four corners, `size` out from its centre each way,
    // each with the billboard's colour and its corner of the texture.
    inline void placeCorners(Billboard &billboard, ee::Float size)
    {
        for (uint32_t i = 0; i < 4; ++i)
        {
            const bool left = ((i + 1) & 2u) != 0;
            const bool bottom = (i & 2u) != 0;
            QuadVertex &corner = billboard.quad.corners[i];
            std::memcpy(corner.colour, billboard.colour, 4);
            corner.u = left ? 0.0f : 1.0f;
            corner.v = bottom ? 1.0f : 0.0f;
            billboard.corner.x = left ? -size : size;
            billboard.corner.y = bottom ? size : -size;
            billboard.corner.z = 0.0f;
            corner.position = zge::transformed(billboard.camera, billboard.corner);
        }
    }

    // The billboard drawn: its quad, in the world.
    inline int32_t drawBillboard(Billboard &billboard)
    {
        return zge::drawPrimitives(kWorldPrimitive, &billboard.quad.record, 1, 0);
    }

    // A particle's alpha: `rate` a millisecond of the time it has left,
    // at most `full` (the cap tested with `testRate`, the same value read
    // from its own word).
    inline uint8_t fadedAlpha(uint32_t left, const ee::Float &testRate, const ee::Float &rate, int32_t full)
    {
        uint8_t alpha;
        if (ee::toInt(unsignedToFloat(left) * testRate) < full + 1)
            alpha = static_cast<uint8_t>(ee::toInt(unsignedToFloat(left) * rate));
        else
            alpha = static_cast<uint8_t>(full);
        return alpha;
    }

    // What a shard starts from ("shard": the named effect's parameters;
    // with none, the defaults at shardDefaults: a small triangle, living 2
    // seconds; debris makes them, fx/spawns.cpp).
    struct ShardParameters
    {
        uint32_t flags;              // +0x00 (Shard::shardFlags)
        uint8_t _04[0x0C];
        zge::Vec4 frame[4];          // +0x10 its orientation; row 3 (+0x40) its position
        QuadVertex corners[3];       // +0x50 the triangle in its own frame
        zge::Vec4 velocity;          // +0xB0
        zge::EulerAngles spin;       // +0xC0 a tick's turn
        uint8_t _c6[2];
        uint32_t lifetime;           // +0xC8 milliseconds
        uint32_t texture;            // +0xCC
        uint8_t colour[4];           // +0xD0 RGBA
        ee::Float floorHeight;       // +0xD4 (Shard::floorHeight)
    };
    static_assert(offsetof(ShardParameters, corners) == 0x50 && offsetof(ShardParameters, velocity) == 0xB0);
    static_assert(offsetof(ShardParameters, spin) == 0xC0 && offsetof(ShardParameters, lifetime) == 0xC8);
    static_assert(offsetof(ShardParameters, colour) == 0xD0 && offsetof(ShardParameters, floorHeight) == 0xD4);

    // A shard's flags (its parameters'): 1 and 2 choose its draw's kind
    // and colours, 4 a second draw (drawShards's tests); 8 still (the
    // "floorcoll" affector sets it once a shard comes to rest on its floor).
    constexpr uint32_t kShardStill = 0x8; // its frame not turned by its spin, not moved by the affectors

    // A shard (0x120 bytes; shards.cpp).
    struct Shard : Particle
    {
        zge::Matrix frame;           // +0x70 its orientation and position (turned by its spin)
        QuadVertex corners[3];       // +0xB0
        ee::Float floorHeight;       // +0x110 the parameters' (y down: the floor "floorcoll" bounces it off)
        uint32_t shardFlags;         // +0x114
        uint8_t _118[8];
    };
    static_assert(offsetof(Shard, frame) == 0x70 && offsetof(Shard, corners) == 0xB0);
    static_assert(offsetof(Shard, floorHeight) == 0x110 && offsetof(Shard, shardFlags) == 0x114 && sizeof(Shard) == 0x120);
}
