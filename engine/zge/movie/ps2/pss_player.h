#pragma once

// The movie player's PS2 form (zge/ps2/ps2Movie.c, movieNoop-setMovieAudioVolume;
// zge/movie/ps2/pss_player.cpp): Sony's MPEG stream sample (the ezmovie
// sources linked in before it, audioDecCreate-voBufDecCount: audiodec,
// readbuf, strfile, vibuf, videodec, vobuf, not rewritten here; Z-Axis's
// changes to them in zge/movie/ps2/mpeg_sample.cpp) driven as a task of the
// engine: a movie opened from a path (its buffers allocated, the file opened,
// the decoder set up), played into a texture (ps2MovieTexture) by a task at
// each frame, looped or stopped at its end. The engine's movie interface
// (zge/movie/movie.h) is in front of it; on the host the stream's parts
// (startMovie, stepMovie, endMovie and the movie's sound) have a host form
// (zge/host/movie_stream.cpp) that keeps the state below as this form leaves
// it.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "zge/assets/file.h"
#include "zge/movie/ps2/mpeg_sample.h"
#include "zge/render/texture.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/movie/ps2/pss_player.h.h"

namespace zge { struct Task; } // (zge/platform/tasks.h)

namespace zge
{
    // The sample's ReadBuf: a ring of five 64 KB reads between the file and
    // the demuxer.
    struct MovieReadBuffer
    {
        uint8_t data[5 * 0x10000];
        int32_t put;
        int32_t count;
        int32_t size;
    };
    static_assert(offsetof(MovieReadBuffer, size) == 0x50008 && sizeof(MovieReadBuffer) == 0x5000C);

    // The player's state (g_ps2MovieReady).
    struct MoviePlayer
    {
        uint32_t ready;       // +0x00 ps2MovieInit has run
        uint32_t open;        // +0x04 a movie is open
        uint32_t playing;     // +0x08
        uint32_t loop;        // +0x0C ps2MoviePlay's: 1 starts it again at its end
        uint32_t texture;     // +0x10 the texture it plays into (a Texture, newTexture)
        ee::Ptr<Task> task;   // +0x14 ps2MovieTask's task while it plays
        // The stream's buffers (kMemoryMovie; startMovie hands them to the
        // sample's parts):
        uint32_t frameData;   // +0x18 1 MB: voBufCreate's two decoded frames
        uint32_t frameTags;   // +0x1C 0x31C80 bytes: their DMA tags
        uint32_t videoInput;  // +0x20 1 MB: videoDecCreate's input (the video stream)
        uint32_t videoTags;   // +0x24 0x2010 bytes: its DMA tags
        ee::Ptr<MovieReadBuffer> readBuffer; // +0x28 readBufCreate's
        uint32_t decoderWork; // +0x2C 0x91768 bytes: sceMpeg's work area
        char path[0x100];     // +0x30 the open movie's path (a copy)
    };
    static_assert(offsetof(MoviePlayer, frameData) == 0x18 && offsetof(MoviePlayer, decoderWork) == 0x2C &&
                  offsetof(MoviePlayer, path) == 0x30);

    constexpr uint32_t kMoviePlayerAddress = sym::g_ps2MovieReady;
    inline ee::Global<MoviePlayer, kMoviePlayerAddress> g_moviePlayer;

