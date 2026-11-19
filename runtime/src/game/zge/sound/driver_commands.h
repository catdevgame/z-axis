#pragma once

// The sound driver's commands: what the engine's sound code (zge/sound) sends
// ZAUDIO.IRX, Z-Axis' own sound driver on the IOP, over its one RPC
// (soundRpc(command, record, bytes, replyWanted): channels.cpp), as the
// driver reads them. The driver plays sample banks on 40 voices, streams
// files off the disc into four more, and writes each voice's status back into
// EE memory sixty times a second.
//
// Plain C++ on purpose: the typed calls the engine makes are zge/sound/
// driver.h; the host's driver (game_overrides/zge_sound.cpp), which stands in
// for ZAUDIO, reads the same records by these names, so the two sides agree
// on every word by one definition.

#include <cstddef>
#include <cstdint>

namespace zge::zaudio
{
    enum Command : uint32_t
    {
        kInit = 0,          // DriverInit: the driver started, told where to write the voices' status
        kReset = 1,         // every wave freed, every voice keyed off (shutdownSoundChannels sends it)
        kAllocateWave = 2,  // {bytes}: sound memory for a wave; the reply's word is its handle
        kFreeWave = 3,      // {handle}
        kSendWave = 4,      // WaveLoad: a piece of a wave
        kSetVoice = 5,      // VoiceCommand
        kKeyOn = 6,         // the 64-bit mask of the voices to start (kKeyMaskBytes)
        kKeyOff = 7,        // ... to stop
        kOpenStream = 8,    // StreamOpen; the reply's word is the stream's id
        kCloseStream = 9,   // {id}
        kStreamRequest = 10, // StreamRequest: the stream's source, pitch, loops, volume or fade
        kPlayStream = 11,   // {id}
        kStopStream = 12,   // {id}
        kPauseStream = 13,  // StreamPause
        kStatusOff = 14,    // the status writes held (a movie plays its own sound through the IOP)
        kStatusOn = 15,     // ... resumed
    };

    // ---- the voices --------------------------------------------------------------------

    // A voice's level, 0 .. 127 (128ths of full scale), as the driver's volume
    // word (the SPU2's VOLL / VOLR: 0x3F80 for 127).
    constexpr uint16_t voiceVolume(uint32_t level) { return static_cast<uint16_t>(level << 7); }
    constexpr uint32_t kFullVoiceLevel = 127;

    // The envelope's two words (the SPU2's ADSR1 and ADSR2), from a bank
    // sample's rates: attack 7 bits, decay 4, sustain level 4; sustain rate 7,
    // release 5. Every sample's sustain goes down linearly (kSustainFalling).
    constexpr uint16_t kSustainFalling = 1u << 14;
    constexpr uint16_t kReleaseRateMask = 0x1F;
    constexpr uint16_t attackDecaySustainWord(uint32_t attackRate, uint32_t decayRate, uint32_t sustainLevel)
    {
        return static_cast<uint16_t>((attackRate & 0x7Fu) << 8 | (decayRate & 0xFu) << 4 | (sustainLevel & 0xFu));
    }
    constexpr uint16_t sustainReleaseWord(uint32_t sustainRate, uint32_t releaseRate)
    {
        return static_cast<uint16_t>((sustainRate & 0x7Fu) << 6 | (releaseRate & kReleaseRateMask) | kSustainFalling);
    }
    // The fastest release: a stolen voice goes out at once.
    constexpr uint32_t kReleaseFast = 0xF;

    // What a VoiceCommand changes.
    enum VoiceChanges : uint32_t
    {
        kChangeWave = 1u << 0,
        kChangePitch = 1u << 1,
        kChangeVolume = 1u << 2,
        kChangeEnvelope = 1u << 3,
        kChangeAll = 0xF,
    };

    // A voice's parameters (kSetVoice). The engine keeps one in each voice's
    // record (voice.h) and sends it whole, marking what changed.
    struct VoiceCommand
    {
        uint32_t driverVoice;        // +0x00 the driver's voice, 0 .. 39
        uint32_t changes;            // +0x04 VoiceChanges
        uint32_t wave;               // +0x08 the wave's handle (kAllocateWave's reply; BMX XXX: its sound memory address)
        uint32_t pitch;              // +0x0C 0x1000 = 44100 Hz
        uint16_t volumeLeft;         // +0x10 voiceVolume()
        uint16_t volumeRight;        // +0x12
        uint16_t attackDecaySustain; // +0x14 attackDecaySustainWord()
        uint16_t sustainRelease;     // +0x16 sustainReleaseWord(); its low bits the release rate

        constexpr void setVolumes(uint32_t left, uint32_t right)
        {
            volumeLeft = voiceVolume(left);
            volumeRight = voiceVolume(right);
        }
        constexpr void setEnvelope(uint32_t attackRate, uint32_t decayRate, uint32_t sustainLevel, uint32_t sustainRate, uint32_t releaseRate)
        {
            attackDecaySustain = attackDecaySustainWord(attackRate, decayRate, sustainLevel);
            sustainRelease = sustainReleaseWord(sustainRate, releaseRate);
        }
        constexpr uint32_t releaseRate() const { return sustainRelease & kReleaseRateMask; }
        constexpr void setReleaseRate(uint32_t rate)
        {
            sustainRelease = static_cast<uint16_t>((sustainRelease & ~kReleaseRateMask) | (rate & kReleaseRateMask));
        }
    };
    static_assert(sizeof(VoiceCommand) == 0x18 && offsetof(VoiceCommand, volumeLeft) == 0x10 &&
                  offsetof(VoiceCommand, sustainRelease) == 0x16);

