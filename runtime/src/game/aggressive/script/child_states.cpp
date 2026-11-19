// The child states (child_states.h): the list the natives add to, its copy
// in a saved game and the level's objects set back from it on a load. The
// list's nodes come from the level script's pool, 20 bytes each.

#include "child_states.h"

#include "script.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/platform/platform.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"
#include "zge/collision/collision.h"
#include "zge/scene/scene.h"
#include "aggressive/timers.h"
#include "aggressive/zone_memory.h"

#include <cstddef>
#include "sym/aggressive/script/child_states.cpp.h"

namespace aggressive::script
{
    namespace
    {
        inline ee::Global<uint32_t, sym::g_hidePointsTimer> g_hidePointsTimer; // challenge.cpp
        inline ee::Global<uint32_t, sym::g_childStateCount> g_childStateCount;
        inline ee::Global<ee::Ptr<ChildState>, sym::g_childStateFirst> g_childStateFirst;
        inline ee::Global<ee::Ptr<ChildState>, sym::g_childStateLast> g_childStateLast;
        inline ee::Global<uint32_t, sym::g_levelScriptPool> g_levelScriptPool;

        ee::Ptr<ChildState> takeState()
        {
            return ee::Ptr<ChildState>(aggressive::poolTake(*g_levelScriptPool));
        }

        // The state's fields from the object's (shown goes for collides too).
        void fillObjectState(ee::Ptr<ChildState> state, uint32_t objectHash, uint32_t shown, uint32_t keepChildren)
        {
            state->objectHash = objectHash;
            state->childHash = 0;
            state->next = ee::Ptr<ChildState>();
            const uint32_t shownBits = shown ? kChildShown | kChildCollides : 0u;
            state->flags = (state->flags & ~kChildStateFlags) | shownBits | kChildWholeTree | (keepChildren ? kChildFlagKeep : 0u);
        }

        void fillChildState(ee::Ptr<ChildState> state, uint32_t objectHash, ee::Ptr<ChildProperties> properties)
        {
            state->objectHash = objectHash;
            state->childHash = properties->childHash;
            state->next = ee::Ptr<ChildState>();
            state->flags = (state->flags & ~kChildStateFlags) | (properties->shown ? kChildShown : 0u) |
                           (properties->collides ? kChildCollides : 0u) | (properties->wholeTree ? kChildWholeTree : 0u) | kChildFlagKeep;
        }

        // On the end of the list.
        void append(ee::Ptr<ChildState> state, uint32_t appendBlock, uint32_t doneBlock)
        {
            if (*g_childStateCount == 0)
            {
                *g_childStateLast = state;
                *g_childStateFirst = state;
                state->previous = ee::Ptr<ChildState>();
            }
            else
            {
                const ee::Ptr<ChildState> last = *g_childStateLast;
                state->previous = last;
                last->next = state;
                *g_childStateLast = state;
            }
            ++*g_childStateCount;
        }

        // The whole list back to the pool, first to last.
        void giveStatesBack()
        {
            ee::Ptr<ChildState> state = *g_childStateFirst;
            while (state)
            {
                const ee::Ptr<ChildState> next = state->next;
                aggressive::poolGive(*g_levelScriptPool, state);
                *g_childStateFirst = next;
                state = next;
            }
        }
    }

    // setChildShownByHash, a visitor over a node's subtree: on the named child the
    // shown and collision settings, for it alone or its whole subtree;
    // 0 stops the walk there.
    int32_t setChildShownByHash(ee::Ptr<zge::Node> node, ee::Ptr<ChildProperties> properties)
    {
        if (node->nameHash != properties->childHash)
            return 1;
        const uint32_t wholeTree = properties->wholeTree;
        const uint32_t collides = properties->collides;
        zge::setNodeShown(node, properties->shown, wholeTree);
        zge::setNodeCollides(node, collides, wholeTree);
        return 0;
    }
    READABLE_FUNCTION_WAITS(setChildShownByHash, Verified);

    // saveChildStates: the list as records for a saved game, in a new block;
    // *bytes says how long it is.
    uint32_t saveChildStates(ee::Ptr<uint32_t> bytes)
    {
        const uint32_t size = *g_childStateCount * sizeof(SavedChildState);
        *bytes = size;
        const uint32_t records = zge::allocateMemory(size);
        ee::Ptr<ChildState> state = *g_childStateFirst;
        ee::Ptr<SavedChildState> record(records);
        while (state)
        {
            record->objectHash = state->objectHash;
            record->childHash = state->childHash;
            record->flags = (record->flags & ~kChildStateFlags) | (state->flags & kChildStateFlags);
            state = state->next;
            ++record;
            state;
        }
        return records;
    }
    READABLE_FUNCTION_WAITS(saveChildStates, Verified);

