// DM2's level goals (game.c).

#include "goals.h"

#include "players.h"
#include "round.h"
#include "score.h"
#include "dm2/action/action.h"
#include "dm2/action/action_state.h"
#include "dm2/rider/actions/actions.h"
#include "dm2/rider/collision/collision.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/overlay/overlay.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/zgui/widget_parts.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "ee/readable.h"
#include "zge/math/conversions.h"
#include "zge/math/types.h"
#include "zge/render/material.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/dm2/game/goals.cpp.h"

namespace game::dm2
{
    int32_t releaseGoalsMaterial()
    {
        return game::tail(zge::releaseMaterial, *g_goalsMaterial);
    }
    READABLE_FUNCTION(releaseGoalsMaterial, Verified);

    uint32_t gameTouchHandler(uint32_t player, ee::Ptr<zge::SceneObject> object)
    {
        return goalTouch(player, object);
    }
    READABLE_FUNCTION(gameTouchHandler, Verified);

    namespace
    {
        // ScoreBlock::goalFlags: two reward bits a challenge (0 .. 14); 0x8000
        // is the level's gaps all found.
        constexpr uint32_t kChallengeRewardBits = 0x7FFF;
    }

    uint32_t challengeRewardEarned(uint32_t player)
    {
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        if (multiplayerMode() != 0)
            return 0;
        if (singlePlayerMode() == kLevelGoals)
        {
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
            const uint8_t count = (*g_challengeGoalCounts)[goals->challenge];
            if (goals->goalsDone != count)
                return 0;
            for (uint32_t goal = 0; goal < count; ++goal)
                if (goals->challenges[goals->challenge].goals[goal].flags & kGoalGiven)
                    return 1;
            return 0;
        }
        if (singlePlayerMode() != kCompetition)
            return 0;
        return (rider->score.goalFlags & kChallengeRewardBits) != 0 ? 1 : 0;
    }
    READABLE_FUNCTION(challengeRewardEarned, Verified);

    namespace
    {
        // The byte of the player's current challenge's goal that keeps the
        // objective's four bits (two objectives a byte; the original's
        // address arithmetic, so a goal or objective past the record's
        // reaches the bytes after it), and the shift of its four bits.
        ee::Ptr<uint8_t> objectiveByte(uint32_t player, uint32_t goal, uint32_t objective)
        {
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
            const ee::Ptr<uint8_t> parts(ee::Ptr<uint8_t>(goals->challenges[goals->challenge].goals[0].parts));
            return parts + static_cast<ptrdiff_t>(goal * sizeof(ChallengeGoal) + (objective >> 1));
        }
        uint32_t objectiveShift(uint32_t objective)
        {
            return (objective & 1) * 4;
        }
        constexpr uint32_t kObjectiveStateMask = 0x7;
        constexpr uint32_t kObjectiveMarkShift = 3;
        // The objective states the goals' code sets: 0 not started (its goal
        // not given yet), 1 started (its goal given), 4 met, 6 met before its
        // goal was given (giveGoal makes it 4). (2 shows the objective label,
        // 5 activates the objective's entities: setObjectiveState.)
        constexpr uint32_t kObjectiveStateNotStarted = 0;
        constexpr uint32_t kObjectiveStateStarted = 1;
        constexpr uint32_t kObjectiveStateMet = 4;
        constexpr uint32_t kObjectiveStateMetEarly = 6;
        // 3: done in the current trick line (the line's landing is to meet it: giveGoal makes it 4).
        constexpr uint32_t kObjectiveStateInLine = 3;

    }

    uint32_t objectiveState(uint32_t player, uint32_t goal, uint32_t objective)
    {
        return (*objectiveByte(player, goal, objective) >> objectiveShift(objective)) & kObjectiveStateMask;
    }
    READABLE_FUNCTION(objectiveState, Verified);

    uint32_t objectiveMarked(uint32_t player, uint32_t goal, uint32_t objective, uint32_t mask)
    {
        return (*objectiveByte(player, goal, objective) >> (objectiveShift(objective) + kObjectiveMarkShift)) & (mask & 1);
    }
    READABLE_FUNCTION(objectiveMarked, Verified);

    void setObjectiveMark(uint32_t player, uint32_t goal, uint32_t objective, uint32_t bits, uint32_t on)
    {
        const ee::Ptr<uint8_t> at = objectiveByte(player, goal, objective);
        const uint8_t mark = static_cast<uint8_t>(bits << (objectiveShift(objective) + kObjectiveMarkShift));
        *at = on != 0 ? static_cast<uint8_t>(*at | mark) : static_cast<uint8_t>(*at & ~mark);
    }
    READABLE_FUNCTION(setObjectiveMark, Verified);

    namespace
    {
        // Scaffolding (address-call): DM2's nameHash called at its address,
        // as the original calls it: the call after a hash (initLevelGoals'
        // newMaterial) is handed the registers the hash left, which a plain
        // call does not leave.
        namespace calls
        {
            uint32_t nameHash(ee::Ptr<const char> name) { return ee::game<uint32_t>(sym::nameHash, name); }
        }
    }

    uint32_t bmxobjctvGenerateHash(ee::Ptr<const char> name)
    {
        if (!name)
            return 0;
        return calls::nameHash(name);
    }
    READABLE_FUNCTION(bmxobjctvGenerateHash, Verified);


    namespace
    {
        // The definitions' names are hashed once a run.
        inline ee::Global<uint32_t, sym::g_goalNamesHashed> g_goalNamesHashed;
        // The goals' material's states (setMaterialState's numbers and values).
        constexpr uint32_t kGoalsMaterialStates[4][2] = {{3, 1}, {4, 0}, {5, 2}, {11, 1}};

        // The words of an objective of each kind that hold names: kinds 1 .. 3
        // the fourth, 6 and 7 the second, 8 .. 13 and 23 the first, 14 .. 21
        // the first two.
        void hashObjectiveNames(GoalObjective &objective)
        {
            switch (objective.kind)
            {
            case 1: case 2: case 3:
                objective.words[3] = bmxobjctvGenerateHash(ee::Ptr<const char>(objective.words[3]));
                break;
            case 6: case 7:
                objective.words[1] = bmxobjctvGenerateHash(ee::Ptr<const char>(objective.words[1]));
                break;
            case 8: case 9: case 10: case 11: case 12: case 13: case 23:
                objective.words[0] = bmxobjctvGenerateHash(ee::Ptr<const char>(objective.words[0]));
                break;
            case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21:
                for (uint32_t word = 0; word < 2; ++word)
                    objective.words[word] = bmxobjctvGenerateHash(ee::Ptr<const char>(objective.words[word]));
                break;
            default:
                break;
            }
        }
    }

