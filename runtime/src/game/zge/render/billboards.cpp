// ZGE4's billboard queue (BMX XXX only; ZGE2 and ZGE3 have none). The
// "regular billboard" particle type keeps its billboards in effect records
// (BillboardEffect, from a pool of their own), each with a ring of packed
// billboards. While the scene is drawn the type's draw queues each effect in
// view (the queue below, a cursor array); the view's packet then sends every
// queued effect to VU1 (drawQueuedBillboards, beginViewPacket's step after
// the texture cache's hook): the effect's material, its parameters, the
// screen projection and the billboards' GIF tag, then its billboards by
// reference, a batch at a time.
//
// An effect counts frames (the display's vsyncs a second to a second); a
// billboard keeps the frame it was born in, and goes when the effect's life
// in frames is up for it. The effect spawns by its rate through its spawner
// (the billboard type's, or the default's) while emitting, and VU1 moves,
// sizes and fades its billboards from their start, the effect's acceleration
// and its parameters.

#include "billboards.h"
#include "display.h"
#include "material.h"
#include "primitives.h"
#include "renderer.h"
#include "vif_codes.h"

#include "ee/abi.h"
#include "ee/field.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/packet_calls.h"
#include "zge/container/array.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/pool.h"
#include "zge/platform/platform.h"
#include "zge/platform/tasks.h"
#include "zge/render/view_regions.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/scene/visibility_regions.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/billboards.cpp.h"

namespace zge
{
    namespace
    {
        // An effect's billboards: a ring of fixed-size elements (a packed
        // billboard a quadword) in the effect's buffer. The effect's set-up
        // makes it empty (read and write at the start); a billboard spawned
        // goes in at write while there is room, the oldest leaves at read,
        // each moving on and wrapping from the end to the start.
        struct BillboardRing
        {
            ee::Ptr<PackedBillboard> start; // +0x00 the buffer
            ee::Ptr<PackedBillboard> end;   // +0x04 start + capacity * elementBytes
            ee::Ptr<PackedBillboard> write; // +0x08 where the next billboard goes
            ee::Ptr<PackedBillboard> read;  // +0x0C the oldest billboard
            uint32_t elementBytes;    // +0x10 16
            uint32_t capacity;        // +0x14
            uint32_t count;           // +0x18 the billboards in it
        };
        static_assert(sizeof(BillboardRing) == 0x1C && offsetof(BillboardRing, read) == 0x0C &&
                      offsetof(BillboardRing, elementBytes) == 0x10 && offsetof(BillboardRing, count) == 0x18);
        constexpr uint32_t kBillboardBytes = sizeof(PackedBillboard);
    }

