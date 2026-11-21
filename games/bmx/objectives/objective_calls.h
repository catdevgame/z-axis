#pragma once
// What BMX XXX's objectives code (games/bmx/objectives) shares: the engine's
// objectives module as the game calls it, the records the two sides hand
// each other, and the script blocks' readers.
//
// The engine's side is ZGE4's objectives module (zge/scene/objectives.h):
// an objective is a record read from the level's objectives file; what kind
// of objective it is (its TYPE) is one of the types the game registers
// here, each a set of callbacks. The game calls the module, the script
// blocks (zge/text/script_blocks.h) and their values (script_values.h) by
// their names in zge.

#include "bmx/game/player_riders.h"
#include "bmx/game/trick_types.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/memory/pool.h"
#include "zge/scene/message.h"
#include "zge/scene/objectives.h"
#include "zge/scene/scene.h"
#include "zge/text/script_blocks.h"
#include "zge/text/script_values.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/objective_calls.h.h"

namespace game::bmx::objectives
{
    using Objective = ee::Ptr<zge::Objective>;

    // An objective's state, as the game's code reads and sets it.
    enum ObjectiveState : uint32_t
    {
        kObjectiveActive = 2,     // waiting for the rider to start on it
        kObjectiveInProgress = 3, // a type's frame check answered that the rider is on it
        kObjectiveDone = 4,       // the type's own code saw it done: the engine completes it on its next frame
        kObjectiveComplete = 5,
    };

    // What the game is told (events) and each frame's update carry to every
    // objective's type: who, and what happened. The words after `kind` are
    // the kind's own.
    struct ObjectiveEvent
    {
        uint32_t player;  // +0x00
        uint32_t kind;    // +0x04 ObjectiveEventKind
        uint32_t words[5]; // +0x08 .. +0x1B by kind (see the senders, objectives.cpp)
    };
    static_assert(sizeof(ObjectiveEvent) == 0x1C);
    enum ObjectiveEventKind : uint32_t
    {
        kFrameEvent = 0, // the frame's update: no words
    };

    // A rider activated a level object (the game's riderActivatedObject asks
    // the objectives before the object gets the activation).
    constexpr uint32_t kObjectActivatedEvent = 3;
    struct ObjectActivatedEvent
    {
        uint32_t player;                           // +0x00
        uint32_t kind;                             // +0x04 kObjectActivatedEvent
        ee::Ptr<zge::SceneObject> object;          // +0x08 the object activated
        ee::Ptr<zge::EngineMessage> activation;    // +0x0C the activation message it would get
        uint32_t taken;                            // +0x10 set by an objective the object belongs to: the game leaves the object alone
        uint32_t delivered;                        // +0x14 set by an objective that sent the activation on itself
        uint32_t _18;
    };
    static_assert(sizeof(ObjectActivatedEvent) == sizeof(ObjectiveEvent) && offsetof(ObjectActivatedEvent, delivered) == 0x14);

    // The messages BMX XXX's level objects take (the level lane's
    // entity_messages.h, its names and numbers) and an activation's data.
    // Scaffolding (address-call): the level lane's header once it is on this line.
    constexpr uint32_t kEntityActivateMessage = 0x100;
    constexpr uint32_t kEntityEnableMessage = 0x102;
    constexpr uint32_t kEntityDisableMessage = 0x103;
    struct EntityActivation
    {
        uint32_t quiet;  // +0x00 1 from the objectives' own activations: an object activated with it set skips its activation's animation and sound (DM2's ActivationData's name)
        uint32_t player; // +0x04
        uint8_t _08[0x18];
    };
    static_assert(sizeof(EntityActivation) == 0x20);

    // A type's name and callbacks (the engine's zge::ObjectiveTypeSetup), as each type registers them
    // (zge::registerObjectiveType); a type leaves out (0) what it does not need. Each callback is named for when
    // the engine calls it: readParameters the TYPE line read (the type's own block may follow); freeData the
    // objective freed, or given another type; resetProgress its progress cleared, on activating and on a reset;
    // onEvent a game event (an ObjectiveEvent) while it is active or in progress; onActivate (objective, 0) on
    // activating, (objective, 1) when a set makes it active; onComplete (objective, 0) on completing, (objective,
    // 1) on a reset that leaves it complete; drawForRider the game's per-rider visit of an objective in progress
    // whose script says HIGHLIGHT; moveBounds each frame before inProgress, for one whose script says MOVING;
    // inProgress each frame while it runs, non-zero while the rider is on it; statusText its status line.
    using zge::ObjectiveTypeSetup;

