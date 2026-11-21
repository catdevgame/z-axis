// BMX XXX's "RECORD" objective type: one of the game's records (RECORDTYPE:
// SCORE_GAME .. DISTANCE_WALLRIDE) reaching a VALUE. BMX XXX's own code
// (its game block); the names are the script's keywords and the type's
// callbacks' places in its registration.

#include "objective_calls.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/record_type.cpp.h"

namespace game::bmx::objectives
{
    namespace
    {
        // The block's keys: RECORDTYPE, VALUE.
        constexpr uint32_t kRecordKeys = 2;
        // RECORDTYPE's words, in the order of the game's records:
        // SCORE_GAME, SCORE_RUN, SCORE_STACK, SCORE_TRICK, COMBO, SPIN, then
        // the distances: DISTANCE_AIR, _TAP, _GRIND, _JUMP, _MANUAL,
        // _NOSEWHEELIE, _SKID, _WALLRIDE.
        constexpr uint32_t kRecordTypes = 14;
        constexpr uint32_t kFirstDistanceRecord = 6;
        // A distance's VALUE is in feet; the game's records are in metres.
        // 0.3048 as the game has it (the float just under it).
        constexpr ee::Float kMetresPerFoot = std::bit_cast<float>(0x3E9C0EBEu); // not an address

        // A RECORD objective's record (0xC bytes, from the type's pool).
        struct RecordObjective
        {
            uint8_t type;      // +0x00 RECORDTYPE: which record (what the pool left when the script's word is none of them)
            uint8_t _01[3];
            ee::Float value;   // +0x04 VALUE: what the record has to reach
            ee::Float latest;  // +0x08 the record's value as last told
        };
        static_assert(offsetof(RecordObjective, value) == 0x04 && sizeof(RecordObjective) == 0x0C);

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_recordObjectives> g_recordObjectives;
    }

    void readRecordParameters(Objective objective, ee::Ptr<const char> parentKeyword);
    uint32_t readRecordBlock(ee::Ptr<const char> beginKeyword);
    void freeRecord(Objective objective);
    void resetRecord();
    void recordEvent(Objective objective, ee::Ptr<RecordEvent> event);
    void recordCompleted();
    int32_t recordStatusText(Objective objective, ee::Ptr<char> text);

