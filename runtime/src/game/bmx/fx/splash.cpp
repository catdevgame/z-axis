// BMX XXX's splash (splash.h). BMX XXX's own code (fx.c's block),
// Aggressive Inline's line (game/aggressive/fx/blood_splash.cpp drawSplash)
// with no texture: BMX XXX's drops are drawn untextured.

#include "splash.h"

#include "effects_clock.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/render/primitives.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include <cstring>
#include "sym/bmx/fx/splash.cpp.h"

namespace game::bmx
{
    namespace
    {
        // drawPrimitiveQuads' flags for a quad in the world.
        constexpr uint32_t kWorldQuads = 2;
        // A drop's alpha: its milliseconds left / 8 over its last 0x400, else
        // half; none with more than 30 seconds left.
        constexpr uint32_t kLongestLife = 30000, kFadeMs = 0x400;
        constexpr uint8_t kHalfAlpha = 0x80;
        uint8_t dropAlpha(uint32_t left)
        {
            if (left > kLongestLife)
                left = 0;
            return static_cast<uint8_t>(left < kFadeMs ? left >> 3 : kHalfAlpha);
        }
    }

    // drawSplash ("splash" +0x20): each drop a square of its size about it in
    // the camera's x and y, grey as light as its alpha, untextured. Returns
    // how many it drew.
    int32_t drawSplash(uint32_t, ee::Ptr<Emitter> emitter)
    {
        int32_t drawn = 0;
        const uint32_t now = static_cast<uint32_t>(effectsClock());
        ee::Local<Quad> quad;
        ee::Local<QuadVertex[4]> vertices;
        ee::Local<zge::Matrix> camera;
        ee::Local<uint8_t[4]> colour;
        for (uint32_t i = 0; i < 4; ++i)
            quad->corners[i] = &vertices[i];
        const ee::Ptr<zge::SceneObject> cameraObject(zge::cameraObject(ee::Ptr<zge::Camera>(*zge::g_currentCamera)));
        *camera = *zge::objectMatrix(cameraObject, 1);
        for (uint32_t particle = first(*emitter->particles); particle; particle = next(*emitter->particles), ++drawn)
        {
            const Particle &drop = *ee::Ptr<Particle>(particle);
            camera->translation() = drop.position;
            std::memcpy(&colour[0], drop.colour, 4);
            colour[3] = dropAlpha(drop.lifetime - (now - drop.born));
            colour[0] = colour[3];
            colour[2] = colour[3];
            colour[1] = colour[3];
            // The corners from the last to the first: (-, -), (-, +), (+, +),
            // (+, -) in the camera's x and y, each with its corner of the
            // texture's place.
            for (int32_t i = 3; i >= 0; --i)
            {
                QuadVertex &vertex = vertices[i];
                const bool left = i >= 2;          // corners 2 and 3
                const bool low = i == 0 || i == 3; // corners 0 and 3
                std::memcpy(vertex.colour, &colour[0], 4);
                vertex.position.x = left ? -drop.size : drop.size;
                vertex.position.z = 0.0f;
                vertex.position.y = low ? -drop.size : drop.size;
                vertex.position = zge::transformed(*camera, vertex.position);
                vertex.u = left ? 0.0f : 1.0f;
                vertex.v = low ? 0.0f : 1.0f;
            }
            quad->texture = 0;
            zge::drawPrimitiveQuads(kWorldQuads, quad, 1, 0);
        }
        return drawn;
    }
    READABLE_FUNCTION_WAITS(drawSplash, Draft);
}
