// The PS2 movie player's decoding side (zge/movie/ps2/pss_player.h): what
// Z-Axis changed or wrote in Sony's MPEG stream sample, which ps2Movie.c
// compiles in. The sample's own functions (audiodec, readbuf, vibuf,
// videodec, vobuf) stay the library's: the recompiled originals, listed in
// zge/not_ported.tsv (no source of the sample is on disk). Here are Z-Axis's:
// the IOP lock and the sound and DMA calls it guards, the movie file on
// zfile, the decoder's step with the picture handed to the texture, and the
// words Z-Axis added to the sample's records.

#include "ee/abi.h"
#include "ee/delay.h"
#include "ee/field.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/movie/ps2/mpeg_sample.h"
#include "zge/movie/ps2/pss_player.h"
#include "zge/platform/platform.h"
#include "zge/render/texture.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/movie/ps2/mpeg_sample.cpp.h"

namespace zge { uint32_t setGsTextureFromImage(ee::Ptr<GsTexture>, uint32_t, uint32_t, uint32_t); }

namespace zge
{
    namespace
    {
        // The kernel and the libraries these call.
        constexpr uint32_t kWaitSema = sym::waitSema;
        constexpr uint32_t kSignalSema = sym::signalSema;
        constexpr uint32_t kDisableInterrupts = sym::DIntr;
        constexpr uint32_t kEnableInterrupts = sym::EIntr;
        constexpr uint32_t kSceSifSetDma = sym::sceSifSetDma;
        constexpr uint32_t kSceSifDmaStat = sym::sceSifDmaStat;
        constexpr uint32_t kSceSifFreeIopHeap = sym::sceSifFreeIopHeap;
        constexpr uint32_t kSceSdRemote = sym::sceSdRemote;
        constexpr uint32_t kSceMpegIsEnd = sym::sceMpegIsEnd;
        constexpr uint32_t kTextAudioDecDelete = sym::g_textAudioDecDelete; // "audioDecDelete"

        // The IOP lock's words (movie.h): its semaphore, how deep it is held,
        // its holder's label and the holder before.
        inline ee::Global<int32_t, sym::g_audioDecSema> g_audioDecSema;
        inline ee::Global<int32_t, sym::g_audioDecLockDepth> g_audioDecLockDepth;
        inline ee::Global<int32_t, sym::g_audioDecLockOwner> g_audioDecLockOwner;
        inline ee::Global<int32_t, sym::g_audioDecPreviousLockOwner> g_audioDecPreviousLockOwner;
        // (g_mpegPictureReady, the stop word: pss_player.h)

        namespace routed
        {
        }
    }

    // ---- the IOP lock (movie.h) ----------------------------------------------

    // ZGE2 (DM2) keeps the lock's count and labels but waits on no semaphore:
    // its holder is marked with the label itself, and letting it go answers
    // the label it held.
    void takeIopLock(uint32_t label)
    {
        const int32_t holder = *g_audioDecLockOwner;
        [[maybe_unused]] const int32_t semaphore = *g_audioDecSema;
        ++*g_audioDecLockDepth;
        *g_audioDecLockOwner = static_cast<int32_t>(label);
        *g_audioDecPreviousLockOwner = holder;
        if constexpr (!game::readable::kIsDm2)
        {
            ee::call(kWaitSema, semaphore);
            *g_audioDecLockOwner = static_cast<int32_t>(label + 1);
        }
    }

    int32_t releaseIopLock()
    {
        const int32_t holder = *g_audioDecLockOwner;
        [[maybe_unused]] const int32_t semaphore = *g_audioDecSema;
        --*g_audioDecLockDepth;
        *g_audioDecPreviousLockOwner = holder;
        *g_audioDecLockOwner = 0;
        if constexpr (game::readable::kIsDm2)
            return holder;
        else
            return ee::call<int32_t>(kSignalSema, semaphore);
    }

    // ---- the audio's DMA to the IOP ---------------------------------------------

    namespace
    {
        constexpr uint32_t kLabelSendToIop = sym::g_textSendToIopDma; // "Send to IOP DMA"

        // sceSifDmaData: one transfer of EE memory into IOP memory.
        struct SifDmaTransfer
        {
            uint32_t source;      // +0x00 EE address
            uint32_t destination; // +0x04 IOP address
            int32_t bytes;        // +0x08
            uint32_t mode;        // +0x0C 0: none of the transfer's options
        };
        static_assert(sizeof(SifDmaTransfer) == 0x10);