    // registerRecordType: the "RECORD" type and its callbacks.
    void registerRecordType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::recordTypeName);
        setup->hooks.readParameters = sym::readRecordParameters;
        setup->hooks.freeData = sym::freeRecord;
        setup->hooks.resetProgress = sym::resetRecord;
        setup->hooks.onEvent = sym::recordEvent;
        setup->hooks.onComplete = sym::recordCompleted;
        setup->hooks.statusText = sym::recordStatusText;
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerRecordType, Verified);

    // readRecordParameters (the type's readParameters): the block
    // RECORD_BEGIN .. RECORD_END is made readable inside the objective's
    // block, with the objective as its owner.
    void readRecordParameters(Objective objective, ee::Ptr<const char> parentKeyword)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kRecordKeys]> keyNames;
        ee::Local<uint32_t[kRecordKeys]> keyKinds;
        const ee::Ptr<ee::Ptr<const char>> names(sym::recordKeyNames);
        for (uint32_t i = 0; i < kRecordKeys; ++i)
            keyNames[i] = names[i];
        calls::memset(keyKinds, 0, sizeof(uint32_t[kRecordKeys]));
        setup->parentBegin = parentKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->begin = ee::Ptr<const char>(sym::recordBeginKeyword);
        setup->end = ee::Ptr<const char>(sym::recordEndKeyword);
        setup->keyCount = kRecordKeys;
        setup->onEnd = sym::readRecordBlock;
        setup->onBegin = 0;
        zge::registerBlockClass(setup);
        zge::setBlockClassOwner(ee::Ptr<const char>(sym::recordBeginKeyword), objective);
    }
    READABLE_FUNCTION_WAITS(readRecordParameters, Verified);

    // readRecordBlock (the block's handler): the objective's record made
    // from the block's values. RECORDTYPE is looked up among the record
    // types' words; the VALUE of a distance is turned from feet to metres.
    // A block without RECORDTYPE, or with a word that is none of the types,
    // leaves the record as the pool gave it. Always 1.
    uint32_t readRecordBlock(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[0x20]> key;
        ee::Local<char[0x40]> typeName;
        ee::Local<uint32_t[kRecordTypes]> typeNames;
        const ee::Ptr<uint32_t> names(sym::recordTypeNames);
        for (uint32_t i = 0; i < kRecordTypes; ++i)
            typeNames[i] = names[i];
        const Objective owner(zge::blockClassOwner(beginKeyword));
        const ee::Ptr<BlockValues> values = zge::blockClassValues(beginKeyword);
        const ee::Ptr<RecordObjective> record(zge::poolTakeCreating(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_recordObjectives), sizeof(RecordObjective)));

        zge::formatString(key, sym::bmx::recordKeyRecordType);
        ee::Ptr<BlockValue> value = zge::findScriptValue(values, key);
        if (value)
        {
            zge::scriptValueText(value, typeName, sizeof(char[0x40]));
            uint32_t type = 0;
            while (type < kRecordTypes && zge::compareStrings(typeName, typeNames[type]) != 0)
                ++type;
            if (type < kRecordTypes)
            {
                record->type = static_cast<uint8_t>(type);
                zge::formatString(key, sym::bmx::recordKeyValue);
                value = zge::findScriptValue(values, key);
                if (value)
                {
                    const ee::Float asWritten = zge::scriptValueFloat(value);
                    record->value = asWritten;
                    if (record->type < kRecordTypes && record->type >= kFirstDistanceRecord)
                        record->value = asWritten * kMetresPerFoot;
                }
            }
        }
        zge::setObjectiveData(owner, ee::Address(record));
        zge::unregisterBlockClass(ee::Ptr<const char>(sym::recordBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS(readRecordBlock, Verified);

    // freeRecord (the type's free): the record back to the pool.
    void freeRecord(Objective objective)
    {
        const ee::Address record = zge::objectiveData(objective);
        if (record)
            zge::poolGiveDestroying(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_recordObjectives), record);
        calls::setObjectiveDataTail(objective, ee::Address());
    }
    READABLE_FUNCTION_WAITS(freeRecord, Verified);

    // resetRecord (the type's reset): nothing to do.
    void resetRecord()
    {
    }
    READABLE_FUNCTION(resetRecord, Verified);

    // recordEvent (the type's event): the objective's record told: its
    // value is kept, and the objective is done when it has reached VALUE.
    void recordEvent(Objective objective, ee::Ptr<RecordEvent> event)
    {
        const ee::Ptr<RecordObjective> record(zge::objectiveData(objective));
        if (event->kind != kRecordEvent)
            return;
        if (event->record != record->type)
            return;
        const ee::Float value = event->value;
        record->latest = value;
        if (record->value <= value)
            calls::setObjectiveStateTail(objective, kObjectiveDone);
    }
    READABLE_FUNCTION_WAITS(recordEvent, Verified);

    // recordCompleted (the type's completed): nothing to do.
    void recordCompleted()
    {
    }
    READABLE_FUNCTION(recordCompleted, Verified);

    // recordStatusText (the type's statusText): the string table's text
    // "<STRINGID>_STATUS", a format, written with the record's latest value
    // and the value to reach. 0 when the table has no such text.
    int32_t recordStatusText(Objective objective, ee::Ptr<char> text)
    {
        ee::Local<char[0x40]> key;
        const ee::Ptr<RecordObjective> record(zge::objectiveData(objective));
        zge::formatString(key, sym::bmx::recordStatusKeyFormat, zge::objectiveStringId(objective));
        const char *format = zge::stringTableText(zge::nameHash(key));
        if (format == nullptr)
            return 0;
        const uint64_t latest = sce::libgcc::fptodp(record->latest);
        const uint64_t toReach = sce::libgcc::fptodp(record->value);
        return zge::formatString(text, format, latest, toReach);
    }
    READABLE_FUNCTION_WAITS(recordStatusText, Draft);
}
