// The movie player's PS2 form (zge/movie/ps2/ps2_movie.h; ps2Movie.c,
// movieNoop-setMovieAudioVolume, and the two address helpers after it): the
// ezmovie sample's main loop (readMain, the default thread's movieDefaultMain)
// and Z-Axis's player around it, behind the engine's movie interface
// (zge/movie/movie.h). The sample's parts are called where they are (audioDec
// at audioDec, the stream file at streamFile, the read buffer, the video
// decoder at movieVideoDecoderAddress); the main thread's priority goes up (1)
// while it works the decoder and back (10) to let the decoder threads run.

#include "ps2_movie.h"

#include "zge/platform/platform.h"
#include "zge/text/cstring.h"
#include "sce/eeregs.h"
#include "sce/kernel.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/mirror.h"
#include "ee/io.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/memory/regions.h"
#include "zge/platform/tasks.h"
#include "zge/sound/bank.h"
#include "zge/sound/driver.h"
#include "zge/sound/sound.h"
#include "zge/collision/collision.h"
#include "zge/render/texture.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include "sym/zge/movie/ps2/ps2_movie.cpp.h"

namespace zge
{
    namespace
    {
        // The sample's parts (SDK sample code, not rewritten).
        constexpr uint32_t kAudioDecStart = sym::audioDecStartThunk;     // (audioDec)
        constexpr uint32_t kAudioDecReset = sym::audioDecStop;     // (audioDec)
        constexpr uint32_t kAudioDecIsPreset = sym::audioDecIsPreset;  // (audioDec) -> 1 when its buffer is full enough to start
        constexpr uint32_t kAudioDecSetVolume = sym::setAudioDecVolume; // (audioDec, volume)
        constexpr uint32_t kReadBufBeginPut = sym::readBufBeginPut;   // (readBuf, &where) -> bytes free
        constexpr uint32_t kReadBufEndPut = sym::readBufEndPut;     // (readBuf, bytes)
        constexpr uint32_t kReadBufBeginGet = sym::readBufBeginGet;   // (readBuf, &where) -> bytes held
        constexpr uint32_t kReadBufEndGet = sym::readBufEndGet;     // (readBuf, bytes)
        constexpr uint32_t kViBufAddDMA = sym::viBufAddDMA;       // (viBuf)
        constexpr uint32_t kVideoDecAbort = sym::videoDecAbort;     // (videoDec)
        constexpr uint32_t kVideoDecGetState = sym::videoDecGetState;  // (videoDec) -> MovieState
        constexpr uint32_t kVideoDecSetState = sym::videoDecSetState;  // (videoDec, state)
        constexpr uint32_t kVideoDecFlush = sym::videoDecFlush;     // (videoDec) -> 1 once flushing
        constexpr uint32_t kVideoDecIsFlushed = sym::videoDecIsFlushed; // (videoDec) -> 1 when flushed
        constexpr uint32_t kSceMpegDemuxPssRing = sym::sceMpegDemuxPssRing;
        // Z-Axis's parts not yet readable.
        constexpr uint32_t kMovieAudioReady = sym::movieSoundReady;  // () -> its audio is ready to start (or it has none)
        constexpr uint32_t kStartMovie = sym::startMovie;       // (path, or 0 to start the open one again)
        constexpr uint32_t kEndMovie = sym::endMovie;         // the decoder and audio taken down
        // [unknown: the IOP sound driver's command (14 as a movie starts,
        // 15 as it stops: the game's sound held off meanwhile)].
        // The engine.
        constexpr uint32_t kPs2MovieTask = sym::ps2MovieTask;
        constexpr uint32_t kSourceFile = sym::sourceFile; // "../ngengine/ZGE3/ps2/ps2Movie.c"
        constexpr uint32_t kNewTaskLine = game::readable::perGame(1151u, 1017u, 1156u); // ps2Movie.c's line, each version's own
        constexpr uint32_t kMovieTaskPriority = game::readable::perGame(263u, 262u, 263u);
        // The kernel.
        constexpr uint32_t kChangeThreadPriority = sym::changeThreadPriority;
        constexpr uint32_t kRotateThreadReadyQueue = sym::rotateThreadReadyQueue;
        constexpr uint32_t kGetThreadId = sym::getThreadId;
        constexpr uint32_t kCreateSema = sym::createSema;
        constexpr uint32_t kSignalSema = sym::signalSema;
        constexpr uint32_t kWaitSema = sym::waitSema;
        constexpr uint32_t kCreateThread = sym::createThread;       // (parameters) -> thread
        constexpr uint32_t kDeleteThread = sym::deleteThread;
        constexpr uint32_t kStartThread = sym::startThread;        // (thread, argument)
        constexpr uint32_t kTerminateThread = sym::terminateThread;
        constexpr uint32_t kSceSdRemoteInit = sym::sceSdRemoteInit;
        constexpr uint32_t kSceMpegInit = sym::sceMpegInit;
        constexpr uint32_t kSceMpegReset = sym::sceMpegReset;       // (videoDec's mpeg)
        // The sample's parts, to set up and take down.
        constexpr uint32_t kAudioDecCreate = sym::audioDecCreate;     // (audioDec, buffer, bytes, zero bytes)
        constexpr uint32_t kReadBufCreate = sym::readBufCreate;      // (readBuf)
        constexpr uint32_t kReadBufDelete = sym::readBufDelete;
        constexpr uint32_t kVideoDecCreate = sym::videoDecCreate;     // (videoDec, work, bytes, input, tags, tag count, audio buffer, bytes)
        constexpr uint32_t kVideoDecSetStream = sym::videoDecSetStream;  // (videoDec, type, channel, callback, data)
        constexpr uint32_t kVideoDecDelete = sym::videoDecDelete;     // (videoDec)
        constexpr uint32_t kVoBufCreate = sym::voBufCreate;        // (voBuf, frames (uncached), tags, frame count)
        constexpr uint32_t kVoBufDelete = sym::voBufDelete;
        constexpr uint32_t kPssVideoStream = sym::videoCallback, kPssAudioStream = sym::pcmCallback; // the demuxer's callbacks
        // The engine's disc and sound banks.

