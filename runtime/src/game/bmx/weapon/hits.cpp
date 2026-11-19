// BMX XXX's weapons (weapon.h): what a projectile meets on its way, and what
// happens when it does (./src/com/action/weapon.c).

#include "weapon.h"

#include "bmx/fx/emitter_objects.h"
#include "bmx/fx/spawn_effects.h"
#include "bmx/rider/collision/collision.h"
#include "bmx/rider/physics/physics.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/geo.h"
#include "zge/math/plane.h"
#include "zge/math/random.h"
#include "zge/math/scale_leftover.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/math/conversions.h"
#include "zge/math/angle.h"
#include "zge/render/part.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/weapon/hits.cpp.h"

namespace game::bmx
{
    using namespace zge;
    using rider::Rider;
    using rider::collision::NodeFace;
    using rider::collision::NodeFaceCorner;
    using rider::collision::RayHit;

    namespace
    {
        inline ee::Global<WeaponType[kWeaponTypes], sym::g_weaponTypes> g_weaponTypes;
        // The box about a rider a projectile hits: half its width, height
        // and length, and its middle, both along the rider's own axes.
        inline ee::Global<ee::Float[3], sym::g_riderTargetHalfSize> g_riderTargetHalfSize;
        inline ee::Global<Vec4, sym::g_riderTargetMiddle> g_riderTargetMiddle;

        // The level is tested this far along the path at a time.
        constexpr ee::Float kPathStep = ee::Float(10.0f);
        // How far under its path a falling projectile's hit test reaches,
        // and how far a point it hit is raised again.
        constexpr ee::Float kFallReach = std::bit_cast<float>(0x3D99999Au); // 0.075

        // The sound a blast makes, with the projectile's kind.
        constexpr uint32_t kSoundProjectileBlast = 0x33;


        // What the object and rider visits of the hit test are handed.
        struct ObjectVisit
        {
            ee::Ptr<ProjectilePath> path;
            ee::Ptr<SceneObject> found;  // the nearest object met so far
            ee::Float distance;          // nothing farther along the path counts
            uint32_t faceFound;          // the path met the level (path->hit)
        };
        struct RiderVisit
        {
            ee::Ptr<ProjectilePath> path;
            uint8_t found;               // the nearest rider met so far (kNoRider: none)
            ee::Float distance;
            uint32_t faceFound;
        };
        static_assert(offsetof(ObjectVisit, faceFound) == 0xC && offsetof(RiderVisit, distance) == 8);
        constexpr uint8_t kNoRider = 0xFF;

        // A rocket's blast against the node it hit: the node's faces near
        // the point are split into pieces and the pieces thrown off.
        struct FaceBlast
        {
            Vec4 point;                  // +0x00
            int32_t leastSplits;         // +0x10 a face is split at least this often,
            int32_t mostSplits;          // +0x14 at most this often,
            ee::Float radiusSquared;     // +0x18 only faces whose middle is this near are touched
            ee::Float pieceSize;         // +0x1C and between the two until its pieces are under this size
            ee::Float shardRadiusSquared; // +0x20 a piece this near is thrown off
            ee::Float shardSpeed;        // +0x24 along the face's normal
        };
        static_assert(offsetof(FaceBlast, radiusSquared) == 0x18 && offsetof(FaceBlast, shardSpeed) == 0x24);
        // The pieces thrown by the blast so far: 256 end it.
        inline ee::Global<uint32_t, sym::g_blastShardCount> g_blastShardCount;
        constexpr uint32_t kMostBlastShards = 0x100;
        // The largest float: what the shortest side found starts from.
        inline ee::Global<ee::Float, sym::g_largestFloat> g_largestFloat;
        // How many times over the face being shattered has been split.
        inline ee::Global<int32_t, sym::g_shatterDepth> g_shatterDepth;
        // A split face's six points: its three corners, then the middles
        // of its sides (from each corner to the next). These are the four
        // pieces' corners among them: one at each corner, one in the middle.
        constexpr uint32_t kSplitPoints = 6;
        constexpr uint32_t kSplitPieces = 4;
        inline ee::Global<uint32_t[kSplitPieces][3], sym::g_splitPieceCorners> g_splitPieceCorners;
        struct TextureCoordinates
        {
            ee::Float u, v;
        };

