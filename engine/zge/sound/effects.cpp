// The positional sound effects (block musicStreamStubG; BMX XXX and the GameCube
// and Xbox builds have the same update, kUpdateEffects): 256 emitters
// (emitters, 0x30 bytes each), each a sound at a point for a number of
// updates, on a voice of the sound channels (ps2Chan, allocateVoice ..) while it
// is in range of a listener. A listener is an object (the players' cameras
// [inferred]); the emitter's point by its matrix 0 gives the distance (its
// squared length) and the side (x). One listener sets the voice's volume by
// the distance, its pan by the side and its pitch by how fast the distance
// changes (a Doppler shift); two or four (split screen) set the volume by
// the nearest and the pan by which pair of them is nearer, with no shift.
//

#include "zge/memory/pool.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/text/cstring.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/vector.h"
#include "zge/scene/scene.h"
#include "zge/sound/sound.h"
#include "zge/sound/soundtrack.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include "sym/zge/sound/effects.cpp.h"

namespace zge
{
    ee::Ptr<Matrix> listenerPoint(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> point, ee::Ptr<Vec4> out);

    namespace
    {
        // The sound channels (ps2Chan, recompiled).
        constexpr uint8_t kNoVoice = 0xFF;
        constexpr uint32_t kEffectPriority = 0xFE;

        struct SoundEmitter
        {
            uint16_t flags;         // +0x00 kEmitterOn
            uint8_t voice;          // +0x02 kNoVoice: none
            uint8_t _03;
            uint32_t sound;         // +0x04 its EmitterSound, 0: none
            int32_t updatesLeft;    // +0x08 0: done
            uint32_t _0c;
            Vec4 position;          // +0x10
            union // +0x20 the squared distance at the last update (one listener)
            {
                ee::Float lastDistance;
                uint32_t startDistance; // ZGE2's start of a source keeps the whole number (startSource)
            };
            uint16_t pitchShift;    // +0x24 the Doppler shift, +-kMaxPitchShift
            int16_t pitch;          // +0x26
            uint8_t volume;         // +0x28 0 .. 127 by the distance
            uint8_t loudness;       // +0x29 the emitter's own (the voice gets volume * loudness / 128)
            int8_t pan;             // +0x2A
            // +0x2B (ZGE4) the point's depth from the listener, squared, as a share of 127 of the range (one listener)
            uint8_t ahead;
            uint8_t _2c[4];
        };
        static_assert(sizeof(SoundEmitter) == 0x30 && offsetof(SoundEmitter, position) == 0x10);
        static_assert(offsetof(SoundEmitter, pitchShift) == 0x24 && offsetof(SoundEmitter, pan) == 0x2A);
        constexpr uint16_t kEmitterOn = 0x8000;
        constexpr uint32_t kEmitterCount = 256;
        inline ee::Global<SoundEmitter[kEmitterCount], sym::emitters> g_emitters;

        // As far as an emitter reads its sound.
        struct EmitterSound
        {
            uint8_t _00[6];
            uint8_t rangeClass;     // +0x06 which of rangeFactors scales its range
        };
        // ZGE4 scales the range of an emitter with no sound by this class's factor (0.529).
        constexpr uint32_t kNoSoundRangeClass = 64;
        ee::Float rangeFactor(uint32_t rangeClass) { return ee::Ptr<ee::Float>(sym::rangeFactors)[rangeClass]; }
        constexpr int32_t kMaxPitchShift = 700;

        struct Listeners
        {
            int32_t count;                  // 0, 1, 2 or 4
            ee::Ptr<SceneObject> object[4]; // +0x04
        };
        inline ee::Global<Listeners, sym::g_listeners> g_listeners;

        inline ee::Global<int32_t, sym::g_effectsOn> g_effectsOn;
        // The range of a sound (in squared distance), unsigned; a sound's
        // class scales it (truncated to a whole number).
        inline ee::Global<uint32_t, sym::g_effectRange> g_effectRange;
        inline ee::Global<ee::Float, sym::g_dopplerScale> g_dopplerScale;
        // Set: no Doppler shift this update (cleared after it).
        inline ee::Global<int32_t, sym::g_noDoppler> g_noDoppler;

