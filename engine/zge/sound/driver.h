#pragma once

// The engine's calls to its sound driver, by meaning: each is one command
// (driver_commands.h) sent over the driver's RPC, soundRpc (channels.cpp),
// with its record where the caller keeps it (a voice's command in the voice's
// record, a request in the caller's frame, the key masks in their globals).
// The record goes as its address and the driver reads it there; a reply is
// the word the driver wrote back.

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/sound/driver_commands.h"
#include "zge/sound/sound.h"

#include <cstdint>

namespace zge::zaudio
{
    constexpr uint32_t kReplyWanted = 1;
    constexpr uint32_t kNoReply = 0;

    // The word a command with a reply gets back (the reply buffer's first).
    inline uint32_t replyWord(uint32_t reply) { return *ee::Ptr<uint32_t>(reply); }

    // The driver started: the voices' status written where `init` says.
    inline void start(ee::Local<DriverInit> &init) { soundRpc(kInit, init, sizeof(DriverInit), kReplyWanted); }
    // Every wave freed and every voice keyed off, waited for. Returns the
    // reply buffer's address (shutdownSoundChannels returns it as its result).
    inline uint32_t reset() { return soundRpc(kReset, 0, 0, kReplyWanted); }

    // Sound memory for a wave of `bytes`: its handle.
    inline uint32_t allocateWave(ee::Local<uint32_t> &bytes)
    {
        return replyWord(soundRpc(kAllocateWave, bytes, sizeof(uint32_t), kReplyWanted));
    }
    // A piece of a wave sent (not waited for: the caller alternates two pieces).
    template <typename Piece>
    inline void sendWave(ee::Ptr<Piece> piece)
    {
        soundRpc(kSendWave, piece, sizeof(Piece), kNoReply);
    }
    inline void freeWave(ee::Local<uint32_t> &handle) { soundRpc(kFreeWave, handle, sizeof(uint32_t), kNoReply); }

    // A voice's parameters, as `command.changes` marks them.
    inline void setVoice(VoiceCommand &command)
    {
        soundRpc(kSetVoice, ee::Ptr<VoiceCommand>(&command), sizeof(VoiceCommand), kNoReply);
    }
    // The voices in the mask started, stopped.
    template <uint32_t Address>
    inline void keyOn(const ee::Global<uint64_t, Address> &voices)
    {
        soundRpc(kKeyOn, ee::Ptr<uint64_t>(voices.get()), kKeyMaskBytes, kNoReply);
    }
    template <uint32_t Address>
    inline void keyOff(const ee::Global<uint64_t, Address> &voices)
    {
        soundRpc(kKeyOff, ee::Ptr<uint64_t>(voices.get()), kKeyMaskBytes, kNoReply);
    }

    // A stream opened in the driver: its id.
    inline uint32_t open(ee::Local<StreamOpen> &open)
    {
        return replyWord(soundRpc(kOpenStream, open, sizeof(StreamOpen), kReplyWanted));
    }
    inline void close(ee::Local<uint32_t> &id) { soundRpc(kCloseStream, id, kStreamIdBytes, kReplyWanted); }
    inline void request(ee::Local<StreamRequest> &request) { soundRpc(kStreamRequest, request, sizeof(StreamRequest), kNoReply); }
    inline void play(ee::Local<uint32_t> &id) { soundRpc(kPlayStream, id, kStreamIdBytes, kNoReply); }
    inline void stop(ee::Local<uint32_t> &id) { soundRpc(kStopStream, id, kStreamIdBytes, kNoReply); }
    inline void pause(ee::Local<StreamPause> &pause) { soundRpc(kPauseStream, pause, kStreamIdBytes, kNoReply); }

    // The status writes held or resumed (around a movie, which plays its own
    // sound through the IOP), waited for.
    inline void statusWrites(bool on) { soundRpc(on ? kStatusOn : kStatusOff, 0, 0, kReplyWanted); }
}