        constexpr int32_t kFaceShardLifetime = 750;
        // A shard's way is turned by up to 0.05 of a turn about each axis, and sinks.
        constexpr ee::Float kShardTurnAStep = std::bit_cast<float>(0x39CCCCCDu); // 0.000390625 of a turn
        // 0.05 as the game keeps it, a step above 0.05f (y grows downwards: taken off).
        constexpr ee::Float kShardRise = std::bit_cast<float>(0x3D4CCCCEu);

        // The sound a grenade makes as it bounces, with its kind.
        constexpr uint32_t kSoundProjectileBounce = 0x32;
        // A grenade going into a face faster than this, a frame, sounds and
        // tumbles anew.
        constexpr ee::Float kLeastBounce = std::bit_cast<float>(0x3C888889u); // 1 / 60
        // The bounce's new spin about each axis: up to this part of a turn
        // a frame for each of the 128 steps either way the random number gives.
        constexpr ee::Float kBounceSpin = std::bit_cast<float>(0x39CCCCCDu); // 0.000390625
        constexpr ee::Float kAnglesATurn = ee::Float(65536.0f);
        // The part of the speed into the face a grenade comes off it with.
        constexpr ee::Float kBounceBack = std::bit_cast<float>(0x3EE66666u); // 0.45
        // The pock mark texture of a bullet's mark, and its colour unless the
        // owner gives another.
        constexpr uint32_t kBulletMarkTexture = 2;
        constexpr VertexColour kBulletMarkColour{0xFF, 0xC8, 0x08, 0xFF};

        // A random step count from -128 to 127.
        int32_t randomStep() { return static_cast<int32_t>(randomNumber() & 0xFF) - 0x80; }

        // The rider's axes (right, up, forward and a fourth row) as a matrix.
        const Matrix &riderAxes(const Rider *rider) { return *rider::matrix(rider->right); }
        const Vec4 &vec4(const ee::Float *v) { return *rider::vec4(v); }

        // Whether what the visit looks at, `along` the path, lies behind the
        // point where the path met the level.
        bool behindFaceHit(const ProjectilePath *path, uint32_t faceFound, ee::Float along, Vec4 *toFace)
        {
            if (faceFound == 0)
                return false;
            rider::collision::hitPoint(path->hit, &toFace->x);
            *toFace = *toFace - path->start;
            return !(along <= dot(*toFace, path->direction));
        }
    }

