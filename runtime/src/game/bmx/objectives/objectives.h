#pragma once
// BMX XXX's objectives manager (game/bmx/objectives/objectives.cpp and the
// files beside it): what the game keeps around the engine's objectives.
// The level's objectives come from its "objctv.txt"; the ones the script
// marks PERSISTENT are the level's goals, which the player's saved game
// remembers; one objective with a time limit runs for a player at a time,
// counted down by the rider's own timer and shown on the overlay.
//
// No build names these functions: the names are from what each one's code
// does and from the callers that have names.

#include "objective_calls.h"

#include "bmx/overlay/displays.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/scene/animator.h"
#include "zge/scene/objectives.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/objectives.h.h"

namespace game::bmx::objectives
{
    // ---- The engine's objectives module, as the manager calls it ----
    // The manager calls the module by its names in zge/scene/objectives.h.
    // Scaffolding (address-call): the calls below stay by address: the
    // original's tail jump to activateObjective and readObjectives, which
    // has no body in BMX XXX yet.

    // (the original's tail jump) The types forgotten.
    namespace calls
    {
        inline void shutDownObjectiveSystemTail()
        {
            game::tail(zge::shutDownObjectiveSystem);
        }
    }
    // (the original's tail jump) activateObjective: the objective made
    // active (one whose state is 1 is done at once instead).
    namespace calls
    {
        inline void activateObjectiveTail(Objective objective)
        {
            ee::tailCall<void>(sym::activateObjective, objective);
        }
    }
    // timeOutObjective: the objective's TIMEOUT block run, then the
    // objective reset.
    namespace calls
    {
        inline void timeOutObjective(Objective objective)
        {
            zge::timeOutObjective(objective);
        }
    }
    // forEachObjective: visit(objective, data) for every objective, the
    // oldest first, while it answers non-zero. (The game's own entry of the
    // engine's function: a jump to it.)
    namespace calls
    {
        inline void forEachObjective(uint32_t visit, ee::Address data)
        {
            zge::forEachObjectiveJump(visit, data);
        }
    }
    // setObjectiveFlags: the bits of `flags` set (on) or cleared in the
    // objective's flags.
    namespace calls
    {
        inline void setObjectiveFlags(Objective objective, ee::Ptr<uint32_t> flags, uint32_t on)
        {
            zge::setObjectiveFlags(objective, flags, on);
        }
    }
    // clearObjectiveFrameMarks: the objective's marks of what happened to it
    // this frame cleared (kObjectiveActivatedNow and the others).
    namespace calls
    {
        inline void clearObjectiveFrameMarks(Objective objective)
        {
            zge::clearObjectiveFrameMarks(objective);
        }
    }

    // ObjectiveInfo::flags, the bits the manager reads beyond the script's
    // PARAMETERS switches. Scaffolding (address-call): the engine's own
    // enum once its header is on this line.
    constexpr uint32_t kObjectivePersistent = 1u << 0;     // PERSISTENT: one of the level's goals
    constexpr uint32_t kObjectiveHighlight = 1u << 3;      // HIGHLIGHT
    constexpr uint32_t kObjectiveActivatedNow = 1u << 5;   // activated this frame
    constexpr uint32_t kObjectiveCompletedNow = 1u << 6;   // completed this frame
    constexpr uint32_t kObjectiveTimedOutByScript = 1u << 7; // another objective's script block timed this one out, this frame
    constexpr uint32_t kObjectiveIsChild = 1u << 10;       // IS_CHILD: one of a set's objectives

    // ---- The level's goals in the player's saved game ----
    //
    // A character's record of one level is the profile's LevelGoals
    // (profile/players.h): the level's PERSISTENT objectives are its goals,
    // each with a byte of marks at its place among all the level's
    // objectives.
    enum GoalMark : uint8_t
    {
        kGoalSaved = 0x01,        // the objective is a goal: the saved game keeps its marks
        kGoalGap = 0x02,          // a gap (its name starts with GAP)
        kGoalActive = 0x04,       // it was activated: a turn's start activates it again when it has no time limit
        kGoalDone = 0x08,         // completed
        kGoalDoneThisTurn = 0x10, // completed in this turn
        kGoalRewardSlot = 0x60,   // 1 .. 3: which of the level's three slot rewards completing it gives (0: none)
    };
    constexpr uint32_t kGoalRewardSlotShift = 5;
    constexpr uint32_t kLevelSlotRewards = 3;

