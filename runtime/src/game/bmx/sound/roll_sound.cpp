// BMX XXX's rolling sounds (roll_sound.h): BMX XXX's own code (bmx.c).

#include "roll_sound.h"

#include "bmx/sound/sound_state.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstdint>
#include "sym/bmx/sound/roll_sound.cpp.h"

namespace game::bmx
{
    using namespace sound;

    namespace
    {
        // The height a rolling sound's sources start at (20000).
        inline ee::Global<ee::Float, sym::g_rollStartHeight> g_rollStartHeight;
        // The sources' pitch for a unit of speed (0.1), and the most it gets.
        inline ee::Global<ee::Float, sym::g_rollPitchScale> g_rollPitchScale;
        constexpr float kTopRollPitch = 3600.0f;
        // The speed from which the sources sound at the full effects volume.
        constexpr int32_t kFullRollSpeed = 182;

        // A rolling sound's word: the surface's own sample in the low byte,
        // the surface's kind in the next, and two flags.
        constexpr uint32_t surfaceSampleOf(uint32_t sound) { return sound & 0xFFu; }
        constexpr uint32_t rollKindOf(uint32_t sound) { return sound >> 8 & 0xFFu; }
        constexpr uint32_t kRollKind1 = 1, kRollKind2 = 2;
        // With kind 1: the game bank's sample 11 for the tyres, whatever
        // the surface. For the chain: sample 10 in place of 9. What sets
        // either is the rider's (not read here).
        constexpr uint32_t kRollTyresOther = 0x20000, kRollChainOther = 0x10000;
        constexpr uint32_t kTyresSample = 0, kTyresOtherSample = 11, kKind2Samples = 12;
        constexpr uint32_t kChainSample = 9, kChainOtherSample = 10;
    }

    int32_t startRollSound()
    {
        ee::Local<zge::Vec4> start; // where the sources start: high above the level
        start->y = *g_rollStartHeight;
        start->x = 0.0f;
        start->z = 0.0f;
        uint32_t slot = 0;
        while (slot < kRollSlots && kRollSlotTable[slot].tyres != kNoSource)
            ++slot;
        if (slot == kRollSlots)
            return -1;
        RollSlot &roll = kRollSlotTable[slot];
        const uint32_t tyres = calls::startSource(calls::sample(*g_gameBank, kTyresSample), start, 0);
        roll.tyres = static_cast<uint16_t>(tyres);
        if ((tyres & 0xFFFFu) == kNoSource)
            return -1;
        const uint32_t chain = calls::startSource(calls::sample(*g_gameBank, kChainSample), start, 0);
        roll.chain = static_cast<uint16_t>(chain);
        if ((chain & 0xFFFFu) == kNoSource)
        {
            calls::stopSource(roll.tyres);
            roll.tyres = kNoSource;
            return -1;
        }
        calls::sourceVolume(roll.tyres, (*g_volume)[Effects]);
        calls::sourceVolume(roll.chain, (*g_volume)[Effects]);
        return static_cast<int32_t>(slot);
    }
    READABLE_FUNCTION_WAITS(startRollSound, Verified);

    namespace tail // the originals' tail calls into the sound layer (sound_state.h's calls)
    {
        void stopSource(uint32_t source) { ee::tailCall(sym::stopSource, source); }
        void setSourcePosition(uint32_t source, ee::Address position) { ee::tailCall(sym::setSourcePosition, source, position); }
    }

    void stopRollSound(uint32_t slot)
    {
        const RollSlot &roll = kRollSlotTable[slot];
        calls::stopSource(roll.chain);
        tail::stopSource(roll.tyres);
    }
    READABLE_FUNCTION_WAITS(stopRollSound, Verified);

    void updateRollSound(uint32_t slot, uint32_t sound, ee::Address position, int32_t speed)
    {
        const RollSlot &roll = kRollSlotTable[slot];
        if (speed < 0)
            speed = -speed;
        const uint32_t kind = rollKindOf(sound);
        const uint32_t surfaceSample = surfaceSampleOf(sound);
        uint32_t volume = (*g_volume)[Effects];
        if (speed < kFullRollSpeed)
            volume = static_cast<uint32_t>(static_cast<int32_t>(volume * static_cast<uint32_t>(speed)) / kFullRollSpeed) & 0xFFu;
        const int32_t pitch = static_cast<int16_t>(ee::toInt(ee::min(ee::toFloat(speed) * *g_rollPitchScale, ee::Float(kTopRollPitch))));

        // (Each source's id is read again for each call.)
        if (kind == kRollKind1)
        {
            const uint32_t source = roll.tyres;
            calls::sourceSample(source, calls::sample(*g_gameBank, (sound & kRollTyresOther) ? kTyresOtherSample : surfaceSample));
        }
        else if (kind == kRollKind2)
        {
            const uint32_t source = roll.tyres;
            calls::sourceSample(source, calls::sample(*g_gameBank, surfaceSample + kKind2Samples));
        }
        calls::sourceVolume(roll.tyres, volume);
        calls::sourcePitch(roll.tyres, pitch);

        const uint32_t chain = roll.chain;
        calls::sourceSample(chain, calls::sample(*g_gameBank, (sound & kRollChainOther) ? kChainOtherSample : kChainSample));
        calls::sourceVolume(roll.chain, volume);
        calls::sourcePitch(roll.chain, pitch);
        calls::setSourcePosition(roll.tyres, position);
        tail::setSourcePosition(roll.chain, position);
    }
    READABLE_FUNCTION_WAITS(updateRollSound, Verified);
}
