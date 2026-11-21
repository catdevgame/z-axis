// BMX XXX's path men (pathman_tags.h): what the level's data gives each one,
// read from the tag record of its scene object's component. BMX XXX's own code.

#include "pathman_tags.h"
#include "pathman_random.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/animator.h"

#include <cstdint>
#include "sym/bmx/pathman/pathman_tags.cpp.h"

namespace game::bmx
{
    using namespace zge;

    // pathManPath
    int32_t pathManPath(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return tagWord(tags, kPathManTagWords, kPathManTagPath);
    }
    READABLE_FUNCTION(pathManPath, Verified);

    // pathManTopSpeed
    ee::Float pathManTopSpeed(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return tagFloat(tags, kPathManTagWords, kPathManTagTopSpeed);
    }
    READABLE_FUNCTION(pathManTopSpeed, Verified);

    // pathManSpeedStep
    ee::Float pathManSpeedStep(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return tagFloat(tags, kPathManTagWords, kPathManTagSpeedStep);
    }
    READABLE_FUNCTION(pathManSpeedStep, Verified);

    // pathManBlendTime
    ee::Float pathManBlendTime(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return tagFloat(tags, kPathManTagWords, kPathManTagBlendTime);
    }
    READABLE_FUNCTION(pathManBlendTime, Verified);

    // pathManStartTime
    ee::Float pathManStartTime(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return tagFloat(tags, kPathManTagWords, kPathManTagStartFrames) / kFastPathFramesASecond;
    }
    READABLE_FUNCTION(pathManStartTime, Verified);

    // pathMotionList
    int32_t pathMotionList(ee::Ptr<PathRecord> path)
    {
        const ee::Ptr<TagRecord> tags = zge::pathFirstPointTags(path);
        return tagWord(tags, kPathManTagWords, kPathTagMotionList);
    }
    READABLE_FUNCTION(pathMotionList, Verified);

    // fastPathMotionList
    int32_t fastPathMotionList(ee::Ptr<zge::FastPath> path)
    {
        const ee::Ptr<TagRecord> tags = zge::fastPathTags(path);
        return tagWord(tags, kPathManTagWords, kPathTagMotionList);
    }
    READABLE_FUNCTION(fastPathMotionList, Verified);

    // pathManTouchRange
    ee::Float pathManTouchRange(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return tagFloat(tags, kPathManTagWords, kPathManTagTouchRange);
    }
    READABLE_FUNCTION(pathManTouchRange, Verified);

    // pathManNoticeRange
    ee::Float pathManNoticeRange(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return tagFloat(tags, kPathManTagWords, kPathManTagNoticeRange);
    }
    READABLE_FUNCTION(pathManNoticeRange, Verified);

    // pathManHitAnimation
    ee::Ptr<Motion> pathManHitAnimation(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        const uint32_t name = static_cast<uint32_t>(tagWord(tags, kPathManTagWords, kPathManTagHitAnimation));
        if (name == 0)
            return {};
        return findLibraryAnimation(name);
    }
    READABLE_FUNCTION_WAITS(pathManHitAnimation, Verified);

    // pathManDodgeAnimation
    ee::Ptr<Motion> pathManDodgeAnimation(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        const uint32_t name = static_cast<uint32_t>(tagWord(tags, kPathManTagWords, kPathManTagDodgeAnimation));
        if (name == 0)
            return {};
        return findLibraryAnimation(name);
    }
    READABLE_FUNCTION_WAITS(pathManDodgeAnimation, Verified);

    // pathManAfterHitAnimation
    ee::Ptr<Motion> pathManAfterHitAnimation(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        const uint32_t name = static_cast<uint32_t>(tagWord(tags, kPathManTagWords, kPathManTagAfterHitAnimation));
        if (name == 0)
            return {};
        return findLibraryAnimation(name);
    }
    READABLE_FUNCTION_WAITS(pathManAfterHitAnimation, Verified);

    // pathManAfterDodgeAnimation
    ee::Ptr<Motion> pathManAfterDodgeAnimation(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        const uint32_t name = static_cast<uint32_t>(tagWord(tags, kPathManTagWords, kPathManTagAfterDodgeAnimation));
        if (name == 0)
            return {};
        return findLibraryAnimation(name);
    }
    READABLE_FUNCTION_WAITS(pathManAfterDodgeAnimation, Verified);

    // pathManObjectiveName
    uint32_t pathManObjectiveName(ee::Ptr<SceneObject> pathMan, ee::Address out)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return zge::tagString(tags, kPathManTagTexts, kPathManTagObjectiveName, out);
    }
    READABLE_FUNCTION(pathManObjectiveName, Verified);

    // pathManActsUnseen
    uint32_t pathManActsUnseen(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return tagByte(tags, kPathManTagBytes, kPathManTagActsUnseen);
    }
    READABLE_FUNCTION(pathManActsUnseen, Verified);
}
