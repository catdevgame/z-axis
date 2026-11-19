// BMX XXX's "SET" objective type: an objective made of other objectives
// (OBJECTIVE1 .. OBJECTIVE32, each an objective's name), done when
// NUMREQUIRED of them are. Its TYPE says how the members are to be done:
// NotOrdered and NotOrderedExclusive (any of them, in any order), Ordered
// (one after the other: only the next one is active), OrderedStrict (one
// after the other, and the run starts over when it breaks) or Simultaneous
// (all in the same frame). BMX XXX's own code (its game block); the names
// are the script's keywords and the type's callbacks' places in its
// registration.

#include "objective_calls.h"
#include "objectives.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/set_type.cpp.h"

namespace game::bmx::objectives
{
    namespace
    {
        constexpr int32_t kMaxMembers = 32;
        // The block's keys: NUMOBJECTIVES, NUMREQUIRED, TYPE, PLACEHOLDER, OBJECTIVE1 .. OBJECTIVE32.
        constexpr uint32_t kSetKeys = 4 + kMaxMembers;
        constexpr uint32_t kNameBytes = 0x20;

        // How a set's members are to be done: the TYPE line's words, in
        // the order of the game's table of them.
        enum SetOrder : uint32_t
        {
            kNotOrdered = 0,
            kNotOrderedExclusive = 1,
            kOrdered = 2,
            kOrderedStrict = 3,
            kSimultaneous = 4,
            kSetOrders = 5,
        };

        // A SET objective's record (0xB8 bytes, from the type's pool).
        struct SetObjective
        {
            uint32_t order;                    // +0x00 SetOrder (TYPE; NotOrdered when the script gives none)
            uint8_t flags;                     // +0x04 kSetHasStatus
            uint8_t _05[3];
            int32_t count;                     // +0x08 NUMOBJECTIVES, 32 at most (0 when the block was not read whole)
            int32_t required;                  // +0x0C NUMREQUIRED: how many members make the set done (all of them when the script gives none or more than there are)
            int32_t done;                      // +0x10 how many are done, as the last frame's event counted them
            int32_t last;                      // +0x14 an ordered set's newest member done, by index (-1: none yet)
            Objective members[kMaxMembers];    // +0x18
            uint8_t memberFlags[kMaxMembers];  // +0x98 kMemberHasStatus, kMemberIsSet
        };
        static_assert(offsetof(SetObjective, count) == 0x08 && offsetof(SetObjective, last) == 0x14);
        static_assert(offsetof(SetObjective, members) == 0x18 && offsetof(SetObjective, memberFlags) == 0x98);
        static_assert(sizeof(SetObjective) == 0xB8);
        constexpr uint8_t kSetHasStatus = 0x1;    // the string table has a status text for the set itself
        constexpr uint8_t kMemberHasStatus = 0x1; // the string table has a status text for the member
        constexpr uint8_t kMemberIsSet = 0x2;     // the member is a set too
        constexpr int32_t kNoMember = -1;

        // The hash of the type's own name, "SET".
        constexpr uint32_t kSetTypeHash = 0x57A4;

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_setObjectives> g_setObjectives;
        // The block's two keywords, kept as pointers to their texts.
        inline ee::Global<ee::Ptr<const char>, sym::g_setBeginKeyword> g_setBeginKeyword;
        inline ee::Global<ee::Ptr<const char>, sym::g_setEndKeyword> g_setEndKeyword;
        inline ee::Global<uint8_t, sym::g_currentPlayer> g_currentPlayer;

        // Done or complete, asked of the engine state by state (the
        // engine's objectiveFinished, written out where the original has
        // it so).
        bool finished(Objective objective)
        {
            return zge::objectiveState(objective) == kObjectiveDone || zge::objectiveState(objective) == kObjectiveComplete;
        }
    }

