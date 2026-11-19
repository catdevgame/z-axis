// DM2's rider respawn (physics.c): the level's spawn locators, the ground
// under a spawn point, and the face the rider starts on.
//
// A spawn is a FaceRef (collision.h: the scene node in the low word, the
// face in the high word), whole in v0: resetPhysics keeps all 8 bytes.
//
// Counterparts: DM2 GameCube 0x80077038
// (spawnPoint), 0x800771A4, 0x8007748C, 0x80077D8C, 0x80078134, 0x80095518
// (locatorMatrix).

#include "../../level/level_collision.h"
#include "physics.h"
#include "dm2/editor/start_tool.h"

#include "../collision/collision.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "zge/collision/dynamics.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/math/quaternion.h"
#include "zge/math/vector.h"
#include "zge/scene/locators.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "sym/dm2/rider/physics/respawn.cpp.h"

namespace game::dm2::rider::physics
{
    using collision::EdgeRef;
    using collision::FaceRef;

    namespace
    {
        // The locators' names (nameHash's strings).
        constexpr uint32_t kSpawnName = sym::spawnName;  // "Spawn" (spawnPoint's)
        constexpr uint32_t kPlayerName = sym::dm2PlayerName; // "Player"
        constexpr uint32_t kSpawnKind = sym::spawnKind;  // "Spawn" (locatorMatrix's: the same string twice)


        // spawnPoint's last spawn face (and its result).
        inline ee::Global<FaceRef, sym::g_spawnFace> g_spawnFace;
        // The session's level (the session's bytes: +1 the game type, +2
        // the level, +3 the players). Level 8 is a park of the Park Editor,
        // which starts at its locators (spawnAtLocator).
        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;
        constexpr uint8_t kLevelStartsAtLocators = 8;
        // Nearest so far, to start (FLT_MAX).
        inline ee::Global<ee::Float, sym::g_spawnFar> g_spawnFar;
        inline ee::Global<ee::Float, sym::g_locatorFar> g_locatorFar;
        // groundBelowSpawn: the ray from this far above the point to just
        // above the level's bottom.
        inline ee::Global<ee::Float, sym::g_spawnRayAbove> g_spawnRayAbove;  // 20000
        inline ee::Global<ee::Float, sym::g_spawnRayBottom> g_spawnRayBottom; // 0.01
        // spawnAtPlayer's search: the ring's growth a step (1.1), and the
        // height its last try drops from (-10000).
        inline ee::Global<ee::Float, sym::g_ringGrowth> g_ringGrowth;
        inline ee::Global<ee::Float, sym::g_dropHeight> g_dropHeight;

        // The surface sounds no spawn is on (6 and 7: water, by the sounds' table).
        constexpr uint32_t kNoSpawnSoundFirst = 6;
        constexpr uint32_t kNoSpawnSoundEnd = 8;
        // The surface kinds a spawn may be on.
        constexpr uint32_t kSurfaceGround = 0;
        constexpr uint32_t kSurfaceKind4 = 4;
        // The steepest a spawn face may be (30 degrees: acos(-normal.y)).
        constexpr int32_t kSpawnSteepest = 0x1556;
        // The spiral's step: 60 degrees a try (0x10000 a turn), in the upper
        // half of the word the original steps (its angle is the upper half,
        // sign-extended).
        constexpr uint32_t kSpiralStep = 0x2AAA0000u;
        constexpr int32_t kSpiralTries = 24;

        uint32_t hashOf(uint32_t text) { return zge::nameHash(ee::pointerAt<char>(text)); }

        // A spiral's face: wide enough (its shortest side 1.5), within 4
        // below and 2 above the point (unless `anyHeight`), flatter than 30
        // degrees, of kind 0 or 4, not water, with room above its centre
        // (`probe` gets the centre less 1.25).
        bool spawnFaceFits(const GroundHit &hit, const ee::Float *point, bool anyHeight, ee::Float *probe)
        {
            const FaceRef at = static_cast<FaceRef>(hit.face);
            if (!(ee::Float(1.5f) <= collision::faceShortestSide(at)))
                return false;
            if (!anyHeight)
            {
                const ee::Float rise = hit.point[1] - point[1];
                if (!(rise < ee::Float(4.0f) && ee::Float(-2.0f) <= rise))
                    return false;
            }
            if (!(zge::angleAcos(-hit.plane[1]) < kSpawnSteepest && collision::acceptAll() != 0))
                return false;
            const uint32_t kind = collision::faceSurfaceKind(at);
            if (!(kind == kSurfaceGround || kind == kSurfaceKind4))
                return false;
            const uint32_t sound = collision::faceSound(at);
            if (sound < kNoSpawnSoundEnd && sound >= kNoSpawnSoundFirst)
                return false;
            collision::faceCentre(at, probe);
            probe[1] = probe[1] - ee::Float(1.25f);
            return collision::sphereQuery(probe, ee::Float(1.0f)) == nullptr;
        }

