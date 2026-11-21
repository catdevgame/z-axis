#pragma once

// Sound streams (zge/sound/streams.cpp; the ps2Chan block soundStreamStubZeroA-soundStreamStubZeroG):
// four stream slots, each a stream of the sound driver (ZAUDIO.IRX on the
// IOP) playing a file off the disc: the music tracks and long samples. The
// engine only sends the driver's stream commands (zge/sound/driver.h); the
// driver reads, decodes and plays, and writes the stream's state back into
// the slot.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/sound/bank.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/sound/streams.h.h"

namespace zge
{
    // A stream file (16 at streamFiles, 0x10C bytes each; addStreamFile fills
    // one from a wave's WaveLocation, freeStreamFile frees it).
    struct StreamFile
    {
        uint16_t rate;       // +0x00 samples a second: 44100, 22050, 16000 or 11025 by the open's flags
        uint8_t channels;    // +0x02 2 (stereo, kStreamStereo) or 1
        uint8_t _03;
        uint32_t size;       // +0x04 the wave's size; 0: the record is free
        uint32_t offset;     // +0x08 where the wave starts in the file
        char name[0x100];    // +0x0C the file's name
    };
    static_assert(offsetof(StreamFile, size) == 4 && offsetof(StreamFile, name) == 0x0C && sizeof(StreamFile) == 0x10C);

    // A stream slot (4 at soundStreams, 0x40 bytes each).
    struct SoundStream
    {
        uint16_t flags;          // +0x00 kStreamInUse
        uint16_t _02;
        uint32_t id;             // +0x04 the driver's stream (the reply to the open)
        ee::Ptr<StreamFile> file; // +0x08 0 when none
        uint32_t loops;          // +0x0C the loop count last set (setSoundStreamLoops)
        uint32_t clearedAtOpen[2]; // +0x10
        uint8_t _18[0x18];
        // +0x30 the driver's state, written by the IOP (the open request
        // hands it this word's address): 0 .. 8, streamStatus's.
        uint32_t state;
        uint8_t _34[0x0C];
    };
    static_assert(offsetof(SoundStream, file) == 8 && offsetof(SoundStream, state) == 0x30 && sizeof(SoundStream) == 0x40);

    constexpr uint16_t kStreamInUse = 1u << 0;            // SoundStream::flags
    constexpr uint32_t kSoundStreams = sym::soundStreams; // SoundStream[4]
    constexpr uint32_t kSoundStreamCount = 4;
    constexpr uint32_t kStreamFiles = sym::streamFiles;  // StreamFile[16]

    // An open's flags (openSoundStream's last argument; addStreamFile reads
    // the rate and channel bits). ZGE2 (DM2) numbers them otherwise: looped
    // 0x02, the rates from 0x08 and stereo 0x100 (its addStreamFile,
    // openSoundStream, openMusicTrack and playSoundStreamSample); no ZGE2
    // code read tests a 22050 bit (22050 is every version's default rate).
    enum StreamFlags : uint32_t
    {
        kStreamStereo = game::readable::perGame(0x02u, 0x100u, 0x02u),
        kStream11025 = game::readable::perGame(0x04u, 0x08u, 0x04u),
        kStream16000 = game::readable::perGame(0x08u, 0x10u, 0x08u),
        kStream22050 = game::readable::perGame(0x10u, 0x00u, 0x10u),
        kStream44100 = game::readable::perGame(0x20u, 0x40u, 0x20u),
        kStreamLooped = game::readable::perGame(0x40u, 0x02u, 0x40u), // the driver's flag 1 [inferred: looped]
        kStreamFlag1000 = 0x1000, // the driver's flag 4, else 0x10 [unknown]
    };

    // streams.cpp
    uint32_t initSoundStreams();
    void closeAllSoundStreams();
    uint32_t streamsFrame();
    // (The third and fourth arguments, the callers' 0 and 0x8000, are not read.)
    uint32_t openSoundStream(ee::Ptr<ee::Ptr<SoundStream>> out, ee::Ptr<WaveLocation> sample, uint32_t, uint32_t, uint32_t flags);
    uint32_t closeSoundStream(ee::Ptr<SoundStream> stream);
    uint32_t openStream(ee::Ptr<SoundStream> stream, ee::Ptr<WaveLocation> sample, uint32_t flags);
    uint32_t setSoundStreamLoops(ee::Ptr<SoundStream> stream, uint32_t loops);
    uint32_t updateSoundStream();
    uint32_t streamStatus(ee::Ptr<SoundStream> stream, ee::Ptr<uint32_t> status);
    uint32_t playStream(ee::Ptr<SoundStream> stream);
    uint32_t stopStream(ee::Ptr<SoundStream> stream);
    uint32_t pauseSoundStream(ee::Ptr<SoundStream> stream, uint32_t paused);
    uint32_t playSoundStreamSample(ee::Ptr<WaveLocation> sample, uint32_t openArgument, uint32_t volume, uint32_t pan);
    uint32_t setStreamVolume(ee::Ptr<SoundStream> stream, uint32_t volume);
    uint32_t rampStreamVolume(ee::Ptr<SoundStream> stream, uint32_t milliseconds, uint32_t volume, uint32_t fadeFlags);
}

namespace zge
{
    // (streams.cpp)
    uint32_t soundStreamStubZeroG();
    uint32_t soundStreamStubZeroH();
    uint32_t startStreamSample(uint16_t bank, uint32_t first, uint32_t second, uint32_t flags);
    uint32_t playStreamSample(uint16_t bank, uint32_t second, uint32_t flags);
    uint32_t soundFrame();
    int32_t playChannelStream(uint8_t channelIndex, ee::Ptr<Playlist> playlist);
    uint32_t stopFinishedStream(uint8_t channelIndex);
    void stopStreamChannel(uint8_t channelIndex);
    void resetStreamChannels();
    void closeStreamChannels();
    int32_t soundVoiceState(uint8_t channelIndex);
    void setChannelVolume(uint8_t channelIndex, uint8_t volume, uint32_t /*pan*/);
    void pauseChannelStream(uint8_t channelIndex);
    void resumeChannelStream(uint8_t channelIndex);
    ee::Ptr<SoundStream> streamChannelStream(uint8_t channelIndex);
    void setMusicParameter(uint32_t parameter, uint32_t value);
}

namespace zge
{
    // (music.cpp)
    void setPlaylistEntry(ee::Address soundtrack, uint32_t index, uint32_t title, uint32_t artist);
    uint32_t resetStreamChannelPlaylists(uint32_t channelIndex); // ZGE4 only (streams.cpp)
}
