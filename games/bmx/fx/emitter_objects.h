#pragma once
// BMX XXX's pool of particle emitter objects (fx.c; emitter_objects.cpp):
// the level's dust, smoke, spray and fire, each a copy of one of the
// effects library's emitter objects ("ZPE_SFX_...") put where it is wanted.

#include "ee/ee.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/fx/emitter_objects.h.h"

namespace game::bmx
{
    // The kinds, in the order of the kind table (each named by its
    // emitter object in the effects library).
    enum EmitterObjectKind : uint32_t
    {
        kEmitterSparks = 0,        // "ZPE_SFX_sparks"
        kEmitterDust = 1,          // "ZPE_SFX_dust"
        kEmitterConcreteDust = 2,  // "ZPE_SFX_concrete_dust"
        kEmitterWaterSpray = 3,    // "ZPE_SFX_water_spray"
        kEmitterWaterSplash = 4,   // "ZPE_SFX_water_splash"
        kEmitterSmoke = 5,         // "ZPE_SFX_smoke"
        kEmitterWeaponSmoke = 6,   // "ZPE_SFX_weapon_smoke"
        kEmitterExplosionFire = 7, // "ZPE_SFX_explosion_fire"
    };

    // One of the pool's 64 emitter objects.
    struct EmitterObjectSlot
    {
        ee::Ptr<zge::SceneObject> object; // +0x00 the copy (0: none made yet)
        uint32_t kind;                    // +0x04 what it was last taken as
        // +0x08 a time on the effects' clock: with every slot busy, the one
        // with the lowest is taken over. Nothing written here sets it.
        ee::Float clockMs;
    };
    static_assert(sizeof(EmitterObjectSlot) == 0x0C);

    // A kind of emitter object: its name in the effects library, the
    // library's object of that name (the effects' set-up looks it up; 0:
    // the library has none), and its flags.
    struct EmitterObjectKindRecord
    {
        uint32_t name;                    // +0x00 the text
        ee::Ptr<zge::SceneObject> source; // +0x04
        uint32_t flags;                   // +0x08 kKindNeverTakesOver
    };
    static_assert(sizeof(EmitterObjectKindRecord) == 0x0C);
    // A kind that waits for a free slot rather than take a busy one over.
    constexpr uint32_t kKindNeverTakesOver = 0x1;

    constexpr uint32_t kEmitterObjectKinds = 8;
    constexpr uint32_t kEmitterObjectSlots = 0x40;
    inline ee::Global<EmitterObjectKindRecord[kEmitterObjectKinds], sym::g_emitterObjectKinds> g_emitterObjectKinds;
    inline ee::Global<EmitterObjectSlot[kEmitterObjectSlots], sym::g_emitterObjects> g_emitterObjects;
    // Where the next search for a slot starts.
    inline ee::Global<uint32_t, sym::g_nextEmitterObject> g_nextEmitterObject;

    // A slot with a fresh copy of the kind's emitter object: the next slot
    // from the cursor whose object is gone or has no particle left, else
    // the busy one with the lowest clock (never, for a kind that may not
    // take one over). 0 when the kind has no emitter object in the library
    // or no slot can be had.
    ee::Ptr<EmitterObjectSlot> takeEmitterObject(uint32_t kind);
    // A slot's object deleted.
    void freeEmitterObject(ee::Ptr<EmitterObjectSlot> slot);

    // A puff of concrete dust, and of dust, at a point.
    void spawnConcreteDust(ee::Ptr<zge::Vec4> position);
    void spawnDust(ee::Ptr<zge::Vec4> position);
    // Smoke at a point (0: where the library's object is), turned by a
    // matrix (0: as it is), its speed, rate, size and life scaled; an
    // explosion's fire at a point, its rate and size scaled. (The kind
    // table's "ZPE_SFX_smoke" and "ZPE_SFX_explosion_fire".)
    void spawnSmoke(ee::Ptr<zge::Vec4> position, ee::Ptr<zge::Matrix> rotation, ee::Float speed, ee::Float rate,
                          ee::Float size, ee::Float life);
    void spawnExplosionFire(ee::Ptr<zge::Vec4> position, ee::Float rate, ee::Float size);
}
