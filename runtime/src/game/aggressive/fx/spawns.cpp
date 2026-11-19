// The effects the game spawns by name (inlineFx.c): a fire trail between two
// points, the level-up flash (the skater's model tinted yellow and back five
// times), a bail's blood at a point or a bone, a splash in the water and the
// ring it leaves, and the debris of a breaking pane (glass or ice shards along
// its face). Each named effect is spawned on the effects' object
// (g_effectsObject) with its parameters (fx/particles.h, billboard.h
// ShardParameters), from its defaults in .data.
//

#include "aggressive/fx/billboard.h"
#include "aggressive/fx/library.h"
#include "aggressive/sound/events.h"
#include "aggressive/fx/particles.h"
#include "aggressive/level/level_object.h"
#include "aggressive/skater/skater.h"
#include "aggressive/tuning.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "zge/render/part.h"
#include "zge/scene/particle_types.h"
#include "zge/math/random.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/fx/spawns.cpp.h"

namespace aggressive::fx
{
    namespace
    {
        using skater::Player;
        using VectorPtr = ee::Ptr<zge::Vec4>;

        // The scene object the named effects are spawned on (initInlineFx
        // makes it; skater_effects.cpp's trails too). 0: none yet.
        inline ee::Global<uint32_t, sym::g_effectsObject> g_effectsObject;

        // The skater's four effect points (skater_effects.cpp), this frame's,
        // one for each player, and the bones the points past them name.
        constexpr ee::Ptr<zge::Vec4[2]> g_pointsNow{sym::pointsNow};
        constexpr ee::Ptr<uint32_t> g_pointBones{sym::g_pointBones}; // name hashes
        constexpr uint32_t kEffectPoints = 4;
        constexpr VectorPtr g_boneOrigin{sym::boneOrigin}; // (0, 0, 0, 0): the bone's own point

        // The sound of a splash: soundEvent 47 is the main bank's sample
        // placed at a point, at a level.
        constexpr uint32_t kPlacedSample = 47;
        constexpr uint32_t kSplashSample = 0x30;
        constexpr uint32_t kFullLevel = 127;

        // A small random spread: (random & mask) - half, times `step`.
        ee::Float spread(uint32_t random, uint32_t mask, int32_t half, ee::Float step)
        {
            return ee::toFloat(static_cast<int32_t>(random & mask) - half) * step;
        }

        void memset(ee::Address to, uint32_t byte, uint32_t bytes) { ee::game(sym::memset, to, byte, bytes); }
        void playSplashSound(ee::Address position) { aggressive::soundEvent(kPlacedSample, position, kSplashSample, kFullLevel, 0u); }

        // The named effect, on the effects' object once there is one.
        void spawnEffect(uint32_t nameHash, ee::Address parameters)
        {
            const uint32_t object = *g_effectsObject;
            if (object != 0)
                zge::spawnNamedEffect(nameHash, object, parameters);
        }

        inline ee::Global<FiretrailParams, sym::firetrailDefaults> g_firetrailDefaults; // size 0.2, 150 ms
    }

    // fireTrail: a "firetrail" between the two points.
    void fireTrail(VectorPtr head, VectorPtr tail)
    {
        ee::Local<FiretrailParams> trail;
        *trail = *g_firetrailDefaults;
        trail->head = *head;
        trail->tail = *tail;
        spawnEffect(*g_firetrailHash, trail);
    }
    READABLE_FUNCTION_WAITS(fireTrail, Verified);

    namespace
    {
        // A player's level-up flash: the model's tint ramps from dim to
        // bright and back, waiting at each end, five times, then goes back to
        // what it was.
        struct LevelUpFlash
        {
            uint32_t active;          // +0x00
            ee::Ptr<zge::Node> node;  // +0x04 the model's root node
            uint8_t _08[8];
            zge::Vec4 saved;          // +0x10 the tint before
            zge::Vec4 bright;         // +0x20 yellow (1, 1, 0)
            zge::Vec4 dim;            // +0x30 the tint before, less 0.3
            zge::Vec4 tint;           // +0x40 now
            zge::Vec4 step;           // +0x50 a second's change ((bright - dim) / rampTime)
            uint32_t rising;          // +0x60
            uint32_t flashesLeft;     // +0x64
            ee::Float rampTime;       // +0x68 0.05 s
            ee::Float hold;           // +0x6C 0.08 s at each end
            ee::Float wait;           // +0x70 left of the hold
        };
        static_assert(offsetof(LevelUpFlash, step) == 0x50 && offsetof(LevelUpFlash, wait) == 0x70 && sizeof(LevelUpFlash) == 0x80);
        constexpr ee::Ptr<LevelUpFlash> g_levelUpFlashes{sym::levelUpFlashes}; // one a player
        constexpr uint32_t kFlashCount = 5;
        inline ee::Global<ee::Float, sym::g_flashRampTime> g_flashRampTime; // 0.05
        inline ee::Global<ee::Float, sym::g_flashDim> g_flashDim;           // 0.3
        inline ee::Global<ee::Float, sym::g_flashHold> g_flashHold;         // 0.08
    }