        constexpr ee::Ptr<AudioDec> kAudioDec{sym::audioDec};          // the sample's AudioDec
        constexpr ee::Ptr<MovieStreamFile> kStreamFile{sym::streamFile}; // its StrFile (the movie's file)
        constexpr uint32_t kVideoDec = kMovieVideoDecoderAddress;
        constexpr uint32_t kVideoInputBuffer = kVideoDec + offsetof(MovieVideoDecoder, inputBuffer);

        constexpr int32_t kDefaultPriority = 10; // the sample's DEF_PRIORITY (and the main thread's)
        constexpr int32_t kTopPriority = 1;
        constexpr int32_t kLowestDemuxBytes = 5; // below this nothing is left to demux
        constexpr uint32_t kReadUnit = 0x10000u; // the sample's READ_UNIT_SIZE

        inline ee::Global<int32_t, sym::g_audioDecSema> g_audioDecSema;
        inline ee::Global<int32_t, sym::g_audioDecLockDepth> g_audioDecLockDepth;
        // (the stream's state: ps2_movie.h)

        constexpr int32_t kDmaOn = 0x3;              // D_CTRL: DMA on, cycle stealing release
        constexpr int32_t kGifDmaInterrupt = 1 << 2; // D_STAT: channel 2's, the GIF's

        inline ee::Global<uint16_t, sym::g_movieBank> g_movieBank;
        // The decoder's and the frame buffer's words (videoDec, voBuf).
        inline ee::Global<int32_t, sym::g_movieDecoderTop> g_movieDecoderTop;
        inline ee::Global<int32_t, sym::g_movieDecoderTexture> g_movieDecoderTexture;
        constexpr uint32_t kVoBuf = sym::voBuf;
        constexpr uint32_t kMovieAudioBuffer = sym::movieAudioBuffer, kMovieAudioBufferBytes = 0x400;
        constexpr uint32_t kAudioDecBuffer = sym::audioDecBuffer, kAudioDecBufferBytes = 0xC000, kAudioDecZeroBytes = 0x6000;
        constexpr uint32_t kBankPath = sym::bankPath, kBankPathBytes = 0x100;
        constexpr uint32_t kBankExtension = sym::g_textZsd;     // "ZSD"
        constexpr uint32_t kExtensionPsm = sym::g_textPsm, kExtensionPss = sym::g_textPss, kExtensionPsa = sym::g_textPsa, kExtensionPsb = sym::g_textPsb;
        constexpr uint32_t kExtensionChars = 3;
        constexpr uint32_t kDecoderWorkBytes = game::readable::perGame(0x91768u, 0x90800u, 0x91768u), kVideoTagCount = 0x200; // (ZGE2's libmpeg asks less)
        constexpr uint32_t kFrameDataBytes = 1u << 20, kFrameTagBytes = 0x31C80, kVideoInputBytes = 1u << 20, kVideoTagBytes = 0x2010;
        constexpr uint32_t kDefaultThreadStack = sym::defaultThreadStack, kDefaultThreadStackBytes = 0x2000;
        constexpr uint32_t kGlobalPointer = sym::gp;
        constexpr int32_t kStreamVideo = 0, kStreamAudio = 2, kDecodedFrames = 2;
        constexpr uint32_t kStartLock = sym::g_textRemoteInit, kEndLock = sym::g_textDeleteThread; // the IOP lock's labels (ps2_movie.h)

        MoviePlayer &player() { return *g_moviePlayer; }

        namespace // the game functions below call: ee/plain.h
        {
            int32_t videoDecGetState(ee::Address videoDec) { return ee::game<int32_t>(kVideoDecGetState, videoDec); }
        }

        int32_t videoState() { return videoDecGetState(kVideoDec); }
    }

