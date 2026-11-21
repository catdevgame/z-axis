#pragma once
// BMX XXX's effects library (fx.c; library.cpp): what a level's load sets up
// for its effects and its end takes down: the effect classes by name, their
// affectors and starters, the emitter objects and textures of the effects'
// zfile library (assets\fx.zal), and the effects' two tasks.

#include "ee/ee.h"
#include "zge/assets/zfile_library.h"

#include <cstdint>

namespace game::bmx
{
    // loadFxMaterials' flags: the material's state and how its image is read.
    enum FxMaterialFlags : uint32_t
    {
        kFxBlendModeMask = 0x0FFFFFFFu, // the material's blend mode
        kFxKindTwo = 0x10000000u,       // the image read as kind 2
        kFxTwoSidedOff = 0x20000000u,   // the two-sided state set 0
        kFxKindThree = 0x40000000u,     // the image read as kind 3 (else as the file's own reader says)
        kFxNoDepthTest = 0x80000000u,   // the depth test always passes
    };

    // The effects' set-up and shutdown.
    void initEffects();
    void shutdownEffects();
    // Every effect gone (a turn's start; the shutdown).
    void clearEffects();
    // The effects' first task: nothing. Returns 1.
    uint32_t emptyEffectsTask();

    // `count` textures, read one after another from the asset of the library
    // named `assetName` on, each made a material of its own into `materials`
    // (0 where the material could not be made; a slot is left alone when the
    // image or texture fails). Nothing when the library has no such asset.
    // `name` only names the materials for a debug text nothing reads; the
    // last two words are not read (the set-up passes a step and 7 steps).
    void loadFxMaterials(ee::Ptr<zge::ZfileLibrary> library, const char *assetName, uint32_t name, uint32_t count, uint32_t flags,
                         ee::Ptr<uint32_t> materials, uint32_t step, uint32_t steps);
    // `count` materials let go.
    void releaseFxMaterials(ee::Ptr<uint32_t> materials, uint32_t count);
}
