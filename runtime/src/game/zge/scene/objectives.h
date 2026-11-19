#pragma once

// ZGE4's objectives (objectives.cpp; BMX XXX only: ZGE2 and ZGE3 have no such module). An objective is read from
// a script block (OBJECTIVE_BEGIN .. OBJECTIVE_END: its NAME and TYPE, then PARAMETERS, PREREQUISITE, ACTIVATE,
// COMPLETE and TIMEOUT blocks) into a record of the objectives' pool; the game finds one by its name, asks what it
// is and whether it can be activated.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "zge/math/box.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct Objective;

    constexpr uint32_t kObjectiveNameBytes = 0x20;
    constexpr uint32_t kObjectivePrerequisites = 4;

    // What a game registers for an objective type (registerObjectiveType; 0x2C bytes): its name and the type's
    // hooks, each 0 for none. The slot meanings are the callers' (the module's wrappers) and the game's types'.
    struct Objective;
    using ObjectiveHook = ee::FunctionPtr<uint32_t(ee::Ptr<Objective> objective)>;
    using ObjectiveHookWith = ee::FunctionPtr<uint32_t(ee::Ptr<Objective> objective, ee::Address word)>;
    struct ObjectiveTypeHooks
    {
        ObjectiveHookWith readParameters; // (objective, the parent block's keyword): the TYPE reader, once the type is set
        ObjectiveHook freeData;           // before the objective's type is replaced: the type's own record freed
        ObjectiveHook resetProgress;      // the objective activated or reset
        ObjectiveHookWith onEvent;        // (objective, the game's event record)
        ObjectiveHookWith onActivate;     // (objective, 0) when activated, (objective, 1) when made active again
        ObjectiveHookWith onComplete;     // (objective, 0) when completed, (objective, 1) from the reset
        ObjectiveHookWith drawForRider;   // (objective, word): only for a HIGHLIGHT objective
        ObjectiveHook moveBounds;         // a MOVING objective's bounds worked out again
        ObjectiveHookWith inProgress;     // (objective, the frame's event) each frame: non-zero while the objective is being done
        ObjectiveHookWith statusText;     // (objective, text): the status line into the emptied text
    };
    static_assert(sizeof(ObjectiveTypeHooks) == 0x28);
    struct ObjectiveTypeSetup
    {
        ee::Ptr<const char> name;         // +0x00
        ObjectiveTypeHooks hooks;         // +0x04
    };
    static_assert(sizeof(ObjectiveTypeSetup) == 0x2C);

    constexpr uint32_t kObjectiveTypeNameBytes = 0x20;
    constexpr uint32_t kObjectiveTypes = 32;
    // A registered type (0x4C bytes, in the module's table): the name copied, its hash, the hooks.
    struct ObjectiveType
    {
        char name[kObjectiveTypeNameBytes]; // +0x00
        uint32_t nameHash;                  // +0x20
        ObjectiveTypeHooks hooks;           // +0x24
    };
    static_assert(sizeof(ObjectiveType) == 0x4C && offsetof(ObjectiveType, hooks) == 0x24);

    struct SceneObject;

    // What a cue is (ObjectiveGameHooks::cue's first word).
    enum ObjectiveCue : uint32_t
    {
        kObjectiveCueCutScene = 0, // (the cut scene's name hash, quiet: played to its end at once)
        kObjectiveCueSound = 1,    // (the sound's name hash, 0)
    };

    // The two functions the game gives the module at set-up (initObjectiveSystem): what an objective's script
    // blocks (ACTIVATE, COMPLETE, TIMEOUT) do in the game. The module has its own for a 0 (a cut scene played,
    // nothing done to an object). `quiet` is 1 when the block runs as the objective is reset (resetObjective,
    // completeObjective's): a cut scene is played to its end at once, a sound is not cued.
    using ObjectiveCueHook = ee::FunctionPtr<uint32_t(uint32_t cue, uint32_t nameHash, uint32_t quiet)>;
    using ObjectiveObjectActionHook = ee::FunctionPtr<uint32_t(ee::Ptr<SceneObject> object, ee::Ptr<char> action, uint32_t quiet)>;
    struct ObjectiveGameHooks
    {
        ObjectiveCueHook cue;                   // +0x00 a block's cut scene (ANIMSCRIPT) or sound (AUDIO)
        ObjectiveObjectActionHook objectAction; // +0x04 a block's action on a level object (its ENTITY blocks)
    };
    static_assert(sizeof(ObjectiveGameHooks) == 0x08);

    // The module's state: the objectives' pool, then the registered types, then the game's two hooks.
    struct ObjectiveSystem
    {
        ee::Ptr<struct Pool> objectives;       // +0x000 0 before the module's set-up and after its shut-down
        uint32_t typeCount;                    // +0x004
        ObjectiveType types[kObjectiveTypes];  // +0x008
        ObjectiveGameHooks gameHooks;          // +0x988 [unplaced: the set-up's hooks, or the module's defaults]
    };
    static_assert(offsetof(ObjectiveSystem, types) == 0x8 && offsetof(ObjectiveSystem, gameHooks) == 0x988);

    // An objective's state word. Only the states this file's code reads or writes are named; the script's
    // OBJECTIVE block leaves a new objective active.
    enum ObjectiveState : uint32_t
    {
        kObjectiveActive = 2,
        kObjectiveInProgress = 3, // its type's inProgress hook answers non-zero (the frame's visit)
        kObjectiveDone = 4,       // the game's types set it; the frame's visit then completes the objective
        kObjectiveComplete = 5,
    };

    // What one of an objective's three script blocks (ACTIVATE, COMPLETE, TIMEOUT) does when it runs
    // (runObjectiveActions): each ENTITY block's action on a level object, each OBJCTVMSG block's message to
    // another objective, then its cut scene (or, with none, its sound).
    constexpr uint32_t kObjectiveEntityActions = 16;
    constexpr uint32_t kObjectiveMessageActions = 16;
    struct ObjectiveEntityAction
    {
        ee::Ptr<SceneObject> object;           // +0x00 the ENTITY block's NAME, found in the level
        char action[kObjectiveNameBytes];      // +0x04 its MESSAGE: what the game's objectAction hook does to it
    };
    static_assert(sizeof(ObjectiveEntityAction) == 0x24);
    // The messages one objective's block sends another (OBJCTVMSG's MESSAGE: objectiveMessageType).
    enum ObjectiveMessage : uint32_t
    {
        kObjectiveMessageActivate = 0, // "Activate": activated if it is not active yet
        kObjectiveMessageComplete = 1, // "Complete": completed, quietly, if it is not complete yet
        kObjectiveMessageTimeout = 2,  // "Timeout": timed out if it is active and not complete
        kObjectiveMessageUnknown = 3,  // a name that is none of them
    };
    struct ObjectiveMessageAction
    {
        char objective[kObjectiveNameBytes];   // +0x00 the OBJCTVMSG block's NAME: the objective it goes to
        uint32_t message;                      // +0x20 ObjectiveMessage
    };
    static_assert(sizeof(ObjectiveMessageAction) == 0x24);
    struct ObjectiveActions
    {
        uint32_t cutScene;                                         // +0x000 ANIMSCRIPT's name hash, 0: none or not in the level's cut scenes
        uint32_t sound;                                            // +0x004 AUDIO's name hash, 0: none
        uint32_t movie;                                            // +0x008 MOVIE's name hash, 0: none (nothing here plays it)
        uint32_t entityCount;                                      // +0x00C
        ObjectiveEntityAction entities[kObjectiveEntityActions];   // +0x010
        uint32_t messageCount;                                     // +0x250
        ObjectiveMessageAction messages[kObjectiveMessageActions]; // +0x254
    };
    static_assert(sizeof(ObjectiveActions) == 0x494 && offsetof(ObjectiveActions, messageCount) == 0x250);
    // Objective::actions' three blocks.
    enum ObjectiveActionSet : uint32_t
    {
        kObjectiveOnActivate = 0, // ACTIVATE
        kObjectiveOnComplete = 1, // COMPLETE
        kObjectiveOnTimeout = 2,  // TIMEOUT
        kObjectiveActionSets = 3,
    };

    // An objective (0xE70 bytes, cleared when made). The offsets are those of ZGE4's own code: the name and its
    // hash from the NAME reader, the rest of the first words from the OBJECTIVE block's own keys (readObjectiveBlock), the prerequisites from
    // their resolver and check, the state from its accessors, the type from the TYPE reader.
    struct Objective
    {
        uint32_t flags;                                          // +0x000 the OBJECTIVE block's switches (below)
        char name[kObjectiveNameBytes];                          // +0x004 NAME
        uint32_t nameHash;                                       // +0x024 nameHash(name)
        ee::Float timeout;                                       // +0x028 TIMEOUT
        char stringId[kObjectiveNameBytes];                      // +0x02C STRINGID
        char animPreview[kObjectiveNameBytes];                   // +0x04C ANIM_PREVIEW
        ObjectiveActions actions[kObjectiveActionSets];          // +0x06C ACTIVATE, COMPLETE, TIMEOUT (ObjectiveActionSet)
        // +0xE28 the objectives to complete first (0: none there); each holds the PREREQUISITE's name hash until
        // the file's objectives are all read (resolvePrerequisites).
        ee::Ptr<Objective> prerequisites[kObjectivePrerequisites];
        uint32_t state;                                          // +0xE38 ObjectiveState
        uint32_t _E3C;                                           // +0xE3C [unplaced]
        Box bounds;                                              // +0xE40 where it is (the game's types' updateBounds: least and most x, y, z)
        ee::Ptr<ObjectiveType> type;                             // +0xE60 TYPE ("NO OP" when made)
        uint32_t data;                                           // +0xE64 the type's own record (setObjectiveData)
        uint8_t _E68[0xE70 - 0xE68];                             // +0xE68 [unplaced]
    };
    static_assert(sizeof(Objective) == 0xE70 && offsetof(Objective, nameHash) == 0x24 && offsetof(Objective, stringId) == 0x2C &&
                  offsetof(Objective, animPreview) == 0x4C && offsetof(Objective, actions) == 0x6C &&
                  offsetof(Objective, prerequisites) == 0xE28 &&
                  offsetof(Objective, state) == 0xE38 && offsetof(Objective, bounds) == 0xE40 && offsetof(Objective, type) == 0xE60 &&
                  offsetof(Objective, data) == 0xE64);

    // Objective::flags, as the OBJECTIVE block's end handler (readObjectiveBlock) sets them from its values.
    enum ObjectiveFlags : uint32_t
    {
        kObjectivePersistent = 1u << 0, // PERSISTENT
        kObjectiveStartsActive = 1u << 1, // ACTIVE
        kObjectiveRepeatable = 1u << 2, // REPEATABLE
        kObjectiveHighlight = 1u << 3,  // HIGHLIGHT
        kObjectiveMoving = 1u << 4,     // MOVING
        // The module's own marks of what happened to the objective: the first four this frame (the game clears
        // them each frame, clearObjectiveFrameMarks), all five with its progress (typeResetProgress).
        kObjectiveActivatedNow = 1u << 5,     // activated (activateObjective, resetObjective)
        kObjectiveCompletedNow = 1u << 6,     // completed by the frame's visit (updateObjectiveVisit)
        kObjectiveTimedOutByScript = 1u << 7, // another objective's block timed it out (sendObjectiveMessage)
        kObjectiveStartedNow = 1u << 8,       // its type first said it is in progress, this frame
        kObjectiveStarted = 1u << 9,          // ... since its progress was last reset
        kObjectiveIsChild = 1u << 10,   // IS_CHILD
    };

    // What objectiveInfo answers: one record the engine keeps, filled for the objective asked about last.
    struct ObjectiveInfo
    {
        ee::Ptr<char> name;              // +0x00 the objective's own texts, not copies
        ee::Ptr<ObjectiveType> type;     // +0x04
        ee::Ptr<char> stringId;          // +0x08
        ee::Ptr<char> animPreview;       // +0x0C
        ee::Float timeout;               // +0x10
        uint32_t state;                  // +0x14
        uint32_t flags;                  // +0x18
    };
    static_assert(sizeof(ObjectiveInfo) == 0x1C);

    // findObjective: the objective of that name, or 0.
    ee::Ptr<Objective> findObjective(ee::Ptr<const char> name);
    // objectiveInfo: the engine's info record filled from the objective; the record.
    ee::Ptr<ObjectiveInfo> objectiveInfo(ee::Ptr<Objective> objective);
    // objectiveReadyToActivate: 1 when the objective is not active yet (its state below kObjectiveActive) and every
    // prerequisite it has is complete; else 0.
    uint32_t objectiveReadyToActivate(ee::Ptr<Objective> objective);
    // The objective's words by name, functions of their own in ZGE4.
    ee::Ptr<char> objectiveName(ee::Ptr<Objective> objective);
    uint32_t objectiveNameHash(ee::Ptr<Objective> objective);
    ee::Ptr<ObjectiveType> objectiveType(ee::Ptr<Objective> objective);
    uint32_t objectiveTypeHash(ee::Ptr<Objective> objective);
    ee::Ptr<Box> objectiveBounds(ee::Ptr<Objective> objective);
    uint32_t objectiveState(ee::Ptr<Objective> objective);
    void setObjectiveState(ee::Ptr<Objective> objective, uint32_t state);
    ee::Ptr<char> objectiveStringId(ee::Ptr<Objective> objective);
    uint32_t objectiveData(ee::Ptr<Objective> objective);
    void setObjectiveData(ee::Ptr<Objective> objective, uint32_t data);
    // objectiveStatusText: the type's status line into text (emptied first); the hook's answer, 0 with no hook.
    uint32_t objectiveStatusText(ee::Ptr<Objective> objective, ee::Ptr<char> text);
    // registerObjectiveType: the set-up copied into the next record of the type table (its name hashed).
    void registerObjectiveType(ee::Ptr<ObjectiveTypeSetup> setup);
    // findObjectiveType: the registered type of that name, or 0.
    ee::Ptr<ObjectiveType> findObjectiveType(ee::Ptr<const char> name);
    // objectiveFinished: 1 when the objective is done or complete.
    uint32_t objectiveFinished(ee::Ptr<Objective> objective);
    // reactivateObjective: active again, its type told so (onActivate with 1).
    void reactivateObjective(ee::Ptr<Objective> objective);
    // deleteObjective: the type's record freed (its hook), the objective back to the pool.
    void deleteObjective(ee::Ptr<Objective> objective);
    // freeObjectives: every objective deleted (forEachObjective's result).
    uint32_t freeObjectives();
    // clearObjectiveTypes: no type registered.
    void clearObjectiveTypes();
    // resetObjectives: every objective back to how the script left it; updateObjectives: the frame's visit of each,
    // with the caller's word; completeObjective: one objective complete (the type's step told whether quietly).
    uint32_t resetObjectives();
    uint32_t updateObjectives(uint32_t event);
    void completeObjective(ee::Ptr<Objective> objective, uint32_t quiet);
    // resetObjective: one objective back to how the script left it (a complete one that is not REPEATABLE
    // completed again, quietly); timeOutObjective: its TIMEOUT block run, then it reset.
    void resetObjective(ee::Ptr<Objective> objective);
    void timeOutObjective(ee::Ptr<Objective> objective);
    // runObjectiveActions: one of the objective's blocks run (ObjectiveActionSet); 1.
    uint32_t runObjectiveActions(ee::Ptr<Objective> objective, uint32_t set, uint32_t quiet);
    // objectiveByName: the objective whose name hashes to nameHash, or 0.
    ee::Ptr<Objective> objectiveByName(uint32_t nameHash);
    // setObjectiveFlags: the bits of *bits set (on) or cleared in the objective's flags.
    void setObjectiveFlags(ee::Ptr<Objective> objective, ee::Ptr<const uint32_t> bits, uint32_t on);
    // clearObjectiveFrameMarks: the marks of what happened to the objective this frame cleared.
    void clearObjectiveFrameMarks(ee::Ptr<Objective> objective);
    // forEachObjectiveJump: forEachObjective again, the game's entry of it (a jump there).
    uint32_t forEachObjectiveJump(uint32_t visit, ee::Address data);
    // shutDownObjectiveSystem: no type registered.
    void shutDownObjectiveSystem();
    // initObjectiveSystem: the module's set-up with the game's two hooks; highlightObjectives: every live objective
    // in progress drawn for the caller's word; activateObjective; sendObjectiveEvent: an event to one live
    // objective's type; objectiveLive: active or further, not complete, prerequisites complete.
    void initObjectiveSystem(ee::Ptr<const ObjectiveGameHooks> gameHooks);
    uint32_t highlightObjectives(uint32_t word);
    void activateObjective(ee::Ptr<Objective> objective);
    void sendObjectiveEvent(ee::Ptr<Objective> objective, uint32_t event);
    uint32_t objectiveLive(ee::Ptr<Objective> objective);
    // forEachObjective: visit(objective, data) for each objective, the oldest first, while it returns non-zero
    // (poolForEachOldestFirst's result); nothing when there are no objectives.
    uint32_t forEachObjective(uint32_t visit, ee::Address data);
    // readObjectives: the objectives of an open objectives file made (its OBJECTIVE blocks); 1 when the file read
    // and every prerequisite names an objective, else every objective freed.
    uint32_t readObjectives(uint32_t file);
}