    // movieNoop: nothing (videoCallback's and viBufAddDMA's hook).
    void movieNoop()
    {
    }
    READABLE_FUNCTION(movieNoop, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t rotateThreadReadyQueue(uint32_t defaultPriority) { return ee::tailCall<int32_t>(kRotateThreadReadyQueue, defaultPriority); }
        }
    }

    // switchThread: the other threads of the default priority run (the
    // sample's switchThread).
    int32_t switchThread()
    {
        return tail::rotateThreadReadyQueue(kDefaultPriority);
    }
    READABLE_FUNCTION(switchThread, Verified);

    // proceedAudio: the audio sent on to the IOP when the movie has any
    // (the sample's proceedAudio; ZGE2 sends it whether or not).
    void proceedAudio()
    {
        if (game::readable::kIsDm2 || *g_movieHasAudio != 0)
        {
            game::tail(audioDecSendToIOP, kAudioDec);
            return;
        }
    }
    READABLE_FUNCTION_WAITS(proceedAudio, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t videoDecFlush(uint32_t videoDec) { return ee::game<int32_t>(kVideoDecFlush, videoDec); }
        int32_t videoDecIsFlushed(uint32_t videoDec) { return ee::game<int32_t>(kVideoDecIsFlushed, videoDec); }
    }

    // movieDefaultMain: the default thread (the sample's defMain): the stream
    // read and demuxed until it runs out or the movie ends, the decoder
    // flushed, then it waits while the movie stays ended; when the player is
    // not to go on it parks for good (flag set), else it starts over.
    void movieDefaultMain()
    {
        for (;;)
        {
            // Read and demux while there is stream left and the movie runs.
            while (*g_movieDemuxLeft >= kLowestDemuxBytes && videoState() != static_cast<int32_t>(kMovieEnd))
            {
                readMain();
                switchThread();
            }
            // The decoder flushed.
            while (videoDecFlush(kVideoDec) == 0)
                switchThread();
            while (videoDecIsFlushed(kVideoDec) == 0 && videoState() != static_cast<int32_t>(kMovieEnd))
                switchThread();
            // Wait while the movie stays ended.
            while (videoState() == static_cast<int32_t>(kMovieEnd))
            {
                if (*g_movieDefaultKeepsRunning == 0)
                {
                    *g_movieDefaultParked = 1;
                    for (;;)
                        switchThread();
                }
                switchThread();
            }
            switchThread();
        }
    }
    READABLE_FUNCTION_WAITS(movieDefaultMain, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t createSema(ee::Ptr<sce::SemaParam> parameters) { return ee::game<int32_t>(kCreateSema, parameters); }
    }

    // ps2MovieInit: the player made ready once: the audio's semaphore (one
    // count) and the texture it plays into (ZGE2 makes the texture as each
    // movie opens, movieOpenWithTexture). 1.
    uint32_t ps2MovieInit()
    {
        if (player().ready == 0)
        {
            player().open = 0;
            player().ready = 1;
            player().playing = 0;
            ee::Local<sce::SemaParam> semaphore;
            semaphore->maxCount = 1;
            semaphore->initCount = 1;
            *g_audioDecSema = createSema(semaphore);
            *g_audioDecLockDepth = 0;
            if constexpr (!game::readable::kIsDm2)
                player().texture = ee::Address(newLoadedTexture());
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(ps2MovieInit, Verified);

    // ps2MovieShutdown: the player's texture freed. 1.
    uint32_t ps2MovieShutdown()
    {
        if (player().ready != 0)
        {
            releaseLoadedTexture(ee::Ptr<Texture>(player().texture));
            player().ready = 0;
        }
        return 1;
    }

    uint32_t ps2MovieClose(uint32_t handedOn);

    // movieShutdownClosing: ZGE2's ps2MovieShutdown: an open movie closed (its
    // texture is the movie's, movieCloseWithTexture), the player no longer
    // ready. 1. (The original hands the close whatever its own caller left as
    // a first argument; ZGE2's close and stop never read it.)
    uint32_t movieShutdownClosing()
    {
        constexpr uint32_t kNotRead = 0;
        if (player().ready != 0)
        {
            if (player().open != 0)
                ps2MovieClose(kNotRead);
            player().ready = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(ps2MovieShutdown, Verified, dm2, movieShutdownClosing);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t audioDecIsPreset(ee::Ptr<AudioDec> audioDec) { return ee::game<uint32_t>(kAudioDecIsPreset, audioDec); }
    }

    // movieSoundReady: 1 when the movie's audio can start, or it has none.
    uint32_t movieSoundReady()
    {
        uint32_t ready = 1;
        if (*g_movieHasAudio != 0)
            ready = audioDecIsPreset(kAudioDec);
        return ready;
    }
    READABLE_FUNCTION(movieSoundReady, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void readBufCreate(ee::Ptr<MovieReadBuffer> readBuf) { ee::game(kReadBufCreate, readBuf); }
        void sceMpegInit() { ee::game(kSceMpegInit); }
        void videoDecCreate(uint32_t videoDec, uint32_t mpegWork, uint32_t mpegWorkSize, uint32_t data, uint32_t tag, uint32_t tagSize, uint32_t timeStamps, uint32_t timeStampCount) { ee::game(kVideoDecCreate, videoDec, mpegWork, mpegWorkSize, data, tag, tagSize, timeStamps, timeStampCount); }
        void sceSdRemoteInit() { ee::game(kSceSdRemoteInit); }
        void audioDecCreate(ee::Ptr<AudioDec> audioDec, uint32_t data, uint32_t size, uint32_t iopBufferSize) { ee::game(kAudioDecCreate, audioDec, data, size, iopBufferSize); }
        void videoDecSetStream(uint32_t videoDec, uint32_t type, uint32_t channel, uint32_t callback, ee::Ptr<MovieReadBuffer> data) { ee::game(kVideoDecSetStream, videoDec, type, channel, callback, data); }
        void voBufCreate(uint32_t voBuf, uint32_t uncached, uint32_t tag, uint32_t size) { ee::game(kVoBufCreate, voBuf, uncached, tag, size); }
        // The kernel reads the record alone; the words after it are what the
        // start-up last had in hand, and the call hands them on all the same.
        int32_t createThread(ee::Ptr<sce::ThreadParam> parameters, uint32_t stackBytes, uint32_t priority, uint32_t globalPointer, uint32_t streamBytes) { return ee::game<int32_t>(kCreateThread, parameters, stackBytes, priority, globalPointer, streamBytes); }
        int32_t startThread(uint32_t thread, uint32_t argument) { return ee::game<int32_t>(kStartThread, thread, argument); }
    }

    // startMovie: the sample's parts up for a movie (the path's extension
    // says what it holds; 0: as the last one), its sound bank beside it
    // loaded if it has one, and the default thread started (its host form:
    // zge/host/movie_stream.cpp). Returns StartThread's.
    int32_t startMovie(uint32_t path)
    {
        MoviePlayer &movie = player();
        readBufCreate(movie.readBuffer);
        sceMpegInit();
        videoDecCreate(kVideoDec, movie.decoderWork, kDecoderWorkBytes, movie.videoInput, movie.videoTags, kVideoTagCount, kMovieAudioBuffer, kMovieAudioBufferBytes);
        *g_movieDecoderTop = 0;
        *g_movieDecoderTexture = static_cast<int32_t>(movie.texture);
        *g_movieAudioStarted = 0;
        if (path != 0)
        {
            const uint32_t fileExtension = path + stringLength(path) - kExtensionChars;
            const auto isExtension = [&](uint32_t extension)
            {
                return stringCompareNoCase(fileExtension, extension) == 0;
            };
            if (isExtension(kExtensionPsm))
                *g_movieHasAudio = 0;
            else if (isExtension(kExtensionPss))
            {
                *g_movieStreamKind = kStreamPss;
                *g_movieHasAudio = 1;
            }
            else
            {
                if (isExtension(kExtensionPsa))
                {
                    *g_movieStreamKind = kStreamPsa;
                    *g_movieHasBank = 0;
                }
                else
                {
                    if (isExtension(kExtensionPsb))
                        *g_movieStreamKind = kStreamPsb;
                    *g_movieHasAudio = 0;
                }
            }
        }
        takeIopLock(kStartLock);
        sceSdRemoteInit();
        releaseIopLock();
        if (*g_movieHasAudio != 0)
            audioDecCreate(kAudioDec, kAudioDecBuffer, kAudioDecBufferBytes, kAudioDecZeroBytes);
        if (*g_movieHasBank != 0)
        {
            if (path != 0)
            {
                memoryFill(kBankPath, 0, kBankPathBytes);
                const int32_t length = stringLength(path);
                stringCopyN(kBankPath, path, length - kExtensionChars);
                appendString(kBankPath, kBankExtension);
            }
            const int32_t failed = loadBank(kBankPath, 0, g_movieBank.get());
            *g_movieHasBank = failed == 0 ? 1 : 0;
        }
        videoDecSetStream(kVideoDec, kStreamVideo, 0, kPssVideoStream, movie.readBuffer);
        if (*g_movieHasAudio != 0)
            videoDecSetStream(kVideoDec, kStreamAudio, 0, kPssAudioStream, movie.readBuffer);
        voBufCreate(kVoBuf, ee::uncachedMirror(ee::physical(movie.frameData)), movie.frameTags, kDecodedFrames);
        const int32_t streamBytes = *g_movieStreamBytes;
        *g_movieDefaultKeepsRunning = 1;
        *g_movieDemuxLeft = streamBytes;
        *g_movieReadLeft = streamBytes;
        *g_movieDefaultParked = 0;
        ee::Local<sce::ThreadParam> defaultThread;
        defaultThread->entry = sym::movieDefaultMain;
        defaultThread->stack = kDefaultThreadStack;
        defaultThread->stackSize = kDefaultThreadStackBytes;
        defaultThread->gpReg = kGlobalPointer;
        defaultThread->initPriority = kDefaultPriority;
        defaultThread->option = 0;
        const int32_t thread = createThread(defaultThread, kDefaultThreadStackBytes, kDefaultPriority, kGlobalPointer, streamBytes);
        *g_movieDefaultThread = thread;
        return startThread(thread, 0);
    }
    READABLE_FUNCTION_WAITS(startMovie, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t getThreadId() { return ee::game<int32_t>(kGetThreadId); }
        void changeThreadPriority(uint32_t thread, uint32_t defaultPriority) { ee::game(kChangeThreadPriority, thread, defaultPriority); }
        void waitSema(uint32_t dvdLock) { ee::game(kWaitSema, dvdLock); }
        void signalSema(uint32_t dvdLock) { ee::game(kSignalSema, dvdLock); }
        void terminateThread(uint32_t movieDefaultThread) { ee::game(kTerminateThread, movieDefaultThread); }
        void deleteThread(uint32_t movieDefaultThread) { ee::game(kDeleteThread, movieDefaultThread); }
        void readBufDelete() { ee::game(kReadBufDelete); }
        void voBufDelete() { ee::game(kVoBufDelete); }
        void videoDecDelete(uint32_t videoDec) { ee::game(kVideoDecDelete, videoDec); }
        void sceMpegReset(uint32_t videoDec) { ee::game(kSceMpegReset, videoDec); }
        namespace tail
        {
        int32_t changeThreadPriority(uint32_t main, uint32_t defaultPriority) { return ee::tailCall<int32_t>(kChangeThreadPriority, main, defaultPriority); }
        }
    }

    // endMovie: the bank freed, the default thread parked and removed, the
    // sample's parts down (its host form: zge/host/movie_stream.cpp). Returns
    // what setting the priority back does.
    int32_t endMovie()
    {
        if (*g_movieHasBank != 0)
            unloadSoundBank(*g_movieBank);
        const int32_t thread = getThreadId();
        changeThreadPriority(thread, kDefaultPriority);
        *g_movieDefaultKeepsRunning = 0;
        // Let it finish its loop.
        while (*g_movieDefaultParked == 0)
            switchThread();
        const int32_t self = getThreadId();
        changeThreadPriority(self, kTopPriority);
        takeIopLock(kEndLock);
        // The disc client's reads have finished once its lock is free.
        const int32_t dvdLock = dvdReadError();
        waitSema(dvdLock);
        signalSema(dvdLock);
        terminateThread(*g_movieDefaultThread);
        deleteThread(*g_movieDefaultThread);
        readBufDelete();
        voBufDelete();
        releaseIopLock();
        videoDecDelete(kVideoDec);
        if (*g_movieHasAudio != 0)
            audioDecDelete(kAudioDec);
        sceMpegReset(kVideoDec);
        const int32_t main = getThreadId();
        return tail::changeThreadPriority(main, kDefaultPriority);
    }
    READABLE_FUNCTION_WAITS(endMovie, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void audioDecStop(ee::Ptr<AudioDec> audioDec) { ee::game(kAudioDecReset, audioDec); }
    }

    // ps2MovieStopTasks: the playing stopped: the audio reset, the task deleted,
    // and the game's sound let go again. 1.
    uint32_t ps2MovieStopTasks()
    {
        if (*g_movieHasAudio != 0)
            audioDecStop(kAudioDec);
        if (player().task != 0)
        {
            deleteTask(player().task);
            player().task = 0;
        }
        if constexpr (game::readable::kIsDm2) // (ZGE2 holds no sound updates while a movie plays)
            player().playing = 0;
        else if (player().playing != 0)
        {
            player().playing = 0;
            if (*g_movieHasAudio != 0)
                releaseSoundUpdates();
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(ps2MovieStopTasks, Verified);

    // allocateMovieBuffers: the stream's buffers (MoviePlayer, kMemoryMovie, cleared,
    // the cache written back); 1 when all six were had.
    int32_t allocateMovieBuffers()
    {
        constexpr uint32_t kWriteBack = 1;
        MoviePlayer &movie = player();
        movie.frameData = allocateIn(kFrameDataBytes, kMemoryMovie, kWriteBack);
        movie.frameTags = allocateIn(kFrameTagBytes, kMemoryMovie, kWriteBack);
        movie.videoInput = allocateIn(kVideoInputBytes, kMemoryMovie, kWriteBack);
        movie.videoTags = allocateIn(kVideoTagBytes, kMemoryMovie, kWriteBack);
        movie.readBuffer = ee::Ptr<MovieReadBuffer>(allocateIn(static_cast<uint32_t>(sizeof(MovieReadBuffer)), kMemoryMovie, kWriteBack));
        movie.decoderWork = allocateIn(kDecoderWorkBytes, kMemoryMovie, kWriteBack);
        const bool allHad = movie.frameData != 0 && movie.frameTags != 0 && movie.videoInput != 0 && movie.videoTags != 0 &&
                            movie.readBuffer && movie.decoderWork != 0;
        return allHad ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(allocateMovieBuffers, Verified);

    // freeMovieBuffers: the stream's buffers freed (those it has).
    void freeMovieBuffers()
    {
        MoviePlayer &movie = player();
        const auto release = [](auto &buffer) {
            if (buffer)
            {
                freeIn(buffer, kMemoryMovie);
                buffer = std::remove_reference_t<decltype(buffer)>();
            }
        };
        release(movie.frameData);
        release(movie.frameTags);
        release(movie.videoInput);
        release(movie.videoTags);
        release(movie.readBuffer);
        release(movie.decoderWork);
    }
    READABLE_FUNCTION_WAITS(freeMovieBuffers, Verified);

    // ps2MovieOpen: a movie opened: its path kept, its buffers had (freed
    // again and 0 when they are not), its file opened (tried until it opens),
    // the DMA controller set for it and the movie set up (startMovie). 1.
    uint32_t ps2MovieOpen(uint32_t path)
    {
        if (player().ready == 0)
            return 0;
        stringCopy(ee::Ptr<char>(player().path), path);
        if (allocateMovieBuffers() == 0)
        {
            freeMovieBuffers();
            return 0;
        }
        while (strFileOpen(kStreamFile, path) == 0)
        {
        }
        ee::io::store<int32_t>(sce::D_CTRL, ee::io::load<int32_t>(sce::D_CTRL) | kDmaOn);
        ee::io::store<int32_t>(sce::D_STAT, kGifDmaInterrupt);
        startMovie(path);
        player().open = 1;
        return 1;
    }

    // movieOpenWithTexture: ZGE2's ps2MovieOpen: a movie still open closed
    // first, the path kept, the stream's buffers had from the heap (aligned
    // to 64 bytes; not tested), the file opened (tried until it opens), the
    // texture made for this movie, the DMA controller set for it and the
    // movie set up (startMovie). 1; 0 before ps2MovieInit.
    uint32_t movieOpenWithTexture(uint32_t path)
    {
        constexpr uint32_t kBufferAlignment = 0x40;
        constexpr uint32_t kNotRead = 0; // (ZGE2's close and stop never read their word)
        MoviePlayer &movie = player();
        if (movie.ready == 0)
            return 0;
        if (movie.open != 0)
            ps2MovieClose(kNotRead);
        stringCopy(ee::Ptr<char>(movie.path), path);
        movie.frameData = allocateAligned(kBufferAlignment, kFrameDataBytes);
        movie.frameTags = allocateAligned(kBufferAlignment, kFrameTagBytes);
        movie.videoInput = allocateAligned(kBufferAlignment, kVideoInputBytes);
        movie.videoTags = allocateAligned(kBufferAlignment, kVideoTagBytes);
        movie.readBuffer = ee::Ptr<MovieReadBuffer>(allocateAligned(kBufferAlignment, static_cast<uint32_t>(sizeof(MovieReadBuffer))));
        movie.decoderWork = allocateAligned(kBufferAlignment, kDecoderWorkBytes);
        while (strFileOpen(kStreamFile, path) == 0)
        {
        }
        movie.texture = ee::Address(newLoadedTexture());
        ee::io::store<int32_t>(sce::D_CTRL, ee::io::load<int32_t>(sce::D_CTRL) | kDmaOn);
        ee::io::store<int32_t>(sce::D_STAT, kGifDmaInterrupt);
        startMovie(path);
        movie.open = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(ps2MovieOpen, Verified, dm2, movieOpenWithTexture);

    // ps2MovieClose: the open movie closed: stopped if it plays (the argument
    // handed on), taken down, its file closed and its buffers freed. 0 when none
    // was open.
    uint32_t ps2MovieClose(uint32_t handedOn)
    {
        if (player().ready == 0 || player().open == 0)
            return 0;
        if (player().playing == 1)
            ps2MovieStop(handedOn);
        endMovie();
        strFileClose(kStreamFile);
        freeMovieBuffers();
        player().open = 0;
        return 1;
    }

    // movieCloseWithTexture: ZGE2's ps2MovieClose: stopped if it plays, taken
    // down, the movie's texture released, its file closed and its six buffers
    // given back to the heap (their slots kept as they were). 0 when none was
    // open.
    uint32_t movieCloseWithTexture(uint32_t handedOn)
    {
        MoviePlayer &movie = player();
        if (movie.ready == 0 || movie.open == 0)
            return 0;
        if (movie.playing == 1)
            ps2MovieStop(handedOn);
        endMovie();
        releaseLoadedTexture(ee::Ptr<Texture>(movie.texture));
        strFileClose(kStreamFile);
        heapFree(movie.frameData);
        heapFree(movie.frameTags);
        heapFree(movie.videoInput);
        heapFree(movie.videoTags);
        heapFree(movie.readBuffer);
        heapFree(movie.decoderWork);
        movie.open = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(ps2MovieClose, Verified, dm2, movieCloseWithTexture);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t readBufBeginPut(ee::Ptr<MovieReadBuffer> readBuffer, ee::Ptr<uint32_t> where) { return ee::game<int32_t>(kReadBufBeginPut, readBuffer, where); }
        void readBufEndPut(ee::Ptr<MovieReadBuffer> readBuf, uint32_t bytes) { ee::game(kReadBufEndPut, readBuf, bytes); }
        int32_t readBufBeginGet(ee::Ptr<MovieReadBuffer> readBuffer, ee::Ptr<uint32_t> where) { return ee::game<int32_t>(kReadBufBeginGet, readBuffer, where); }
        uint32_t sceMpegDemuxPssRing(uint32_t videoDec, uint32_t stream, int32_t bytes, ee::Ptr<MovieReadBuffer> ring, int32_t ringBytes) { return ee::game<uint32_t>(kSceMpegDemuxPssRing, videoDec, stream, bytes, ring, ringBytes); }
        void readBufEndGet(ee::Ptr<MovieReadBuffer> readBuf, uint32_t bytes) { ee::game(kReadBufEndGet, readBuf, bytes); }
        void viBufAddDMA(uint32_t viBuf) { ee::game(kViBufAddDMA, viBuf); }
    }

    // readMain: one step of the stream (the sample's readMain), unless
    // nothing is left to demux or the movie has ended: at the top priority,
    // the audio sent on and up to 64 KB read from the file into the read
    // buffer when it has room; a turn for the other threads; then what the
    // buffer holds demuxed (sceMpegDemuxPssRing) and the video's DMA
    // restarted when it waits.
    void readMain()
    {
        if (*g_movieDemuxLeft < kLowestDemuxBytes || videoState() == static_cast<int32_t>(kMovieEnd))
            return;

        ee::Local<uint32_t> put; // where the buffer takes the next read
        ee::Local<uint32_t> get; // where it holds the next data to demux
        const auto setPriority = [](int32_t priority) { changeThreadPriority(getThreadId(), priority); };

        setPriority(kTopPriority);
        proceedAudio();
        const int32_t room = readBufBeginPut(player().readBuffer, put);
        if (*g_movieReadLeft > 0 && static_cast<int32_t>(kReadUnit - 1) < room)
        {
            const uint32_t bytesRead = strFileRead(kStreamFile, *put, kReadUnit);
            readBufEndPut(player().readBuffer, bytesRead);
            *g_movieReadLeft = *g_movieReadLeft - static_cast<int32_t>(bytesRead);
        }

        setPriority(kDefaultPriority);
        switchThread();
        setPriority(kTopPriority);

        const int32_t held = readBufBeginGet(player().readBuffer, get);
        if (held > 0)
        {
            const ee::Ptr<MovieReadBuffer> ring = player().readBuffer;
            const uint32_t demuxed = sceMpegDemuxPssRing(kVideoDec, *get, held, ring, ring->size);
            readBufEndGet(player().readBuffer, demuxed);
            *g_movieDemuxLeft = *g_movieDemuxLeft - static_cast<int32_t>(demuxed);
        }
        if (g_movieVideoDecoder->inputDmaPending != 0)
            viBufAddDMA(kVideoInputBuffer);
        setPriority(kDefaultPriority);
    }
    READABLE_FUNCTION_WAITS(readMain, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void audioDecStartThunk(ee::Ptr<AudioDec> audioDec) { ee::game(kAudioDecStart, audioDec); }
        void videoDecSetState(ee::Ptr<MovieVideoDecoder> videoDec, MovieState state) { ee::game(kVideoDecSetState, videoDec, state); }
    }

    // ps2MovieTask: the movie's task, each frame: the decoder's frame
    // (videoDecMain), with the main thread lowered meanwhile; the audio
    // started once it is ready (when the decoder says it may); a running
    // movie over 100 on its late count ended; at its end started again
    // when it loops (the clock restarted), else stopped. The decoder's
    // result.
    uint32_t ps2MovieTask(ee::Ptr<MovieVideoDecoder> decoder)
    {
        changeThreadPriority(getThreadId(), kDefaultPriority);
        const uint32_t result = stepMovie(decoder);
        if (*g_movieAudioStarted == 0 && static_cast<int32_t>(movieSoundReady()) != 0 && decoder->phase == kMoviePhaseDecoding)
        {
            if (*g_movieHasAudio != 0)
                audioDecStartThunk(kAudioDec);
            *g_movieAudioStarted = 1;
        }
        if (*g_movieAudioStarted != 0 && g_movieVideoDecoder->dataWaits >= 101)
            videoDecSetState(decoder, kMovieEnd);
        if (videoDecGetState(decoder) == static_cast<int32_t>(kMovieEnd))
        {
            if (player().loop == 1)
            {
                endMovie();
                strFileReset(kStreamFile);
                videoDecSetState(decoder, kMovieNormal);
                decoder->phase = kMoviePhaseStart;
                decoder->startTime = collisionClock();
                startMovie(0);
            }
            else
                ps2MovieStop(1);
        }
        changeThreadPriority(getThreadId(), kTopPriority);
        return result;
    }
    READABLE_FUNCTION_WAITS(ps2MovieTask, Verified);

    namespace
    {
        // The texture cache flushed for the next frame: ZGE4's player calls the
        // flush through its thunk (requestTextureCacheFlushThunk).
        [[gnu::always_inline]] inline void flushTextureCache()
        {
            if constexpr (game::readable::kIsBmx)
                requestTextureCacheFlushThunk();
            else
                requestTextureCacheFlush();
        }
    }

    // ps2MoviePlay: the open movie played (looping when asked): the game's
    // sound held off and told (a movie with audio), the task made, the
    // clock started, the texture cache flushed. 1; 0 when none is open.
    // Bug kept: when it is not playing yet it calls ps2MovieStop first,
    // which does nothing then (the test is the wrong way round).
    uint32_t ps2MoviePlay(uint32_t movie, uint32_t loop)
    {
        (void)movie;
        if (player().ready == 0 || player().open == 0)
            return 0;
        if (player().playing == 0)
            ps2MovieStop(1);
        const uint32_t hasAudio = *g_movieHasAudio;
        player().loop = loop;
        player().playing = 1;
        if (hasAudio != 0)
            holdSoundUpdates();
        const int32_t thread = getThreadId();
        changeThreadPriority(thread, kTopPriority);
        if (*g_movieHasAudio != 0)
            zaudio::statusWrites(false);
        const int32_t thread2 = getThreadId();
        changeThreadPriority(thread2, kDefaultPriority);
        player().task = newTask(kMovieTaskPriority, kPs2MovieTask, kVideoDec, 1, kSourceFile, kNewTaskLine);
        g_movieVideoDecoder->startTime = collisionClock();
        flushTextureCache();
        return 1;
    }

    // playOpenMovie: ZGE2's ps2MoviePlay, one word (the loop; DM2's moviePlay
    // hands its first on): the open movie played, the task made and the clock
    // started; the game's sound is not held off, nor the texture cache
    // flushed. 1; 0 when none is open. Bug kept as in ZGE3: when it is not
    // playing yet it calls ps2MovieStop first (handed the loop, which it does
    // not read), which does nothing then.
    uint32_t playOpenMovie(uint32_t loop)
    {
        MoviePlayer &movie = player();
        if (movie.ready == 0 || movie.open == 0)
            return 0;
        if (movie.playing == 0)
            ps2MovieStop(loop);
        movie.loop = loop;
        movie.playing = 1;
        changeThreadPriority(getThreadId(), kTopPriority);
        if (*g_movieHasAudio != 0)
            zaudio::statusWrites(false);
        changeThreadPriority(getThreadId(), kDefaultPriority);
        movie.task = newTask(kMovieTaskPriority, kPs2MovieTask, kVideoDec, 1, kSourceFile, kNewTaskLine);
        g_movieVideoDecoder->startTime = collisionClock();
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(ps2MoviePlay, Verified, dm2, playOpenMovie);

    // ps2MovieIsPlaying: 1 while a movie plays.
    int32_t ps2MovieIsPlaying()
    {
        if (player().ready == 0 || player().open == 0)
            return 0;
        const uint32_t playing = player().playing;
        return playing != 0 ? 1 : 0;
    }
    READABLE_FUNCTION(ps2MovieIsPlaying, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void videoDecAbort(uint32_t videoDec) { ee::game(kVideoDecAbort, videoDec); }
    }

    void movieEndCallback(uint32_t movie, int32_t status);

    // ps2MovieStop: the playing movie stopped: no more looping; a running
    // one aborted and its task run until it has ended (or stopped playing:
    // 1 then); the tasks stopped, the game's sound told (a movie with
    // audio), the game's end hook called (the argument handed on, which it
    // does not read) and the texture cache flushed. 1; 0 when none plays.
    uint32_t ps2MovieStop(uint32_t handedOn)
    {
        if (player().ready == 0 || player().open == 0 || player().playing == 0)
            return 0;
        player().loop = 0;
        if (videoState() != static_cast<int32_t>(kMovieEnd))
        {
            videoDecAbort(kVideoDec);
            while (videoState() != static_cast<int32_t>(kMovieEnd))
            {
                ps2MovieTask(ee::Ptr<MovieVideoDecoder>(kVideoDec));
                if constexpr (!game::readable::kIsDm2) // (ZGE2 runs the task until the movie has ended)
                {
                    if (player().playing == 0)
                        return 1;
                }
            }
        }
        changeThreadPriority(getThreadId(), kTopPriority);
        ps2MovieStopTasks();
        changeThreadPriority(getThreadId(), kDefaultPriority);
        changeThreadPriority(getThreadId(), kTopPriority);
        if (*g_movieHasAudio != 0)
            zaudio::statusWrites(true);
        changeThreadPriority(getThreadId(), kDefaultPriority);
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no end hook and leaves the texture cache)
        {
            movieEndCallback(handedOn, 0);
            flushTextureCache();
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(ps2MovieStop, Verified);

    // ps2MovieTexture: the texture the movie plays into (the player's slot),
    // its ready word set to 1.
    ee::Ptr<uint32_t> ps2MovieTexture(uint32_t movie, ee::Ptr<uint32_t> ready)
    {
        (void)movie;
        *ready = 1;
        return &player().texture;
    }
    READABLE_FUNCTION(ps2MovieTexture, Verified);

    // ps2MoviePlayerTexture: ZGE2 only: the texture the movie plays into, as the word it is (ZGE3's ps2MovieTexture
    // answers the slot and sets a ready word; ZGE2's takes nothing).
    inline uint32_t ps2MoviePlayerTexture() { return player().texture; }
    READABLE_FUNCTION_ONLY(dm2, ps2MoviePlayerTexture);

    // setMovieStreamCallback (no caller): the decoder's stream callback and its data;
    // the decoder's address.
    uint32_t setMovieStreamCallback(uint32_t callback, uint32_t data)
    {
        g_movieVideoDecoder->streamCallbackData = data;
        g_movieVideoDecoder->streamCallback = callback;
        return kVideoDec;
    }
    READABLE_FUNCTION(setMovieStreamCallback, Verified);


    // setMovieAudioVolume (no caller in play): the movie audio's volume (its low
    // byte), when it has audio. 1.
    uint32_t setMovieAudioVolume(uint32_t movie, uint32_t volume)
    {
        (void)movie;
        if (*g_movieHasAudio != 0)
            setAudioDecVolume(kAudioDec, volume & 0xFFu);
        return 1;
    }

    // setMovieVolume: ZGE2's setMovieAudioVolume, one word: the movie audio's
    // volume (its low byte), whether or not the movie has audio. 1.
    uint32_t setMovieVolume(uint32_t volume)
    {
        setAudioDecVolume(kAudioDec, volume & 0xFFu);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(setMovieAudioVolume, Verified, dm2, setMovieVolume);

    uint32_t uncachedToPhysical(uint32_t address)
    {
        return ee::physical(address);
    }
    READABLE_FUNCTION(uncachedToPhysical, Verified);

    uint32_t uncachedAddress(uint32_t address)
    {
        return ee::uncachedMirror(ee::physical(address));
    }
    READABLE_FUNCTION(uncachedAddress, Verified);
}
