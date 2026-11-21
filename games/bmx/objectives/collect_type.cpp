// BMX XXX's "COLLECT" objective type: a list of level objects (ITEM1 ..
// ITEM64) the rider activates, all of them or NUMREQUIRED of them, in any
// order or one after the other (SEQUENTIAL). BMX XXX's own code (its game
// block); the names are the script's keywords and the type's callbacks'
// places in its registration.
//
// Unless the script says NO_MANAGE the objective manages its items itself:
// they are enabled while it is active (the next one alone when sequential)
// and disabled before and after.

#include "objective_calls.h"

#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/collect_type.cpp.h"

namespace game::bmx::objectives
{
    namespace
    {
        constexpr uint32_t kMaxItems = 64;
        // The block's keys: NUMITEMS, NUMREQUIRED, SEQUENTIAL, NO_MANAGE, ITEM1 .. ITEM64.
        constexpr uint32_t kCollectKeys = 4 + kMaxItems;
        constexpr uint32_t kItemNameBytes = 0x40;

        // A COLLECT objective's record (0x110 bytes, from the type's pool).
        struct CollectObjective
        {
            uint8_t flags;                              // +0x00 kSequential, kNoManage
            uint8_t _01;
            uint8_t itemCount;                          // +0x02 NUMITEMS (0 when the block was not read whole)
            uint8_t required;                           // +0x03 NUMREQUIRED (the item count when the script has none)
            uint8_t collected;                          // +0x04 how many so far: the next item's index when sequential
            uint8_t collectedBits[kMaxItems / 8];       // +0x05 a bit per item collected
            uint8_t _0d[3];
            ee::Ptr<zge::SceneObject> items[kMaxItems]; // +0x10 ITEM1 .. (0: no object of that name)
        };
        static_assert(offsetof(CollectObjective, collectedBits) == 0x05 && offsetof(CollectObjective, items) == 0x10 && sizeof(CollectObjective) == 0x110);
        constexpr uint8_t kSequential = 0x1; // SEQUENTIAL
        constexpr uint8_t kNoManage = 0x2;   // NO_MANAGE

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_collectObjectives> g_collectObjectives;
        // Whose turn it is (bmx.c's).
        inline ee::Global<uint8_t, sym::g_currentPlayer> g_currentPlayer;

        // The highlight's colour for an item still to collect.
        constexpr uint32_t kItemHighlight = highlightColour(0x40, 0xD0, 0x60);

        ee::Ptr<CollectObjective> collectOf(Objective objective)
        {
            return ee::Ptr<CollectObjective>(zge::objectiveData(objective));
        }

        bool isCollected(const CollectObjective &collect, uint32_t item)
        {
            return ((collect.collectedBits[item >> 3] >> (item & 7)) & 1) != 0;
        }
    }

    void readCollectParameters(Objective objective, ee::Ptr<const char> parentKeyword);
    uint32_t readCollectBlock(ee::Ptr<const char> beginKeyword);
    void freeCollect(Objective objective);
    void resetCollect(Objective objective);
    void collectEvent(Objective objective, ee::Ptr<ObjectActivatedEvent> event);
    void collectActivated(Objective objective, uint32_t again);
    void collectCompleted(Objective objective);
    void highlightCollect(Objective objective, ee::Ptr<rider::Rider> rider);
    int32_t collectStatusText(Objective objective, ee::Ptr<char> text);