    // startLevelUpFlash: the level-up flash started (experience.cpp: a level
    // gained), unless one runs.
    void startLevelUpFlash(ee::Ptr<Player> player)
    {
        LevelUpFlash &flash = g_levelUpFlashes[player->number];
        if (flash.active != 0)
            return;
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(player->node);
        const ee::Ptr<zge::Node> root = zge::sceneRootNode(scene);
        flash.flashesLeft = kFlashCount;
        flash.node = root;
        flash.active = 1;
        zge::nodeAmbient(root, &flash.saved.xyz());
        flash.tint = flash.saved;

        flash.bright = flash.saved;
        flash.bright.x = 1.0f;
        flash.bright.y = 1.0f;
        flash.bright.z = 0.0f;

        const ee::Float less = *g_flashDim;
        flash.dim = flash.saved;
        flash.dim.x = flash.dim.x - less;
        flash.dim.y = flash.dim.y - less;
        flash.dim.z = flash.dim.z - less;

        const ee::Float time = *g_flashRampTime;
        flash.rampTime = time;
        flash.step = zge::Vec4{(flash.bright.x - flash.dim.x) / time, (flash.bright.y - flash.dim.y) / time, (flash.bright.z - flash.dim.z) / time, 1.0f};
        flash.rising = 1;
        flash.hold = *g_flashHold;
        flash.wait = flash.hold;
    }
    READABLE_FUNCTION(startLevelUpFlash, Verified);

    // updateLevelUpFlash: the level-up flash's tick (skater_effects.cpp):
    // after its wait the tint moves by the step, to bright (then down) or to
    // dim (then up, a flash fewer; the last puts the saved tint back), and
    // the model is tinted so.
    void updateLevelUpFlash(ee::Ptr<Player> player)
    {
        LevelUpFlash &flash = g_levelUpFlashes[player->number];
        const ee::Float tick = *g_tickTime;
        if (flash.active == 0)
            return;
        if (tick < flash.wait)
        {
            flash.wait = flash.wait - tick;
            return;
        }
        const zge::Vec4 change = flash.step * tick;
        if (flash.rising)
        {
            flash.tint = flash.tint + change;
            if (flash.bright.x <= flash.tint.x)
            {
                flash.tint = flash.bright;
                flash.rising = 0;
                flash.wait = flash.hold;
            }
        }
        else
        {
            flash.tint = flash.tint - change;
            if (flash.tint.x <= flash.dim.x)
            {
                flash.tint = flash.dim;
                flash.rising = 1;
                flash.wait = flash.hold;
                flash.flashesLeft = flash.flashesLeft - 1;
                if (flash.flashesLeft == 0)
                {
                    flash.active = 0;
                    flash.tint = flash.saved;
                }
            }
        }
        zge::setNodeAmbient(flash.node, &flash.tint.xyz());
    }
    READABLE_FUNCTION(updateLevelUpFlash, Verified);

    namespace
    {
        struct BloodDefaults
        {
            BloodParameters squirt; // size 0.15, 5000 ms
            BloodParameters cloud;  // size 0.15, growth 0.005, 500 ms
        };
        static_assert(sizeof(BloodDefaults) == 0x60);
        inline ee::Global<BloodDefaults, sym::bloodDefaults> g_bloodDefaults;
        inline ee::Global<ee::Float, sym::g_bloodSpeed> g_bloodSpeed; // 0.3 of the skater's velocity
        constexpr float kRandomStep = 1.0f / 128.0f;
    }