    // initLevelGoals: bug kept: every level's goals are hashed by the counts
    // of goals the session level's challenges have (g_challengeGoalCounts),
    // so a level whose challenge has more goals than the first level played
    // keeps those goals' names unhashed for the run.
    void initLevelGoals()
    {
        if (*g_goalNamesHashed == 0)
        {
            for (uint32_t level = 0; level < kGoalLevels; ++level)
                for (uint32_t challenge = 0; challenge < kLevelChallenges; ++challenge)
                    for (uint32_t goal = 0; goal < (*g_challengeGoalCounts)[challenge]; ++goal)
                    {
                        GoalDefinition &definition = (*g_goalDefinitions)[level][challenge][goal];
                        for (uint32_t objective = 0; objective < definition.objectiveCount; ++objective)
                            hashObjectiveNames(definition.objectives[objective]);
                    }
            *g_goalNamesHashed = 1;
        }
        *g_goalsMaterial = ee::Address(zge::newMaterial());
        for (const auto &state : kGoalsMaterialStates)
            // (the engine's jump to setMaterialState: AI's name for the
            // same one-instruction function)
            zge::zgui::rebuildMaterialRegisters(ee::Ptr<zge::Material>(*g_goalsMaterial), 0, state[0], state[1]);
        zge::memoryFill(g_objectiveTargets.get(), 0, sizeof(*g_objectiveTargets));
    }
    READABLE_FUNCTION(initLevelGoals, Verified);

    namespace
    {
        constexpr uint32_t kObjectiveBits = 0xF;
        constexpr uint32_t kObjectiveStateLabelShown = 2;
        constexpr uint32_t kObjectiveStateActivates = 5;
        // The engine message an objective's entities are sent (0x100), with
        // 0x20 bytes of data, all 0.
        constexpr uint32_t kActivateMessage = 0x100;
        constexpr uint32_t kActivateDataBytes = 0x20;
        constexpr uint32_t kMessageQueued = 1;
    }

    void setObjectiveState(uint32_t player, uint32_t goal, uint32_t objective, uint32_t state)
    {
        ee::Local<uint8_t[4]> label;  // the objective label's message
        ee::Local<uint8_t[0x20]> data;
        ee::Local<uint32_t> name;
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
        const ee::Ptr<uint8_t> parts(ee::Ptr<uint8_t>(goals->challenges[goals->challenge].goals[0].parts));
        const ee::Ptr<uint8_t> at = parts + static_cast<ptrdiff_t>(goal * sizeof(ChallengeGoal) + (objective >> 1));
        const uint32_t shift = objectiveShift(objective);
        const uint8_t was = *at;
        *at = static_cast<uint8_t>((was & ~(kObjectiveBits << shift)) | (state << shift));
        if (((was >> shift) & kObjectiveStateMask) == kObjectiveStateLabelShown && state > kObjectiveStateLabelShown)
        {
            // The label hidden (its second byte is the frame's, as the original leaves it).
            label[0] = kOverlayNone;
            label[2] = 0;
            label[3] = 0;
            overlayMessage(kOverlayObjective, label);
        }
        if (state != kObjectiveStateActivates)
            return;
        const GoalObjective &definition =
            (*g_goalDefinitions)[*g_sessionLevel][goals->challenge][goal].objectives[objective];
        for (uint32_t entity = 0; entity < kGoalObjectives; ++entity)
        {
            if (!definition.entities[entity])
                continue;
            *name = bmxobjctvGenerateHash(definition.entities[entity]);
            if (*name == 0)
                continue;
            ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(name, 1);
            if (!found)
                continue;
            zge::memoryFill(data, 0, kActivateDataBytes);
            *ee::Ptr<uint32_t>(data.address()) = 0;
            for (; *found; found = found + 1)
                zge::sendNewMessage(rider::riderOf(riderIndexOf(player))->messenger, *found, kActivateMessage, data,
                                    kMessageQueued);
        }
    }
    READABLE_FUNCTION(setObjectiveState, Verified);

    void giveGoal(uint32_t player, uint32_t goal)
    {
        ee::Local<uint8_t[4]> label; // the objective label's state, then its message
        rider::riderOf(riderIndexOf(player)); // (looked up; nothing is read)
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
        GoalDefinition *definition = &(*g_goalDefinitions)[*g_sessionLevel][goals->challenge][0];
        overlayFrameState(kOverlayObjective, label);
        ChallengeGoal &given = goals->challenges[goals->challenge].goals[goal];
        if (!(given.flags & kGoalRevealed))
            given.flags |= kGoalRevealed | kGoalGiven;
        for (uint32_t other = 0; other < (*g_challengeGoalCounts)[goals->challenge]; ++other, ++definition)
        {
            if (goals->challenges[goals->challenge].goals[other].flags & kGoalDone)
                continue;
            for (uint32_t objective = 0; objective < definition->objectiveCount; ++objective)
            {
                if (other == goal)
                {
                    if (objectiveState(player, other, objective) < kObjectiveStateMet)
                        setObjectiveState(player, other, objective, kObjectiveStateStarted);
                    // (A state asked again: the original's test for 3 here can never hold.)
                    else if (objectiveState(player, other, objective) == 3 ||
                             objectiveState(player, other, objective) == kObjectiveStateMetEarly)
                        setObjectiveState(player, other, objective, kObjectiveStateMet);
                }
                else
                {
                    const uint32_t state = objectiveState(player, other, objective);
                    if (state != kObjectiveStateNotStarted && state < kObjectiveStateMet)
                        setObjectiveState(player, other, objective, kObjectiveStateStarted);
                }
            }
        }
        // The label hidden (its second byte as its state had it).
        label[0] = kOverlayNone;
        label[2] = 0;
        label[3] = 0;
        overlayMessage(kOverlayObjective, label);
    }
    READABLE_FUNCTION(giveGoal, Verified);

    namespace
    {
        // (kObjectiveTouchNonColliding: an object whose node queries do not test, its collision off.)

        // Whether touching `object` meets the objective.
        bool touchMeets(const GoalObjective &objective, ee::Ptr<zge::SceneObject> object)
        {
            if (objective.kind == kObjectiveTouchObject)
                return zge::objectNameHash(object) == objective.words[0];
            if (objective.kind == kObjectiveTouchNonColliding)
            {
                const uint64_t node = rider::collision::nodeHandle(ee::Address(zge::objectLevelNode(object)));
                return rider::collision::nodeCollidable(node) == 0 && zge::objectNameHash(object) == objective.words[0];
            }
            return false;
        }
    }

