#pragma once

// Lighting of skinned models: three
// directional lights and an ambient term, uploaded to VU1 per node.

#include "ee/ee.h"
#include "zge/math/types.h"
#include "sym/zge/scene/light.h.h"

namespace zge
{
    // A node's own lights (Node::lights, with kNodeOwnLights). The renderer
    // multiplies the directions by the node's world matrix before upload.
    struct LightBlock
    {
        // Columns 0-2 are the directions of lights 0-2 (rows x, y, z).
        Vec4 directions[4]; // +0x00
        // Rows 0-2: the colours of lights 0-2; row 3: ambient. 1.0 is the
        // GS's 0x80 (full brightness).
        Vec4 colours[4];    // +0x40
    };
    static_assert(sizeof(LightBlock) == 0x80);

    // The scene's lights, used by nodes without their own: the same layout
    // as a light block's two halves.
    inline ee::Global<Vec4[4], sym::g_globalLights> g_globalLightDirections;
    inline ee::Global<Vec4[4], sym::g_globalLightColours> g_globalLightColours;
}

namespace zge
{
    // (light_objects.cpp)
    uint32_t lightObjectStubZero();
}
