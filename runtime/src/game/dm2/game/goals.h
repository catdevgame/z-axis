#pragma once
// DM2's level goals (game.c): the things a level asks of the rider, tier by
// tier, and what the game draws for them.

#include "players.h"
#include "ee/ee.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/game/goals.h.h"

namespace zge
{
    struct SceneObject;
}

namespace game::dm2
{
    // The level goals' definitions (game.c's, g_goalDefinitions): eight
    // levels of five challenges of four goals. A goal lists up to four
    // objectives ("goalLevel, goalNum, objctvNum" in the Xbox build's
    // messages), each of a kind its check is for, with the names (hashed
    // once: initLevelGoals) and values that check compares.
    // Scaffolding (unnamed-field): what the kinds' words and the rest of a
    // definition mean is the goals' checks' (goalsFrame and the goal...
    // functions), not read yet; here only the respect a goal earns and the
    // words a kind keeps a name in are known.
    // The objective kinds (GoalObjective::kind) by what their checks ask (goalsFrame; goalTouch for the
    // touches). The words of each:
    //   tricks (0 .. 3): the trick, its motion, the line's spin in half turns at least, the face's or edge's
    //     tag or the object's name (1 .. 3);
    //   4: the air tricks the turn's lines hold, at least;
    //   actions (5 .. 7): the rider's action state, the face's tag or the object's name (6, 7);
    //   runs (8 .. 10): the edge's tag, the object's name or the face's tag, the turn's score of the kind at
    //     least, whether the line's first trick is to be ridden fakie (8, 9) or be the nose wheelie (10),
    //     its motion;
    //   off an object (11, 12): the object's name, -, the trick the turn's lines are to hold (12);
    //   touches (13, 23): the object's name.
    enum ObjectiveKind : uint32_t
    {
        kObjectiveTrick = 0,
        kObjectiveTrickOnFace = 1,
        kObjectiveTrickOnObject = 2,
        kObjectiveTrickOnEdge = 3,
        kObjectiveAirTricks = 4,
        kObjectiveAction = 5,
        kObjectiveActionOnFace = 6,
        kObjectiveActionOnObject = 7,
        kObjectiveGrindOnEdge = 8,
        kObjectiveGrindOnObject = 9,
        kObjectiveWheelieOnFace = 10,
        kObjectiveAirOffObject = 11,
        kObjectiveTrickOffObject = 12,
        kObjectiveTouchObject = 13,
        // A transfer: in one line, from a face of the first name to one of the second (goalObjectTrick checks a
        // landing's two contacts, trickEvent's, against the two names). The goals' texts name them ("Transfer
        // between two commuter bench awnings": 14; "Pull a 360 tailwhip over the center of the dinosaur
        // skeleton spine": 15, words[2] the trick and [3] the spin in half turns; "Pull a modified backflip
        // between the junkyard trailers": 17, the trick and the motion; "Grind-to-grind transfer between any
        // two logs": 18, edges). A trick or motion word of 0xFF takes any (or none), 0xFE any but none. No
        // goal of the game has kinds 16, 19, 20 or 21: by the checks, the motion and spin (16), objects (19),
        // a face and an edge (20), a face and an object (21).
        kObjectiveTransfer = 14,
        kObjectiveTransferTrickSpin = 15,
        kObjectiveTransferMotionSpin = 16,
        kObjectiveTransferTrickMotion = 17,
        kObjectiveGrindTransfer = 18,
        kObjectiveObjectsTransfer = 19,
        kObjectiveFaceEdgeTransfer = 20,
        kObjectiveFaceObjectTransfer = 21,
        kObjectiveRecord = 22, // a record kind's value passed: the kind (words[0]), the value (words[1])
        kObjectiveTouchNonColliding = 23,
    };

