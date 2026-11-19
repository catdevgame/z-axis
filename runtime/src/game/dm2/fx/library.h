#pragma once

// DM2's effects library (fx.c; library.cpp): the effects' textures read from
// the effects' zfile library (assets\fx.zal) into materials, and the effects'
// two tasks.

#include "ee/ee.h"
#include "zge/assets/zfile_library.h"

#include <cstdint>

namespace game::dm2
{
    // loadFxMaterials' flags: the material's state and how its image is read.
    enum FxMaterialFlags : uint32_t
    {
        kFxStateMask = 0x1FFFFFFFu,  // register 5's value
        kFxRegister7 = 0x20000000u,  // register 7 set (0)
        kFxKindTwo = 0x40000000u,    // the image read as kind 2 (else as the file's own reader says)
        kFxRegister3 = 0x80000000u,  // register 3 set (0)
    };

    // `count` textures, assets 0 of the library's groups `firstGroup` on, each
    // made a material of its own into `materials` (0 where the material could
    // not be made; a slot is left alone when the image or texture fails).
    void loadFxMaterials(ee::Ptr<zge::ZfileLibrary> library, uint32_t firstGroup, uint32_t name, uint32_t count, uint32_t flags,
                         ee::Ptr<uint32_t> materials);
    // `count` materials let go.
    void releaseFxMaterials(ee::Ptr<uint32_t> materials, uint32_t count);

    // The level's lens flare put at the level's "LensFlare" point, if it
    // has one (made with its textures from `library` the first time).
    void placeLevelLensFlare(ee::Ptr<zge::ZfileLibrary> library);

    // The level's effects set up: the effects' library opened, the engine's
    // effect tables and the effect manager started, the object the game's
    // effects are spawned at made, the nine effect classes registered with
    // their names, affectors and starters, the effects' textures read (the
    // loading screen's progress told as "FX", 0 to 7), the level's lens
    // flare placed, the water wake and the trees' fade set up, and the
    // effects' two tasks started.
    void initInlineFx();
    // All of that undone.
    void shutdownInlineFx();
    // The water wake's class ("WaterWake") registered, with its texture and
    // its object; once, until freeWaterWake.
    void initWaterWake();
    void freeWaterWake();

    // The effects' first task: while a depth range script runs, the camera's
    // scene camera gets the script's next pair each frame, and (0, 0) at its
    // end. Returns 1.
    uint32_t depthRangeScriptFrame();
    // The built-in script started: eight frames of (0, y), y -5, 4, -4, 3,
    // -3, 2, 2, -1 (in 200ths).
    void startDepthRangeScript();
    // The effects' second task: a frame counted. Returns 1.
    uint32_t countEffectsFrame();
}
