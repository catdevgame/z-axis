#pragma once

// An Aggressive Inline level script's goals for the panel, read at build
// time from the level's .BIN (games/aggressive/script/script.h
// has the format): the objective table of its data block and, by reading
// the handlers, what each goal counts and which objects it wants done (the
// tooling's panel_goals.py, the reference). Nothing here runs in the game.
//
// A goal's handler ends in setObjectiveState(complete, index). From there:
// - the counter: the `load v; sub N` test before the call, N the goal's
//   target, v the handler's object's variable. The Xbox release's scripts
//   complete some goals later, in another object's handler (a reward
//   animation's syncStart): there the counter is the test before the
//   counting object's SEND to that object, the counting object being one
//   the completing handler moves to another state;
// - the score: what the value tested before the call is made of (the
//   skater's score, the running line's points, a timed run's points), read
//   from the handler's stores;
// - the targets: the objects that send the counting handler's event (a bike
//   rack's grindStart sends bikeRackGrind to the helper that counts them);
//   with no sender, the handler's object itself;
// - a target is done when its handler moved it to another state, or set its
//   own first-time flag (`push 1; store v`).

#include "panel_maps_parts.h"

namespace zx::app::extras::panel_maps
{
    struct ScriptOp
    {
        uint32_t op = 0, arg = 0;
        bool operator==(const ScriptOp &o) const { return op == o.op && arg == o.arg; }
    };

    // One event handler: the object, its state and the event it answers.
    struct ScriptHandler
    {
        uint32_t object = 0, state = 0, event = 0;
        std::vector<ScriptOp> code;
    };

    // A row of the script's objective table (Objective, 0x2C bytes).
    struct Objective
    {
        uint32_t index = 0, locked = 0, tier = 0, points = 0, state = 0, type = 0, owner = 0;
        std::string title, text;
        uint32_t titleAt = 0, textAt = 0; // in the script's strings
    };

    class LevelScript
    {
    public:
        explicit LevelScript(ByteView data);
        std::string string(uint32_t offset) const;
        std::vector<Objective> objectives() const;
        // The word an object keeps its state in (its variable k at + 1 + 2k).
        uint32_t stateVariable(uint32_t object) const;

        uint32_t variableCount = 0;
        std::vector<ScriptHandler> handlers;

    private:
        ByteView m_strings, m_data;
        std::unordered_map<uint32_t, uint32_t> m_objects;
    };

    // How a target is done: the script word and the value it holds then.
    struct DoneTest
    {
        int64_t word = 0;
        uint32_t value = 0;
    };

    // GoalRecord.scoreKind: the skater's score, the running line's points, a
    // timed run's points, the score with the running line's points.
    enum ScoreKind : int64_t
    {
        ScoreNone,
        ScoreTotal,
        ScoreLine,
        ScoreChallenge,
        ScoreWithLine,
    };

    struct ScriptGoal
    {
        Objective objective;
        std::optional<std::pair<int64_t, int64_t>> counter; // (script word, target)
        std::optional<std::pair<int64_t, int64_t>> score;   // (kind, target)
        // The objects to do, in the order found, each with how it is done.
        std::vector<std::pair<uint32_t, std::optional<DoneTest>>> targets;
    };
    std::vector<ScriptGoal> analyseGoals(const LevelScript &script);

    // An NPC's challenge bubble: it shows while the handler's object's state
    // word holds the handler's state.
    struct NpcBubble
    {
        uint32_t npc = 0, word = 0, state = 0;
    };
    std::vector<NpcBubble> npcBubbles(const LevelScript &script);
}