    // bleed: blood at one of the skater's effect points (0 .. 3) or at the
    // bone the point table names past them: one to three "bloodsquirt"s
    // thrown out with a third of the skater's velocity and a little random
    // spread, falling to the skater's height, and a "bloodcloud".
    void bleed(ee::Ptr<Player> player, uint32_t pointIndex)
    {
        ee::Local<zge::Vec4> thrown;
        ee::Local<BloodParameters> squirt;
        ee::Local<BloodParameters> cloud;
        const uint32_t point = pointIndex & 0xFFu;
        *squirt = g_bloodDefaults->squirt;
        *cloud = g_bloodDefaults->cloud;
        if (point >= kEffectPoints)
        {
            const ee::Ptr<zge::Scene> scene = zge::sceneOf(player->node);
            const ee::Ptr<zge::SceneBone> bone = zge::findSceneModelBone(scene, g_pointBones[point]);
            zge::bonePoint(scene, ee::Ptr<zge::SceneBone>(bone), g_boneOrigin, &squirt->position, 0);
        }
        else
            squirt->position = g_pointsNow[point][player->number];
        squirt->floorOrGrowth = player->drawnPosition.y;
        *thrown = player->velocity * *g_bloodSpeed;

        const uint32_t squirts = zge::randomNumber() % 3u + 1;
        for (uint32_t made = 0; made < squirts; ++made)
        {
            squirt->velocity = *thrown;
            squirt->velocity.x = squirt->velocity.x + spread(zge::randomNumber(), 0xFFu, 0x80, kRandomStep);
            const uint32_t up = zge::randomNumber();
            squirt->velocity.y = ee::Float(-4.0f) - ee::toFloat(static_cast<int32_t>(up & 3u)) * ee::Float(0.5f);
            squirt->velocity.z = squirt->velocity.z + spread(zge::randomNumber(), 0xFFu, 0x80, kRandomStep);
            spawnEffect(*g_bloodSquirtHash, squirt);
        }

        cloud->position = squirt->position;
        cloud->velocity = *thrown;
        const uint32_t rise = zge::randomNumber();
        cloud->velocity.y = ee::Float(-0.75f) - ee::toFloat(static_cast<int32_t>(rise & 3u)) * ee::Float(0.25f);
        spawnEffect(*g_bloodCloudHash, cloud);
    }
    READABLE_FUNCTION_WAITS(bleed, Verified);

    // animationEventHook (player, the hook's number: 10 or 11): the animation
    // event hook (skaterAnimationEvent's): nothing.
    void animationEventHook(ee::Ptr<skater::Player> /*player*/, uint32_t /*hook*/)
    {
    }
    READABLE_FUNCTION(animationEventHook, Verified);

    // splashWith: a "splashroot" there with the velocity's x and y and the
    // rings' radius, and the splash's sound.
    void splashWith(ee::Address position, ee::Address velocity, ee::Float radius)
    {
        ee::Local<SplashParameters> splash;
        memset(splash, 0, sizeof(SplashParameters));
        splash->position = *VectorPtr(position);
        const zge::Vec4 &thrown = *VectorPtr(velocity);
        splash->velocity.x = thrown.x;
        splash->velocity.y = thrown.y;
        splash->velocity.z = 0.0f;
        splash->radius = radius;
        spawnEffect(*g_splashRootHash, splash);
        playSplashSound(position);
    }
    READABLE_FUNCTION_WAITS(splashWith, Verified);

    namespace
    {
        inline ee::Global<SplashParameters, sym::splashDefaults> g_splashDefaults; // radius 0.4
    }

    // splash: the skater's splash in the water (move_collide.cpp): a
    // "splashroot" thrown up 2.5 to 3.25, down at half the velocity's y but
    // between 10 and 15, and its sound.
    void splash(VectorPtr position, VectorPtr velocity)
    {
        ee::Local<SplashParameters> splash;
        *splash = *g_splashDefaults;
        splash->position = *position;
        const uint32_t random = zge::randomNumber();
        const ee::Float down = velocity->y * ee::Float(-0.5f);
        splash->velocity.x = ee::toFloat(static_cast<int32_t>(random & 3u)) * ee::Float(0.25f) + ee::Float(2.5f);
        if (down < ee::Float(-15.0f))
            splash->velocity.y = -15.0f;
        else if (ee::Float(-10.0f) < down)
            splash->velocity.y = -10.0f;
        else
            splash->velocity.y = down;
        splash->velocity.z = 0.0f;
        spawnEffect(*g_splashRootHash, splash);
        playSplashSound(position);
    }
    READABLE_FUNCTION_WAITS(splash, Verified);

    namespace
    {
        // The rings' points: 16, a sixteenth of a turn apart.
        constexpr uint32_t kRingPoints = 16;
        inline ee::Global<uint16_t, sym::g_ringStep> g_ringStep; // 0xFFF

        // A "splashring": the band between two rings.
        struct SplashBandParameters
        {
            zge::Vec4 toRing[kRingPoints];
            zge::Vec4 fromRing[kRingPoints];
        };
        static_assert(sizeof(SplashBandParameters) == 0x200);
    }

