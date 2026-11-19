// The sound sources: up to 32 scripts of timed events, each a group's data,
// stepped once a frame (updateSoundSources) and playing their notes on up
// to 64 source voices, which hold the hardware voices. A source's voices
// are a list by time, each one's timer counted from the one before it.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "zge/sound/bank.h"
#include "ee/plain.h"
#include "zge/sound/sound.h"
#include "sce/libgcc.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/sound/sources.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kNewGroupSource = sym::newGroupSource;
        constexpr uint32_t kReleaseSourceVoices = sym::releaseSourceVoices;
        constexpr uint32_t kRestartSourceVoices = sym::restartSourceVoices;
        constexpr uint32_t kSetSourceMix = sym::setSourceMix;
        constexpr uint32_t kSoundSourceActive = sym::soundSourceActive;
        constexpr uint32_t kSoundDataLength = sym::soundDataLength;
        constexpr uint32_t kReadSoundSourceEvent = sym::readSoundSourceEvent;
        constexpr uint32_t kPlaySoundSourceVoice = sym::playSoundSourceVoice;
        constexpr uint32_t kUpdateSourceVoiceMix = sym::updateSourceVoiceMix;
        constexpr uint32_t kSourceDataEnded = sym::sourceDataEnded;
        constexpr uint32_t kSeekSourceVoice = sym::seekSourceVoice;
        constexpr uint32_t kLinkSourceVoice = sym::linkSourceVoice;
        constexpr uint32_t kReleaseSourceVoice = sym::releaseSourceVoice;
        constexpr uint32_t kSoundSetEntry = sym::soundSetEntry;

        constexpr uint32_t kSourceCount = 32, kSourceVoiceCount = 64;

        enum SourceVoiceFlags : uint8_t
        {
            kSourceVoiceUsed = 1 << 0,
            kSourceVoiceSilent = 1 << 1, // it took no hardware voice
        };

        // A note of a source (0x14 bytes).
        struct SourceVoice
        {
            uint8_t flags;      // +0x00
            uint8_t volume;     // +0x01
            int16_t pitch;      // +0x02
            ee::Ptr<BankSample> sample; // +0x04
            uint8_t voice;      // +0x08 the hardware voice
            uint8_t _09[3];     // +0x09
            uint32_t timer;     // +0x0C after the voice before it
            ee::Ptr<SourceVoice> next; // +0x10
        };
        static_assert(sizeof(SourceVoice) == 0x14);

        // SoundSource::flags (the low two bits are how it was asked to play).
        constexpr uint16_t kHowMask = 0x3;
        constexpr uint16_t kReleaseWhenDone = 0x0004;
        constexpr uint16_t kRepeats = 0x0800;
        constexpr uint16_t kSilentWhilePaused = 0x1000;
        constexpr uint16_t kNeverSounds = 0x2000;
        constexpr uint16_t kStopping = 0x4000;
        constexpr uint16_t kSourceInUse = 0x8000;
        constexpr uint16_t kSourceFlagsAsked = 0xFFFC;

        enum SourceState : uint8_t
        {
            kSourceStopped = 0,
            kSourceRunning = 1,
            kSourcePaused = 2,
        };

        struct SoundSource
        {
            uint16_t flags;      // +0x00
            uint8_t state;       // +0x02
            uint8_t noteVolume;  // +0x03 the last note's
            uint8_t volume;      // +0x04 asked of it (setSourceMix)
            uint8_t scriptVolume; // +0x05 its script's
            int8_t pan;          // +0x06
            uint8_t _07;         // +0x07
            ee::Ptr<SoundGroup> group; // +0x08
            uint32_t _0c;        // +0x0C
            uint32_t _10;        // +0x10
            uint32_t _14;        // +0x14
            uint32_t _18;        // +0x18
            ee::Ptr<BankSample> sample; // +0x1C its notes' sample
            ee::Ptr<SoundSet> sampleSet; // +0x20 or the set they pick one from
            ee::Ptr<uint8_t> dataStart;  // +0x24
            ee::Ptr<uint8_t> dataCursor; // +0x28 the next event
            uint32_t timer;      // +0x2C to it, in 4096ths of a tick
            uint32_t frameTime;  // +0x30 a frame's worth of timer
            uint32_t ticksPerUnit; // +0x34 12.12 fixed, a note's length to frames
            ee::Ptr<SourceVoice> voices; // +0x38
            uint32_t callback;   // +0x3C (argument, event's two bytes)
            uint32_t callbackArgument; // +0x40
            uint16_t bank;       // +0x44
        };
        static_assert(sizeof(SoundSource) == 0x48 && offsetof(SoundSource, voices) == 0x38);

        // An event as the script gives it (on updateSoundSources' stack).
        struct SoundEvent
        {
            ee::Ptr<SoundSource> source; // +0x00
            uint32_t delay;              // +0x04
            union
            {
                struct
                {
                    uint8_t first, second;
                };
                uint32_t seekOffset;     // kCommandSeek's, from the script's start
            };                           // +0x08
            uint32_t handler;            // +0x0C (event *), 0: none
        };
        static_assert(sizeof(SoundEvent) == 0x10);

        // updateSourceVoiceMix's first byte.
        enum MixChange : uint8_t
        {
            kChangeVolume = 0,
            kChangePan = 1,
            kChangeSample = 4,
            kChangeSampleSet = 5,
        };

        // The script's commands (a byte's top five bits, or a whole byte
        // after kLongCommand).
        enum SourceCommand : uint32_t
        {
            kCommandNote = 0x00,
            kCommandNoteAtLastVolume = 0x08,
            kCommandMix = 0x10,
            kCommandEnd = 0x18,
            kCommandNothing20 = 0x20,
            kCommandNothing28 = 0x28,
            kCommandSeek = 0x30,
            kCommandNothing38 = 0x38,
            kCommandNothing40 = 0x40,
            kLongCommand = 0xF8,
        };
        constexpr uint32_t kCommandLimit = 0x41;
        constexpr uint32_t kArgumentSlots = 4;

        // A sample's middle note, and the pitch units of a note.
        constexpr int32_t kMiddleNote = 60, kPitchPerNote = 100;

        inline ee::Global<int32_t, sym::g_soundSourcesBusy> g_soundSourcesBusy; // no step while it is not 0
        inline ee::Global<uint32_t, sym::g_soundRate> g_soundRate;       // frames a second
        inline ee::Global<uint32_t, sym::g_nextSource> g_nextSource;
        inline ee::Global<uint32_t, sym::g_nextSourceVoice> g_nextSourceVoice;
        inline ee::Global<uint32_t, sym::g_sourceVoicesPaused> g_sourceVoicesPaused;
        inline ee::Global<ee::Ptr<SoundSource>[kSourceCount], sym::g_sourceSlots> g_sourceSlots;
        inline ee::Global<SoundSource[kSourceCount], sym::g_sources> g_sources;
        inline ee::Global<SourceVoice[kSourceVoiceCount], sym::g_sourceVoices> g_sourceVoices;

        // Two volumes of 0 to 127 on a note's.
        uint32_t mixedVolume(uint8_t noteVolume, const SoundSource &source)
        {
            const uint32_t once = static_cast<uint32_t>(noteVolume) * source.volume >> 7 & 0xFFu;
            return once * source.scriptVolume >> 7 & 0xFFu;
        }

        ee::Ptr<SoundSource> sourceAt(uint32_t handle) { return (*g_sourceSlots)[handle & 0xFFu]; }
    }

    // initSoundSources: every source and voice free (a source's in-use bit only).
    uint32_t initSoundSources()
    {
        for (uint32_t i = 0;;)
        {
            SoundSource &source = (*g_sources)[i];
            (*g_sourceSlots)[i] = ee::Ptr<SoundSource>();
            source.flags = source.flags & static_cast<uint16_t>(~kSourceInUse);
            if (++i < kSourceCount)
                continue;
            break;
        }
        *g_nextSource = 0;
        for (uint32_t i = 0;;)
        {
            (*g_sourceVoices)[i].flags = 0;
            if (++i < kSourceVoiceCount)
                continue;
            break;
        }
        *g_nextSourceVoice = 0;
        *g_sourceVoicesPaused = 0;
        return 0;
    }
    READABLE_FUNCTION(initSoundSources, Verified);

    // releaseAllSourceVoices: every source that is on let go.
    uint32_t releaseAllSourceVoices()
    {
        for (uint32_t source = 0;;)
        {
            if ((*g_sourceSlots)[source])
                releaseSourceVoices(source);
            if (++source < kSourceCount)
                continue;
            return 0;
        }
    }
    READABLE_FUNCTION(releaseAllSourceVoices, Verified);

    int32_t releaseSourceVoice(ee::Ptr<SourceVoice> voice);

    int32_t soundSourceActive(ee::Ptr<SoundSource> source);

    void readSoundSourceEvent(ee::Ptr<SoundSource> source, ee::Ptr<SoundEvent> event);

    uint32_t soundDataLength(ee::Ptr<SoundSource> source);

    namespace // the game functions below call: ee/plain.h
    {
        // What the script's event runs, and a source's own callback.
        void runEventHandler(uint32_t handler, ee::Ptr<SoundEvent> event) { ee::game(handler, event); }
        void runSourceCallback(uint32_t callback, uint32_t argument, uint8_t first, uint8_t second) { ee::game(callback, argument, first, second); }
    }

    // updateSoundSources: the frame's step: the front voice's timer down, those it
    // ends released; each source's time on, and the events it has come to
    // read and handled, as many as a frame covers.
    uint32_t updateSoundSources()
    {
        ee::Local<SoundEvent> event;
        if (*g_soundSourcesBusy != 0)
            return 0;
        for (uint32_t index = 0;;)
        {
            const ee::Ptr<SoundSource> source = (*g_sourceSlots)[index];
            const uint32_t next = index + 1;
            bool active = true;
            if (source)
            {
                ee::Ptr<SourceVoice> voice = source->voices;
                if (voice)
                    voice->timer = voice->timer - 1;
                voice = source->voices;
                if (voice)
                {
                    if (voice->timer == 0)
                    {
                        for (;;)
                        {
                            releaseSourceVoice(voice);
                            voice = voice->next;
                            if (voice && voice->timer == 0)
                                continue;
                            break;
                        }
                    }
                }
                source->voices = voice;
                active = soundSourceActive(source) != 0;
                if (active)
                {
                    uint32_t remaining = source->frameTime;
                    uint32_t timer = source->timer;
                    uint32_t after = timer - remaining;
                    if (remaining >= timer)
                    {
                        const auto stepEvents = [&]()
                        {
                            for (;;)
                            {
                                if (remaining < timer)
                                    return false;
                                remaining -= timer;
                                readSoundSourceEvent(source, event);
                                if (event->handler != 0)
                                    runEventHandler(event->handler, event);
                                if (soundSourceActive(source) == 0)
                                    return true;
                                timer = soundDataLength(source) << 12;
                                source->timer = timer;
                            }
                        };
                        if (stepEvents())
                        {
                            if ((source->flags & kReleaseWhenDone) != 0)
                                releaseSourceVoices(index);
                            timer = source->timer;
                        }
                        after = timer - remaining;
                    }
                    source->timer = after;
                }
            }
            index = next;
            if (index < kSourceCount)
                continue;
            return 0;
        }
    }
    READABLE_FUNCTION(updateSoundSources, Verified);

    // newGroupSource: a free source for a group, its handle, 255 if none is
    // free. The bank its sample is found in is the slot's last one: the
    // group's is stored after.
    uint32_t newGroupSource(ee::Ptr<SoundGroup> group, uint32_t flags)
    {
        const uint32_t start = *g_nextSource;
        *g_soundSourcesBusy = *g_soundSourcesBusy + 1;
        const auto claim = [&](uint32_t index)
        {
            SoundSource &found = (*g_sources)[index];
            found.flags = kSourceInUse;
            *g_nextSource = index + 1;
            if (index + 1 == kSourceCount)
                *g_nextSource = 0;
            return ee::Ptr<SoundSource>(&found);
        };
        uint32_t index = start;
        ee::Ptr<SoundSource> source;
        if (((*g_sources)[index].flags & kSourceInUse) == 0)
            source = claim(index);
        else
        {
            for (;;)
            {
                index = index + 1 == kSourceCount ? 0 : index + 1;
                if (index == start)
                    break;
                if (((*g_sources)[index].flags & kSourceInUse) == 0)
                {
                    source = claim(index);
                    break;
                }
            }
        }
        if (!source)
        {
            *g_soundSourcesBusy = *g_soundSourcesBusy - 1;
            return 255;
        }
        source->state = kSourceStopped;
        const uint32_t rate = *g_soundRate;
        const uint32_t frameTime = (1000u << 12) / rate;
        const uint32_t ticksPerUnit = (rate << 12) / 1000u;
        source->flags = source->flags | static_cast<uint16_t>(flags & kSourceFlagsAsked);
        const ee::Ptr<uint8_t> events = ee::Ptr<uint8_t>(group->data) + 4;
        source->dataCursor = events;
        source->dataStart = events;
        source->frameTime = frameTime;
        source->noteVolume = 127;
        source->voices = ee::Ptr<SourceVoice>();
        source->ticksPerUnit = ticksPerUnit;
        source->volume = group->volume;
        source->scriptVolume = 127;
        source->group = group;
        source->pan = group->pan;
        constexpr uint8_t kNone = 255;
        if (group->sample != kNone)
            source->sample = sample(source->bank, group->sample);
        else
            source->sample = 0;
        if (group->sampleSet != kNone)
            source->sampleSet = bankSoundSet(source->bank, group->sampleSet);
        else
            source->sampleSet = 0;
        source->bank = group->bank;
        (*g_sourceSlots)[index] = source;
        *g_soundSourcesBusy = *g_soundSourcesBusy - 1;
        return index & 0xFFu;
    }
    READABLE_FUNCTION(newGroupSource, Verified);

    // releaseSourceVoices: a source's voices let go, and it is free.
    uint32_t releaseSourceVoices(uint32_t handle)
    {
        const uint32_t index = handle & 0xFFu;
        ee::Ptr<SourceVoice> voice = (*g_sourceSlots)[index]->voices;
        *g_soundSourcesBusy = *g_soundSourcesBusy + 1;
        if (voice)
        {
            for (;;)
            {
                releaseSourceVoice(voice);
                voice = voice->next;
                if (voice)
                    continue;
                break;
            }
        }
        *g_nextSource = index;
        (*g_sourceSlots)[index] = ee::Ptr<SoundSource>();
        *g_soundSourcesBusy = *g_soundSourcesBusy - 1;
        (*g_sources)[index].flags = 0;
        return 0;
    }
    READABLE_FUNCTION(releaseSourceVoices, Verified);

    // restartSourceVoices: a source going: from the top, its voices let go, or if
    // it is paused its silent voices' hardware voices started again.
    uint32_t restartSourceVoices(uint32_t handle)
    {
        *g_soundSourcesBusy = *g_soundSourcesBusy + 1;
        const ee::Ptr<SoundSource> source = sourceAt(handle);
        ee::Ptr<SourceVoice> voice = source->voices;
        if (source->state != kSourcePaused)
        {
            if (voice)
            {
                for (;;)
                {
                    releaseSourceVoice(voice);
                    voice = voice->next;
                    if (voice)
                        continue;
                    break;
                }
            }
            source->voices = ee::Ptr<SourceVoice>();
            source->dataCursor = source->dataStart;
            source->timer = soundDataLength(source) << 12;
        }
        else
        {
            if (!voice)
            {
                source->state = kSourceRunning;
                *g_soundSourcesBusy = *g_soundSourcesBusy - 1;
                return 0;
            }
            for (;;)
            {
                if ((voice->flags & kSourceVoiceSilent) == 0)
                    startVoice(voice->voice);
                voice = voice->next;
                if (!voice)
                    break;
            }
        }
        source->state = kSourceRunning;
        *g_soundSourcesBusy = *g_soundSourcesBusy - 1;
        return 0;
    }
    READABLE_FUNCTION(restartSourceVoices, Verified);

    // stopSoundSource: a source paused: its hardware voices stopped, or if it
    // was asked to play a way that can't pause (above 1), marked stopping.
    uint32_t stopSoundSource(uint32_t handle)
    {
        const ee::Ptr<SoundSource> source = sourceAt(handle);
        *g_soundSourcesBusy = *g_soundSourcesBusy + 1;
        if (soundSourceActive(source) != 0)
        {
            const uint32_t how = source->flags & kHowMask;
            if (how != 0)
            {
                if (how != 1)
                    source->flags = source->flags | kStopping;
            }
            else
            {
                ee::Ptr<SourceVoice> voice = source->voices;
                if (voice)
                {
                    for (;;)
                    {
                        if ((voice->flags & kSourceVoiceSilent) == 0)
                            stopVoice(voice->voice, 0);
                        voice = voice->next;
                        if (!voice)
                            break;
                    }
                }
            }
        }
        source->state = kSourcePaused;
        *g_soundSourcesBusy = *g_soundSourcesBusy - 1;
        return 0;
    }
    READABLE_FUNCTION(stopSoundSource, Verified);

    // setSourceMix: a source's volume and pan, on its voices too.
    uint32_t setSourceMix(uint32_t handle, uint32_t volume, uint32_t pan)
    {
        const ee::Ptr<SoundSource> source = sourceAt(handle);
        ee::Ptr<SourceVoice> voice = source->voices;
        source->volume = static_cast<uint8_t>(volume);
        source->pan = static_cast<int8_t>(pan);
        if (!voice)
            return 0;
        for (;;)
        {
            if ((voice->flags & kSourceVoiceSilent) == 0)
            {
                setVoiceVolume(voice->voice, mixedVolume(voice->volume, *source));
                setVoicePan(voice->voice, source->pan);
            }
            voice = voice->next;
            if (!voice)
                break;
        }
        return 0;
    }
    READABLE_FUNCTION(setSourceMix, Verified);

    // playGroupOnce: a group played once at a volume and pan: 0, or 255 with
    // the source given back if it wouldn't start.
    uint32_t playGroupOnce(ee::Ptr<SoundGroup> group, uint32_t volume, uint32_t pan)
    {
        const uint32_t mixVolume = volume & 0xFFu;
        const int32_t mixPan = static_cast<int8_t>(pan);
        const int32_t source = static_cast<int32_t>(newGroupSource(group, kReleaseWhenDone));
        if (source != 255)
        {
            if (static_cast<int32_t>(setSourceMix(source, mixVolume, mixPan)) == 0)
            {
                if (static_cast<int32_t>(restartSourceVoices(source)) == 0)
                    return 0;
            }
            releaseSourceVoices(source);
        }
        return 255;
    }
    READABLE_FUNCTION(playGroupOnce, Draft);

    // soundSourceActive: running, or still stopping.
    int32_t soundSourceActive(ee::Ptr<SoundSource> source)
    {
        if (source->state == kSourceRunning)
            return 1;
        const uint16_t flags = source->flags;
        return (flags & kStopping) != 0 ? 1 : 0;
    }
    READABLE_FUNCTION(soundSourceActive, Verified);

    int32_t readBytesU32(ee::Ptr<uint8_t> bytes);

    // soundDataLength: the time to the next event: 0, a byte or a word by the
    // low three bits of its first byte, the delay's coding.
    uint32_t soundDataLength(ee::Ptr<SoundSource> source)
    {
        const ee::Ptr<uint8_t> cursor = source->dataCursor;
        const uint32_t coding = *cursor & 0x7u;
        if (coding < 3)
            return 0;
        if (coding < 6)
            return cursor[1];
        return static_cast<uint32_t>(readBytesU32(cursor + 1));
    }
    READABLE_FUNCTION(soundDataLength, Verified);

    // readSoundSourceEvent: the event at the cursor. Its first byte: the top five
    // bits the command, the low three how its delay is coded, past the time
    // to it (soundDataLength reads that). The command's two arguments follow;
    // command kLongCommand has a whole byte for the command, then a mask and
    // up to four arguments, each with its two mask bits: the original tests
    // those with 2, not 3, so only a 2 or 3 in them means anything, and that
    // is three bytes with the last one taken. A long command that leaves out
    // its first or second argument would hand on what the frame held; no sound
    // data has one (no long command in the nine A/B inputs or 535 recorded calls).
    void readSoundSourceEvent(ee::Ptr<SoundSource> source, ee::Ptr<SoundEvent> event)
    {
        // A mask can skip a slot: that slot then keeps whatever the stack
        // held, and a command reads it all the same.
        ee::Local<uint8_t[kArgumentSlots]> arguments;
        event->handler = 0;
        event->source = source;
        ee::Ptr<uint8_t> at = source->dataCursor;
        const uint8_t first = *at;
        ++at;
        uint32_t command = first & 0xF8u;

        // The low three bits: the bytes of the time to this event (skipped
        // here) and of its delay.
        struct DelayCoding
        {
            uint8_t timeBytes, delayBytes;
        };
        static constexpr DelayCoding kDelayCodings[8] = {{0, 0}, {0, 1}, {0, 4}, {1, 0}, {1, 1}, {1, 4}, {4, 0}, {4, 4}};
        const DelayCoding coding = kDelayCodings[first & 0x7u];
        at += coding.timeBytes;
        if (coding.delayBytes == 1)
            event->delay = *at;
        else if (coding.delayBytes == 4)
            event->delay = static_cast<uint32_t>(readBytesU32(at));
        else
            event->delay = 0;
        at += coding.delayBytes;

        const bool longForm = command == kLongCommand;
        if (longForm)
        {
            command = at[0];
            uint32_t mask = at[1];
            at += 2;
            for (uint32_t slot = 0; slot < kArgumentSlots; ++slot, mask >>= 2)
            {
                if ((mask & 2) != 0)
                {
                    at += 2;
                    arguments[slot] = *at;
                    ++at;
                }
            }
        }
        else
        {
            arguments[0] = at[0];
            arguments[1] = at[1];
        }

        // The bytes a short-form event's arguments take after `at`; the long
        // form has already gone past its own.
        uint32_t argumentBytes = 0;
        const auto twoArguments = [&](uint32_t handler)
        {
            const uint8_t firstArgument = arguments[0], secondArgument = arguments[1];
            event->handler = handler;
            argumentBytes = 2;
            event->second = secondArgument;
            event->first = firstArgument;
        };
        switch (command)
        {
        case kCommandNote:
            twoArguments(kPlaySoundSourceVoice);
            break;
        case kCommandNoteAtLastVolume:
            argumentBytes = 1;
            event->handler = kPlaySoundSourceVoice;
            event->first = arguments[0];
            event->second = source->noteVolume;
            break;
        case kCommandMix:
            twoArguments(kUpdateSourceVoiceMix);
            break;
        case kCommandEnd:
            twoArguments(kSourceDataEnded);
            break;
        case kCommandSeek:
            argumentBytes = 4;
            event->handler = kSeekSourceVoice;
            event->seekOffset = static_cast<uint32_t>(readBytesU32(at));
            break;
        default: // no handler: the commands that do nothing, and any other
            break;
        }
        source->dataCursor = longForm ? at : at + argumentBytes;
    }
    READABLE_FUNCTION(readSoundSourceEvent, Verified);

    uint32_t soundSetEntry(ee::Ptr<SoundSet> soundSet, uint32_t note, ee::Ptr<ee::Ptr<BankSample>> sampleOut, ee::Ptr<int16_t> pitchOut);

    void linkSourceVoice(ee::Ptr<SoundSource> source, ee::Ptr<SourceVoice> voice);

    // playSoundSourceVoice: an event's note: a free voice, its sample (the source's
    // one, at this note's pitch, or the set's for this note), started, in
    // the source's list. 1 if it went in.
    uint32_t playSoundSourceVoice(ee::Ptr<SoundEvent> event)
    {
        ee::Local<int16_t> pitch;
        ee::Local<ee::Ptr<BankSample>> found;
        const ee::Ptr<SoundSource> source = event->source;
        const ee::Ptr<BankSample> sample = source->sample;
        source->noteVolume = event->second;
        *found = ee::Ptr<BankSample>();
        if (!sample)
        {
            if (source->sampleSet)
                soundSetEntry(source->sampleSet, event->first, found, pitch);
        }
        else
        {
            *found = sample;
            *pitch = static_cast<int16_t>((event->first - kMiddleNote) * kPitchPerNote);
        }
        const uint32_t start = *g_nextSourceVoice;
        if (!*found)
            return 0;
        uint32_t index = start;
        ee::Ptr<SourceVoice> voice;
        for (;;)
        {
            SourceVoice &candidate = (*g_sourceVoices)[index];
            index = index + 1 == kSourceVoiceCount ? 0 : index + 1;
            if ((candidate.flags & kSourceVoiceUsed) == 0)
            {
                voice = ee::Ptr<SourceVoice>(&candidate);
                candidate.flags = kSourceVoiceUsed;
                candidate.next = ee::Ptr<SourceVoice>();
                *g_nextSourceVoice = index;
                break;
            }
            if (index == start)
                break;
        }
        if (voice)
        {
            voice->sample = *found;
            voice->volume = event->second;
            voice->pitch = *pitch;
            const uint32_t timer = static_cast<uint32_t>(event->delay * source->ticksPerUnit) >> 12;
            voice->timer = timer == 0 ? 1 : timer;
            const uint16_t flags = source->flags;
            bool silent = (flags & kNeverSounds) != 0;
            if (!silent && *g_sourceVoicesPaused != 0 && (flags & kSilentWhilePaused) != 0)
                silent = true;
            bool started = false;
            if (!silent)
            {
                const uint32_t hardware = allocateVoice(*found, 255);
                voice->voice = static_cast<uint8_t>(hardware);
                if ((hardware & 0xFFu) == 255)
                {
                    voice->flags = 0;
                    *g_nextSourceVoice = index != 0 ? index - 1 : kSourceVoiceCount - 1;
                    return 0;
                }
                setVoicePitch(hardware & 0xFFu, voice->pitch);
                setVoiceVolume(voice->voice, mixedVolume(voice->volume, *source));
                setVoicePan(voice->voice, source->pan);
                startVoice(voice->voice);
                started = true;
            }
            if (!started)
                voice->flags = voice->flags | kSourceVoiceSilent;
            linkSourceVoice(source, voice);
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(playSoundSourceVoice, Verified);

    // updateSourceVoiceMix: the script changes a source: the event's first byte
    // says what, its second to what. 1, or 0 for a change there is none of.
    uint32_t updateSourceVoiceMix(ee::Ptr<SoundEvent> event)
    {
        const uint8_t change = event->first;
        const ee::Ptr<SoundSource> source = event->source;
        constexpr uint8_t kNone = 255;
        const auto eachSounding = [&](const auto &apply)
        {
            for (ee::Ptr<SourceVoice> voice = source->voices; voice; voice = voice->next)
            {
                if ((voice->flags & kSourceVoiceSilent) == 0)
                    apply(voice);
            }
        };
        switch (change)
        {
        case kChangeVolume:
            source->scriptVolume = event->second;
            if (source->voices)
            {
                eachSounding([&](ee::Ptr<SourceVoice> voice)
                             {
                                 setVoiceVolume(voice->voice, mixedVolume(voice->volume, *source));
                             });
                return 1;
            }
            break;
        case kChangePan:
            source->pan = static_cast<int8_t>(event->second);
            if (source->voices)
            {
                eachSounding([&](ee::Ptr<SourceVoice> voice)
                             {
                                 setVoicePan(voice->voice, source->pan);
                             });
                return 1;
            }
            break;
        case kChangeSample:
            if (event->second == kNone)
                source->sample = 0;
            else
                source->sample = sample(source->bank, event->second);
            break;
        case kChangeSampleSet:
            if (event->second == kNone)
                source->sampleSet = 0;
            else
                source->sampleSet = bankSoundSet(source->bank, event->second);
            break;
        case 2:
        case 3:
            break;
        default:
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(updateSourceVoiceMix, Verified);

    // sourceDataEnded: the script's end (its first byte 1): from the top again
    // if it repeats, else stopped; the source's callback told either way.
    uint32_t sourceDataEnded(ee::Ptr<SoundEvent> event)
    {
        const ee::Ptr<SoundSource> source = event->source;
        if (event->first == 1)
        {
            if ((source->flags & kRepeats) != 0)
            {
                source->voices = ee::Ptr<SourceVoice>();
                source->dataCursor = source->dataStart;
                source->timer = soundDataLength(source) << 12;
            }
            else
                source->state = kSourceStopped;
        }
        if (source->callback != 0)
            runSourceCallback(source->callback, source->callbackArgument, event->first, event->second);
        return 1;
    }
    READABLE_FUNCTION(sourceDataEnded, Verified);

    // seekSourceVoice: the script's cursor moved, by the event's word.
    uint32_t seekSourceVoice(ee::Ptr<SoundEvent> event)
    {
        const ee::Ptr<SoundSource> source = event->source;
        source->dataCursor = source->dataStart + event->seekOffset;
        source->timer = soundDataLength(source) << 12;
        return 1;
    }
    READABLE_FUNCTION(seekSourceVoice, Verified);

    // linkSourceVoice: a voice into a source's list, by the time it ends; the
    // timers stay differences.
    void linkSourceVoice(ee::Ptr<SoundSource> source, ee::Ptr<SourceVoice> voice)
    {
        ee::Ptr<SourceVoice> next = source->voices, previous;
        uint32_t before = 0;
        if (next)
        {
            uint32_t timer = next->timer;
            const uint32_t wanted = voice->timer;
            if (wanted >= timer)
            {
                previous = next;
                for (;;)
                {
                    next = next->next;
                    before += timer;
                    if (!next)
                        break;
                    timer = next->timer;
                    if (wanted < before + timer)
                        break;
                    previous = next;
                }
            }
        }
        if (previous)
        {
            previous->next = voice;
            voice->timer = voice->timer - before;
        }
        else
            source->voices = voice;
        voice->next = next;
        if (next)
            next->timer = next->timer - voice->timer;
    }
    READABLE_FUNCTION(linkSourceVoice, Verified);

    // releaseSourceVoice: a voice free, its hardware voice stopped and let go.
    int32_t releaseSourceVoice(ee::Ptr<SourceVoice> voice)
    {
        int32_t result = voice->flags & kSourceVoiceSilent;
        if (result == 0)
        {
            stopVoice(voice->voice, 0);
            result = releaseVoice(voice->voice);
        }
        voice->flags = 0;
        return result;
    }
    READABLE_FUNCTION(releaseSourceVoice, Verified);

    // soundSetEntry: a sample set's entry for a note: the sample and its
    // pitch, 1 if it has one.
    uint32_t soundSetEntry(ee::Ptr<SoundSet> soundSet, uint32_t note, ee::Ptr<ee::Ptr<BankSample>> sampleOut, ee::Ptr<int16_t> pitchOut)
    {
        const ee::Ptr<SoundSet> set = soundSet;
        const uint32_t wanted = note & 0xFFu;
        *sampleOut = ee::Ptr<BankSample>();
        if (wanted >= set->firstNote)
        {
            const int32_t first = set->firstNote;
            if (static_cast<int32_t>(wanted) < first + set->noteCount)
            {
                const SoundSet::Note entry = (ee::Ptr<SoundSet::Note>(set->notes))[wanted - first];
                constexpr uint8_t kNone = 255;
                if (entry.sample == kNone)
                    return 0;
                *sampleOut = zge::sample(set->bank, entry.sample);
                *pitchOut = static_cast<int16_t>(entry.notesAway * kPitchPerNote);
                return 1;
            }
        }
        return 0;
    }
    READABLE_FUNCTION(soundSetEntry, Verified);

    // readBytesU32: a little-endian word at any address.
    int32_t readBytesU32(ee::Ptr<uint8_t> bytes)
    {
        return bytes[0] + (bytes[1] << 8) + (bytes[2] << 16) + (bytes[3] << 24);
    }
    READABLE_FUNCTION(readBytesU32, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_effectsOn> g_effectsOn; // g_effectsOn ((anonymous)::g_effectsOn)
    }

    namespace
    {
        namespace routed
        {
        }
    }

    void resetSoundSources()
    {
        zsndSourceReset();
        *g_effectsOn = 0;
    }
    READABLE_FUNCTION(resetSoundSources, Verified);

    // ---- the source controls no level uses ----------------------------------------
    //
    // A source asked to play the first way (kHowMask 0) acts on its voices at
    // once; the second way (1) only changes its state; a later way (2, 3) is
    // marked stopping (kStopping) instead.

    namespace
    {
        // soundSourceState's answer.
        enum SoundSourceReport : uint32_t
        {
            kReportStopped = 1,
            kReportPlaying = 2, // running, or still stopping
            kReportPaused = 3,
            kReportOther = game::readable::perGame(5u, 4u, 5u), // a state not one of the three (ZGE2 answers 4)
        };
        constexpr uint32_t kHowNow = 0, kHowStateOnly = 1;
        constexpr uint32_t kNoVoice = 255;
        // setSoundSourceTiming's scale: 1.0 as 65536.
        constexpr ee::Float kMillisecondsPerSecond = 1000.0f;
        constexpr ee::Float kPerSixteenThousand = 6.25e-05f;
        constexpr uint32_t kTimingScaleShift = 4;

        // An unsigned word as a float, as the compiled code converts one: a
        // word with its top bit set is halved (keeping its low bit), then
        // doubled.
        ee::Float unsignedToFloat(uint32_t word)
        {
            if (static_cast<int32_t>(word) >= 0)
                return ee::toFloat(static_cast<int32_t>(word));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((word >> 1) | (word & 1u)));
            return half + half;
        }
    }

    // stopSourceVoices: a source stopped: asked to play the first way, its
    // voices let go; a later way, marked stopping.
    uint32_t stopSourceVoices(uint32_t handle)
    {
        const ee::Ptr<SoundSource> source = sourceAt(handle);
        *g_soundSourcesBusy = *g_soundSourcesBusy + 1;
        if (soundSourceActive(source) != 0)
        {
            const uint32_t how = source->flags & kHowMask;
            if (how == kHowNow)
            {
                ee::Ptr<SourceVoice> voice = source->voices;
                while (voice)
                {
                    releaseSourceVoice(voice);
                    voice = voice->next;
                }
                source->voices = ee::Ptr<SourceVoice>();
            }
            else if (how != kHowStateOnly)
                source->flags = source->flags | kStopping;
        }
        source->state = kSourceStopped;
        *g_soundSourcesBusy = *g_soundSourcesBusy - 1;
        return 0;
    }
    READABLE_FUNCTION(stopSourceVoices, Draft);

    // setSoundSourceSilenced (was startSourceVoices): a source's notes kept
    // from sounding (kNeverSounds) or let sound again. Silenced while it
    // plays the first way, its voices give up their hardware voices; let
    // sound the first or second way, each silent voice takes one again, at
    // its pitch and the source's mix, and starts.
    uint32_t setSoundSourceSilenced(uint32_t handle, uint32_t silenced)
    {
        *g_soundSourcesBusy = *g_soundSourcesBusy + 1;
        const ee::Ptr<SoundSource> source = sourceAt(handle);
        if (silenced != 0)
        {
            if (soundSourceActive(source) != 0)
            {
                const uint32_t how = source->flags & kHowMask;
                if (how == kHowNow)
                {
                    for (ee::Ptr<SourceVoice> voice = source->voices; voice; voice = voice->next)
                    {
                        if ((voice->flags & kSourceVoiceSilent) != 0)
                            continue;
                        stopVoice(voice->voice, 0);
                        releaseVoice(voice->voice);
                        voice->flags = voice->flags | kSourceVoiceSilent;
                    }
                }
                else if (how != kHowStateOnly)
                    source->flags = source->flags | kStopping;
            }
            source->flags = source->flags | kNeverSounds;
        }
        else
        {
            if ((source->flags & kHowMask) < 2)
            {
                for (ee::Ptr<SourceVoice> voice = source->voices; voice; voice = voice->next)
                {
                    if ((voice->flags & kSourceVoiceSilent) == 0)
                        continue;
                    const uint32_t hardware = allocateVoice(voice->sample, kNoVoice);
                    voice->voice = static_cast<uint8_t>(hardware);
                    if ((hardware & 0xFFu) == kNoVoice)
                        continue;
                    setVoicePitch(hardware & 0xFFu, voice->pitch);
                    setVoiceVolume(voice->voice, mixedVolume(voice->volume, *source));
                    setVoicePan(voice->voice, source->pan);
                    startVoice(voice->voice);
                    voice->flags = voice->flags & static_cast<uint8_t>(~kSourceVoiceSilent);
                }
            }
            else
                source->flags = source->flags | kStopping;
            source->flags = source->flags & static_cast<uint16_t>(~kNeverSounds);
        }
        *g_soundSourcesBusy = *g_soundSourcesBusy - 1;
        return 0;
    }
    READABLE_FUNCTION_WAITS(setSoundSourceSilenced, Draft);

    // pauseSourceVoices: a source's new notes silent from now (on) or not:
    // its kSilentWhilePaused set or cleared, and g_sourceVoicesPaused, for
    // every source, set or cleared with it. A source asked to play a later
    // way is marked stopping (on: only while it is active).
    uint32_t pauseSourceVoices(uint32_t handle, uint32_t paused)
    {
        const ee::Ptr<SoundSource> source = sourceAt(handle);
        if (paused != 0)
        {
            if (soundSourceActive(source) != 0 && (source->flags & kHowMask) >= 2)
                source->flags = source->flags | kStopping;
            source->flags = source->flags | kSilentWhilePaused;
            *g_sourceVoicesPaused = 1;
            return 0;
        }
        if ((source->flags & kHowMask) >= 2)
            source->flags = source->flags | kStopping;
        source->flags = source->flags & static_cast<uint16_t>(~kSilentWhilePaused);
        *g_sourceVoicesPaused = 0;
        return 0;
    }
    READABLE_FUNCTION(pauseSourceVoices, Draft);

    // soundSourceState: how a source stands (SoundSourceReport).
    uint32_t soundSourceState(uint32_t handle)
    {
        if (soundSourceActive(sourceAt(handle)) != 0)
            return kReportPlaying;
        const uint8_t state = sourceAt(handle)->state;
        if (state == kSourceStopped)
            return kReportStopped;
        if (state == kSourcePaused)
            return kReportPaused;
        return kReportOther;
    }
    READABLE_FUNCTION(soundSourceState, Draft);

    // setSoundSourceMode: how a source is asked to play (kHowMask); one that
    // is stopping gets its stop, pause, silencing or new-note pause again
    // the new way.
    uint32_t setSoundSourceMode(uint32_t handle, uint32_t how)
    {
        const uint32_t index = handle & 0xFFu;
        const ee::Ptr<SoundSource> source = sourceAt(index);
        const uint16_t flags = static_cast<uint16_t>((source->flags & kSourceFlagsAsked) | (how & kHowMask));
        source->flags = flags;
        if ((flags & kStopping) == 0)
            return 0;
        if (source->state == kSourceStopped)
            stopSourceVoices(index);
        else if (source->state == kSourcePaused)
            zge::stopSoundSource(index);
        else if ((flags & kNeverSounds) != 0)
            setSoundSourceSilenced(index, 1);
        else if ((flags & kSilentWhilePaused) != 0)
            pauseSourceVoices(index, 1);
        return 0;
    }
    READABLE_FUNCTION_WAITS(setSoundSourceMode, Draft);

    // setSoundSourceTiming (was setSoundSourceFadeTime): a source's script
    // timing from a scale, 65536 being newGroupSource's (a script unit a
    // millisecond): its frame's worth of timer scale * 1000 / (rate * 16),
    // and a note's length in frames rate * scale / 16000 (both 12.12).
    uint32_t setSoundSourceTiming(uint32_t handle, ee::Float scale)
    {
        const ee::Ptr<SoundSource> source = sourceAt(handle);
        const ee::Float perFrame = scale * kMillisecondsPerSecond / unsignedToFloat(*g_soundRate << kTimingScaleShift);
        source->frameTime = sce::libgcc::fptoui(perFrame);
        source->ticksPerUnit = sce::libgcc::fptoui(unsignedToFloat(*g_soundRate) * scale * kPerSixteenThousand);
        return 0;
    }
    READABLE_FUNCTION(setSoundSourceTiming, Draft);

    // setSoundSourceLooping: whether a source's script starts again at its
    // end (kRepeats).
    uint32_t setSoundSourceLooping(uint32_t handle, uint32_t looping)
    {
        const ee::Ptr<SoundSource> source = sourceAt(handle);
        if (looping != 0)
            source->flags = source->flags | kRepeats;
        else
            source->flags = source->flags & static_cast<uint16_t>(~kRepeats);
        return 0;
    }
    READABLE_FUNCTION(setSoundSourceLooping, Draft);

    // setSoundSourceCallback: what a source's script end calls (callback, a
    // game function: (argument, the event's two bytes)), and its argument.
    uint32_t setSoundSourceCallback(uint32_t handle, uint32_t callback, uint32_t argument)
    {
        const ee::Ptr<SoundSource> source = sourceAt(handle);
        source->callbackArgument = argument;
        source->callback = callback;
        return 0;
    }
    READABLE_FUNCTION(setSoundSourceCallback, Draft);
}
