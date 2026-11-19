// The engine's three particle types: spark, billboard and poly
// (particle_types.h). Each is a table of methods registered with
// registerParticleType, with two behaviours of its own, an acceleration and
// a deflector, bound to it. An emitter object (particle_emitter_objects.cpp)
// makes one effect a type; the type spawns its particles into the effect's
// list from a pool of its own, moves and ages them each frame, then lets the
// emitter's bound behaviours act on them, and draws them.
//
// Sparks are drawn as a line from where each was a frame ago to where it is.
// The empty methods and the effects' frees are in render/particle_instances.cpp.

#include "particle_types.h"
#include "scene.h"

#include "zge/container/array.h"
#include "zge/container/list.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "sce/libgcc.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/render/material.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/render/billboards.h"
#include "zge/render/primitives.h"
#include "zge/zgui/widget_parts.h"
#include "zge/math/plane.h"
#include "zge/math/random.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/particle_kinds.cpp.h"

namespace zge
{
    namespace
    {
        // A billboard (0x60 bytes from the billboard pool): a square that
        // faces the camera.
        struct Billboard
        {
            ListNode node;         // +0x00 on its effect's list
            uint32_t _08, _0c;
            Vec4 position;         // +0x10
            Vec4 previous;         // +0x20 where it was a frame ago
            Vec4 velocity;         // +0x30 a second
            // +0x40 its alpha fades in over its first tenth of a second and
            // out over its last
            ParticleColour colour;
            ee::Float size;        // +0x44 its full size
            // +0x48 its size this frame: the square's corners lie this far
            // from its centre across and up
            ee::Float sizeNow;
            uint32_t material;     // +0x4C
            ee::Float life;        // +0x50 the seconds it lives
            ee::Float age;         // +0x54
            uint8_t _58[0x08];
        };
        static_assert(offsetof(Billboard, colour) == 0x40 && offsetof(Billboard, sizeNow) == 0x48);
        static_assert(offsetof(Billboard, life) == 0x50 && offsetof(Billboard, age) == 0x54 && sizeof(Billboard) == 0x60);

        // A poly (0x70 bytes from the poly pool): a square that turns about
        // an axis of its own.
        struct Poly
        {
            ListNode node;           // +0x00 on its effect's list
            uint32_t _08, _0c;
            Vec4 position;           // +0x10
            Vec4 velocity;           // +0x20 a second
            Quaternion orientation;  // +0x30 how it is turned before it spins (none: polys start it unturned)
            Vec4 spinAxis;           // +0x40
            Angle angle;             // +0x50 about its axis
            uint16_t _52;
            ee::Float spinRate;      // +0x54 angle a second (65536 a turn)
            ParticleColour colour;   // +0x58 its alpha fades in and out as a billboard's
            ee::Float size;          // +0x5C its full size
            ee::Float sizeNow;       // +0x60 its size this frame
            uint32_t material;       // +0x64
            ee::Float life;          // +0x68 the seconds it lives
            ee::Float age;           // +0x6C
        };
        static_assert(offsetof(Poly, orientation) == 0x30 && offsetof(Poly, spinAxis) == 0x40 && offsetof(Poly, spinRate) == 0x54);
        static_assert(offsetof(Poly, colour) == 0x58 && offsetof(Poly, material) == 0x64 && sizeof(Poly) == 0x70);

        // A vertex, a line and a quad as the primitive API takes them
        // (drawLines, drawPrimitiveQuads; the layout of
        // render/ps2/primitive_packets.cpp's PrimitiveVertex and PrimitiveLine).
        struct alignas(16) Vertex
        {
            Vec4 position;         // +0x00 (w not read)
            ee::Float u, v;        // +0x10 texture coordinates
            ParticleColour colour; // +0x18
            uint32_t _1c;
        };
        static_assert(offsetof(Vertex, u) == 0x10 && offsetof(Vertex, colour) == 0x18 && sizeof(Vertex) == 0x20);

        struct Line
        {
            uint32_t texture;       // +0x00
            ee::Ptr<Vertex> first;  // +0x04
            ee::Ptr<Vertex> second; // +0x08
        };
        static_assert(sizeof(Line) == 0x0C);

        struct Quad
        {
            uint32_t texture;           // +0x00
            ee::Ptr<Vertex> corners[4]; // +0x04
        };
        static_assert(sizeof(Quad) == 0x14);
    }

    namespace
    {
        inline ee::Global<ee::Ptr<Pool>, sym::g_sparkPool> g_sparkPool;
        // The acceleration's change of velocity this frame (a global in the
        // original, as it works it out).
        inline ee::Global<Vec4, sym::g_sparkGravityStep> g_sparkGravityStep;
        inline ee::Global<ee::Ptr<Pool>, sym::g_billboardPool> g_billboardPool;
        inline ee::Global<Vec4, sym::g_billboardGravityStep> g_billboardGravityStep;
        inline ee::Global<ee::Ptr<Pool>, sym::g_polyParticlePool> g_polyPool;
        inline ee::Global<Vec4, sym::g_polyGravityStep> g_polyGravityStep;

        // Records a block of the kinds' pools (createPool's recordsABlock). ZGE4 (BMX XXX) asks for none, so its
        // pools take createPool's least block; its billboards have no pool at all.
        constexpr uint32_t kSparkPoolBlock = game::readable::kIsBmx ? 0 : 0x400;
        constexpr uint32_t kBillboardPoolBlock = 0x400;
        constexpr uint32_t kPolyPoolBlock = game::readable::kIsBmx ? 0 : 0x400;
        // A poly record's bytes in the pool: ZGE4's (BMX XXX) is 0x80, its material, life and age a word on from
        // ZGE3's Poly (+0x68, +0x6C, +0x70 in BMX XXX's spawnPolys and updatePolys).
        constexpr uint32_t kPolyRecordBytes = game::readable::kIsBmx ? 0x80 : sizeof(Poly);
        constexpr ee::Float kTurn = 65536.0f;          // angle units a turn
        constexpr ee::Float kRandomScale = 2.3283064e-10f; // 2 to the -32
        constexpr uint32_t kRandomAngles = 0xFFFF;     // a random angle is a random number modulo this
        // A billboard or poly grows from this part of its size to all of it,
        // and shrinks back to it at the end (by kSizeGrowth over the time).
        constexpr ee::Float kSizeStart = 0.1f, kSizeGrowth = 0.9f;
        // Its alpha fades in and out over this many seconds, at this many
        // steps of alpha a second (255 over the fade).
        constexpr ee::Float kFadeSeconds = 0.1f, kFadeRate = 2550.0f;
        constexpr uint32_t kUpdateWord = 1;           // the only word the updates are given
        constexpr uint32_t kCameraMatrix = 1;         // cachedMatrix's camera matrix
        constexpr uint8_t kOpaque = 255;
        constexpr ee::Float kGravity = 9.81f;         // a second squared: the acceleration's unit
        // ZGE4's billboard update multiplies a bound acceleration (in g) by this for the queue's effect: 98.
        constexpr ee::Float kBillboardAccelerationScale = 98.0f;
        constexpr uint64_t kLookChanged = 1;           // ParticleLook::changed's bit (ZGE4)
        // ZGE4's: the particles' off switch (setParticlesOn), the origin its spawn points start from, the identity.
        inline ee::Global<uint32_t, sym::bmx::g_particlesOff> g_particlesOff;
        inline ee::Global<Vec4, sym::bmx::origin> g_origin;
        inline ee::Global<Matrix, sym::identityMatrix> g_identityMatrix;
        inline ee::Global<ParticleBehaviour, sym::g_billboardAcceleration> g_billboardAcceleration;
        constexpr ee::Float kBoundsStart = 99999.0f;  // the bounds' start, past any particle