    // findProjectileHit: what the projectile meets on its path: 1 when
    // anything, with path->hits saying what.
    //
    // The level first: rays of ten units at a time along the path (a falling
    // projectile's reach a little under it) until one hits a face. Then
    // every object and every rider is looked at, with the distance to that
    // face (or the path's length) as the farthest that counts. A rider met
    // wins over an object, and an object over the level; a face that is an
    // object's counts as the object.
    uint32_t findProjectileHit(ProjectilePath *path)
    {
        ee::Local<Vec4> facePoint;
        ee::Local<Vec4> stepStart;
        ee::Local<Vec4> rayEnd;
        ee::Local<ObjectVisit> objects;
        ee::Local<RiderVisit> riders;
        Vec4 stepEnd;
        Vec4 reach;

        ee::Float left = path->length;
        path->rider = kNoRider;
        path->hits = 0;
        path->object = 0;
        *stepStart = path->start;
        uint32_t faceFound = 0;
        while (ee::Float(0.0f) < left)
        {
            stepEnd = scaledCarrying(path->direction, ee::min(left, kPathStep), rider::physics::upperHalf(*stepStart));
            stepEnd = stepEnd + *stepStart;
            *rayEnd = stepEnd;
            if (path->projectile->flags & kProjectileFalls)
                rayEnd->y = rayEnd->y + kFallReach;
            path->hit = rider::collision::rayCast(&stepStart->x, &rayEnd->x);
            if (path->hit != 0)
            {
                rider::collision::hitPoint(path->hit, &facePoint->x);
                reach = *facePoint - path->start;
                faceFound = 1;
                break;
            }
            *stepStart = stepEnd;
            left = left - kPathStep;
        }
        if (faceFound == 0)
            reach = scaledCarrying(path->direction, path->length, rider::physics::upperHalf(*stepStart));
        if (path->projectile->flags & kProjectileFalls)
            reach.y = reach.y - kFallReach;
        const ee::Float distance = dot(path->direction, reach);

        objects->path = path;
        objects->found = ee::Ptr<SceneObject>();
        objects->distance = distance;
        objects->faceFound = faceFound;
        forEachObject(sym::projectileMeetsObject, objects.address());
        riders->path = path;
        riders->found = kNoRider;
        riders->distance = distance;
        riders->faceFound = faceFound;
        rider::forEachRider(rider::RiderVisit(sym::projectileMeetsRider), riders);

        if (riders->found != kNoRider)
        {
            path->rider = riders->found;
            path->hit = ee::Ptr<RayHit>();
            path->hits |= kProjectileHitRider;
            return 1;
        }
        if (objects->found != 0)
        {
            path->object = ee::Address(objects->found);
            path->hit = ee::Ptr<RayHit>();
            path->hits |= kProjectileHitObject;
            return 1;
        }
        if (faceFound == 0)
            return 0;
        if (rider::collision::hitFlags(path->hit) & rider::collision::kHitObject)
        {
            path->hits |= kProjectileHitObject;
            path->object = rider::collision::hitObject(path->hit);
            path->hit = ee::Ptr<RayHit>();
        }
        else
            path->hits |= kProjectileHitFace;
        return 1;
    }
    READABLE_FUNCTION(findProjectileHit, Verified);

    // projectileMeetsObject: the hit test's look at one object: it is met
    // when it lies ahead on the path, nearer than what the visit has so far
    // and not behind the level's face, and the path's point nearest to it,
    // taken through its node's matrix, is inside its model's bounds. Then it
    // is the visit's object and its distance the new limit.
    uint32_t projectileMeetsObject(ee::Ptr<SceneObject> object, ObjectVisit *visit)
    {
        ee::Local<Vec4> toFace;
        const ProjectilePath *path = visit->path;
        Vec4 toObject = *objectPosition(object) - path->start;
        const ee::Float along = dot(toObject, path->direction);
        if (!(ee::Float(0.0f) <= along))
            return 1;
        if (!(squaredLength(toObject) < visit->distance * visit->distance))
            return 1;
        Vec4 nearest = scaledAfterMeasure(path->direction, along);
        nearest = nearest + path->start;
        if (behindFaceHit(path, visit->faceFound, along, toFace))
            return 1;
        const Vec4 fromObject = nearest - *objectPosition(object);
        const ee::Ptr<Node> node = objectLevelNode(object);
        if (node == 0)
            return 1;
        const ee::Ptr<GeoObject> model = node->model;
        const Matrix matrix = *nodeMatrix(node, 1);
        const Vec4 inModel = transformed(matrix, fromObject);
        if (model->boundsMin.x < inModel.x && inModel.x < model->boundsMax.x && model->boundsMin.y < inModel.y &&
            inModel.y < model->boundsMax.y && model->boundsMin.z < inModel.z && inModel.z < model->boundsMax.z)
        {
            visit->distance = along;
            visit->found = object;
        }
        return 1;
    }
    READABLE_FUNCTION(projectileMeetsObject, Verified);