        // An unsigned word as a float.
        ee::Float unsignedToFloat(uint32_t value)
        {
            if (static_cast<int32_t>(value) >= 0)
                return ee::toFloat(static_cast<int32_t>(value));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((value & 1u) | (value >> 1)));
            return half + half;
        }


        // The emitter's voice, taken and started if it has none; false when
        // none is free (the update then reports 2).
        bool takeVoice(SoundEmitter &e)
        {
            if (e.voice != kNoVoice)
                return true;
            e.voice = static_cast<uint8_t>(allocateVoice(ee::Ptr<BankSample>(e.sound), kEffectPriority));
            if (e.voice == kNoVoice)
                return false;
            startVoice(e.voice);
            return true;
        }

        // The volume by the (squared) distance in the range.
        void setVolume(SoundEmitter &e, ee::Float distance, ee::Float range)
        {
            const uint32_t loudness = e.loudness;
            const int32_t volume = 127 - ee::toInt(distance * ee::Float(127.0f) / range);
            e.volume = static_cast<uint8_t>(volume);
            setVoiceVolume(e.voice, (loudness * (static_cast<uint32_t>(volume) & 0xFFu)) >> 7 & 0xFFu);
        }

        void releaseVoice(SoundEmitter &e)
        {
            zge::releaseVoice(e.voice);
            e.voice = kNoVoice;
        }

        // One listener: the volume by the distance, the pan by the side, the
        // pitch shifted by the distance's change since the last update.
        void hearFromOne(SoundEmitter &e, ee::Float range, int32_t &result)
        {
            ee::Local<Vec4> heard;
            listenerPoint(g_listeners->object[0], ee::Ptr<Vec4>(&e.position), heard);
            const ee::Float distance = squaredLength(*heard);
            if (!(distance < range))
            {
                e.volume = 0;
                if (e.voice != kNoVoice)
                    releaseVoice(e);
                e.lastDistance = distance;
                return;
            }
            if (!takeVoice(e))
            {
                result = 2;
                return;
            }
            setVolume(e, distance, range);
            if constexpr (game::readable::kIsBmx)
            {
                // ZGE4 pans by the side squared (never to the left) and keeps the depth squared likewise.
                e.pan = static_cast<int8_t>(ee::toInt(heard->x * heard->x * ee::Float(127.0f) / range));
                e.ahead = static_cast<uint8_t>(ee::toInt(heard->z * heard->z * ee::Float(127.0f) / range));
            }
            else if (!(heard->x == ee::Float(0.0f)))
                e.pan = static_cast<int8_t>(ee::toInt(heard->x * ee::Float(127.0f) / range));
            else
                e.pan = 0;
            setVoicePan(e.voice, static_cast<int32_t>(e.pan));
            if (!*g_noDoppler)
            {
                // Coming nearer raises the pitch.
                const int32_t shift = static_cast<int16_t>(-ee::toInt((distance - e.lastDistance) * *g_dopplerScale));
                e.pitchShift = static_cast<uint16_t>(std::clamp(shift, -kMaxPitchShift, kMaxPitchShift));
            }
            else
                e.pitchShift = 0;
            setVoicePitch(e.voice, static_cast<int32_t>(static_cast<int16_t>(static_cast<uint16_t>(e.pitch) + e.pitchShift)));
            e.lastDistance = distance;
        }

