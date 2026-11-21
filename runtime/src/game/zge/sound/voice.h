#pragma once

// The sound channels' voices (zge/sound/channels.cpp): what the engine keeps
// of each of the driver's 40 voices, with the command it last sent for one
// (zaudio::VoiceCommand, driver_commands.h).

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/sound/bank.h"
#include "zge/sound/driver_commands.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // (channels.cpp's own: the records are its alone.)
    namespace
    {
        // A voice: ZGE4's (BMX XXX) keeps its state in a byte at +0x04, the
        // frames it stays in it after (stateFrames) and its pitch at +0x06
        // (BMX XXX's startVoice, setVoicePitch, initSoundChannels,
        // copyVoiceStateCodes, stopVoice and soundTask read them there).
        struct Voice
        {
            uint8_t flags;        // +0x00 kVoice*
            uint8_t priority;     // +0x01 0: free
            uint8_t volume;       // +0x02 0 .. 127
            int8_t pan;           // +0x03
#if ZX_GAME_BMX
            uint8_t state;        // BMX +0x04 kState*
            uint8_t stateFrames;  // BMX +0x05 frames left in a held state (soundTask counts them down)
            uint16_t pitch;       // BMX +0x06
#else
            uint16_t pitch;       // +0x04
            uint16_t state;       // +0x06 kState*
#endif
            uint8_t channel;      // +0x08 its entry in the channel table
            uint8_t _09[3];
            ee::Ptr<BankSample> sample; // +0x0C
            uint32_t driverVoice; // +0x10 the driver's voice it plays on, 0 .. 39
            uint8_t _14[0x0C];
            zaudio::VoiceCommand command; // +0x20 as last sent
            uint8_t _38[8];
        };
        static_assert(sizeof(Voice) == 0x40 && offsetof(Voice, command) == 0x20 &&
                      offsetof(Voice, state) == game::readable::perGame(0x06u, 0x06u, 0x04u) &&
                      offsetof(Voice, pitch) == game::readable::perGame(0x04u, 0x04u, 0x06u));

        // ZGE4 holds a voice keyed on, finished and stopped for this many
        // frames each (Voice::stateFrames); ZGE2 and ZGE3 hold none, and
        // these do nothing there.
        constexpr uint8_t kVoiceHeldFrames = 30;
        inline void holdVoiceState(Voice &voice)
        {
#if ZX_GAME_BMX
            voice.stateFrames = kVoiceHeldFrames;
#else
            (void)voice;
#endif
        }
        // A frame of the hold gone; the frames left.
        inline uint8_t countDownVoiceState(Voice &voice)
        {
#if ZX_GAME_BMX
            voice.stateFrames = static_cast<uint8_t>(voice.stateFrames - 1);
            return voice.stateFrames;
#else
            (void)voice;
            return 0;
#endif
        }
    }
}