    // restoreChildStates: a saved game's records become the list again and the
    // level's objects are set from it. An object's state sets its whole
    // tree, then, unless it keeps its children that way, the object's own
    // node goes back to what it was.
    void restoreChildStates(ee::Ptr<SavedChildState> records, uint32_t bytes)
    {
        ee::Local<ChildProperties> properties;
        if (!bytes)
            return;
        if (*g_childStateCount)
        {
            giveStatesBack();
            *g_childStateLast = ee::Ptr<ChildState>();
        }

        // The records become the list.
        const uint32_t count = bytes / sizeof(SavedChildState);
        *g_childStateCount = count;
        ee::Ptr<ChildState> last;
        for (uint32_t index = 0; index < count; ++index)
        {
            const ee::Ptr<ChildState> state = takeState();
            const SavedChildState &record = records[index];
            state->objectHash = record.objectHash;
            state->childHash = record.childHash;
            state->flags = (state->flags & ~kChildStateFlags) | (record.flags & kChildStateFlags);
            state->next = ee::Ptr<ChildState>();
            if (index == 0)
            {
                *g_childStateFirst = state;
                state->previous = ee::Ptr<ChildState>();
            }
            else
            {
                last->next = state;
                state->previous = last;
            }
            last = state;
        }
        *g_childStateLast = last;

        // The level's objects follow the list.
        for (ee::Ptr<ChildState> state = *g_childStateFirst; state; state = state->next)
        {
            const bool wholeObject = state->childHash == 0;
            if (!wholeObject)
            {
                properties->childHash = state->childHash;
                properties->shown = state->flags & kChildShown;
                properties->collides = (state->flags & kChildCollides) ? 1u : 0u;
                properties->wholeTree = (state->flags & kChildWholeTree) ? 1u : 0u;
            }
            const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(state, 1); // the hash is the state's first word
            if (!found)
                continue;
            const ee::Ptr<zge::Node> node(zge::objectLevelNode(*found));
            if (!node)
                continue;
            if (!wholeObject)
            {
                zge::forEachNodeInSubtree(node, kSetChildShownByHash, properties);
                continue;
            }
            const uint32_t wasShown = node->flags & zge::kNodeShown;
            zge::setNodeShown(node, state->flags & kChildShown, 1);
            zge::setNodeCollides(node, state->flags & kChildShown, 1);
            if (!(state->flags & kChildFlagKeep))
            {
                zge::setNodeShown(node, wasShown, 0);
                zge::setNodeCollides(node, wasShown, 0);
            }
        }
    }
    READABLE_FUNCTION_WAITS(restoreChildStates, Verified);

    // queueObjectVisibility (setObjectVisible): the object's state, replaced if it
    // has one, moved to the end of the list.
    void queueObjectVisibility(uint32_t objectHash, uint32_t visible, uint32_t keepChildren)
    {
        namespace here = points::queueObjectVisibility;
        if (*g_childStateCount == 0)
        {
            const ee::Ptr<ChildState> state = takeState();
            *g_childStateLast = state;
            *g_childStateFirst = state;
            fillObjectState(state, objectHash, visible, keepChildren);
            state->previous = ee::Ptr<ChildState>();
            ++*g_childStateCount;
            return;
        }
        ee::Ptr<ChildState> state = *g_childStateFirst;
        const uint32_t shownBit = visible ? 1u : 0u, keepBit = keepChildren ? 1u : 0u;
        bool found = false;
        if (state)
        {
            for (;;)
            {
                if (state->objectHash != objectHash)
                    state = state->next;
                else if (state->childHash != 0)
                    state = state->next;
                else
                {
                    const ee::Ptr<ChildState> before = state->previous, after = state->next;
                    if (before)
                        before->next = after;
                    if (after)
                        after->previous = before;
                    if (state == *g_childStateLast)
                        *g_childStateLast = before;
                    if (state == *g_childStateFirst)
                        *g_childStateFirst = after;
                    found = true;
                    --*g_childStateCount;
                }
                if (!state || found)
                    break;
            }
        }
        if (!found)
            state = takeState();
        fillObjectState(state, objectHash, shownBit, keepBit);
        append(state, here::taken8, here::taken9);
    }
    READABLE_FUNCTION_WAITS(queueObjectVisibility, Verified);

    // recordChildState (setChildVisible, setChildProperties): the child's state,
    // replaced if it has one, moved to the end of the list. A state set for
    // the whole tree is not replaced by one for the child alone: that one
    // goes on the end as well, and both stay.
    void recordChildState(uint32_t objectHash, ee::Ptr<ChildProperties> properties)
    {
        namespace here = points::recordChildState;
        if (*g_childStateCount == 0)
        {
            const ee::Ptr<ChildState> state = takeState();
            *g_childStateLast = state;
            *g_childStateFirst = state;
            fillChildState(state, objectHash, properties);
            state->previous = ee::Ptr<ChildState>();
            ++*g_childStateCount;
            return;
        }
        ee::Ptr<ChildState> state = *g_childStateFirst;
        bool found = false, unlinked = false;
        if (state)
        {
            uint32_t count = *g_childStateCount;
            ee::Ptr<ChildState> first = state, last = *g_childStateLast;
            for (;;)
            {
                if (state->objectHash != objectHash || state->childHash != properties->childHash)
                    state = state->next;
                else if ((state->flags & kChildWholeTree) && !properties->wholeTree)
                    found = true;
                else
                {
                    const ee::Ptr<ChildState> before = state->previous, after = state->next;
                    if (before)
                        before->next = after;
                    if (after)
                        after->previous = before;
                    if (state == last)
                        last = before;
                    if (state == first)
                        first = after;
                    --count;
                    found = unlinked = true;
                }
                if (!state || found)
                    break;
            }
            *g_childStateCount = count;
            *g_childStateFirst = first;
            *g_childStateLast = last;
        }
        if (!unlinked)
            state = takeState();
        fillChildState(state, objectHash, properties);
        append(state, here::taken11, here::taken12);
    }
    READABLE_FUNCTION_WAITS(recordChildState, Verified);

    // unloadLevelScript: the level's script gone: its code, its natives, the
    // timer that hides the points panel, the child states and their pool.
    int32_t unloadLevelScript()
    {
        scriptUnload();
        scriptFreeNatives();
        const uint32_t timer = *g_hidePointsTimer;
        if (timer)
        {
            releaseTimer(timer);
            *g_hidePointsTimer = 0;
        }
        if (*g_childStateCount)
        {
            giveStatesBack();
            *g_childStateLast = ee::Ptr<ChildState>();
            *g_childStateCount = 0;
        }
        return game::tail(aggressive::freePool, *g_levelScriptPool);
    }
    READABLE_FUNCTION_WAITS(unloadLevelScript, Verified);
}