        // The particle types' registry (particle_types.cpp).
        ee::Ptr<Spark> newSpark() { return poolAllocAs<Spark>(*g_sparkPool); }
        ee::Ptr<Billboard> newBillboard() { return poolAllocAs<Billboard>(*g_billboardPool); }
        ee::Ptr<Poly> newPoly() { return poolAllocAs<Poly>(*g_polyPool); }

        // The factor a poly's spreads are scaled by. Meant to run from -1 to 1,
        // it runs from -2 to 0: the random word is read as a signed number,
        // so its quarter of a turn is -0.5 to 0.5 before it is doubled and
        // 1 taken off.
        ee::Float spreadFactor()
        {
            const ee::Float unit = ee::toFloat(static_cast<int32_t>(randomNumber())) * kRandomScale;
            return unit + unit - 1.0f;
        }
        void reflectVector(ee::Ptr<Vec4> out, ee::Ptr<Vec4> vector, ee::Ptr<Vec4> normal) { ee::game(sym::reflectVector, out, vector, normal); }

        namespace tail
        {
            int32_t freeBlock(ee::Address block) { return ee::tailCall<int32_t>(sym::heapFree, block); }
        }

        // The upper half of a vector's quadword (z and w), as the engine's
        // vector macro carries it into VU0 with the next factor.
        uint64_t upperHalf(const Vec4 &v) { return v.z.bits() | static_cast<uint64_t>(v.w.bits()) << 32; }

        // The effect's bounds made to take in a particle at p: the high side
        // first, then the low, as the types do it.
        void takeIntoBounds(ee::Ptr<ParticleEffect> effect, const Vec4 &p)
        {
            if (effect->boundsHigh.x < p.x)
                effect->boundsHigh.x = p.x;
            if (effect->boundsHigh.y < p.y)
                effect->boundsHigh.y = p.y;
            if (effect->boundsHigh.z < p.z)
                effect->boundsHigh.z = p.z;
            if (p.x < effect->boundsLow.x)
                effect->boundsLow.x = p.x;
            if (p.y < effect->boundsLow.y)
                effect->boundsLow.y = p.y;
            if (p.z < effect->boundsLow.z)
                effect->boundsLow.z = p.z;
        }

        // The bounds started again, before the first particle is taken in.
        void startBounds(ee::Ptr<ParticleEffect> effect)
        {
            effect->boundsLow.x = kBoundsStart;
            effect->boundsHigh.x = -kBoundsStart;
            effect->boundsLow.z = kBoundsStart;
            effect->boundsLow.y = kBoundsStart;
            effect->boundsHigh.z = -kBoundsStart;
            effect->boundsHigh.y = -kBoundsStart;
        }

        // A square's corners in drawing order: their texture coordinates and
        // which way from the centre they lie across and up.
        struct Corner
        {
            ee::Float u, v;
            bool left, down;
        };
        constexpr Corner kCorners[4] = {
            {1.0f, 0.0f, false, true}, {0.0f, 0.0f, true, true}, {0.0f, 1.0f, true, false}, {1.0f, 1.0f, false, false}};

        // A billboard's or poly's four corners, sizeNow from its centre across
        // and up, through turn (whose translation is the centre), in its
        // colour. offset is the frame's place for each corner's offset (its w
        // never written).
        void placeCorners(Vertex *corners, const Matrix &turn, const ParticleColour &colour, ee::Float sizeNow, Vec4 &offset)
        {
            for (uint32_t corner = 0; corner < 4; ++corner)
            {
                Vertex &vertex = corners[corner];
                vertex.colour = colour;
                vertex.u = kCorners[corner].u;
                vertex.v = kCorners[corner].v;
                offset.x = kCorners[corner].left ? -sizeNow : sizeNow;
                offset.y = kCorners[corner].down ? -sizeNow : sizeNow;
                offset.z = 0.0f;
                vertex.position = transformed(turn, offset);
            }
        }

        // Each behaviour bound to the emitter (its emitter definitions) run on
        // the effect.
        void applyBehaviours(ee::Ptr<ParticleEffect> effect, ee::Ptr<ParticleEmitterData> data)
        {
            uint32_t item = walkFirst(*data->bindings);
            while (item)
            {
                const ee::Ptr<EmitterBinding> binding(item);
                const ee::Ptr<ParticleBehaviour> behaviour = binding->behaviour;
                behaviour->apply(behaviour, effect, binding->definition);
                walkNext(*data->bindings, item);
            }
        }
    }

    // ---- Sparks ----

    // newSparkEffect (the spark type's start): an effect with no sparks yet.
    ee::Ptr<ParticleEffect> newSparkEffect(ee::Ptr<ParticleType> type)
    {
        const ee::Ptr<ParticleEffect> effect(heapAllocate(sizeof(ParticleEffect)));
        initList(&effect->particles);
        effect->type = type;
        return effect;
    }
    READABLE_FUNCTION_WAITS(newSparkEffect, Draft);

    // readSparkSettings (the spark type's readSettings): the settings, with a
    // look taken from the ZPED record (its colour, life and size), or the
    // type's defaults when there is no record.
    ee::Ptr<ParticleSettings> readSparkSettings(ee::Ptr<ParticleType> type, ee::Ptr<ZpedRecord> record)
    {
        const ee::Ptr<ParticleSettings> settings(heapAllocate(sizeof(ParticleSettings)));
        const ee::Ptr<ParticleLook> look(heapAllocate(sizeof(ParticleLook)));
        settings->type = type;
        settings->look = look;
        if (!record)
        {
            memoryCopy(look, sym::g_sparkDefaults, sizeof(ParticleLook));
            return settings;
        }
        look->bytes = sizeof(ParticleLook);
        look->typeHash = record->typeHash;
        memoryFill(&look->cleared, 0, sizeof(Vec4));
        memoryFill(&look->upVelocity, 0, sizeof(Vec4));
        look->size = record->params.size;
        look->life = record->params.life;
        look->colour = record->colour;
        look->material = 0;
        return settings;
    }
    READABLE_FUNCTION_WAITS(readSparkSettings, Draft);