    // registerCollectType: the "COLLECT" type and its callbacks.
    void registerCollectType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::collectTypeName);
        setup->hooks.readParameters = sym::readCollectParameters;
        setup->hooks.freeData = sym::freeCollect;
        setup->hooks.resetProgress = sym::resetCollect;
        setup->hooks.onEvent = sym::collectEvent;
        setup->hooks.onActivate = sym::collectActivated;
        setup->hooks.onComplete = sym::collectCompleted;
        setup->hooks.drawForRider = sym::highlightCollect;
        setup->hooks.statusText = sym::collectStatusText;
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerCollectType, Verified);

    // readCollectParameters (the type's readParameters): the block
    // COLLECT_BEGIN .. COLLECT_END is made readable inside the objective's
    // block, with the objective as its owner.
    void readCollectParameters(Objective objective, ee::Ptr<const char> parentKeyword)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kCollectKeys]> keyNames;
        ee::Local<uint32_t[kCollectKeys]> keyKinds;
        const ee::Ptr<ee::Ptr<const char>> names(sym::collectKeyNames);
        for (uint32_t i = 0; i < kCollectKeys; ++i)
            keyNames[i] = names[i];
        calls::memset(keyKinds, 0, sizeof(uint32_t[kCollectKeys]));
        setup->parentBegin = parentKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->begin = ee::Ptr<const char>(sym::collectBeginKeyword);
        setup->end = ee::Ptr<const char>(sym::collectEndKeyword);
        setup->keyCount = kCollectKeys;
        setup->onEnd = sym::readCollectBlock;
        setup->onBegin = 0;
        zge::registerBlockClass(setup);
        zge::setBlockClassOwner(ee::Ptr<const char>(sym::collectBeginKeyword), objective);
    }
    READABLE_FUNCTION_WAITS(readCollectParameters, Verified);

    // readCollectBlock (the block's handler): the objective's record made
    // from the block's values. An item is a level object's name. A block
    // with no NUMITEMS, or with an item missing or naming no object, leaves
    // the objective with no items. Always 1.
    uint32_t readCollectBlock(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[0x20]> key;
        ee::Local<char[kMaxItems][kItemNameBytes]> itemNames;
        ee::Local<Objective> owner;
        bool whole = true;
        *owner = Objective(zge::blockClassOwner(beginKeyword));
        const ee::Ptr<BlockValues> values = zge::blockClassValues(beginKeyword);
        const ee::Ptr<CollectObjective> collect(zge::poolTakeCreating(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_collectObjectives), sizeof(CollectObjective)));
        collect->itemCount = 0;
        collect->flags &= static_cast<uint8_t>(~(kSequential | kNoManage));

        zge::formatString(key, sym::bmx::collectKeyNumItems);
        ee::Ptr<BlockValue> value = zge::findScriptValue(values, key);
        bool counted = false;
        if (!value)
            whole = false;
        else
        {
            const int32_t count = zge::scriptValueInteger(value);
            collect->itemCount = static_cast<uint8_t>(count);
            counted = collect->itemCount != 0;
        }
        if (counted)
        {
            for (uint32_t i = 0; i < collect->itemCount; ++i)
            {
                zge::formatString(key, sym::bmx::collectItemKeyFormat, i + 1);
                value = zge::findScriptValue(values, key);
                if (value)
                {
                    const ee::Ptr<char> name(itemNames.address() + i * kItemNameBytes);
                    zge::scriptValueText(value, name, kItemNameBytes);
                    const ee::Ptr<zge::SceneObject> item = zge::findObjectByName(ee::Address(name));
                    if (!item)
                        whole = false;
                    collect->items[i] = item;
                }
                else
                    whole = false;
            }

            zge::formatString(key, sym::bmx::collectKeyNumRequired);
            value = zge::findScriptValue(values, key);
            if (value)
                collect->required = static_cast<uint8_t>(zge::scriptValueInteger(value));
            else
                collect->required = collect->itemCount;

            zge::formatString(key, sym::bmx::collectKeySequential);
            value = zge::findScriptValue(values, key);
            if (value)
            {
                const uint32_t on = zge::scriptValueSwitch(value, collect->flags & kSequential);
                collect->flags = static_cast<uint8_t>((collect->flags & ~kSequential) | (on & 1));
            }

            zge::formatString(key, sym::bmx::collectKeyNoManage);
            value = zge::findScriptValue(values, key);
            if (value)
            {
                const uint32_t on = zge::scriptValueSwitch(value, (collect->flags >> 1) & 1);
                collect->flags = static_cast<uint8_t>((collect->flags & ~kNoManage) | ((on & 1) << 1));
            }
        }
        collect->collected = 0;
        zge::memoryFill(collect->collectedBits, 0, sizeof(collect->collectedBits));
        if (!whole)
            collect->itemCount = 0;
        zge::setObjectiveData(*owner, ee::Address(collect));
        zge::unregisterBlockClass(ee::Ptr<const char>(sym::collectBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS(readCollectBlock, Verified);

    // freeCollect (the type's free): the record back to the pool.
    void freeCollect(Objective objective)
    {
        const ee::Address collect = zge::objectiveData(objective);
        if (collect)
            zge::poolGiveDestroying(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_collectObjectives), collect);
        calls::setObjectiveDataTail(objective, ee::Address());
    }
    READABLE_FUNCTION_WAITS(freeCollect, Verified);

    // resetCollect (the type's reset): nothing collected, and the items put
    // in the state the objective's own state asks for: disabled while it is
    // not active yet; enabled while it is active or in progress (when
    // sequential the first alone, the others disabled); and when it is done
    // or complete, activated outright in the current player's name. The
    // first two only when the objective manages its items.
    void resetCollect(Objective objective)
    {
        ee::Local<EntityActivation> activation;
        ee::Local<zge::EngineMessage> message;
        const ee::Ptr<CollectObjective> collect = collectOf(objective);
        collect->collected = 0;
        zge::memoryFill(collect->collectedBits, 0, sizeof(collect->collectedBits));
        message->flags = zge::kMessageNow;
        message->sender = 0;
        message->data = ee::Address();
        switch (zge::objectiveState(objective))
        {
        case 0:
        case 1:
            if ((collect->flags & kNoManage) != 0)
                break;
            message->type = kEntityDisableMessage;
            for (uint32_t i = 0; i < collect->itemCount; ++i)
            {
                message->target = collect->items[i];
                zge::sendEngineMessage(message);
            }
            break;
        case kObjectiveActive:
        case kObjectiveInProgress:
            if ((collect->flags & kNoManage) != 0)
                break;
            for (uint32_t i = 0; i < collect->itemCount; ++i)
            {
                if ((collect->flags & kSequential) != 0 && i != collect->collected)
                    message->type = kEntityDisableMessage;
                else
                    message->type = kEntityEnableMessage;
                message->target = collect->items[i];
                zge::sendEngineMessage(message);
            }
            break;
        case kObjectiveDone:
        case kObjectiveComplete:
            message->data = activation;
            message->type = kEntityActivateMessage;
            zge::memoryFill(activation, 0, sizeof(EntityActivation));
            activation->quiet = 1;
            activation->player = *g_currentPlayer;
            for (uint32_t i = 0; i < collect->itemCount; ++i)
            {
                message->target = collect->items[i];
                zge::sendEngineMessage(message);
            }
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION_WAITS(resetCollect, Verified);

    // collectEvent (the type's event), while active or in progress: a rider
    // activating one of the items collects it, unless it is collected
    // already or, when sequential, is not the next one. The objective sends
    // the activation on itself; with the required number collected it is
    // done, else when it is sequential and manages its items the next one
    // is enabled. Either way the item's activation is taken from the game.
    void collectEvent(Objective objective, ee::Ptr<ObjectActivatedEvent> event)
    {
        ee::Local<zge::EngineMessage> enable;
        const uint32_t state = zge::objectiveState(objective);
        if (state >= 4 || state < 2)
            return;
        const ee::Ptr<CollectObjective> collect = collectOf(objective);
        if (event->kind != kObjectActivatedEvent)
            return;
        if (event->activation->type != kEntityActivateMessage)
            return;
        const uint32_t count = collect->itemCount;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (event->object != collect->items[i])
                continue;
            if (!isCollected(*collect, i) && ((collect->flags & kSequential) == 0 || i == collect->collected))
            {
                event->delivered = 1;
                zge::sendEngineMessage(event->activation);
                collect->collectedBits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
                collect->collected = static_cast<uint8_t>(collect->collected + 1);
                if (collect->collected == collect->required)
                    zge::setObjectiveState(objective, kObjectiveDone);
                else if ((collect->flags & (kSequential | kNoManage)) == kSequential)
                {
                    enable->target = collect->items[collect->collected];
                    enable->flags = zge::kMessageNow;
                    enable->type = kEntityEnableMessage;
                    enable->sender = 0;
                    enable->data = ee::Address();
                    zge::sendEngineMessage(enable);
                }
            }
            event->taken = 1;
            return;
        }
    }
    READABLE_FUNCTION_WAITS(collectEvent, Verified);

    // collectActivated (the type's activated): made active again, the
    // objective starts over.
    void collectActivated(Objective objective, uint32_t again)
    {
        if (again != 0)
            game::tail(resetCollect, objective);
    }
    READABLE_FUNCTION_WAITS(collectActivated, Verified);

    // collectCompleted (the type's completed): the items disabled, when the
    // objective manages them.
    void collectCompleted(Objective objective)
    {
        ee::Local<zge::EngineMessage> message;
        const ee::Ptr<CollectObjective> collect = collectOf(objective);
        if ((collect->flags & kNoManage) != 0)
            return;
        message->flags = zge::kMessageNow;
        message->type = kEntityDisableMessage;
        message->sender = 0;
        message->data = ee::Address();
        for (uint32_t i = 0; i < collect->itemCount; ++i)
        {
            message->target = collect->items[i];
            zge::sendEngineMessage(message);
        }
    }
    READABLE_FUNCTION_WAITS(collectCompleted, Verified);

    // highlightCollect (the type's highlight): every item still to collect
    // is highlighted for the rider (when sequential, the next one alone).
    void highlightCollect(Objective objective, ee::Ptr<rider::Rider> rider)
    {
        const ee::Ptr<CollectObjective> collect = collectOf(objective);
        for (uint32_t i = 0; i < collect->itemCount; ++i)
        {
            if (isCollected(*collect, i))
                continue;
            if ((collect->flags & kSequential) != 0 && i != collect->collected)
                continue;
            highlightObject(collect->items[i], &rider->position, kItemHighlight);
        }
    }
    READABLE_FUNCTION_WAITS(highlightCollect, Draft);

    // collectStatusText (the type's statusText): the string table's text
    // "<STRINGID>_STATUS", a format, written with how many are collected
    // and how many are required. 0 when the table has no such text.
    int32_t collectStatusText(Objective objective, ee::Ptr<char> text)
    {
        ee::Local<char[0x40]> key;
        const ee::Ptr<CollectObjective> collect = collectOf(objective);
        zge::formatString(key, sym::bmx::collectStatusKeyFormat, zge::objectiveStringId(objective));
        const char *format = zge::stringTableText(zge::nameHash(key));
        if (format == nullptr)
            return 0;
        return zge::formatString(text, format, collect->collected, collect->required);
    }
    READABLE_FUNCTION_WAITS(collectStatusText, Verified);
}