        // Two or four listeners (split screen): the nearest's distance; the
        // pan by which pair is nearer (the first and third against the second
        // and fourth), no shift.
        void hearFromSeveral(SoundEmitter &e, ee::Float range, int32_t &result)
        {
            ee::Float distance[4];
            const uint32_t count = static_cast<uint32_t>(g_listeners->count);
            for (uint32_t i = 0; i < count; ++i)
            {
                ee::Local<Vec4> heard;
                listenerPoint(g_listeners->object[i], ee::Ptr<Vec4>(&e.position), heard);
                distance[i] = squaredLength(*heard);
            }
            // The nearer of each pair.
            ee::Float firstPair, secondPair;
            if (count == 2)
            {
                firstPair = distance[0];
                secondPair = distance[1];
            }
            else
            {
                firstPair = ee::min(distance[2], distance[0]);
                secondPair = ee::min(distance[3], distance[1]);
            }
            const ee::Float nearest = ee::min(secondPair, firstPair);
            if (!(nearest < range))
            {
                e.volume = 0;
                if (e.voice != kNoVoice)
                    releaseVoice(e);
                return;
            }
            if (!takeVoice(e))
            {
                result = 2;
                return;
            }
            setVolume(e, nearest, range);
            ee::Float side;
            if (firstPair <= secondPair)
                side = -ee::min(range, secondPair - firstPair);
            else
                side = ee::min(range, firstPair - secondPair);
            const int32_t pan = ee::toInt(side * ee::Float(127.0f) / range);
            e.pan = static_cast<int8_t>(pan);
            setVoicePan(e.voice, static_cast<int32_t>(static_cast<int8_t>(pan)));
            const int32_t pitch = e.pitch;
            e.pitchShift = 0;
            setVoicePitch(e.voice, pitch);
        }