    // A billboard effect (0x110 bytes from the pool, in the uncached mirror):
    // its first ten quadwords are the parameters VU1's billboard program reads
    // (the matrix, colours, sizes, the fade; its billboard count among them).
    struct BillboardEffect
    {
        Matrix matrix;             // +0x00 the emitter's (setBillboardEffectMatrix); VU1 places the billboards by it
        Vec4 startColour;          // +0x40 the look's, each channel halved and made a float (the GS's 0x80 is 1)
        Vec4 endColour;            // +0x50
        Vec4 acceleration;         // +0x60 the look's
        ee::Float startSize;       // +0x70 the look's
        ee::Float endSize;         // +0x74
        int32_t billboardCount;    // +0x78 the billboards it has (its ring's count, kept here too)
        ee::Float constantOne;     // +0x7C 1 from when the effect is taken (VU1 reads it; nothing here does)
        ee::Float frame;           // +0x80 the effect's frame count, round at 4096 (a billboard keeps the one it was born in)
        ee::Float lifeFrames;      // +0x84 the look's life in frames (its seconds by the display's vsyncs a second)
        ee::Float fadeInFrames;    // +0x88 the look's fade in, in frames
        ee::Float fadeOutFrame;    // +0x8C the frame of a billboard's life it starts fading out at (its life less the fade out)
        ee::Float lifeInverse;     // +0x90 1 / lifeFrames
        ee::Float viewFade;        // +0x94 1, or how far short of its view distance the effect is, over the last four units (queueBillboardEffect)
        uint32_t _98, _9c;
        ee::Float spawnRate;       // +0xA0 the look's, billboards a second
        ee::Float spawnOwed;       // +0xA4 the part of a billboard the rate has built up
        uint32_t buffer;           // +0xA8 the ring's memory (allocateIn; freed with the effect)
        ee::Ptr<Material> material; // +0xAC
        BillboardRing billboards;  // +0xB0
        BillboardSpawner spawner;  // +0xCC what spawns one billboard (updateBillboardEffect, once a billboard owed)
        uint32_t spawnerContext;   // +0xD0
        uint32_t capacity;         // +0xD4 the ring's billboards (the look's; once the ring is made it cannot grow)
        uint32_t _d8, _dc;
        // +0xE0 the view test's sphere (partInView): the matrix's position as it last moved past
        // g_billboardMoveEpsilon, the view radius as its w and after it
        ViewSphere viewSphere;
        ee::Float viewDistance;    // +0x100 the look's: the view depth past which the effect fades out and is not drawn
        ee::Ptr<Region> region;    // +0x104 the view region at the sphere's centre (findRegionAt; 0: none)
        uint64_t flags;            // +0x108 BillboardEffectFlags
    };
    static_assert(sizeof(BillboardEffect) == 0x110 && offsetof(BillboardEffect, billboardCount) == 0x78 &&
                  offsetof(BillboardEffect, frame) == 0x80 && offsetof(BillboardEffect, spawnRate) == 0xA0 &&
                  offsetof(BillboardEffect, material) == 0xAC && offsetof(BillboardEffect, billboards) == 0xB0 &&
                  offsetof(BillboardEffect, spawner) == 0xCC && offsetof(BillboardEffect, viewSphere) == 0xE0 &&
                  offsetof(BillboardEffect, viewDistance) == 0x100 && offsetof(BillboardEffect, flags) == 0x108);

    namespace
    {
        enum BillboardEffectFlags : uint64_t
        {
            kEffectEmitting = 1ull << 0, // it spawns by its rate (setBillboardEffectEmitting)
            kEffectTaken = 1ull << 1,    // set as the effect is taken from the pool; nothing here reads it
        };
        constexpr int32_t kEffectParameterQwords = 10;

        // The queue's words: whether it is made, whether effects may be
        // queued (only initBillboardQueue sets it), the effects' pool and the
        // effects queued this frame.
        inline ee::Global<uint32_t, sym::bmx::g_billboardQueueReady> g_billboardQueueReady;
        inline ee::Global<uint32_t, sym::bmx::g_billboardQueueOpen> g_billboardQueueOpen;
        inline ee::Global<ee::Ptr<Pool>, sym::bmx::g_billboardEffectPool> g_billboardEffectPool;
        inline ee::Global<ee::Ptr<PointerArray>, sym::bmx::g_queuedBillboardEffects> g_queuedBillboardEffects;
        constexpr uint32_t kQueueWordsBytes = 0x10;
        constexpr uint32_t kQueueCapacity = 0x40;
        // The pool: 16 effects a block, in the uncached mirror (VU1's DMA
        // reads them); an effect still in use when the pool goes is released
        // through the deferred releases.
        constexpr uint32_t kEffectsABlock = 16u | static_cast<uint32_t>(kPoolUncached) << 16;