    // projectileMeetsRider: the hit test's look at one rider (one that can
    // be met): the rider's box is met when its middle lies ahead on the
    // path, nearer than what the visit has so far and not behind the
    // level's face, and the path's point nearest to the middle is inside
    // the box. The shooter's owner is then asked (kProjectileMeetsRider):
    // unless it refuses, the rider is the visit's and the distance the new
    // limit.
    uint32_t projectileMeetsRider(uint32_t riderIndex, RiderVisit *visit)
    {
        ee::Local<Vec4> toFace;
        ee::Local<ProjectileMessage> message;
        riderIndex &= 0xFF;
        const ProjectilePath *path = visit->path;
        const Rider *rider = rider::riderOf(riderIndex);
        if (!rider::hasFlag(rider, rider::kFlagShown))
            return 1;
        Vec4 middle = transformed(riderAxes(rider), *g_riderTargetMiddle);
        middle = middle + vec4(rider->position);
        middle = middle + vec4(rider->rootOffset);
        Vec4 toRider = middle - path->start;
        const ee::Float along = dot(toRider, path->direction);
        if (!(ee::Float(0.0f) <= along))
            return 1;
        if (!(squaredLength(toRider) < visit->distance * visit->distance))
            return 1;
        Vec4 nearest = scaledAfterMeasure(path->direction, along);
        nearest = nearest + path->start;
        if (behindFaceHit(path, visit->faceFound, along, toFace))
            return 1;
        const Vec4 fromPath = middle - nearest;
        const Vec4 inBox = transformed(riderAxes(rider), fromPath);
        const ee::Float *half = *g_riderTargetHalfSize;
        if (!(ee::abs(inBox.x) < half[0]) || !(ee::abs(inBox.y) < half[1]) || !(ee::abs(inBox.z) < half[2]))
            return 1;
        message->event = kProjectileMeetsRider;
        message->subject = riderIndex;
        const ee::Ptr<Projectile> projectile = path->projectile;
        const ProjectileHandler owner = projectile->shooter->owner;
        if (owner && owner(projectile, message) != 1)
            return 1;
        visit->distance = along;
        visit->found = static_cast<uint8_t>(riderIndex);
        return 1;
    }
    READABLE_FUNCTION(projectileMeetsRider, Verified);

    // explodeProjectile: a grenade's or rocket's blast where it is: every
    // rider within the weapon's blast radius is told to its owner, then the
    // sound, the fireball and the flash.
    void explodeProjectile(Projectile *projectile)
    {
        rider::forEachRider(rider::RiderVisit(sym::blastReachesRider), projectile);
        const ee::Ptr<Vec4> position(&projectile->matrix.translation());
        soundEvent(kSoundProjectileBlast, position, projectile->kind, 0, 0);
        // The smoke: 7 times as fast, twice the size, lasting a hundredth as
        // long (the float word 0x3C23D70A); the fire 5 times the rate.
        spawnSmoke(position, {}, ee::Float(7.0f), ee::Float(1.0f), ee::Float(2.0f), std::bit_cast<float>(0x3C23D70Au));
        game::tail(spawnExplosionFire, position, ee::Float(5.0f), ee::Float(1.0f));
    }
    READABLE_FUNCTION(explodeProjectile, Verified);

    // blastReachesRider: the blast's look at one rider (one that can be
    // met): within the blast radius of the weapon in the shooter's hand, the
    // shooter's owner is told (kProjectileBlastReachesRider) with the
    // rider and the blast's strength there: 1 at the projectile, 0 at the
    // radius, the root of the squared distances' part between.
    uint32_t blastReachesRider(uint32_t riderIndex, Projectile *projectile)
    {
        ee::Local<ProjectileMessage> message;
        riderIndex &= 0xFF;
        const Rider *rider = rider::riderOf(riderIndex);
        if (!rider::hasFlag(rider, rider::kFlagShown))
            return 1;
        const Vec4 at = vec4(rider->position) + vec4(rider->rootOffset);
        const Vec4 away = at - projectile->matrix.translation();
        const Shooter *shooter = projectile->shooter;
        const ee::Float radius = (*g_weaponTypes)[shooter->weapon].blastRadius;
        const ee::Float radiusSquared = radius * radius;
        const ee::Float distanceSquared = squaredLength(away);
        if (distanceSquared < radiusSquared)
        {
            message->event = kProjectileBlastReachesRider;
            message->subject = riderIndex;
            message->strength = ee::sqrt((radiusSquared - distanceSquared) / radiusSquared);
            if (shooter->owner)
                shooter->owner(projectile, message);
        }
        return 1;
    }
    READABLE_FUNCTION(blastReachesRider, Verified);