    // freeSparkSettings (the spark type's releaseSettings): the look's
    // material released, then the look and the settings freed.
    int32_t freeSparkSettings(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleSettings> settings)
    {
        const ee::Ptr<ParticleLook> look = settings->look;
        if (look->material)
        {
            releaseMaterial(look->material);
            look->material = 0;
        }
        heapFree(settings->look);
        settings->look = ee::Ptr<ParticleLook>();
        return tail::freeBlock(settings);
    }
    READABLE_FUNCTION_WAITS(freeSparkSettings, Draft);

    // setSparkMaterial (the spark type's setMaterialTextures): the material
    // its sparks are drawn with.
    void setSparkMaterial(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleSettings> settings, uint32_t material)
    {
        settings->look->material = material;
    }
    READABLE_FUNCTION(setSparkMaterial, Draft);

    // updateSparks (the spark type's update): each spark moved by its
    // velocity over the frame and aged, the effect's bounds made to take in
    // where they are, and a spark past its life freed; then the emitter's
    // behaviours run on the effect. Returns how many sparks there were.
    int32_t updateSparks(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleEffect> effect, uint32_t word, ee::Ptr<ParticleEmitterData> data)
    {
        ee::Local<Vec4> step;
        if (word != kUpdateWord)
            return 0;
        int32_t count = 0;
        const ee::Float seconds = zge::frameSeconds();
        if (effect && effect->particles.head)
        {
            startBounds(effect);
            ee::Ptr<Spark> spark = recordOfNode<Spark>(effect->particles.head);
            while (true)
            {
                ++count;
                spark->previous = spark->position;
                *step = scaledCarrying(spark->velocity, seconds, upperHalf(spark->position));
                spark->position = spark->position + *step;
                takeIntoBounds(effect, spark->position);
                spark->age = spark->age + seconds;
                const ee::Ptr<Spark> next = recordOfNode<Spark>(spark->node.next);
                if (spark->life <= spark->age)
                {
                    listRemove(&effect->particles, &spark->node);
                    poolFree(*g_sparkPool, spark);
                }
                if (!next)
                    break;
                spark = next;
            }
        }
        applyBehaviours(effect, data);
        return count;
    }
    READABLE_FUNCTION(updateSparks, Draft);

    // emitSparks (the spark type's spawn): count sparks (its low byte) at the
    // emitter, each with its start and velocity from the emitter's settings
    // and the look's colour, life and size (the type's defaults for an object
    // with no data). Nothing without a type or an effect. 0.
    uint32_t emitSparks(ee::Ptr<ParticleType> type, ee::Ptr<SceneObject> object, ee::Ptr<ParticleEffect> effect, uint32_t count,
                        ee::Ptr<ParticleEmitterData> data)
    {
        ee::Local<Vec4> origin;
        ee::Local<Matrix> turn;
        const uint32_t sparks = count & 0xFFu;
        const ee::Ptr<ParticleLook> look = data ? data->settings->look : ee::Ptr<ParticleLook>(sym::g_sparkDefaults);
        if (!type || !effect)
            return 0;
        emitterPosition(object, origin);
        emitterMatrix(object, turn);
        for (uint32_t made = 0; made < sparks;)
        {
            const ee::Ptr<Spark> spark = newSpark();
            spark->age = 0.0f;
            spark->colour = look->colour;
            particleSpawnPoint(&spark->position, origin, object);
            spark->previous = spark->position;
            ++made;
            particleSpawnVelocity(&spark->velocity, turn, object);
            spark->size = look->size;
            spark->life = look->life;
            spark->material = look->material;
            listPush(&effect->particles, &spark->node);
        }
        return 0;
    }
    READABLE_EXCEPT(bmx, READABLE_FUNCTION_WAITS(emitSparks, Draft)); // (BMX XXX's: game/bmx/fx/particle_kinds.cpp)

    // drawSparkLines (the spark type's draw): each spark a line, full alpha,
    // from where it was a frame ago to where it is, with its material.
    // Returns how many. Before the lines the camera's matrix is copied and
    // its turn transposed, and each spark's position goes into the copy's
    // last row, though the lines use neither.
    int32_t drawSparkLines(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleEffect> effect)
    {
        ee::Local<Line> line;
        ee::Local<Vertex> from;
        ee::Local<Vertex> to;
        ee::Local<Matrix> camera;
        ee::Local<Matrix> cameraTurn;
        ee::Local<ParticleColour> colour;
        line->first = from;
        line->second = to;
        int32_t count = 0;
        *camera = *cachedMatrix(ee::Ptr<MatrixCache>(*g_currentCamera), kCameraMatrix);
        matrixTranspose3(cameraTurn, camera);
        ee::Ptr<Spark> spark = recordOfNode<Spark>(effect->particles.head);
        if (!spark)
            return count;
        Vec4 &lastRow = camera->translation();
        lastRow.z = 0.0f;
        while (true)
        {
            lastRow.y = 0.0f;
            lastRow.x = 0.0f;
            *colour = spark->colour;
            ++count;
            colour->alpha = kOpaque;
            lastRow.x = spark->position.x;
            lastRow.y = spark->position.y;
            from->u = 0.0f;
            lastRow.z = spark->position.z;
            from->v = 0.0f;
            to->colour = *colour;
            to->position.x = spark->position.x;
            to->position.y = spark->position.y;
            to->u = 0.0f;
            to->position.z = spark->position.z;
            to->v = 0.0f;
            colour->alpha = kOpaque;
            from->colour = *colour;
            from->position.x = spark->previous.x;
            from->position.y = spark->previous.y;
            from->position.z = spark->previous.z;
            line->texture = spark->material;
            drawLines(0, ee::Address(line), 1, 0);
            spark = recordOfNode<Spark>(spark->node.next);
            if (!spark)
                return count;
            lastRow.z = 0.0f;
        }
    }
    READABLE_FUNCTION_WAITS(drawSparkLines, Draft);

    // accelerateSparks (the sparks' acceleration behaviour): the definition's
    // acceleration (in g) over this frame added to each spark's velocity.
    void accelerateSparks(ee::Ptr<ParticleBehaviour> /*behaviour*/, ee::Ptr<ParticleEffect> effect, ee::Ptr<EmitterDefinition> definition)
    {
        const ee::Float seconds = zge::frameSeconds();
        *g_sparkGravityStep = definition->acceleration;
        *g_sparkGravityStep = scaledCarrying(*g_sparkGravityStep, seconds * kGravity, upperHalf(definition->acceleration));
        if (!effect)
            return;
        for (ee::Ptr<Spark> spark = recordOfNode<Spark>(effect->particles.head); spark; spark = recordOfNode<Spark>(spark->node.next))
            spark->velocity = spark->velocity + *g_sparkGravityStep;
    }
    READABLE_FUNCTION(accelerateSparks, Draft);

