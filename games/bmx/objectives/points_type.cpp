// BMX XXX's "POINTS" objective type: MIN_POINTS scored in trick stacks,
// by tricks of one TRICK_TYPE (NONE, AIR, GRIND, STALL, TAP, MANUAL,
// FLATLAND) or, with no type, by any. BMX XXX's own code (its game block);
// the names are the script's keywords and the type's callbacks' places in
// its registration.

#include "objective_calls.h"

#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/points_type.cpp.h"

namespace game::bmx::objectives
{
    namespace
    {
        // The block's keys: MIN_POINTS, TRICK_TYPE.
        constexpr uint32_t kPointsKeys = 2;

        // A POINTS objective's record (0xC bytes, from the type's pool).
        struct PointsObjective
        {
            uint32_t minPoints; // +0x00 MIN_POINTS
            uint32_t points;    // +0x04 scored so far
            uint32_t trickType; // +0x08 TRICK_TYPE (TrickType); kTrickTypes: any trick counts
        };
        static_assert(sizeof(PointsObjective) == 0x0C);

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_pointsObjectives> g_pointsObjectives;

        // TRICK_TYPE's words, by the hash of each.
        struct TrickTypeWord
        {
            uint32_t nameHash; // +0x00
            uint32_t type;     // +0x04 TrickType
        };
        static_assert(sizeof(TrickTypeWord) == 8);
    }

    void readPointsParameters(Objective objective, ee::Ptr<const char> parentKeyword);
    uint32_t readPointsBlock(ee::Ptr<const char> beginKeyword);
    void freePoints(Objective objective);
    void resetPoints(Objective objective);
    void pointsEvent(Objective objective, ee::Ptr<RecordEvent> event);
    int32_t pointsStatusText(Objective objective, ee::Ptr<char> text);
    uint32_t parseTrickType(ee::Ptr<const char> word, ee::Address objective);