        // The ring of tries out from the point (unless `flags` bit 0): each
        // 60 degrees on and 1.1 times further (the first at the point, the
        // second 1 out), the ground under each (groundBelowSpawn's long ray
        // with `longRay`, else groundBelow's), until a face fits (unless
        // `flags` bit 1, at any height). Its face, or 0.
        FaceRef spawnSpiral(const ee::Float *point, uint32_t flags, ee::Float growth, GroundHit &hit, ee::Float *probe, bool longRay)
        {
            FaceRef face = 0;
            ee::Float radius = 0.0f;
            int32_t angle = 0;
            uint32_t nextAngle = kSpiralStep;
            probe[0] = radius;
            for (int32_t tries = kSpiralTries;;)
            {
                probe[1] = 0.0f;
                probe[2] = 0.0f;
                zge::vec3RotateY(&vec4(probe)->xyz(), &vec4(probe)->xyz(), angle);
                *vec4(probe) = *vec4(probe) + *vec4(point);
                const int32_t ground = longRay ? groundBelowSpawn(probe, &hit) : groundBelow(probe, &hit);
                if (ground != 0 && (hit.flags & kGroundObject) == 0 && spawnFaceFits(hit, point, (flags & 2u) != 0, probe))
                    face = static_cast<FaceRef>(hit.face);
                angle = static_cast<int32_t>(nextAngle) >> 16;
                nextAngle += kSpiralStep;
                if (radius == ee::Float(0.0f))
                    radius = radius + ee::Float(1.0f);
                else
                    radius = radius * growth;
                if (face != 0 || --tries == 0)
                    return face;
                probe[0] = radius;
            }
        }
    }

    // The matrix of the locator named `name` nearest the point: its rotation
    // and position into `matrix`. 0 if the level has none. GameCube
    // 0x80095518.
    int32_t locatorMatrix(const ee::Float *point, uint32_t name, zge::Matrix *matrix)
    {
        ee::Local<zge::LocatorList> list;
        ee::Local<ee::Float[4]> apart;
        ee::Float nearest = *g_locatorFar;
        ee::Ptr<zge::Locator> best;
        zge::findLocators(list, hashOf(kSpawnKind), name);
        for (ee::Ptr<zge::Locator> entry = list->head; entry; entry = entry->next)
        {
            *vec4(apart) = *vec4(point) - *vec4(locatorPoint(*entry).position);
            const ee::Float distance = collision::engine::lengthSquared(apart);
            if (distance < nearest)
            {
                nearest = distance;
                best = entry;
            }
        }
        if (!best)
        {
            zge::freeLocators(list);
            return 0;
        }
        ScenePoint &spawn = locatorPoint(*best);
        zge::matrixIdentity(matrix);
        zge::quaternionToMatrix(matrix, &spawn.rotation);
        matrix->translation() = *vec4(spawn.position);
        zge::freeLocators(list);
        return 1;
    }
    READABLE_FUNCTION(locatorMatrix, Verified);

    // The face under the Spawn locator named `name` nearest the point (a
    // face, not an object, below it); kept in g_spawnFace. 0 if none.
    // GameCube 0x80077038.
    FaceRef spawnPoint(const ee::Float *point, uint32_t name)
    {
        ee::Local<GroundHit> hit;
        ee::Local<zge::LocatorList> list;
        ee::Local<ee::Float[4]> apart;
        ee::Float nearest = *g_spawnFar;
        bool found = false;
        *g_spawnFace = 0;
        zge::findLocators(list, hashOf(kSpawnName), name);
        for (ee::Ptr<zge::Locator> entry = list->head; entry; entry = entry->next)
        {
            const ee::Float *position = locatorPoint(*entry).position;
            *vec4(apart) = *vec4(point) - *vec4(position);
            const ee::Float distance = collision::engine::lengthSquared(apart);
            if (found && !(distance < nearest))
                continue;
            if (groundBelow(position, hit) != 0)
            {
                if (hit->flags & kGroundObject)
                    continue;
                nearest = distance;
                found = true;
                *g_spawnFace = static_cast<FaceRef>(hit->face);
            }
        }
        zge::freeLocators(list);
        return *g_spawnFace;
    }
    READABLE_FUNCTION(spawnPoint, Draft);