    // The sample's video decoder (videodec.c's VideoDec at movieVideoDecoderAddress, with
    // Z-Axis's fields after it), as far as the player reads it.
    struct MovieVideoDecoder
    {
        // libmpeg's sceMpeg first: the stream's picture size.
        int32_t width;             // +0x00
        int32_t height;            // +0x04
        uint8_t _08[0x40];
        uint8_t inputBuffer[0x44]; // +0x48 its ViBuf (viBufAddDMA's)
        uint32_t inputDmaPending;  // +0x8C [inferred: the ViBuf's DMA to restart] readMain adds DMA while set
        uint8_t _90[0x18];
        uint32_t state;            // +0xA8 kMovieState
        uint32_t phase;            // +0xAC MoviePhase: stepMovie's (the audio may start once it decodes)
        uint8_t _b0[4];
        // +0xB4 how often the decoder has run out of data since the demuxer last
        // gave it video (mpegNodata counts, videoCallback and videoDecReset clear
        // it); over 100 once the audio runs ends the movie (ps2MovieTask).
        int32_t dataWaits;
        uint32_t startTime;        // +0xB8 the game clock when it started
        uint8_t _bc[8];
        // The decoded picture's hand-over to the texture (outputMovieFrame):
        uint32_t needPicture;      // +0xC4 1: the decoder may decode the next picture into the ring
        uint32_t uploadPending;    // +0xC8 1: the decoded picture goes into the texture
        // setMovieStreamCallback's (no caller): called with the decoder and
        // its data after each picture is uploaded.
        ee::FunctionPtr<void(ee::Ptr<MovieVideoDecoder> decoder, uint32_t data)> streamCallback; // +0xCC
        uint32_t streamCallbackData; // +0xD0
        ee::Ptr<Texture> texture;  // +0xD4 the player's
        uint32_t pixels;           // +0xD8 the decoded picture (a slot of voBuf's ring)
    };
    static_assert(offsetof(MovieVideoDecoder, inputDmaPending) == 0x8C && offsetof(MovieVideoDecoder, state) == 0xA8 &&
                  offsetof(MovieVideoDecoder, startTime) == 0xB8 && offsetof(MovieVideoDecoder, texture) == 0xD4 &&
                  offsetof(MovieVideoDecoder, needPicture) == 0xC4 && offsetof(MovieVideoDecoder, pixels) == 0xD8);

    constexpr uint32_t kMovieVideoDecoderAddress = sym::movieVideoDecoderAddress;
    inline ee::Global<MovieVideoDecoder, kMovieVideoDecoderAddress> g_movieVideoDecoder;

    // The movie's stream file: the sample's StrFile, which Z-Axis opens,
    // seeks and reads through zfile (strFileOpen and the rest).
    struct MovieStreamFile
    {
        ee::Ptr<ZFile> file; // +0x00 the open movie
        uint32_t size;       // +0x04 its size in bytes
    };
    static_assert(offsetof(MovieStreamFile, size) == 0x4 && sizeof(MovieStreamFile) == 0x8);

    // The movie's stream state (ps2Movie.c's globals): its audio
    // (g_movieHasAudio, g_movieAudioStarted), whether a sound bank goes with
    // it (g_movieHasBank: a ZSD beside the movie's file; stays set for a PSM
    // or PSB with one), the stream's kind by the file's extension (Z-Axis's
    // word after the sample's AudioDec: AudioDec::streamKind), the default
    // thread and its flags (g_movieDefaultKeepsRunning, g_movieDefaultParked),
    // and the stream's counts (g_movieStreamBytes, the file's size;
    // g_movieReadLeft, g_movieDemuxLeft: what is left to read from the file,
    // and to demux).
    inline ee::Global<uint32_t, sym::g_movieHasAudio> g_movieHasAudio;
    inline ee::Global<uint32_t, sym::g_movieAudioStarted> g_movieAudioStarted;
    inline ee::Global<int32_t, sym::g_movieHasBank> g_movieHasBank;
    enum MovieStreamKind : int32_t
    {
        kStreamPss = 1, // video with sound
        kStreamPsb = 2,
        kStreamPsa = 3,
    };
    inline ee::Global<int32_t, sym::g_movieStreamKind> g_movieStreamKind;
    inline ee::Global<int32_t, sym::g_movieDefaultThread> g_movieDefaultThread;
    inline ee::Global<uint32_t, sym::g_movieDefaultKeepsRunning> g_movieDefaultKeepsRunning;
    inline ee::Global<uint32_t, sym::g_movieDefaultParked> g_movieDefaultParked;
    inline ee::Global<int32_t, sym::g_movieStreamBytes> g_movieStreamBytes;
    inline ee::Global<int32_t, sym::g_movieReadLeft> g_movieReadLeft;
    inline ee::Global<int32_t, sym::g_movieDemuxLeft> g_movieDemuxLeft;
    // A word stepMovie clears as a movie starts and no code of the game sets;
    // decodeMoviePicture ends the movie when it is set.
    inline ee::Global<int32_t, sym::g_mpegPictureReady> g_mpegPictureReady;