    struct GoalObjective
    {
        uint32_t kind;          // +0x00
        uint32_t words[4];      // +0x04
        // +0x14 the scene entities it activates as its state becomes 5
        // ("Warning: activated entity "%s" not found in scene", the Xbox
        // build's), by name (0: none).
        ee::Ptr<const char> entities[4];
        uint8_t _24[0x30 - 0x24];
        // +0x30 the box the rider is to be in for the objective to show
        // (inObjectiveBox: its least and its most corner; x, y, z and a
        // fourth word nothing reads).
        ee::Float boxMin[4];
        ee::Float boxMax[4];    // +0x40
    };
    static_assert(offsetof(GoalObjective, entities) == 0x14 && sizeof(GoalObjective) == 0x50);
    static_assert(offsetof(GoalObjective, boxMin) == 0x30 && offsetof(GoalObjective, boxMax) == 0x40);
    constexpr uint32_t kGoalObjectives = 4;
    struct GoalDefinition
    {
        uint8_t objectiveCount; // +0x00
        uint8_t _01[3];
        int32_t respect;        // +0x04 what the goal earns
        // +0x08 the scene entities a goal done keeps active: sent the
        // activate message again as the level's goals restart, by name (0:
        // none).
        ee::Ptr<const char> entities[3];
        uint8_t _14[0x20 - 0x14];
        GoalObjective objectives[kGoalObjectives]; // +0x20
    };
    static_assert(offsetof(GoalDefinition, respect) == 0x04 && offsetof(GoalDefinition, entities) == 0x08);
    static_assert(offsetof(GoalDefinition, objectives) == 0x20);
    static_assert(sizeof(GoalDefinition) == 0x160);
    constexpr uint32_t kGoalLevels = 8;
    // The material the goals draw with: initLevelGoals makes it, the
    // objectives' targets are drawn with it (goal_targets.cpp).
    inline ee::Global<ee::Address, sym::g_goalsMaterial> g_goalsMaterial;
    // How many goals each challenge of the session level has.
    inline ee::Global<uint8_t[kLevelChallenges], sym::g_challengeGoalCounts> g_challengeGoalCounts;
    inline ee::Global<GoalDefinition[kGoalLevels][kLevelChallenges][kChallengeGoals], sym::g_goalDefinitions>
        g_goalDefinitions;

    // Each goal and objective of the current challenge has its targets:
    // the faces, edges or objects its names stand for (findObjectiveTargets
    // finds them as the challenge starts), a list for each of its two
    // names, which its box is fitted round (fitObjectiveBox) and which are
    // drawn (drawGoalTargets). An entry is a FaceRef, an EdgeRef or an
    // object's address (its low word), by the objective's kind.
    constexpr uint32_t kObjectiveTargetsMost = 0x80;
    struct TargetList
    {
        uint64_t targets[kObjectiveTargetsMost];
        uint32_t count;          // +0x400
        uint8_t _404[4];
    };
    struct ObjectiveTargets
    {
        TargetList named[2];     // by the objective's first and second name
    };
    static_assert(sizeof(TargetList) == 0x408 && sizeof(ObjectiveTargets) == 0x810);
    inline ee::Global<ObjectiveTargets[kChallengeGoals][kGoalObjectives], sym::g_objectiveTargets> g_objectiveTargets;

    // The level's goals set up as it starts: the definitions' objective
    // names made hashes (once a run), the goals' material made, and every
    // objective's record cleared.
    void initLevelGoals();

    // The material the goals draw with (made as the level's goals are set
    // up) given up; the engine's answer.
    int32_t releaseGoalsMaterial();
    // A player's rider touched an object (a trigger, a goal's target): the
    // level's goals are told (the rider's and the bodies' touch call it).
    // Whether it met any objective (goalTouch's answer).
    uint32_t gameTouchHandler(uint32_t player, ee::Ptr<zge::SceneObject> object);
    // The touch checked against the open objectives of the player's current
    // challenge in a ProQuest level (kinds 13 and 23: the named object
    // touched): each one met is set to state 4 (6 when its goal was not
    // given yet) and the object is sent the activate message (0x100) from
    // the player's rider. 1 when any was met.
    uint32_t goalTouch(uint32_t player, ee::Ptr<zge::SceneObject> object);
    // Whether the player's turn earned its challenge's reward: in a ProQuest
    // level, the challenge done with a goal its NPC gave; in a competition,
    // any of the turn's reward bits (the rider's goal flags but the gaps').
    // 0 in any other game.
    uint32_t challengeRewardEarned(uint32_t player);

    // An objective's name as the goals compare it (bmxobjctvGenerateHash, the
    // Xbox build's name): the name's hash, 0 for no name (the Xbox build
    // warns "null string passed to bmxobjctvGenerateHash()"; this one does
    // not).
    uint32_t bmxobjctvGenerateHash(ee::Ptr<const char> name);