    // A reward of the game's reward tables (giveReward gives one to a
    // player; the reward display shows it).
    struct Reward
    {
        uint16_t kind;  // +0x00 0: none
        uint16_t value; // +0x02 by kind
    };
    static_assert(sizeof(Reward) == 0x04);
    // Reward::kind, named by the reward texts ("KSTRSP_REWARD_<KIND>..").
    // The numbers between and after have no text and give nothing.
    enum RewardKind : uint16_t
    {
        kRewardKindAttribute = 1,
        kRewardKindRespect = 2,
        kRewardKindTrick = 3,
        kRewardKindTrick02 = 4,
        kRewardKindBike = 5,          // value: the bike
        kRewardKindLook = 6,
        kRewardKindCharacter = 7,     // value: the rider character
        kRewardKindTeam = 8,
        kRewardKindLevel = 9,         // value: the level
        kRewardKindContestLevel = 10,
        kRewardKindEditTheme = 11,    // value: the park editor's theme
        kRewardKindEditPiece = 12,
        kRewardKindTip = 14,
        kRewardKindCharacterTip = 15,
        kRewardKindMovie = 16,        // value: the movie
        kRewardKindCutscene = 17,     // value: the level
        kRewardKindCode = 18,
        kRewardKindFeature = 19,      // value: the feature's bit
    };
    // The reward put into the saved game for the player (objective_rewards.cpp).
    void giveReward(uint32_t player, ee::Ptr<Reward> reward);
    // The reward display's line for a reward written to `text`; 0 when the
    // reward's kind has no text.
    int32_t rewardText(uint32_t player, ee::Ptr<Reward> reward, ee::Ptr<char> text);

    // The level's objectives by their place in its file: what the saved
    // marks and the overlay name an objective by.
    struct GoalSlot
    {
        uint32_t nameHash;   // +0x00 the objective's name, hashed
        Objective objective; // +0x04
    };
    static_assert(sizeof(GoalSlot) == 0x08);
    inline ee::Global<GoalSlot[kLevelObjectives], sym::g_goalSlots> g_goalSlots;
    constexpr int32_t kNoGoal = -1;

    // What the manager's visits of every objective carry (forEachObjective's
    // data): the place of the objective being visited among the level's,
    // counted up by each visit, and whose goals.
    struct GoalRegistration // loadLevelObjectives to registerGoal
    {
        uint32_t goal;             // +0x00
        ee::Ptr<LevelGoals> goals; // +0x04
    };
    static_assert(sizeof(GoalRegistration) == 0x08);
    struct GoalRestore // resetTurnGoals to restoreGoal and markActiveGoal
    {
        ee::Ptr<LevelGoals> goals; // +0x00
        uint32_t goal;             // +0x04
    };
    static_assert(sizeof(GoalRestore) == 0x08);

    // ---- The rider's objective words ----
    //
    // The words the rider keeps for the objectives, just before its wheels'
    // contacts. Scaffolding (unnamed-field): the rider lane's header has
    // these bytes as padding still; this is the objectives' view of them.
    struct RiderObjectives
    {
        uint64_t turnAwards;       // +0x00 bits the game's end-of-turn code sets as it gives the turn's rewards (not read further here)
        uint32_t _08;
        Objective timedObjective;  // +0x0C the objective with a time limit that runs for the player (0: none)
        uint32_t ticksLeft;        // +0x10 its time left, in frames
        uint32_t _14;
    };
    static_assert(offsetof(RiderObjectives, timedObjective) == 0x0C && sizeof(RiderObjectives) == 0x18);
    inline ee::Ptr<RiderObjectives> riderObjectives(ee::Ptr<rider::Rider> rider)
    {
        return (ee::Ptr<rider::WheelContact>(&rider->wheels[0]).cast<uint8_t>() - sizeof(RiderObjectives)).cast<RiderObjectives>();
    }
    static_assert(offsetof(rider::Rider, wheels) - sizeof(RiderObjectives) == 0x2588);

    // ---- The events the manager sends ----

    // A rider's trick on a surface started or ended (the game's trickEvent
    // tells it): with `started`, what each wheel is on.
    constexpr uint32_t kTrickEvent = 2;
    enum TrickSurfaceSort : uint32_t
    {
        kTrickOnFace = 0,   // `surface` is the face's tag word
        kTrickOnEdge = 1,   // the edge's tag word
        kTrickOnObject = 2, // the name hash of the object the wheel is on
    };
    struct TrickEvent
    {
        uint32_t player;     // +0x00
        uint32_t kind;       // +0x04 kTrickEvent
        uint32_t started;    // +0x08
        uint32_t surface[2]; // +0x0C per wheel (left as they were when the trick ends)
        uint32_t sort[2];    // +0x14 TrickSurfaceSort
    };
    static_assert(offsetof(TrickEvent, surface) == 0x0C && offsetof(TrickEvent, sort) == 0x14 && sizeof(TrickEvent) == sizeof(ObjectiveEvent));

    // ---- The overlay's displays the objectives drive (overlay/displays.h:
    // kOverlayTimer, kOverlayObjective, kOverlayReward) ----