        void disableInterrupts() { ee::game(kDisableInterrupts); }
        void enableInterrupts() { ee::game(kEnableInterrupts); }
        uint32_t startSifDma(ee::Ptr<SifDmaTransfer> transfers, uint32_t count) { return ee::game<uint32_t>(kSceSifSetDma, transfers, count); }
        int32_t sifDmaStatus(uint32_t id) { return ee::game<int32_t>(kSceSifDmaStat, id); } // < 0 once done
    }

    // sendToIOP: `bytes` of EE memory at source into IOP memory at
    // destination by the SIF's DMA (started with interrupts off) and waited
    // for, under the IOP lock. The bytes sent; 0 for none.
    int32_t sendToIOP(uint32_t destination, uint32_t source, int32_t bytes)
    {
        if (bytes <= 0)
            return 0;
        ee::Local<SifDmaTransfer> transfer;
        transfer->source = source;
        transfer->destination = destination;
        transfer->bytes = bytes;
        transfer->mode = 0;
        takeIopLock(kLabelSendToIop);
        disableInterrupts();
        const uint32_t id = startSifDma(transfer, 1);
        enableInterrupts();
        while (sifDmaStatus(id) >= 0)
        {
        }
        releaseIopLock();
        return bytes;
    }
    READABLE_FUNCTION_WAITS(sendToIOP, Verified);

    // ---- the movie's sound: the SPU2's sound input (Z-Axis's lock around the
    // sample's sound driver calls) ----------------------------------------------

    namespace
    {
        // The labels the IOP lock is marked with here.
        constexpr uint32_t kLabelInputVolume = sym::g_textInputVolume; // "Input Volume"
        constexpr uint32_t kLabelAudioDecDelete = kTextAudioDecDelete; // "audioDecDelete"

        // The sound driver's remote calls (sceSdRemote's first word is 1 for
        // these) and what they name: SPU2 core 1 takes the movie's sound as
        // its block-transfer input, at the input volume (BVOLL, BVOLR).
        constexpr uint32_t kSdRemoteCall = 1;
        constexpr uint32_t kSdSetParam = 0x8010;           // sceSdSetParam(param, value)
        constexpr uint32_t kSdBlockTrans = 0x80E0;         // sceSdBlockTrans(core, mode, ...)
        constexpr uint32_t kSoundCore = 1;                 // the core the movie's sound goes in by
        constexpr uint32_t kBlockTransStop = 2;            // SD_TRANS_MODE_STOP
        constexpr uint32_t kInputVolumeLeft = 0x0F81;      // SD_P_BVOLL of core 1
        constexpr uint32_t kInputVolumeRight = 0x1081;     // SD_P_BVOLR of core 1

        void setSoundParameter(uint32_t parameter, uint32_t value)
        {
            ee::game(kSceSdRemote, kSdRemoteCall, kSdSetParam, parameter, value);
        }
        // The transfer stopped; where it was (its IOP address in the low 24 bits).
        uint32_t stopBlockTransfer(uint32_t core)
        {
            return ee::game<uint32_t>(kSceSdRemote, kSdRemoteCall, kSdBlockTrans, core, kBlockTransStop, 0, 0);
        }
        // sceSdVoiceTrans(channel, mode, IOP address, SPU2 address, bytes): IOP memory written into the SPU2's.
        constexpr uint32_t kSdVoiceTrans = 0x80D0;
        constexpr uint32_t kVoiceTransChannel = 1, kVoiceTransWrite = 0;
        void writeSpuMemory(uint32_t iopAddress, uint32_t spuAddress, uint32_t bytes)
        {
            ee::game(kSceSdRemote, kSdRemoteCall, kSdVoiceTrans, kVoiceTransChannel, kVoiceTransWrite, iopAddress, spuAddress, bytes);
        }
        constexpr uint32_t kIopAddressMask = (1u << 24) - 1; // sceSdBlockTrans's position: the address in its low 24 bits
        // What a pause clears in SPU2 memory from the IOP's zero buffer.
        constexpr uint32_t kClearedSpuAddress = 0x4000, kClearedSpuBytes = 0x800;
        constexpr uint32_t kLabelAudioDecPause = sym::g_textAudioDecPause; // "audioDecPause"

        // (ZGE2 frees through sceSifFreeSysMemory itself.)
        constexpr uint32_t kFreeIopMemory = game::readable::perGame(kSceSifFreeIopHeap, uint32_t{sym::sceSifFreeSysMemory}, kSceSifFreeIopHeap);
        void freeIopMemory(uint32_t iopAddress) { ee::game(kFreeIopMemory, iopAddress); }
    }


