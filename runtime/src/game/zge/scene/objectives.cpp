// ZGE4's objectives (objectives.h; BMX XXX only): finding one by name, what the game is told about one, and
// whether one can be activated. The script reader, the blocks' records and the state changes are the original's.

#include "objectives.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/scene/cut_scenes.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/script_blocks.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/objectives.cpp.h"

namespace zge
{
    void poolGiveDestroying(ee::Ptr<ee::Ptr<Pool>> slot, ee::Address record); // (pool.cpp)

    namespace
    {
        // The module's state; its first word is the objectives' pool.
        inline ee::Global<ObjectiveSystem, sym::bmx::g_objectives> g_objectiveSystem;
        // objectiveInfo's record.
        inline ee::Global<ObjectiveInfo, sym::bmx::g_objectiveInfo> g_objectiveInfo;

        // What a search for one objective carries through forEachObjective.
        struct ObjectiveQuery
        {
            uint32_t byName;              // +0x00 1: asked for by name (findObjective); the search by hash alone leaves 0
            uint32_t nameHash;            // +0x04
            ee::Ptr<const char> name;     // +0x08 the name asked for (byName)
            ee::Ptr<Objective> found;     // +0x0C 0 until one matches
        };
        static_assert(sizeof(ObjectiveQuery) == 0x10);
    }

    // The objective's small accessors, functions of their own in ZGE4.
    inline ee::Ptr<char> objectiveName(ee::Ptr<Objective> objective) { return ee::Ptr<char>(objective->name); }
    READABLE_FUNCTION_ONLY(bmx, objectiveName);
    inline ee::Ptr<ObjectiveType> objectiveType(ee::Ptr<Objective> objective) { return objective->type; }
    READABLE_FUNCTION_ONLY(bmx, objectiveType);
    inline uint32_t objectiveState(ee::Ptr<Objective> objective) { return objective->state; }
    READABLE_FUNCTION_ONLY(bmx, objectiveState);
    inline uint32_t objectiveNameHash(ee::Ptr<Objective> objective) { return objective->nameHash; }
    READABLE_FUNCTION_ONLY(bmx, objectiveNameHash);
    inline uint32_t objectiveTypeHash(ee::Ptr<Objective> objective) { return objective->type->nameHash; }
    READABLE_FUNCTION_ONLY(bmx, objectiveTypeHash);
    inline ee::Ptr<Box> objectiveBounds(ee::Ptr<Objective> objective) { return &objective->bounds; }
    READABLE_FUNCTION_ONLY(bmx, objectiveBounds);
    inline void setObjectiveState(ee::Ptr<Objective> objective, uint32_t state) { objective->state = state; }
    READABLE_FUNCTION_ONLY(bmx, setObjectiveState);
    inline ee::Ptr<char> objectiveStringId(ee::Ptr<Objective> objective) { return ee::Ptr<char>(objective->stringId); }
    READABLE_FUNCTION_ONLY(bmx, objectiveStringId);
    inline uint32_t objectiveData(ee::Ptr<Objective> objective) { return objective->data; }
    READABLE_FUNCTION_ONLY(bmx, objectiveData);
    inline void setObjectiveData(ee::Ptr<Objective> objective, uint32_t data) { objective->data = data; }
    READABLE_FUNCTION_ONLY(bmx, setObjectiveData);
    // objectiveHooks: the objective's type again, as the module's own code asks for it before calling a hook.
    inline ee::Ptr<ObjectiveType> objectiveHooks(ee::Ptr<Objective> objective) { return objective->type; }
    READABLE_FUNCTION_ONLY(bmx, objectiveHooks);

    // typeStatusText: the text emptied, then the type's status hook asked; its answer, 0 with no hook.
    inline uint32_t typeStatusText(ee::Ptr<Objective> objective, ee::Ptr<char> text)
    {
        const ee::Ptr<ObjectiveType> type = objectiveHooks(objective);
        *text = 0;
        if (!type->hooks.statusText)
            return 0;
        return type->hooks.statusText(objective, text);
    }
    READABLE_FUNCTION_ONLY(bmx, typeStatusText);
    inline uint32_t objectiveStatusText(ee::Ptr<Objective> objective, ee::Ptr<char> text) { return typeStatusText(objective, text); }
    READABLE_FUNCTION_ONLY(bmx, objectiveStatusText);

    inline void registerObjectiveType(ee::Ptr<ObjectiveTypeSetup> setup)
    {
        ObjectiveSystem &system = *g_objectiveSystem;
        const ee::Ptr<ObjectiveType> type(&system.types[system.typeCount]);
        stringCopy(type, setup->name);
        type->nameHash = nameHash(type->name);
        type->hooks = setup->hooks;
        system.typeCount = system.typeCount + 1;
    }
    READABLE_FUNCTION_ONLY(bmx, registerObjectiveType);

    inline ee::Ptr<ObjectiveType> findObjectiveType(ee::Ptr<const char> name)
    {
        const uint32_t hash = nameHash(name);
        const ObjectiveSystem &system = *g_objectiveSystem;
        for (uint32_t i = 0; i < system.typeCount; ++i)
        {
            if (system.types[i].nameHash == hash)
                return ee::Ptr<ObjectiveType>(sym::bmx::g_objectives + offsetof(ObjectiveSystem, types) + i * sizeof(ObjectiveType));
        }
        return ee::Ptr<ObjectiveType>();
    }
    READABLE_FUNCTION_ONLY(bmx, findObjectiveType);

    // objectiveHasHash: forEachObjective's visitor for a search: the objective whose name's hash is the query's
    // is kept in the query and the walk stopped (0); else the walk goes on (1).
    inline uint32_t objectiveHasHash(ee::Ptr<Objective> objective, ee::Ptr<ObjectiveQuery> query)
    {
        if (objective->nameHash != query->nameHash)
            return 1;
        query->found = objective;
        return 0;
    }
    READABLE_FUNCTION_ONLY(bmx, objectiveHasHash);

    inline uint32_t forEachObjective(uint32_t visit, ee::Address data)
    {
        const ee::Ptr<Pool> objectives = g_objectiveSystem->objectives;
        if (!objectives)
            return ee::high(sym::bmx::g_objectives); // (the original answers nothing here: the register holds the high half of the state's address)
        return game::tail(poolForEachOldestFirst, objectives, visit, data);
    }
    READABLE_FUNCTION_ONLY(bmx, forEachObjective);