        // One emitter's update.
        void updateEmitter(SoundEmitter &e, int32_t &result)
        {
            if (!(e.flags & kEmitterOn) || e.updatesLeft == 0)
                return;
            if (--e.updatesLeft == 0)
            {
                // Done: its voice goes.
                if (e.voice != kNoVoice)
                    releaseVoice(e);
                return;
            }
            // A voice taken over by another sound is not the emitter's any more.
            if (e.voice != kNoVoice && voiceSound(e.voice) != ee::Ptr<BankSample>(e.sound))
                releaseVoice(e);
            ee::Float reach = unsignedToFloat(*g_effectRange);
            // ZGE4 keeps the scaled range a float, and scales an emitter's with no sound too.
            if constexpr (game::readable::kIsBmx)
                reach = reach * rangeFactor(e.sound ? ee::Ptr<EmitterSound>(e.sound)->rangeClass : kNoSoundRangeClass);
            else if (e.sound)
            {
                const uint8_t rangeClass = ee::Ptr<EmitterSound>(e.sound)->rangeClass;
                reach = ee::toFloat(ee::toInt(reach * rangeFactor(rangeClass)));
            }
            switch (g_listeners->count)
            {
            case 1:
                hearFromOne(e, reach, result);
                break;
            case 2:
            case 4:
                hearFromSeveral(e, reach, result);
                break;
            default:
                break;
            }
        }
    }

    // listenerPoint: the point by the object's matrix 0 (objectMatrix), into
    // out; the matrix. For a listener, the emitter's point as the listener
    // hears it: its squared length is the distance, its x the side.
    ee::Ptr<Matrix> listenerPoint(ee::Ptr<SceneObject> object, ee::Ptr<Vec4> point, ee::Ptr<Vec4> out)
    {
        const ee::Ptr<Matrix> matrix = objectMatrix(object, 0u);
        *out = transformed(*matrix, *point);
        return matrix;
    }
    READABLE_FUNCTION_VU0(listenerPoint, Verified);

    // updateEffects (kUpdateEffects): every emitter's update, once a frame
    // (aggressive/game_frame.cpp); 2 when an emitter in range found no free
    // voice, else 0. The Doppler's pause (g_noDoppler) ends with it.
    int32_t updateEffects()
    {
        if (!*g_effectsOn)
            return 0;
        int32_t result = 0;
        for (uint32_t i = 0; i < kEmitterCount; ++i)
            updateEmitter((*g_emitters)[i], result);
        *g_noDoppler = 0;
        return result;
    }

    void pointFromListener(ee::Ptr<Vec4> point, ee::Ptr<Vec4> out); // (ZGE2 only, below)
    namespace
    {
        // The distance a whole pan is at (g_effectDistance, below; setEffectRange sets it).
        inline ee::Global<uint32_t, sym::g_effectDistance> g_panDistance;
    }

    // updateEffectsInWholeUnits: ZGE2's updateEffects: each emitter's update
    // as ZGE3's for one listener, but in whole units: the point from the
    // listener (pointFromListener) truncated to integers, its squared length
    // against g_effectRange (unsigned); the volume 127 at the listener down
    // to 0 at the range, in 1/128ths of both; the pan the point's x over
    // g_effectDistance, 0 straight ahead; the Doppler shift from the change
    // of the whole squared distance, kept as that integer (startDistance).
    // No sound classes, no split screen. 2 when an emitter in range found no
    // free voice, else 0.
    int32_t updateEffectsInWholeUnits()
    {
        if (!*g_effectsOn)
            return 0;
        int32_t result = 0;
        for (uint32_t i = 0; i < kEmitterCount; ++i)
        {
            SoundEmitter &e = (*g_emitters)[i];
            if (!(e.flags & kEmitterOn) || e.updatesLeft == 0)
                continue;
            if (--e.updatesLeft == 0)
            {
                if (e.voice != kNoVoice)
                    releaseVoice(e);
                continue;
            }
            if (e.voice != kNoVoice && voiceSound(e.voice) != ee::Ptr<BankSample>(e.sound))
                releaseVoice(e);
            ee::Local<Vec4> heard;
            pointFromListener(ee::Ptr<Vec4>(&e.position), heard);
            const int32_t x = ee::toInt(heard->x), y = ee::toInt(heard->y), z = ee::toInt(heard->z);
            const uint32_t distance = static_cast<uint32_t>(x * x + y * y + z * z);
            if (!(distance < *g_effectRange))
            {
                e.volume = 0;
                if (e.voice != kNoVoice)
                    releaseVoice(e);
                e.startDistance = distance;
                continue;
            }
            if (!takeVoice(e))
            {
                result = 2;
                continue;
            }
            const uint32_t volume = 127u - ((distance >> 7) * 127u) / (*g_effectRange >> 7);
            e.volume = static_cast<uint8_t>(volume);
            setVoiceVolume(e.voice, (e.loudness * (volume & 0xFFu)) >> 7 & 0xFFu);
            e.pan = x != 0 ? static_cast<int8_t>(x * 127 / static_cast<int32_t>(*g_panDistance)) : int8_t{0};
            setVoicePan(e.voice, e.pan);
            if (*g_noDoppler != 0)
                e.pitchShift = 0;
            else
            {
                const int32_t change = static_cast<int32_t>(distance - e.startDistance);
                const int32_t shift = -ee::toInt(ee::toFloat(change) * *g_dopplerScale);
                e.pitchShift = static_cast<uint16_t>(std::clamp(static_cast<int32_t>(static_cast<int16_t>(shift)), -kMaxPitchShift, kMaxPitchShift));
            }
            setVoicePitch(e.voice, static_cast<int16_t>(e.pitch + e.pitchShift));
            e.startDistance = distance;
        }
        *g_noDoppler = 0;
        return result;
    }
    READABLE_FUNCTION_WAITS_OWN(updateEffects, Verified, dm2, updateEffectsInWholeUnits);


    namespace
    {
        inline ee::Global<int32_t, sym::g_soundMillisecondTicks> g_soundMillisecondTicks;
        inline ee::Global<int32_t, sym::g_soundRate> g_soundRate; // g_soundRate (aggressive::(anonymous)::g_soundRate, (anonymous)::g_soundRate)
        inline ee::Global<int32_t, sym::g_soundBankPoolReady> g_soundBankPoolReady;
        inline ee::Global<int32_t, sym::g_soundBankPool> g_soundBankPool;
        constexpr uint32_t kBankWaveCount = sym::bankWaveCount; // bankWaveCount
        constexpr uint32_t kPoolFree = sym::poolFree; // poolFree ((pool, record); readable: poolFree_, Verified)
    }


    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(poolFreeTailint32_t, ee::tailCall<int32_t>, kPoolFree);
            GAME_TABLE_ROUTE(bankWaveCountint32_t, ee::game<int32_t>, kBankWaveCount);
        }
    }

    namespace
    {
        // The range as a distance (g_effectRange is its square); set by
        // initSoundEmitters, read by nothing in AI.
        inline ee::Global<uint32_t, sym::g_effectDistance> g_effectDistance;
        constexpr uint32_t kMillisecondsASecond = 1000;
    }

    // initSoundEmitters (initSound): every emitter off, no listeners, the
    // effects on with the Doppler shift. range is how far a sound carries
    // (kept squared, g_effectRange); the Doppler's scale is 0.5 / range;
    // milliseconds becomes a count of sound updates (at g_soundRate a
    // second). 0.
    int32_t initSoundEmitters(uint32_t range, uint32_t milliseconds)
    {
        for (uint32_t i = 0; i < kEmitterCount; ++i)
            (*g_emitters)[i].flags = 0;
        const uint32_t ticks = (milliseconds * static_cast<uint32_t>(*g_soundRate)) / kMillisecondsASecond;
        *g_effectDistance = range;
        *g_effectRange = range * range;
        *g_soundMillisecondTicks = static_cast<int32_t>(ticks);
        const ee::Float scale = ee::Float(0.5f) / unsignedToFloat(range);
        g_listeners->count = 0;
        *g_effectsOn = 1;
        *g_noDoppler = 0;
        *g_dopplerScale = scale;
        return 0;
    }
    READABLE_FUNCTION(initSoundEmitters, Verified);

    // (rate): g_soundRate (setSoundSourceFadeTime's rate)
    void setSoundTickRate(uint32_t soundRate)
    {
        *g_soundRate = soundRate;
    }
    READABLE_FUNCTION(setSoundTickRate, Draft);

    uint32_t soundStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(soundStubZero, Draft);

    // InitSound's: a pool of soundtracks (g_soundBankPool: 2124-byte records,
    // sixteen a block; ZGE4's 20-byte records one a block, createPool's
    // one-record flag 4 in the high half) made, g_soundBankPoolReady = 1.
    int32_t initSoundBankPool()
    {
        constexpr uint32_t kSoundtracksABlock = game::readable::perGame(0x10u, 0x10u, 1u | 4u << 16);
        const int32_t poolResult = createPool(kSoundtrackBytes, kSoundtracksABlock, 0);
        *g_soundBankPool = poolResult;
        *g_soundBankPoolReady = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initSoundBankPool, Verified);

    // The sound record pool (g_soundBankPool) destroyed.
    uint32_t shutdownSoundRecords()
    {
        destroyPool(ee::Ptr<Pool>(static_cast<uint32_t>(*g_soundBankPool)));
        *g_soundBankPoolReady = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownSoundRecords, Draft);

    // Frees every sound bank record, first to last, until the pool has none. Returns 1.
    uint32_t freeAllSoundRecords()
    {
        for (;;)
        {
            const int32_t record = zge::poolFirst(ee::Ptr<PoolList>(static_cast<uint32_t>(*g_soundBankPool)));
            if (record == 0)
                return 1;
            zge::poolFree(ee::Ptr<Pool>(static_cast<uint32_t>(*g_soundBankPool)), record);
        }
    }
    READABLE_FUNCTION(freeAllSoundRecords, Draft);

    // (record): poolFree of its link (poolLinkOf)
    int32_t freeSoundRecord(uint32_t arg0)
    {
        const int32_t record = zge::poolLinkOf(arg0);
        return routed::poolFreeTailint32_t(*g_soundBankPool, record);
    }
    READABLE_FUNCTION(freeSoundRecord, Draft);

    int32_t soundRecordsInUse()
    {
        return zge::poolCount(ee::Ptr<Pool>(static_cast<uint32_t>(*g_soundBankPool)));
    }
    READABLE_FUNCTION(soundRecordsInUse, Draft);

    namespace
    {
        inline ee::Global<uint32_t, sym::g_emitterSearchStart> g_emitterSearchStart;
        constexpr uint32_t kNoEmitter = 0xFFFF;
        constexpr uint32_t kFullVolume = 127;


        // A sound's range: what it is scaled by for its class, to a whole
        // number; with no sound the range as it is. ZGE4 keeps the scaled
        // range a float, and reads the range as signed for no sound.
        ee::Float soundRange(ee::Ptr<EmitterSound> sound)
        {
            if constexpr (game::readable::kIsBmx)
            {
                if (!sound)
                    return ee::toFloat(static_cast<int32_t>(*g_effectRange));
                return unsignedToFloat(*g_effectRange) * rangeFactor(sound->rangeClass);
            }
            const ee::Float range = unsignedToFloat(*g_effectRange);
            if (!sound)
                return range;
            return ee::toFloat(ee::toInt(range * rangeFactor(sound->rangeClass)));
        }

        // Louder the nearer, 0 .. 127 of the range.
        uint8_t heardVolume(ee::Float distance, ee::Float range)
        {
            return static_cast<uint8_t>(127 - ee::toInt(distance * ee::Float(127.0f) / range));
        }
        int8_t heardPan(ee::Float across, ee::Float range)
        {
            return static_cast<int8_t>(ee::toInt(across * ee::Float(127.0f) / range));
        }
    }

    // ---- an emitter's settings (by its handle, as startSource gave it) ------------

    // sourceSample: the emitter's sound (an EmitterSound).
    uint32_t sourceSample(uint32_t emitter, uint32_t sound)
    {
        (*g_emitters)[emitter & 0xFFFFu].sound = sound;
        return 0;
    }
    READABLE_FUNCTION(sourceSample, Verified);

    // setSourcePosition: the emitter's point (x, y and z), and its updates
    // left set to g_soundMillisecondTicks.
    uint32_t setSourcePosition(uint32_t emitter, ee::Ptr<Vec4> position)
    {
        const int32_t updates = *g_soundMillisecondTicks;
        SoundEmitter &target = (*g_emitters)[emitter & 0xFFFFu];
        target.position.x = position->x;
        target.position.y = position->y;
        target.updatesLeft = updates;
        target.position.z = position->z;
        return 0;
    }
    READABLE_FUNCTION(setSourcePosition, Verified);

    // sourcePitch: the emitter's own pitch (the Doppler shift goes on it).
    uint32_t sourcePitch(uint32_t emitter, uint32_t pitch)
    {
        (*g_emitters)[emitter & 0xFFFFu].pitch = static_cast<int16_t>(pitch);
        return 0;
    }
    READABLE_FUNCTION(sourcePitch, Verified);

    // sourceVolume: the emitter's own loudness, 0 .. 127.
    uint32_t sourceVolume(uint32_t emitter, uint32_t loudness)
    {
        (*g_emitters)[emitter & 0xFFFFu].loudness = static_cast<uint8_t>(loudness);
        return 0;
    }
    READABLE_FUNCTION(sourceVolume, Verified);

    // sourceVoice (was soundEmitterByte2): the emitter's voice, kNoVoice
    // with none.
    uint32_t sourceVoice(uint32_t emitter)
    {
        return (*g_emitters)[emitter & 0xFFFFu].voice;
    }
    READABLE_FUNCTION(sourceVoice, Draft);

    // How a sound at a point is heard: its volume and pan, and 0, or 1 when
    // it is not heard. One listener hears it by the squared distance, the
    // point's x in its view panning it. Split, the listeners' distances make
    // a left (the first player's, or the nearer of the first and third) and a
    // right (the second's, or the nearer of the second and fourth), and it
    // pans towards the nearer of the two. ZGE4 hears one listener's as
    // updateEffects does: the pan by the side squared, and the depth squared
    // in its third output (not written for a split screen).
    uint32_t sourceHeard(ee::Address soundRecord, ee::Ptr<Vec4> position, ee::Ptr<uint8_t> volumeOut, ee::Ptr<int8_t> panOut
                         READABLE_PARAM_ONLY(bmx, ee::Ptr<uint8_t> aheadOut = {}))
    {
        const ee::Ptr<EmitterSound> sound(soundRecord);
        ee::Local<Vec4> relative;
        const ee::Float range = soundRange(sound);
        const int32_t listeners = g_listeners->count;

        if (listeners == 1)
        {
            listenerPoint(g_listeners->object[0], position, relative);
            const ee::Float distance = squaredLength(*relative);
            if (!(distance < range))
            {
                *volumeOut = 0;
                *panOut = 0;
                if constexpr (game::readable::kIsBmx)
                    *aheadOut = 0;
                return 1;
            }
            if constexpr (game::readable::kIsBmx)
            {
                *volumeOut = heardVolume(distance, range);
                *panOut = heardPan(relative->x * relative->x, range);
                *aheadOut = static_cast<uint8_t>(heardPan(relative->z * relative->z, range));
                return 0;
            }
            const ee::Float across = relative->x;
            *volumeOut = heardVolume(distance, range);
            *panOut = across == ee::Float(0.0f) ? static_cast<int8_t>(0) : heardPan(across, range);
            return 0;
        }
        if (listeners != 2 && listeners != 4)
            return 1;

        ee::Float distances[4];
        for (int32_t i = 0; i < g_listeners->count; ++i)
        {
            listenerPoint(g_listeners->object[i], position, relative);
            distances[i] = squaredLength(*relative);
        }
        const bool two = g_listeners->count == 2;
        const ee::Float left = two ? distances[0] : ee::min(distances[2], distances[0]);
        const ee::Float right = two ? distances[1] : ee::min(distances[3], distances[1]);
        const ee::Float nearest = ee::min(right, left);
        if (!(nearest < range))
        {
            *volumeOut = 0;
            *panOut = 0;
            return 1;
        }
        *volumeOut = heardVolume(nearest, range);
        const ee::Float across = left <= right ? -ee::min(range, right - left) : ee::min(range, left - right);
        *panOut = heardPan(across, range);
        return 0;
    }

    // pointFromListener: ZGE2 only: the point as the one listener has it (by the listener's matrix 0, transformPoint).
    // With no listener, or two or four (a split screen), the origin: (0, 0, 0, 1); any other count leaves out as it
    // was.
    inline void pointFromListener(ee::Ptr<Vec4> point, ee::Ptr<Vec4> out)
    {
        switch (g_listeners->count)
        {
        case 1:
            game::tail(transformPoint, out, objectMatrix(g_listeners->object[0], 0u), point);
            return;
        case 0:
        case 2:
        case 4:
            *out = Vec4{ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(1.0f)};
            return;
        default:
            return;
        }
    }
    READABLE_FUNCTION_VU0_ONLY(dm2, pointFromListener);



    // positionHeard: ZGE2's sourceHeard: how a sound at a point is heard, with no sound record (every sound has the
    // one range) and in whole units: the point from the listener truncated to integers, its squared length against
    // g_effectRange (not heard at or past it: volume and pan 0, and 1), the volume 127 at the listener falling to 0
    // at the range. The pan is the point's x over the range's distance, 0 when the point is straight ahead (x 0)
    // or level with the listener's depth (z 0). Kept from the original: a point off the listener's height is turned
    // about the x axis first, which leaves the x the pan is taken from as it was.
    uint32_t positionHeard(ee::Ptr<Vec4> position, ee::Ptr<uint8_t> volumeOut, ee::Ptr<int8_t> panOut)
    {
        ee::Local<Vec4> relative;
        pointFromListener(position, relative);
        const ee::Float across = relative->x, up = relative->y, ahead = relative->z;
        const uint32_t x = static_cast<uint32_t>(ee::toInt(across)), y = static_cast<uint32_t>(ee::toInt(up)), z = static_cast<uint32_t>(ee::toInt(ahead));
        const uint32_t distance = x * x + y * y + z * z;
        const uint32_t range = *g_effectRange;
        if (!(distance < range))
        {
            *volumeOut = 0;
            *panOut = 0;
            return 1;
        }
        *volumeOut = static_cast<uint8_t>(127u - distance * 127u / range);
        if (across == ee::Float(0.0f) || ahead == ee::Float(0.0f))
        {
            *panOut = 0;
            return 0;
        }
        if (!(up == ee::Float(0.0f)))
        {
            const Angle tilt = angleAtan2(up, ahead);
            vec3RotateX(&relative->xyz(), &relative->xyz(), static_cast<Angle>(-tilt));
        }
        *panOut = static_cast<int8_t>(ee::toInt(relative->x * ee::Float(127.0f) / unsignedToFloat(*g_effectDistance)));
        return 0;
    }
    READABLE_FUNCTION_OWN(sourceHeard, Verified, dm2, positionHeard);

    // A free emitter for a sound at a point, kNoEmitter with none. It starts
    // silent, with no voice, and its distance as if the listener were at the
    // origin; the search starts after the last one found.
    uint32_t startSource(ee::Address sound, ee::Address where)
    {
        const ee::Ptr<Vec4> position(where);
        const uint32_t first = *g_emitterSearchStart;
        uint32_t index = first;
        for (;;)
        {
            SoundEmitter &emitter = (*g_emitters)[index];
            if ((emitter.flags & kEmitterOn) == 0)
            {
                emitter.flags = kEmitterOn;
                *g_emitterSearchStart = index + 1 == kEmitterCount ? 0 : index + 1;
                emitter.voice = kNoVoice;
                emitter.sound = sound;
                emitter.loudness = kFullVolume;
                emitter.pitch = 0;
                const uint32_t x = static_cast<uint32_t>(ee::toInt(position->x));
                const uint32_t y = static_cast<uint32_t>(ee::toInt(position->y));
                const uint32_t z = static_cast<uint32_t>(ee::toInt(position->z));
                const uint32_t squared = x * x + y * y + z * z;
                // (ZGE4 takes the sum as signed: past 2^31 it goes negative.)
                if constexpr (game::readable::kIsDm2)
                    emitter.startDistance = squared;
                else
                    emitter.lastDistance = game::readable::kIsBmx ? ee::toFloat(static_cast<int32_t>(squared)) : unsignedToFloat(squared);
                setSourcePosition(index, position);
                emitter.updatesLeft = 0;
                return index;
            }
            index = index + 1 == kEmitterCount ? 0 : index + 1;
            if (index == first)
                return kNoEmitter;
        }
    }
    READABLE_FUNCTION(startSource, Verified);

    // An emitter freed, and its voice.
    void stopSource(uint16_t source)
    {
        SoundEmitter &emitter = (*g_emitters)[source];
        if (emitter.voice != kNoVoice)
        {
            releaseVoice(emitter.voice);
            emitter.voice = kNoVoice;
        }
        emitter.flags = 0;
    }
    READABLE_FUNCTION(stopSource, Verified);

    // The listeners as the camera has them now.
    void cameraMoved(ee::Address listeners)
    {
        memoryCopy(sym::g_listeners, listeners, sizeof(Listeners));
    }
    READABLE_FUNCTION(cameraMoved, Verified);

    // No Doppler shift for this update: voices that have one go back to their
    // own pitch.
    void disableDoppler()
    {
        *g_noDoppler = 1;
        for (uint32_t i = 0; i < kEmitterCount; ++i)
        {
            const SoundEmitter &emitter = (*g_emitters)[i];
            if ((emitter.flags & kEmitterOn) != 0 && emitter.updatesLeft != 0 && emitter.pitchShift != 0)
                sourcePitch(i, emitter.pitch);
        }
    }
    READABLE_FUNCTION(disableDoppler, Draft);

    // Every emitter stopped and no listeners.
    void zsndSourceReset()
    {
        for (uint32_t i = 0; i < kEmitterCount; ++i)
        {
            if (((*g_emitters)[i].flags & kEmitterOn) != 0)
                stopSource(i);
        }
        g_listeners->count = 0;
        *g_emitterSearchStart = 0;
    }
    READABLE_FUNCTION(zsndSourceReset, Verified);
}