    // changeInputVolume: the movie sound's volume (0 to 0x3FFF) on both sides
    // of the SPU2's sound input, under the IOP lock. The kernel's result of
    // letting the lock go.
    int32_t changeInputVolume(uint32_t volume)
    {
        takeIopLock(kLabelInputVolume);
        setSoundParameter(kInputVolumeLeft, volume);
        setSoundParameter(kInputVolumeRight, volume);
        return releaseIopLock();
    }
    READABLE_FUNCTION_WAITS(changeInputVolume, Verified);


    // setAudioDecVolume: the movie sound's volume, 0 to 255, as the SPU2's input
    // volume (63 times that; the original leaves the raw value in the second
    // argument, which changeInputVolume does not read). The decoder in the first
    // argument goes unread. Not Verified: the host's movies replace it in a
    // default run.
    int32_t setAudioDecVolume(ee::Ptr<AudioDec>, uint32_t volume)
    {
        const uint32_t level = volume & 0xFFu;
        return game::tail(changeInputVolume, (level << 6) - level);
    }
    READABLE_FUNCTION_WAITS(setAudioDecVolume, Verified);

    // audioDecPause: the movie's sound paused (state 3). For a movie with
    // sound the input volume goes to 0, then under the IOP lock the block
    // transfer into the SPU2 stops, where it stopped is kept (as an offset in
    // the IOP's sound ring, for audioDecStart to go on from), and 0x800 bytes
    // of SPU2 memory are cleared from the IOP's zero buffer. The kernel's
    // result of letting the lock go; 3 for a movie without sound.
    int32_t audioDecPause(ee::Ptr<AudioDec> audioDec)
    {
        audioDec->state = kAudioDecPaused;
        if (audioDec->streamKind != static_cast<int32_t>(kStreamPss))
            return kAudioDecPaused;
        if constexpr (game::readable::kIsDm2) // (ZGE2 takes the lock first: the volume's own lock nests in it)
        {
            takeIopLock(kLabelAudioDecPause);
            changeInputVolume(0);
        }
        else
        {
            changeInputVolume(0);
            takeIopLock(kLabelAudioDecPause);
        }
        const uint32_t stoppedAt = stopBlockTransfer(kSoundCore);
        audioDec->iopPausePosition = static_cast<int32_t>(stoppedAt & kIopAddressMask) - audioDec->iopBuffer;
        writeSpuMemory(audioDec->iopZeroBuffer, kClearedSpuAddress, kClearedSpuBytes);
        return releaseIopLock();
    }
    READABLE_FUNCTION_WAITS(audioDecPause, Verified);