    // What the objective display is sent and answers when asked its state:
    // which of the level's objectives a player's display shows.
    constexpr uint32_t kNoGoalShown = 0xFFFFFFFFu;
    struct ObjectiveDisplayMessage
    {
        uint8_t player;      // +0x00
        uint8_t pad01;       // padding: no reader (int25: the senders never set it; only whole-message copies move it)
        uint16_t visible;    // +0x02 (state only) whether the display's widget shows
        uint32_t goal;       // +0x04 the objective's place in the level's file, or kNoGoalShown
        Objective objective; // +0x08 (state only) the objective shown
    };
    static_assert(offsetof(ObjectiveDisplayMessage, goal) == 0x04 && sizeof(ObjectiveDisplayMessage) == 0x0C);

    // What the reward display is sent: a line for a player.
    enum RewardLine : uint8_t
    {
        kRewardText = 0,              // `what` is a text
        kRewardObjectiveDone = 1,     // `what` is the objective's info
        kRewardObjectiveTimedOut = 2, // `what` is the objective's info
        kRewardAllGoalsDone = 3,      // no `what`
        kRewardGiven = 4,             // `what` is the Reward
        kRewardGapDone = 5,           // `what` is the objective's info
        kRewardAllGapsDone = 6,       // no `what`
    };
    struct RewardMessage
    {
        uint8_t player; // +0x00
        uint8_t line;   // +0x01 RewardLine
        uint8_t _02[2];
        uint32_t what;  // +0x04 by line
    };
    static_assert(offsetof(RewardMessage, what) == 0x04 && sizeof(RewardMessage) == 0x08);

    // The frame's visit of every objective for a player (updatePlayerObjectives
    // to updateGoal).
    struct PlayerGoalVisit
    {
        uint32_t player;               // +0x00
        uint32_t goal;                 // +0x04 the visited objective's place among the level's
        ObjectiveDisplayMessage shown; // +0x08 the player's objective display: its state, and what the visit sends it
        uint32_t soundAfterVisit;      // +0x14 set: kGoalVisitSound plays after the visit. Cleared before it, and set
                                       //       by nothing (updateGoal never writes it, on PS2 or GameCube)
    };
    static_assert(offsetof(PlayerGoalVisit, shown) == 0x08 && offsetof(PlayerGoalVisit, soundAfterVisit) == 0x14 && sizeof(PlayerGoalVisit) == 0x18);

    // ---- Other modules' by address ----
    // Scaffolding (address-call).
    //
    // ZGE4's own lookup of an animation by its name (the name hashed, then
    // the lookup by hash): the animation, or 0. Not AI's findAnimation,
    // which answers through a second word.
    namespace calls
    {
        inline ee::Ptr<zge::Motion> findAnimationNamed(ee::Ptr<const char> name)
        {
            return ee::game<ee::Ptr<zge::Motion>>(sym::findAnimationNamed, name);
        }
    }
    // ZGE4's remakeObjectAnimator: the object gets a new animator with a
    // channel for each of its bones (what the old one played is carried
    // over); the animator.
    namespace calls
    {
        inline ee::Address remakeObjectAnimator(ee::Ptr<zge::SceneObject> object)
        {
            return ee::game<ee::Address>(sym::remakeObjectAnimator, object);
        }
    }

    // ---- The manager (objectives.cpp) ----

    // The game's set-up and shut-down: the engine's module, the types, the
    // profiler's bar.
    void setUpObjectives();
    void shutDownObjectives();
    // A level's objectives read from its file and freed again.
    void loadLevelObjectives();
    void freeLevelObjectives();
    // The two icon models and their animations.
    void loadObjectiveIcons();
    void freeObjectiveIcons();
    // A player's frame, and the frame's highlights.
    void updatePlayerObjectives(uint8_t player);
    void drawObjectiveHighlights();
    // The events.
    void sendRecordEvent(uint8_t player, uint32_t record, ee::Float value);
    void sendTrickEvent(uint8_t player, uint32_t started);
    uint32_t sendObjectActivatedEvent(uint8_t player, ee::Ptr<zge::SceneObject> object, ee::Ptr<zge::EngineMessage> activation);
    // An objective activated for a player, and whether it may be.
    void activatePlayerObjectiveByName(uint8_t player, ee::Ptr<const char> name);
    void activatePlayerObjective(uint32_t player, Objective objective);
    uint32_t playerCanActivateObjectiveByName(uint8_t player, ee::Ptr<const char> name);
    uint32_t playerCanActivateObjective(uint8_t player, Objective objective);
    // The goals.
    uint32_t objectiveSavedMark(uint8_t player, Objective objective);
    void resetTurnGoals(uint8_t player);
    void clearPlayerObjective(uint8_t player);
    void unusedTurnStartHook();
    uint32_t playerHasTurnAwards(uint8_t player);
    uint32_t goalNameHash(uint32_t player, uint32_t goal);
    int32_t goalIndexOf(uint32_t player, uint32_t nameHash);
}
