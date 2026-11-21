#pragma once
// BMX XXX's water wake (fx.c; water_wake.cpp): the "WaterWake" effect
// class's texture, object and name, set up with the effects.

#include "ee/ee.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>

namespace game::bmx
{
    // What the wakes keep: one record (fx.c's own code reaches the object as
    // the record's +0x10 and the ready word as the material's address less 4).
    struct WaterWake
    {
        uint32_t ready;                    // +0x00 1 once set up
        uint32_t material;                 // +0x04 the wake's material
        uint8_t _08[0x08];
        ee::Ptr<zge::SceneObject> emitter; // +0x10 the emitter object every wake is spawned in
    };
    static_assert(offsetof(WaterWake, material) == 0x04 && offsetof(WaterWake, emitter) == 0x10);

    // The wake set up, once: its texture read from the effects' library, an
    // object for its effects made and its class registered by name.
    void initWaterWake();
    // The wake's class name unregistered and its texture let go.
    void freeWaterWake();
}
