#pragma once

// ZGE4's queue of billboard effects (billboards.cpp; BMX XXX only): the
// "regular billboard" particle type keeps its billboards in effect records of
// the queue's pool, each a ring of packed billboards VU1 expands; the type
// queues each effect it draws while the scene is drawn, and the view's packet
// sends them to VU1 after the scene.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/render/material.h"
#include "zge/scene/camera.h"
#include "zge/scene/particle_types.h"

#include <cstdint>

namespace zge
{
    struct BillboardEffect;

    // A billboard as the ring packs it (a quadword): its position and velocity
    // in 1024ths, and the effect's frame it was born in (billboardAge counts
    // from it). VU1 moves and fades it by the effect's parameters.
    struct PackedBillboard
    {
        int16_t x, y, z;    // +0x00 position times 1024
        uint16_t bornFrame; // +0x06
        int16_t vx, vy, vz; // +0x08 velocity times 1024, a second
        int16_t velocityW;  // +0x0E 0: the second V4-16 vector's w as VU1 unpacks it
    };
    static_assert(sizeof(PackedBillboard) == 0x10);

    // What a billboard starts with (addBillboard's second argument): where
    // and how fast, as floats.
    struct BillboardStart
    {
        Vec4 position; // +0x00
        Vec4 velocity; // +0x10 a second
    };
    static_assert(sizeof(BillboardStart) == 0x20);

    // What spawns one billboard into an effect (setBillboardSpawner; the
    // billboard type's, or the default's for an effect with no look): 0 when
    // none could be made (the effect spawns no more this frame).
    using BillboardSpawner = ee::FunctionPtr<int32_t(ee::Ptr<BillboardEffect> effect, uint32_t context)>;

    // How an effect's billboards look and behave (setBillboardEffectLook's
    // argument, 0x50 bytes; g_defaultBillboardLook when the effect has none):
    // what the billboard type builds from an emitter's data and look.
    struct BillboardLook
    {
        ee::Ptr<Material> material;  // +0x00
        ParticleColour startColour;  // +0x04 a billboard's colour as born
        ParticleColour endColour;    // +0x08 and at the end of its life
        ee::Float startSize;         // +0x0C
        ee::Float endSize;           // +0x10
        ee::Float life;              // +0x14 seconds
        ee::Float spawnRate;         // +0x18 billboards a second the effect spawns by itself
        uint32_t _1c;
        Vec4 acceleration;           // +0x20 what the billboards accelerate by (VU1 applies it)
        uint32_t capacity;           // +0x30 the ring's billboards
        ee::Float fadeInSeconds;     // +0x34 a billboard fades in over, from its birth
        ee::Float fadeOutSeconds;    // +0x38 and out over, to the end of its life
        BillboardSpawner spawner;    // +0x3C
        uint32_t spawnerContext;     // +0x40
        ee::Float viewRadius;        // +0x44 the effect's view sphere, about the matrix's position
        ee::Float viewDistance;      // +0x48 the view depth past which the effect fades out and is not drawn
        uint32_t _4c;
    };
    static_assert(sizeof(BillboardLook) == 0x50);

    // initBillboardQueue (initRenderer): the effects' pool and the queue made; 1.
    uint32_t initBillboardQueue();
    // freeBillboardQueue (shutDownRenderer): the queue and the pool freed (when made); 1.
    uint32_t freeBillboardQueue();
    // drawQueuedBillboards (beginViewPacket): every queued effect's billboards sent to VU1, the queue emptied.
    void drawQueuedBillboards();
    // beginViewBillboardsNoop (ZGE4 only; beginViewDraws' step for the queue): nothing.
    void beginViewBillboardsNoop();

    // An effect from the pool with the look (0: the default's), none of its billboards yet, emitting.
    ee::Ptr<BillboardEffect> takeBillboardEffect(ee::Ptr<BillboardLook> look);
    // The effect's ring freed and the effect back to the pool.
    void freeBillboardEffect(ee::Ptr<BillboardEffect> effect);
    // The look applied to the effect (the ring made when it has none yet).
    void setBillboardEffectLook(ee::Ptr<BillboardEffect> effect, ee::Ptr<BillboardLook> look);
    // The effect's matrix, and its view sphere and region when the position moved.
    void setBillboardEffectMatrix(ee::Ptr<BillboardEffect> effect, ee::Ptr<Matrix> matrix);
    void setBillboardEffectEmitting(ee::Ptr<BillboardEffect> effect, uint32_t on);
    void setBillboardSpawner(ee::Ptr<BillboardEffect> effect, BillboardSpawner spawner, uint32_t context);
    // An effect still in use as the pool goes (the pool's destroyRecord): its release deferred.
    void deferBillboardEffectRelease(ee::Ptr<BillboardEffect> effect);
    // A frame of the effect: the expired billboards dropped, the owed ones spawned; its billboard count.
    uint32_t updateBillboardEffect(ee::Ptr<BillboardEffect> effect);
    // The effect queued for the view's packet when in view; 0.
    uint32_t queueBillboardEffect(ee::Ptr<BillboardEffect> effect);
    // A billboard into the effect's ring (0: the ring is full); 1.
    uint32_t addBillboard(ee::Ptr<BillboardEffect> effect, ee::Ptr<BillboardStart> start);
}