    void readSetParameters(Objective objective, ee::Ptr<const char> parentKeyword);
    uint32_t readSetBlock(ee::Ptr<const char> beginKeyword);
    void freeSet(Objective objective);
    void setEvent(Objective objective, ee::Ptr<ObjectiveEvent> event);
    void setActivated(Objective objective, uint32_t bySet);
    void setCompleted(Objective objective);
    void resetSet(Objective objective);
    int32_t setStatusText(Objective objective, ee::Ptr<char> text);
    int32_t setProgress(Objective objective);
    void updateSimultaneousSet(Objective objective, ee::Ptr<SetObjective> set);
    void updateOrderedSet(Objective objective, ee::Ptr<SetObjective> set);
    void updateOrderedStrictSet(Objective objective, ee::Ptr<SetObjective> set);
    uint32_t setContainsObjective(Objective objective, Objective wanted);

    // registerSetType: the "SET" type and its callbacks.
    void registerSetType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::setTypeName);
        setup->hooks.readParameters = sym::readSetParameters;
        setup->hooks.freeData = sym::freeSet;
        setup->hooks.onEvent = sym::setEvent;
        setup->hooks.onActivate = sym::setActivated;
        setup->hooks.onComplete = sym::setCompleted;
        setup->hooks.resetProgress = sym::resetSet;
        setup->hooks.statusText = sym::setStatusText;
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerSetType, Verified);

    // readSetParameters (the type's readParameters): the block
    // SET_BEGIN .. SET_END is made readable inside the objective's block,
    // with the objective as its owner.
    void readSetParameters(Objective objective, ee::Ptr<const char> parentKeyword)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kSetKeys]> keyNames;
        ee::Local<uint32_t[kSetKeys]> keyKinds;
        const ee::Ptr<ee::Ptr<const char>> names(sym::setKeyNames);
        for (uint32_t i = 0; i < kSetKeys; ++i)
            keyNames[i] = names[i];
        calls::memset(keyKinds, 0, sizeof(uint32_t[kSetKeys]));
        setup->parentBegin = parentKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->begin = *g_setBeginKeyword;
        setup->end = *g_setEndKeyword;
        setup->keyCount = kSetKeys;
        setup->onEnd = sym::readSetBlock;
        setup->onBegin = 0;
        zge::registerBlockClass(setup);
        zge::setBlockClassOwner(*g_setBeginKeyword, objective);
    }
    READABLE_FUNCTION_WAITS(readSetParameters, Verified);

    // readSetBlock (the block's handler): the objective's record made from
    // the block's values. Each OBJECTIVE is another objective's name; one
    // that is a set itself is marked so, and so is one the string table
    // has a status text for ("<its STRINGID>_STATUS"), as is the set for
    // its own. NUMREQUIRED is NUMOBJECTIVES at most; TYPE is one of the
    // five order words, whatever their case. A block with no
    // NUMOBJECTIVES, or with an OBJECTIVE missing or naming no objective,
    // leaves the set with no members. Always 1.
    uint32_t readSetBlock(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[kNameBytes]> key;
        ee::Local<char[kNameBytes]> memberName;
        ee::Local<char[0x40]> statusKey;
        ee::Local<Objective> owner;
        bool whole = true;
        *owner = Objective(zge::blockClassOwner(beginKeyword));
        const ee::Ptr<BlockValues> values = zge::blockClassValues(beginKeyword);
        const ee::Ptr<SetObjective> set(zge::poolTakeCreating(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_setObjectives), sizeof(SetObjective)));
        zge::memoryFill(set, 0, sizeof(SetObjective));
        set->last = kNoMember;

        zge::formatString(key, sym::bmx::setKeyNumObjectives);
        ee::Ptr<BlockValue> value = zge::findScriptValue(values, key);
        bool counted = false;
        if (!value)
            whole = false;
        else
        {
            set->count = zge::scriptValueInteger(value);
            counted = set->count != 0;
        }
        if (counted)
        {
            if (set->count > kMaxMembers)
                set->count = kMaxMembers;
            for (int32_t i = 0; i < set->count; ++i)
            {
                zge::formatString(key, sym::bmx::setMemberKeyFormat, i + 1);
                value = zge::findScriptValue(values, key);
                if (!value)
                {
                    whole = false;
                    continue;
                }
                zge::scriptValueText(value, memberName, kNameBytes);
                const Objective member = zge::findObjective(memberName);
                set->members[i] = member;
                if (!member)
                {
                    whole = false;
                    continue;
                }
                if (zge::objectiveTypeHash(member) == kSetTypeHash)
                    set->memberFlags[i] |= kMemberIsSet;
                zge::formatString(statusKey, sym::bmx::setStatusKeyFormat, zge::objectiveStringId(set->members[i]));
                if (zge::stringTableText(zge::nameHash(statusKey)) != nullptr)
                    set->memberFlags[i] |= kMemberHasStatus;
            }

            zge::formatString(statusKey, sym::bmx::setStatusKeyFormat, zge::objectiveStringId(*owner));
            if (zge::stringTableText(zge::nameHash(statusKey)) != nullptr)
                set->flags |= kSetHasStatus;

            zge::formatString(key, sym::bmx::setKeyNumRequired);
            value = zge::findScriptValue(values, key);
            if (!value)
                set->required = set->count;
            else
            {
                const int32_t required = zge::scriptValueInteger(value);
                set->required = required;
                if (required != 0 && set->count < required)
                    set->required = set->count;
            }

            zge::formatString(key, sym::bmx::setKeyType);
            value = zge::findScriptValue(values, key);
            if (value)
            {
                zge::scriptValueText(value, key, kNameBytes);
                const ee::Ptr<ee::Ptr<const char>> orderNames(sym::setOrderNames);
                for (uint32_t order = 0; order < kSetOrders; ++order)
                {
                    if (zge::compareStringsNoCaseN(orderNames[order], key, kNameBytes) == 0)
                        set->order = order;
                }
                // The original compares with the first word once more when
                // the set is left NotOrdered, and does nothing with the
                // answer.
                if (set->order == kNotOrdered)
                    zge::compareStringsNoCaseN(orderNames[kNotOrdered], key, kNameBytes);
            }
        }
        if (!whole)
            set->count = 0;
        zge::setObjectiveData(*owner, ee::Address(set));
        zge::unregisterBlockClass(*g_setBeginKeyword);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readSetBlock, Verified);

    // freeSet (the type's free): the record back to the pool.
    void freeSet(Objective objective)
    {
        const ee::Address set = zge::objectiveData(objective);
        if (set)
            zge::poolGiveDestroying(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_setObjectives), set);
        calls::setObjectiveDataTail(objective, ee::Address());
    }
    READABLE_FUNCTION_WAITS(freeSet, Draft);

    // setEvent (the type's event), on the frame's event while the set is
    // active or in progress: the members done are counted; with
    // NUMREQUIRED of them the set is done, and until then an ordered or a
    // simultaneous set looks after its members.
    void setEvent(Objective objective, ee::Ptr<ObjectiveEvent> event)
    {
        if (zge::objectiveState(objective) != kObjectiveActive && zge::objectiveState(objective) != kObjectiveInProgress)
            return;
        if (event->kind != kFrameEvent)
            return;
        const ee::Ptr<SetObjective> set(zge::objectiveData(objective));
        const int32_t done = setProgress(objective);
        set->done = done;
        if (!(done < set->required))
        {
            calls::setObjectiveStateTail(objective, kObjectiveDone);
            return;
        }
        switch (set->order)
        {
        case kOrdered:
            game::tail(updateOrderedSet, objective, set);
            return;
        case kOrderedStrict:
            game::tail(updateOrderedStrictSet, objective, set);
            return;
        case kSimultaneous:
            game::tail(updateSimultaneousSet, objective, set);
            return;
        default:
            return;
        }
    }
    READABLE_FUNCTION_WAITS(setEvent, Verified);

    // setActivated (the type's activated): the set's members are made
    // active with it; of an Ordered set of two or more, the first alone.
    // Activated by the game, each member the current player may activate
    // is activated for that player. Made active again by a set it belongs
    // to (bySet), each member that is done or complete is made active
    // again in its turn; for the others the original asks the set's state
    // and does nothing with it.
    void setActivated(Objective objective, uint32_t bySet)
    {
        const ee::Ptr<SetObjective> set(zge::objectiveData(objective));
        int32_t count = set->count;
        if (set->order == kOrdered && !(count < 2))
            count = 1;
        for (int32_t i = 0; i < count; ++i)
        {
            if (bySet != 0)
            {
                if (zge::objectiveFinished(set->members[i]) != 0)
                    zge::reactivateObjective(set->members[i]);
                else
                    zge::objectiveState(objective);
            }
            else if (playerCanActivateObjective(currentPlayer(), set->members[i]) != 0)
                activatePlayerObjective(currentPlayer(), set->members[i]);
        }
    }
    READABLE_FUNCTION_WAITS(setActivated, Verified);

    // setCompleted (the type's completed): every member that is neither
    // done nor complete is reset.
    void setCompleted(Objective objective)
    {
        const ee::Ptr<SetObjective> set(zge::objectiveData(objective));
        for (int32_t i = 0; i < set->count; ++i)
        {
            if (!finished(set->members[i]))
                calls::resetObjective(set->members[i]);
        }
    }
    READABLE_FUNCTION_WAITS(setCompleted, Verified);

    // resetSet (the type's reset): an ordered set starts over and every
    // member is reset. A complete member whose saved mark the current
    // player's saved game does not have is first taken out of its
    // complete state, so that the reset does not keep it complete.
    void resetSet(Objective objective)
    {
        const ee::Ptr<SetObjective> set(zge::objectiveData(objective));
        set->last = kNoMember;
        for (int32_t i = 0; i < set->count; ++i)
        {
            if (!(zge::objectiveState(set->members[i]) < kObjectiveComplete) &&
                objectiveSavedMark(*g_currentPlayer, set->members[i]) == 0)
                zge::setObjectiveState(set->members[i], 0);
            calls::resetObjective(set->members[i]);
        }
    }
    READABLE_FUNCTION_WAITS(resetSet, Verified);

    // setStatusText (the type's statusText): the status of the first
    // member that is active or in progress and is a set or has a status
    // text of its own, when that member's type writes one. Otherwise the
    // set's own: the string table's text "<STRINGID>_STATUS", a format,
    // written with how many members are done and how many are required.
    // 0 when there is neither.
    int32_t setStatusText(Objective objective, ee::Ptr<char> text)
    {
        ee::Local<char[0x40]> key;
        const ee::Ptr<SetObjective> set(zge::objectiveData(objective));
        for (uint32_t i = 0; i < static_cast<uint32_t>(set->count); ++i)
        {
            const Objective member = set->members[i];
            if (zge::objectiveState(member) < kObjectiveActive)
                continue;
            if (!(zge::objectiveState(member) < kObjectiveDone))
                continue;
            const uint8_t memberFlags = set->memberFlags[i];
            if ((memberFlags & kMemberIsSet) == 0 && (memberFlags & kMemberHasStatus) == 0)
                continue;
            const int32_t written = zge::objectiveStatusText(member, text);
            if (written != 0)
                return written;
        }
        if ((set->flags & kSetHasStatus) == 0)
            return 0;
        zge::formatString(key, sym::bmx::setStatusKeyFormat, zge::objectiveStringId(objective));
        const char *format = zge::stringTableText(zge::nameHash(key));
        if (format == nullptr)
            return 0;
        return zge::formatString(text, format, set->done, set->required);
    }
    READABLE_FUNCTION_WAITS(setStatusText, Verified);

    // setProgress: how many of the set's members count as done: of an
    // ordered set those up to the newest one done in order, of the others
    // every member that is done or complete.
    int32_t setProgress(Objective objective)
    {
        const ee::Ptr<SetObjective> set(zge::objectiveData(objective));
        int32_t done = 0;
        switch (set->order)
        {
        case kNotOrdered:
        case kNotOrderedExclusive:
        case kSimultaneous:
            for (int32_t i = 0; i < set->count; ++i)
                done += finished(set->members[i]) ? 1 : 0;
            break;
        case kOrdered:
        case kOrderedStrict:
            done = set->last + 1;
            break;
        default:
            break;
        }
        return done;
    }
    READABLE_FUNCTION_WAITS(setProgress, Verified);

    // updateSimultaneousSet: the set is not done this frame, so every
    // member that is done or complete is made active again: the members
    // count only when enough of them are done in one frame. (For the
    // others the original asks the set's state and does nothing with it.)
    void updateSimultaneousSet(Objective objective, ee::Ptr<SetObjective> set)
    {
        for (int32_t i = 0; i < set->count; ++i)
        {
            if (zge::objectiveFinished(set->members[i]) != 0)
                zge::reactivateObjective(set->members[i]);
            else
                zge::objectiveState(objective);
        }
    }
    READABLE_FUNCTION_WAITS(updateSimultaneousSet, Verified);

    // updateOrderedSet: the run of members done moves on past every member
    // that is done or complete, and the member after it is activated for
    // the current player when that player may activate it and it is not
    // active or in progress yet. (The original then asks the state of
    // every member after that one and does nothing with it.)
    void updateOrderedSet(Objective objective, ee::Ptr<SetObjective> set)
    {
        (void)objective;
        int32_t next = set->last + 1;
        while (next < set->count && finished(set->members[next]))
            ++next;
        set->last = next - 1;
        if (next < set->count &&
            playerCanActivateObjective(currentPlayer(), set->members[next]) != 0 &&
            zge::objectiveState(set->members[next]) != kObjectiveActive &&
            zge::objectiveState(set->members[next]) != kObjectiveInProgress)
            activatePlayerObjective(currentPlayer(), set->members[next]);
        for (int32_t later = set->last + 2; later < set->count; ++later)
            zge::objectiveState(set->members[later]);
    }
    READABLE_FUNCTION_WAITS(updateOrderedSet, Verified);

    // updateOrderedStrictSet: when the member after the newest one done is
    // done or complete, the run moves on to it; when it is not and the
    // newest one done is no longer done or complete either, the run starts
    // over. Then every member from the newest one done on that is done or
    // complete is made active again. (For the others, and before the first
    // member when the run has none, the original asks the set's state and
    // does nothing with it. The member after the newest is read without a
    // look at how many members there are.)
    void updateOrderedStrictSet(Objective objective, ee::Ptr<SetObjective> set)
    {
        int32_t from;
        if (finished(set->members[set->last + 1]))
        {
            set->last = set->last + 1;
            from = set->last;
        }
        else
        {
            from = set->last;
            if (from != kNoMember)
            {
                if (finished(set->members[from]))
                    from = set->last;
                else
                {
                    set->last = kNoMember;
                    from = kNoMember;
                }
            }
        }
        for (int32_t i = from; i < set->count; ++i)
        {
            if (i >= 0 && zge::objectiveFinished(set->members[i]) != 0)
                zge::reactivateObjective(set->members[i]);
            else
                zge::objectiveState(objective);
        }
    }
    READABLE_FUNCTION_WAITS(updateOrderedStrictSet, Verified);

    // setContainsObjective: 1 when the objective is a set that has
    // `wanted` among its members, or among those of a member that is a
    // set itself.
    uint32_t setContainsObjective(Objective objective, Objective wanted)
    {
        if (zge::objectiveTypeHash(objective) != kSetTypeHash)
            return 0;
        const ee::Ptr<SetObjective> set(zge::objectiveData(objective));
        for (int32_t i = 0; i < set->count; ++i)
        {
            if (set->members[i] == wanted)
                return 1;
            if (setContainsObjective(set->members[i], wanted) != 0)
                return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(setContainsObjective, Verified);
}