    // bounceSparks (the sparks' deflector behaviour): a spark in front of the
    // definition's plane and less than a unit from it has its velocity
    // reflected off the plane.
    void bounceSparks(ee::Ptr<ParticleBehaviour> /*behaviour*/, ee::Ptr<ParticleEffect> effect, ee::Ptr<EmitterDefinition> definition)
    {
        if (!effect)
            return;
        for (ee::Ptr<Spark> spark = recordOfNode<Spark>(effect->particles.head); spark; spark = recordOfNode<Spark>(spark->node.next))
        {
            const ee::Float distance = planeDistance(&definition->plane, &spark->position);
            if (!(distance < 0.0f) && distance < 1.0f)
                reflectVector(&spark->velocity, &spark->velocity, &definition->plane);
        }
    }
    READABLE_FUNCTION(bounceSparks, Draft);

    // registerSparkParticles: the spark pool made, the "spark" type
    // registered under its name's hash, its acceleration and deflector
    // registered and both bound to it (the last binding's result).
    uint32_t registerSparkParticles()
    {
        const ee::Ptr<ParticleType> type(sym::g_sparkType);
        const ee::Ptr<ParticleBehaviour> acceleration(sym::g_sparkAcceleration);
        const ee::Ptr<ParticleBehaviour> deflector(sym::g_sparkDeflector);
        *g_sparkPool = ee::Ptr<Pool>(createPool(sizeof(Spark), kSparkPoolBlock, 0));
        type->nameHash = nameHash(ee::Ptr<const char>(sym::g_textSpark));
        registerParticleType(type);
        acceleration->kindHash = behaviourKindHash(kAccelerationBehaviour);
        deflector->kindHash = behaviourKindHash(kDeflectorBehaviour);
        registerParticleBehaviour(acceleration);
        registerParticleBehaviour(deflector);
        bindParticleBehaviour(type->nameHash, acceleration->kindHash, 0);
        return game::tail(bindParticleBehaviour, type->nameHash, deflector->kindHash, 0);
    }
    READABLE_FUNCTION_WAITS(registerSparkParticles, Draft);

    // ---- Billboards ----

    // newBillboardEffect (the billboard type's start): an effect with no
    // billboards yet (ZGE4's: a record of the billboard queue's, with no look).
    ee::Ptr<ParticleEffect> newBillboardEffect(ee::Ptr<ParticleType> type)
    {
        const ee::Ptr<ParticleEffect> effect(heapAllocate(sizeof(ParticleEffect)));
        if constexpr (game::readable::kIsBmx)
            effect->billboardEffect = takeBillboardEffect(ee::Ptr<BillboardLook>());
        else
            initList(&effect->particles);
        effect->type = type;
        return effect;
    }
    READABLE_FUNCTION_WAITS(newBillboardEffect, Draft);

    // readBillboardSettings (the billboard type's readSettings): the settings,
    // with a look taken from the ZPED record: its colour, life and size, and
    // its speed straight up turned by its orientation. With no record the
    // look is left as the heap gave it (the type's defaults are not copied,
    // as the spark type does).
    ee::Ptr<ParticleSettings> readBillboardSettings(ee::Ptr<ParticleType> type, ee::Ptr<ZpedRecord> record)
    {
        ee::Local<Quaternion> orientation;
        ee::Local<Matrix> turn;
        const ee::Ptr<ParticleSettings> settings(heapAllocate(sizeof(ParticleSettings)));
        const ee::Ptr<ParticleLook> look(heapAllocate(sizeof(ParticleLook)));
        settings->type = type;
        settings->look = look;
        if (!record)
            return settings;
        look->bytes = sizeof(ParticleLook);
        look->typeHash = record->typeHash;
        memoryFill(&look->cleared, 0, sizeof(Vec4));
        look->upVelocity = Vec4{0.0f, -record->params.speed, 0.0f, 1.0f};
        *orientation = Quaternion{record->orientation[0], record->orientation[1], record->orientation[2], record->orientation[3]};
        matrixIdentity(turn);
        quaternionToMatrix(turn, orientation);
        look->upVelocity = transformed(*turn, look->upVelocity);
        look->size = record->params.size;
        look->life = record->params.life;
        look->colour = record->colour;
        look->material = 0;
        if constexpr (game::readable::kIsBmx)
            lookChanged(*look) |= kLookChanged; // (ZGE4: the type's update hands the new look to the effect)
        return settings;
    }
    READABLE_FUNCTION_WAITS(readBillboardSettings, Draft);

    // freeBillboardSettings (the billboard type's releaseSettings): the look's
    // material released, then the look and the settings freed.
    int32_t freeBillboardSettings(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleSettings> settings)
    {
        const ee::Ptr<ParticleLook> look = settings->look;
        if (look || !game::readable::kIsBmx) // (ZGE4 frees nothing of a missing look)
        {
            if (look->material)
            {
                releaseMaterial(look->material);
                look->material = 0;
            }
            heapFree(settings->look);
            settings->look = ee::Ptr<ParticleLook>();
        }
        return tail::freeBlock(settings);
    }
    READABLE_FUNCTION_WAITS(freeBillboardSettings, Draft);

    // setBillboardMaterial (the billboard type's setMaterialTextures): the
    // material its billboards are drawn with.
    void setBillboardMaterial(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleSettings> settings, uint32_t material)
    {
        settings->look->material = material;
    }
    READABLE_FUNCTION_OWN(setBillboardMaterial, Draft, bmx, setQueuedBillboardMaterial);

    // ---- ZGE4's billboard type (BMX XXX): its billboards are the billboard
    // queue's (render/billboards.cpp), one effect record an emitter ----

    // setQueuedBillboardMaterial (ZGE4's setBillboardMaterial): the look's
    // material, set up as the queue draws it (no depth write, both texture
    // axes repeating), and the look marked changed.
    void setQueuedBillboardMaterial(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleSettings> settings, uint32_t material)
    {
        const ee::Ptr<ParticleLook> look = settings->look;
        look->material = material;
        zgui::rebuildMaterialRegisters(ee::Ptr<Material>(material), 0, kStateDepthWrite, 0);
        zgui::rebuildMaterialRegisters(ee::Ptr<Material>(material), 0, kStateRepeatU, 1);
        zgui::rebuildMaterialRegisters(ee::Ptr<Material>(material), 0, kStateRepeatV, 1);
        lookChanged(*look) |= kLookChanged;
    }

    // copyBillboardSettings (ZGE4's copySettings for the billboard type): a
    // copy of the settings and of their look, the look's material copied too
    // (copyMaterial) and the copy marked changed. The new settings.
    inline ee::Ptr<ParticleSettings> copyBillboardSettings(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleSettings> settings)
    {
        const ee::Ptr<ParticleSettings> copy(heapAllocate(sizeof(ParticleSettings)));
        const ee::Ptr<ParticleLook> look(heapAllocate(sizeof(ParticleLook)));
        const ee::Ptr<ParticleLook> from = settings->look;
        *copy = *settings;
        *look = *from;
        copy->look = look;
        if (from->material)
            look->material = ee::Address(copyMaterial(ee::Ptr<Material>(from->material)));
        lookChanged(*look) |= kLookChanged;
        return copy;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, copyBillboardSettings);

