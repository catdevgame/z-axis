// The sound driver's RPC, and the sound channels' per-frame task (ps2Chan; the
// rest of the module, .. unloadSoundBankAt, is in the sweep): the driver's 40 voices, each stepped through its
// states on the status block the driver writes, and the frame's changes sent to
// the driver (zge/sound/driver.h): the voices to key on, a stolen voice with a fast
// release for its key-off, the voices to key off.

#include "zge/assets/file.h"
#include "zge/sound/bank.h"
#include "zge/memory/regions.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/platform/platform.h"
#include "sce/kernel.h"
#include "zge/platform/tasks.h"
#include "zge/sound/sound.h"
#include "zge/sound/driver.h"
#include "zge/sound/voice.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/sound/channels.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kSoundClient = sym::g_soundRpcClient;     // the SifRpcClientData of ZAUDIO.IRX
        inline ee::Global<sce::sceSifClientData, kSoundClient> g_soundClient;
        constexpr uint32_t kSoundReply = sym::soundReply;      // the reply's 0x200 bytes
        constexpr uint32_t kSoundReplyBytes = 0x200;
        constexpr uint32_t kSoundServerId = 0x47;
        constexpr uint32_t kRpcNoWait = 1;                 // SIF_RPCM_NOWAIT
        constexpr uint32_t kSifBindRpc = sym::sifBindRpc;      // (client, id, mode) -> negative: failed
        constexpr uint32_t kSifCheckStatRpc = sym::sifCheckStatRpc; // (client) -> 0 once it is done
        constexpr uint32_t kFlushCache = sym::flushCache;      // (0): the data cache written back
        constexpr int32_t kBindRetrySpins = 0x10000;

        enum : uint8_t
        {
            kVoiceFreedAtEnd = 1u << 0,
            kVoiceVolumeChanged = 1u << 1,
            kVoicePitchChanged = 1u << 2,
        };
        // A voice from its key-on to its end, which the status block tells
        // (zaudio::kVoiceStatusSilent, or keyed off: stopped).
        enum : uint16_t
        {
            kStateIdle = 1,
            kStateKeyOn = 2,
            kStatePlaying = 3,
            kStateFinished = 4,
            kStateStopped = 5,
        };
        constexpr uint32_t kVoiceCount = 40;

        // A channel's entry: what the game holds a voice by.
        struct ChannelEntry
        {
            uint8_t used;
            uint8_t voice; // 0xFF: none
        };

        constexpr uint32_t kVoices = sym::voices;
        constexpr uint32_t kChannels = sym::channels;
        constexpr uint32_t kVoiceStatus = sym::voiceStatus;    // a byte per voice, written by the driver
        constexpr uint32_t kVoiceStatusEnd = sym::g_soundBanks; // (its cache line)
        constexpr uint32_t kKeyOnRequests = sym::keyOnRequests;
        constexpr uint32_t kKeyOffRequests = sym::keyOffRequests;
        inline ee::Global<uint32_t, sym::g_soundFrames> g_soundFrames;
        inline ee::Global<int32_t, sym::g_voiceLockDepth> g_voiceLockDepth;
        inline ee::Global<int32_t, sym::g_soundUpdateHold> g_soundUpdateHold;
        inline ee::Global<uint64_t, kKeyOnRequests> g_keyOnRequests;
        inline ee::Global<uint64_t, kKeyOffRequests> g_keyOffRequests;
        // The voices allocateVoice took from their sounds this frame.
        inline ee::Global<uint64_t, sym::g_stolenVoices> g_stolenVoices;

        constexpr uint32_t kInvalidDCache = sym::InvalidDCache;

        // A voice's left and right volumes by its pan: the far side scaled
        // down, in 128ths. (A pan of -128 scales by -1 in 32 bits, the shift
        // unsigned, as the original has it.)
        void panVolumes(const Voice &voice, uint32_t &left, uint32_t &right)
        {
            const int32_t pan = voice.pan;
            const uint32_t volume = voice.volume;
            if (pan == 0)
                left = right = volume;
            else
            {
                if (pan > 0)
                {
                    right = volume;
                    left = (volume * static_cast<uint32_t>(127 - pan)) >> 7 & 0xFFu;
                }
                else
                {
                    left = volume;
                    right = (volume * static_cast<uint32_t>(pan + 127)) >> 7 & 0xFFu;
                }
            }
        }

        // A voice's step through its states; which of the original's two
        // joins its paths come through.
        enum class Join
        {
            Both,
            LastOnly,
            None,
        };
        Join stepVoice(Voice &voice, uint32_t index, uint64_t bit)
        {
            const uint32_t state = voice.state;
            if (state == kStatePlaying)
            {
                const int32_t status = ee::Ptr<int8_t>(kVoiceStatus)[index];
                if (status == zaudio::kVoiceStatusSilent)
                {
                    voice.state = kStateFinished;
                    return Join::Both;
                }
                if (status <= 0)
                    return Join::Both;
                if (status < zaudio::kVoiceStatusSounding)
                {
                    voice.state = kStateStopped;
                    return Join::Both;
                }
                return Join::LastOnly;
            }
            if (state < kStateFinished)
            {
                if (state != kStateKeyOn)
                    return Join::LastOnly;
                voice.state = kStatePlaying;
                return Join::Both;
            }
            if (state == kStateFinished)
                *g_keyOffRequests = *g_keyOffRequests | bit;
            else if (state != kStateStopped)
                return Join::None;
            if ((voice.flags & kVoiceFreedAtEnd) == 0)
            {
                voice.state = kStateIdle;
                return Join::LastOnly;
            }
            ChannelEntry &channel = ee::Ptr<ChannelEntry>(kChannels)[voice.channel];
            voice.flags = 0;
            voice.priority = 0;
            voice.state = 0;
            channel.voice = 0xFF;
            channel.used = 0;
            return Join::Both;
        }

        // ZGE4's step through a voice's states: it stays keyed on, finished
        // and stopped for kVoiceHeldFrames frames each (holdVoiceState).
        // Keyed on, then playing; playing until the status block says it
        // finished or stopped; finished, then keyed off and stopped; stopped,
        // then freed with its channel when the voice was to be freed at its
        // end or its channel is no longer used, else idle.
        void stepHeldVoice(Voice &voice, uint32_t index, uint64_t bit)
        {
            switch (voice.state)
            {
            case kStateKeyOn:
                if (countDownVoiceState(voice) == 0)
                    voice.state = kStatePlaying;
                return;
            case kStatePlaying:
            {
                const int32_t status = ee::Ptr<int8_t>(kVoiceStatus)[index];
                if (status == zaudio::kVoiceStatusSilent)
                {
                    voice.state = kStateFinished;
                    holdVoiceState(voice);
                }
                else if (status > 0 && status < zaudio::kVoiceStatusSounding)
                {
                    voice.state = kStateStopped;
                    holdVoiceState(voice);
                }
                return;
            }
            case kStateFinished:
                if (countDownVoiceState(voice) == 0)
                {
                    *g_keyOffRequests = *g_keyOffRequests | bit;
                    voice.state = kStateStopped;
                    holdVoiceState(voice);
                }
                return;
            case kStateStopped:
            {
                if (countDownVoiceState(voice) != 0)
                    return;
                ChannelEntry &channel = ee::Ptr<ChannelEntry>(kChannels)[voice.channel];
                if ((voice.flags & kVoiceFreedAtEnd) == 0 && channel.used != 0)
                {
                    voice.state = kStateIdle;
                    return;
                }
                voice.flags = 0;
                voice.priority = 0;
                voice.state = 0;
                channel.voice = 0xFF;
                channel.used = 0;
                return;
            }
            default:
                return;
            }
        }
    }

    namespace // the game functions below call: ee/plain.h
    {
        int32_t sifBindRpc(uint32_t client, uint32_t id, uint32_t mode) { return ee::game<int32_t>(kSifBindRpc, client, id, mode); }
        int32_t sifCheckStatRpc(uint32_t client) { return ee::game<int32_t>(kSifCheckStatRpc, client); }
    }

    // bindSoundRpc: the driver's RPC bound, as soon as the IOP has it. A game
    // override hooks it (PS2X_MOVIES=hle: this one runs). Returns the server,
    // or the bind's failure.
    int32_t bindSoundRpc()
    {
        for (;;)
        {
            const int32_t bound = sifBindRpc(kSoundClient, kSoundServerId, kRpcNoWait);
            if (bound < 0)
                return bound;
            while (sifCheckStatRpc(kSoundClient) != 0)
            {
            }
            const int32_t server = static_cast<int32_t>(g_soundClient->serve);
            if (server != 0)
                return server;
            // A while, and it binds again.
            for (int32_t spins = kBindRetrySpins; spins != -1; --spins)
            {
            }
        }
    }
    READABLE_FUNCTION_WAITS(bindSoundRpc, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void flushCache(uint32_t mode) { ee::game(kFlushCache, mode); }
    }

    // soundRpc: a command to the driver, once the last one is done; without
    // a reply it does not wait. A game override hooks it. Returns the reply's
    // buffer, 0 with none.
    uint32_t soundRpc(uint32_t function, ee::Address send, uint32_t sendBytes, uint32_t replyWanted)
    {
        const uint32_t mode = replyWanted == 0 ? kRpcNoWait : 0;
        flushCache(0);
        while (sifCheckStatRpc(kSoundClient) != 0)
        {
        }
        ee::Local<uint32_t> endFunctionData; // sceSifCallRpc's ninth argument, on the stack
        *endFunctionData = 0;
        callIopRpc(kSoundClient, function, mode, send, sendBytes, replyWanted != 0 ? kSoundReply : 0, replyWanted != 0 ? kSoundReplyBytes : 0, 0);
        return replyWanted != 0 ? kSoundReply : 0;
    }
    READABLE_FUNCTION_WAITS(soundRpc, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void InvalidDCache(uint32_t voiceStatus, uint32_t voiceStatusEnd) { ee::game(kInvalidDCache, voiceStatus, voiceStatusEnd); }
    }

    // soundTask (each frame, unless the updates are held): the sound
    // sources' update; then, with no voice being set up: the frame counted,
    // each voice's state stepped and a changed volume or pitch sent; the
    // key-ons sent; each stolen voice's envelope sent with a fast release, the
    // key-offs, then its own release back. Always 1. A game override wraps it
    // (the volume settings).
    uint32_t soundTask()
    {
        if constexpr (!game::readable::kIsDm2) // (ZGE2's updates cannot be held)
        {
            if (*g_soundUpdateHold != 0)
                return 1;
        }
        updateSoundSources();
        if (*g_voiceLockDepth != 0)
            return 1;

        ee::Local<zaudio::VoiceCommand> envelope;   // a stolen voice's, as sent
        ee::Local<uint8_t[kVoiceCount]> releases;   // each stolen voice's own release rate
        const ee::Ptr<Voice> voices(kVoices);

        *g_soundFrames = *g_soundFrames + 1;
        InvalidDCache(kVoiceStatus, kVoiceStatusEnd);
        for (uint32_t index = 0; index < kVoiceCount; ++index)
        {
            Voice &voice = voices[index];
            const uint64_t bit = 1ull << index;
            if (voice.priority != 0)
            {
                Join join = Join::LastOnly;
                if ((*g_keyOnRequests & bit) != 0)
                {
                    voice.state = kStateKeyOn;
                    holdVoiceState(voice); // (ZGE4 holds it keyed on)
                }
                else if constexpr (game::readable::kIsBmx)
                    stepHeldVoice(voice, index, bit);
                else
                    join = stepVoice(voice, index, bit);
                const uint32_t flags = voice.flags;
                if ((flags & (kVoiceVolumeChanged | kVoicePitchChanged)) != 0)
                {
                    zaudio::VoiceCommand &command = voice.command;
                    command.changes = 0;
                    command.driverVoice = voice.driverVoice;
                    if ((flags & kVoiceVolumeChanged) != 0)
                    {
                        uint32_t left = 0;
                        uint32_t right = 0;
                        panVolumes(voice, left, right);
                        command.setVolumes(left, right);
                        command.changes |= zaudio::kChangeVolume;
                    }
                    if ((flags & kVoicePitchChanged) != 0)
                    {
                        command.changes |= zaudio::kChangePitch;
                        command.pitch = voice.pitch;
                    }
                    zaudio::setVoice(command);
                    voice.flags = static_cast<uint8_t>(voice.flags & ~(kVoiceVolumeChanged | kVoicePitchChanged));
                }
            }
            if ((*g_stolenVoices & bit) != 0)
                releases[index] = static_cast<uint8_t>(voice.command.releaseRate());
        }

        if (*g_keyOnRequests != 0)
        {
            zaudio::keyOn(g_keyOnRequests);
            *g_keyOnRequests = 0;
        }

        // A stolen voice's parameters sent again, the envelope alone marked
        // changed, with the release given.
        const auto sendEnvelope = [&](const Voice &voice, uint32_t release)
        {
            *envelope = voice.command;
            envelope->changes = zaudio::kChangeEnvelope;
            envelope->setReleaseRate(release);
            zaudio::setVoice(*envelope);
        };
        if (*g_stolenVoices != 0)
        {
            for (uint32_t index = 0; index < kVoiceCount; ++index)
                if ((*g_stolenVoices & (1ull << index)) != 0)
                    sendEnvelope(voices[index], zaudio::kReleaseFast);
        }
        if (*g_keyOffRequests != 0)
        {
            zaudio::keyOff(g_keyOffRequests);
            *g_keyOffRequests = 0;
        }
        if (*g_stolenVoices != 0)
        {
            for (uint32_t index = 0; index < kVoiceCount; ++index)
                if ((*g_stolenVoices & (1ull << index)) != 0)
                    sendEnvelope(voices[index], releases[index]);
            *g_stolenVoices = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(soundTask, Verified);

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(flushCache, ee::game, kFlushCache);
            GAME_TABLE_ROUTE(sifCheckStatRpcint32_t, ee::game<int32_t>, kSifCheckStatRpc);
        }
    }

    uint32_t waitSoundRpc()
    {
        for (;;)
        {
            const int32_t sifCheckStatRpc = routed::sifCheckStatRpcint32_t(kSoundClient);
            if (sifCheckStatRpc != 0)
                continue;
            return 0;
        }
        return 0;
    }
    READABLE_FUNCTION(waitSoundRpc, Draft);

    // Ps2Chan: empty; dead (the first of a run of stubs)
    void ps2ChanUnusedHook()
    {
    }
    READABLE_FUNCTION(ps2ChanUnusedHook, Draft);

    // Ps2Chan: returns 0; dead (stub 1 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero1()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero1, Draft);

    // Ps2Chan: returns 0; dead (stub 2 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero2()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero2, Draft);

    // Ps2Chan: returns 0; dead (stub 3 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero3()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero3, Draft);

    // Ps2Chan: returns 0; dead (stub 4 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero4()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero4, Draft);

    // Ps2Chan: returns 0; dead (stub 5 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero5()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero5, Draft);

    // Ps2Chan: returns 0; dead (stub 6 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero6()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero6, Draft);

    // Ps2Chan: returns 0; dead (stub 7 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero7()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero7, Draft);

    // Ps2Chan: returns 0; dead (stub 8 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero8()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero8, Draft);

    // Ps2Chan: returns 0; dead (stub 9 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero9()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero9, Draft);

    // Ps2Chan: returns 0; dead (stub 10 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero10()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero10, Draft);

    // Ps2Chan: returns 0; dead (stub 11 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero11()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero11, Draft);

    // Ps2Chan: returns 0; dead (stub 12 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero12()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero12, Draft);

    // Ps2Chan: returns 0; dead (stub 13 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero13()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero13, Draft);

    // Ps2Chan: returns 0; dead (stub 14 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero14()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero14, Draft);

    // Ps2Chan: returns 0; dead (stub 15 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero15()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero15, Draft);

    // Ps2Chan: returns 0; dead (stub 16 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero16()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero16, Draft);

    // Ps2Chan: returns 0; dead (stub 17 of the 17 after ps2ChanUnusedHook: unimplemented channel calls)
    uint32_t ps2ChanStubZero17()
    {
        return 0;
    }
    READABLE_FUNCTION(ps2ChanStubZero17, Draft);

    // Ps2Chan: the hold count (g_soundUpdateHold) up one; ps2MoviePlay calls it (releaseSoundUpdates is its
    // pair)
    int32_t holdSoundUpdates()
    {
        const int32_t soundUpdateHold = *g_soundUpdateHold + 1;
        *g_soundUpdateHold = soundUpdateHold;
        return soundUpdateHold;
    }
    READABLE_FUNCTION(holdSoundUpdates, Verified);

    // Ps2Chan: the hold count (g_soundUpdateHold) down one; ps2MovieStopTasks calls it.
    int32_t releaseSoundUpdates()
    {
        const int32_t soundUpdateHold = *g_soundUpdateHold - 1;
        *g_soundUpdateHold = soundUpdateHold;
        return soundUpdateHold;
    }
    READABLE_FUNCTION(releaseSoundUpdates, Verified);

    uint32_t audioFrame()
    {
        return 0;
    }
    READABLE_FUNCTION(audioFrame, Verified);

    uint32_t releaseVoice(uint32_t voice)
    {
        stopVoice(voice & 0xFFu, 2);
        return 0;
    }
    READABLE_FUNCTION(releaseVoice, Verified);

    uint32_t iopCommand()
    {
        return 0;
    }
    READABLE_FUNCTION(iopCommand, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_nextVoice> g_nextVoice;           // where the search for a voice starts
        inline ee::Global<int32_t, sym::g_voiceSearchStart> g_channelSearchStart; // ... for a channel
        inline ee::Global<ee::Ptr<Task>, sym::g_soundChannelTask> g_soundChannelTask;
        inline ee::Global<uint32_t, sym::g_soundChannelsReady> g_soundChannelsReady;
        // What the game sees of each voice state.
        inline ee::Global<uint8_t[8], sym::g_voiceStateCodes> g_voiceStateCodes;
        // A pitch change in cents: the whole semitones, -36 to 36, then the
        // rest, each a factor of 0x1000.
        inline ee::Global<uint16_t[73], sym::g_pitchSemitoneTable> g_pitchSemitoneTable;
        inline ee::Global<uint16_t[100], sym::g_pitchCentsTable> g_pitchCentsTable;
        constexpr int32_t kMostSemitones = 36;

        constexpr uint32_t kChannelCount = 0x80;
        constexpr uint32_t kNoChannel = 255;
        constexpr uint8_t kNoVoice = 255;

        enum : uint32_t
        {
            kStopAtOnce = 1u << 0,
            kStopFreeChannel = 1u << 1,
        };

        // A voice's table entries are in use while this is alive: the
        // interrupt side leaves them alone.
        struct VoiceLock
        {
            VoiceLock() { *g_voiceLockDepth += 1; }
            ~VoiceLock() { *g_voiceLockDepth -= 1; }
        };

        // The sound task: ZGE2 (DM2) makes it at priority 6; the line of ps2Chan.c is each version's own, for its debug.
        constexpr uint32_t kSoundTaskPriority = game::readable::perGame(7u, 6u, 7u);
        constexpr uint32_t kSoundTaskLine = game::readable::perGame(235u, 233u, 237u);

        ee::Ptr<Voice> voiceAt(uint32_t index) { return ee::Ptr<Voice>(kVoices) + index; }
        ee::Ptr<ChannelEntry> channelAt(uint32_t index) { return ee::Ptr<ChannelEntry>(kChannels) + index; }
        uint64_t voiceBit(const Voice &voice) { return uint64_t{1} << (voice.driverVoice & 0x3F); }
        uint32_t nextVoiceIndex(uint32_t index) { return index + 1 == kVoiceCount ? 0 : index + 1; }

        // The SPU told to let it go at the next key-off.
        void requestKeyOff(uint64_t bit)
        {
            *g_keyOffRequests |= bit;
            *g_keyOnRequests &= ~bit;
        }

    }

    // The voice a channel is playing on, if it has one.
    ee::Ptr<Voice> voiceChannel(uint8_t channel)
    {
        if (channel >= kChannelCount || channelAt(channel)->used == 0)
            return nullptr;
        const uint8_t voice = channelAt(channel)->voice;
        return voice < kVoiceCount ? voiceAt(voice) : nullptr;
    }
    READABLE_FUNCTION(voiceChannel, Verified);

    // A channel for a sample at a priority, 255 if nothing can play it. It
    // takes the next free channel of the table, however long that takes, and
    // a voice: the first free one from where the last search stopped or, with
    // none, the least important one this priority may take from its sound.
    // sample may be 0: a voice to set up by hand.
    uint32_t allocateVoice(ee::Ptr<BankSample> sample, uint8_t priority)
    {
        ee::Ptr<WaveReference> reference;
        if (sample)
        {
            if (sample->reference == kNoWave)
                return kNoChannel;
            reference = waveRef(sample->bank, sample->reference);
            if (reference->wave == kNoWave)
                return kNoChannel;
            bankSampleInfo(sample->bank, reference->wave);
        }

        VoiceLock lock;
        uint32_t channel = *g_channelSearchStart;
        while (channelAt(channel)->used != 0)
        {
            channel = channel + 1 == kChannelCount ? 0 : channel + 1;
            // ZGE4 gives up after a lap of the table. Kept bug: it returns
            // with the voice lock still held, so soundTask steps no voice
            // again.
            if constexpr (game::readable::kIsBmx)
                if (channel == *g_channelSearchStart)
                {
                    *g_voiceLockDepth += 1;
                    return kNoChannel;
                }
        }
        channelAt(channel)->used = 1;
        channelAt(channel)->voice = kNoVoice;
        *g_channelSearchStart = channel + 1 == kChannelCount ? 0 : channel + 1;

        ee::Ptr<Voice> voice;
        uint32_t chosen = 0;
        uint32_t lowest = 255;
        uint32_t index = *g_nextVoice;
        do
        {
            const ee::Ptr<Voice> candidate = voiceAt(index);
            if (candidate->priority == 0)
            {
                voice = candidate;
                chosen = index;
                *g_nextVoice = nextVoiceIndex(index);
                break;
            }
            if (lowest >= candidate->priority)
            {
                lowest = candidate->priority;
                chosen = index;
            }
            index = nextVoiceIndex(index);
        } while (index != static_cast<uint32_t>(*g_nextVoice));

        if (!voice && priority >= lowest)
        {
            // Taken from its sound, which is told by g_stolenVoices.
            voice = voiceAt(chosen);
            const uint64_t bit = voiceBit(*voice);
            requestKeyOff(bit);
            *g_stolenVoices |= bit;
            if constexpr (game::readable::kIsBmx)
            {
                // (ZGE4 unlinks the voice's channel only when it was to be freed with the voice)
                if ((voice->flags & kVoiceFreedAtEnd) != 0)
                {
                    channelAt(voice->channel)->used = 0;
                    channelAt(voice->channel)->voice = kNoVoice;
                }
            }
            else
            {
                channelAt(voice->channel)->voice = kNoVoice;
                if ((voice->flags & kVoiceFreedAtEnd) != 0)
                    channelAt(voice->channel)->used = 0;
            }
            *g_nextVoice = nextVoiceIndex(chosen);
        }
        if (!voice)
        {
            channelAt(channel)->used = 0;
            return kNoChannel;
        }

        voice->priority = priority;
        voice->state = kStateIdle;
        voice->flags = 0;
        voice->channel = static_cast<uint8_t>(channel);
        voice->sample = sample;
        if (sample)
        {
            voice->volume = sample->volume;
            voice->pan = sample->pan;
            uint32_t left = 0, right = 0;
            panVolumes(*voice, left, right);

            zaudio::VoiceCommand &command = voice->command;
            command.changes = zaudio::kChangeAll;
            command.driverVoice = voice->driverVoice;
            command.wave = reference->handle;
            command.pitch = (uint32_t{sample->pitch} * reference->pitchBase) >> 12 & 0xFFFFu;
            command.setVolumes(left, right);
            command.setEnvelope(sample->attackRate, sample->decayRate, sample->sustainLevel, sample->sustainRate, sample->releaseRate);
            zaudio::setVoice(command);
        }
        else
        {
            voice->pan = 0;
            voice->volume = 127;
        }
        channelAt(channel)->voice = static_cast<uint8_t>(chosen);
        return channel;
    }
    READABLE_FUNCTION_WAITS(allocateVoice, Verified);

    // A channel's volume, as a share in 128ths of its sample's.
    void setVoiceVolume(uint8_t channel, uint8_t volume)
    {
        VoiceLock lock;
        const ee::Ptr<Voice> voice = voiceChannel(channel);
        if (voice)
        {
            voice->flags |= kVoiceVolumeChanged;
            voice->volume = static_cast<uint8_t>((uint32_t{volume} * voice->sample->volume) >> 7);
        }
    }
    READABLE_FUNCTION(setVoiceVolume, Verified);

    // A channel's pan, on top of its sample's: -127 to 127.
    void setVoicePan(uint8_t channel, int8_t pan)
    {
        VoiceLock lock;
        const ee::Ptr<Voice> voice = voiceChannel(channel);
        if (voice)
        {
            const int32_t total = pan + voice->sample->pan;
            voice->flags |= kVoiceVolumeChanged;
            voice->pan = static_cast<int8_t>(total < -127 ? -127 : total > 127 ? 127 : total);
        }
    }
    READABLE_FUNCTION(setVoicePan, Verified);

    // A channel's pitch: its wave's at its sample's, moved by `cents`. With none the voice is not told.
    void setVoicePitch(uint8_t channel, int16_t cents)
    {
        VoiceLock lock;
        const ee::Ptr<Voice> voice = voiceChannel(channel);
        if (!voice)
            return;

        const ee::Ptr<BankSample> sample = voice->sample;
        const ee::Ptr<WaveReference> reference = waveRef(sample->bank, sample->reference);
        voice->pitch = static_cast<uint16_t>((uint32_t{reference->pitchBase} * sample->pitch) >> 12);
        if (cents == 0)
            return;

        int32_t semitones = cents / 100;
        int32_t rest = cents % 100;
        if (rest < 0)
        {
            rest += 100;
            --semitones;
        }
        semitones = semitones < -kMostSemitones ? -kMostSemitones : semitones > kMostSemitones ? kMostSemitones : semitones;
        const uint32_t moved = (uint32_t{voice->pitch} * (*g_pitchSemitoneTable)[semitones + kMostSemitones]) >> 12;
        voice->flags |= kVoicePitchChanged;
        voice->pitch = static_cast<uint16_t>(((moved & 0xFFFFu) * (*g_pitchCentsTable)[rest]) >> 12);
    }
    READABLE_FUNCTION(setVoicePitch, Verified);

    // A voice keyed on at the next frame's request.
    void startVoice(uint8_t channel)
    {
        VoiceLock lock;
        const ee::Ptr<Voice> voice = voiceChannel(channel);
        if (voice)
        {
            const uint64_t bit = voiceBit(*voice);
            *g_keyOnRequests |= bit;
            *g_keyOffRequests &= ~bit;
            *g_stolenVoices &= ~bit;
            voice->state = kStateKeyOn;
        }
    }
    READABLE_FUNCTION(startVoice, Verified);

    // A sample played once at a pitch, volume, pan and priority: its channel
    // frees itself when it ends.
    void playSample(ee::Ptr<BankSample> sample, int16_t cents, uint8_t volume, int8_t pan, uint8_t priority)
    {
        if (!sample)
            return;
        VoiceLock lock;
        const uint32_t channel = allocateVoice(sample, priority);
        if (channel != kNoChannel)
        {
            setVoiceVolume(channel, volume);
            setVoicePan(channel, pan);
            setVoicePitch(channel, cents);
            startVoice(channel);
            voiceAt(channelAt(channel)->voice)->flags |= kVoiceFreedAtEnd;
        }
    }
    READABLE_FUNCTION_WAITS(playSample, Verified);

    // A channel stopped: its voice keyed off if it has started (stolen, with
    // kStopAtOnce, so its sound hears of it); with kStopFreeChannel the channel
    // given up too, at once if the voice never started or kStopAtOnce says so,
    // else when the voice ends.
    void stopVoice(uint8_t channel, uint32_t how)
    {
        VoiceLock lock;
        const ee::Ptr<Voice> voice = voiceChannel(channel);
        if (!voice)
        {
            if ((how & kStopFreeChannel) != 0)
                channelAt(channel)->used = 0;
            return;
        }

        // (ZGE4 leaves a voice that has finished or stopped alone, and holds
        // the one it keys off finished)
        if (voice->state >= kStateKeyOn &&
            (game::readable::kIsBmx ? voice->state < kStateFinished || voice->state == 6 : voice->state <= 6))
        {
            const uint64_t bit = voiceBit(*voice);
            requestKeyOff(bit);
            if ((how & kStopAtOnce) != 0)
                *g_stolenVoices |= bit;
            else
                *g_stolenVoices &= ~bit;
            if constexpr (game::readable::kIsBmx)
            {
                voice->state = kStateFinished;
                holdVoiceState(*voice);
            }
        }
        if ((how & kStopFreeChannel) == 0)
            return;

        if ((how & kStopAtOnce) != 0 || voice->state == kStateIdle)
        {
            voice->priority = 0;
            voice->flags = 0;
            voice->state = 0;
            channelAt(voice->channel)->voice = kNoVoice;
            channelAt(voice->channel)->used = 0;
        }
        else
            voice->flags |= kVoiceFreedAtEnd;
    }
    READABLE_FUNCTION(stopVoice, Verified);

    // Every voice of a sample stopped, as if stolen: a bank that goes takes
    // its sounds with it.
    void stopChannelsOfOwner(ee::Ptr<BankSample> sample)
    {
        VoiceLock lock;
        for (uint32_t i = 0; i < kVoiceCount; ++i)
        {
            const ee::Ptr<Voice> voice = voiceAt(i);
            if (voice->sample == sample)
            {
                const uint64_t bit = voiceBit(*voice);
                requestKeyOff(bit);
                *g_stolenVoices |= bit;
            }
        }
    }
    READABLE_FUNCTION(stopChannelsOfOwner, Verified);

    // Every channel released and the mixer told at once.
    uint32_t releaseAllVoices()
    {
        for (uint32_t channel = 0; channel < kChannelCount; ++channel)
        {
            if (channelAt(channel)->used != 0)
                releaseVoice(channel);
        }
        *g_nextVoice = 0;
        return game::tail(soundTask);
    }

    // releaseAllVoicesKeyedOff: ZGE4's releaseAllVoices: every channel
    // released, every voice keyed off with nothing keyed on or stolen, the
    // sound task run once to send it, then every voice free. 0.
    uint32_t releaseAllVoicesKeyedOff()
    {
        constexpr uint64_t kEveryVoice = (uint64_t{1} << kVoiceCount) - 1;
        for (uint32_t channel = 0; channel < kChannelCount; ++channel)
        {
            if (channelAt(channel)->used != 0)
                releaseVoice(channel);
        }
        *g_nextVoice = 0;
        *g_keyOffRequests = kEveryVoice;
        *g_keyOnRequests = 0;
        *g_stolenVoices = 0;
        soundTask();
        for (uint32_t index = 0; index < kVoiceCount; ++index)
        {
            voiceAt(index)->priority = 0;
            voiceAt(index)->state = 0;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS_OWN(releaseAllVoices, Verified, bmx, releaseAllVoicesKeyedOff);

    // The code the game sees for what a channel's voice is doing, 0 with none.
    uint32_t voiceState(uint8_t channel)
    {
        const ee::Ptr<Voice> voice = voiceChannel(channel);
        return voice ? (*g_voiceStateCodes)[voice->state] : 0;
    }
    READABLE_FUNCTION(voiceState, Verified);

    // The sample a channel plays.
    ee::Ptr<BankSample> voiceSound(uint8_t channel)
    {
        const ee::Ptr<Voice> voice = voiceChannel(channel);
        return voice ? voice->sample : ee::Ptr<BankSample>();
    }
    READABLE_FUNCTION(voiceSound, Verified);

    // The voice of a channel's table entry, whatever state the channel is in.
    uint32_t channelVoiceIndex(uint8_t channel) { return channelAt(channel)->voice; }
    READABLE_FUNCTION(channelVoiceIndex, Draft);

    // Each voice's state code, in a byte each.
    void copyVoiceStateCodes(ee::Ptr<uint8_t> out)
    {
        for (uint32_t i = 0; i < kVoiceCount; ++i)
            out[i] = (*g_voiceStateCodes)[voiceAt(i)->state];
    }
    READABLE_FUNCTION(copyVoiceStateCodes, Draft);

    // The channels emptied, the driver started and told where to write the
    // voices' status, the voices cleared and the sound task started.
    uint32_t initSoundChannels()
    {
        ee::Local<zaudio::DriverInit> init;

        *g_keyOnRequests = 0;
        *g_keyOffRequests = 0;
        *g_stolenVoices = 0;
        for (uint32_t i = 0; i < kChannelCount; ++i)
        {
            channelAt(i)->used = 0;
            channelAt(i)->voice = kNoVoice;
        }
        bindSoundRpc();
        init->statusOn = 1;
        init->voiceStatus = kVoiceStatus;
        zaudio::start(init);
        for (uint32_t i = 0; i < kVoiceCount; ++i)
        {
            const ee::Ptr<Voice> voice = voiceAt(i);
            voice->driverVoice = i;
            voice->priority = 0;
            voice->flags = 0;
            voice->sample = nullptr;
            voice->state = 0;
        }
        *g_keyOnRequests = 0;
        *g_keyOffRequests = 0;
        *g_stolenVoices = 0;
        *g_soundChannelTask = newTask(kSoundTaskPriority, sym::soundTask, 0, 1, sym::g_textNgengineZge3Ps2SoundPs2Chan, kSoundTaskLine);
        *g_soundChannelsReady = 1;
        return 0;
    }
    READABLE_FUNCTION_WAITS(initSoundChannels, Verified);

    // The sound task ended, every channel released and the driver stopped.
    uint32_t shutdownSoundChannels()
    {
        deleteTask(*g_soundChannelTask);
        for (uint32_t channel = 0; channel < kChannelCount; ++channel)
        {
            if (channelAt(channel)->used != 0)
                releaseVoice(channel);
        }
        const uint32_t reply = zaudio::reset();
        *g_soundChannelsReady = 0;
        return reply;
    }
    READABLE_FUNCTION_WAITS(shutdownSoundChannels, Verified);
}