        // The look an effect with none gets (ELF data: no material, colours
        // 0x80000080 and 0x80008000, sizes 0.1 and 0.2, life 8, rate 480, an
        // acceleration of 1.8 up, no ring, fades 0.1 and 0.2,
        // spawnDefaultBillboard, view radius 5 and distance 50).
        inline ee::Global<BillboardLook, sym::bmx::g_defaultBillboardLook> g_defaultBillboardLook;
        // The least (x) and most (y) a billboard's position or velocity lane
        // may be, as the ring packs them in 1024ths of a signed halfword:
        // -31.9 and 31.9.
        inline ee::Global<Vec4, sym::bmx::g_billboardCoordinateLimits> g_billboardCoordinateLimits;
        // How far the matrix's position must move on an axis for the view
        // sphere and region to be found again: 0.00001.
        inline ee::Global<ee::Float, sym::bmx::g_billboardMoveEpsilon> g_billboardMoveEpsilon;
        // The default spawner's cube of starts, half a side: 0.1.
        inline ee::Global<ee::Float, sym::bmx::g_defaultBillboardSpawnSize> g_defaultBillboardSpawnSize;
        inline ee::Global<Matrix, sym::identityMatrix> g_identityMatrix;
        inline ee::Global<uint32_t, sym::g_outOfViewMark> g_outOfViewMark;

        constexpr ee::Float kFrameWrap = 4096.0f;       // the effect's frame count goes round at this
        constexpr ee::Float kPackedUnit = 1024.0f;      // a packed coordinate is this many to one
        constexpr ee::Float kRandomUnit = 4.656613e-10f; // 2 to the -31: a 31-bit random number to [0, 1)
        constexpr ee::Float kRandomLaneUnit = 3.0517578e-05f; // 2 to the -15: a signed 10-bit lane in its top bits to [-1, 1)
        constexpr uint32_t kRandomLaneBits = 10, kLaneToHalfword = 22, kHalfwordToWord = 16;
        constexpr ee::Float kViewFadeUnits = -4.0f, kViewFadeRate = -0.25f; // the fade over the last four units before the view distance
        constexpr ee::Float kDefaultBillboardFall = -7.0f; // the default spawner's velocity y (y is down)
        constexpr uint32_t kCameraFromWorldMatrix = 0;  // cachedMatrix's world -> camera matrix
        constexpr uint32_t kSpriteMaterial = 6;         // kStatePrimType's sprite
        constexpr uint32_t kRingMemoryKind = 0;         // allocateIn's kind for the ring (and freeIn's)

        // The billboards' GIF tag (ELF data): a sprite a billboard (PRE with
        // alpha blending), five registers: ST, RGBAQ, XYZ2, ST, XYZ2.
        inline ee::Global<ee::Qword, sym::bmx::g_billboardGifTag> g_billboardGifTag;
        // The view's screen projection (view -> GS screen; the display's).
        inline ee::Global<ee::Ptr<Matrix>, sym::g_screenProjection> g_screenProjection;
        constexpr int32_t kProjectionQwords = 4;

        // VU1's billboard program's entries: after the material, after the effect's parameters.
        constexpr uint32_t kAfterMaterial = 0x26, kAfterParameters = 0x24;
        // A material's GS block: 8 quadwords to VU1 0 (from TOPS).
        constexpr uint32_t kUnpackRegisters = vif::unpack(vif::kUnpackV4_32, 8, vif::kUnpackFromTops | 0u);
        // UNPACK's NUM is eight bits (256 is 0).
        constexpr uint32_t unpackCode(uint32_t format, uint32_t count, uint32_t vuAddress)
        {
            return vif::unpack(format, count & 0xFFu, vuAddress | vif::kUnpackFromTops);
        }
        // The billboards a program run takes, and a reference's at most; a
        // billboard is two V4-16 elements in VU1.
        constexpr uint32_t kBatchBillboards = 0x1C0, kReferenceBillboards = 0x80, kElementsABillboard = 2;

        // A colour's channel as VU1 takes it: the byte halved, as a float.
        inline ee::Float colourLane(uint8_t channel) { return ee::toFloat(static_cast<int32_t>(channel >> 1)); }
        inline Vec4 colourLanes(ParticleColour colour)
        {
            return Vec4{colourLane(colour.red), colourLane(colour.green), colourLane(colour.blue), colourLane(colour.alpha)};
        }
        // Seconds as the effect's frames.
        inline ee::Float frames(ee::Float seconds) { return seconds * ee::toFloat(static_cast<int32_t>(vsyncsASecond(*g_display))); }
        // The ring's next element after `at`, round from the end to the start.
        inline ee::Ptr<PackedBillboard> ringNext(const BillboardRing &ring, ee::Ptr<PackedBillboard> at)
        {
            const ee::Ptr<PackedBillboard> next = at + 1; // (an element is elementBytes, a packed billboard)
            return ee::Address(next) == ee::Address(ring.end) ? ring.start : next;
        }
        // A random number's lane of kRandomLaneBits bits, from `shift`, as a signed halfword in its top bits.
        inline int32_t randomLane(uint32_t random, uint32_t shift)
        {
            return static_cast<int32_t>((random >> shift) << kLaneToHalfword) >> kHalfwordToWord;
        }
    }