    uint32_t goalTouch(uint32_t player, ee::Ptr<zge::SceneObject> object)
    {
        ee::Local<uint8_t[kActivateDataBytes]> data;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        if (multiplayerMode() != 0 || singlePlayerMode() != kLevelGoals)
            return 0;
        uint32_t touched = 0;
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
        const GoalDefinition *definition = &(*g_goalDefinitions)[*g_sessionLevel][goals->challenge][0];
        for (uint32_t goal = 0; goal < (*g_challengeGoalCounts)[goals->challenge]; ++goal, ++definition)
        {
            if (goals->challenges[goals->challenge].goals[goal].flags & kGoalDone)
                continue;
            for (uint32_t objective = 0; objective < definition->objectiveCount; ++objective)
            {
                if (objectiveState(player, goal, objective) >= kObjectiveStateMet)
                    continue;
                if (!touchMeets(definition->objectives[objective], object))
                    continue;
                setObjectiveState(player, goal, objective,
                                  objectiveState(player, goal, objective) == kObjectiveStateNotStarted
                                      ? kObjectiveStateMetEarly : kObjectiveStateMet);
                touched = 1;
                zge::memoryFill(data, 0, kActivateDataBytes);
                *ee::Ptr<uint32_t>(data.address()) = 0;
                zge::sendNewMessage(rider->messenger, object, kActivateMessage, data, kMessageQueued);
            }
        }
        return touched;
    }
    READABLE_FUNCTION(goalTouch, Verified);

    void growObjectiveBox(GoalObjective *objective, const ee::Float *point)
    {
        for (uint32_t axis = 0; axis < 3; ++axis)
            objective->boxMin[axis] = ee::min(objective->boxMin[axis], point[axis]);
        for (uint32_t axis = 0; axis < 3; ++axis)
            objective->boxMax[axis] = ee::max(objective->boxMax[axis], point[axis]);
    }
    READABLE_FUNCTION(growObjectiveBox, Verified);

    namespace
    {
        // The empty box an objective's starts from, and the margin round its targets.
        constexpr float kEmptyBoxMin = 99999.9f, kEmptyBoxMax = -99999.9f;
        constexpr float kBoxMargin = 20.0f;

        // The objective's box grown by a list's targets as faces (their
        // centres), edges (both ends) or objects (their positions).
        void growByFaces(GoalObjective &objective, const TargetList &list, ee::Ptr<ee::Float> point)
        {
            for (uint32_t i = 0; i < list.count; ++i)
            {
                rider::collision::faceCentre(list.targets[i], point);
                growObjectiveBox(&objective, point);
            }
        }
        void growByEdges(GoalObjective &objective, const TargetList &list, ee::Ptr<ee::Float> point)
        {
            for (uint32_t i = 0; i < list.count; ++i)
            {
                for (uint32_t end = 0; end < 2; ++end)
                {
                    rider::collision::edgePoint(list.targets[i], end, point);
                    growObjectiveBox(&objective, point);
                }
            }
        }
        void growByObjects(GoalObjective &objective, const TargetList &list, ee::Ptr<ee::Float> point)
        {
            for (uint32_t i = 0; i < list.count; ++i)
            {
                zge::quadwordOf(point) =
                    *zge::objectPosition(ee::Ptr<zge::SceneObject>(static_cast<uint32_t>(list.targets[i])));
                growObjectiveBox(&objective, point);
            }
        }
    }