    // The objective's state (zge::objectiveState, setObjectiveState); the
    // original's tail jump to its setter:
    namespace calls
    {
        inline void setObjectiveStateTail(Objective objective, uint32_t state)
        {
            ee::tailCall<void>(sym::setObjectiveState, objective, state);
        }
    }

    // The type's own record of an objective: one word the engine keeps for it
    // (zge::objectiveData, setObjectiveData); the original's tail jump to its
    // setter:
    namespace calls
    {
        inline void setObjectiveDataTail(Objective objective, ee::Address data)
        {
            ee::tailCall<void>(sym::setObjectiveData, objective, data);
        }
    }


    // resetObjective: one objective reset as the engine's reset of them all
    // does it: its state from the script again and its type's progress
    // cleared (one that stays complete has its type's `completed` told
    // with 1 instead).
    namespace calls
    {
        inline void resetObjective(Objective objective)
        {
            ee::game<void>(sym::resetObjective, objective);
        }
    }

    // What zge::objectiveInfo answers: one record the engine keeps, filled for
    // the objective asked about last (its flags: the script's PARAMETERS
    // switches).
    using zge::ObjectiveInfo;

    // ---- The rider's trick lines (the rider's score block, rider.h) ----
    using rider::kScoreTrickAlleyOop;
    using rider::ScoreLine;
    using rider::ScoreTrick;

    // A trick's type by its number, as the scripts name them (TRICK_TYPE):
    // game/trick_types.h's TrickType and trickType.

    // The game tells a record's new value (the game's records, in the
    // order of the RECORD type's words).
    constexpr uint32_t kRecordEvent = 1;
    struct RecordEvent
    {
        uint32_t player;  // +0x00
        uint32_t kind;    // +0x04 kRecordEvent
        uint32_t record;  // +0x08 which record
        ee::Float value;  // +0x0C
        uint8_t _10[0xC];
    };
    static_assert(offsetof(RecordEvent, value) == 0x0C && sizeof(RecordEvent) == sizeof(ObjectiveEvent));
    constexpr uint32_t kStackScoreRecord = 2; // SCORE_STACK

    // Whose turn it is (profile/players.h's currentPlayer), and the player's
    // rider (game/player_riders.h's riderIndexOf, the rider module's riderOf).

    // ---- The engine and libc by address ----
    // Scaffolding (address-call).
    namespace calls
    {
        inline void memset(ee::Address dst, int32_t byte, uint32_t bytes)
        {
            ee::game<void>(sym::memset, dst, byte, bytes);
        }
    }

    // ---- The manager's own, for the types (objectives.cpp, the highlights) ----
    //
    // growBounds: the box grown to hold the point.
    void growBounds(ee::Ptr<zge::Box> bounds, ee::Address point); // (objectives.cpp)
    // The SET type's (set_type.cpp): 1 when the objective is a set that has
    // `wanted` among its members, or among those of a member that is a set.
    uint32_t setContainsObjective(Objective objective, Objective wanted);
    // A face, an edge or an object drawn in `colour` for a rider at `from`:
    // only when its node is drawn and it is within 25 units
    // (objective_highlights.cpp).
    void highlightFace(uint64_t face, ee::Address from, uint32_t colour);
    // A highlight's colour word, 0x00RRGGBB (objective_highlights.cpp fades it with the distance).
    constexpr uint32_t highlightColour(uint32_t red, uint32_t green, uint32_t blue) { return (red << 16) | (green << 8) | blue; }
    void highlightEdge(uint64_t edge, ee::Address from, uint32_t colour);
    void highlightObject(ee::Ptr<zge::SceneObject> object, ee::Address from, uint32_t colour);

    // ---- The script's keyword blocks (the engine's block reader) ----
    //
    // A block class is a pair of keywords (X_BEGIN .. X_END) with the keys
    // its lines may have; the reader gathers a block's lines as keyed values
    // and calls the class's handler at its end. A type's readParameters
    // registers its own block class under the objective's block, with the
    // objective as the class's owner; the handler reads the values, makes
    // the type's record and unregisters the class.
    // The engine's zge::BlockClassSetup: the two keywords, the block it is read inside, the keys (the game's are
    // all text, kind 0) and the handler called at the block's end (the game's give no onBegin). The class's
    // owner (zge::setBlockClassOwner, blockClassOwner) is the objective.
    using zge::BlockClassSetup;

    // The values of the block just read (zge::blockClassValues; made empty when it has none), and one of them by
    // its key (zge::findScriptValue; 0: the block had no such line).
    using BlockValues = zge::ScriptValueSet;
    using BlockValue = zge::ScriptValue;

    // A value as a text, an integer, a float or a switch: zge::scriptValueText, scriptValueInteger,
    // scriptValueFloat, scriptValueSwitch.
}