    // The face under the nearest Spawn locator named Player. GameCube
    // 0x800771A4.
    FaceRef playerSpawnPoint(const ee::Float *point)
    {
        return spawnPoint(point, hashOf(kPlayerName));
    }
    READABLE_FUNCTION(playerSpawnPoint, Draft);

    // The ground under a spawn point: a ray from high above it to the level's
    // bottom. A face facing up inside the level is the ground (its point,
    // plane and the face, or the object it belongs to): 1; else 0. As
    // groundBelow, with the long ray and the level test.
    // GameCube 0x8007748C.
    int32_t groundBelowSpawn(const ee::Float *point, GroundHit *hit)
    {
        ee::Local<ee::Float[4]> from;
        ee::Local<ee::Float[4]> to;
        ee::Local<ee::Float[4]> plane;
        *vec4(from) = *vec4(point);
        *vec4(to) = *vec4(point);
        from[1] = from[1] - *g_spawnRayAbove;
        const ee::Float bottom = levelBounds()->max[1];
        hit->flags = 0;
        hit->face = 0;
        hit->object = 0;
        to[1] = bottom - *g_spawnRayBottom;

        const collision::RayHit *found = collision::rayCast(from, to);
        if (!found)
            return 0;
        collision::hitPlane(found, plane);
        const FaceRef face = collision::hitFace(found);
        if (!(plane[1] < 0.0f))
            return 0;
        collision::hitPoint(found, hit->point);
        *vec4(hit->plane) = *vec4(plane);
        if (dm2::pointInLevel(vec4(hit->point)) == 0) // the level's playable space (0: outside)
            return 0;
        if ((collision::hitFlags(found) & collision::kHitFace) && face != 0)
        {
            hit->face = static_cast<int64_t>(face);
            return 1;
        }
        if (collision::hitFlags(found) & collision::kHitObject)
        {
            hit->flags |= kGroundObject;
            hit->object = collision::hitObject(found);
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION(groundBelowSpawn, Verified);

    // The face to start on near the point (the Player locator's ground):
    // the spiral of tries (spawnSpiral, the ground under each by
    // groundBelow); else the ground under the nearest Player locator, its
    // Spawn locator, or under the level's origin. 0 if none.
    // GameCube 0x80077D8C.
    FaceRef spawnAtPlayer(const ee::Float *point, uint32_t flags)
    {
        ee::Local<GroundHit> hit;
        ee::Local<ee::Float[4]> probe;
        ee::Local<zge::Matrix> locator;
        FaceRef face = 0;
        if ((flags & 1u) == 0)
            face = spawnSpiral(point, flags, *g_ringGrowth, hit, probe, false);
        if (face == 0 && locatorMatrix(point, hashOf(kPlayerName), locator) != 0)
        {
            *vec4(probe) = locator->translation();
            if (groundBelow(probe, hit) != 0)
                face = static_cast<FaceRef>(hit->face);
        }
        if (face == 0)
            face = playerSpawnPoint(point);
        if (face == 0)
        {
            setOrigin(probe);
            probe[1] = *g_dropHeight;
            if (groundBelow(probe, hit) != 0)
                face = static_cast<FaceRef>(hit->face);
        }
        return face;
    }
    READABLE_FUNCTION(spawnAtPlayer, Verified);

    // The face the rider starts on: at the level's locators in level 8,
    // else near the Player locator. GameCube 0x80078134.
    FaceRef respawnAtStart(const ee::Float *point, uint32_t flags)
    {
        if (*g_sessionLevel == kLevelStartsAtLocators)
            return spawnAtLocator(point, flags);
        return spawnAtPlayer(point, flags);
    }
    READABLE_FUNCTION(respawnAtStart, Verified);

    namespace
    {
        constexpr uint32_t kPlayerOneName = sym::dm2PlayerOneName; // "PlayerOne"
        // spawnAtLocator's search: the ring's growth (1.1), nearest so far
        // to start (FLT_MAX), the height its last try drops from (-10000).
        inline ee::Global<ee::Float, sym::g_locatorRingGrowth> g_locatorRingGrowth;
        inline ee::Global<ee::Float, sym::g_locatorSpawnFar> g_locatorSpawnFar;
        inline ee::Global<ee::Float, sym::g_locatorDropHeight> g_locatorDropHeight;

        // The ground under the nearest of the level's Spawn locators named
        // `name` that is nearer than `nearest` (any, while there is no
        // face): face and nearest updated; `objects` lets an object's
        // ground count (it gives no face then).
        void nearestLocatorGround(const ee::Float *point, uint32_t name, GroundHit *hit, ee::Float *apart, zge::LocatorList *list,
                                  FaceRef &face, ee::Float &nearest, bool objects)
        {
            zge::findLocators(list, hashOf(kSpawnName), name);
            for (ee::Ptr<zge::Locator> entry = list->head; entry; entry = entry->next)
            {
                const ee::Float *position = locatorPoint(*entry).position;
                *vec4(apart) = *vec4(point) - *vec4(position);
                const ee::Float d = collision::engine::lengthSquared(apart);
                if (face != 0 && !(d < nearest))
                    continue;
                if (groundBelowSpawn(position, hit) == 0)
                    continue;
                if (!objects && (hit->flags & kGroundObject) != 0)
                    continue;
                face = static_cast<FaceRef>(hit->face);
                nearest = d;
            }
            zge::freeLocators(list);
        }
    }

    // The face to start on in level 8 (a park of the Park Editor: its
    // Test Run, editTestLevel, and Free Ride's Play Park, bmxmenuPlayPark,
    // set it): unless `flags` bit 0, the spiral of tries (spawnSpiral, the
    // ground under each by groundBelowSpawn); then the ground under a Spawn
    // locator named Player or PlayerOne nearer than that face (objects'
    // ground not counted); with none, under the nearest PlayerOne locator
    // (objects counted, twice over as the original searches it twice); last
    // under the level's origin from high up. 0 if none. GameCube 0x80077638.
    FaceRef spawnAtLocator(const ee::Float *point, uint32_t flags)
    {
        ee::Local<GroundHit> hit;
        ee::Local<zge::LocatorList> list;
        ee::Local<ee::Float[4]> probe;
        // Each locator search's difference to the point, a slot each.
        ee::Local<ee::Float[4]> playerApart;
        ee::Local<ee::Float[4]> playerOneApart;
        ee::Local<ee::Float[4]> anyApart;
        ee::Local<ee::Float[4]> againApart;
        FaceRef face = 0;

        const uint32_t player = hashOf(kPlayerName);
        if ((flags & 1u) == 0)
            face = spawnSpiral(point, flags, *g_locatorRingGrowth, hit, probe, true);

        // How far the spiral's face is (its centre less 1.25).
        ee::Float nearest;
        if (face == 0)
            nearest = *g_locatorSpawnFar;
        else
        {
            *vec4(probe) = *vec4(point) - *vec4(probe);
            nearest = zge::dot(*vec4(probe), *vec4(probe));
        }
        nearestLocatorGround(point, player, hit, playerApart, list, face, nearest, false);
        const uint32_t playerOne = hashOf(kPlayerOneName);
        nearestLocatorGround(point, playerOne, hit, playerOneApart, list, face, nearest, false);
        if (face == 0)
        {
            ee::Float far = *g_locatorSpawnFar;
            nearestLocatorGround(point, playerOne, hit, anyApart, list, face, far, true);
            nearestLocatorGround(point, hashOf(kPlayerOneName), hit, againApart, list, face, far, true);
        }
        if (face == 0)
        {
            setOrigin(probe);
            probe[1] = *g_locatorDropHeight;
            if (groundBelowSpawn(probe, hit) != 0)
                face = static_cast<FaceRef>(hit->face);
        }
        return face;
    }
    READABLE_FUNCTION(spawnAtLocator, Verified);

    namespace
    {
        // Forward kept unless it is this near the face's normal (|cos|).
        inline ee::Global<ee::Float, sym::g_forwardAlongNormal> g_forwardAlongNormal; // 0.99
    }

    // The physics reset: the touch, offsets, velocity, force, speed, the
    // air's angles, the ride distance and the wheels' poses cleared; the
    // rider on the face respawnAtStart finds near the position (its centre,
    // the face the contact), up the face's normal, forward kept (else the
    // old up), the rows orthonormal; the drawn body and the position lifted
    // by the wheels' heights; physics state 0 (written, not entered).
    // GameCube 0x800771E8.
    void resetPhysics(Rider *rider)
    {
        ee::Local<ee::Float[4]> normal;
        ee::Local<ee::Float[4]> was; // the position before (the original keeps a copy and never reads it)
        ee::Local<FaceRef> face;

        rider->touchedObject = 0;
        setOrigin(rider->rootOffset);
        setOrigin(rider->motionRoot);
        setOrigin(rider->drawnOffset);
        setOrigin(rider->velocity);
        setOrigin(rider->force);
        rider->speed = 0.0f;
        rider->pitch = 0;
        rider->yawRate = 0;
        rider->roll = 0;
        rider->bodyPitch = 0;
        rider->rideDistance = 0;
        rider->pedalForce = 0.0f;
        rider->wheelDrag = 0;
        for (WheelPose &wheel : rider->wheelPoses)
        {
            setOrigin(wheel.velocity);
            wheel.speed = 0.0f;
            wheel.ground = kWheelRolling;
            wheel.angle = 0;
            wheel.spin = 0;
        }

        *vec4(was) = *vec4(rider->position);
        *face = respawnAtStart(rider->position, 0u);
        setContact(rider, face, 0u);
        collision::faceCentre(*face, rider->position);
        collision::faceNormal(*face, &normal[0]);
        zge::Vec4 &right = *vec4(rider->right);
        zge::Vec4 &up = *vec4(rider->up);
        zge::Vec4 &forward = *vec4(rider->forward);
        if (*g_forwardAlongNormal < ee::abs(zge::dot(forward, *vec4(normal))))
            forward = up;
        up = *vec4(normal);
        right = zge::cross(up, forward);
        right = zge::unit(right);
        forward = zge::cross(right, up);
        setOrigin(rider->rowsOrigin);
        zge::setObjectRotationMatrix(ee::Ptr<zge::SceneObject>(rider->messenger), matrix(rider->right));

        for (int i = 0; i < 3; ++i)
            rider->drawnPosition[i] =
                rider->position[i] + rider->up[i] * ((rider->frontWheelHeight + rider->backWheelHeight) * ee::Float(0.5f));
        *vec4(rider->position) = *vec4(rider->drawnPosition);
        engine::copyMatrix(rider->body, rider->right);
        rider->physicsState = kPhysicsGround;
    }
    READABLE_FUNCTION(resetPhysics, Verified);

    // A rider's reset (rider index, shown): shown, the motion flags 2, the
    // buttons, sticks and taps cleared, the action states riding (level 2:
    // 4), no physics state then the physics reset, the bail's body put at
    // the rider (unless kFlagNoPlayer), shown; hidden, not shown. Both
    // models shown or hidden. GameCube 0x8008A368.
    void riderReset(uint32_t index, uint32_t shown)
    {
        Rider *rider = riderOf(index & 0xFFu).get();
        if (shown != 0)
        {
            rider->motionFlags = 2;
            rider->flags &= ~kFlagBailHands;
            rider->input.held = 0;
            rider->input.changed = 0;
            rider->hands[0].holding = 0;
            rider->hands[1].holding = 0;
            for (int i = 0; i < 2; ++i)
            {
                rider->input.previous[i].x = 0.0f;
                rider->input.previous[i].y = 0.0f;
                rider->input.sticks[i].x = 0.0f;
                rider->input.sticks[i].y = 0.0f;
            }
            for (uint8_t &tap : rider->taps)
                tap = kDirNone;
            rider->tapCount = 0;
            rider->flags |= kFlagMotionEnded;
            setActionState(rider, 0u, uint32_t(kActionRiding));
            setActionState(rider, 1u, 0u);
            setActionState(rider, 2u, 4u);
            rider->soundWait = 0;
            rider->physicsState = kPhysicsNone;
            resetPhysics(rider);
            if ((rider->flags & kFlagNoPlayer) == 0)
            {
                // The bail's tumbling body at the rider, still.
                const ee::Ptr<zge::RigidBody> body(rider->bailBody);
                engine::copyMatrix(&body->rotation, rider->right);
                body->position = *vec4(rider->position);
                zge::sweepUpdateBody(levelSweep(), body);
                body->velocity = *vec4(rider->velocity);
                body->spin = zge::kZero;
            }
            rider->flags |= kFlagShown;
        }
        else
        {
            rider->flags &= ~kFlagShown;
        }
        zge::setSceneObjectShown(ee::Ptr<zge::SceneObject>(rider->messenger), shown);
        // The bike's model last, as the original's tail call.
        game::tail(zge::setSceneObjectShown, ee::Ptr<zge::SceneObject>(rider->bikeModel), shown);
    }
    READABLE_FUNCTION(riderReset, Verified);
}