    // A goal's objectives (the parts the goal definition lists; "goalNum,
    // objctvNum" in the Xbox build's messages), as the player's level
    // goals keep them: four bits each in the ChallengeGoal's parts, a state
    // (0 .. 7) in the low three and a mark in the fourth. Of the player's
    // current challenge in the session level.
    uint32_t objectiveState(uint32_t player, uint32_t goal, uint32_t objective);
    // The objective's mark, masked by `mask` (bit 0 of it).
    uint32_t objectiveMarked(uint32_t player, uint32_t goal, uint32_t objective, uint32_t mask);
    // An NPC's goal of the player's current challenge made the player's
    // (the NPC screen's accept): the goal shown and kGoalGiven (unless shown
    // already); its objectives below state 4 set to 1 and those in state 6 to
    // 4; the other goals' objectives in states 1 .. 3 back to 1; the objective
    // label hidden. Goals done are left as they are.
    void giveGoal(uint32_t player, uint32_t goal);
    // The objective's state set (its mark cleared with it). Leaving state 2
    // for one past it hides the objective label; state 5 activates its
    // entities (each found in the scene is sent the activate message from
    // the player's rider).
    void setObjectiveState(uint32_t player, uint32_t goal, uint32_t objective, uint32_t state);
    // The player's goals of the session level made ready for another try
    // (a rider placed again, the game saved, the level left): the challenge
    // is the first not done (at most the last); each goal not done loses
    // its objectives' progress (states back to 0 or 1, marked, none
    // counted met) and its kGoalGiven; each goal done has its objectives' and
    // its own entities activated again. Then the challenge's objective
    // records are filled (findObjectiveTargets, fitObjectiveBox) and its goals given: the
    // first challenge's first, a middle challenge's all but the last, none
    // of the last. (Row named from this use: no other build names it.)
    void restartLevelGoals(uint32_t player);
    // The level goals' frame for a player in a ProQuest level: each open
    // objective of the current challenge, started (its goal given) and not
    // met, is checked against what the rider is doing (its kind: a trick, an
    // action, a grind, a wheelie, the air off an object, on a tagged face or
    // edge or a named object); one met at once goes to state 4, one done in
    // the trick line to 3. Then each started objective shows its label while
    // the rider is in its box (state 2, else 1), and each met one is counted
    // (state 5) into its goal: the goal done at its last (the reward shown,
    // its entities activated, its respect earned).
    void goalsFrame(uint32_t player);
    // An objective's targets found as its challenge starts (g_objectiveTargets):
    // the level's faces tagged with its name (by the face's tag word), its
    // edges (the edge's tag word) or its named objects, by its kind, for its
    // first name and, for the gap kinds, its second; at most 128 of each.
    // Bug kept: the face-and-edge gap (kind 20) looks its edges up by two
    // tags, its second name and the word after it, which is never set.
    void findObjectiveTargets(uint32_t challenge, uint32_t goal, uint32_t objective);
    // An objective's box (GoalObjective::boxMin, boxMax) grown to hold a
    // point (x, y and z).
    void growObjectiveBox(GoalObjective *objective, const ee::Float *point);
    // An objective's box fitted round its targets (the faces' centres, the
    // edges' ends, the objects' positions its kind uses), 20 more on every
    // side; with no targets it stays the empty box (99999.9 .. -99999.9)
    // grown by 20.
    void fitObjectiveBox(uint32_t challenge, uint32_t goal, uint32_t objective);
    // The targets of the open objectives of the current challenge whose
    // label is up (state 2), each drawn in its challenge's colour when its
    // node was listed by the scene's last draw and it is within 25 of the
    // rider: a face by its centre, an edge by its middle, an object by its
    // position (goal_targets.cpp). With the debug draw setting, the rider's
    // own contact and the edges about it too. The draws are counted.
    void drawFaceTargetNear(uint64_t face, const ee::Float *riderPosition, uint32_t colour);
    void drawEdgeTargetNear(uint64_t edge, const ee::Float *riderPosition, uint32_t colour);
    void drawObjectTargetNear(uint32_t object, const ee::Float *riderPosition, uint32_t colour);
    void drawGoalTargets();
    // A trick line's end (trickEvent: landed, or 0 for a bail): the gap
    // objectives' landings and the objectives done in the line (goals.cpp).
    void goalObjectTrick(uint32_t player, uint32_t landed);
    // A value the player reached in one of the record kinds (checkGameRecord):
    // each open record objective (kind 22) of the player's current challenge
    // in a ProQuest level for that kind, started and not met, is met (state
    // 4) when the value passes the objective's (a count or points for kinds
    // 0 .. 5, a measure from kind 6 on, compared as floats).
    void goalRecord(uint32_t player, uint32_t kind, uint32_t value);
    // Whether the player's rider is in the objective's box (kinds 0, 4, 5,
    // 22 and 24 have none: always 1).
    uint32_t inObjectiveBox(uint32_t player, uint32_t goal, uint32_t objective);
    // `bits` (bit 0) set in the objective's mark when `on`, else cleared.
    void setObjectiveMark(uint32_t player, uint32_t goal, uint32_t objective, uint32_t bits, uint32_t on);
}
