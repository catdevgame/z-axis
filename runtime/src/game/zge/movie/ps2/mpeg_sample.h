#pragma once

// Records of Sony's MPEG stream sample (audiodec.c, vobuf.c) as the movie
// player keeps them; ps2_movie.h has its video decoder.

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // audiodec.c's decoder: the stream's audio on its way to the IOP's buffer.
    struct AudioDec
    {
        int32_t state;            // +0x00 AudioDecState
        uint8_t _04[0x28];
        int32_t headerCount;      // +0x2C
        int32_t data;             // +0x30
        int32_t putOffset;        // +0x34
        int32_t count;            // +0x38
        int32_t size;             // +0x3C
        int32_t totalBytes;       // +0x40
        int32_t iopBuffer;        // +0x44
        int32_t iopBufferSize;    // +0x48
        int32_t iopLastPosition;  // +0x4C
        int32_t iopPausePosition; // +0x50
        int32_t totalBytesSent;   // +0x54
        int32_t iopZeroBuffer;    // +0x58
        // Z-Axis's word after the sample's: the movie's stream kind
        // (movie_player.cpp's g_movieStreamKind); only a PSS (1: video with
        // sound) starts or pauses the sound.
        int32_t streamKind;       // +0x5C
    };
    static_assert(offsetof(AudioDec, headerCount) == 0x2C && offsetof(AudioDec, iopBuffer) == 0x44);
    static_assert(offsetof(AudioDec, iopZeroBuffer) == 0x58 && offsetof(AudioDec, streamKind) == 0x5C);

    // audiodec.c's states (AudioDec::state) as audioDecStart, audioDecPause and
    // audioDecStop set them; audioDecEndPut sets preset once the stream's
    // 40-byte header is in, and audioDecSendToIOP then fills the IOP's ring
    // before the sound starts (audioDecIsPreset: until it has been filled).
    enum AudioDecState : int32_t
    {
        kAudioDecIdle = 0,
        kAudioDecPreset = 1,
        kAudioDecPlaying = 2,
        kAudioDecPaused = 3,
    };

    // libmpeg's sceMpegCbDataStr: what the demuxer hands a stream's callback.
    struct MpegStreamData
    {
        int32_t type;    // +0x00 sceMpegCbType
        uint32_t header; // +0x04 the packet's header
        uint32_t data;   // +0x08 the packet's data (in the read ring)
        uint32_t bytes;  // +0x0C
        uint64_t pts;    // +0x10 presentation time stamp
        uint64_t dts;    // +0x18 decoding time stamp
    };
    static_assert(offsetof(MpegStreamData, data) == 0x8 && offsetof(MpegStreamData, pts) == 0x10 &&
                  offsetof(MpegStreamData, dts) == 0x18);

    // vobuf.c's ring of decoded frames.
    struct VoBuf
    {
        int32_t data;  // +0x00 its slots of decoded pictures (uncached)
        int32_t tag;   // +0x04 their tags, a slot's first word its state
        int32_t write; // +0x08
        int32_t count; // +0x0C
        int32_t size;  // +0x10
    };
    static_assert(offsetof(VoBuf, tag) == 0x4 && offsetof(VoBuf, size) == 0x10);
}