    // spawnSplashRing (blood_splash.cpp's splash root): a "splashring"
    // between two rings of 16 points about the centre: one through `from`,
    // one through `to` (each moved by the offset on x first), turned about
    // the y axis, at their heights.
    void spawnSplashRing(VectorPtr centre, VectorPtr from, VectorPtr to, ee::Float offset)
    {
        ee::Local<SplashBandParameters> band;
        ee::Local<zge::Vec4> fromCentre;
        ee::Local<zge::Vec4> toCentre;
        *fromCentre = *from;
        fromCentre->x = fromCentre->x + offset;
        *fromCentre = *fromCentre - *centre;
        *toCentre = *to;
        toCentre->x = toCentre->x + offset;
        *toCentre = *toCentre - *centre;

        zge::Angle angle = 0;
        for (uint32_t point = 0; point < kRingPoints; ++point)
        {
            zge::Vec4 &onFrom = band->fromRing[point];
            zge::Vec4 &onTo = band->toRing[point];
            zge::vec3RotateY(asVec3(onFrom), asVec3(*fromCentre), angle);
            onFrom.y = fromCentre->y;
            zge::vec3RotateY(asVec3(onTo), asVec3(*toCentre), angle);
            onTo.y = toCentre->y;
            onFrom = onFrom + *centre;
            onTo = onTo + *centre;
            angle = static_cast<zge::Angle>(angle + *g_ringStep);
        }
        spawnEffect(*g_splashRingHash, band);
    }
    READABLE_FUNCTION_WAITS(spawnSplashRing, Verified);

    namespace
    {
        // One of four triangles a shard is scaled from.
        constexpr ee::Ptr<zge::Vec4[3]> g_shardTriangles{sym::shardTriangles};
        constexpr uint32_t kShardDraw = 2;                  // ShardParameters::flags
        inline ee::Global<ee::Float, sym::g_paneLifeMax> g_paneLifeMax;   // kind 1: 3000 ms
        inline ee::Global<ee::Float, sym::g_paneSizeMin> g_paneSizeMin;   // 0.1
        inline ee::Global<ee::Float, sym::g_paneSizeMax> g_paneSizeMax;   // 0.45
        inline ee::Global<ee::Float, sym::g_glassLifeMax> g_glassLifeMax; // the other kinds: 3000 ms
        inline ee::Global<ee::Float, sym::g_glassSizeMin> g_glassSizeMin; // 0.15
        inline ee::Global<ee::Float, sym::g_sixteenth> g_sixteenth;       // 1 / 15: (random & 15) times this, 0 .. 1
        inline ee::Global<ee::Float, sym::g_speedSpread> g_speedSpread;   // 0.01875: the velocity by 0.7 .. 1.28
        constexpr float kPositionStep = 3.0f / 1024.0f;     // (random & 511) - 256 times this, about 0.75 either way
    }

