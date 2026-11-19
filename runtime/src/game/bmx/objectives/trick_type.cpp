// BMX XXX's "TRICK" objective type: the rider doing a trick or being in an
// action state (NAME), optionally with a second trick (MODIFIER), holding
// it up to a VALUE (a distance, a spin, a time: by what the trick is) and
// as an alley-oop (ALLEYOOP). BMX XXX's own code (its game block); the
// names are the script's keywords and the type's callbacks' places in its
// registration.

#include "objective_calls.h"

#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include "sym/bmx/objectives/trick_type.cpp.h"

namespace game::bmx::objectives
{
    using rider::Rider;

    namespace
    {
        // The block's keys: VALUE, ALLEYOOP, NAME, MODIFIER.
        constexpr uint32_t kTrickKeys = 4;

        // What a TRICK objective's VALUE measures, which its status text
        // shows by: a distance (the script's in feet, the game's in metres;
        // shown in the status's own unit), a spin (half turns; shown in
        // degrees), or neither (a time in seconds, shown as it is).
        enum ValueUnit : uint8_t
        {
            kValuePlain = 0,
            kValueDistance = 1,
            kValueSpin = 2,
        };
        constexpr ee::Float kMetresPerFoot = std::bit_cast<float>(0x3E9C0EBEu); // 0.3048 as the game has it (the float just under it); not an address
        constexpr ee::Float kDistanceStatusScale = 2.54f;
        constexpr ee::Float kDegreesPerHalfTurn = 180.0f;
        constexpr ee::Float kSecondsPerFrame = 0.016666668f; // 1 / 60
        // A VALUE this near 0 asks nothing.
        constexpr ee::Float kNoValue = 0.00001f;

        // A TRICK objective's record (0x14 bytes, from the type's pool).
        struct TrickObjective
        {
            uint32_t alleyOop;  // +0x00 ALLEYOOP
            uint32_t isState;   // +0x04 NAME is an action state's ("kbmxriderStateMain..."), not a trick's
            uint8_t name;       // +0x08 NAME: the trick's number or the action state (kAny when the script names none)
            uint8_t modifier;   // +0x09 MODIFIER: the second trick's number (kAny: whichever)
            uint8_t valueUnit;  // +0x0A ValueUnit
            uint8_t _0b;
            ee::Float latest;   // +0x0C what the rider has reached, as last measured
            ee::Float value;    // +0x10 VALUE
        };
        static_assert(offsetof(TrickObjective, name) == 0x08 && offsetof(TrickObjective, latest) == 0x0C && sizeof(TrickObjective) == 0x14);
        constexpr uint8_t kAny = 0xFE;

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_trickObjectives> g_trickObjectives;

        // NAME's and MODIFIER's words, by the hash of each.
        struct TrickWord
        {
            uint32_t nameHash; // +0x00
            uint8_t number;    // +0x04
            uint8_t _05[3];
        };
        static_assert(sizeof(TrickWord) == 8);
        constexpr uint32_t kNameWords = 0x78;
        constexpr uint32_t kModifierWords = 0x5A;
        // The action states' names begin with this (18 letters).
        constexpr uint32_t kStatePrefixBytes = 0x12;

        // The two wheelie tricks whose distances the rider keeps apart
        // (ScoreBlock::kindScores[1] and [2]).
        constexpr uint8_t kTrickWithScore1 = 0x48;
        constexpr uint8_t kTrickWithScore2 = 0x49;

        // A riding state's VALUE is measured by the rider's skid distance
        // (Rider +0x1A0), as a distance.
        ee::Float ridingMeasure(const Rider *rider)
        {
            return rider->skidDistance;
        }
    }

