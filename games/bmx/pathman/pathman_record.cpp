// BMX XXX's path men (pathman.h): the record each one keeps beside its tag
// record, made the first time it is asked for. BMX XXX's own code.

#include "pathman.h"
#include "pathman_calls.h"
#include "pathman_random.h"
#include "pathman_tags.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/text/cstring.h"
#include "zge/memory/pool.h"
#include "zge/scene/animation.h"
#include "zge/scene/follower.h"

#include <cstdint>
#include <limits>
#include "sym/bmx/pathman/pathman_record.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        // The pool the records come from, made with the first and destroyed
        // with the last.
        inline ee::Global<ee::Ptr<Pool>, sym::g_pathManRecords> g_pathManRecords;

        // The left and right bones a mirrored pose swaps, by name (a bone
        // in the middle is paired with itself); a row of zeros ends it.
        constexpr uint32_t kMirrorBoneRows = 12;
        inline ee::Global<BoneNamePair[kMirrorBoneRows], sym::pathManMirrorBones> g_pathManMirrorBones;

        // A path man that gives an objective notices a rider at least this
        // far away.
        constexpr ee::Float kLeastObjectiveRange = 5.0f;
        // No rider measured yet.
        constexpr ee::Float kNoRiderNear = std::numeric_limits<float>::max();
        // The frames before it first speaks in passing: up to this many.
        constexpr uint32_t kFirstPhraseFrames = 0x400;
    }

    // setUpPathManRecord: the record cleared and filled from the path man's
    // tags: its objective (looked up by name), its ranges squared, its top
    // speed, a random wait before its first phrase, and its bone mirror.
    void setUpPathManRecord(ee::Ptr<SceneObject> pathMan, ee::Ptr<PathManRecord> record)
    {
        memoryFill(record, 0, sizeof(PathManRecord));
        record->objectiveName[0] = 0;
        pathManObjectiveName(pathMan, objectiveNameOf(record));
        if (record->objectiveName[0] == 0)
            record->objective = {};
        else
            record->objective = zge::findObjective(objectiveNameOf(record));

        ee::Float noticeRange = pathManNoticeRange(pathMan);
        if (record->objective)
            noticeRange = ee::max(noticeRange, kLeastObjectiveRange);
        record->noticeRangeSquared = noticeRange * noticeRange;
        record->awakeRangeSquared = record->noticeRangeSquared + record->noticeRangeSquared;
        const ee::Float touchRange = pathManTouchRange(pathMan);
        record->touchRangeSquared = touchRange * touchRange;
        record->drawnRiderDistanceSquared = kNoRiderNear;
        record->riderDistanceSquared = kNoRiderNear;

        const int32_t phraseFrames = pathManRandom(pathMan, kFirstPhraseFrames);
        record->riderNearFrames = 0;
        record->phraseFrames = static_cast<uint16_t>(phraseFrames);
        const ee::Float topSpeed = pathManTopSpeed(pathMan);
        record->animation.state = kPathManNoState;
        record->travel.speedWanted = topSpeed;
        record->travel.topSpeed = topSpeed;
        record->animation.boneMirror = newPathManBoneMirror(pathMan);
    }
    READABLE_FUNCTION_WAITS(setUpPathManRecord, Verified);

    // pathManRecord
    ee::Ptr<PathManRecord> pathManRecord(ee::Ptr<SceneObject> pathMan)
    {
        ee::Ptr<PathManRecord> record(tagWord(objectComponent(pathMan), kPathManTagWords, kPathManTagRecord));
        if (!record)
        {
            record = ee::Ptr<PathManRecord>(zge::poolTakeCreating(&*g_pathManRecords, sizeof(PathManRecord)));
            setUpPathManRecord(pathMan, record);
            setTagWord(objectComponent(pathMan), kPathManTagWords, kPathManTagRecord, record);
        }
        return record;
    }
    READABLE_FUNCTION_WAITS(pathManRecord, Verified);

    // pathManTravel: the record's first part.
    ee::Ptr<PathManTravel> pathManTravel(ee::Ptr<SceneObject> pathMan)
    {
        return &pathManRecord(pathMan)->travel;
    }
    READABLE_FUNCTION_WAITS(pathManTravel, Verified);

    // pathManAnimation: the record's animation part.
    ee::Ptr<PathManAnimation> pathManAnimation(ee::Ptr<SceneObject> pathMan)
    {
        return &pathManRecord(pathMan)->animation;
    }
    READABLE_FUNCTION_WAITS(pathManAnimation, Verified);

    // freePathManRecord: its follower deleted, its animator and bone mirror
    // freed, the record given back to the pool and the path man's tag for it
    // cleared.
    void freePathManRecord(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<PathManRecord> record = pathManRecord(pathMan);
        if (record->travel.follower != 0)
        {
            if (record->travel.fast != 0)
                zge::deleteFastFollower(ee::Ptr<zge::FastFollower>(record->travel.follower));
            else
                deletePathFollower(ee::Ptr<Follower>(record->travel.follower));
            record->travel.follower = 0;
        }
        pathman::freeAnimatorOf(pathMan);
        freeBoneMirror(record->animation.boneMirror);
        record->animation.boneMirror = {};
        zge::poolGiveDestroying(&*g_pathManRecords, record);
        game::tail(setTagWord, objectComponent(pathMan), kPathManTagWords, kPathManTagRecord, 0u);
    }
    READABLE_FUNCTION_WAITS(freePathManRecord, Verified);

    // newPathManBoneMirror: a bone mirror for the path man's model, from a
    // copy of the game's table of bone names.
    ee::Ptr<BoneMirror> newPathManBoneMirror(ee::Ptr<SceneObject> pathMan)
    {
        ee::Local<BoneNamePair[kMirrorBoneRows]> names;
        for (uint32_t row = 0; row < kMirrorBoneRows; ++row)
            names[row] = (*g_pathManMirrorBones)[row];
        return ee::Ptr<BoneMirror>(zge::newBoneMirror(pathMan, names));
    }
    READABLE_FUNCTION_WAITS(newPathManBoneMirror, Verified);
}