    // debris: a breaking pane's debris: shards ("shard") about the centre in
    // the plane of the normal, each a random triangle of a random size, life
    // and spin, turned at random about the normal and tipped, thrown with the
    // velocity a little spread. Kind 0: 16 glass shards (2 to 3 s, 0.15 to
    // 1); 1: 6 (2 to 3 s, 0.1 to 0.45); 3: 2 of ice (1 to 1.8 s, 0.75 to
    // 2.75); else 4 of glass (2 to 3 s, 0.15 to 1).
    void debris(VectorPtr centre, VectorPtr velocity, VectorPtr normal, uint32_t kindArgument)
    {
        ee::Local<ShardParameters> shard;
        const uint32_t kind = kindArgument & 0xFFu;
        uint32_t count;
        ee::Float lifeMin, lifeMax, sizeMin, sizeMax;
        if (kind == 1)
        {
            count = 6;
            lifeMin = 2000.0f;
            lifeMax = *g_paneLifeMax;
            sizeMin = *g_paneSizeMin;
            sizeMax = *g_paneSizeMax;
            shard->texture = *g_glassTexture;
        }
        else if (kind == 3)
        {
            count = 2;
            lifeMin = 1000.0f;
            lifeMax = 1800.0f;
            sizeMin = 0.75f;
            sizeMax = 2.75f;
            shard->texture = *g_iceGlassTexture;
        }
        else
        {
            count = kind == 0 ? 16 : 4;
            lifeMin = 2000.0f;
            lifeMax = *g_glassLifeMax;
            sizeMin = *g_glassSizeMin;
            sizeMax = 1.0f;
            shard->texture = *g_glassTexture;
        }
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        shard->flags = kShardDraw;
        shard->colour[0] = 0x80;
        shard->colour[1] = 0x80;
        shard->colour[2] = 0xD0;
        shard->colour[3] = 0x30;
        shard->floorHeight = 0.0f;
        const ee::Float lifeRange = lifeMax - lifeMin;
        const ee::Float sizeRange = sizeMax - sizeMin;
        for (uint32_t made = 0; made < count; ++made)
        {
            // Its life and size.
            const uint32_t lifeRandom = zge::randomNumber();
            const ee::Float sixteenth = *g_sixteenth;
            shard->lifetime = static_cast<uint32_t>(ee::toInt(lifeMin + ee::toFloat(static_cast<int32_t>(lifeRandom & 0xFu)) * (lifeRange * sixteenth)));
            const uint32_t sizeRandom = zge::randomNumber();
            const ee::Float size = sizeMin + ee::toFloat(static_cast<int32_t>(sizeRandom & 0xFu)) * (sizeRange * sixteenth);

            // Its triangle: one of four, scaled; textured in its corners.
            const zge::Vec4 (&triangle)[3] = g_shardTriangles[zge::randomNumber() & 3u];
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                QuadVertex &vertex = shard->corners[corner];
                vertex.position = triangle[corner] * size;
                if (shard->texture == 0)
                {
                    vertex.u = 0.0f;
                    vertex.v = 0.0f;
                }
                else
                {
                    vertex.u = ((corner + 1) & 2u) ? 0.0f : 1.0f;
                    vertex.v = (corner & 2u) ? 1.0f : 0.0f;
                }
            }

            // Its spin.
            shard->spin.x = static_cast<int16_t>((zge::randomNumber() & 0xFFFu) - 0x800);
            shard->spin.y = static_cast<int16_t>((zge::randomNumber() & 0xFFFu) - 0x800);
            shard->spin.z = static_cast<int16_t>((zge::randomNumber() & 0xFFFu) - 0x800);

            // Its frame: the normal, turned about y and x at random, made
            // square.
            shard->frame[2] = *normal;
            const uint32_t yaw = zge::randomNumber();
            zge::vec3RotateY(asVec3(shard->frame[1]), asVec3(shard->frame[2]), static_cast<zge::Angle>(yaw));
            const uint32_t pitch = zge::randomNumber();
            zge::vec3RotateX(asVec3(shard->frame[1]), asVec3(shard->frame[1]), static_cast<zge::Angle>(pitch));
            shard->frame[0] = zge::cross(shard->frame[1], shard->frame[2]);
            if (!(zge::vec3Length(asVec3(shard->frame[0])) == ee::Float(0.0f)))
                shard->frame[0] = zge::unit(shard->frame[0]);
            shard->frame[1] = zge::cross(shard->frame[2], shard->frame[0]);

            // Its position: about the centre, in the plane of the normal.
            zge::Vec4 &place = shard->frame[3];
            place.x = centre->x + spread(zge::randomNumber(), 0x1FFu, 0x100, kPositionStep);
            place.y = centre->y + spread(zge::randomNumber(), 0x1FFu, 0x100, kPositionStep);
            place.z = centre->z + spread(zge::randomNumber(), 0x1FFu, 0x100, kPositionStep);
            const zge::Vec4 offset = place - *centre;
            const ee::Float along = zge::dot(offset, *normal);
            place.x = place.x - normal->x * along;
            place.y = place.y - normal->y * along;
            place.z = place.z - normal->z * along;

            // Its velocity: the given one, each axis by 0.7 .. 1.28.
            const ee::Float speedSpread = *g_speedSpread;
            shard->velocity.x = velocity->x * (spread(zge::randomNumber(), 0x1Fu, 0x10, speedSpread) + ee::Float(1.0f));
            shard->velocity.y = velocity->y * (spread(zge::randomNumber(), 0x1Fu, 0x10, speedSpread) + ee::Float(1.0f));
            shard->velocity.z = velocity->z * (spread(zge::randomNumber(), 0x1Fu, 0x10, speedSpread) + ee::Float(1.0f));
            zge::spawnNamedEffect(*g_shardHash, object, shard);
        }
    }
    READABLE_FUNCTION_WAITS(debris, Verified);

    namespace
    {
        // "impactring": a ring on the ground (fx/ripples.cpp's kind).
        struct ImpactRingParams
        {
            zge::Vec4 position;        // +0x00
            zge::Vec4 velocity;        // +0x10
            zge::Quaternion orientation; // +0x20
            uint8_t _30[0x10];
        };
        static_assert(offsetof(ImpactRingParams, orientation) == 0x20 && sizeof(ImpactRingParams) == 0x40);

        // Parameters of which only the position is set here: the rest is the
        // effect's defaults as they stand.
        template <size_t Bytes>
        struct PlacedParameters
        {
            zge::Vec4 position;
            uint8_t fromDefaults[Bytes - sizeof(zge::Vec4)];
        };
        struct DebrisParameters
        {
            zge::Vec4 position;        // +0x00
            uint8_t fromDefaults[0x38];
            ee::Float floorHeight;     // +0x48 the height its pieces land on (y down: moveDebris bounces them there)
            uint32_t lastOfDefaults;   // +0x4C
        };
        static_assert(offsetof(DebrisParameters, floorHeight) == 0x48 && sizeof(DebrisParameters) == 0x50);

        inline ee::Global<ImpactRingParams, sym::impactRingDefaults> g_impactRingDefaults;
        inline ee::Global<PlacedParameters<0x30>, sym::fireflyDefaults> g_fireflyDefaults;
        inline ee::Global<PlacedParameters<0x50>, sym::bubbleDefaults> g_bubbleDefaults;
        inline ee::Global<PlacedParameters<0x50>, sym::explosionDefaults> g_explosionDefaults;
        inline ee::Global<DebrisParameters, sym::debrisDefaults> g_debrisDefaults;
        inline ee::Global<SparkParams, sym::grindSparkDefaults> g_grindSparkDefaults;
        inline ee::Global<SmokeParameters, sym::smokeBlastDefaults> g_smokeBlastDefaults;
        constexpr uint32_t kGrindSparkCount = 5;

        inline ee::Global<ee::Float, sym::g_grindSparkSpeed> g_grindSparkSpeed; // 0.65 of the grind's direction
        inline ee::Global<ee::Float, sym::g_bonePuffSpeedStep> g_bonePuffSpeedStep; // 0.1 a step of the random (0 .. 3)
        inline ee::Global<ee::Float, sym::g_bonePuffSpeedBase> g_bonePuffSpeedBase; // 0.8
        inline ee::Global<ee::Float, sym::g_smokeFloor> g_smokeFloor;         // the "smoke_floor_coll" affector's floor
        inline ee::Global<uint32_t[4], sym::g_smokeBallTexture> g_smokeTextures;    // smoke, steam ball, snow ball, fire ball
        inline ee::Global<uint32_t, sym::g_fxTable> g_fxTable;

        // The park editor's create and delete tools' puff (the pisser's).
        inline ee::Global<uint8_t, sym::g_pisserPuffKind> g_pisserPuffKind;
        inline ee::Global<ee::Float, sym::g_pisserPuffTime> g_pisserPuffTime; // seconds left
        constexpr VectorPtr g_pisserPuffAt{sym::pisserPuffAt};
        inline ee::Global<uint32_t, sym::g_overlayFrames> g_overlayFrames;
        constexpr uint32_t kDefaultFxTable = sym::defaultFxTable;

        // The smoke blast's kinds (its texture and colours).
        enum SmokeBlastKind : uint8_t
        {
            kSmokeBlast = 0,
            kSteamBlast = 1,
            kSnowBlast = 2,
            kFireBlast = 3,
        };
        // The FX tuning's Smoke Blast entries.
        enum SmokeBlastTuning : uint32_t
        {
            kBlastSize = 0,
            kBlastYVel = 1,
            kBlastSpread = 2,
            kBlastEmitFreq = 3,
            kBlastColorTime = 4, // four of them
            kBlastScaleRate = 8,
            kBlastFadeRate = 9,
            kBlastGroundHeight = 10,
        };
        constexpr uint32_t kBlastKeyMs = 800; // a key's time when the colour changes

        // A player's puff from a bone.
        constexpr ee::Ptr<SmokeParameters> g_bonePuff{sym::bonePuff};
    }

    // startPisserCreatePuff: the create tool's puff (snow) at the tool, for a
    // second, 3 lower.
    void startPisserCreatePuff(uint32_t toolPosition)
    {
        const VectorPtr position{toolPosition};
        *g_pisserPuffKind = kSnowBlast;
        *g_pisserPuffTime = ee::Float(1.0f);
        *g_pisserPuffAt = *position;
        g_pisserPuffAt->y = g_pisserPuffAt->y + ee::Float(3.0f);
    }
    READABLE_FUNCTION(startPisserCreatePuff, Verified);

    // startPisserDeletePuff: the delete tool's (fire).
    void startPisserDeletePuff(uint32_t toolPosition)
    {
        const VectorPtr position{toolPosition};
        *g_pisserPuffKind = kFireBlast;
        *g_pisserPuffTime = ee::Float(1.0f);
        *g_pisserPuffAt = *position;
        g_pisserPuffAt->y = g_pisserPuffAt->y + ee::Float(3.0f);
    }
    READABLE_FUNCTION(startPisserDeletePuff, Verified);

    void spawnSmokeBlast(uint32_t kind, VectorPtr position, ee::Ptr<SmokeParameters> record);

    // updatePisserPuff (updateEffects' fourth step): while the tools' puff
    // lasts, a smoke blast of its kind each frame.
    void updatePisserPuff(ee::Float seconds)
    {
        const ee::Float left = *g_pisserPuffTime;
        if (left == ee::Float(0.0f))
            return;
        *g_pisserPuffTime = seconds < left ? left - seconds : ee::Float(0.0f);
        game::tail(aggressive::fx::spawnSmokeBlast, static_cast<uint32_t>(*g_pisserPuffKind), g_pisserPuffAt, ee::Ptr<SmokeParameters>());
    }
    READABLE_FUNCTION_WAITS(updatePisserPuff, Verified);

    // idleFxStage: a draw stage of the effects that does nothing.
    uint32_t idleFxStage()
    {
        return 1;
    }
    READABLE_FUNCTION(idleFxStage, Verified);

    // drawFxOverlay: the effects' overlay stage: counted, and the screen flash.
    uint32_t drawFxOverlay()
    {
        *g_overlayFrames = *g_overlayFrames + 1;
        drawScreenFlash();
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawFxOverlay, Verified);

    // useDefaultFxTable: the effects' table set to the defaults (no caller).
    void useDefaultFxTable()
    {
        *g_fxTable = kDefaultFxTable;
    }
    READABLE_FUNCTION(useDefaultFxTable, Draft);

    // impactRing (airExit's): an impact ring half a unit up, spreading at
    // twice the skater's horizontal velocity.
    void impactRing(VectorPtr position, ee::Ptr<zge::Quaternion> orientation, VectorPtr velocity)
    {
        ee::Local<ImpactRingParams> ring;
        *ring = *g_impactRingDefaults;
        ring->position = *position;
        ring->position.y = ring->position.y - ee::Float(0.5f);
        zge::quaternionCopy(&ring->orientation, orientation);
        ring->velocity = *velocity;
        ring->velocity.y = 0.0f;
        ring->velocity = ring->velocity * ee::Float(2.0f);
        spawnEffect(*g_impactRingHash, ring);
    }
    READABLE_FUNCTION_WAITS(impactRing, Verified);

    // spawnFireflies: fireflies there (no caller).
    void spawnFireflies(VectorPtr position)
    {
        ee::Local<PlacedParameters<0x30>> fireflies;
        *fireflies = *g_fireflyDefaults;
        fireflies->position = *position;
        spawnEffect(*g_fireflyHash, fireflies);
    }
    READABLE_FUNCTION_WAITS(spawnFireflies, Draft);

    // spawnBubbles: bubbles there (no caller).
    void spawnBubbles(VectorPtr position)
    {
        ee::Local<PlacedParameters<0x50>> bubbles;
        *bubbles = *g_bubbleDefaults;
        bubbles->position = *position;
        spawnEffect(*g_bubbleHash, bubbles);
    }
    READABLE_FUNCTION_WAITS(spawnBubbles, Draft);

    // spawnExplosion: an explosion there, and with `withDebris` its debris,
    // the pieces landing at `floorHeight`.
    void spawnExplosion(ee::Address position, uint32_t withDebris, ee::Float floorHeight)
    {
        ee::Local<PlacedParameters<0x50>> explosion;
        ee::Local<DebrisParameters> debris;
        *explosion = *g_explosionDefaults;
        *debris = *g_debrisDefaults;
        explosion->position = *VectorPtr(position);
        debris->position = *VectorPtr(position);
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        zge::spawnNamedEffect(*g_explosionHash, object, explosion);
        if (withDebris)
        {
            debris->floorHeight = floorHeight;
            zge::spawnNamedEffect(*g_debrisHash, object, debris);
        }
    }
    READABLE_FUNCTION_WAITS(spawnExplosion, Verified);

    // grindSparks (skaterEffects'): a grind's five sparks at `position`,
    // thrown along 0.65 of `direction`, following `source`.
    void grindSparks(ee::Ptr<skater::Player> source, VectorPtr position, VectorPtr direction, uint32_t on)
    {
        ee::Local<SparkParams> sparks;
        *sparks = *g_grindSparkDefaults;
        if (!on)
            return;
        const uint32_t object = *g_effectsObject;
        if (object == 0)
            return;
        sparks->position = *position;
        sparks->velocity = *direction * *g_grindSparkSpeed;
        sparks->count = kGrindSparkCount;
        sparks->source = source;
        zge::spawnNamedEffect(*g_sparkHash, object, sparks);
    }
    READABLE_FUNCTION_WAITS(grindSparks, Verified);

    // spawnSmokeBlast: a smoke blast. With no record, the defaults with the
    // FX tuning's Smoke Blast values (and its Ground Height for the smoke's
    // floor), the kind's texture and colours, at `position` lowered by its
    // size; else the record as it is. The colour rates follow from the keys
    // over the first key's time.
    void spawnSmokeBlast(uint32_t kind, VectorPtr position, ee::Ptr<SmokeParameters> record)
    {
        ee::Local<SmokeParameters> blast;
        const uint8_t which = static_cast<uint8_t>(kind);
        *blast = *g_smokeBlastDefaults;
        if (!record)
        {
            const ee::Ptr<TuningEntry> fx = (*g_tuningCategories)[kTuningFx].entries;
            const auto value = [&](uint32_t entry) { return fx[entry].value; };
            blast->countWord = ee::toInt(value(kBlastEmitFreq));
            blast->velocity.y = value(kBlastYVel);
            blast->size = value(kBlastSize);
            blast->positionSpread.x = value(kBlastSpread);
            blast->positionSpread.z = value(kBlastSpread);
            for (uint32_t key = 0; key < 4; ++key)
                blast->keyTime[key] = static_cast<uint32_t>(ee::toInt(value(kBlastColorTime + key)));
            blast->growth = value(kBlastScaleRate);
            *g_smokeFloor = value(kBlastGroundHeight);
            blast->fade = static_cast<uint32_t>(ee::toInt(value(kBlastFadeRate)));

            // The kind: smoke and steam start in their second key's colour
            // and keep it; those and snow change colour over 800 ms a key
            // (from 1 ms).
            if (which <= kSteamBlast)
            {
                std::memcpy(blast->keyColour[3], blast->keyColour[1], 4);
                std::memcpy(blast->keyColour[2], blast->keyColour[3], 4);
                std::memcpy(blast->keyColour[0], blast->keyColour[2], 4);
                blast->texture = which == kSteamBlast ? *g_steamBallTexture : (*g_smokeTextures)[0];
            }
            else if (which == kSnowBlast)
                blast->texture = *g_snowBallTexture;
            else if (which == kFireBlast)
                blast->texture = *g_fireBallTexture;
            if (which <= kSnowBlast)
            {
                blast->keyTime[0] = 1;
                blast->keyTime[1] = kBlastKeyMs;
                blast->keyTime[2] = kBlastKeyMs;
                blast->keyTime[3] = kBlastKeyMs;
            }
            blast->position = *position;
            blast->position.y = blast->position.y - blast->size;
        }
        else
            zge::memoryCopy(blast, record, static_cast<uint32_t>(sizeof(SmokeParameters)));
        const ee::Float firstKey = ee::toFloat(static_cast<int32_t>(blast->keyTime[0]));
        for (uint32_t c = 0; c < 4; ++c)
            blast->colourRate[c] = ee::toFloat(static_cast<int32_t>(blast->keyColour[1][c]) - static_cast<int32_t>(blast->keyColour[0][c])) / firstKey;
        spawnEffect(*g_smokeHash, blast);
    }
    READABLE_FUNCTION_WAITS(spawnSmokeBlast, Verified);

    // unusedSpawnHook: empty (no caller and no reference in the game).
    void unusedSpawnHook()
    {
    }
    READABLE_FUNCTION(unusedSpawnHook, Verified);

    // spawnBonePuff: while the player moves faster than 1, a puff of the
    // kind's smoke at the point's bone, drifting with the player's velocity
    // times 0.8 .. 1.1 at random (no caller).
    void spawnBonePuff(uint32_t playerIndex, uint32_t point, uint32_t kind)
    {
        ee::Local<zge::Vec4> onBone;
        const ee::Ptr<Player> player(parts::skaterHandle(static_cast<int32_t>(playerIndex & 0xFFu)));
        if (!(ee::Float(1.0f) < player->speed))
            return;
        const uint32_t random = zge::randomNumber();
        const ee::Float speed = ee::toFloat(static_cast<int32_t>(random & 0x3u)) * *g_bonePuffSpeedStep + *g_bonePuffSpeedBase;
        g_bonePuff->velocity = player->velocity * speed;
        *onBone = zge::Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(player->node);
        const ee::Ptr<zge::SceneBone> bone = zge::findSceneModelBone(scene, g_pointBones[point & 0xFFu]);
        const ee::Ptr<zge::Scene> sameScene = zge::sceneOf(player->node);
        zge::bonePoint(sameScene, ee::Ptr<zge::SceneBone>(bone), onBone, &g_bonePuff->position, 0);
        g_bonePuff->texture = (*g_smokeTextures)[kind];
        spawnSmokeBlast(kind & 0xFFu, VectorPtr(), g_bonePuff);
    }
    READABLE_FUNCTION_WAITS(spawnBonePuff, Draft);

    namespace
    {
        inline ee::Global<int32_t, sym::g_clearPosters> g_clearPosters; // posters.cpp
    }

    // destroySkidMarks (a script native): every poster (the skid marks and
    // other decals) goes on the posters' next move. Always 1.
    int32_t destroySkidMarks()
    {
        *g_clearPosters = 1;
        return 1;
    }
    READABLE_FUNCTION(destroySkidMarks, Verified);

    void inlineFxNoopB()
    {
    }
    READABLE_FUNCTION(inlineFxNoopB, Draft);

    void inlineFxNoopC()
    {
    }
    READABLE_FUNCTION(inlineFxNoopC, Draft);

    void inlineFxNoopD()
    {
    }
    READABLE_FUNCTION(inlineFxNoopD, Draft);
}
