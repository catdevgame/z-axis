#pragma once

// The full-screen effect manager's records (zge/render/fx_manager.cpp): an
// effect registered by name, and a run of one. The effects themselves
// (screen_effects.cpp's motion blur, lens_flare.cpp) are a definition of
// these methods each.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "zge/container/tree.h"

#include <cstddef>
#include <cstdint>

namespace sce
{
    struct Vif1Packet;
}

namespace zge
{
    struct Camera; // zge/scene/camera.h

    struct FullScreenEffect;
    // A running full-screen effect (a record of g_fullScreenEffectPool).
    struct RunningEffect
    {
        ee::Ptr<FullScreenEffect> effect; // +0x00
        uint32_t flags;                   // +0x04 RunningEffectFlags
        uint16_t clearedAtStart;          // +0x08 the start clears it; nothing found reads it
        uint16_t _0a;
        uint32_t data;                    // +0x0C the effect's own (the motion blur's camera, the lens flare's table)
        // +0x10 the effect's own counter: the lens flare counts its frames
        // through each occlusion test here. The start does not clear it.
        uint32_t counter;
        uint32_t _14;
    };
    static_assert(offsetof(RunningEffect, data) == 0x0C && offsetof(RunningEffect, counter) == 0x10 && sizeof(RunningEffect) == 0x18);

    // A full-screen effect (0x34 bytes, a copy of the definition
    // registerFullScreenEffect is given, in the tree by its name's hash).
    struct FullScreenEffect
    {
        SearchTreeNode node;   // +0x00
        uint32_t nameHash;     // +0x08 the tree's value
        // +0x0C bits 0-2 FullScreenEffectFlags; bits 3-12 its runs (start
        // adds one, stop takes one; it can go only at 0)
        uint32_t flags;
        uint32_t _10;
        uint32_t clearedAtRegister; // +0x14 registerFullScreenEffect clears it; nothing found reads it
        ee::FunctionPtr<void(ee::Ptr<FullScreenEffect> effect)> registered; // +0x18
        ee::FunctionPtr<void(ee::Ptr<FullScreenEffect> effect)> released;   // +0x1C as it goes
        ee::FunctionPtr<void(ee::Ptr<RunningEffect> run, uint32_t request)> start; // +0x20
        ee::FunctionPtr<void(ee::Ptr<RunningEffect> run)> update;           // +0x24 every frame, while it updates
        ee::FunctionPtr<void(ee::Ptr<RunningEffect> run)> draw;             // +0x28 every frame, while it draws
        ee::FunctionPtr<void(ee::Ptr<RunningEffect> run, ee::Ptr<FullScreenEffect> effect)> stop; // +0x2C
        ee::FunctionPtr<uint32_t(ee::Ptr<RunningEffect> run, uint32_t value)> setValue; // +0x30
    };
    static_assert(offsetof(FullScreenEffect, flags) == 0x0C && offsetof(FullScreenEffect, registered) == 0x18);
    static_assert(offsetof(FullScreenEffect, setValue) == 0x30 && sizeof(FullScreenEffect) == 0x34);

    enum FullScreenEffectFlags : uint32_t
    {
        kEffectHandsBackRun = 1u, // startFullScreenEffect returns the run (else 0); a stop inside the update pass leaves it running
        kEffectUpdates = 2u,
        kEffectDraws = 4u,
    };
}

namespace zge
{
    // (screen_effects.cpp)
    uint32_t initScreenOverlay();
    uint32_t shutDownScreenOverlay();
    uint32_t initFramePrologue();
    uint32_t shutDownFramePrologue();
    int32_t buildFogPlanes();
    void registerMotionBlurEffect();
    void drawBlur(ee::Ptr<Camera> camera, ee::Ptr<sce::Vif1Packet> packet);
    uint32_t effectAllocate(uint32_t bytes);
    int32_t initEffectArrays();
}

namespace zge
{
    // (fx_manager.cpp)
    int32_t initEffectManager();
    int32_t stopFullScreenEffect(ee::Ptr<RunningEffect> run);
    uint32_t shutdownEffectManager();
    uint32_t registerFullScreenEffect(uint32_t name, ee::Ptr<FullScreenEffect> definition);
    ee::Ptr<RunningEffect> startFullScreenEffect(uint32_t hash, ee::Address request);
    int32_t setEffectValue(ee::Ptr<RunningEffect> run, ee::Address value);
}

namespace zge
{
    // (lens_flare.cpp)
    void registerLensFlareEffect();
}