    // spawnEmitterBillboard (the look's spawner for an emitter's billboards;
    // the queue calls it once a billboard owed): nothing while the particles
    // are off; else a billboard started at the emitter's spawn point with its
    // spawn velocity (unturned: the effect's matrix turns it). addBillboard's
    // answer.
    inline int32_t spawnEmitterBillboard(ee::Ptr<BillboardEffect> effect, ee::Ptr<SceneObject> object)
    {
        ee::Local<BillboardStart> start;
        if (*g_particlesOff != 0)
            return 0;
        particleSpawnPoint(&start->position, &*g_origin, object);
        particleSpawnVelocity(&start->velocity, &*g_identityMatrix, object);
        return static_cast<int32_t>(addBillboard(effect, start));
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, spawnEmitterBillboard);

    // updateBillboards (the billboard type's update): each billboard aged, and
    // freed past its life; the others moved by their velocity over the frame,
    // the effect's bounds made to take them in, their size grown over the
    // emitter's growTime from the start and shrunk over its shrinkTime to the
    // end, their alpha faded in and out. Then the emitter's behaviours run on
    // the effect. Returns how many billboards there were.
    int32_t updateBillboards(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleEffect> effect, uint32_t word, ee::Ptr<ParticleEmitterData> data)
    {
        ee::Local<Vec4> step;
        if (word != kUpdateWord)
            return 0;
        int32_t count = 0;
        const ee::Float seconds = zge::frameSeconds();
        if (effect && effect->particles.head)
        {
            startBounds(effect);
            ee::Ptr<Billboard> billboard = recordOfNode<Billboard>(effect->particles.head);
            while (true)
            {
                ++count;
                billboard->age = billboard->age + seconds;
                const ee::Ptr<Billboard> next = recordOfNode<Billboard>(billboard->node.next);
                if (billboard->life <= billboard->age)
                {
                    listRemove(&effect->particles, &billboard->node);
                    poolFree(*g_billboardPool, billboard);
                }
                else
                {
                    billboard->previous = billboard->position;
                    *step = scaledCarrying(billboard->velocity, seconds, upperHalf(billboard->position));
                    billboard->position = billboard->position + *step;
                    takeIntoBounds(effect, billboard->position);
                    const ee::Float age = billboard->age;
                    if (age < data->params.growTime)
                        billboard->sizeNow = billboard->size * (age * kSizeGrowth / data->params.growTime + kSizeStart);
                    else if (billboard->life - data->params.shrinkTime < age)
                        billboard->sizeNow = billboard->size * ((billboard->life - age) * kSizeGrowth / data->params.shrinkTime + kSizeStart);
                    else
                        billboard->sizeNow = billboard->size;
                    if (age < kFadeSeconds)
                        billboard->colour.alpha = static_cast<uint8_t>(ee::toInt(age * kFadeRate));
                    else if (billboard->life - kFadeSeconds < age)
                        billboard->colour.alpha = static_cast<uint8_t>(ee::toInt((billboard->life - age) * kFadeRate));
                    else
                        billboard->colour.alpha = kOpaque;
                }
                if (!next)
                    break;
                billboard = next;
            }
        }
        applyBehaviours(effect, data);
        return count;
    }
    READABLE_FUNCTION_OWN(updateBillboards, Draft, bmx, updateQueuedBillboards);

    // updateQueuedBillboards (ZGE4's updateBillboards): when the emitter's
    // look changed, the queue's look built from it (the colour as start and
    // end, the params' size and end size, the look's life, the live rate, a
    // ring for a life's worth of billboards and one, the five billboard words,
    // spawnEmitterBillboard with the object; the bound acceleration, turned
    // into the emitter's frame and scaled, as the acceleration) and given to
    // the effect's record; then, with an object, the record placed at the
    // emitter's matrix, emitting as the data is, and updated: its billboard
    // count. 0 for another word or no object.
    int32_t updateQueuedBillboards(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleEffect> effect, uint32_t word, ee::Ptr<ParticleEmitterData> data)
    {
        ee::Local<BillboardLook> look;
        ee::Local<Matrix> turn;
        if (word != kUpdateWord)
            return 0;
        const ee::Ptr<ParticleLook> settings = data->settings->look;
        if (lookChanged(*settings) & kLookChanged)
        {
            lookChanged(*settings) &= ~static_cast<uint64_t>(kLookChanged);
            look->material = ee::Ptr<Material>(settings->material);
            look->startColour = settings->colour;
            look->endColour = settings->colour;
            look->startSize = data->params.size;
            look->endSize = data->params.endSize;
            look->life = settings->life;
            look->spawnRate = emitterBurst(*data).rate;
            look->acceleration.x = ee::Float(0.0f);
            look->acceleration.y = ee::Float(0.0f);
            look->acceleration.z = ee::Float(0.0f);
            look->fadeInSeconds = billboardWords(*data).fadeInSeconds;
            look->fadeOutSeconds = billboardWords(*data).fadeOutSeconds;
            look->spawner = BillboardSpawner(sym::bmx::spawnEmitterBillboard);
            look->spawnerContext = ee::Address(emitterObject(*data));
            look->viewRadius = billboardWords(*data).viewRadius;
            look->viewDistance = billboardWords(*data).viewDistance;
            look->capacity = sce::libgcc::fptoui(settings->life * emitterBurst(*data).rate + ee::Float(1.0f));
            for (uint32_t item = walkFirst(*data->bindings); item; walkNext(*data->bindings, item))
            {
                const ee::Ptr<ParticleBehaviour> behaviour = ee::Ptr<EmitterBinding>(item)->behaviour;
                if (behaviour->kindHash == g_billboardAcceleration->kindHash && behaviour->typeIndex == g_billboardAcceleration->typeIndex)
                {
                    matrixTranspose3(turn, emitterMatrixOf(emitterObject(*data)));
                    look->acceleration = rotated(*turn, ee::Ptr<EmitterBinding>(item)->definition->acceleration) * kBillboardAccelerationScale;
                    break;
                }
            }
            setBillboardEffectLook(effect->billboardEffect, look);
        }
        if (!emitterObject(*data))
            return 0;
        setBillboardEffectMatrix(effect->billboardEffect, emitterMatrixOf(emitterObject(*data)));
        setBillboardEffectEmitting(effect->billboardEffect, static_cast<uint32_t>(data->emitting));
        return static_cast<int32_t>(updateBillboardEffect(effect->billboardEffect));
    }