    // registerPointsType: the "POINTS" type and its callbacks.
    void registerPointsType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::pointsTypeName);
        setup->hooks.readParameters = sym::readPointsParameters;
        setup->hooks.freeData = sym::freePoints;
        setup->hooks.onEvent = sym::pointsEvent;
        setup->hooks.resetProgress = sym::resetPoints;
        setup->hooks.statusText = sym::pointsStatusText;
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerPointsType, Verified);

    // readPointsParameters (the type's readParameters): the block
    // POINTS_BEGIN .. POINTS_END is made readable inside the objective's
    // block, with the objective as its owner.
    void readPointsParameters(Objective objective, ee::Ptr<const char> parentKeyword)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kPointsKeys]> keyNames;
        ee::Local<uint32_t[kPointsKeys]> keyKinds;
        const ee::Ptr<ee::Ptr<const char>> names(sym::pointsKeyNames);
        for (uint32_t i = 0; i < kPointsKeys; ++i)
            keyNames[i] = names[i];
        calls::memset(keyKinds, 0, sizeof(uint32_t[kPointsKeys]));
        setup->parentBegin = parentKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->begin = ee::Ptr<const char>(sym::pointsBeginKeyword);
        setup->end = ee::Ptr<const char>(sym::pointsEndKeyword);
        setup->keyCount = kPointsKeys;
        setup->onEnd = sym::readPointsBlock;
        setup->onBegin = 0;
        zge::registerBlockClass(setup);
        zge::setBlockClassOwner(ee::Ptr<const char>(sym::pointsBeginKeyword), objective);
    }
    READABLE_FUNCTION_WAITS(readPointsParameters, Verified);

    // readPointsBlock (the block's handler): the objective's record made
    // from the block's values: nothing scored, any trick type unless the
    // block names one. Always 1.
    uint32_t readPointsBlock(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[0x20]> typeWord;
        const Objective owner(zge::blockClassOwner(beginKeyword));
        const ee::Ptr<BlockValues> values = zge::blockClassValues(beginKeyword);
        const ee::Ptr<PointsObjective> points(zge::poolTakeCreating(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_pointsObjectives), sizeof(PointsObjective)));
        zge::memoryFill(points, 0, sizeof(PointsObjective));
        points->trickType = kTrickTypes;
        ee::Ptr<BlockValue> value = zge::findScriptValue(values, ee::Ptr<const char>(sym::pointsKeyMinPoints));
        if (value)
            points->minPoints = static_cast<uint32_t>(zge::scriptValueInteger(value));
        value = zge::findScriptValue(values, ee::Ptr<const char>(sym::pointsKeyTrickType));
        if (value)
        {
            zge::scriptValueText(value, typeWord, sizeof(char[0x20]));
            parseTrickType(typeWord, points);
        }
        zge::setObjectiveData(owner, ee::Address(points));
        zge::unregisterBlockClass(ee::Ptr<const char>(sym::pointsBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS(readPointsBlock, Verified);

    // freePoints (the type's free): the record back to the pool.
    void freePoints(Objective objective)
    {
        const ee::Address points = zge::objectiveData(objective);
        if (points)
            zge::poolGiveDestroying(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_pointsObjectives), points);
        calls::setObjectiveDataTail(objective, ee::Address());
    }
    READABLE_FUNCTION_WAITS(freePoints, Verified);

    // pointsEvent (the type's event): a trick stack's score told (the
    // SCORE_STACK record). With a trick type, the points of every trick of
    // that type in the rider's trick lines are added; with none, the
    // stack's score. With MIN_POINTS reached the objective is done.
    void pointsEvent(Objective objective, ee::Ptr<RecordEvent> event)
    {
        const ee::Ptr<PointsObjective> points(zge::objectiveData(objective));
        if (event->kind != kRecordEvent)
            return;
        if (event->record != kStackScoreRecord)
            return;
        if (points->trickType < kTrickTypes)
        {
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(event->player));
            const ScoreLine *const lines = rider->score.lines;
            for (uint32_t line = 0; line < rider->score.lineCount; ++line)
            {
                for (ee::Ptr<ScoreTrick> trick = lines[line].tricks; trick; trick = trick->next)
                {
                    if (points->trickType == trickType(trick->trick))
                        points->points += static_cast<uint32_t>(trick->points);
                }
            }
        }
        else
            points->points += static_cast<uint32_t>(ee::toInt(event->value));
        if (points->points >= points->minPoints)
            calls::setObjectiveStateTail(objective, kObjectiveDone);
    }
    READABLE_FUNCTION_WAITS(pointsEvent, Verified);

    // resetPoints (the type's reset): nothing scored.
    void resetPoints(Objective objective)
    {
        ee::Ptr<PointsObjective>(zge::objectiveData(objective))->points = 0;
    }
    READABLE_FUNCTION_WAITS(resetPoints, Verified);

    // pointsStatusText (the type's statusText): the string table's text
    // "<STRINGID>_STATUS", a format, written with the points scored and
    // MIN_POINTS. 0 when the table has no such text.
    int32_t pointsStatusText(Objective objective, ee::Ptr<char> text)
    {
        ee::Local<char[0x40]> key;
        const ee::Ptr<PointsObjective> points(zge::objectiveData(objective));
        zge::formatString(key, sym::bmx::pointsStatusKeyFormat, zge::objectiveStringId(objective));
        const char *format = zge::stringTableText(zge::nameHash(key));
        if (format == nullptr)
            return 0;
        return zge::formatString(text, format, points->points, points->minPoints);
    }
    READABLE_FUNCTION_WAITS(pointsStatusText, Verified);

    // parseTrickType: the objective's trick type from the script's word:
    // any type unless the word is one of the seven; whether it was.
    uint32_t parseTrickType(ee::Ptr<const char> word, ee::Address objective)
    {
        const ee::Ptr<PointsObjective> points(objective);
        const uint32_t hash = zge::nameHash(word);
        const ee::Ptr<TrickTypeWord> words(sym::trickTypeWords);
        points->trickType = kTrickTypes;
        for (uint32_t i = 0; i < kTrickTypes; ++i)
        {
            if (words[i].nameHash == hash)
            {
                points->trickType = words[i].type;
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(parseTrickType, Draft);
}