    // projectileHit: what the hit test found, told to the shooter's owner
    // (a rider, an object, the level's face with the point hit, a falling
    // projectile's raised again), and unless the owner refuses, the
    // projectile's end there.
    void projectileHit(Projectile *projectile, ProjectilePath *path)
    {
        ee::Local<ProjectileMessage> message;
        if (path->hits & kProjectileHitRider)
        {
            message->event = kProjectileHitsRider;
            message->subject = path->rider;
            const ProjectileHandler owner = projectile->shooter->owner;
            if (!owner || owner(projectile, message) == 1)
                projectileHitRider(projectile, path);
        }
        if (path->hits & kProjectileHitObject)
        {
            message->event = kProjectileHitsObject;
            message->subject = path->object;
            const ProjectileHandler owner = projectile->shooter->owner;
            if (!owner || owner(projectile, message) == 1)
                projectileHitObject(projectile, path);
        }
        if (path->hits & kProjectileHitFace)
        {
            message->event = kProjectileHitsFace;
            message->face = rider::collision::hitFace(path->hit);
            rider::collision::hitPoint(path->hit, &message->point.x);
            if (projectile->flags & kProjectileFalls)
                message->point.y = message->point.y - kFallReach;
            const ProjectileHandler owner = projectile->shooter->owner;
            if (!owner || owner(projectile, message) == 1)
                projectileHitFace(projectile, path);
        }
    }
    READABLE_FUNCTION(projectileHit, Verified);

    // projectileHitRider: a projectile's end at a rider: a grenade or a
    // rocket blasts, and anything but a bullet (which was never in flight)
    // is freed. The rider is looked up and not used.
    void projectileHitRider(Projectile *projectile, ProjectilePath *path)
    {
        rider::riderOf(path->rider);
        if (projectile->kind == kProjectileBullet)
            return;
        if (projectile->kind < kProjectileKinds)
            explodeProjectile(projectile);
        game::tail(freeProjectile, projectile);
    }
    READABLE_FUNCTION(projectileHitRider, Draft);

    // projectileHitObject: a projectile's end at an object: as at a rider.
    void projectileHitObject(Projectile *projectile, ProjectilePath *)
    {
        if (projectile->kind == kProjectileBullet)
            return;
        if (projectile->kind < kProjectileKinds)
            explodeProjectile(projectile);
        game::tail(freeProjectile, projectile);
    }
    READABLE_FUNCTION(projectileHitObject, Verified);

