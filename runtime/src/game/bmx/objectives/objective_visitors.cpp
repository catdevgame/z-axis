// BMX XXX's objectives manager: what it hands the engine to call back. The
// action hook (an objective's script block acting on a level object) and
// the visits of every objective: a level's objectives given their places,
// a player's frame of the goals, an event passed on, and a turn's start.
// BMX XXX's own code (its game block); no build names these functions: the
// names are from what each one's code does.

#include "objectives.h"

#include "bmx/action/camera_state.h"
#include "bmx/game/round.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/objective_visitors.cpp.h"

namespace game::bmx::objectives
{
    namespace
    {
        inline ee::Global<uint8_t, sym::g_currentPlayer> g_currentPlayer;
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        // The level's string table and the game's (the one normally in use).
        inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_levelStrings> g_levelStrings;
        inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_gameStrings> g_gameStrings;

        // The words a script block's action on an object may be, each with
        // the message the object is sent: ACTIVATE, DEACTIVATE, ENABLE,
        // DISABLE (the hashes of these words) and a fifth, not recovered.
        constexpr uint32_t kObjectActionWords = 5;
        struct ObjectActionWord
        {
            uint32_t message;  // +0x00 the entity message's number
            uint32_t nameHash; // +0x04 the word, hashed
        };
        static_assert(sizeof(ObjectActionWord) == 0x08);
        constexpr uint32_t kEntityDeactivateMessage = 0x101; // DEACTIVATE

        // What an activation or a deactivation from a script block carries
        // (the level's activation record, as this code fills it).
        struct ActionActivation
        {
            uint32_t quiet;  // +0x00 the hook's last word: the block runs for an objective put back, not one changing now
            uint32_t player; // +0x04
            uint8_t _08[8];
            zge::Vec4 velocity; // +0x10 (0, 0, 0, 1): no touch (DM2's ActivationData's name)
        };
        static_assert(offsetof(ActionActivation, velocity) == 0x10 && sizeof(ActionActivation) == 0x20);

        // The objectives that give each level's three slot rewards, by name
        // hash (twelve levels).
        constexpr uint32_t kLevels = 12;
        // A gap's name starts with these letters.
        constexpr uint32_t kGapPrefixLetters = 3;

        // The saved game's options word and the bit that makes the gaps
        // highlight. Scaffolding (unnamed-field): the profile lane's header
        // has no field for it yet.
        constexpr uint32_t kSavedGameOptionsOffset = 0x02;
        constexpr uint16_t kOptionHighlightGaps = 0x4;

        // The rewards for a level's goals by how many are done (twenty a
        // level), and the level's three slot rewards.
        constexpr uint32_t kLevelGoalRewards = 20;
        // A slot reward of this kind sets the game's byte for it to its
        // value and a round flag.
        constexpr uint16_t kRewardKind16 = 0x10;
        constexpr uint32_t kRoundFlagOfKind16 = 0x200;
        inline ee::Global<uint8_t, sym::g_rewardKind16Value> g_rewardKind16Value;

        // A goal done adds this much to the rider's health, up to 0xFF.
        constexpr uint32_t kGoalDoneGain = 0x19;

        constexpr ee::Float kTicksPerSecondFloat = 60.0f;
        constexpr uint16_t kDisplayAsked = 0xFF; // the objective display was sent an objective in this visit

        bool running(uint32_t state)
        {
            return state == kObjectiveActive || state == kObjectiveInProgress;
        }

        ee::Ptr<Reward> goalReward(uint32_t level, uint32_t goalsDone)
        {
            return ee::Ptr<Reward>(sym::g_goalRewards + level * kLevelGoalRewards * sizeof(Reward) + (goalsDone - 1u) * sizeof(Reward));
        }
        ee::Ptr<Reward> slotReward(uint32_t level, uint32_t slot)
        {
            return ee::Ptr<Reward>(sym::g_slotRewards + level * kLevelSlotRewards * sizeof(Reward) + (slot - 1u) * sizeof(Reward));
        }
    }

    // The cues a script block can give (the engine's cue hook's kinds).
    enum ObjectiveCueKind : uint32_t
    {
        kCueCutScene = 0, // (the cut scene's id, whether it plays now)
        kCueSound = 1,    // (the sound event's third word)
    };
    // The sound module's event a sound cue sends (no name for it here).
    constexpr uint32_t kCueSoundEvent = 0x4A;