    // The movie's IOP lock (its semaphore made by ps2MovieInit): Z-Axis takes
    // it around what the movie asks of the IOP while the stream runs on
    // another thread: the sound driver's remote calls, the audio's DMA, the
    // movie file's open, seek and reads, the stream's start and end. It counts
    // how deep it is held and, for a debugger, marks who holds it by a label
    // (a text in the game's data): the label's address while waiting for it,
    // one past that once it is held; the holder before is kept too.
    void takeIopLock(uint32_t label);
    // Lets it go; the kernel's result of signalling the semaphore (ZGE2: the
    // label it held, as it has no semaphore).
    int32_t releaseIopLock();

    // stepMovie's phases (MovieVideoDecoder::phase): set up on the first
    // step, then a picture each step, then the pictures left handed on.
    enum MoviePhase : uint32_t
    {
        kMoviePhaseStart = 0,
        kMoviePhaseDecoding = 2,
        kMoviePhaseEnded = 3,
    };

    // videodec.c's states (MovieVideoDecoder::state).
    enum MovieState : uint32_t
    {
        kMovieNormal = 0,
        kMovieAbort = 1,
        kMovieEnd = 3,
    };

    void movieNoop();
    int32_t switchThread();
    void proceedAudio();
    void movieDefaultMain();
    uint32_t ps2MovieInit();
    uint32_t ps2MovieShutdown();
    uint32_t ps2MovieStopTasks();
    int32_t allocateMovieBuffers();
    void freeMovieBuffers();
    uint32_t ps2MovieOpen(uint32_t path);
    uint32_t ps2MovieClose(uint32_t handedOn);
    void readMain();
    uint32_t outputMovieFrame(ee::Ptr<MovieVideoDecoder> decoder);
    int32_t decodeMoviePicture(ee::Ptr<MovieVideoDecoder> decoder);
    uint32_t ps2MovieTask(ee::Ptr<MovieVideoDecoder> decoder);
    uint32_t ps2MoviePlay(uint32_t movie, uint32_t loop);
    int32_t ps2MovieIsPlaying();
    uint32_t ps2MovieStop(uint32_t handedOn);
    ee::Ptr<uint32_t> ps2MovieTexture(uint32_t movie, ee::Ptr<uint32_t> ready);
    uint32_t ps2MoviePlayerTexture(); // ZGE2 only: the player's texture (a Texture)
    uint32_t setMovieStreamCallback(uint32_t callback, uint32_t data);
    uint32_t setMovieAudioVolume(uint32_t movie, uint32_t volume);
    uint32_t setMovieVolume(uint32_t volume); // ZGE2's setMovieAudioVolume
    uint32_t uncachedToPhysical(uint32_t address);
    uint32_t uncachedAddress(uint32_t address);
}

namespace zge
{
    // (mpeg_sample.cpp)
    int32_t changeInputVolume(uint32_t volume);
    int32_t setAudioDecVolume(ee::Ptr<AudioDec> audioDec, uint32_t volume);
    uint32_t audioDecDelete(ee::Ptr<AudioDec> audioDec);
    int32_t audioDecSendToIOP(ee::Ptr<AudioDec> audioDec);
    ee::Ptr<ZFile> strFileOpen(ee::Ptr<MovieStreamFile> stream, uint32_t path);
    uint32_t strFileReset(ee::Ptr<MovieStreamFile> stream);
    uint32_t strFileClose(ee::Ptr<MovieStreamFile> stream);
    int32_t strFileRead(ee::Ptr<MovieStreamFile> stream, uint32_t dst, uint32_t bytes);
    uint32_t stepMovie(ee::Ptr<MovieVideoDecoder> decoder);
}