    // projectileHitFace: a projectile's end at a face of the level.
    //
    // A bullet leaves a mark on the face (and a puff on three of the
    // surface sounds: 5, 10 and 11, not named): the owner is asked for the
    // mark's colour (kProjectileMarksFace) and the usual one is used unless
    // it answers 1.
    //
    // A grenade bounces: its speed into the face is taken out, what is left
    // halved, and it comes off the face with a part of the speed it went in
    // with, from the point it hit. Going in fast enough it sounds and gets
    // a new spin on top of its own.
    //
    // A rocket blasts, the faces of the node it hit that lie within three
    // units are shattered, and it is freed.
    void projectileHitFace(Projectile *projectile, ProjectilePath *path)
    {
        ee::Local<Vec4> still;
        ee::Local<Vec4> point;
        ee::Local<Vec4> normal;
        ee::Local<Vec4> plane;
        ee::Local<FaceBlast> blast;
        ee::Local<ProjectileMessage> message;
        ee::Local<VertexColour> colour;

        const rider::collision::FaceRef face = rider::collision::hitFace(path->hit);
        rider::collision::hitPoint(path->hit, &point->x);
        if (projectile->flags & kProjectileFalls)
            point->y = point->y - kFallReach;
        rider::collision::faceNormal(face, &normal->x);

        const uint32_t kind = projectile->kind;
        if (kind == kProjectileBullet)
        {
            const uint32_t sound = rider::collision::faceSound(face);
            if (sound == 5 || sound == 10 || sound == 11)
            {
                // (the original zeroes a vector and hands it on too, a second
                // word spawnDust does not take)
                *still = kZero;
                spawnDust(point);
            }
            message->event = kProjectileMarksFace;
            const ProjectileHandler owner = projectile->shooter->owner;
            if (owner && owner(projectile, message) == 1)
                *colour = message->colour;
            else
                *colour = kBulletMarkColour;
            spawnBulletMark(point, &normal->xyz(), kBulletMarkTexture, &colour->red);
        }
        else if (kind == kProjectileGrenade)
        {
            planeThrough(plane, point, normal);
            Vec4 &velocity = projectile->velocity;
            const ee::Float into = -dot(velocity, *plane);
            const bool hard = kLeastBounce < into;
            if (hard)
            {
                soundEvent(kSoundProjectileBounce, point, projectile->kind, 0, 0);
                projectile->spin.x = static_cast<Angle>(projectile->spin.x + ee::toInt(ee::toFloat(randomStep()) * kBounceSpin * kAnglesATurn));
                projectile->spin.y = static_cast<Angle>(projectile->spin.y + ee::toInt(ee::toFloat(randomStep()) * kBounceSpin * kAnglesATurn));
                projectile->spin.z = static_cast<Angle>(projectile->spin.z + ee::toInt(ee::toFloat(randomStep()) * kBounceSpin * kAnglesATurn));
            }
            const ee::Float outX = into * plane->x;
            const ee::Float outY = into * plane->y;
            const ee::Float outZ = into * plane->z;
            velocity.x = velocity.x + outX;
            velocity.y = velocity.y + outY;
            velocity.z = velocity.z + outZ;
            // Scaffolding (t0-leftover): docs/SCAFFOLDING.md
            velocity = hard ? scaledCarrying(velocity, ee::Float(0.5f), scaleLeftover(LeftBy::lastCall)) : scaledAfterMeasure(velocity, ee::Float(0.5f));
            const ee::Float back = into * kBounceBack;
            const ee::Float backZ = back * plane->z;
            const ee::Float backX = back * plane->x;
            const ee::Float backY = back * plane->y;
            velocity.z = velocity.z + backZ;
            velocity.x = velocity.x + backX;
            velocity.y = velocity.y + backY;
            projectile->matrix.translation() = *point;
        }
        else if (kind == kProjectileRocket)
        {
            explodeProjectile(projectile);
            blast->point = *point;
            blast->leastSplits = 1;
            blast->mostSplits = 2;
            blast->radiusSquared = ee::Float(9.0f);
            blast->pieceSize = ee::Float(0.5625f);
            blast->shardRadiusSquared = ee::Float(2.25f);
            blast->shardSpeed = std::bit_cast<float>(0x3E088889u); // 2 / 15
            *g_blastShardCount = 0;
            rider::collision::forEachNodeFace(rider::collision::widen(static_cast<uint32_t>(face)), rider::collision::NodeFaceVisit(sym::shatterFaceNearBlast), blast);
            freeProjectile(projectile);
        }
    }
    READABLE_FUNCTION(projectileHitFace, Verified);

    void shatterFace(NodeFace *face, FaceBlast *blast);
    void throwFaceShard(NodeFace *face, ee::Float speed);
    int32_t compareFaceSize(NodeFace *face, ee::Float size);