    void fitObjectiveBox(uint32_t challenge, uint32_t goal, uint32_t objective)
    {
        ee::Local<ee::Float[4]> point;
        GoalObjective &check = (*g_goalDefinitions)[*g_sessionLevel][challenge][goal].objectives[objective];
        const ObjectiveTargets &found = (*g_objectiveTargets)[goal][objective];
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            check.boxMin[axis] = kEmptyBoxMin;
            check.boxMax[axis] = kEmptyBoxMax;
        }
        const TargetList &first = found.named[0];
        const TargetList &second = found.named[1];
        switch (check.kind)
        {
        case kObjectiveTransfer:
        case kObjectiveTransferTrickSpin:
        case kObjectiveTransferMotionSpin:
        case kObjectiveTransferTrickMotion:
            growByFaces(check, second, point);
            [[fallthrough]];
        case kObjectiveTrickOnFace:
        case kObjectiveActionOnFace:
        case kObjectiveWheelieOnFace:
            growByFaces(check, first, point);
            break;
        case kObjectiveGrindTransfer:
            growByEdges(check, second, point);
            [[fallthrough]];
        case kObjectiveTrickOnEdge:
        case kObjectiveGrindOnEdge:
            growByEdges(check, first, point);
            break;
        case kObjectiveObjectsTransfer:
            growByObjects(check, second, point);
            [[fallthrough]];
        case kObjectiveTrickOnObject:
        case kObjectiveActionOnObject:
        case kObjectiveGrindOnObject:
        case kObjectiveAirOffObject:
        case kObjectiveTrickOffObject:
        case kObjectiveTouchObject:
        case kObjectiveTouchNonColliding:
            growByObjects(check, first, point);
            break;
        case kObjectiveFaceEdgeTransfer:
            growByFaces(check, first, point);
            growByEdges(check, second, point);
            break;
        case kObjectiveFaceObjectTransfer:
            growByFaces(check, first, point);
            growByObjects(check, second, point);
            break;
        default:
            break;
        }
        for (uint32_t axis = 0; axis < 3; ++axis)
            check.boxMin[axis] = check.boxMin[axis] - ee::Float(kBoxMargin);
        for (uint32_t axis = 0; axis < 3; ++axis)
            check.boxMax[axis] = check.boxMax[axis] + ee::Float(kBoxMargin);
    }
    READABLE_FUNCTION(fitObjectiveBox, Verified);

    namespace rider::collision
    {
        uint64_t *collectTaggedFaces(const uint32_t *words, uint32_t count); // faces.cpp
        uint64_t *collectTaggedEdges(const uint32_t *words, uint32_t count);
    }

    namespace
    {
        // A found list (0 ended; none: 0) copied into a target list, 128 at most; the targets copied.
        uint32_t copyTargets(TargetList &list, const uint64_t *found)
        {
            uint32_t n = 0;
            if (found)
                while (found[n] != 0 && n < kObjectiveTargetsMost)
                {
                    list.targets[n] = found[n];
                    ++n;
                }
            return n;
        }
        // The same for objects: each object's address goes in its entry's low
        // word (the high word keeps what it held).
        // A target entry's low word (an object's address goes there; the high word keeps what it held).
        ee::Ptr<uint32_t> lowWordOf(uint64_t &target) { return ee::Ptr<uint64_t>(&target).cast<uint32_t>(); }
        uint32_t copyObjectTargets(TargetList &list, ee::Ptr<ee::Ptr<zge::SceneObject>> found)
        {
            uint32_t n = 0;
            if (found)
                while (found[n] && n < kObjectiveTargetsMost)
                {
                    *lowWordOf(list.targets[n]) = ee::Address(found[n]);
                    ++n;
                }
            return n;
        }
    }

    void findObjectiveTargets(uint32_t challenge, uint32_t goal, uint32_t objective)
    {
        // The objective's names: kinds 1 .. 3 keep theirs in words[3], 6 and 7
        // in words[1], 8 .. 13 and 23 in words[0], the gaps in words[0] and [1].
        ee::Local<uint32_t[4]> names;
        const GoalObjective &check = (*g_goalDefinitions)[*g_sessionLevel][challenge][goal].objectives[objective];
        ObjectiveTargets &found = (*g_objectiveTargets)[goal][objective];
        const uint32_t kind = check.kind;
        names[0] = 0;
        names[1] = 0;
        switch (kind)
        {
        case kObjectiveTrickOnFace:
        case kObjectiveTrickOnObject:
        case kObjectiveTrickOnEdge:
            names[0] = check.words[3];
            break;
        case kObjectiveActionOnFace:
        case kObjectiveActionOnObject:
            names[0] = check.words[1];
            break;
        case kObjectiveGrindOnEdge:
        case kObjectiveGrindOnObject:
        case kObjectiveWheelieOnFace:
        case kObjectiveAirOffObject:
        case kObjectiveTrickOffObject:
        case kObjectiveTouchObject:
        case kObjectiveTouchNonColliding:
            names[0] = check.words[0];
            break;
        case kObjectiveTransfer:
        case kObjectiveTransferTrickSpin:
        case kObjectiveTransferMotionSpin:
        case kObjectiveTransferTrickMotion:
        case kObjectiveGrindTransfer:
        case kObjectiveObjectsTransfer:
        case kObjectiveFaceEdgeTransfer:
        case kObjectiveFaceObjectTransfer:
            for (uint32_t i = 0; i < 2; ++i)
                names[i] = check.words[i];
            break;
        default:
            break;
        }

        uint32_t second = 0;
        uint32_t first = 0;
        TargetList &firstList = found.named[0];
        TargetList &secondList = found.named[1];
        switch (kind)
        {
        case kObjectiveTransfer:
        case kObjectiveTransferTrickSpin:
        case kObjectiveTransferMotionSpin:
        case kObjectiveTransferTrickMotion:
            second = copyTargets(secondList, rider::collision::collectTaggedFaces(&names[1], 1));
            [[fallthrough]];
        case kObjectiveTrickOnFace:
        case kObjectiveActionOnFace:
        case kObjectiveWheelieOnFace:
            first = copyTargets(firstList, rider::collision::collectTaggedFaces(names, 1));
            break;
        case kObjectiveGrindTransfer:
            second = copyTargets(secondList, rider::collision::collectTaggedEdges(&names[1], 1));
            [[fallthrough]];
        case kObjectiveTrickOnEdge:
        case kObjectiveGrindOnEdge:
            first = copyTargets(firstList, rider::collision::collectTaggedEdges(names, 1));
            break;
        case kObjectiveObjectsTransfer:
            second = copyObjectTargets(secondList, zge::findObjects(&names[1], 1));
            [[fallthrough]];
        case kObjectiveTrickOnObject:
        case kObjectiveActionOnObject:
        case kObjectiveGrindOnObject:
        case kObjectiveAirOffObject:
        case kObjectiveTrickOffObject:
        case kObjectiveTouchObject:
        case kObjectiveTouchNonColliding:
            first = copyObjectTargets(firstList, zge::findObjects(names, 1));
            break;
        case kObjectiveFaceEdgeTransfer:
            first = copyTargets(firstList, rider::collision::collectTaggedFaces(names, 1));
            second = copyTargets(secondList, rider::collision::collectTaggedEdges(&names[1], 2));
            break;
        case kObjectiveFaceObjectTransfer:
            first = copyTargets(firstList, rider::collision::collectTaggedFaces(names, 1));
            second = copyObjectTargets(secondList, zge::findObjects(&names[1], 1));
            break;
        default:
            break;
        }
        secondList.count = second;
        firstList.count = first;
    }
    READABLE_FUNCTION(findObjectiveTargets, Verified);

    namespace
    {
        constexpr uint32_t kLastChallenge = kLevelChallenges - 1;
        constexpr uint32_t kGoalEntities = 3;
        // The activate message's first data word as a restart sends it (setObjectiveState's first activation
        // sends 0).
        constexpr uint32_t kActivateWordRestart = 1;

        // The entity named `entity` (0: none) and every object of its name in the scene sent the activate message
        // from the player's rider. `data` and `name` are the caller's frame's (the original's places).
        void activateAgain(uint32_t player, ee::Ptr<const char> entity, ee::Local<uint8_t[kActivateDataBytes]> &data,
                           ee::Local<uint32_t> &name)
        {
            if (!entity)
                return;
            *name = bmxobjctvGenerateHash(entity);
            if (*name == 0)
                return;
            ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(name, 1);
            if (!found)
                return;
            zge::memoryFill(data, 0, kActivateDataBytes);
            *ee::Ptr<uint32_t>(data.address()) = kActivateWordRestart;
            for (; *found; found = found + 1)
                zge::sendNewMessage(rider::riderOf(riderIndexOf(player))->messenger, *found, kActivateMessage, data,
                                    kMessageQueued);
        }
    }

    void restartLevelGoals(uint32_t player)
    {
        ee::Local<uint8_t[kActivateDataBytes]> data;
        ee::Local<uint32_t> name;
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
        goals->challenge = 0;
        for (;;)
        {
            const GoalDefinition *definition = &(*g_goalDefinitions)[*g_sessionLevel][goals->challenge][0];
            goals->goalsDone = 0;
            for (uint32_t goal = 0; goal < (*g_challengeGoalCounts)[goals->challenge]; ++goal, ++definition)
            {
                ChallengeGoal &flags = goals->challenges[goals->challenge].goals[goal];
                if (!(flags.flags & kGoalDone))
                {
                    for (uint32_t objective = 0; objective < definition->objectiveCount; ++objective)
                    {
                        setObjectiveState(player, goal, objective,
                                          objectiveState(player, goal, objective) != kObjectiveStateNotStarted
                                              ? kObjectiveStateStarted : kObjectiveStateNotStarted);
                        setObjectiveMark(player, goal, objective, 1, 1);
                    }
                    flags.flags &= static_cast<uint8_t>(~kGoalGiven);
                    flags.flags &= static_cast<uint8_t>(~kGoalObjectivesMet);
                }
                else
                {
                    ++goals->goalsDone;
                    for (uint32_t objective = 0; objective < definition->objectiveCount; ++objective)
                        for (uint32_t entity = 0; entity < kGoalObjectives; ++entity)
                            activateAgain(player, definition->objectives[objective].entities[entity], data, name);
                    for (uint32_t entity = 0; entity < kGoalEntities; ++entity)
                        activateAgain(player, definition->entities[entity], data, name);
                }
                flags.flags &= static_cast<uint8_t>(~kGoalGiven);
            }
            if (goals->goalsDone < (*g_challengeGoalCounts)[goals->challenge])
                break;
            goals->challenge = static_cast<uint8_t>(goals->challenge + 1);
            if (goals->challenge >= kLevelChallenges)
                break;
        }
        goals->challenge = goals->challenge > kLastChallenge ? static_cast<uint8_t>(kLastChallenge) : goals->challenge;

        const GoalDefinition *definition = &(*g_goalDefinitions)[*g_sessionLevel][goals->challenge][0];
        for (uint32_t goal = 0; goal < (*g_challengeGoalCounts)[goals->challenge]; ++goal, ++definition)
            for (uint32_t objective = 0; objective < definition->objectiveCount; ++objective)
            {
                findObjectiveTargets(goals->challenge, goal, objective);
                fitObjectiveBox(goals->challenge, goal, objective);
            }
        if (goals->challenge == 0)
        {
            giveGoal(player, 0);
            return;
        }
        if (goals->challenge == kLastChallenge)
            return;
        // (A challenge with no goals would count to 0xFFFFFFFF, as the original's unsigned compare does.)
        for (uint32_t goal = 0; goal < static_cast<uint32_t>((*g_challengeGoalCounts)[goals->challenge]) - 1; ++goal)
            giveGoal(player, goal);
    }
    READABLE_FUNCTION(restartLevelGoals, Verified);

    namespace
    {
        // The record kinds whose values are counts or points (the measures
        // after them are compared as floats).
        constexpr uint32_t kCountedRecordKinds = 6;
    }

    void goalRecord(uint32_t player, uint32_t kind, uint32_t value)
    {
        if (multiplayerMode() != 0 || singlePlayerMode() != kLevelGoals)
            return;
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
        const uint32_t challenge = goals->challenge;
        const GoalDefinition *definition = (*g_goalDefinitions)[*g_sessionLevel][challenge];
        for (uint32_t goal = 0; goal < (*g_challengeGoalCounts)[challenge]; ++goal, ++definition)
        {
            if (goals->challenges[challenge].goals[goal].flags & kGoalDone)
                continue;
            for (uint32_t objective = 0; objective < definition->objectiveCount; ++objective)
            {
                const GoalObjective &check = definition->objectives[objective];
                if (check.kind != kObjectiveRecord || check.words[0] != kind)
                    continue;
                if (objectiveState(player, goal, objective) == kObjectiveStateNotStarted ||
                    objectiveState(player, goal, objective) > kObjectiveStateInLine)
                    continue;
                if (kind < kCountedRecordKinds)
                {
                    if (check.words[1] < value)
                        setObjectiveState(player, goal, objective, kObjectiveStateMet);
                }
                else if (ee::toFloat(static_cast<int32_t>(check.words[1])) < zge::unsignedToFloat(value))
                {
                    setObjectiveState(player, goal, objective, kObjectiveStateMet);
                }
            }
        }
    }
    READABLE_FUNCTION(goalRecord, Verified);

    uint32_t inObjectiveBox(uint32_t player, uint32_t goal, uint32_t objective)
    {
        ee::Local<zge::Vec4> at;
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
        const GoalObjective &box = (*g_goalDefinitions)[*g_sessionLevel][goals->challenge][goal].objectives[objective];
        switch (box.kind)
        {
        case kObjectiveTrick:
        case kObjectiveAirTricks:
        case kObjectiveAction:
        case 22: // (kinds 22 and 24: not read yet)
        case 24:
            return 1;
        default:
            break;
        }
        *at = *rider::vec4(rider::riderOf(riderIndexOf(player))->position);
        if (!(box.boxMin[0] <= at->x) || !(box.boxMin[1] <= at->y) || !(box.boxMin[2] <= at->z))
            return 0;
        if (!(at->x <= box.boxMax[0]) || !(at->y <= box.boxMax[1]) || !(at->z <= box.boxMax[2]))
            return 0;
        return 1;
    }
    READABLE_FUNCTION(inObjectiveBox, Verified);

    namespace
    {
        // A trick or motion an objective asks for: any (none too), or any but none (a trick: an air trick).
        constexpr uint32_t kWantAny = 0xFF;
        constexpr uint32_t kWantSome = 0xFE;
        // The nose wheelie's trick (PhysicsState kPhysicsNoseWheelie's).
        constexpr uint8_t kNoseWheelieTrick = 0x47;
        // The contact kinds the checks tell apart (Rider::contactKind): the faces' are those below 4.
        constexpr uint8_t kContactFaces = 4;
        constexpr uint8_t kContactObjectEdge = 7;   // (inferred: read as a rail edge's, its node's object)
        // The reward display's kinds a goal's progress sends (kOverlayReward; "$KSTRSP_REWARD_OBJCTV%02d"): the
        // objectives still to meet less one, the goals done (from 2), the challenge done (from 6).
        constexpr uint8_t kRewardObjectivesLeft = 0;
        constexpr uint8_t kRewardGoalsDone = 2;
        constexpr uint8_t kRewardChallengeDone = 6;
        constexpr uint16_t kLabelShown = 0xFF;

        bool trickWanted(uint32_t want, uint8_t trick)
        {
            if (want == kWantAny || want == trick)
                return true;
            if (want != kWantSome || trick == rider::kNoTrick)
                return false;
            return trick < rider::kGrindTrickFirst;
        }
        bool motionWanted(uint32_t want, uint8_t motion)
        {
            if (want == kWantAny || want == motion)
                return true;
            return want == kWantSome && motion != rider::kNoTrick;
        }

        // The turn's current (last) trick line: the line before the first when there is none (the original's
        // address arithmetic, which the runs' checks reach).
        ee::Ptr<rider::ScoreLine> lastLine(const ee::Ptr<rider::Rider> &rider)
        {
            const ee::Ptr<rider::ScoreLine> lines(rider->score.lines);
            return lines + (static_cast<int32_t>(rider->score.lineCount) - 1);
        }

        // A trick objective's first test: the current line's last trick is the trick and motion asked for, the
        // line's spin as much as asked. Its line, or 0.
        ee::Ptr<rider::ScoreLine> trickLine(const ee::Ptr<rider::Rider> &rider, const GoalObjective &objective)
        {
            if (rider->score.lineCount == 0)
                return {};
            const ee::Ptr<rider::ScoreLine> line = lastLine(rider);
            ee::Ptr<rider::ScoreTrick> trick = line->tricks;
            while (trick->next)
                trick = trick->next;
            if (!trickWanted(objective.words[0], trick->trick) || !motionWanted(objective.words[1], trick->trickMotion))
                return {};
            if (line->spinHalfTurns < objective.words[2])
                return {};
            return line;
        }
        // A trick objective done: a grind, stall or later trick asked by itself is met at once; an air trick (or
        // any trick) waits for the line.
        uint32_t trickDone(const GoalObjective &objective)
        {
            const uint32_t trick = objective.words[0];
            if (trick < rider::kGrindTrickFirst || trick == kWantAny || trick == kWantSome)
                return kObjectiveStateInLine;
            return kObjectiveStateMet;
        }
        // A run objective's last test, on the current line's first trick: ridden fakie (or the nose wheelie)
        // when asked, its motion as asked. Met, or 0.
        uint32_t runDone(const ee::Ptr<rider::Rider> &rider, const GoalObjective &objective, bool noseWheelie)
        {
            if (objective.words[2] != 0)
            {
                if (rider->score.lineCount == 0)
                    return 0;
                const ee::Ptr<rider::ScoreTrick> first = lastLine(rider)->tricks;
                if (noseWheelie ? first->trick != kNoseWheelieTrick : !(first->flags & rider::kScoreTrickFakie))
                    return 0;
            }
            const uint32_t want = objective.words[3];
            if (want == kWantAny)
                return kObjectiveStateMet;
            return motionWanted(want, lastLine(rider)->tricks->trickMotion) ? kObjectiveStateMet : 0;
        }

        uint32_t contactWord(const uint8_t *contact)
        {
            uint32_t word;
            std::memcpy(&word, contact, sizeof(word));
            return word;
        }
        // The object of the rider's contact as an edge (its node's), or 0.
        uint32_t edgeObject(const uint8_t *contact)
        {
            const uint64_t node = rider::collision::nodeOf(rider::collision::widen(contactWord(contact)));
            return rider::collision::engine::nodeObject(node, 0u);
        }
        uint32_t nameHashOf(uint32_t object)
        {
            return zge::objectNameHash(ee::Ptr<zge::SceneObject>(object));
        }

        // The state an open objective's check gives it this frame (0: not done). `lastObject` is the frame's: an
        // object trick objective on a contact of another kind takes the object found last (bug kept).
        uint32_t objectiveCheck(const ee::Ptr<rider::Rider> &rider, const GoalObjective &objective,
                                uint32_t &lastObject)
        {
            const uint32_t *words = objective.words;
            switch (objective.kind)
            {
            case kObjectiveTrick:
                return trickLine(rider, objective) ? trickDone(objective) : 0;
            case kObjectiveTrickOnFace:
                if (!trickLine(rider, objective))
                    return 0;
                if (rider->contactKind >= rider::kContactKinds || rider->contactKind == kContactRailEdge ||
                    rider->contactKind == rider::kContactObject)
                    return 0;
                if (rider::collision::faceTagWord(rider->contactRef()) != words[3])
                    return 0;
                return trickDone(objective);
            case kObjectiveTrickOnObject:
                if (!trickLine(rider, objective))
                    return 0;
                if (rider->contactKind == rider::kContactObject)
                    lastObject = contactWord(rider->contact);
                else if (rider->contactKind == kContactRailEdge || rider->contactKind == kContactObjectEdge)
                    lastObject = edgeObject(rider->contact);
                if (lastObject == 0 || nameHashOf(lastObject) != words[3])
                    return 0;
                return trickDone(objective);
            case kObjectiveTrickOnEdge:
                if (!trickLine(rider, objective) || rider->contactKind != kContactRailEdge)
                    return 0;
                return rider::collision::edgeTagWord(rider->contactRef()) == words[3] ? kObjectiveStateInLine : 0;
            case kObjectiveAirTricks:
            {
                uint32_t airTricks = 0;
                const uint32_t lines = rider->score.lineCount;
                for (uint32_t line = 0; line < lines; ++line)
                    for (ee::Ptr<rider::ScoreTrick> trick = rider->score.lines[line].tricks; trick; trick = trick->next)
                        if (trick->trick < rider::kGrindTrickFirst)
                            ++airTricks;
                return airTricks < words[0] ? 0 : kObjectiveStateInLine;
            }
            case kObjectiveAction:
                return rider::actionState(rider.get(), rider::kLevelAction) == words[0] ? kObjectiveStateMet : 0;
            case kObjectiveActionOnFace:
                if (rider::actionState(rider.get(), rider::kLevelAction) != words[0] || rider->contactKind >= kContactFaces)
                    return 0;
                if (rider::collision::faceTagWord(rider->contactRef()) != words[1])
                    return 0;
                // (riding or a wheelie meets it at once; the others wait for the line)
                return words[0] == rider::kActionRiding || words[0] == rider::kActionWheelie ? kObjectiveStateMet
                                                                                             : kObjectiveStateInLine;
            case kObjectiveActionOnObject:
                if (rider::actionState(rider.get(), rider::kLevelAction) != words[0] ||
                    rider->contactKind != rider::kContactObject)
                    return 0;
                return nameHashOf(contactWord(rider->contact)) == words[1] ? kObjectiveStateInLine : 0;
            case kObjectiveGrindOnEdge:
                if (rider::actionState(rider.get(), rider::kLevelAction) != rider::kActionGrind &&
                    rider::actionState(rider.get(), rider::kLevelAction) != rider::kActionStall)
                    return 0;
                if (rider->contactKind != kContactRailEdge ||
                    rider::collision::edgeTagWord(rider->contactRef()) != words[0])
                    return 0;
                if (!(zge::unsignedToFloat(words[1]) <= rider->score.kindScores[0]))
                    return 0;
                return runDone(rider, objective, false);
            case kObjectiveGrindOnObject:
                if (!((rider::actionState(rider.get(), rider::kLevelAction) == rider::kActionGrind &&
                       rider::physicsState(rider.get()) == rider::kPhysicsGrind) ||
                      (rider::actionState(rider.get(), rider::kLevelAction) == rider::kActionStall &&
                       rider::physicsState(rider.get()) == rider::kPhysicsStall)))
                    return 0;
                if (rider->contactKind != kContactRailEdge)
                    return 0;
                lastObject = edgeObject(rider->contact);
                if (lastObject == 0 || nameHashOf(lastObject) != words[0])
                    return 0;
                if (!(zge::unsignedToFloat(words[1]) <= rider->score.kindScores[0]))
                    return 0;
                return runDone(rider, objective, false);
            case kObjectiveWheelieOnFace:
                if (rider::actionState(rider.get(), rider::kLevelAction) != rider::kActionWheelie ||
                    rider->contactKind >= kContactFaces)
                    return 0;
                if (rider::collision::faceTagWord(rider->contactRef()) != words[0])
                    return 0;
                if (!(zge::unsignedToFloat(words[1]) <= rider->score.kindScores[1]))
                    return 0;
                return runDone(rider, objective, true);
            case kObjectiveAirOffObject:
            case kObjectiveTrickOffObject:
            {
                if (rider::actionState(rider.get(), rider::kLevelAction) != rider::kActionAir ||
                    rider->contactKind != rider::kContactObject)
                    return 0;
                if (nameHashOf(contactWord(rider->contact)) != words[0])
                    return 0;
                // Not while the back wheel is still on the object (or on an edge of it).
                const rider::WheelContact &back = rider->wheels[0];
                if (back.kind == rider::kContactObject && nameHashOf(contactWord(back.contact)) == words[0])
                    return 0;
                if (back.kind == kContactRailEdge)
                {
                    lastObject = edgeObject(back.contact);
                    if (lastObject != 0 && nameHashOf(lastObject) == words[0])
                        return 0;
                }
                if (objective.kind != kObjectiveTrickOffObject)
                    return kObjectiveStateInLine;
                const uint32_t lines = rider->score.lineCount;
                for (uint32_t line = 0; line < lines; ++line)
                    for (ee::Ptr<rider::ScoreTrick> trick = rider->score.lines[line].tricks; trick; trick = trick->next)
                        if (trick->trick == words[2])
                            return kObjectiveStateInLine;
                return 0;
            }
            default:
                return 0;
            }
        }
    }

    void goalsFrame(uint32_t player)
    {
        ee::Local<LabelState> label;
        ee::Local<OverlayReward> reward;
        ee::Local<uint8_t[kActivateDataBytes]> data;
        ee::Local<uint32_t> name;
        uint32_t lastObject = 0;
        const ee::Ptr<RiderCharacter> character = playerCharacter(player);
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        if (multiplayerMode() != 0 || singlePlayerMode() != kLevelGoals)
            return;
        overlayFrameState(kOverlayObjective, label);
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
        const GoalDefinition *definition = &(*g_goalDefinitions)[*g_sessionLevel][goals->challenge][0];
        for (uint32_t goal = 0; goal < (*g_challengeGoalCounts)[goals->challenge]; ++goal, ++definition)
        {
            if (goals->challenges[goals->challenge].goals[goal].flags & kGoalDone)
                continue;
            for (uint32_t objective = 0; objective < definition->objectiveCount; ++objective)
            {
                if (objectiveState(player, goal, objective) != kObjectiveStateNotStarted &&
                    objectiveState(player, goal, objective) <= kObjectiveStateLabelShown)
                {
                    const uint32_t done = objectiveCheck(rider, definition->objectives[objective], lastObject);
                    if (done != 0)
                        setObjectiveState(player, goal, objective, done);
                }

                const uint32_t state = objectiveState(player, goal, objective);
                if (state == kObjectiveStateNotStarted)
                    continue;
                if (state <= kObjectiveStateLabelShown)
                {
                    fitObjectiveBox(goals->challenge, goal, objective);
                    if (inObjectiveBox(player, goal, objective) == 0)
                    {
                        setObjectiveState(player, goal, objective, kObjectiveStateStarted);
                        continue;
                    }
                    setObjectiveState(player, goal, objective, kObjectiveStateLabelShown);
                    if (objectiveMarked(player, goal, objective, 1) != 0)
                        setObjectiveMark(player, goal, objective, 1, 0);
                    if (label->visible == 0)
                    {
                        label->visible = kLabelShown;
                        label->value = static_cast<uint8_t>(goals->challenge * kChallengeGoals + goal);
                        overlayMessage(kOverlayObjective, label);
                    }
                    continue;
                }
                if (state != kObjectiveStateMet)
                    continue;

                // Met: counted into its goal.
                setObjectiveState(player, goal, objective, kObjectiveStateActivates);
                ChallengeGoal &counted = goals->challenges[goals->challenge].goals[goal];
                counted.flags = static_cast<uint8_t>(counted.flags + 1);
                if ((counted.flags & kGoalObjectivesMet) != definition->objectiveCount)
                {
                    if (actionMode() == kActionPlay)
                    {
                        const uint8_t left = goals->challenges[goals->challenge].goals[goal].flags & kGoalObjectivesMet;
                        reward->kind = static_cast<uint8_t>(kRewardObjectivesLeft + definition->objectiveCount - left - 1);
                        overlayMessage(kOverlayReward, reward);
                    }
                    continue;
                }
                // The goal done: the reward shown (in play), its entities activated, its respect earned.
                ++goals->goalsDone;
                if (actionMode() == kActionPlay)
                {
                    reward->kind = goals->goalsDone < (*g_challengeGoalCounts)[goals->challenge]
                                       ? static_cast<uint8_t>(kRewardGoalsDone + goals->goalsDone)
                                       : static_cast<uint8_t>(kRewardChallengeDone + goals->challenge);
                    reward->value = zge::unsignedToFloat(static_cast<uint32_t>(definition->respect));
                    overlayMessage(kOverlayReward, reward);
                }
                zge::memoryFill(data, 0, kActivateDataBytes);
                *ee::Ptr<uint32_t>(data.address()) = 0;
                for (uint32_t entity = 0; entity < kGoalEntities; ++entity)
                {
                    if (!definition->entities[entity])
                        continue;
                    *name = bmxobjctvGenerateHash(definition->entities[entity]);
                    if (*name == 0)
                        continue;
                    ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(name, 1);
                    if (!found)
                        continue;
                    for (; *found; found = found + 1)
                        zge::sendNewMessage(rider->messenger, *found, kActivateMessage, data, kMessageQueued);
                }
                if (actionMode() == kActionPlay)
                {
                    const uint32_t respect = static_cast<uint32_t>(definition->respect);
                    // (by the session level: the park editor's, 8, which has no goals, would reach `respect`)
                    (&character->levelRespect[0])[*g_sessionLevel] += respect;
                    character->respect += respect;
                    rider->score.respectEarned += respect;
                }
                goals->challenges[goals->challenge].goals[goal].flags |= kGoalDone | kGoalGiven;
            }
        }
    }
    READABLE_FUNCTION(goalsFrame, Verified);

    namespace
    {
        // Whether any of the turn's lines holds a trick the objective asks
        // for: the trick, its motion, and the line's spin at least.
        bool linesHoldTrick(const rider::ScoreBlock &score, uint32_t trick, uint32_t motion, uint32_t spin)
        {
            for (uint32_t index = 0; index < score.lineCount; ++index)
            {
                const rider::ScoreLine &line = score.lines[index];
                for (ee::Ptr<rider::ScoreTrick> held = line.tricks; held; held = held->next)
                    if (line.spinHalfTurns >= spin && trickWanted(trick, held->trick) && motionWanted(motion, held->trickMotion))
                        return true;
            }
            return false;
        }
    }

    // goalObjectTrick: a trick line's end (trickEvent). Landed, each wheel's
    // contact is reduced to its target (an edge's or face's tag word, an
    // object's name hash: an edge on an object counts as the object); a
    // contact of kinds 6, 9 or 10 ends the check. Then, in a ProQuest level,
    // each open objective of the current challenge: a gap objective started
    // (states 1 .. 2) is met when the two wheels landed on its two targets
    // (and, for 15 .. 17, a line holds its trick); an objective done in the
    // line (state 3) is met as the line lands (an air off an object not when
    // it landed on that object; a trick objective when a line holds its
    // trick), or goes back to started when it bails.
    void goalObjectTrick(uint32_t player, uint32_t landed)
    {
        // The two wheels' targets and what they are. Bug kept: a wheel with
        // no contact leaves its words as the stack held them, and the checks
        // read them.
        ee::Local<uint32_t[2]> targets;
        ee::Local<uint32_t[2]> targetKinds;
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
        const rider::ScoreBlock &score = rider->score;
        if (landed != 0)
        {
            for (uint32_t wheel = 0; wheel < 2; ++wheel)
            {
                const rider::WheelContact &contact = rider->wheels[wheel];
                uint32_t object;
                uint64_t ref;
                std::memcpy(&object, contact.contact, sizeof(object));
                std::memcpy(&ref, contact.contact, sizeof(ref));
                if (object == 0)
                    continue;
                uint32_t tag;
                uint32_t kind;
                switch (contact.kind)
                {
                case kContactKind6:
                case kContactKind9:
                case kContactKind10:
                    return;
                case rider::kContactObject:
                    tag = zge::objectNameHash(ee::Ptr<zge::SceneObject>(object));
                    kind = kTaggedObject;
                    break;
                case kContactRailEdge:
                {
                    const uint64_t node = rider::collision::nodeOf(rider::collision::widen(object));
                    const ee::Ptr<zge::SceneObject> owner =
                        zge::nodeSceneObject(ee::Ptr<zge::Node>(static_cast<uint32_t>(node)), ee::Ptr<ee::Ptr<zge::SceneBone>>());
                    if (owner)
                    {
                        tag = zge::objectNameHash(owner);
                        kind = kTaggedObject;
                    }
                    else
                    {
                        tag = rider::collision::edgeTagWord(ref);
                        kind = kTaggedEdge;
                    }
                    break;
                }
                default:
                    tag = rider::collision::faceTagWord(ref);
                    kind = kTaggedFace;
                    break;
                }
                targets[wheel] = tag;
                targetKinds[wheel] = kind;
            }
        }
        if (multiplayerMode() != 0 || singlePlayerMode() != kLevelGoals)
            return;

        const ee::Ptr<LevelGoals> goals = playerLevelGoals(player, *g_sessionLevel);
        const uint32_t challenge = goals->challenge;
        const GoalDefinition *definition = (*g_goalDefinitions)[*g_sessionLevel][challenge];
        for (uint32_t goal = 0; goal < (*g_challengeGoalCounts)[challenge]; ++goal, ++definition)
        {
            if (goals->challenges[challenge].goals[goal].flags & kGoalDone)
                continue;
            for (uint32_t objective = 0; objective < definition->objectiveCount; ++objective)
            {
                const GoalObjective &check = definition->objectives[objective];
                if (objectiveState(player, goal, objective) == kObjectiveStateNotStarted ||
                    objectiveState(player, goal, objective) > kObjectiveStateLabelShown || landed == 0 ||
                    targets[0] == 0 || targets[1] == 0 || targets[0] == targets[1])
                {
                    // Not a gap's landing: an objective done in the line lands or bails with it.
                    if (objectiveState(player, goal, objective) != kObjectiveStateInLine)
                        continue;
                    if (landed == 0)
                    {
                        setObjectiveState(player, goal, objective, kObjectiveStateStarted);
                        continue;
                    }
                    if (check.kind == kObjectiveTrick)
                    {
                        const bool held = linesHoldTrick(score, check.words[0], check.words[1], check.words[2]);
                        setObjectiveState(player, goal, objective, held ? kObjectiveStateMet : kObjectiveStateStarted);
                        continue;
                    }
                    if ((check.kind == kObjectiveAirOffObject || check.kind == kObjectiveTrickOffObject) &&
                        targetKinds[1] == kTaggedObject && targets[1] == check.words[0])
                        setObjectiveState(player, goal, objective, kObjectiveStateStarted);
                    else
                        setObjectiveState(player, goal, objective, kObjectiveStateMet);
                    continue;
                }

                // A gap's landing: the two wheels on the objective's two targets.
                const uint32_t a = check.words[0], b = check.words[1];
                const bool either = (targets[0] == a && targets[1] == b) || (targets[0] == b && targets[1] == a);
                const bool inOrder = targets[0] == a && targets[1] == b;
                switch (check.kind)
                {
                case kObjectiveTransfer:
                case kObjectiveTransferTrickSpin:
                case kObjectiveTransferMotionSpin:
                case kObjectiveTransferTrickMotion:
                {
                    if (targetKinds[0] != kTaggedFace || targetKinds[1] != kTaggedFace || !either)
                        break;
                    if (check.kind == kObjectiveTransfer)
                    {
                        setObjectiveState(player, goal, objective, kObjectiveStateMet);
                        break;
                    }
                    // 15: a trick (words[2]) with a spin (words[3]); 16: a motion with a spin; 17: a trick and a motion.
                    const uint32_t trick = check.kind == kObjectiveTransferTrickSpin || check.kind == kObjectiveTransferTrickMotion
                                               ? check.words[2] : kWantAny;
                    uint32_t motion = check.kind == kObjectiveTransferMotionSpin ? check.words[2] : kWantAny;
                    const uint32_t spin = check.kind == kObjectiveTransferTrickSpin || check.kind == kObjectiveTransferMotionSpin
                                              ? check.words[3] : 0;
                    if (check.kind == kObjectiveTransferTrickMotion)
                        motion = check.words[3];
                    if (linesHoldTrick(score, trick, motion, spin))
                        setObjectiveState(player, goal, objective, kObjectiveStateMet);
                    break;
                }
                case kObjectiveGrindTransfer:
                    if (targetKinds[0] == kTaggedEdge && targetKinds[1] == kTaggedEdge && either)
                        setObjectiveState(player, goal, objective, kObjectiveStateMet);
                    break;
                case kObjectiveObjectsTransfer:
                    if (targetKinds[0] == kTaggedObject && targetKinds[1] == kTaggedObject && either)
                        setObjectiveState(player, goal, objective, kObjectiveStateMet);
                    break;
                case kObjectiveFaceEdgeTransfer:
                    if (targetKinds[0] == kTaggedFace && targetKinds[1] == kTaggedEdge && inOrder)
                        setObjectiveState(player, goal, objective, kObjectiveStateMet);
                    break;
                case kObjectiveFaceObjectTransfer:
                    if (targetKinds[0] == kTaggedFace && targetKinds[1] == kTaggedObject && inOrder)
                        setObjectiveState(player, goal, objective, kObjectiveStateMet);
                    break;
                default:
                    break;
                }
            }
        }
    }
    READABLE_FUNCTION(goalObjectTrick, Verified);
}