    // spawnBillboards (the billboard type's spawn): count billboards (its low
    // byte) at the emitter, each with its start and velocity from the
    // emitter's settings, the look's colour (alpha 0: it fades in), life and
    // size (the type's defaults for an object with no data). Nothing without
    // a type or an effect. 0.
    uint32_t spawnBillboards(ee::Ptr<ParticleType> type, ee::Ptr<SceneObject> object, ee::Ptr<ParticleEffect> effect, uint32_t count,
                             ee::Ptr<ParticleEmitterData> data)
    {
        ee::Local<Vec4> origin;
        ee::Local<Matrix> turn;
        const uint32_t billboards = count & 0xFFu;
        const ee::Ptr<ParticleLook> look = data ? data->settings->look : ee::Ptr<ParticleLook>(sym::g_billboardDefaults);
        if (!type || !effect)
            return 0;
        emitterPosition(object, origin);
        emitterMatrix(object, turn);
        for (uint32_t made = 0; made < billboards;)
        {
            const ee::Ptr<Billboard> billboard = newBillboard();
            billboard->age = 0.0f;
            billboard->colour = look->colour;
            billboard->colour.alpha = 0;
            particleSpawnPoint(&billboard->position, origin, object);
            billboard->previous = billboard->position;
            ++made;
            particleSpawnVelocity(&billboard->velocity, turn, object);
            billboard->size = look->size;
            billboard->sizeNow = look->size;
            billboard->life = look->life;
            billboard->material = look->material;
            listPush(&effect->particles, &billboard->node);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS_OWN(spawnBillboards, Draft, bmx, spawnNoBillboards);

    // spawnNoBillboards (ZGE4's spawnBillboards): nothing; the queue's spawner
    // spawns the emitter's billboards by its rate. 0.
    uint32_t spawnNoBillboards(ee::Ptr<ParticleType> /*type*/, ee::Ptr<SceneObject> /*object*/, ee::Ptr<ParticleEffect> /*effect*/, uint32_t /*count*/,
                               ee::Ptr<ParticleEmitterData> /*data*/)
    {
        return 0;
    }

    // drawBillboards (the billboard type's draw): each billboard a square of
    // its size this frame, centred on it and facing the camera (its corners
    // through the camera's turn), in its colour and material, the material
    // set not to write depth. Returns how many.
    int32_t drawBillboards(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleEffect> effect)
    {
        ee::Local<Quad> quad;
        ee::Local<Vertex[4]> corners;
        ee::Local<Matrix> camera;
        ee::Local<ParticleColour> colour;
        ee::Local<Vec4> offset;
        for (uint32_t corner = 0; corner < 4; ++corner)
            quad->corners[corner] = &corners[corner];
        int32_t count = 0;
        *camera = *cachedMatrix(ee::Ptr<MatrixCache>(*g_currentCamera), kCameraMatrix);
        for (ee::Ptr<Billboard> billboard = recordOfNode<Billboard>(effect->particles.head); billboard;
             billboard = recordOfNode<Billboard>(billboard->node.next))
        {
            *colour = billboard->colour;
            quad->texture = billboard->material;
            zgui::rebuildMaterialRegisters(ee::Ptr<Material>(billboard->material), 0, kStateDepthWrite, 0);
            // The camera's turn about the billboard: its corners' offsets go
            // through it.
            camera->translation() = billboard->position;
            ++count;
            placeCorners(&corners[0], *camera, *colour, billboard->sizeNow, *offset);
            drawPrimitiveQuads(0, quad, 1, 0);
        }
        return count;
    }
    READABLE_FUNCTION_WAITS_OWN(drawBillboards, Draft, bmx, queueBillboards);

    // queueBillboards (ZGE4's drawBillboards): the effect's record queued for
    // the view's packet (queueBillboardEffect's 0).
    int32_t queueBillboards(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleEffect> effect)
    {
        return static_cast<int32_t>(queueBillboardEffect(effect->billboardEffect));
    }

    // accelerateBillboards (the billboards' acceleration behaviour): the
    // definition's acceleration (in g) over this frame added to each
    // billboard's velocity.
    void accelerateBillboards(ee::Ptr<ParticleBehaviour> /*behaviour*/, ee::Ptr<ParticleEffect> effect, ee::Ptr<EmitterDefinition> definition)
    {
        const ee::Float seconds = zge::frameSeconds();
        *g_billboardGravityStep = definition->acceleration;
        *g_billboardGravityStep = scaledCarrying(*g_billboardGravityStep, seconds * kGravity, upperHalf(definition->acceleration));
        if (!effect)
            return;
        for (ee::Ptr<Billboard> billboard = recordOfNode<Billboard>(effect->particles.head); billboard;
             billboard = recordOfNode<Billboard>(billboard->node.next))
            billboard->velocity = billboard->velocity + *g_billboardGravityStep;
    }
    READABLE_FUNCTION(accelerateBillboards, Draft);

    // bounceBillboards (the billboards' deflector behaviour): a billboard in
    // front of the definition's plane and less than a unit from it has its
    // velocity reflected off the plane.
    void bounceBillboards(ee::Ptr<ParticleBehaviour> /*behaviour*/, ee::Ptr<ParticleEffect> effect, ee::Ptr<EmitterDefinition> definition)
    {
        if (!effect)
            return;
        for (ee::Ptr<Billboard> billboard = recordOfNode<Billboard>(effect->particles.head); billboard;
             billboard = recordOfNode<Billboard>(billboard->node.next))
        {
            const ee::Float distance = planeDistance(&definition->plane, &billboard->position);
            if (!(distance < 0.0f) && distance < 1.0f)
                reflectVector(&billboard->velocity, &billboard->velocity, &definition->plane);
        }
    }
    READABLE_FUNCTION(bounceBillboards, Draft);

    // registerBillboardParticles: the billboard pool made, the "regular
    // billboard" type registered under its name's hash, its acceleration and
    // deflector registered and both bound to it (the last binding's result).
    // Unlike the spark type it never sets its behaviours' kind hashes: they
    // keep the data's 0 (or what an earlier registration left), so both
    // bindings ask for the behaviour of that kind hash. ZGE4 (BMX XXX) makes
    // no billboard pool and sets the kind hashes as the spark type does.
    uint32_t registerBillboardParticles()
    {
        const ee::Ptr<ParticleType> type(sym::g_billboardType);
        const ee::Ptr<ParticleBehaviour> acceleration(sym::g_billboardAcceleration);
        const ee::Ptr<ParticleBehaviour> deflector(sym::g_billboardDeflector);
        if constexpr (!game::readable::kIsBmx)
            *g_billboardPool = ee::Ptr<Pool>(createPool(sizeof(Billboard), kBillboardPoolBlock, 0));
        type->nameHash = nameHash(ee::Ptr<const char>(sym::g_textRegularBillboard));
        registerParticleType(type);
        if constexpr (game::readable::kIsBmx)
        {
            acceleration->kindHash = behaviourKindHash(kAccelerationBehaviour);
            deflector->kindHash = behaviourKindHash(kDeflectorBehaviour);
        }
        registerParticleBehaviour(acceleration);
        registerParticleBehaviour(deflector);
        bindParticleBehaviour(type->nameHash, acceleration->kindHash, 0);
        return game::tail(bindParticleBehaviour, type->nameHash, deflector->kindHash, 0);
    }
    READABLE_FUNCTION_WAITS(registerBillboardParticles, Draft);

    // ---- Polys ----

    namespace
    {
        // A new poly's start angle, spin rate and axis from the emitter's
        // data (spawnPolys). randomTurn and spreadTurn are the frame's places
        // for the random axis's turn and the given axis's.
        void startSpin(ee::Ptr<Poly> poly, ee::Ptr<ParticleEmitterData> data, EulerAngles &randomTurn, EulerAngles &spreadTurn)
        {
            poly->angle = data->startAngle;
            poly->angle = static_cast<Angle>(poly->angle + ee::toInt(ee::toFloat(data->startAngle) * spreadFactor() * data->startAngleSpread));
            const ee::Float periodSpread = data->spinPeriod * spreadFactor() * data->spinPeriodSpread;
            if (data->spinAxisKind == kSpinAboutRandomAxis)
            {
                poly->spinAxis.x = 1.0f;
                poly->spinAxis.w = 1.0f;
                poly->spinAxis.y = 0.0f;
                poly->spinAxis.z = 0.0f;
                randomTurn.x = static_cast<Angle>(randomNumber() % kRandomAngles);
                randomTurn.y = static_cast<Angle>(randomNumber() % kRandomAngles);
                randomTurn.z = static_cast<Angle>(randomNumber() % kRandomAngles);
                vec3RotateEuler(&poly->spinAxis.xyz(), &poly->spinAxis.xyz(), &randomTurn);
            }
            else if (data->spinAxisKind == kSpinAboutGivenAxis)
            {
                poly->spinAxis = data->spinAxis;
                if (data->spinAxisSpread != 0)
                {
                    spreadTurn.x = static_cast<Angle>(ee::toInt(ee::toFloat(data->spinAxisSpread) * spreadFactor()));
                    spreadTurn.y = static_cast<Angle>(ee::toInt(ee::toFloat(data->spinAxisSpread) * spreadFactor()));
                    spreadTurn.z = static_cast<Angle>(ee::toInt(ee::toFloat(data->spinAxisSpread) * spreadFactor()));
                    vec3RotateEuler(&poly->spinAxis.xyz(), &poly->spinAxis.xyz(), &spreadTurn);
                }
            }
            if (data->spinPeriod != 0.0f)
                poly->spinRate = kTurn / (data->spinPeriod + periodSpread);
            else
                poly->spinRate = 0.0f;
        }
    }

    // newPolyEffect (the poly type's start): an effect with no polys yet.
    ee::Ptr<ParticleEffect> newPolyEffect(ee::Ptr<ParticleType> type)
    {
        const ee::Ptr<ParticleEffect> effect(heapAllocate(sizeof(ParticleEffect)));
        initList(&effect->particles);
        effect->type = type;
        return effect;
    }
    READABLE_FUNCTION_WAITS(newPolyEffect, Draft);

    // readPolySettings (the poly type's readSettings): as the billboard
    // type's, with the poly's larger look (its origin not set). With no
    // record the look is left as the heap gave it.
    ee::Ptr<ParticleSettings> readPolySettings(ee::Ptr<ParticleType> type, ee::Ptr<ZpedRecord> record)
    {
        ee::Local<Quaternion> orientation;
        ee::Local<Matrix> turn;
        const ee::Ptr<ParticleSettings> settings(heapAllocate(sizeof(ParticleSettings)));
        const ee::Ptr<PolyLook> look(heapAllocate(sizeof(PolyLook)));
        settings->type = type;
        settings->polyLook = look;
        if (!record)
            return settings;
        look->bytes = sizeof(PolyLook);
        look->typeHash = record->typeHash;
        memoryFill(&look->cleared, 0, sizeof(Vec4));
        look->upVelocity = Vec4{0.0f, -record->params.speed, 0.0f, 1.0f};
        *orientation = Quaternion{record->orientation[0], record->orientation[1], record->orientation[2], record->orientation[3]};
        matrixIdentity(turn);
        quaternionToMatrix(turn, orientation);
        look->upVelocity = transformed(*turn, look->upVelocity);
        look->size = record->params.size;
        look->life = record->params.life;
        look->colour = record->colour;
        look->material = 0;
        return settings;
    }
    READABLE_FUNCTION_WAITS(readPolySettings, Draft);

    // freePolySettings (the poly type's releaseSettings): the look's material
    // released, then the look and the settings freed.
    int32_t freePolySettings(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleSettings> settings)
    {
        const ee::Ptr<PolyLook> look = settings->polyLook;
        if (look->material)
        {
            releaseMaterial(look->material);
            look->material = 0;
        }
        heapFree(settings->polyLook);
        settings->polyLook = ee::Ptr<PolyLook>();
        return tail::freeBlock(settings);
    }
    READABLE_FUNCTION_WAITS(freePolySettings, Draft);

    // setPolyMaterial (the poly type's setMaterialTextures): the material its
    // polys are drawn with.
    void setPolyMaterial(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleSettings> settings, uint32_t material)
    {
        settings->polyLook->material = material;
    }
    READABLE_FUNCTION(setPolyMaterial, Draft);

    // updatePolys (the poly type's update): as updateBillboards (aged, freed
    // past their life, moved, bounds, size grown and shrunk, alpha faded),
    // and each poly turned about its axis by its spin rate over the frame.
    // Returns how many polys there were.
    int32_t updatePolys(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleEffect> effect, uint32_t word, ee::Ptr<ParticleEmitterData> data)
    {
        ee::Local<Vec4> step;
        if (word != kUpdateWord)
            return 0;
        int32_t count = 0;
        const ee::Float seconds = zge::frameSeconds();
        if (effect && effect->particles.head)
        {
            startBounds(effect);
            ee::Ptr<Poly> poly = recordOfNode<Poly>(effect->particles.head);
            while (true)
            {
                ++count;
                poly->age = poly->age + seconds;
                const ee::Ptr<Poly> next = recordOfNode<Poly>(poly->node.next);
                if (poly->life <= poly->age)
                {
                    listRemove(&effect->particles, &poly->node);
                    poolFree(*g_polyPool, poly);
                }
                else
                {
                    *step = poly->velocity * seconds;
                    poly->position = poly->position + *step;
                    takeIntoBounds(effect, poly->position);
                    const ee::Float age = poly->age;
                    if (age < data->params.growTime)
                        poly->sizeNow = poly->size * (age * kSizeGrowth / data->params.growTime + kSizeStart);
                    else if (poly->life - data->params.shrinkTime < age)
                        poly->sizeNow = poly->size * ((poly->life - age) * kSizeGrowth / data->params.shrinkTime + kSizeStart);
                    else
                        poly->sizeNow = poly->size;
                    if (age < kFadeSeconds)
                        poly->colour.alpha = static_cast<uint8_t>(ee::toInt(age * kFadeRate));
                    else if (poly->life - kFadeSeconds < age)
                        poly->colour.alpha = static_cast<uint8_t>(ee::toInt((poly->life - age) * kFadeRate));
                    else
                        poly->colour.alpha = kOpaque;
                    if (poly->spinRate != 0.0f)
                        poly->angle = static_cast<Angle>(poly->angle + ee::toInt(poly->spinRate * seconds));
                }
                if (!next)
                    break;
                poly = next;
            }
        }
        applyBehaviours(effect, data);
        return count;
    }
    READABLE_FUNCTION(updatePolys, Draft);

    // spawnPolys (the poly type's spawn): count polys (its low byte) at the
    // emitter, unturned, each with its start and velocity from the emitter's
    // settings and the look's colour (alpha 0), life and size; with the
    // emitter's data also its start angle, spin rate and axis (startSpin).
    // With no data the look is the type's defaults and the spin is left as
    // the pool slot had it. Nothing without a type or an effect. 0.
    uint32_t spawnPolys(ee::Ptr<ParticleType> type, ee::Ptr<SceneObject> object, ee::Ptr<ParticleEffect> effect, uint32_t count,
                        ee::Ptr<ParticleEmitterData> data)
    {
        ee::Local<Vec4> origin;
        ee::Local<Matrix> turn;
        ee::Local<EulerAngles> randomTurn;
        ee::Local<EulerAngles> spreadTurn;
        const uint32_t polys = count & 0xFFu;
        const ee::Ptr<PolyLook> look = data ? data->settings->polyLook : ee::Ptr<PolyLook>(sym::g_polyDefaults);
        if (!type || !effect)
            return 0;
        emitterPosition(object, origin);
        emitterMatrix(object, turn);
        for (uint32_t made = 0; made < polys; ++made)
        {
            const ee::Ptr<Poly> poly = newPoly();
            poly->age = 0.0f;
            poly->colour = look->colour;
            poly->colour.alpha = 0;
            particleSpawnPoint(&poly->position, origin, object);
            particleSpawnVelocity(&poly->velocity, turn, object);
            quaternionIdentity(&poly->orientation);
            poly->size = look->size;
            poly->sizeNow = look->size;
            poly->life = look->life;
            poly->material = look->material;
            if (data)
                startSpin(poly, data, *randomTurn, *spreadTurn);
            listPush(&effect->particles, &poly->node);
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(spawnPolys, Draft);

    // drawPolys (the poly type's draw): each poly a square of its size this
    // frame, centred on it, turned by its orientation and then by its angle
    // about its axis, in its colour and material, the material set two-sided
    // and not to write depth. Returns how many.
    int32_t drawPolys(ee::Ptr<ParticleType> /*type*/, ee::Ptr<ParticleEffect> effect)
    {
        ee::Local<Quad> quad;
        ee::Local<Vertex[4]> corners;
        ee::Local<Matrix> turn;
        ee::Local<ParticleColour> colour;
        ee::Local<Quaternion> orientation;
        ee::Local<Quaternion> spin;
        ee::Local<Vec4> offset;
        for (uint32_t corner = 0; corner < 4; ++corner)
            quad->corners[corner] = &corners[corner];
        int32_t count = 0;
        matrixIdentity(turn);
        for (ee::Ptr<Poly> poly = recordOfNode<Poly>(effect->particles.head); poly; poly = recordOfNode<Poly>(poly->node.next))
        {
            *colour = poly->colour;
            quad->texture = poly->material;
            zgui::rebuildMaterialRegisters(ee::Ptr<Material>(poly->material), 0, kStateDepthWrite, 0);
            zgui::rebuildMaterialRegisters(ee::Ptr<Material>(quad->texture), 0, kStateTwoSided, 1);
            quaternionCopy(orientation, &poly->orientation);
            quaternionFromAxisAngle(spin, &poly->spinAxis.xyz(), poly->angle);
            quaternionMultiply(orientation, orientation, spin);
            quaternionToMatrix(turn, orientation);
            turn->translation() = poly->position;
            ++count;
            placeCorners(&corners[0], *turn, *colour, poly->sizeNow, *offset);
            drawPrimitiveQuads(0, quad, 1, 0);
        }
        return count;
    }
    READABLE_FUNCTION_WAITS(drawPolys, Draft);

    // acceleratePolys (the polys' acceleration behaviour): the definition's
    // acceleration (in g) over this frame added to each poly's velocity.
    void acceleratePolys(ee::Ptr<ParticleBehaviour> /*behaviour*/, ee::Ptr<ParticleEffect> effect, ee::Ptr<EmitterDefinition> definition)
    {
        const ee::Float seconds = zge::frameSeconds();
        *g_polyGravityStep = definition->acceleration;
        *g_polyGravityStep = scaledCarrying(*g_polyGravityStep, seconds * kGravity, upperHalf(definition->acceleration));
        if (!effect)
            return;
        for (ee::Ptr<Poly> poly = recordOfNode<Poly>(effect->particles.head); poly; poly = recordOfNode<Poly>(poly->node.next))
            poly->velocity = poly->velocity + *g_polyGravityStep;
    }
    READABLE_FUNCTION(acceleratePolys, Draft);

    // bouncePolys (the polys' deflector behaviour): a poly in front of the
    // definition's plane and less than a unit from it has its velocity
    // reflected off the plane.
    void bouncePolys(ee::Ptr<ParticleBehaviour> /*behaviour*/, ee::Ptr<ParticleEffect> effect, ee::Ptr<EmitterDefinition> definition)
    {
        if (!effect)
            return;
        for (ee::Ptr<Poly> poly = recordOfNode<Poly>(effect->particles.head); poly; poly = recordOfNode<Poly>(poly->node.next))
        {
            const ee::Float distance = planeDistance(&definition->plane, &poly->position);
            if (!(distance < 0.0f) && distance < 1.0f)
                reflectVector(&poly->velocity, &poly->velocity, &definition->plane);
        }
    }
    READABLE_FUNCTION(bouncePolys, Draft);

    // registerPolyParticles: the poly pool made, the "poly" type registered
    // under its name's hash, its acceleration and deflector registered and
    // both bound to it (the last binding's result).
    uint32_t registerPolyParticles()
    {
        const ee::Ptr<ParticleType> type(sym::g_polyType);
        const ee::Ptr<ParticleBehaviour> acceleration(sym::g_polyAcceleration);
        const ee::Ptr<ParticleBehaviour> deflector(sym::g_polyDeflector);
        *g_polyPool = ee::Ptr<Pool>(createPool(kPolyRecordBytes, kPolyPoolBlock, 0));
        type->nameHash = nameHash(ee::Ptr<const char>(sym::g_textPoly));
        registerParticleType(type);
        acceleration->kindHash = behaviourKindHash(kAccelerationBehaviour);
        deflector->kindHash = behaviourKindHash(kDeflectorBehaviour);
        registerParticleBehaviour(acceleration);
        registerParticleBehaviour(deflector);
        bindParticleBehaviour(type->nameHash, acceleration->kindHash, 0);
        return game::tail(bindParticleBehaviour, type->nameHash, deflector->kindHash, 0);
    }
    READABLE_FUNCTION_WAITS(registerPolyParticles, Draft);
}