    void readTrickParameters(Objective objective, ee::Ptr<const char> parentKeyword);
    uint32_t readTrickBlock(ee::Ptr<const char> beginKeyword);
    void freeTrick(Objective objective);
    void trickEvent(Objective objective, ee::Ptr<ObjectiveEvent> event);
    int32_t trickStatusText(Objective objective, ee::Ptr<char> text);
    ee::Float lastLineSpin(ee::Ptr<Rider> rider);
    ee::Float heightAboveContact(ee::Ptr<Rider> rider);
    ee::Float wheelContactsApartSquared(ee::Ptr<Rider> rider);
    ee::Float firstKindScore(ee::Ptr<Rider> rider);
    ee::Float ridingMeasureOf(ee::Ptr<Rider> rider);
    ee::Float secondsInAction(ee::Ptr<Rider> rider);
    uint32_t trickValueReached(ee::Address objective);
    void setTrickValueUnit(ee::Address objective);
    uint32_t parseTrickName(ee::Ptr<const char> word, ee::Address objective);
    uint32_t parseTrickModifier(ee::Ptr<const char> word, ee::Address objective);

    // registerTrickType: the "TRICK" type and its callbacks.
    void registerTrickType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::trickTypeName);
        setup->hooks.readParameters = sym::readTrickParameters;
        setup->hooks.freeData = sym::freeTrick;
        setup->hooks.onEvent = sym::trickEvent;
        setup->hooks.statusText = sym::trickStatusText;
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerTrickType, Verified);

    // readTrickParameters (the type's readParameters): the block
    // TRICK_BEGIN .. TRICK_END is made readable inside the objective's
    // block, with the objective as its owner.
    void readTrickParameters(Objective objective, ee::Ptr<const char> parentKeyword)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kTrickKeys]> keyNames;
        ee::Local<uint32_t[kTrickKeys]> keyKinds;
        const ee::Ptr<ee::Ptr<const char>> names(sym::trickKeyNames);
        for (uint32_t i = 0; i < kTrickKeys; ++i)
            keyNames[i] = names[i];
        calls::memset(keyKinds, 0, sizeof(uint32_t[kTrickKeys]));
        setup->parentBegin = parentKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->begin = ee::Ptr<const char>(sym::trickBeginKeyword);
        setup->end = ee::Ptr<const char>(sym::trickEndKeyword);
        setup->keyCount = kTrickKeys;
        setup->onEnd = sym::readTrickBlock;
        setup->onBegin = 0;
        zge::registerBlockClass(setup);
        zge::setBlockClassOwner(ee::Ptr<const char>(sym::trickBeginKeyword), objective);
    }
    READABLE_FUNCTION_WAITS(readTrickParameters, Verified);

    // readTrickBlock (the block's handler): the objective's record made
    // from the block's values: any trick and any modifier unless NAME and
    // MODIFIER say which; a distance's VALUE turned from feet to metres.
    // Always 1.
    uint32_t readTrickBlock(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[0x20]> text;
        const Objective owner(zge::blockClassOwner(beginKeyword));
        const ee::Ptr<BlockValues> values = zge::blockClassValues(beginKeyword);
        const ee::Ptr<TrickObjective> trick(zge::poolTakeCreating(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_trickObjectives), sizeof(TrickObjective)));
        zge::memoryFill(trick, 0, sizeof(TrickObjective));
        trick->value = 0.0f;
        trick->modifier = kAny;
        trick->name = kAny;

        zge::formatString(text, sym::bmx::trickKeyName);
        ee::Ptr<BlockValue> value = zge::findScriptValue(values, text);
        if (value)
        {
            zge::scriptValueText(value, text, sizeof(char[0x20]));
            parseTrickName(text, trick);
        }

        zge::formatString(text, sym::bmx::trickKeyModifier);
        value = zge::findScriptValue(values, text);
        if (value)
        {
            zge::scriptValueText(value, text, sizeof(char[0x20]));
            parseTrickModifier(text, trick);
        }

        zge::formatString(text, sym::bmx::trickKeyValue);
        value = zge::findScriptValue(values, text);
        if (value)
        {
            trick->value = zge::scriptValueFloat(value);
            setTrickValueUnit(trick);
            if (trick->valueUnit == kValueDistance)
                trick->value = trick->value * kMetresPerFoot;
        }

        zge::formatString(text, sym::bmx::trickKeyAlleyOop);
        value = zge::findScriptValue(values, text);
        if (value)
            trick->alleyOop = zge::scriptValueSwitch(value, trick->alleyOop);

        zge::setObjectiveData(owner, ee::Address(trick));
        zge::unregisterBlockClass(ee::Ptr<const char>(sym::trickBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS(readTrickBlock, Verified);

    // freeTrick (the type's free): the record back to the pool.
    void freeTrick(Objective objective)
    {
        const ee::Address trick = zge::objectiveData(objective);
        if (trick)
            zge::poolGiveDestroying(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_trickObjectives), trick);
        calls::setObjectiveDataTail(objective, ee::Address());
    }
    READABLE_FUNCTION_WAITS(freeTrick, Draft);

    // trickEvent (the type's event; the type has no frame callback, so the
    // engine sends it the frame's event every frame): the objective is done
    // when the player's rider is in the named action state, or doing the
    // named trick (any, with no name), with the modifier asked for, has
    // reached the VALUE (when there is one), and, for an alley-oop, the
    // current trick line's first trick is marked as one.
    void trickEvent(Objective objective, ee::Ptr<ObjectiveEvent> event)
    {
        const ee::Ptr<TrickObjective> trick(zge::objectiveData(objective));
        if (event->kind != kFrameEvent)
            return;
        const ee::Ptr<Rider> rider = rider::riderOf(riderIndexOf(event->player));
        if (trick->isState != 0)
        {
            if (rider::actionState(rider.get(), rider::kLevelAction) != trick->name)
                return;
        }
        else if (rider->trick != trick->name && trick->name != kAny)
            return;
        if (trick->modifier != kAny && trick->modifier != rider->trickMotion)
            return;
        if (!(ee::abs(trick->value) < kNoValue))
        {
            if (trickValueReached(trick) == 0)
                return;
        }
        if (trick->alleyOop != 0)
        {
            const uint32_t lines = rider->score.lineCount;
            if (lines == 0)
                return;
            const ee::Ptr<ScoreTrick> first = rider->score.lines[lines - 1].tricks;
            if ((first->flags & kScoreTrickAlleyOop) == 0)
                return;
        }
        calls::setObjectiveStateTail(objective, kObjectiveDone);
    }
    READABLE_FUNCTION_WAITS(trickEvent, Verified);

    // trickStatusText (the type's statusText): the string table's text
    // "<STRINGID>_STATUS", a format, written with what the rider has
    // reached and the VALUE, each in the status's unit. 0 when the table
    // has no such text.
    int32_t trickStatusText(Objective objective, ee::Ptr<char> text)
    {
        ee::Local<char[0x40]> key;
        const ee::Ptr<TrickObjective> trick(zge::objectiveData(objective));
        zge::formatString(key, sym::bmx::trickStatusKeyFormat, zge::objectiveStringId(objective));
        const char *format = zge::stringTableText(zge::nameHash(key));
        if (format == nullptr)
            return 0;
        ee::Float latest = trick->latest;
        ee::Float toReach = trick->value;
        if (trick->valueUnit == kValueDistance)
        {
            toReach = toReach * kDistanceStatusScale;
            latest = latest * kDistanceStatusScale;
        }
        else if (trick->valueUnit == kValueSpin)
        {
            toReach = toReach * kDegreesPerHalfTurn;
            latest = latest * kDegreesPerHalfTurn;
        }
        const uint64_t latestWide = sce::libgcc::fptodp(latest);
        const uint64_t toReachWide = sce::libgcc::fptodp(toReach);
        return zge::formatString(text, format, latestWide, toReachWide);
    }
    READABLE_FUNCTION_WAITS(trickStatusText, Verified);

    // lastLineSpin: the newest trick line's spin (0 with no line).
    ee::Float lastLineSpin(ee::Ptr<Rider> rider)
    {
        const uint32_t lines = rider->score.lineCount;
        if (lines == 0)
            return 0.0f;
        return ee::Float(static_cast<float>(rider->score.lines[lines - 1].spin));
    }
    READABLE_FUNCTION(lastLineSpin, Verified);

    // heightAboveContact: how far the rider is above the first wheel's
    // contact point (y grows downwards).
    ee::Float heightAboveContact(ee::Ptr<Rider> rider)
    {
        return rider->wheels[0].point[1] - rider->position[1];
    }
    READABLE_FUNCTION(heightAboveContact, Verified);

    // wheelContactsApartSquared: the squared level distance between the two
    // wheels' contact points.
    ee::Float wheelContactsApartSquared(ee::Ptr<Rider> rider)
    {
        ee::Local<zge::Vec4> apart;
        *apart = *rider::vec4(rider->wheels[1].point) - *rider::vec4(rider->wheels[0].point);
        apart->y = 0.0f;
        return zge::squaredLength(*apart);
    }
    READABLE_FUNCTION(wheelContactsApartSquared, Draft);

    // firstKindScore: the rider's kindScores[0].
    ee::Float firstKindScore(ee::Ptr<Rider> rider)
    {
        return rider->score.kindScores[0];
    }
    READABLE_FUNCTION(firstKindScore, Draft);

    ee::Float ridingMeasureOf(ee::Ptr<Rider> rider)
    {
        return ridingMeasure(rider.get());
    }
    READABLE_FUNCTION(ridingMeasureOf, Draft);

    // secondsInAction: how long the rider has been in its action state.
    ee::Float secondsInAction(ee::Ptr<Rider> rider)
    {
        return ee::Float(static_cast<float>(rider->actionFrames[rider::kLevelAction])) * kSecondsPerFrame;
    }
    READABLE_FUNCTION(secondsInAction, Verified);

    // trickValueReached: what the current player's rider has reached of the
    // objective's kind is measured and kept, with its unit; whether it is
    // the VALUE or more. An action state is measured by state: riding by
    // the riding measure, a grind by the first kind score, a wheelie by the
    // larger of the other two kind scores, a landing by how far apart the
    // wheels' contact points are, the air, stall and lip trick by the
    // height above the contact (distances all), the later states by the
    // time in the state. A trick is measured by its type: an air (and no
    // type, while in the air) by the trick line's spin, a grind by the
    // first kind score, a stall or tap by the height, the two wheelie
    // tricks by their kind scores, flatland by the time. What is not
    // measured keeps the last measure.
    uint32_t trickValueReached(ee::Address objective)
    {
        const ee::Ptr<TrickObjective> trick(objective);
        const ee::Ptr<Rider> rider = rider::riderOf(riderIndexOf(currentPlayer()));
        ee::Float toReach = trick->value;
        trick->valueUnit = kValuePlain;
        if (trick->isState != 0)
        {
            switch (trick->name)
            {
            case rider::kActionRiding:
                trick->latest = ridingMeasureOf(rider);
                trick->valueUnit = kValueDistance;
                break;
            case rider::kActionGrind:
                trick->latest = firstKindScore(rider);
                trick->valueUnit = kValueDistance;
                break;
            case rider::kActionAir:
            case rider::kActionStall:
            case rider::kActionLipTrick:
                trick->latest = heightAboveContact(rider);
                trick->valueUnit = kValueDistance;
                break;
            case rider::kActionWheelie:
            {
                const ee::Float second = rider->score.kindScores[1];
                const ee::Float third = rider->score.kindScores[2];
                trick->valueUnit = kValueDistance;
                trick->latest = ee::max(third, second);
                break;
            }
            case rider::kActionLanding:
            {
                const ee::Float apart = wheelContactsApartSquared(rider);
                const ee::Float value = trick->value;
                trick->latest = apart;
                trick->valueUnit = kValueDistance;
                toReach = value * value;
                break;
            }
            case rider::kActionState6:
            case 8:
            case 9:
            case 10:
                trick->latest = secondsInAction(rider);
                break;
            default:
                break;
            }
        }
        else
        {
            switch (trickType(trick->name))
            {
            case kTrickNone:
                if (rider::actionState(rider.get(), rider::kLevelAction) == rider::kActionAir)
                {
                    trick->latest = lastLineSpin(rider);
                    trick->valueUnit = kValueSpin;
                }
                break;
            case kTrickAir:
                trick->latest = lastLineSpin(rider);
                trick->valueUnit = kValueSpin;
                break;
            case kTrickGrind:
                trick->latest = firstKindScore(rider);
                trick->valueUnit = kValueDistance;
                break;
            case kTrickStall:
            case kTrickTap:
                trick->latest = heightAboveContact(rider);
                trick->valueUnit = kValueDistance;
                break;
            case kTrickManual:
                if (trick->name == kTrickWithScore1)
                {
                    trick->valueUnit = kValueDistance;
                    trick->latest = rider->score.kindScores[1];
                }
                else if (trick->name == kTrickWithScore2)
                {
                    trick->valueUnit = kValueDistance;
                    trick->latest = rider->score.kindScores[2];
                }
                break;
            case kTrickFlatland:
                trick->latest = secondsInAction(rider);
                break;
            default:
                break;
            }
        }
        return toReach <= trick->latest ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(trickValueReached, Verified);

    // setTrickValueUnit (the block's reader): the unit the VALUE is written
    // in: for an action state a distance up to the wheelie and for the
    // landing; for a trick a spin when it has no type or is an air, a
    // distance for a grind, stall, tap or manual.
    void setTrickValueUnit(ee::Address objective)
    {
        const ee::Ptr<TrickObjective> trick(objective);
        trick->valueUnit = kValuePlain;
        if (trick->isState != 0)
        {
            if (trick->name < 6 || trick->name == rider::kActionLanding)
                trick->valueUnit = kValueDistance;
        }
        else
        {
            const uint32_t type = trickType(trick->name);
            if (type < kTrickGrind)
                trick->valueUnit = kValueSpin;
            else if (type < kTrickFlatland)
                trick->valueUnit = kValueDistance;
        }
    }
    READABLE_FUNCTION_WAITS(setTrickValueUnit, Verified);

    // parseTrickName: the objective's NAME from the script's word, looked
    // up by its hash among the tricks' and the action states' names; a
    // name that begins as the action states' do is a state's. Whether the
    // word was found (the record is left as it was when not).
    uint32_t parseTrickName(ee::Ptr<const char> word, ee::Address objective)
    {
        const ee::Ptr<TrickObjective> trick(objective);
        const uint32_t isState = zge::compareStringsNoCaseN(word, sym::actionStateNamePrefix, kStatePrefixBytes) == 0 ? 1 : 0;
        const uint32_t hash = zge::nameHash(word);
        const ee::Ptr<TrickWord> words(sym::trickNameWords);
        for (uint32_t i = 0; i < kNameWords; ++i)
        {
            if (words[i].nameHash == hash)
            {
                trick->isState = isState;
                trick->name = words[i].number;
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(parseTrickName, Verified);

    // parseTrickModifier: the objective's MODIFIER from the script's word,
    // by its hash. Whether the word was found.
    uint32_t parseTrickModifier(ee::Ptr<const char> word, ee::Address objective)
    {
        const ee::Ptr<TrickObjective> trick(objective);
        const uint32_t hash = zge::nameHash(word);
        const ee::Ptr<TrickWord> words(sym::trickModifierWords);
        for (uint32_t i = 0; i < kModifierWords; ++i)
        {
            if (words[i].nameHash == hash)
            {
                trick->modifier = words[i].number;
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(parseTrickModifier, Verified);
}