    // shatterFaceNearBlast: the blast's look at one face of the node: one
    // whose middle is within the blast's radius is shattered. The walk goes
    // on until 256 pieces have been thrown.
    uint32_t shatterFaceNearBlast(uint64_t, NodeFace *face, FaceBlast *blast)
    {
        constexpr ee::Float kThird = std::bit_cast<float>(0x3EAAAAABu);
        const ee::Ptr<NodeFaceCorner> corners = face->corners;
        Vec4 middle;
        middle.x = (corners[0].position.x + corners[1].position.x + corners[2].position.x) * kThird;
        middle.y = (corners[0].position.y + corners[1].position.y + corners[2].position.y) * kThird;
        middle.z = (corners[0].position.z + corners[1].position.z + corners[2].position.z) * kThird;
        middle.w = ee::Float(0.0f); // (the original's is whatever its stack held: nothing reads it)
        const Vec4 fromBlast = middle - blast->point;
        if (!(squaredLength(fromBlast) < blast->radiusSquared))
            return 1;
        shatterFace(face, blast);
        return *g_blastShardCount < kMostBlastShards ? 1 : 0;
    }
    READABLE_FUNCTION(shatterFaceNearBlast, Verified);

    // compareFaceSize: a three-cornered face's size against a size: -1
    // smaller, 1 larger, 0 the same. The face's size is half the root of
    // its longest side's squared length times its shortest's (a side that
    // sets a new longest is not tried as the shortest).
    int32_t compareFaceSize(NodeFace *face, ee::Float size)
    {
        ee::Float longest = ee::Float(0.0f);
        ee::Float shortest = *g_largestFloat;
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            const ee::Ptr<NodeFaceCorner> corners = face->corners;
            const Vec4 side = corners[(corner + 1) % 3].position - corners[corner].position;
            const ee::Float squared = squaredLength(side);
            if (longest < squared)
                longest = squared;
            else if (squared < shortest)
                shortest = squared;
        }
        const ee::Float faceSize = ee::sqrt(longest * shortest) * ee::Float(0.5f);
        if (faceSize < size)
            return -1;
        if (size < faceSize)
            return 1;
        return 0;
    }
    READABLE_FUNCTION(compareFaceSize, Verified);

    // shatterFace: a face of the blast's node split into four (each side
    // halved) and each piece shattered in turn, until the pieces are small
    // enough: under the blast's least number of splits a face is always
    // split; from there to the most, while it is not under the blast's
    // piece size. A piece that is not split is thrown off as a shard when
    // its middle is within the blast's shard radius, and left alone when
    // not. The new corners' picture places and colours are the halves of
    // the sides' ends'.
    void shatterFace(NodeFace *face, FaceBlast *blast)
    {
        ee::Local<Vec4> middle;
        ee::Local<Vec4[kSplitPoints]> positions;
        ee::Local<TextureCoordinates[kSplitPoints]> pictures;
        ee::Local<VertexColour[kSplitPoints]> colours;
        ee::Local<NodeFace> piece;
        ee::Local<NodeFaceCorner[3]> pieceCorners;
        ee::Local<uint32_t[kSplitPieces][3]> pieceCornerPoints;
        for (uint32_t i = 0; i < kSplitPieces; ++i)
            for (uint32_t k = 0; k < 3; ++k)
                pieceCornerPoints[i][k] = (*g_splitPieceCorners)[i][k];

        bool split = *g_shatterDepth < blast->leastSplits;
        if (!split && !(blast->mostSplits < *g_shatterDepth))
            split = compareFaceSize(face, blast->pieceSize) >= 0;
        if (!split)
        {
            constexpr ee::Float kThird = std::bit_cast<float>(0x3EAAAAABu);
            const ee::Ptr<NodeFaceCorner> corners = face->corners;
            middle->x = (corners[0].position.x + corners[1].position.x + corners[2].position.x) * kThird;
            middle->y = (corners[0].position.y + corners[1].position.y + corners[2].position.y) * kThird;
            middle->z = (corners[0].position.z + corners[1].position.z + corners[2].position.z) * kThird;
            *middle = *middle - blast->point;
            if (squaredLength(*middle) < blast->shardRadiusSquared)
                throwFaceShard(face, blast->shardSpeed);
            return;
        }

        for (uint32_t i = 0; i < 3; ++i)
        {
            const NodeFaceCorner &corner = face->corners[i];
            const NodeFaceCorner &next = face->corners[(i + 1) % 3];
            positions[i] = corner.position;
            pictures[i].u = corner.u;
            pictures[i].v = corner.v;
            colours[i] = corner.colour;
            positions[3 + i] = corner.position + next.position;
            positions[3 + i] = scaledCarrying(positions[3 + i], ee::Float(0.5f), rider::physics::upperHalf(corner.position));
            pictures[3 + i].u = (corner.u + next.u) * ee::Float(0.5f);
            pictures[3 + i].v = (corner.v + next.v) * ee::Float(0.5f);
            colours[3 + i].red = static_cast<uint8_t>((corner.colour.red + next.colour.red) >> 1);
            colours[3 + i].green = static_cast<uint8_t>((corner.colour.green + next.colour.green) >> 1);
            colours[3 + i].blue = static_cast<uint8_t>((corner.colour.blue + next.colour.blue) >> 1);
            colours[3 + i].alpha = static_cast<uint8_t>((corner.colour.alpha + next.colour.alpha) >> 1);
        }
        for (uint32_t i = 0; i < kSplitPieces; ++i)
        {
            piece->material = face->material;
            piece->cornerCount = 3;
            piece->normal = face->normal;
            piece->corners = pieceCorners;
            for (uint32_t k = 0; k < 3; ++k)
            {
                const uint32_t point = pieceCornerPoints[i][k];
                pieceCorners[k].position = positions[point];
                pieceCorners[k].u = pictures[point].u;
                pieceCorners[k].v = pictures[point].v;
                pieceCorners[k].colour = colours[point];
            }
            ++*g_shatterDepth;
            shatterFace(piece, blast);
            --*g_shatterDepth;
        }
    }
    READABLE_FUNCTION(shatterFace, Verified);

    // throwFaceShard: a three-cornered face thrown off as a shard: its
    // material, its first corner's colour, its corners with their places
    // on the picture; along its normal at the speed, the way turned a
    // little at random about each axis and lifted; gone in three quarters
    // of a second. The blast's count of shards goes up.
    void throwFaceShard(NodeFace *face, ee::Float speed)
    {
        ee::Local<ShardTriangle> shard;
        ee::Local<EulerAngles> turn;
        shard->flags = 0;
        shard->material = face->material;
        shard->colour = face->corners[0].colour;
        shard->lifetime = kFaceShardLifetime;
        shard->velocity = scaledCarrying(face->normal, speed, scaleLeftover(LeftBy::caller));
        turn->x = static_cast<Angle>(ee::toInt(ee::toFloat(randomStep()) * kShardTurnAStep * kAnglesATurn));
        turn->y = static_cast<Angle>(ee::toInt(ee::toFloat(randomStep()) * kShardTurnAStep * kAnglesATurn));
        turn->z = static_cast<Angle>(ee::toInt(ee::toFloat(randomStep()) * kShardTurnAStep * kAnglesATurn));
        vec3RotateEuler(&shard->velocity.xyz(), &shard->velocity.xyz(), turn);
        shard->velocity.y = shard->velocity.y - kShardRise;
        for (uint32_t i = 0; i < 3; ++i)
        {
            shard->corners[i].position = face->corners[i].position;
            shard->corners[i].u = face->corners[i].u;
            shard->corners[i].v = face->corners[i].v;
        }
        spawnShardFromTriangle(shard);
        ++*g_blastShardCount;
    }
    READABLE_FUNCTION(throwFaceShard, Verified);
}