    // (container/array.cpp)
    ee::Ptr<PointerArray> newCursorArray(uint32_t capacity);
    void freePointerArray(ee::Ptr<PointerArray> array);
    void clearCursorArray(ee::Ptr<PointerArray> array, ee::FunctionPtr<int32_t(uint32_t item, uint32_t context)> visit, uint32_t context);

    // initBillboardQueue: the queue's words cleared, the queue made and open,
    // the effects' pool and the queue's array made. 1.
    inline uint32_t initBillboardQueue()
    {
        memoryFill(&*g_billboardQueueReady, 0, kQueueWordsBytes);
        *g_billboardQueueOpen = 1;
        *g_billboardQueueReady = 1;
        *g_billboardEffectPool = ee::Ptr<Pool>(createPool(sizeof(BillboardEffect), kEffectsABlock, sym::bmx::deferBillboardEffectRelease));
        *g_queuedBillboardEffects = newCursorArray(kQueueCapacity);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, initBillboardQueue);

    // freeBillboardQueue: the array and the pool freed and the queue marked
    // not made (when it was). 1.
    inline uint32_t freeBillboardQueue()
    {
        if (*g_billboardQueueReady != 0)
        {
            freePointerArray(*g_queuedBillboardEffects);
            destroyPool(*g_billboardEffectPool);
            *g_billboardQueueReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeBillboardQueue);

    // beginViewBillboardsNoop (beginViewDraws' step for the queue): nothing.
    inline void beginViewBillboardsNoop() {}
    READABLE_FUNCTION_ONLY(bmx, beginViewBillboardsNoop);

    // deferBillboardEffectRelease (the pool's destroyRecord: an effect still
    // in use as the pool goes): the effect's release deferred (releaseDeferred
    // frees it, freeBillboardEffect).
    inline void deferBillboardEffectRelease(ee::Ptr<BillboardEffect> effect)
    {
        game::tail(deferRelease, effect, kDeferredBillboardEffect);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, deferBillboardEffectRelease);

    // drawQueuedBillboards: when an effect is queued, the view's packet gets
    // FLUSHA, BASE 0 and OFFSET 0, then each effect with billboards: its
    // material listed for the texture uploads and its GS block referenced,
    // MSCAL 0x26; its parameters, the screen projection and the GIF tag
    // unpacked, MSCAL 0x24; then its billboards from the ring's oldest on, in
    // runs of at most 0x1C0 (each referenced in pieces that stop at the
    // ring's end and hold at most 0x80, the VU addresses going on), MSCNT
    // after each run. A FLUSHA after the last effect; the queue emptied.
    inline void drawQueuedBillboards()
    {
        const ee::Qword gifTag = *g_billboardGifTag;
        PointerArray &queued = **g_queuedBillboardEffects;
        ee::Ptr<BillboardEffect> effect(walkFirst(queued));
        if (effect)
        {
            const ee::Ptr<sce::Vif1Packet> packet(meshPacket());
            sce::vif1PacketCnt(packet, 0);
            sce::vif1PacketAddCode(packet, vif::flushA());
            sce::vif1PacketAddCode(packet, vif::base(0)); // BASE 0, OFFSET 0: no double buffering
            sce::vif1PacketAddCode(packet, vif::offset(0));
            do
            {
                if (effect->billboardCount != 0)
                {
                    const BillboardRing &ring = effect->billboards;
                    listPrimitiveMaterial(effect->material);
                    const ee::Ptr<Material> material = effect->material;
                    sce::vif1PacketRef(packet, ee::Ptr<uint64_t>(materialLayer(material, 0).gifTag), layerQwordCount(material, 0), vif::stcycl(4, 4),
                                       kUnpackRegisters, 0);
                    sce::vif1PacketCnt(packet, 0);
                    sce::vif1PacketAddCode(packet, vif::mscal(kAfterMaterial));
                    sce::vif1PacketAddCode(packet, vif::flushA());
                    sce::vif1PacketOpenUnpack(packet, vif::kUnpackFromTops, vif::kUnpackV4_32, 4, 4);
                    sce::vif1PacketAddQwords(packet, effect, kEffectParameterQwords);
                    sce::vif1PacketAddQwords(packet, *g_screenProjection, kProjectionQwords);
                    sce::vif1PacketAddQword(packet, gifTag);
                    sce::vif1PacketCloseUnpack(packet);
                    sce::vif1PacketAddCode(packet, vif::mscal(kAfterParameters));
                    sce::vif1PacketAddCode(packet, vif::flushA());
                    uint32_t read = ring.count != 0 ? static_cast<uint32_t>(ee::Address(ring.read)) : 0u;
                    uint32_t left = effect->billboardCount;
                    while (left != 0)
                    {
                        uint32_t run = left < kBatchBillboards + 1 ? left : kBatchBillboards;
                        left -= run;
                        uint32_t vuAddress = 0;
                        while (run != 0)
                        {
                            uint32_t piece = (ee::Address(ring.end) - read) / ring.elementBytes; // up to the ring's end
                            piece = run < piece ? run : piece;
                            piece = piece < kReferenceBillboards + 1 ? piece : kReferenceBillboards;
                            const uint32_t elements = piece * kElementsABillboard;
                            sce::vif1PacketRef(packet, read, piece, vif::stcycl(4, 4), unpackCode(vif::kUnpackV4_16, elements, vuAddress), 0);
                            read += piece * ring.elementBytes;
                            if (!(read < ee::Address(ring.end)))
                                read -= ee::Address(ring.end) - ee::Address(ring.start);
                            run -= piece;
                            vuAddress += elements;
                        }
                        sce::vif1PacketCnt(packet, 0);
                        sce::vif1PacketAddCode(packet, vif::mscnt()); // the program goes on (after a batch)
                        sce::vif1PacketAddCode(packet, vif::flushA());
                    }
                }
                uint32_t next = 0;
                walkNext(queued, next);
                effect = ee::Ptr<BillboardEffect>(next);
            } while (effect);
            sce::vif1PacketAddCode(packet, vif::flushA());
        }
        clearCursorArray(*g_queuedBillboardEffects, 0u, 0);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, drawQueuedBillboards);

    // ---- The effects ----

    // billboardAge: the frames since the billboard was born: the effect's
    // frame less its birth frame, round the 4096.
    inline ee::Float billboardAge(ee::Ptr<BillboardEffect> effect, ee::Ptr<PackedBillboard> billboard)
    {
        ee::Float age = effect->frame - ee::toFloat(static_cast<int32_t>(billboard->bornFrame));
        if (age < ee::Float(0.0f))
            age = age + kFrameWrap;
        return age;
    }
    READABLE_FUNCTION_ONLY(bmx, billboardAge);

    // billboardExpired: whether the effect's life in frames is up for the billboard.
    inline uint32_t billboardExpired(ee::Ptr<BillboardEffect> effect, ee::Ptr<PackedBillboard> billboard)
    {
        return effect->lifeFrames <= billboardAge(effect, billboard) ? 1u : 0u;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, billboardExpired);

    // setBillboardEffectEmitting: whether the effect spawns by its rate (the flag's bit).
    inline void setBillboardEffectEmitting(ee::Ptr<BillboardEffect> effect, uint32_t on)
    {
        effect->flags = (effect->flags & ~static_cast<uint64_t>(kEffectEmitting)) | (on & 1u);
    }
    READABLE_FUNCTION_ONLY(bmx, setBillboardEffectEmitting);

    // setBillboardSpawner: what spawns the effect's billboards, and what it is given.
    inline void setBillboardSpawner(ee::Ptr<BillboardEffect> effect, BillboardSpawner spawner, uint32_t context)
    {
        effect->spawnerContext = context;
        effect->spawner = spawner;
    }
    READABLE_FUNCTION_ONLY(bmx, setBillboardSpawner);

    // dropOldestBillboard: the ring's read on by one (round at the end); one
    // fewer in the ring and in the effect.
    inline void dropOldestBillboard(ee::Ptr<BillboardEffect> effect)
    {
        BillboardRing &ring = effect->billboards;
        ring.read = ringNext(ring, ring.read);
        ring.count = ring.count - 1;
        effect->billboardCount = effect->billboardCount - 1;
    }
    READABLE_FUNCTION_ONLY(bmx, dropOldestBillboard);

    // addBillboard: the billboard into the ring at write (0 when the ring is
    // full), born this frame and moved on by its velocity for a random part
    // of the frame, so a frame's billboards spread along their paths; its
    // position and velocity kept within g_billboardCoordinateLimits and
    // packed in 1024ths. 1.
    inline uint32_t addBillboard(ee::Ptr<BillboardEffect> effect, ee::Ptr<BillboardStart> start)
    {
        BillboardRing &ring = effect->billboards;
        if (ring.count == ring.capacity)
            return 0;
        const ee::Ptr<PackedBillboard> billboard = ring.write;
        ring.write = ringNext(ring, ring.write);
        ring.count = ring.count + 1;
        billboard->bornFrame = static_cast<uint16_t>(ee::toInt(effect->frame));
        const ee::Float part = ee::toFloat(static_cast<int32_t>(randomNumber() & 0x7FFFFFFFu)) * kRandomUnit;
        const Vec4 step = start->velocity * (part * zge::frameSeconds());
        start->position = start->position + step;
        start->position = keptBetweenLanes(start->position, *g_billboardCoordinateLimits);
        start->velocity = keptBetweenLanes(start->velocity, *g_billboardCoordinateLimits);
        billboard->x = static_cast<int16_t>(ee::toInt(start->position.x * kPackedUnit));
        billboard->y = static_cast<int16_t>(ee::toInt(start->position.y * kPackedUnit));
        billboard->z = static_cast<int16_t>(ee::toInt(start->position.z * kPackedUnit));
        billboard->vx = static_cast<int16_t>(ee::toInt(start->velocity.x * kPackedUnit));
        billboard->vy = static_cast<int16_t>(ee::toInt(start->velocity.y * kPackedUnit));
        billboard->velocityW = 0;
        billboard->vz = static_cast<int16_t>(ee::toInt(start->velocity.z * kPackedUnit));
        effect->billboardCount = effect->billboardCount + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, addBillboard);

    // randomCubeVector: x, y and z each from ten bits of one random number,
    // in [-1, 1).
    inline void randomCubeVector(ee::Ptr<Vec3> out)
    {
        const uint32_t random = randomNumber();
        out->x = ee::toFloat(randomLane(random, 0)) * kRandomLaneUnit;
        out->y = ee::toFloat(randomLane(random, kRandomLaneBits)) * kRandomLaneUnit;
        out->z = ee::toFloat(randomLane(random, 2 * kRandomLaneBits)) * kRandomLaneUnit;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, randomCubeVector);

    // spawnDefaultBillboard (the default look's spawner): a billboard at a
    // random point of the default cube about the effect's origin, with a
    // random velocity straight down at 7 a second. addBillboard's answer.
    inline int32_t spawnDefaultBillboard(ee::Ptr<BillboardEffect> effect, uint32_t /*context*/)
    {
        ee::Local<BillboardStart> start;
        randomCubeVector(&start->position.xyz());
        randomCubeVector(&start->velocity.xyz());
        start->position = start->position * *g_defaultBillboardSpawnSize;
        start->velocity = start->velocity * ee::Float(1.0f);
        start->velocity.y = kDefaultBillboardFall;
        return static_cast<int32_t>(addBillboard(effect, start));
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, spawnDefaultBillboard);

    // setBillboardEffectMatrix: the matrix copied in; when its position moved
    // past g_billboardMoveEpsilon on any axis, the view sphere's centre moves
    // to it and the region at the given matrix's position is found again.
    inline void setBillboardEffectMatrix(ee::Ptr<BillboardEffect> effect, ee::Ptr<Matrix> matrix)
    {
        const Vec4 was = effect->matrix.translation();
        effect->matrix = *matrix;
        const ee::Float epsilon = *g_billboardMoveEpsilon;
        const Vec4 &now = effect->matrix.translation();
        if (ee::abs(was.x - now.x) < epsilon && ee::abs(was.y - now.y) < epsilon && ee::abs(was.z - now.z) < epsilon)
            return;
        effect->viewSphere.centre = matrix->translation();
        effect->viewSphere.centre.w = effect->viewSphere.radius;
        effect->region = findRegionAt(&matrix->translation().xyz()); // (the given matrix's position: the same point)
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setBillboardEffectMatrix);

    // setBillboardEffectLook: the look (g_defaultBillboardLook for none)
    // applied: the material and its states (a sprite, no depth write, both
    // texture axes repeating), the colours, the acceleration, the sizes, the
    // life, the fades and the rate in frames, the effect placed at the
    // identity, its fade whole; then the ring made for the look's capacity,
    // when there is none yet (an effect with a ring keeps it: a larger
    // capacity is refused and the old one kept, a smaller noted alone).
    inline void setBillboardEffectLook(ee::Ptr<BillboardEffect> effect, ee::Ptr<BillboardLook> look)
    {
        if (!look)
            look = ee::Ptr<BillboardLook>(&*g_defaultBillboardLook);
        effect->material = look->material;
        const uint32_t hadCapacity = effect->capacity;
        effect->startColour = colourLanes(look->startColour);
        effect->endColour = colourLanes(look->endColour);
        effect->startSize = look->startSize;
        effect->endSize = look->endSize;
        effect->lifeFrames = frames(look->life);
        effect->spawnRate = look->spawnRate;
        effect->acceleration = look->acceleration;
        effect->capacity = look->capacity;
        effect->viewDistance = look->viewDistance;
        effect->viewSphere.centre.w = look->viewRadius;
        effect->viewSphere.radius = look->viewRadius;
        setBillboardEffectMatrix(effect, &*g_identityMatrix);
        effect->viewFade = ee::Float(1.0f);
        effect->lifeInverse = ee::Float(1.0f) / effect->lifeFrames;
        if (effect->material)
        {
            setMaterialState(effect->material, 0, kStatePrimType, kSpriteMaterial);
            zgui::rebuildMaterialRegisters(effect->material, 0, kStateDepthWrite, 0);
            zgui::rebuildMaterialRegisters(effect->material, 0, kStateRepeatU, 1);
            zgui::rebuildMaterialRegisters(effect->material, 0, kStateRepeatV, 1);
        }
        effect->fadeInFrames = frames(look->fadeInSeconds);
        effect->fadeOutFrame = effect->lifeFrames - frames(look->fadeOutSeconds);
        setBillboardSpawner(effect, look->spawner, look->spawnerContext);
        const uint32_t bytes = effect->capacity * kBillboardBytes;
        if (bytes == 0)
            return;
        if (effect->buffer != 0)
        {
            if (hadCapacity < effect->capacity)
                effect->capacity = hadCapacity;
            return;
        }
        const uint32_t buffer = allocateIn(bytes, kRingMemoryKind, 1);
        BillboardRing &ring = effect->billboards;
        ring.start = ee::Ptr<PackedBillboard>(buffer);
        effect->buffer = buffer;
        ring.end = ee::Ptr<PackedBillboard>(buffer + effect->capacity * kBillboardBytes);
        ring.elementBytes = kBillboardBytes;
        ring.capacity = effect->capacity;
        ring.count = 0;
        ring.write = ee::Ptr<PackedBillboard>(buffer);
        ring.read = ee::Ptr<PackedBillboard>(buffer);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setBillboardEffectLook);

    // takeBillboardEffect: an effect from the pool, cleared, at the identity,
    // taken and emitting, with no billboards, at frame 0 and owing none, its
    // 1 set; then the look applied (the default's for none). The effect.
    inline ee::Ptr<BillboardEffect> takeBillboardEffect(ee::Ptr<BillboardLook> look)
    {
        const ee::Ptr<BillboardEffect> effect(poolAlloc(*g_billboardEffectPool));
        memoryFill(effect, 0, sizeof(BillboardEffect));
        matrixIdentity(ee::field(effect, &BillboardEffect::matrix));
        effect->billboardCount = 0;
        effect->spawnOwed = ee::Float(0.0f);
        effect->frame = ee::Float(0.0f);
        effect->constantOne = ee::Float(1.0f);
        effect->flags = effect->flags | kEffectEmitting | kEffectTaken;
        setBillboardEffectLook(effect, look);
        return effect;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, takeBillboardEffect);

    // freeBillboardEffect: the ring's memory freed and the effect back to the pool.
    inline void freeBillboardEffect(ee::Ptr<BillboardEffect> effect)
    {
        freeIn(effect->buffer, kRingMemoryKind);
        game::tail(poolFree, *g_billboardEffectPool, effect);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeBillboardEffect);

    // updateBillboardEffect: the frame on by one (round past 4096), the
    // expired billboards dropped from the oldest on, then, while emitting,
    // the rate's billboards for a frame owed and each whole one spawned
    // through the spawner until it answers 0. The billboard count.
    inline uint32_t updateBillboardEffect(ee::Ptr<BillboardEffect> effect)
    {
        effect->frame = effect->frame + ee::Float(1.0f);
        if (kFrameWrap < effect->frame)
            effect->frame = effect->frame - kFrameWrap;
        BillboardRing &ring = effect->billboards;
        while (ring.count != 0 && ring.read && billboardExpired(effect, ring.read))
            dropOldestBillboard(effect);
        if (effect->flags & kEffectEmitting)
        {
            effect->spawnOwed = effect->spawnOwed + effect->spawnRate / ee::toFloat(static_cast<int32_t>(vsyncsASecond(*g_display)));
            const int32_t owed = ee::toInt(effect->spawnOwed);
            effect->spawnOwed = effect->spawnOwed - ee::toFloat(owed);
            for (int32_t left = owed; left != 0; --left)
                if (effect->spawner(effect, effect->spawnerContext) == 0)
                    break;
        }
        return static_cast<uint32_t>(effect->billboardCount);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, updateBillboardEffect);

    // queueBillboardEffect: while the queue is open, the effect queued for
    // the view's packet when the camera's region does not hide its region
    // (a camera in no area, or an effect in no region, is never hidden), its
    // sphere is in view, its view depth is not past its view distance and it
    // has a material; its fade from how far short of the distance it is. 0.
    inline uint32_t queueBillboardEffect(ee::Ptr<BillboardEffect> effect)
    {
        if (*g_billboardQueueOpen == 0)
            return 0;
        const ee::Ptr<Camera> camera(*g_currentCamera);
        bool shown = true;
        if (cameraArea(camera) != 0 && effect->region)
            shown = effect->region->outOfViewMark != *g_outOfViewMark;
        if (!shown)
            return 0;
        if (partInView(camera, ee::field(effect, &BillboardEffect::viewSphere)) & kSphereOutOfView)
            return 0;
        const Vec4 inView = transformed(*cachedMatrix(camera, kCameraFromWorldMatrix), effect->viewSphere.centre);
        const ee::Float past = inView.z - effect->viewSphere.radius - effect->viewDistance;
        if (ee::Float(0.0f) < past)
            return 0;
        if (!effect->material)
            return 0;
        effect->viewFade = kViewFadeUnits < past ? past * kViewFadeRate : ee::Float(1.0f);
        pointerArrayPushBack(*g_queuedBillboardEffects, effect);
        return 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, queueBillboardEffect);
}