    // kSendWave's record: a kilobyte of a wave's sound data at an offset in it
    // (the engine's piece, banks.cpp's WavePiece, has each version's padding after it).
    constexpr uint32_t kWaveLoadBytes = 0x400;
    struct WaveLoad
    {
        uint32_t handle; // +0x00 the wave's (kAllocateWave's reply)
        uint32_t offset; // +0x04 where in the wave
        uint8_t data[kWaveLoadBytes]; // +0x08
    };
    static_assert(sizeof(WaveLoad) == 0x408);

    // kKeyOn's and kKeyOff's record: the 64-bit mask of voices (bit n: driver voice n).
    constexpr uint32_t kKeyMaskBytes = 8;

    // kInit's record.
    struct DriverInit
    {
        uint32_t statusOn;    // 1: write the voices' status
        uint32_t voiceStatus; // where: a byte per voice in EE memory
    };
    static_assert(sizeof(DriverInit) == 8);

    // A voice's status byte as the driver writes it each tick.
    constexpr int32_t kVoiceStatusSilent = 1;   // its envelope has run out
    constexpr int32_t kVoiceStatusKeyedOff = 2; // keyed off since the last tick (3 on the console too; never seen from the host)
    constexpr int32_t kVoiceStatusSounding = 4; // its envelope running

    // ---- the streams -------------------------------------------------------------------

    // kOpenStream's record (the stream's slot in the driver; its reply is the stream's id).
    enum StreamOpenFlags : uint32_t
    {
        kOpenLooped = 1u << 0,
        kOpenStereo = 1u << 1,
        kOpenNoSource = 1u << 2,   // the EE feeds the stream (no file)
        kOpenFileBytes = 1u << 3,  // the source's offset and length in bytes
        kOpenDiscSectors = 1u << 4, // ... in 2 KB sectors (the driver's reading of any flag but the two above)
    };
    struct StreamOpen
    {
        uint32_t flags;      // +0x00 StreamOpenFlags
        uint32_t alwaysZero; // +0x04
        uint32_t pitch;      // +0x08 0x1000 = 44100 Hz
        uint32_t slot;       // +0x0C the EE's stream record (SoundStream), for the driver to know it by
        uint32_t state;      // +0x10 the EE word the driver writes the stream's state into
    };
    static_assert(sizeof(StreamOpen) == 20);

    // kStreamRequest's record: one request per flag set.
    enum StreamRequestFlags : uint32_t
    {
        kRequestPitch = 1u << 0,
        kRequestLoops = 1u << 1,
        kRequestSource = 1u << 2,
        kRequestVolume = 1u << 3,
        kRequestFade = 1u << 4,
    };
    struct StreamRequest
    {
        uint32_t id;              // +0x00 the stream's id
        uint16_t flags;           // +0x04 StreamRequestFlags
        uint16_t pitch;           // +0x06 (kRequestPitch)
        uint32_t loops;           // +0x08 (kRequestLoops) how many times the source plays
        uint32_t length;          // +0x0C (kRequestSource) the source's length in the file
        uint32_t offset;          // +0x10 ... and where it starts
        char path[0x100];         // +0x14 ... the file's disc path
        uint32_t volume;          // +0x114 (kRequestVolume) streamVolume()
        uint32_t fadeTarget;      // +0x118 (kRequestFade) streamVolume() to fade to
        uint32_t fadeMilliseconds; // +0x11C ... over this long
        uint32_t fadeFlags;       // +0x120 ... and what happens at the end (each game's driver names its own)
    };
    static_assert(sizeof(StreamRequest) == 0x124 && offsetof(StreamRequest, path) == 0x14 &&
                  offsetof(StreamRequest, volume) == 0x114 && offsetof(StreamRequest, fadeFlags) == 0x120);

    // A stream's level, 0 .. 255, as the driver's volume word: 0 silence, else
    // the level in the word's top byte over 0x7F (0x7FFF for 255).
    constexpr uint32_t streamVolume(uint32_t level) { return level != 0 ? (level << 7) | 0x7Fu : 0u; }

    // kPauseStream's record.
    struct StreamPause
    {
        uint32_t id;     // +0x00
        uint32_t paused; // +0x04 1 paused, 0 playing again
    };
    static_assert(sizeof(StreamPause) == 8);

    // kCloseStream, kPlayStream, kStopStream and kPauseStream send this many
    // bytes from the stream's id word, whatever follows it (the driver reads
    // the id, and a pause its word after it).
    constexpr uint32_t kStreamIdBytes = 0x20;

    // The stream state word the driver writes (StreamOpen::state).
    enum StreamState : uint32_t
    {
        kStreamStopped = 2,
        kStreamPlayingOdd = 3, // playing, an odd number of ring halves crossed
        kStreamPlaying = 4,
        kStreamPaused = 6,
        kStreamNoRefill = 7,   // DM2's driver crossed a ring half with no refill in it
        kStreamEnding = 8,     // the source read to its end: stops at the next ring half
    };
}
