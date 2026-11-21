// BMX XXX's path men (pathman.h): the states a path man is in and the
// animation each plays. On its path it plays motions picked at random from
// the path's motion list; a rider coming at it or reaching it puts it in a
// state of its own, and each state's animation ending leads to the next.
// BMX XXX's own code.

#include "pathman.h"
#include "pathman_calls.h"
#include "pathman_random.h"
#include "pathman_tags.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/follower.h"
#include "zge/scene/locators.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/pathman/pathman_animation.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        constexpr uint32_t kMotionListKindName = sym::motionListKindName;          // "MOTIONLIST"
        constexpr uint32_t kNoAnimationStateFormat = sym::bmx::noAnimationStateFormat;  // "No animation state for NPC %s"
        constexpr uint32_t kUndefinedStateFormat = sym::bmx::undefinedStateFormat;      // "Undefined state for NPC %s"
        constexpr PathManPoseHook kMirrorPose(sym::mirrorPathManPose);

        // Where a listed motion's values are in a motion list's tag record.
        constexpr uint32_t listedMotionTag(uint32_t motion, uint32_t field) { return motion * sizeof(ListedMotion) + field; }
    }

    // pathManName: the path man's own name: what follows the last '_' of its
    // level node's name, 0 when there is no node or no '_'.
    ee::Ptr<const char> pathManName(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<const char> nodeName = pathman::objectLevelNodeName(pathMan);
        if (nodeName)
        {
            const ee::Ptr<const char> underscore = stringFindLast(nodeName, '_');
            if (underscore)
                return underscore + 1;
        }
        return {};
    }
    READABLE_FUNCTION(pathManName, Draft);

    // mirrorPathManPose (the animation's pose hook): a mirrored animation's
    // pose has its left and right bones swapped.
    void mirrorPathManPose(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManAnimation> animation = pathManAnimation(pathMan);
        if (animation->mirrored != 0)
            game::tail(mirrorPose, animation->boneMirror);
    }
    READABLE_FUNCTION_WAITS(mirrorPathManPose, Verified);

    // startListedMotion: the `pick`-th motion of the list (empty names not
    // counted) becomes the path man's animation: its name, whether it loops
    // and how it scales the path man's speed. A motion the game has no
    // animation for frees the path man's animator instead.
    void startListedMotion(ee::Ptr<SceneObject> pathMan, ee::Ptr<ScenePoint> motionList, uint32_t pick)
    {
        ee::Local<char[0x40]> name;
        uint32_t named = 0;
        for (uint32_t motion = 0; motion < kListedMotions; ++motion)
        {
            zge::tagString(motionList->tags, kPathManTagTexts, listedMotionTag(motion, offsetof(ListedMotion, name)), name);
            if (name[0] == 0)
                continue;
            if (named++ != pick)
                continue;

            const ee::Ptr<PathManAnimation> animation = pathManAnimation(pathMan);
            animation->speedScale = tagFloat(motionList->tags, kPathManTagWords, listedMotionTag(motion, offsetof(ListedMotion, speedScale)));
            animation->mirrored = 0;
            if (tagByte(motionList->tags, kPathManTagBytes, listedMotionTag(motion, offsetof(ListedMotion, loops))) != 0)
            {
                animation->playMode = pathman::kPlayLooped;
                animation->poseHook = PathManPoseHook();
            }
            else
            {
                animation->poseHook = kMirrorPose;
                animation->playMode = 0;
            }
            animation->state = kPathManOnPath;
            animation->animation = findAnimationInLibraries(name);
            if (animation->animation)
                stringCopy(animation->name, name);
            else
                pathman::freeAnimatorOf(pathMan);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(startListedMotion, Verified);

    // pickPathMotion: one of the motions of the motion list of the path the
    // path man is on, picked at random. Nothing without a follower, a motion
    // list or a motion in it. (A list the level has no point for is not
    // checked for.)
    void pickPathMotion(ee::Ptr<SceneObject> pathMan)
    {
        ee::Local<char[0x40]> name;
        const ee::Ptr<PathManTravel> travel = pathManTravel(pathMan);
        int32_t listName = 0;
        if (travel->fast != 0)
        {
            if (travel->follower != 0)
                listName = fastPathMotionList(zge::fastFollowerPath(ee::Ptr<zge::FastFollower>(travel->follower)));
        }
        else if (travel->follower != 0)
        {
            listName = pathMotionList(followerSegment(ee::Ptr<Follower>(travel->follower)));
        }
        if (listName == 0)
            return;

        const uint32_t kind = nameHash(ee::Ptr<const char>(kMotionListKindName));
        const ee::Ptr<ScenePoint> motionList = findScenePoint(kind, static_cast<uint32_t>(listName), 0);
        uint32_t named = 0;
        for (uint32_t motion = 0; motion < kListedMotions; ++motion)
        {
            zge::tagString(motionList->tags, kPathManTagTexts, listedMotionTag(motion, offsetof(ListedMotion, name)), name);
            if (name[0] != 0)
                ++named;
        }
        if (named == 0)
            return;
        const int32_t pick = pathManRandom(pathMan, named - 1);
        startListedMotion(pathMan, motionList, static_cast<uint32_t>(pick));
    }
    READABLE_FUNCTION_WAITS(pickPathMotion, Verified);

    // setPathManState: the state entered and its animation started.
    //   on the path   a motion of the path's list
    //   a dodge       the dodge animation, mirrored for the other side
    //   hit           the hit animation; the path man stops on its path
    //   after either  that state's animation, looped
    // A state the path man's data has no animation for is not entered (0).
    // Entering the hit state or the one after a dodge from another state, the
    // path man is first put where its model's root bone is (at its own
    // height) and the animation starts with no blend.
    ee::Ptr<Motion> setPathManState(ee::Ptr<SceneObject> pathMan, int32_t state, ee::Float blend)
    {
        const ee::Ptr<PathManAnimation> animation = pathManAnimation(pathMan);
        const int32_t stateBefore = animation->state;
        switch (state)
        {
        case kPathManOnPath:
            pickPathMotion(pathMan);
            if (!animation->animation)
                return {};
            break;

        case kPathManDodge:
        case kPathManDodgeMirrored:
        {
            const ee::Ptr<Motion> dodge = pathManDodgeAnimation(pathMan);
            if (!dodge)
                return {};
            animation->animation = dodge;
            animation->poseHook = kMirrorPose;
            animation->mirrored = state == kPathManDodgeMirrored;
            animation->state = state;
            animation->speedScale = 0.0f;
            animation->playMode = 0;
            break;
        }

        case kPathManHit:
        {
            const ee::Ptr<PathManTravel> travel = pathManTravel(pathMan);
            const ee::Ptr<Motion> hit = pathManHitAnimation(pathMan);
            if (!hit)
                return {};
            animation->state = state;
            animation->animation = hit;
            animation->poseHook = kMirrorPose;
            animation->speedScale = 0.0f;
            animation->playMode = 0;
            animation->mirrored = 0;
            travel->speedWanted = 0.0f;
            if (travel->follower != 0)
            {
                if (travel->fast != 0)
                    zge::setFastFollowerRate(ee::Ptr<zge::FastFollower>(travel->follower), travel->speedWanted);
                else
                    setFollowerSpeed(ee::Ptr<Follower>(travel->follower), travel->speedWanted);
            }
            break;
        }

        case kPathManAfterDodge:
        case kPathManAfterHit:
        {
            const ee::Ptr<Motion> after = state == kPathManAfterHit ? pathManAfterHitAnimation(pathMan) : pathManAfterDodgeAnimation(pathMan);
            if (!after)
                return {};
            animation->state = state;
            animation->animation = after;
            animation->playMode = pathman::kPlayLooped;
            animation->poseHook = kMirrorPose;
            animation->speedScale = 0.0f;
            animation->mirrored = 0;
            break;
        }

        default:
        {
            // The text is made and shown nowhere.
            ee::Local<char[0x40]> text;
            formatString(text, kNoAnimationStateFormat, pathManName(pathMan));
            animation->animation = {};
            animation->state = kPathManNoState;
            return {};
        }
        }

        if (animation->state != stateBefore && (animation->state == kPathManAfterDodge || animation->state == kPathManHit))
        {
            ee::Local<Vec4> rootPosition;
            ee::Local<Vec4> position;
            *position = *objectPosition(pathMan);
            pathManPosition(pathMan, rootPosition);
            blend = 0.0f;
            rootPosition->y = position->y;
            setObjectPosition(pathMan, rootPosition);
        }
        if (pathManActsUnseen(pathMan) == 0)
            animation->playMode |= pathman::kPlayOnScreenOnly;
        playAnimationWithHooks(pathMan, animation->animation, animation->playMode, zge::BlendHook(animation->poseHook), 0, blend);
        return animation->animation;
    }
    READABLE_FUNCTION_WAITS(setPathManState, Verified);

    // nextPathManState: the state that follows the one whose animation has
    // ended: on the path another motion of the list; after a dodge and after
    // a hit the state that loops; with no state, back to the path.
    ee::Ptr<Motion> nextPathManState(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManAnimation> animation = pathManAnimation(pathMan);
        switch (animation->state)
        {
        case kPathManNoState:
            return returnPathManToPath(pathMan);
        case kPathManOnPath:
            return setPathManState(pathMan, kPathManOnPath, pathManBlendTime(pathMan));
        case kPathManDodge:
        case kPathManDodgeMirrored:
        case kPathManAfterDodge:
            return setPathManState(pathMan, kPathManAfterDodge, pathManBlendTime(pathMan));
        case kPathManHit:
        case kPathManAfterHit:
            return setPathManState(pathMan, kPathManAfterHit, pathManBlendTime(pathMan));
        default:
        {
            // The text is made and shown nowhere.
            ee::Local<char[0x40]> text;
            formatString(text, kUndefinedStateFormat, pathManName(pathMan));
            animation->animation = {};
            animation->state = kPathManNoState;
            return {};
        }
        }
    }
    READABLE_FUNCTION_WAITS(nextPathManState, Verified);

    // pathManAnimationEnded: 1 when the path man's animation has ended (or
    // it has no animator), the next state then entered; 0 while it plays or
    // is paused.
    uint32_t pathManAnimationEnded(ee::Ptr<SceneObject> pathMan)
    {
        ee::Local<ee::Float> time;
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        *time = 0.0f;
        if (record->animation.paused != 0)
            return 0;
        if (pathman::objectHasAnimator(pathMan) != 0)
            animationTime(pathMan, time);
        if (animationDone(pathMan, *time) == 0 && pathman::objectHasAnimator(pathMan) != 0)
            return 0;
        nextPathManState(pathMan);
        return 1;
    }
    READABLE_FUNCTION_WAITS(pathManAnimationEnded, Verified);

    // sendPathManBackToPath
    ee::Ptr<Motion> sendPathManBackToPath(ee::Ptr<SceneObject> pathMan)
    {
        return game::tail(returnPathManToPath, pathMan);
    }
    READABLE_FUNCTION_WAITS(sendPathManBackToPath, Verified);

    // returnPathManToPath: back to walking its path, unless it does already
    // or is paused.
    ee::Ptr<Motion> returnPathManToPath(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManAnimation> animation = pathManAnimation(pathMan);
        if (animation->state == kPathManOnPath)
            return {};
        if (animation->paused != 0)
            return {};
        return setPathManState(pathMan, kPathManOnPath, 0.0f);
    }
    READABLE_FUNCTION_WAITS(returnPathManToPath, Verified);
}