    // audioDecDelete: the movie's sound taken down: under the IOP lock its
    // transfer into the SPU2 stopped and its two buffers in IOP memory (the
    // sound ring and the silence) freed; then the input volume to 0. 1.
    uint32_t audioDecDelete(ee::Ptr<zge::AudioDec> audioDec)
    {
        takeIopLock(kLabelAudioDecDelete);
        stopBlockTransfer(kSoundCore);
        freeIopMemory(audioDec->iopBuffer);
        freeIopMemory(audioDec->iopZeroBuffer);
        releaseIopLock();
        changeInputVolume(0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(audioDecDelete, Verified);

    namespace
    {
        constexpr uint32_t kLabelSendToIop1 = sym::g_textSendToIOP1; // "SendToIOP1"
        constexpr uint32_t kSdBlockTransStatus = 0x8100; // sceSdBlockTransStatus(core): where the block transfer is
        constexpr int32_t kSendUnit = 1024;              // the sound goes to the IOP in whole KB, at least one

        uint32_t blockTransferStatus(uint32_t core)
        {
            return ee::game<uint32_t>(kSceSdRemote, kSdRemoteCall, kSdBlockTransStatus, core);
        }

        // Where in the IOP's sound ring the next bytes may go: up to two
        // areas, the second from the ring's start (the sample's pd0, d0,
        // pd1, d1).
        struct IopAreas
        {
            int32_t area0;
            int32_t bytes0;
            int32_t area1;
            int32_t bytes1;
        };
        static_assert(sizeof(IopAreas) == 0x10);

        // The library's (audiodec.c): the free areas of the IOP's ring behind
        // the SPU2's play position (an offset into the ring).
        void iopGetArea(ee::Ptr<IopAreas> areas, ee::Ptr<AudioDec> audioDec, int32_t playPosition)
        {
            ee::game(sym::iopGetArea, ee::field(areas, &IopAreas::area0), ee::field(areas, &IopAreas::bytes0),
                     ee::field(areas, &IopAreas::area1), ee::field(areas, &IopAreas::bytes1), audioDec, playPosition);
        }
        // The library's (audiodec.c): two pieces of EE memory into the two
        // areas, by sendToIOP; the bytes sent.
        int32_t sendToIOP2area(int32_t area0, int32_t bytes0, int32_t area1, int32_t bytes1, uint32_t source0,
                               int32_t sourceBytes0, uint32_t source1, int32_t sourceBytes1)
        {
            return ee::game<int32_t>(sym::sendToIOP2area, area0, bytes0, area1, bytes1, source0, sourceBytes0, source1,
                                     sourceBytes1);
        }

        // A remainder as the EE's division leaves it: C++'s, but the dividend
        // for a divisor of 0 and 0 for INT32_MIN by -1, which C++ leaves
        // undefined. (The original traps a divisor of 0 with BREAK first,
        // which the runtime only reports; the sound's ring sizes are never 0.)
        int32_t remainderOf(int32_t dividend, int32_t divisor)
        {
            if (divisor == 0)
                return dividend;
            if (divisor == -1)
                return 0;
            return dividend % divisor;
        }
    }

    // audioDecSendToIOP: the movie's sound, as much as is waiting in the EE's
    // ring, sent on to the IOP's ring, in whole KB and only with at least 1 KB
    // to send and 1 KB of room. While the stream's header is in and the
    // sound has not started (preset), the IOP's ring is filled from where
    // the sends got to; while it plays, its free areas are the ones behind
    // the SPU2's play position, asked for under the IOP lock (Z-Axis's,
    // label "SendToIOP1"). Idle or paused, nothing goes. The bytes sent;
    // what was sent leaves the EE's ring and moves the IOP's last position
    // on. proceedAudio's, on the stream's thread; only ZX_MOVIES=hle
    // reaches it.
    int32_t audioDecSendToIOP(ee::Ptr<AudioDec> audioDec)
    {
        ee::Local<IopAreas> areas;
        switch (audioDec->state)
        {
        case kAudioDecIdle:
        case kAudioDecPaused:
            return 0;
        case kAudioDecPreset:
            areas->area0 = audioDec->iopBuffer + remainderOf(audioDec->totalBytesSent, audioDec->iopBufferSize);
            areas->bytes0 = audioDec->iopBufferSize - audioDec->totalBytesSent;
            areas->area1 = 0;
            areas->bytes1 = 0;
            break;
        case kAudioDecPlaying:
        {
            takeIopLock(kLabelSendToIop1);
            const uint32_t transferAt = blockTransferStatus(kSoundCore);
            const int32_t playPosition = static_cast<int32_t>(transferAt & kIopAddressMask) - audioDec->iopBuffer;
            releaseIopLock();
            iopGetArea(areas, audioDec, playPosition);
            break;
        }
        default:
            // (No other state is ever set; the original goes on with the
            // areas its frame held, as the Local does.)
            break;
        }

        const int32_t waiting = audioDec->count;
        const int32_t readPosition = remainderOf(audioDec->putOffset - waiting + audioDec->size, audioDec->size);
        const int32_t toSend = waiting / kSendUnit * kSendUnit;
        const int32_t toRingEnd = audioDec->size - readPosition;
        const int32_t sendBeforeWrap = toSend < toRingEnd ? toSend : toRingEnd;
        const int32_t sendAfterWrap = toSend - sendBeforeWrap;
        int32_t sent = 0;
        if (areas->bytes0 + areas->bytes1 >= kSendUnit && toSend >= kSendUnit)
            sent = sendToIOP2area(areas->area0, areas->bytes0, areas->area1, areas->bytes1, audioDec->data + readPosition,
                                  sendBeforeWrap, audioDec->data, sendAfterWrap);
        audioDec->count -= sent;
        audioDec->totalBytesSent += sent;
        audioDec->iopLastPosition = remainderOf(audioDec->iopLastPosition + sent, audioDec->iopBufferSize);
        return sent;
    }
    READABLE_FUNCTION_WAITS(audioDecSendToIOP, Verified);


    // ---- the movie's file: the sample's strfile.c, which Z-Axis wrote on zfile
    // (the movie's file opened, sought and read like any other, under the IOP
    // lock) -----------------------------------------------------------------------

    namespace
    {
        constexpr uint32_t kLabelFileOpen = sym::g_textFileOpen;   // "File Open"
        constexpr uint32_t kLabelFileReset = sym::g_textFileReset; // "File reset"
        constexpr uint32_t kLabelFileRead = sym::g_textFileRead;   // "File read"

        // The DVD device's own lock (dvd_device.cpp's dvdReadError hands out its
        // semaphore): a read waits until the disc is free, then lets it go at
        // once, before zfile takes it again for the read.
        void waitForDisc()
        {
            const int32_t discLock = static_cast<int32_t>(zge::dvdReadError());
            ee::call(kWaitSema, discLock);
            ee::call(kSignalSema, discLock);
        }
    }

    // strFileOpen: the movie's file opened for reading (no callback: at once)
    // and its size taken, under the IOP lock. The file, or 0 when it did not
    // open (ps2MovieOpen tries again until it does).
    ee::Ptr<ZFile> strFileOpen(ee::Ptr<MovieStreamFile> stream, uint32_t path)
    {
        ee::Local<uint32_t> size;
        takeIopLock(kLabelFileOpen);
        stream->file = openFile(path, kFileModeRead, 0).file();
        fileSize(stream->file, size.address(), 0);
        stream->size = *size;
        releaseIopLock();
        return stream->file;
    }
    READABLE_FUNCTION_WAITS(strFileOpen, Verified);

    // strFileReset: the movie's file back to its start (a looping movie), under
    // the IOP lock. 1.
    uint32_t strFileReset(ee::Ptr<MovieStreamFile> stream)
    {
        takeIopLock(kLabelFileReset);
        fileSeek(stream->file, 0, kSeekFromStart, 0);
        releaseIopLock();
        return 1;
    }
    READABLE_FUNCTION_WAITS(strFileReset, Verified);

    // strFileClose: the movie's file closed. 1.
    uint32_t strFileClose(ee::Ptr<MovieStreamFile> stream)
    {
        closeFile(stream->file, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(strFileClose, Verified);

    // strFileRead: up to `bytes` of the movie's file into dst from where it
    // is, once the disc is free, under the IOP lock. The bytes read.
    int32_t strFileRead(ee::Ptr<MovieStreamFile> stream, uint32_t dst, uint32_t bytes)
    {
        takeIopLock(kLabelFileRead);
        waitForDisc();
        const int32_t bytesRead = fileRead(stream->file, dst, bytes, 0);
        releaseIopLock();
        return bytesRead;
    }
    READABLE_FUNCTION_WAITS(strFileRead, Verified);

    // videoDecReset: the decoder as a movie starts: normal (state 0), stepMovie
    // at its first phase, and Z-Axis's words: no waits for data, the first picture
    // wanted, none to upload. 1 (the value it set needPicture with).
    uint32_t videoDecReset(ee::Ptr<MovieVideoDecoder> decoder)
    {
        decoder->dataWaits = 0;
        decoder->needPicture = 1;
        decoder->state = kMovieNormal;
        decoder->phase = kMoviePhaseStart;
        decoder->uploadPending = 0;
        return 1;
    }
    READABLE_FUNCTION(videoDecReset, Verified);

    namespace
    {
        // The sample's other parts (vobuf.c: the ring of decoded pictures at
        // voBuf; libmpeg), which stay the library's.
        constexpr uint32_t kVoBuf = sym::voBuf;
        uint32_t voBufGetData(uint32_t voBuf) { return ee::game<uint32_t>(sym::voBufGetData, voBuf); }      // a free slot, 0: none
        int32_t voBufDecCount(uint32_t voBuf) { return ee::game<int32_t>(sym::voBufDecCount, voBuf); }      // a slot let go; the slots still counted in
        // videodec.c's videoDecGetState (the library's): MovieVideoDecoder::state.
        uint32_t videoDecGetState(ee::Ptr<MovieVideoDecoder> decoder) { return ee::game<uint32_t>(sym::videoDecGetState, decoder); }
        void voBufIncCount(uint32_t voBuf) { ee::game(sym::voBufIncCount, voBuf); }                         // the slot taken
        int32_t sceMpegIsEnd(ee::Ptr<MovieVideoDecoder> mpeg) { return ee::game<int32_t>(kSceMpegIsEnd, mpeg); }
        // (decoder's sceMpeg, an RGBA picture's slot, its size in macroblocks) -> < 0: a decode error
        int32_t sceMpegGetPicture(ee::Ptr<MovieVideoDecoder> mpeg, uint32_t slot, uint32_t macroblocks)
        {
            return ee::game<int32_t>(sym::sceMpegGetPicture2, mpeg, slot, macroblocks);
        }
        // The sample's error message: printing it is the hook movieNoop here.
        constexpr uint32_t kTextDecodeError = sym::g_textDecodeError; // "sceMpegGetPicture() decode error"
        // ZGE2 still prints the sample's two messages as the decoding ends (printf); ZGE3 and ZGE4 say nothing.
        constexpr uint32_t kTextDecodeThreadAborted = sym::dm2::g_textDecodeThreadAborted; // "decode thread: aborted\n"
        constexpr uint32_t kTextDecodeThreadEnd = sym::dm2::g_textDecodeThreadEnd;         // "decode thread: end\n"
        void print(uint32_t text) { ee::game(sym::printf, text); }
        constexpr uint32_t kPictureMacroblocks = (512 / 16) * (256 / 16); // a 512x256 picture
        constexpr int32_t kUploadDelayRounds = 10000; // outputMovieFrame's wait before the upload
    }

    // outputMovieFrame: the movie's decoded picture into its texture. A
    // picture just decoded (the decoder no longer wants one) is marked to
    // go; a marked one waits first (an empty loop of 10,000 rounds: EE time
    // only, which the game's threads and timers see), then is copied into
    // the player's texture (setGsTextureFromImage, the sceMpeg picture
    // size), handed to the stream callback if one is set, and its ring slot
    // let go (voBufDecCount); the decoder then wants the next. Always 1 (ZGE2: the slots counted in, voBufDecCount's). The host's movie step calls it for each picture it
    // shows.
    uint32_t outputMovieFrame(ee::Ptr<MovieVideoDecoder> decoder)
    {
        if (decoder->uploadPending == 0 && decoder->needPicture == 0)
            decoder->uploadPending = 1;
        if (decoder->uploadPending != 1)
            return 1;
        ee::Local<int32_t> delayRound; // the loop's counter, a volatile stack word in the original
        if constexpr (!game::readable::kIsDm2) // (ZGE2 uploads at once)
            ee::delayLoop(delayRound, kUploadDelayRounds);
        zge::setGsTextureFromImage(ee::field(decoder->texture, &Texture::gs), decoder->pixels, decoder->width, decoder->height);
        if (decoder->streamCallback)
            decoder->streamCallback(decoder, decoder->streamCallbackData);
        if constexpr (game::readable::kIsDm2) // (ZGE2 returns voBufDecCount's count)
        {
            const int32_t slotsLeft = voBufDecCount(kVoBuf);
            decoder->uploadPending = 0;
            decoder->needPicture = 1;
            return static_cast<uint32_t>(slotsLeft);
        }
        voBufDecCount(kVoBuf);
        decoder->uploadPending = 0;
        decoder->needPicture = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(outputMovieFrame, Verified);

    // decodeMoviePicture: the movie's picture step, stepMovie's each frame
    // (the sample's picture loop, with Z-Axis's hand-over to the texture):
    //  - at the stream's end the last picture still goes to the texture, and
    //    the movie has ended (-1); aborted (videoDecAbort), it ends at once (-1);
    //  - while the texture has not taken the picture before (needPicture
    //    clear), or no slot of the picture ring is free, that picture goes on
    //    to the texture instead (1);
    //  - else the next picture is decoded into a free slot (the scratchpad,
    //    which libmpeg uses, kept around it), and, unless a stop word is set
    //    (g_mpegPictureReady: stepMovie clears it as a movie starts and no
    //    code in the game sets it; set, the movie ends: -1), the slot is
    //    counted in and the picture waits for the texture (1). A decode error
    //    only gives the sample's message.
    // The other threads of the decoder's priority run before it returns 1.
    int32_t decodeMoviePicture(ee::Ptr<MovieVideoDecoder> decoder)
    {
        if (sceMpegIsEnd(decoder) != 0)
        {
            outputMovieFrame(decoder);
            if constexpr (game::readable::kIsDm2)
                print(kTextDecodeThreadEnd);
            return -1;
        }
        if (videoDecGetState(decoder) == kMovieAbort)
        {
            if constexpr (game::readable::kIsDm2)
                print(kTextDecodeThreadAborted);
            return -1;
        }
        if (decoder->needPicture != 1)
        {
            outputMovieFrame(decoder);
            switchThread();
            return 1;
        }
        const uint32_t slot = voBufGetData(kVoBuf);
        if (slot == 0)
        {
            outputMovieFrame(decoder);
            return 1;
        }
        decoder->pixels = slot;
        // (ZGE2 does not keep the scratchpad around the decode.)
        if constexpr (!game::readable::kIsDm2)
            zge::saveScratchpad();
        const int32_t decoded = sceMpegGetPicture(decoder, slot, kPictureMacroblocks);
        if constexpr (!game::readable::kIsDm2)
            zge::restoreScratchpad();
        if (*g_mpegPictureReady != 0)
            return -1;
        if (decoded < 0)
            movieNoop(); // (the original hands it kTextDecodeError, which it does not read)
        decoder->uploadPending = 0;
        decoder->needPicture = 0;
        voBufIncCount(kVoBuf);
        switchThread();
        return 1;
    }
    READABLE_FUNCTION_WAITS(decodeMoviePicture, Verified);

    namespace
    {
        // vibuf.c's viBufAddDMA (the library's): the decoder's input DMA started
        // again from what the ring holds.
        void viBufAddDMA(ee::Ptr<uint8_t> viBuf) { ee::game(sym::viBufAddDMA, viBuf); }
    }

    // mpegNodata: libmpeg's callback when the decoder has no data (the
    // sample's, which Z-Axis made count): the other threads run (the stream's
    // reader among them), the movie's decoder counts one more wait for data,
    // and its input DMA is started again. 1: go on.
    uint32_t mpegNodata()
    {
        switchThread();
        const ee::Ptr<MovieVideoDecoder> decoder(kMovieVideoDecoderAddress);
        decoder->dataWaits = decoder->dataWaits + 1;
        viBufAddDMA(ee::field(decoder, &MovieVideoDecoder::inputBuffer));
        return 1;
    }
    READABLE_FUNCTION_WAITS(mpegNodata, Draft);

    // ---- Z-Axis's parts of videodec.c and audiodec.c the runtime hooks --------

    namespace
    {
        // The library's (vibuf.c, vobuf.c, videodec.c; libmpeg).
        void viBufReset(ee::Ptr<uint8_t> viBuf) { ee::game(sym::viBufReset, viBuf); }
        void voBufReset(uint32_t voBuf) { ee::game(sym::voBufReset, voBuf); }
        void sceMpegReset(ee::Ptr<MovieVideoDecoder> mpeg) { ee::game(sym::sceMpegReset, mpeg); }
        void videoDecSetState(ee::Ptr<MovieVideoDecoder> decoder, MovieState state) { ee::game(sym::videoDecSetState, decoder, state); }
        constexpr int32_t kSettleTurns = 8; // the other threads' turns before the first picture
        int32_t voBufCount(uint32_t voBuf) { return ee::Ptr<VoBuf>(voBuf)->count; }
    }

    // stepMovie: the decoder's work for one frame (the sample's videoDecMain,
    // which Z-Axis made a step of the movie's task). Its first step sets up the
    // input and picture rings and gives the other threads eight turns (the
    // stream's reader fills the input meanwhile); from then on a picture a
    // step (decodeMoviePicture), until that reports the end. Once ended, the
    // pictures still in the ring go to the texture, libmpeg is reset and the
    // decoder's state says the movie has ended. Always 1. Its host form:
    // zge/host/movie_stream.cpp.
    uint32_t stepMovie(ee::Ptr<MovieVideoDecoder> decoder)
    {
        if (decoder->phase == kMoviePhaseStart)
        {
            viBufReset(ee::field(decoder, &MovieVideoDecoder::inputBuffer));
            voBufReset(kVoBuf);
            *g_mpegPictureReady = 0;
            decoder->phase = kMoviePhaseDecoding;
            for (int32_t turn = 0; turn < kSettleTurns; ++turn)
                switchThread();
        }
        else if (decoder->phase == kMoviePhaseEnded)
        {
            while (voBufCount(kVoBuf) > 0)
                outputMovieFrame(decoder);
            sceMpegReset(decoder);
            videoDecSetState(decoder, kMovieEnd);
            return 1;
        }
        else if (decoder->phase != kMoviePhaseDecoding)
            return 1;
        if (decodeMoviePicture(decoder) != 1)
            decoder->phase = kMoviePhaseEnded;
        return 1;
    }
    READABLE_FUNCTION_WAITS(stepMovie, Verified);

    namespace
    {
        constexpr uint32_t kSdBlockTransLoopFrom = 0x13; // SD_BLOCK_LOOP | SD_TRANS_MODE_WRITE_FROM
        constexpr uint32_t kFullVolume = 0x7FFF;
        constexpr int32_t kBlockBytes = 1024;            // the transfer's ring in whole KB
        void startBlockTransfer(uint32_t core, uint32_t ring, uint32_t ringBytes, uint32_t from)
        {
            ee::game(kSceSdRemote, kSdRemoteCall, kSdBlockTrans, core, kSdBlockTransLoopFrom, ring, ringBytes, from);
        }
    }

    // audioDecStart (the sample's audioDecResume, with Z-Axis's stream kind):
    // for a movie with sound the input volume full and the SPU2's block
    // transfer started looping over the IOP's sound ring (in whole KB) from
    // where the pause left it; then state 2 (playing). 2. Its host form:
    // zge/host/movie_stream.cpp.
    int32_t audioDecStart(ee::Ptr<AudioDec> audioDec)
    {
        if (audioDec->streamKind == static_cast<int32_t>(kStreamPss))
        {
            changeInputVolume(kFullVolume);
            const int32_t ringBytes = audioDec->iopBufferSize / kBlockBytes * kBlockBytes;
            startBlockTransfer(kSoundCore, audioDec->iopBuffer, ringBytes, audioDec->iopBuffer + audioDec->iopPausePosition);
        }
        audioDec->state = kAudioDecPlaying;
        return kAudioDecPlaying;
    }
    READABLE_FUNCTION_WAITS(audioDecStart, Verified);

    namespace
    {
        // The library's (videodec.c, copy2area in the sample's main file).
        struct RingAreas // where videoDecBeginPut lets the next bytes go: up to two areas
        {
            uint32_t area0;
            int32_t bytes0;
            uint32_t area1;
            int32_t bytes1;
        };
        static_assert(sizeof(RingAreas) == 0x10);
        void videoDecBeginPut(ee::Ptr<MovieVideoDecoder> decoder, ee::Ptr<RingAreas> areas)
        {
            ee::game(sym::videoDecBeginPut, decoder, ee::field(areas, &RingAreas::area0), ee::field(areas, &RingAreas::bytes0),
                     ee::field(areas, &RingAreas::area1), ee::field(areas, &RingAreas::bytes1));
        }
        // (two areas to fill, two to copy from) -> the bytes copied
        int32_t copyToAreas(uint32_t area0, int32_t bytes0, uint32_t area1, int32_t bytes1, uint32_t source0,
                            uint32_t sourceBytes0, uint32_t source1, uint32_t sourceBytes1)
        {
            return ee::game<int32_t>(sym::copy2area, area0, bytes0, area1, bytes1, source0, sourceBytes0, source1, sourceBytes1);
        }
        int32_t videoDecPutTs(ee::Ptr<MovieVideoDecoder> decoder, uint64_t pts, uint64_t dts, uint32_t where, int32_t bytes)
        {
            return ee::game<int32_t>(sym::videoDecPutTs, decoder, pts, dts, where, bytes);
        }
        void videoDecEndPut(ee::Ptr<MovieVideoDecoder> decoder, int32_t bytes) { ee::game(sym::videoDecEndPut, decoder, bytes); }
        constexpr uint32_t kTextPtsOverflow = sym::g_textPtsOverflow; // "pts buffer overflow"
    }

    // videoCallback: the demuxer's video packet (in the read ring, wrapping at
    // its end) copied into the decoder's input ring, as much as it has room
    // for, with its time stamps; the decoder no longer waits for data
    // (dataWaits 0, Z-Axis's). The sample's message if the time stamps do
    // not fit. 1 if anything was copied.
    int32_t videoCallback(uint32_t /*mpeg*/, ee::Ptr<MpegStreamData> packet, uint32_t readRing)
    {
        const ee::Ptr<MovieVideoDecoder> decoder(kMovieVideoDecoderAddress);
        const uint32_t data = packet->data;
        const uint32_t bytes = packet->bytes;
        const uint32_t ringEnd = readRing + static_cast<uint32_t>(ee::Ptr<MovieReadBuffer>(readRing)->size);
        decoder->dataWaits = 0;
        const uint32_t beforeWrap = bytes < ringEnd - data ? bytes : ringEnd - data;
        const uint32_t afterWrap = bytes - beforeWrap;
        ee::Local<RingAreas> areas;
        videoDecBeginPut(decoder, areas);
        const uint32_t area0 = uncachedAddress(areas->area0);
        const uint32_t area1 = uncachedAddress(areas->area1);
        const int32_t copied = copyToAreas(area0, areas->bytes0, area1, areas->bytes1, data, beforeWrap, readRing, afterWrap);
        if (copied > 0 && videoDecPutTs(decoder, packet->pts, packet->dts, areas->area0, copied) == 0)
            movieNoop(); // (the original hands it kTextPtsOverflow, which it does not read)
        videoDecEndPut(decoder, copied);
        return copied > 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(videoCallback, Verified);
}
