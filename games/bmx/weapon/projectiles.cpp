// BMX XXX's weapons (weapon.h): the grenades and rockets in flight
// (./src/com/action/weapon.c).

#include "weapon.h"

#include "bmx/rider/physics/physics.h"
#include "bmx/action/camera_state.h"
#include "bmx/fx/emitter_objects.h"
#include "bmx/fx/spawns.h"
#include "bmx/sound/events.h"
#include "bmx/ui/ui_nodes.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/math/random.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/weapon/projectiles.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        inline ee::Global<Projectile[kProjectiles], sym::g_projectiles> g_projectiles;
        inline ee::Global<uint32_t, sym::g_projectileCount> g_projectileCount;
        inline ee::Global<uint32_t, sym::g_nextProjectileSlot> g_nextProjectileSlot;
        inline ee::Global<ee::Ptr<Scene>[kProjectileKinds], sym::g_projectileModels> g_projectileModels;
        inline ee::Global<WeaponType[kWeaponTypes], sym::g_weaponTypes> g_weaponTypes;

        // The sound events a projectile sends: as it is fired (from the
        // shooter, with its kind), each frame it flies and as it is freed
        // (both from where it is, with what the first answered).
        constexpr uint32_t kSoundProjectileFired = 0x2F;
        constexpr uint32_t kSoundProjectileFreed = 0x30;
        constexpr uint32_t kSoundProjectileFlies = 0x31;

        // What a falling projectile's downward speed gains each frame.
        constexpr ee::Float kProjectileFall = std::bit_cast<float>(0x3BDA740Fu); // 1 / 150
        // The part of a projectile's velocity its trail starts with.
        constexpr ee::Float kTrailSpeed = std::bit_cast<float>(0x3F266666u); // 0.65

        constexpr ee::Float kFramesASecond = ee::Float(60.0f);
        constexpr ee::Float kAnglesATurn = ee::Float(65536.0f);
        // A grenade's spin about each axis: up to this part of a turn a
        // frame for each of the 128 steps either way the random number gives.
        constexpr ee::Float kGrenadeSpin = std::bit_cast<float>(0x394CCCCDu); // 0.0001953125
        // A rocket's roll, a frame.
        constexpr Angle kRocketRoll = 0xCCC;
        // The recoil's throw for each step of its random number: upwards
        // (128 to 383 steps) and to either side (128 steps each way).
        constexpr ee::Float kRecoilUp = std::bit_cast<float>(0x37B679A7u);   // 2.17527e-05
        constexpr ee::Float kRecoilSide = std::bit_cast<float>(0x37B76EDEu); // 2.18669e-05

        // A random step count from -128 to 127.
        int32_t randomStep() { return static_cast<int32_t>(randomNumber() & 0xFF) - 0x80; }
    }

    // updateProjectiles: every projectile in flight moved a frame: turned by
    // its spin, a falling one's fall added, then its way through the frame
    // tested: one that meets something ends there; one that does not moves
    // on, sounds, moves its model and (a trailing one, one frame in four at
    // random) leaves smoke. One whose life is over expires instead.
    uint32_t updateProjectiles()
    {
        ee::Local<Matrix> turn;
        ee::Local<Vec4> position;
        ee::Local<ProjectilePath> path;
        ee::Local<Vec4> trailStart;
        ee::Local<Vec4> trailVelocity;

        uint32_t left = *g_projectileCount;
        for (uint32_t slot = 0; left != 0 && slot < kProjectiles; ++slot)
        {
            Projectile *projectile = &(*g_projectiles)[slot];
            if ((projectile->flags & kProjectileInUse) == 0)
                continue;
            --left;
            if (projectile->life == 0)
            {
                expireProjectile(projectile);
                continue;
            }
            --projectile->life;

            // Turned about itself: the turn is made with it at the origin.
            *position = projectile->matrix.translation();
            projectile->matrix.translation() = kZero;
            matrixIdentity(turn);
            matrixRotateXYZ(turn, turn, &projectile->spin);
            matrixMultiply(&projectile->matrix, &projectile->matrix, turn);
            projectile->matrix.translation() = *position;
            if (projectile->flags & kProjectileFalls)
                projectile->velocity.y = projectile->velocity.y + kProjectileFall;

            path->projectile = projectile;
            path->start = *position;
            const ee::Float speed = vec3Length(&projectile->velocity.xyz());
            path->length = speed;
            path->direction = scaledCarrying(projectile->velocity, ee::Float(1.0f) / speed, rider::physics::upperHalf(*position));
            if (findProjectileHit(path) != 0)
            {
                projectileHit(projectile, path);
                continue;
            }

            projectile->matrix.translation() += projectile->velocity;
            soundEvent(kSoundProjectileFlies, &projectile->matrix.translation(), projectile->sound, 0, 0);
            if (projectile->node == 0)
                continue;
            setNodeLocalMatrix(projectile->node, &projectile->matrix);
            if ((projectile->flags & kProjectileTrail) == 0)
                continue;
            if ((randomNumber() & 3) != 0)
                continue;
            *trailStart = projectile->matrix.translation();
            *trailVelocity = scaledCarrying(projectile->velocity, kTrailSpeed, rider::physics::upperHalf(*trailStart));
            // (the original hands the velocity on too, a second word spawnConcreteDust does not take)
            spawnConcreteDust(trailStart);
        }
        return 1;
    }
    READABLE_FUNCTION(updateProjectiles, Verified);

    // fireShot: the shot of the weapon in hand.
    //
    // A bullet is not put in flight: it is a projectile on the stack, at the
    // shooter and going the aim's way, that the owner is told of, and what
    // it meets within the weapon's range is hit at once. A weapon with
    // tracers shows one for every fourth round and for the last five.
    //
    // A grenade (falling, spinning at random) or a rocket (rolling, with a
    // trail) is put in flight at the shooter with the weapon's speed and
    // life; if the owner refuses it, it is freed again.
    //
    // Then the muzzle flash plays over the view (not while the game's camera
    // is away) and the recoil throws the aim up and to a side at random.
    void fireShot(Shooter *shooter)
    {
        ee::Local<Projectile> bullet;
        ee::Local<ProjectileMessage> message;
        ee::Local<ProjectilePath> path;
        ee::Local<Vec4> tracerStart;
        ee::Local<Vec4> tracerVelocity;

        const uint32_t kind = (*g_weaponTypes)[shooter->weapon].projectile;
        if (kind == kProjectileBullet)
        {
            memoryFill(bullet, 0, sizeof(Projectile));
            bullet->kind = kProjectileBullet;
            bullet->shooter = shooter;
            bullet->matrix = shooter->aim;
            bullet->matrix.translation() = shooter->position;
            bullet->velocity = scaledCarrying(shooter->aim.row(2), (*g_weaponTypes)[shooter->weapon].force, rider::physics::upperHalf(shooter->position));
            message->event = kProjectileFired;
            const ProjectileHandler owner = bullet->shooter->owner;
            if (!owner || owner(bullet, message) == 1)
            {
                path->projectile = bullet;
                path->start = shooter->position;
                path->direction = shooter->aim.row(2);
                const WeaponType &type = (*g_weaponTypes)[shooter->weapon];
                path->length = type.reach;
                if (type.flags & kWeaponTracers)
                {
                    const uint32_t loaded = shooter->ammunition[shooter->weapon].loaded;
                    if ((loaded & 3) == 0 || loaded < 6)
                    {
                        const uint64_t carried = rider::physics::upperHalf(shooter->aim.row(2));
                        *tracerStart = scaledCarrying(bullet->velocity, ee::Float(2.0f), carried);
                        *tracerStart = *tracerStart + path->start;
                        *tracerVelocity = scaledCarrying(bullet->velocity, ee::Float(0.75f), carried);
                        spawnTracer(tracerStart, tracerVelocity);
                    }
                }
                if (findProjectileHit(path) != 0)
                    projectileHit(bullet, path);
            }
        }
        else
        {
            Projectile *projectile = newProjectile(shooter, kind);
            if (projectile != nullptr)
            {
                if (kind == kProjectileRocket)
                {
                    projectile->spin.z = kRocketRoll;
                    projectile->flags |= kProjectileTrail;
                }
                else
                {
                    projectile->flags |= kProjectileFalls;
                    projectile->spin.x = static_cast<Angle>(ee::toInt(ee::toFloat(randomStep()) * kGrenadeSpin * kAnglesATurn));
                    projectile->spin.y = static_cast<Angle>(ee::toInt(ee::toFloat(randomStep()) * kGrenadeSpin * kAnglesATurn));
                    projectile->spin.z = static_cast<Angle>(ee::toInt(ee::toFloat(randomStep()) * kGrenadeSpin * kAnglesATurn));
                }
                projectile->matrix = shooter->aim;
                projectile->matrix.translation() = shooter->position;
                projectile->velocity = scaledCarrying(shooter->aim.row(2), (*g_weaponTypes)[shooter->weapon].reach, rider::physics::upperHalf(shooter->position));
                projectile->life = static_cast<uint16_t>(ee::toInt((*g_weaponTypes)[shooter->weapon].force * kFramesASecond));
                message->event = kProjectileFired;
                if (shooter->owner && shooter->owner(projectile, message) != 1)
                    freeProjectile(projectile);
            }
        }

        if (((*g_weaponTypes)[shooter->weapon].flags & kWeaponMuzzleFlash) != 0 && cameraState() == 0)
            playUiNode(zge::zgui::findNode(shooter->ui, sym::g_weaponMuzzleFlashName, 1), sym::g_weaponFlashAnimation); // "MuzzleFlash", "Flash"

        const ee::Float up = ee::toFloat(static_cast<int32_t>(randomNumber() & 0xFF) + 128) * kRecoilUp;
        shooter->recoilPitch = static_cast<Angle>(shooter->recoilPitch - ee::toInt((*g_weaponTypes)[shooter->weapon].recoil * up * kAnglesATurn));
        const ee::Float side = ee::toFloat(randomStep()) * kRecoilSide;
        shooter->recoilYaw = static_cast<Angle>(shooter->recoilYaw + ee::toInt((*g_weaponTypes)[shooter->weapon].recoil * side * kAnglesATurn));
    }
    READABLE_FUNCTION(fireShot, Verified);

    // newProjectile: a projectile of a kind for a shooter, in the first free
    // slot from the one after the last given (it waits for one for ever:
    // sixteen are never in flight at once), with its sound started at the
    // shooter and a shown copy of the kind's model, when there is one.
    Projectile *newProjectile(Shooter *shooter, uint32_t kind)
    {
        uint32_t slot = *g_nextProjectileSlot;
        while ((*g_projectiles)[slot].flags & kProjectileInUse)
        {
            ++slot;
            if (slot == kProjectiles)
                slot = 0;
        }
        Projectile *projectile = &(*g_projectiles)[slot];
        memoryFill(projectile, 0, sizeof(Projectile));
        projectile->shooter = shooter;
        projectile->kind = static_cast<uint8_t>(kind);
        projectile->flags |= kProjectileInUse;
        projectile->sound = soundEvent(kSoundProjectileFired, &shooter->position, kind, 0, 0);
        projectile->node = ee::Ptr<Node>();
        const ee::Ptr<Scene> model = (*g_projectileModels)[kind];
        if (model != 0)
        {
            const ee::Ptr<SceneBone> root = rootBone(model);
            const ee::Ptr<Node> copy = instanceGeometryNode(boneNode((*g_projectileModels)[kind], root), 0);
            projectile->node = copy;
            setNodeShown(copy, 1, 1);
        }
        *g_nextProjectileSlot = slot > kProjectiles - 2 ? 0 : slot + 1;
        *g_projectileCount = *g_projectileCount + 1;
        return projectile;
    }
    READABLE_FUNCTION(newProjectile, Verified);

    // freeProjectile: its model's copy let go, its sound ended, and the slot
    // free.
    void freeProjectile(Projectile *projectile)
    {
        if (projectile->node != 0)
            releaseNode(projectile->node, 0);
        soundEvent(kSoundProjectileFreed, &projectile->matrix.translation(), projectile->sound, 0, 0);
        projectile->flags &= static_cast<uint8_t>(~kProjectileInUse);
        *g_projectileCount = *g_projectileCount - 1;
    }
    READABLE_FUNCTION(freeProjectile, Verified);

    // expireProjectile: a projectile whose life is over: its shooter's owner
    // is told, and unless the owner says otherwise a grenade or a rocket
    // blasts where it is. Then it is freed.
    void expireProjectile(Projectile *projectile)
    {
        ee::Local<ProjectileMessage> message;
        message->event = kProjectileExpired;
        const ProjectileHandler owner = projectile->shooter->owner;
        if (!owner || owner(projectile, message) == 1)
        {
            if (projectile->kind == kProjectileGrenade || projectile->kind == kProjectileRocket)
                explodeProjectile(projectile);
        }
        freeProjectile(projectile);
    }
    READABLE_FUNCTION(expireProjectile, Verified);
}
