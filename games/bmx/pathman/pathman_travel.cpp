// BMX XXX's path men (pathman.h): where a path man is and how it moves
// along its path. A "PathMan" walks one of the engine's paths at a speed it
// eases towards the speed its animation asks for, and takes one of the
// path's exits at an end; a "FastPathMan" runs round a fast path by time.
// BMX XXX's own code.

#include "pathman.h"
#include "pathman_calls.h"
#include "pathman_tags.h"

#include "bmx/level/entities.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/scene/follower.h"
#include "zge/scene/scene.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/pathman/pathman_travel.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        // The origin: as a point of a bone, the bone's own place.
        inline ee::Global<Vec4, sym::origin> g_origin;

        // A speed this small either way is no speed.
        constexpr ee::Float kNoSpeed = 0.00001f;
        constexpr ee::Float kNoSpeedBackwards = -0.00001f;
        // A path man this near an end of its path (as a share of the path)
        // and still heading for it leaves by one of the end's exits.
        constexpr uint64_t kNearPathEnd = std::bit_cast<uint64_t>(0.999);
        constexpr uint64_t kNearPathStart = std::bit_cast<uint64_t>(0.001);
        constexpr uint64_t kDoubleZero = 0;
        // Where an end of a path names its exits in its tags.
        constexpr uint32_t kPathTagExits = 0x54;
        // Up, for the orientation of a path's follower (y grows downwards).
        constexpr Vec3 kFollowerUp{0.0f, -1.0f, 0.0f};
    }

    // pathManPosition: where the path man's model stands: its root bone's
    // place in the world, written to `out`, which is answered.
    ee::Ptr<Vec4> pathManPosition(ee::Ptr<SceneObject> pathMan, ee::Ptr<Vec4> out)
    {
        const ee::Ptr<SceneBone> root = pathman::modelRootBone(pathMan);
        objectBonePoint(pathMan, root, &*g_origin, out, 0);
        return out;
    }
    READABLE_FUNCTION(pathManPosition, Verified);

    // pathManBodyFacing: the way the model's root bone faces in the world.
    ee::Ptr<Vec4> pathManBodyFacing(ee::Ptr<SceneObject> pathMan, ee::Ptr<Vec4> out)
    {
        ee::Local<Matrix> rootMatrix;
        const ee::Ptr<SceneBone> root = pathman::modelRootBone(pathMan);
        objectBoneMatrix(pathMan, root, rootMatrix, 0);
        *out = matrixRow(*rootMatrix, 2);
        vec3Negate(&out->xyz(), &out->xyz());
        return out;
    }
    READABLE_FUNCTION(pathManBodyFacing, Verified);

    // pathManFacing: the way the path man itself (its object) faces.
    ee::Ptr<Vec4> pathManFacing(ee::Ptr<SceneObject> pathMan, ee::Ptr<Vec4> out)
    {
        ee::Local<Matrix> rotation;
        quaternionToMatrix(rotation, objectOrientation(pathMan));
        *out = matrixRow(*rotation, 2);
        vec3Negate(&out->xyz(), &out->xyz());
        return out;
    }
    READABLE_FUNCTION(pathManFacing, Draft);

    // putPathManOnPath: the path man's follower is put on the path of that
    // key (made, the first time), at the point of the path nearest to where
    // the path man stands.
    void putPathManOnPath(ee::Ptr<SceneObject> pathMan, uint32_t pathKey)
    {
        ee::Local<Vec4> position;
        const ee::Ptr<PathRecord> path = findPathRecord(pathKey);
        const ee::Ptr<PathManTravel> travel = pathManTravel(pathMan);
        if (travel->follower == 0)
            travel->follower = ee::Address(newPathFollower(path, 0, 0.0f, 0.0f));
        else
            setFollowerSegment(ee::Ptr<Follower>(travel->follower), path);
        const ee::Ptr<Vec4> at = pathManPosition(pathMan, position);
        followerSnap(ee::Ptr<Follower>(travel->follower), at);
    }
    READABLE_FUNCTION_WAITS(putPathManOnPath, Verified);

    // putPathManOnFastPath: the same for a fast path, by its id; the
    // follower starts at the time the path man's data gives.
    void putPathManOnFastPath(ee::Ptr<SceneObject> pathMan, uint32_t pathId)
    {
        const ee::Ptr<zge::FastPath> path = zge::findFastPath(pathId);
        const ee::Ptr<PathManTravel> travel = pathManTravel(pathMan);
        if (travel->follower == 0)
            travel->follower = ee::Address(zge::newFastFollower(path, 0, 0.0f, 0.0f));
        else
            zge::setFastFollowerPath(ee::Ptr<zge::FastFollower>(travel->follower), path);
        const ee::Float start = pathManStartTime(pathMan);
        game::tail(zge::setFastFollowerTime, ee::Ptr<zge::FastFollower>(travel->follower), start);
    }
    READABLE_FUNCTION_WAITS(putPathManOnFastPath, Verified);

    // steerPathManSpeed: the speed wanted follows the animation (top speed x
    // its scale), and the follower's speed moves one step towards it, kept
    // between 0 and the top speed, in the direction it already goes (forwards
    // from a standstill).
    void steerPathManSpeed(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManTravel> travel = pathManTravel(pathMan);
        const ee::Ptr<PathManAnimation> animation = pathManAnimation(pathMan);
        if (animation->animation)
            travel->speedWanted = travel->topSpeed * animation->speedScale;
        if (travel->follower == 0)
            return;

        const ee::Float signedSpeed = travel->fast != 0 ? zge::fastFollowerRate(ee::Ptr<zge::FastFollower>(travel->follower))
                                                        : followerSpeed(ee::Ptr<Follower>(travel->follower));
        const ee::Float direction = signedSpeed < kNoSpeedBackwards ? -1.0f : 1.0f;

        ee::Float speed = ee::abs(signedSpeed);
        if (travel->speedWanted < speed)
            speed = speed - pathManSpeedStep(pathMan);
        else if (speed < travel->speedWanted)
            speed = speed + pathManSpeedStep(pathMan);
        speed = ee::max(ee::min(travel->topSpeed, speed), 0.0f);
        speed = speed * direction;
        if (travel->fast != 0)
            game::tail(zge::setFastFollowerRate, ee::Ptr<zge::FastFollower>(travel->follower), speed);
        else
            game::tail(setFollowerSpeed, ee::Ptr<Follower>(travel->follower), speed);
    }
    READABLE_FUNCTION_WAITS(steerPathManSpeed, Verified);

    // walkPathManPath: a "PathMan" moves along its path for that time. At
    // an end of the path it is still heading for, it goes on to one of the
    // end's exits and picks a new motion.
    void walkPathManPath(ee::Ptr<SceneObject> pathMan, ee::Float time)
    {
        if (pathManAnimation(pathMan)->paused == 1)
            return;
        steerPathManSpeed(pathMan);
        const ee::Ptr<PathManTravel> travel = pathManTravel(pathMan);
        if (travel->follower == 0)
            return;
        advanceFollower(ee::Ptr<Follower>(travel->follower), time);

        bool leaves = false;
        uint32_t atStart = 0;
        const ee::Float along = followerAlong(ee::Ptr<Follower>(travel->follower));
        if (sce::libgcc::dpcmp(sce::libgcc::fptodp(along), kNearPathEnd) > 0 &&
            sce::libgcc::dpcmp(sce::libgcc::fptodp(followerSpeed(ee::Ptr<Follower>(travel->follower))), kDoubleZero) > 0)
        {
            leaves = true;
        }
        else if (sce::libgcc::dpcmp(sce::libgcc::fptodp(along), kNearPathStart) < 0 &&
                 sce::libgcc::dpcmp(sce::libgcc::fptodp(followerSpeed(ee::Ptr<Follower>(travel->follower))), kDoubleZero) < 0)
        {
            leaves = true;
            atStart = 1;
        }
        if (leaves)
        {
            const uint32_t exit = pickSegmentExit(pathMan, ee::Ptr<Follower>(travel->follower), atStart, kPathTagExits);
            putPathManOnPath(pathMan, exit);
            setPathManState(pathMan, kPathManOnPath, pathManBlendTime(pathMan));
        }
    }
    READABLE_FUNCTION_WAITS(walkPathManPath, Verified);

    // walkPathManFastPath: a "FastPathMan" moves on by that time at its
    // rate, wrapping round at either end of its path.
    void walkPathManFastPath(ee::Ptr<SceneObject> pathMan, ee::Float time)
    {
        ee::Local<Vec4> positionCopy;
        ee::Local<Vec4> position;
        if (pathManAnimation(pathMan)->paused == 1)
            return;
        // Its position and its name's hash are asked for and not used.
        *positionCopy = *pathManPosition(pathMan, position);
        objectNameHash(pathMan);

        const ee::Ptr<zge::FastFollower> follower(pathManTravel(pathMan)->follower);
        steerPathManSpeed(pathMan);
        const ee::Float rate = zge::fastFollowerRate(follower);
        const ee::Float endTime = zge::fastPathEndTime(zge::fastFollowerPath(follower));
        const ee::Float moved = zge::fastFollowerTime(follower) + rate * time;
        if (moved < 0.0f || endTime < moved)
        {
            const ee::Float wrapped = ee::Float(0.0f) < rate ? moved - endTime : endTime + moved;
            zge::setFastFollowerTime(follower, wrapped);
            follower->placedTime = wrapped;
        }
        else
        {
            zge::setFastFollowerTime(follower, moved);
        }
    }
    READABLE_FUNCTION_WAITS(walkPathManFastPath, Verified);

    // movePathMan: the path man moves along its path for that time and is
    // put where its follower now is, facing along the path. An animation
    // that asks for no speed leaves it where it is, unless `evenWhenStill`.
    void movePathMan(ee::Ptr<SceneObject> pathMan, uint32_t evenWhenStill, ee::Float time)
    {
        const ee::Ptr<PathManAnimation> animation = pathManAnimation(pathMan);
        if (evenWhenStill == 0 && ee::abs(animation->speedScale) < kNoSpeed)
            return;

        const ee::Ptr<PathManTravel> travel = pathManTravel(pathMan);
        if (travel->fast != 0)
        {
            walkPathManFastPath(pathMan, time);
            if (travel->follower == 0)
                return;
            // The follower's own room holds what is asked of it.
            const ee::Ptr<zge::FastFollower> follower(travel->follower);
            const ee::Ptr<Vec4> position = &follower->position;
            const ee::Ptr<Quaternion> orientation = &follower->orientation;
            zge::fastFollowerPosition(follower, position);
            setObjectPosition(pathMan, position);
            zge::fastFollowerOrientation(follower, orientation);
            setObjectOrientation(pathMan, orientation);
        }
        else
        {
            ee::Local<Matrix> placing;
            ee::Local<Vec3> up;
            walkPathManPath(pathMan, time);
            if (travel->follower == 0)
                return;
            *up = kFollowerUp;
            followerOrientation(ee::Ptr<Follower>(travel->follower), up, placing);
            setObjectRotationMatrix(pathMan, placing);
            setObjectPosition(pathMan, ee::Ptr<Vec4>(placing.address() + 3 * sizeof(Vec4)));
        }
    }
    READABLE_FUNCTION_WAITS(movePathMan, Verified);
}