    inline ee::Ptr<Objective> findObjective(ee::Ptr<const char> name)
    {
        ee::Local<ObjectiveQuery> query;
        const uint32_t hash = nameHash(name);
        query->name = name;
        query->nameHash = hash;
        query->byName = 1;
        query->found = ee::Ptr<Objective>();
        forEachObjective(sym::bmx::objectiveHasHash, query);
        return query->found;
    }
    READABLE_FUNCTION_ONLY(bmx, findObjective);

    inline ee::Ptr<ObjectiveInfo> objectiveInfo(ee::Ptr<Objective> objective)
    {
        ObjectiveInfo &info = *g_objectiveInfo;
        info.name = objectiveName(objective);
        info.type = objectiveType(objective);
        info.stringId = ee::Ptr<char>(objective->stringId);
        info.animPreview = ee::Ptr<char>(objective->animPreview);
        info.timeout = objective->timeout;
        info.state = objective->state;
        memoryCopy(ee::Address(sym::bmx::g_objectiveInfo + offsetof(ObjectiveInfo, flags)), objective, sizeof(uint32_t));
        return ee::Ptr<ObjectiveInfo>(sym::bmx::g_objectiveInfo);
    }
    READABLE_FUNCTION_ONLY(bmx, objectiveInfo);

    // prerequisitesComplete: 1 when every prerequisite the objective has is complete (it may have none); the
    // first one that is not ends the check with 0.
    inline uint32_t prerequisitesComplete(ee::Ptr<Objective> objective)
    {
        for (uint32_t i = 0; i < kObjectivePrerequisites; ++i)
        {
            const ee::Ptr<Objective> prerequisite = objective->prerequisites[i];
            if (prerequisite && objectiveState(prerequisite) != kObjectiveComplete)
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, prerequisitesComplete);

    inline uint32_t objectiveReadyToActivate(ee::Ptr<Objective> objective)
    {
        if (objectiveState(objective) >= kObjectiveActive)
            return 0;
        return prerequisitesComplete(objective) != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_ONLY(bmx, objectiveReadyToActivate);

    // ---- the type's hooks, each asked through a function of its own (nothing is done for a hook the type has not) ----

    inline void typeReadParameters(ee::Ptr<Objective> objective, uint32_t parentKeyword)
    {
        const ee::Ptr<ObjectiveType> type = objectiveHooks(objective);
        if (type->hooks.readParameters)
            type->hooks.readParameters(objective, parentKeyword);
    }
    READABLE_FUNCTION_ONLY(bmx, typeReadParameters);

    inline void typeFreeData(ee::Ptr<Objective> objective)
    {
        const ee::Ptr<ObjectiveType> type = objectiveHooks(objective);
        if (type->hooks.freeData)
            type->hooks.freeData(objective);
    }
    READABLE_FUNCTION_ONLY(bmx, typeFreeData);

    inline void typeEvent(ee::Ptr<Objective> objective, uint32_t event)
    {
        const ee::Ptr<ObjectiveType> type = objectiveHooks(objective);
        if (type->hooks.onEvent)
            type->hooks.onEvent(objective, event);
    }
    READABLE_FUNCTION_ONLY(bmx, typeEvent);

    inline void typeActivate(ee::Ptr<Objective> objective, uint32_t again)
    {
        const ee::Ptr<ObjectiveType> type = objectiveHooks(objective);
        if (type->hooks.onActivate)
            type->hooks.onActivate(objective, again);
    }
    READABLE_FUNCTION_ONLY(bmx, typeActivate);

    inline void typeMoveBounds(ee::Ptr<Objective> objective)
    {
        const ee::Ptr<ObjectiveType> type = objectiveHooks(objective);
        if (type->hooks.moveBounds)
            type->hooks.moveBounds(objective);
    }
    READABLE_FUNCTION_ONLY(bmx, typeMoveBounds);

    // ---- every objective ----

    // deleteObjective: the type's record freed (its hook), the objective back to the pool (the pool goes with its
    // last record).
    inline void deleteObjective(ee::Ptr<Objective> objective)
    {
        typeFreeData(objective);
        game::tail(poolGiveDestroying, ee::Ptr<ee::Ptr<Pool>>(sym::bmx::g_objectives), objective);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, deleteObjective);

    inline uint32_t deleteObjectiveVisit(ee::Ptr<Objective> objective)
    {
        deleteObjective(objective);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, deleteObjectiveVisit);

    inline uint32_t freeObjectives() { return game::tail(forEachObjective, sym::bmx::deleteObjectiveVisit, 0u); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeObjectives);

    // clearObjectiveTypes: no type registered.
    inline void clearObjectiveTypes()
    {
        ObjectiveSystem &system = *g_objectiveSystem;
        memoryFill(sym::bmx::g_objectives + offsetof(ObjectiveSystem, types), 0, sizeof(system.types));
        system.typeCount = 0;
    }
    READABLE_FUNCTION_ONLY(bmx, clearObjectiveTypes);

    // objectiveFinished: 1 when the objective is done (state 4) or complete.
    inline uint32_t objectiveFinished(ee::Ptr<Objective> objective)
    {
        if (objectiveState(objective) == kObjectiveDone)
            return 1;
        return objectiveState(objective) == kObjectiveComplete ? 1 : 0;
    }
    READABLE_FUNCTION_ONLY(bmx, objectiveFinished);

    // reactivateObjective: active again, its type told so (onActivate with 1).
    inline void reactivateObjective(ee::Ptr<Objective> objective)
    {
        setObjectiveState(objective, kObjectiveActive);
        game::tail(typeActivate, objective, 1u);
    }
    READABLE_FUNCTION_ONLY(bmx, reactivateObjective);

    void typeComplete(ee::Ptr<Objective> objective, uint32_t quiet); // (below)

    inline uint32_t resetObjectiveVisit(ee::Ptr<Objective> objective)
    {
        resetObjective(objective);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, resetObjectiveVisit);

    inline uint32_t resetObjectives() { return game::tail(forEachObjective, sym::bmx::resetObjectiveVisit, 0u); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, resetObjectives);

    // updateObjectives: the frame's visit of every objective, with the caller's word (the game's event record).
    inline uint32_t updateObjectives(uint32_t event) { return game::tail(forEachObjective, sym::bmx::updateObjectiveVisit, event); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, updateObjectives);

    // completeObjective: the state complete, then the type's complete step.
    inline void completeObjective(ee::Ptr<Objective> objective, uint32_t quiet)
    {
        setObjectiveState(objective, kObjectiveComplete);
        game::tail(typeComplete, objective, quiet);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, completeObjective);
    // ---- the module's set-up, and one objective's turns ----

    namespace
    {
        // The module's marks (ObjectiveFlags), cleared with the objective's progress, and those of this frame alone.
        constexpr uint32_t kObjectiveProgressFlags =
            kObjectiveActivatedNow | kObjectiveCompletedNow | kObjectiveTimedOutByScript | kObjectiveStartedNow | kObjectiveStarted;
        constexpr uint32_t kObjectiveFrameMarks =
            kObjectiveActivatedNow | kObjectiveCompletedNow | kObjectiveTimedOutByScript | kObjectiveStartedNow;
    }

    // initObjectiveSystem: no type registered, and the game's two hooks kept (the module's own where the game
    // gives 0).
    inline void initObjectiveSystem(ee::Ptr<const ObjectiveGameHooks> gameHooks)
    {
        ObjectiveSystem &system = *g_objectiveSystem;
        memoryFill(sym::bmx::g_objectives + offsetof(ObjectiveSystem, types), 0, sizeof(system.types));
        system.gameHooks.cue = gameHooks->cue;
        if (gameHooks->cue == 0)
            system.gameHooks.cue = sym::bmx::defaultObjectiveScriptHook;
        system.gameHooks.objectAction = gameHooks->objectAction;
        if (gameHooks->objectAction == 0)
            system.gameHooks.objectAction = sym::bmx::defaultObjectiveActionHook;
    }
    READABLE_FUNCTION_ONLY(bmx, initObjectiveSystem);

    // objectiveLive: 1 for an objective that is active or further, not complete, and has its prerequisites complete.
    inline uint32_t objectiveLive(ee::Ptr<Objective> objective)
    {
        if (objectiveState(objective) == kObjectiveComplete)
            return 0;
        if (objectiveState(objective) < kObjectiveActive)
            return 0;
        return prerequisitesComplete(objective);
    }
    READABLE_FUNCTION_ONLY(bmx, objectiveLive);

    // typeResetProgress: the type's resetProgress hook, then the objective's progress flags cleared.
    inline void typeResetProgress(ee::Ptr<Objective> objective)
    {
        const ee::Ptr<ObjectiveType> type = objectiveHooks(objective);
        if (type->hooks.resetProgress)
            type->hooks.resetProgress(objective);
        objective->flags &= ~kObjectiveProgressFlags;
    }
    READABLE_FUNCTION_ONLY(bmx, typeResetProgress);

    // typeDrawForRider: a HIGHLIGHT objective's drawForRider hook.
    inline void typeDrawForRider(ee::Ptr<Objective> objective, uint32_t word)
    {
        const ee::Ptr<ObjectiveType> type = objectiveHooks(objective);
        if ((objective->flags & kObjectiveHighlight) != 0 && type->hooks.drawForRider)
            type->hooks.drawForRider(objective, word);
    }
    READABLE_FUNCTION_ONLY(bmx, typeDrawForRider);

    // highlightObjectives: every live objective in progress drawn for the caller's word (typeDrawForRider).
    inline uint32_t highlightObjectiveVisit(ee::Ptr<Objective> objective, uint32_t word)
    {
        if (objectiveLive(objective) == 1 && objectiveState(objective) == kObjectiveInProgress)
            typeDrawForRider(objective, word);
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, highlightObjectiveVisit);

    inline uint32_t highlightObjectives(uint32_t word) { return game::tail(forEachObjective, sym::bmx::highlightObjectiveVisit, word); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, highlightObjectives);

    // activateObjective: the objective active: its progress reset, its type told (onActivate with 0), marked
    // activated and its first set of actions run. One in state 1 is made done instead.
    inline void activateObjective(ee::Ptr<Objective> objective)
    {
        constexpr uint32_t kStateDoneWhenActivated = 1;
        if (objectiveState(objective) == kStateDoneWhenActivated)
        {
            game::tail(setObjectiveState, objective, static_cast<uint32_t>(kObjectiveDone));
            return;
        }
        setObjectiveState(objective, kObjectiveActive);
        typeResetProgress(objective);
        typeActivate(objective, 0u);
        objective->flags |= kObjectiveActivatedNow;
        game::tail(runObjectiveActions, objective, static_cast<uint32_t>(kObjectiveOnActivate), 0u);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, activateObjective);

    // sendObjectiveEvent: the event to a live objective's type.
    inline void sendObjectiveEvent(ee::Ptr<Objective> objective, uint32_t event)
    {
        if (objectiveLive(objective) != 0)
            game::tail(typeEvent, objective, event);
    }
    READABLE_FUNCTION_ONLY(bmx, sendObjectiveEvent);

    // ---- the objectives' blocks run, and the frame's visit ----

    namespace
    {
        // The names objectiveMessageType knows, in ObjectiveMessage's order ("Activate", "Complete", "Timeout").
        inline ee::Global<ee::Ptr<const char>[kObjectiveMessageUnknown], sym::bmx::g_objectiveMessageNames> g_objectiveMessageNames;
    }

    // objectiveMessageType: the ObjectiveMessage an OBJCTVMSG block's MESSAGE names (case aside), or
    // kObjectiveMessageUnknown.
    inline uint32_t objectiveMessageType(ee::Ptr<const char> name)
    {
        uint32_t message = 0;
        for (; message < kObjectiveMessageUnknown; ++message)
        {
            if (stringCompareNoCase(name, (*g_objectiveMessageNames)[message]) == 0)
                return message;
        }
        return kObjectiveMessageUnknown;
    }
    READABLE_FUNCTION_ONLY(bmx, objectiveMessageType);

    // sendObjectiveMessage: an OBJCTVMSG block's message to the objective of that name (nothing when there is
    // none, or when the objective is past what the message does).
    inline void sendObjectiveMessage(ee::Ptr<const char> name, uint32_t message)
    {
        const ee::Ptr<Objective> objective = findObjective(name);
        if (!objective)
            return;
        if (message == kObjectiveMessageActivate)
        {
            if (objectiveState(objective) < kObjectiveActive)
                game::tail(activateObjective, objective);
        }
        else if (message == kObjectiveMessageComplete)
        {
            if (objectiveState(objective) < kObjectiveComplete)
                game::tail(completeObjective, objective, 1u);
        }
        else if (message == kObjectiveMessageTimeout)
        {
            if (objectiveState(objective) >= kObjectiveActive && objectiveState(objective) < kObjectiveComplete)
            {
                timeOutObjective(objective);
                objective->flags |= kObjectiveTimedOutByScript;
            }
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, sendObjectiveMessage);

    // runObjectiveActions: the objective's block of that set run: each ENTITY block's action through the game's
    // objectAction hook, each OBJCTVMSG block's message, then its cut scene cued, or with none its sound (not
    // when quiet). 1.
    inline uint32_t runObjectiveActions(ee::Ptr<Objective> objective, uint32_t set, uint32_t quiet)
    {
        const ee::Ptr<ObjectiveActions> actions(&objective->actions[set]);
        for (uint32_t i = 0; i < actions->entityCount; ++i)
        {
            const ee::Ptr<ObjectiveEntityAction> entity(&actions->entities[i]);
            g_objectiveSystem->gameHooks.objectAction(entity->object, ee::Ptr<char>(entity->action), quiet);
        }
        for (uint32_t i = 0; i < actions->messageCount; ++i)
        {
            const ee::Ptr<ObjectiveMessageAction> sent(&actions->messages[i]);
            sendObjectiveMessage(ee::Ptr<const char>(sent->objective), sent->message);
        }
        if (actions->cutScene != 0)
            g_objectiveSystem->gameHooks.cue(kObjectiveCueCutScene, actions->cutScene, quiet);
        else if (actions->sound != 0 && quiet == 0)
            g_objectiveSystem->gameHooks.cue(kObjectiveCueSound, actions->sound, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, runObjectiveActions);

    // typeComplete (the complete step): the type's onComplete hook, the COMPLETE block, then a REPEATABLE
    // objective reset at once, so that it can be taken again.
    inline void typeComplete(ee::Ptr<Objective> objective, uint32_t quiet)
    {
        const ee::Ptr<ObjectiveType> type = objectiveHooks(objective);
        if (type->hooks.onComplete)
            type->hooks.onComplete(objective, quiet);
        runObjectiveActions(objective, kObjectiveOnComplete, quiet);
        if ((objective->flags & kObjectiveRepeatable) != 0)
            game::tail(resetObjectiveVisit, objective);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, typeComplete);

    // resetObjective: a complete objective that is not REPEATABLE is completed again, quietly, and stays complete.
    // Any other goes back to active (when the script made it ACTIVE and its prerequisites are complete) or to 0,
    // with its progress reset; an active one runs its ACTIVATE block quietly and is marked activated.
    inline void resetObjective(ee::Ptr<Objective> objective)
    {
        if (objectiveState(objective) == kObjectiveComplete && (objective->flags & kObjectiveRepeatable) == 0)
        {
            game::tail(typeComplete, objective, 1u);
            return;
        }
        if ((objective->flags & kObjectiveStartsActive) != 0 && prerequisitesComplete(objective) != 0)
            setObjectiveState(objective, kObjectiveActive);
        else
            setObjectiveState(objective, 0);
        typeResetProgress(objective);
        if (objectiveState(objective) == kObjectiveActive)
        {
            runObjectiveActions(objective, kObjectiveOnActivate, 1);
            objective->flags |= kObjectiveActivatedNow;
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, resetObjective);

    // timeOutObjective: the TIMEOUT block run, then the objective reset.
    inline void timeOutObjective(ee::Ptr<Objective> objective)
    {
        runObjectiveActions(objective, kObjectiveOnTimeout, 0);
        game::tail(resetObjective, objective);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, timeOutObjective);

    // updateObjectiveVisit: the frame's visit of one live objective with the caller's event. A done one is
    // completed (and marked so); any other has a MOVING one's bounds moved, is in progress or back to active as
    // its type's inProgress hook says (in progress with no hook; the first time since its progress was reset,
    // marked started), and is given the event. 1.
    inline uint32_t updateObjectiveVisit(ee::Ptr<Objective> objective, uint32_t event)
    {
        const ee::Ptr<ObjectiveType> type = objective->type;
        if (objectiveLive(objective) != 1)
            return 1;
        if (objectiveState(objective) == kObjectiveDone)
        {
            completeObjective(objective, 0);
            objective->flags |= kObjectiveCompletedNow;
            return 1;
        }
        if ((objective->flags & kObjectiveMoving) != 0 && type->hooks.moveBounds)
            type->hooks.moveBounds(objective);
        if (!type->hooks.inProgress || type->hooks.inProgress(objective, event) != 0)
        {
            if (objectiveState(objective) < kObjectiveInProgress)
            {
                setObjectiveState(objective, kObjectiveInProgress);
                if ((objective->flags & kObjectiveStarted) == 0)
                    objective->flags |= kObjectiveStartedNow | kObjectiveStarted;
            }
        }
        else if (objectiveState(objective) == kObjectiveInProgress)
            setObjectiveState(objective, kObjectiveActive);
        typeEvent(objective, event);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, updateObjectiveVisit);

    // ---- what the game asks of one objective ----

    inline ee::Ptr<Objective> objectiveByName(uint32_t nameHash)
    {
        ee::Local<ObjectiveQuery> query;
        query->byName = 0;
        query->nameHash = nameHash;
        query->name = ee::Ptr<const char>();
        query->found = ee::Ptr<Objective>();
        forEachObjective(sym::bmx::objectiveHasHash, query);
        return query->found;
    }
    READABLE_FUNCTION_ONLY(bmx, objectiveByName);

    inline uint32_t forEachObjectiveJump(uint32_t visit, ee::Address data) { return game::tail(forEachObjective, visit, data); }
    READABLE_FUNCTION_ONLY(bmx, forEachObjectiveJump);

    inline void setObjectiveFlags(ee::Ptr<Objective> objective, ee::Ptr<const uint32_t> bits, uint32_t on)
    {
        if (on != 0)
            objective->flags |= *bits;
        else
            objective->flags &= ~*bits;
    }
    READABLE_FUNCTION_ONLY(bmx, setObjectiveFlags);

    inline void clearObjectiveFrameMarks(ee::Ptr<Objective> objective) { objective->flags &= ~kObjectiveFrameMarks; }
    READABLE_FUNCTION_ONLY(bmx, clearObjectiveFrameMarks);

    inline void shutDownObjectiveSystem() { game::tail(clearObjectiveTypes); }
    READABLE_FUNCTION_ONLY(bmx, shutDownObjectiveSystem);

    // The module's own game hooks, for a 0 at set-up: a cut scene cue played, nothing else done. 0.
    inline uint32_t defaultObjectiveScriptHook(uint32_t cue, uint32_t nameHash, uint32_t quiet)
    {
        if (cue == kObjectiveCueCutScene)
            startCutScene(nameHash, quiet);
        return 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, defaultObjectiveScriptHook);
    inline uint32_t defaultObjectiveActionHook(ee::Ptr<SceneObject>, ee::Ptr<char>, uint32_t) { return 0; }
    READABLE_FUNCTION_ONLY(bmx, defaultObjectiveActionHook);

    // ---- making the objectives (what the script readers call) ----

    // newObjective: a cleared record from the objectives' pool, ACTIVE and IS_CHILD, state 0, of the "NO OP" type.
    inline ee::Ptr<Objective> newObjective()
    {
        const ee::Ptr<Objective> objective(poolTakeCreating(ee::Ptr<ee::Ptr<Pool>>(sym::bmx::g_objectives), sizeof(Objective)));
        memoryFill(objective, 0, sizeof(Objective));
        objective->flags = (objective->flags | kObjectiveStartsActive | kObjectiveIsChild) & ~kObjectivePersistent;
        objective->state = 0;
        objective->type = findObjectiveType(ee::Ptr<const char>(sym::bmx::noOpObjectiveTypeName));
        return objective;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newObjective);

    // setObjectiveName: NAME copied in and hashed.
    inline void setObjectiveName(ee::Ptr<Objective> objective, ee::Ptr<const char> name)
    {
        stringCopy(ee::Ptr<char>(objective->name), name);
        objective->nameHash = nameHash(name);
    }
    READABLE_FUNCTION_ONLY(bmx, setObjectiveName);

    // setObjectiveTypeByName: TYPE: the old type's record freed, the registered type of that name taken (0: none).
    inline void setObjectiveTypeByName(ee::Ptr<Objective> objective, ee::Ptr<const char> typeName)
    {
        typeFreeData(objective);
        objective->type = findObjectiveType(typeName);
    }
    READABLE_FUNCTION_ONLY(bmx, setObjectiveTypeByName);

    // cutSceneListed: 1 when one of the level's cut scenes has that name (a block's ANIMSCRIPT is kept only then).
    // The objective and the block's key are the GameCube build's warning's, unused here.
    inline uint32_t cutSceneListed(ee::Ptr<Objective>, ee::Ptr<const char>, ee::Ptr<const char> name)
    {
        const uint32_t hash = nameHash(name);
        ee::Ptr<ee::Ptr<CutScene>> cutScene = cutSceneList();
        // (a cut scene's record starts with its name)
        while (*cutScene && nameHash(ee::Ptr<const char>(ee::Address(*cutScene))) != hash)
            ++cutScene;
        return *cutScene ? 1 : 0;
    }
    READABLE_FUNCTION_ONLY(bmx, cutSceneListed);

    // resolvePrerequisites (forEachObjective's visit once the file is read): each prerequisite's name hash made
    // the objective of that name (the first empty place ends them); one that names none is left as it is and
    // *allFound cleared. 1.
    inline uint32_t resolvePrerequisites(ee::Ptr<Objective> objective, ee::Ptr<uint32_t> allFound)
    {
        for (uint32_t i = 0; i < kObjectivePrerequisites; ++i)
        {
            // (the place's word, still the name's hash)
            const ee::Ptr<const uint32_t> place(ee::Address(&objective->prerequisites[i]));
            const uint32_t prerequisiteHash = *place;
            if (prerequisiteHash == 0)
                break;
            const ee::Ptr<Objective> prerequisite = objectiveByName(prerequisiteHash);
            if (prerequisite)
                objective->prerequisites[i] = prerequisite;
            else
                *allFound = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, resolvePrerequisites);
    // ---- reading the objectives (the script's blocks) ----
    //
    // An objectives file is a script of OBJECTIVE blocks (zge/text/script_blocks.h). readObjectives registers the
    // OBJECTIVE class with its keys and reads the file; as each OBJECTIVE block begins, its objective is made and the
    // classes of the blocks inside it are registered with it as their owner (PARAMETERS, PREREQUISITE, and the
    // three action blocks ACTIVATE, COMPLETE and TIMEOUT, which hold ENTITY and OBJCTVMSG blocks); each block's
    // end handler puts what the block said into the objective. A handler's 1 goes on with the reading; a 0 ends it.

    namespace
    {
        // The OBJECTIVE block's keys (objectiveKeyNames: NAME, TYPE, TIMEOUT, PERSISTENT, ACTIVE, REPEATABLE,
        // MOVING, HIGHLIGHT, IS_CHILD, STRINGID, ANIM_PREVIEW); an action block's (objectiveActionKeyNames:
        // ANIMSCRIPT, AUDIO, MOVIE, with objectiveActionKeyKinds' three 0s); an ENTITY or OBJCTVMSG block's
        // (objectiveTargetKeyNames: NAME, MESSAGE).
        constexpr uint32_t kObjectiveKeys = 11;
        constexpr uint32_t kObjectiveActionKeys = 3;
        constexpr uint32_t kObjectiveTargetKeys = 2;
        constexpr uint32_t kKeyTextBytes = 0x20;
        constexpr uint32_t kEntityNameBytes = 0x40;

        ee::Ptr<const char> text(uint32_t symbol) { return ee::Ptr<const char>(symbol); }
    }

    namespace // the game functions below call: ee/plain.h
    {
        void memset(ee::Address destination, int32_t byte, uint32_t bytes) { ee::game(sym::memset, destination, byte, bytes); }
    }

    // readObjectives: the objectives of an open objectives file made, the names of their prerequisites made the
    // objectives (resolvePrerequisites). 1 when the file read and every prerequisite was found; anything else frees
    // every objective and is the result.
    inline uint32_t readObjectives(uint32_t file)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kObjectiveKeys]> keyNames;
        ee::Local<uint32_t[kObjectiveKeys]> keyKinds;
        ee::Local<uint32_t> result;
        const ee::Ptr<ee::Ptr<const char>> names(sym::bmx::objectiveKeyNames);
        for (uint32_t i = 0; i < kObjectiveKeys; ++i)
            keyNames[i] = names[i];
        memset(keyKinds, 0, sizeof(uint32_t[kObjectiveKeys]));
        setup->begin = text(sym::bmx::objectiveBeginKeyword);
        setup->end = text(sym::bmx::objectiveEndKeyword);
        setup->parentBegin = ee::Ptr<const char>();
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->keyCount = kObjectiveKeys;
        setup->onBegin = sym::bmx::beginObjectiveBlock;
        setup->onEnd = sym::bmx::readObjectiveBlock;
        registerBlockClass(setup);
        *result = readScriptBlocks(file);
        unregisterBlockClass(text(sym::bmx::objectiveBeginKeyword));
        if (*result == 1)
            forEachObjective(sym::bmx::resolvePrerequisites, result);
        if (*result != 1)
            freeObjectives();
        return *result;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readObjectives);

    // beginObjectiveBlock (OBJECTIVE's begin): a new objective, the owner of the OBJECTIVE block and of the blocks
    // registered inside it: PARAMETERS (no keys of its own, read as it begins), PREREQUISITE (NAME) and the three
    // action blocks. 1.
    inline uint32_t beginObjectiveBlock(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<BlockClassSetup> parameters;
        ee::Local<BlockClassSetup> prerequisite;
        ee::Local<BlockClassSetup> actions;
        ee::Local<ee::Ptr<const char>> prerequisiteKeyName;
        ee::Local<uint32_t> prerequisiteKeyKind;
        *prerequisiteKeyName = text(sym::bmx::objectiveKeyName);
        *prerequisiteKeyKind = 0;
        const ee::Ptr<Objective> objective = newObjective();
        setBlockClassOwner(beginKeyword, objective);

        parameters->begin = text(sym::bmx::parametersBeginKeyword);
        parameters->end = text(sym::bmx::parametersEndKeyword);
        parameters->parentBegin = beginKeyword;
        parameters->keyNames = ee::Ptr<ee::Ptr<const char>>();
        parameters->keyKinds = ee::Ptr<uint32_t>();
        parameters->keyCount = 0;
        parameters->onBegin = sym::bmx::readObjectiveParameters;
        parameters->onEnd = 0;
        registerBlockClass(parameters);
        setBlockClassOwner(text(sym::bmx::parametersBeginKeyword), objective);

        prerequisite->begin = text(sym::bmx::prerequisiteBeginKeyword);
        prerequisite->end = text(sym::bmx::prerequisiteEndKeyword);
        prerequisite->parentBegin = beginKeyword;
        prerequisite->keyNames = prerequisiteKeyName;
        prerequisite->keyKinds = prerequisiteKeyKind;
        prerequisite->keyCount = 1;
        prerequisite->onBegin = 0;
        prerequisite->onEnd = sym::bmx::readObjectivePrerequisite;
        registerBlockClass(prerequisite);
        setBlockClassOwner(text(sym::bmx::prerequisiteBeginKeyword), objective);

        // (one set-up for the three: each changes only its keywords and handlers)
        actions->begin = text(sym::bmx::activateBeginKeyword);
        actions->end = text(sym::bmx::activateEndKeyword);
        actions->parentBegin = beginKeyword;
        actions->keyNames = ee::Ptr<ee::Ptr<const char>>(sym::bmx::objectiveActionKeyNames);
        actions->keyKinds = ee::Ptr<uint32_t>(sym::bmx::objectiveActionKeyKinds);
        actions->keyCount = kObjectiveActionKeys;
        actions->onBegin = sym::bmx::beginActivateBlock;
        actions->onEnd = sym::bmx::readActivateBlock;
        registerBlockClass(actions);
        setBlockClassOwner(text(sym::bmx::activateBeginKeyword), objective);

        actions->begin = text(sym::bmx::completeBeginKeyword);
        actions->end = text(sym::bmx::completeEndKeyword);
        actions->onBegin = sym::bmx::beginCompleteBlock;
        actions->onEnd = sym::bmx::readCompleteBlock;
        registerBlockClass(actions);
        setBlockClassOwner(text(sym::bmx::completeBeginKeyword), objective);

        actions->begin = text(sym::bmx::timeoutBeginKeyword);
        actions->end = text(sym::bmx::timeoutEndKeyword);
        actions->onBegin = sym::bmx::beginTimeoutBlock;
        actions->onEnd = sym::bmx::readTimeoutBlock;
        registerBlockClass(actions);
        setBlockClassOwner(text(sym::bmx::timeoutBeginKeyword), objective);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, beginObjectiveBlock);

    // A switch key of the OBJECTIVE block: the flag set as it says (its 0 for an unreadable one), left as it was
    // without the key. Whether the key was there.
    inline bool readObjectiveSwitch(ee::Ptr<Objective> objective, ee::Ptr<ScriptValueSet> values, uint32_t key, uint32_t flag)
    {
        const ee::Ptr<ScriptValue> value = findScriptValue(values, text(key));
        if (!value)
            return false;
        const uint32_t on = scriptValueSwitch(value, 0) & 1;
        objective->flags = (objective->flags & ~flag) | (on != 0 ? flag : 0u);
        return true;
    }

    // readObjectiveBlock (OBJECTIVE's end): the block's keys into the objective, which is left active: TIMEOUT, the
    // switches, STRINGID and ANIM_PREVIEW. One that is not IS_CHILD and says nothing of PERSISTENT is persistent.
    // Its progress reset, its bounds worked out, and the inner blocks' classes gone. 1.
    inline uint32_t readObjectiveBlock(ee::Ptr<const char> beginKeyword)
    {
        const ee::Ptr<Objective> objective(blockClassOwner(beginKeyword));
        const ee::Ptr<ScriptValueSet> values = blockClassValues(beginKeyword);
        ee::Ptr<ScriptValue> value = findScriptValue(values, text(sym::bmx::objectiveKeyTimeout));
        if (value)
            objective->timeout = scriptValueFloat(value);
        setObjectiveState(objective, kObjectiveActive);
        readObjectiveSwitch(objective, values, sym::bmx::objectiveKeyActive, kObjectiveStartsActive);
        readObjectiveSwitch(objective, values, sym::bmx::objectiveKeyPersistent, kObjectivePersistent);
        readObjectiveSwitch(objective, values, sym::bmx::objectiveKeyRepeatable, kObjectiveRepeatable);
        readObjectiveSwitch(objective, values, sym::bmx::objectiveKeyHighlight, kObjectiveHighlight);
        readObjectiveSwitch(objective, values, sym::bmx::objectiveKeyMoving, kObjectiveMoving);
        if (readObjectiveSwitch(objective, values, sym::bmx::objectiveKeyIsChild, kObjectiveIsChild) &&
            (objective->flags & kObjectiveIsChild) == 0 && !findScriptValue(values, text(sym::bmx::objectiveKeyPersistent)))
            objective->flags |= kObjectivePersistent;
        value = findScriptValue(values, text(sym::bmx::objectiveKeyStringId));
        if (value)
            scriptValueText(value, ee::Ptr<char>(objective->stringId), kObjectiveNameBytes);
        value = findScriptValue(values, text(sym::bmx::objectiveKeyAnimPreview));
        if (value)
            scriptValueText(value, ee::Ptr<char>(objective->animPreview), kObjectiveNameBytes);
        typeResetProgress(objective);
        typeMoveBounds(objective);
        unregisterBlockClass(text(sym::bmx::parametersBeginKeyword));
        unregisterBlockClass(text(sym::bmx::prerequisiteBeginKeyword));
        unregisterBlockClass(text(sym::bmx::activateBeginKeyword));
        unregisterBlockClass(text(sym::bmx::completeBeginKeyword));
        unregisterBlockClass(text(sym::bmx::timeoutBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readObjectiveBlock);

    // readObjectiveParameters (PARAMETERS' begin): the OBJECTIVE block's NAME and TYPE (its parent's keys, read by
    // then) given to the objective, then the type registers its own block inside PARAMETERS (typeReadParameters),
    // which is read next. 0, ending the
    // reading, with no NAME, no TYPE or a type nobody registered; else 1.
    inline uint32_t readObjectiveParameters(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[kKeyTextBytes]> word; // the parent's keyword, then NAME, then TYPE
        const ee::Ptr<Objective> objective(blockClassOwner(beginKeyword));
        blockClassParentName(beginKeyword, word);
        const ee::Ptr<ScriptValueSet> values = blockClassValues(word);
        ee::Ptr<ScriptValue> value = findScriptValue(values, text(sym::bmx::objectiveKeyName));
        if (!value)
            return 0;
        scriptValueText(value, word, kKeyTextBytes);
        setObjectiveName(objective, word);
        value = findScriptValue(values, text(sym::bmx::objectiveKeyType));
        if (!value)
            return 0;
        scriptValueText(value, word, kKeyTextBytes);
        if (!findObjectiveType(word))
            return 0;
        setObjectiveTypeByName(objective, word);
        typeReadParameters(objective, ee::Address(beginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readObjectiveParameters);

    // readObjectivePrerequisite (PREREQUISITE's end): its NAME's hash in the objective's first empty prerequisite
    // place (none left: nothing). 0, ending the reading, with no NAME; else 1.
    inline uint32_t readObjectivePrerequisite(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[kKeyTextBytes]> name;
        const ee::Ptr<Objective> objective(blockClassOwner(beginKeyword));
        const ee::Ptr<ScriptValue> value = findScriptValue(blockClassValues(beginKeyword), text(sym::bmx::objectiveKeyName));
        if (!value)
            return 0;
        scriptValueText(value, name, kKeyTextBytes);
        for (uint32_t i = 0; i < kObjectivePrerequisites; ++i)
        {
            // (the place's word: the name's hash until resolvePrerequisites makes it the objective)
            const ee::Ptr<uint32_t> place(ee::Address(&objective->prerequisites[i]));
            if (*place == 0)
            {
                *place = nameHash(name);
                break;
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readObjectivePrerequisite);

    // beginActionsBlock (an action block's begin, `set` which of the three: ObjectiveActionSet): the ENTITY and
    // OBJCTVMSG blocks registered inside it (NAME and MESSAGE each), owned by the objective's actions of that set. 1.
    inline uint32_t beginActionsBlock(ee::Ptr<const char> beginKeyword, uint32_t set)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kObjectiveTargetKeys]> keyNames;
        ee::Local<uint32_t[kObjectiveTargetKeys]> keyKinds;
        const ee::Ptr<ee::Ptr<const char>> names(sym::bmx::objectiveTargetKeyNames);
        for (uint32_t i = 0; i < kObjectiveTargetKeys; ++i)
            keyNames[i] = names[i];
        memset(keyKinds, 0, sizeof(uint32_t[kObjectiveTargetKeys]));
        const ee::Ptr<Objective> objective(blockClassOwner(beginKeyword));
        const ee::Ptr<ObjectiveActions> actions(&objective->actions[set]);
        setup->begin = text(sym::bmx::entityBeginKeyword);
        setup->end = text(sym::bmx::entityEndKeyword);
        setup->parentBegin = beginKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->keyCount = kObjectiveTargetKeys;
        setup->onBegin = 0;
        setup->onEnd = sym::bmx::readEntityAction;
        registerBlockClass(setup);
        setBlockClassOwner(text(sym::bmx::entityBeginKeyword), actions);
        setup->begin = text(sym::bmx::objectiveMessageBeginKeyword);
        setup->end = text(sym::bmx::objectiveMessageEndKeyword);
        setup->parentBegin = beginKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->keyCount = kObjectiveTargetKeys;
        setup->onBegin = 0;
        setup->onEnd = sym::bmx::readMessageAction;
        registerBlockClass(setup);
        setBlockClassOwner(text(sym::bmx::objectiveMessageBeginKeyword), actions);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, beginActionsBlock);

    // readActionsBlock (an action block's end): its ANIMSCRIPT (kept only when it names one of the level's cut
    // scenes), AUDIO and MOVIE, each as its name's hash, an empty one left as it was; the ENTITY and OBJCTVMSG
    // classes gone. 1.
    inline uint32_t readActionsBlock(ee::Ptr<const char> beginKeyword, uint32_t set)
    {
        ee::Local<char[kKeyTextBytes]> key;
        ee::Local<char[kKeyTextBytes]> name;
        const ee::Ptr<Objective> objective(blockClassOwner(beginKeyword));
        const ee::Ptr<ObjectiveActions> actions(&objective->actions[set]);
        const ee::Ptr<ScriptValueSet> values = blockClassValues(beginKeyword);
        formatString(key, sym::bmx::objectiveKeyAnimScript);
        ee::Ptr<ScriptValue> value = findScriptValue(values, key);
        if (value)
        {
            scriptValueText(value, name, kKeyTextBytes);
            if (name[0] != 0 && cutSceneListed(objective, key, name) == 1)
                actions->cutScene = nameHash(name);
        }
        formatString(key, sym::bmx::objectiveKeyAudio);
        value = findScriptValue(values, key);
        if (value)
        {
            scriptValueText(value, name, kKeyTextBytes);
            if (name[0] != 0)
                actions->sound = nameHash(name);
        }
        formatString(key, sym::bmx::objectiveKeyMovie);
        value = findScriptValue(values, key);
        if (value)
        {
            scriptValueText(value, name, kKeyTextBytes);
            if (name[0] != 0)
                actions->movie = nameHash(name);
        }
        unregisterBlockClass(text(sym::bmx::entityBeginKeyword));
        unregisterBlockClass(text(sym::bmx::objectiveMessageBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readActionsBlock);

    // The three action blocks' handlers: beginActionsBlock and readActionsBlock for their set.
    inline uint32_t beginActivateBlock(ee::Ptr<const char> beginKeyword) { return beginActionsBlock(beginKeyword, kObjectiveOnActivate); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, beginActivateBlock);
    inline uint32_t readActivateBlock(ee::Ptr<const char> beginKeyword) { return readActionsBlock(beginKeyword, kObjectiveOnActivate); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readActivateBlock);
    inline uint32_t beginCompleteBlock(ee::Ptr<const char> beginKeyword) { return beginActionsBlock(beginKeyword, kObjectiveOnComplete); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, beginCompleteBlock);
    inline uint32_t readCompleteBlock(ee::Ptr<const char> beginKeyword) { return readActionsBlock(beginKeyword, kObjectiveOnComplete); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readCompleteBlock);
    inline uint32_t beginTimeoutBlock(ee::Ptr<const char> beginKeyword) { return beginActionsBlock(beginKeyword, kObjectiveOnTimeout); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, beginTimeoutBlock);
    inline uint32_t readTimeoutBlock(ee::Ptr<const char> beginKeyword) { return readActionsBlock(beginKeyword, kObjectiveOnTimeout); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readTimeoutBlock);

    // readEntityAction (ENTITY's end): the level object its NAME names, with its MESSAGE, the next of the actions'
    // ENTITY actions. 0, ending the reading, when all sixteen are taken, or with no NAME, no such object (its place
    // keeps the 0) or no MESSAGE; else 1. (The action block's owner is asked for too, and not used.)
    inline uint32_t readEntityAction(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[kKeyTextBytes]> key;
        ee::Local<char[kKeyTextBytes]> parentKeyword;
        ee::Local<char[kEntityNameBytes]> objectName;
        const ee::Ptr<ObjectiveActions> actions(blockClassOwner(beginKeyword));
        const ee::Ptr<ScriptValueSet> values = blockClassValues(beginKeyword);
        blockClassParentName(beginKeyword, parentKeyword);
        blockClassOwner(parentKeyword);
        if (actions->entityCount >= kObjectiveEntityActions)
            return 0;
        formatString(key, sym::bmx::objectiveKeyName);
        ee::Ptr<ScriptValue> value = findScriptValue(values, key);
        if (!value)
            return 0;
        scriptValueText(value, objectName, kEntityNameBytes);
        const ee::Ptr<SceneObject> object = findObjectByName(ee::Address(objectName));
        actions->entities[actions->entityCount].object = object;
        if (!object)
            return 0;
        formatString(key, sym::bmx::objectiveKeyMessage);
        value = findScriptValue(values, key);
        if (!value)
            return 0;
        scriptValueText(value, ee::Ptr<char>(actions->entities[actions->entityCount].action), kObjectiveNameBytes);
        ++actions->entityCount;
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readEntityAction);

    // readMessageAction (OBJCTVMSG's end): the objective its NAME names and its MESSAGE (objectiveMessageType), the
    // next of the actions' messages. 0, ending the reading, when all sixteen are taken, with no NAME or MESSAGE, or
    // with a message that is none of the three (it stays in the place, not counted); else 1. (The action block's
    // owner is asked for too, and not used.)
    inline uint32_t readMessageAction(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[kKeyTextBytes]> key; // the parent's keyword, the key, then MESSAGE's text
        const ee::Ptr<ObjectiveActions> actions(blockClassOwner(beginKeyword));
        const ee::Ptr<ScriptValueSet> values = blockClassValues(beginKeyword);
        blockClassParentName(beginKeyword, key);
        blockClassOwner(key);
        if (actions->messageCount >= kObjectiveMessageActions)
            return 0;
        formatString(key, sym::bmx::objectiveKeyName);
        ee::Ptr<ScriptValue> value = findScriptValue(values, key);
        if (!value)
            return 0;
        const ee::Ptr<ObjectiveMessageAction> message(&actions->messages[actions->messageCount]);
        scriptValueText(value, ee::Ptr<char>(message->objective), kObjectiveNameBytes);
        formatString(key, sym::bmx::objectiveKeyMessage);
        value = findScriptValue(values, key);
        if (!value)
            return 0;
        scriptValueText(value, key, kKeyTextBytes);
        message->message = objectiveMessageType(key);
        if (message->message >= kObjectiveMessageUnknown)
            return 0;
        ++actions->messageCount;
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readMessageAction);
}