    // objectiveCue (the engine's cue hook): a script block's cue: a cut
    // scene, or a sound. Always 1.
    uint32_t objectiveCue(uint32_t kind, uint32_t first, uint32_t second)
    {
        if (kind == kCueSound)
            soundEvent(kCueSoundEvent, 0, first, 0, 0);
        else if (kind == kCueCutScene)
            playCutScene(first, second);
        return 1;
    }
    READABLE_FUNCTION_WAITS(objectiveCue, Verified);

    // objectiveObjectAction (the engine's action hook): a script block's
    // action on a level object. The action's word picks the message the
    // object is sent at once; an activation or deactivation carries the
    // current player. 0 for a word that is none of the five, else 1.
    uint32_t objectiveObjectAction(ee::Address object, ee::Ptr<const char> action, uint32_t quiet)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<ActionActivation> activation;
        ee::Local<ObjectActionWord[kObjectActionWords]> words;
        const ee::Ptr<ObjectActionWord> table(sym::objectActionWords);
        for (uint32_t i = 0; i < kObjectActionWords; ++i)
            words[i] = table[i];
        const uint32_t hash = zge::nameHash(action);
        uint32_t word = 0;
        while (word < kObjectActionWords && words[word].nameHash != hash)
            ++word;
        if (word == kObjectActionWords)
            return 0;
        message->target = object;
        message->flags = zge::kMessageNow;
        message->sender = 0;
        message->data = ee::Address();
        message->type = words[word].message;
        if (words[word].message == kEntityActivateMessage || words[word].message == kEntityDeactivateMessage)
        {
            message->data = activation;
            activation->quiet = quiet;
            activation->player = *g_currentPlayer;
            activation->velocity = zge::Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        }
        zge::sendEngineMessage(message);
        return 1;
    }
    READABLE_FUNCTION_WAITS(objectiveObjectAction, Verified);

    // registerGoal (loadLevelObjectives' visit): the objective gets the next
    // goal slot. A PERSISTENT objective is one of the player's goals of the
    // level: marked saved; a gap when its name starts with GAP (gaps
    // highlight when the saved game's option says so); counted among the
    // goals unless it is a set's child; and given its reward slot when it is
    // one of the three objectives the level's slot rewards are for.
    uint32_t registerGoal(Objective objective, ee::Ptr<GoalRegistration> registration)
    {
        ee::Local<uint32_t[kLevels * kLevelSlotRewards]> slotGoals;
        ee::Local<uint32_t> highlight;
        const ee::Ptr<uint32_t> levelSlotGoals(sym::levelSlotGoals);
        for (uint32_t i = 0; i < kLevels * kLevelSlotRewards; ++i)
            slotGoals[i] = levelSlotGoals[i];
        const ee::Ptr<ObjectiveInfo> info = zge::objectiveInfo(objective);
        (*g_goalSlots)[registration->goal].objective = objective;
        const uint32_t nameHash = zge::nameHash(zge::objectiveName(objective));
        (*g_goalSlots)[registration->goal].nameHash = nameHash;
        if ((info->flags & kObjectivePersistent) != 0)
        {
            registration->goals->marks[registration->goal] |= kGoalSaved;
            if (zge::compareStringsN(zge::objectiveName(objective), sym::gapPrefix, kGapPrefixLetters) == 0)
            {
                registration->goals->gaps = static_cast<uint8_t>(registration->goals->gaps + 1);
                registration->goals->marks[registration->goal] |= kGoalGap;
                if ((*ee::Ptr<uint16_t>(sym::g_savedGame + kSavedGameOptionsOffset) & kOptionHighlightGaps) != 0)
                {
                    zge::memoryFill(highlight, 0, sizeof(uint32_t));
                    *highlight = *highlight | kObjectiveHighlight;
                    calls::setObjectiveFlags(objective, highlight, 1);
                }
            }
            if ((info->flags & kObjectiveIsChild) == 0)
                registration->goals->goals = static_cast<uint8_t>(registration->goals->goals + 1);
            for (uint32_t slot = 0; slot < kLevelSlotRewards; ++slot)
            {
                if (zge::nameHash(zge::objectiveName(objective)) != slotGoals[*g_currentLevel * kLevelSlotRewards + slot])
                    continue;
                uint8_t &mark = registration->goals->marks[registration->goal];
                mark = static_cast<uint8_t>((mark & ~kGoalRewardSlot) | (((slot + 1u) & 3u) << kGoalRewardSlotShift));
            }
        }
        registration->goal = registration->goal + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerGoal, Verified);

    // updateGoal (updatePlayerObjectives' visit): one objective's frame for
    // the player.
    //  - Completed this frame: a saved goal not done before is counted (a
    //    gap among the gaps; any other raises the rider's displayed byte)
    //    and marked done this turn. The reward display is told: a gap done,
    //    and all gaps when that was the last; else, unless it is a set's
    //    child, the objective done, the reward for that many goals when
    //    there is one, and all goals when that was the last. The player's
    //    timed objective ends with it. It is marked done, and a goal with a
    //    reward slot gives the slot's reward.
    //  - Running and activated this frame: one with a time limit becomes
    //    the player's timed objective and the objective display is sent it;
    //    it is marked active.
    //  - In progress, the player's timed objective, and the display not
    //    showing: the display is sent it again.
    // The objective's marks of this frame are cleared. Always 1.
    uint32_t updateGoal(Objective objective, ee::Ptr<PlayerGoalVisit> visit)
    {
        ee::Local<RewardMessage> reward;
        const ee::Ptr<ObjectiveInfo> info = zge::objectiveInfo(objective);
        const ee::Ptr<LevelGoals> goals = playerLevelGoals(visit->player, *g_currentLevel);
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(visit->player));
        const ee::Ptr<RiderObjectives> own = riderObjectives(rider);
        if ((info->flags & kObjectiveCompletedNow) != 0)
        {
            const uint8_t before = goals->marks[visit->goal];
            if ((before & kGoalSaved) != 0 && (before & kGoalDone) == 0)
            {
                if ((before & kGoalGap) == 0)
                {
                    goals->goalsDone = static_cast<uint8_t>(goals->goalsDone + 1);
                    const uint32_t raised = rider->health + kGoalDoneGain;
                    rider->health = static_cast<uint8_t>(raised < 0x100 ? raised : 0xFF);
                }
                else
                {
                    goals->gapsDone = static_cast<uint8_t>(goals->gapsDone + 1);
                }
                goals->marks[visit->goal] |= kGoalDoneThisTurn;
            }
            reward->player = static_cast<uint8_t>(visit->player);
            if ((goals->marks[visit->goal] & kGoalGap) != 0)
            {
                reward->what = ee::Address(info);
                reward->line = kRewardGapDone;
                overlayMessage(kOverlayReward, reward);
                if (goals->gapsDone == goals->gaps && (goals->marks[visit->goal] & kGoalDone) == 0)
                {
                    reward->what = 0;
                    reward->line = kRewardAllGapsDone;
                    overlayMessage(kOverlayReward, reward);
                }
            }
            else if ((info->flags & kObjectiveIsChild) == 0)
            {
                reward->what = ee::Address(info);
                reward->line = kRewardObjectiveDone;
                overlayMessage(kOverlayReward, reward);
                const ee::Ptr<Reward> forGoals = goalReward(*g_currentLevel, goals->goalsDone);
                if (forGoals->kind != 0)
                {
                    reward->what = ee::Address(forGoals);
                    reward->line = kRewardGiven;
                    overlayMessage(kOverlayReward, reward);
                }
                if (goals->goalsDone == goals->goals && (goals->marks[visit->goal] & kGoalDone) == 0)
                {
                    reward->what = 0;
                    reward->line = kRewardAllGoalsDone;
                    overlayMessage(kOverlayReward, reward);
                }
            }
            if (own->timedObjective == objective)
            {
                own->timedObjective = Objective();
                own->ticksLeft = 0;
                visit->shown.goal = kNoGoalShown;
                visit->shown.player = static_cast<uint8_t>(visit->player);
                overlayMessage(kOverlayObjective, &visit->shown);
            }
            goals->marks[visit->goal] |= kGoalDone;
            const uint32_t slot = (goals->marks[visit->goal] & kGoalRewardSlot) >> kGoalRewardSlotShift;
            if (slot != 0)
            {
                const ee::Ptr<Reward> forSlot = slotReward(*g_currentLevel, slot);
                giveReward(static_cast<uint8_t>(visit->player), forSlot);
                reward->what = ee::Address(forSlot);
                reward->line = kRewardGiven;
                overlayMessage(kOverlayReward, reward);
                if (forSlot->kind == kRewardKind16)
                {
                    *g_rewardKind16Value = static_cast<uint8_t>(forSlot->value);
                    setRoundFlags(kRoundFlagOfKind16, 1);
                }
            }
        }
        if (running(info->state))
        {
            if ((info->flags & kObjectiveActivatedNow) != 0)
            {
                if (!(info->timeout == 0.0f))
                {
                    own->timedObjective = objective;
                    own->ticksLeft = static_cast<uint32_t>(ee::toInt(info->timeout * kTicksPerSecondFloat));
                    visit->shown.goal = visit->goal;
                    overlayMessage(kOverlayObjective, &visit->shown);
                    visit->shown.visible = kDisplayAsked;
                }
                goals->marks[visit->goal] |= kGoalActive;
            }
            if (info->state == kObjectiveInProgress && visit->shown.visible == 0 && own->timedObjective == objective)
            {
                visit->shown.goal = visit->goal;
                overlayMessage(kOverlayObjective, &visit->shown);
                visit->shown.visible = kDisplayAsked;
            }
        }
        calls::clearObjectiveFrameMarks(objective);
        visit->goal = visit->goal + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateGoal, Verified);

    // sendEventToObjective (the visit of sendRecordEvent and
    // sendTrickEvent): a running objective gets the event. Always 1.
    uint32_t sendEventToObjective(Objective objective, ee::Address event)
    {
        if (running(zge::objectiveState(objective)))
            zge::sendObjectiveEvent(objective, event);
        return 1;
    }
    READABLE_FUNCTION_WAITS(sendEventToObjective, Verified);

    // sendObjectActivatedToObjective (sendObjectActivatedEvent's visit): a
    // running objective gets the event. When it sent the object's
    // activation on itself, its status line goes to the reward display
    // (from the level's strings), unless it is the player's timed objective
    // or one of that set's own. `taken` gathers every objective's answer.
    // Always 1.
    uint32_t sendObjectActivatedToObjective(Objective objective, ee::Ptr<ObjectActivatedEvent> event)
    {
        ee::Local<RewardMessage> reward;
        ee::Local<char[0x100]> status;
        if (!running(zge::objectiveState(objective)))
            return 1;
        const uint32_t takenBefore = event->taken;
        event->taken = 0;
        event->delivered = 0;
        zge::sendObjectiveEvent(objective, ee::Address(event));
        if (event->delivered != 0)
        {
            const Objective timed = riderObjectives(rider::riderOf(riderIndexOf(event->player)))->timedObjective;
            if (!timed || (objective != timed && setContainsObjective(timed, objective) == 0))
            {
                zge::setStringTable(*g_levelStrings);
                zge::objectiveStatusText(objective, status);
                zge::setStringTable(*g_gameStrings);
                if (status[0] != 0)
                {
                    reward->what = status.address();
                    reward->player = static_cast<uint8_t>(event->player);
                    reward->line = kRewardText;
                    overlayMessage(kOverlayReward, reward);
                }
            }
        }
        event->taken = event->taken | takenBefore;
        return 1;
    }
    READABLE_FUNCTION_WAITS(sendObjectActivatedToObjective, Verified);

    // restoreGoal (resetTurnGoals' first visit): the objective put back as
    // the player's saved game has it. A saved goal that is done is counted
    // and completed again; one that was active and has no time limit is
    // made inactive and, when ready, activated again; an objective that is
    // no saved goal is made inactive and loses its done mark. No objective
    // is done this turn yet. Always 1.
    uint32_t restoreGoal(Objective objective, ee::Ptr<GoalRestore> restore)
    {
        const ee::Ptr<LevelGoals> goals = restore->goals;
        const ee::Ptr<ObjectiveInfo> info = zge::objectiveInfo(objective);
        const uint8_t mark = goals->marks[restore->goal];
        if ((mark & kGoalSaved) == 0)
        {
            zge::setObjectiveState(objective, 0);
            goals->marks[restore->goal] &= static_cast<uint8_t>(~kGoalDone);
        }
        else if ((mark & kGoalDone) != 0)
        {
            if ((mark & kGoalGap) == 0)
                goals->goalsDone = static_cast<uint8_t>(goals->goalsDone + 1);
            else
                goals->gapsDone = static_cast<uint8_t>(goals->gapsDone + 1);
            zge::completeObjective(objective, 1);
        }
        else if ((mark & kGoalActive) != 0 && info->timeout == 0.0f)
        {
            zge::setObjectiveState(objective, 0);
            if (zge::objectiveReadyToActivate(objective) != 0)
                zge::activateObjective(objective);
        }
        goals->marks[restore->goal] &= static_cast<uint8_t>(~kGoalDoneThisTurn);
        restore->goal = restore->goal + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(restoreGoal, Verified);

    // markActiveGoal (resetTurnGoals' second visit, after the engine's
    // reset): a running objective the reset activated is marked active.
    // Always 1.
    uint32_t markActiveGoal(Objective objective, ee::Ptr<GoalRestore> restore)
    {
        const ee::Ptr<LevelGoals> goals = restore->goals;
        const ee::Ptr<ObjectiveInfo> info = zge::objectiveInfo(objective);
        if (running(info->state) && (info->flags & kObjectiveActivatedNow) != 0)
            goals->marks[restore->goal] |= kGoalActive;
        restore->goal = restore->goal + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(markActiveGoal, Verified);
}
