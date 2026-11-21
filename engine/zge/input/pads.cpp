// The engine's pads (zinput.c, initPads-padRecordingFrames; zge/input/pads.h): the
// pads the game asks for (records of a pool, each on a driver port), the
// frame's read, and the pad recorder: each frame's buttons of every pad
// written to a file as one word, or read back from one in their place
// (the recording runs at a fixed time step).

#include "pads.h"

#include "zge/assets/file.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/collision/collision.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/input/pads.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kNewPool = sym::createPool;         // (record bytes, records a block, free callback) -> a pool
        constexpr uint32_t kDestroyPool = sym::destroyPool;     // (pool): its records freed (the callback first), then it
        constexpr uint32_t kPoolOldest = sym::poolLast;      // (pool) -> the link of its oldest record in use, or 0
        constexpr uint32_t kPoolNewer = sym::poolPrevious;       // (link) -> the next newer record's link, or 0
        constexpr uint32_t kPadsHaveVibration = sym::padsHaveVibration;  // pad_driver.cpp
        constexpr uint32_t kPadMode = sym::padMode;
        constexpr uint32_t kSetPadVibration = sym::setPadVibration;
        constexpr uint32_t kPollPads = sym::pollPads;
        constexpr uint32_t kFreePad = sym::freePad;           // the pool's free callback

        constexpr uint32_t kPadsPerBlock = 2;
        constexpr uint32_t kOpenRead = 1;  // openFile's modes
        constexpr uint32_t kOpenWrite = 2;
        // %hi(g_padsReady): the recorder's address as the original keeps it
        // in a saved register (its %lo, -0x5EB0, added at each use).
        constexpr uint32_t kRecorderHigh = ee::high(sym::g_padsReady);

        // The recording's time step (g_recordingTimeStep).
        inline ee::Global<ee::Float, sym::g_recordingTimeStep> g_recordingTimeStep;

        PadRecorder &recorder() { return *g_padRecorder; }
        ee::Ptr<Pool> padPool() { return ee::Ptr<Pool>(recorder().padPool); }
    }

    // initPads: the pads' pool and the driver's ports. 1.
    uint32_t initPads()
    {
        recorder().ready = 1;
        recorder().padPool = createPool(static_cast<uint32_t>(sizeof(Pad)), kPadsPerBlock, kFreePad);
        openPadPorts();
        return 1;
    }
    READABLE_FUNCTION_WAITS(initPads, Verified);

    // shutDownPads: the pads' pool freed and the ports cleared. 1.
    uint32_t shutDownPads()
    {
        if (recorder().ready != 0)
        {
            destroyPool(padPool());
            clearPadRecords();
            recorder().ready = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownPads, Verified);

    // newPad: a pad for the game, on the next free port; 0 when the
    // driver has none.
    ee::Ptr<Pad> newPad(uint32_t requestedPort)
    {
        const ee::Ptr<Pad> pad(poolAlloc(padPool()));
        memoryFill(pad, 0, static_cast<uint32_t>(sizeof(Pad)));
        pad->port = findPadRecord(requestedPort, pad);
        if (!pad->port)
        {
            poolFree(padPool(), pad);
            return ee::Ptr<Pad>();
        }
        pad->requestedPort = requestedPort;
        return pad;
    }
    READABLE_FUNCTION_WAITS(newPad, Verified);

    // freePad: a pad freed (the pool's free callback too): its port
    // cleared, its record handed back. 1.
    uint32_t freePad(ee::Ptr<Pad> pad)
    {
        clearPadState(pad);
        poolFree(padPool(), pad);
        return 1;
    }
    READABLE_FUNCTION(freePad, Tested);

    // releasePad: a pad let go of for now (releasePadRecord). 1.
    uint32_t releasePad(ee::Ptr<Pad> pad)
    {
        releasePadRecord(pad);
        return 1;
    }
    READABLE_FUNCTION(releasePad, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        uint32_t pollPads() { return ee::tailCall<uint32_t>(kPollPads); }
        }
    }

    // readPads: the pads for the frame (a recording asked to close is
    // closed first). pollPads' 1.
    uint32_t readPads()
    {
        if (recorder().closeRequested != 0)
        {
            recorder().closeRequested = 0;
            closePadRecording();
        }
        return tail::pollPads(); // the original's tail jump
    }
    READABLE_FUNCTION_WAITS(readPads, Verified);

    // padAt (no caller): the index-th pad in use, the oldest first,
    // or 0.
    ee::Ptr<Pad> padAt(int32_t index)
    {
        uint32_t link = poolLast(&padPool()->used);
        for (int32_t left = index - 1; link != 0; --left)
        {
            if (left == -1)
                return poolRecordAs<Pad>(link);
            link = poolPrevious(ee::Ptr<PoolLink>(link));
        }
        return ee::Ptr<Pad>();
    }
    READABLE_FUNCTION(padAt, Verified);

    // padModeOf (no caller): a pad's PadMode (the pad handed on).
    uint32_t padModeOf(ee::Ptr<Pad> pad)
    {
        return padMode(pad);
    }
    READABLE_FUNCTION(padModeOf, Draft);

    namespace // the game functions below call: ee/plain.h
    {
        // The driver takes the pad too, and does not read it.
        uint32_t padsHaveVibration(ee::Ptr<Pad> pad) { return ee::game<uint32_t>(kPadsHaveVibration, pad); }
    }

    // padCanVibrate: a pad can vibrate (the driver's answer).
    uint32_t padCanVibrate(ee::Ptr<Pad> pad)
    {
        return padsHaveVibration(pad);
    }
    READABLE_FUNCTION(padCanVibrate, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t setPadVibration(ee::Ptr<Pad> pad, uint32_t smallMotor, uint32_t bigMotor, uint32_t time, uint32_t tag) { return ee::tailCall<int32_t>(kSetPadVibration, pad, smallMotor, bigMotor, time, tag); }
        }
    }

    // padVibrate: setPadVibration (the original's tail jump).
    int32_t padVibrate(ee::Ptr<Pad> pad, uint32_t smallMotor, uint32_t bigMotor, uint32_t time, uint32_t tag)
    {
        return tail::setPadVibration(pad, smallMotor, bigMotor, time, tag);
    }
    READABLE_FUNCTION(padVibrate, Verified);

    // openPadRecording (no caller in the game): a recording begun: mode 1
    // writes to the file at `path`, 2 plays it back (its first 64 KB read);
    // the time step fixed and the frames counted from 0. 0 when the file
    // does not open. Any other mode only fixes the time step.
    uint32_t openPadRecording(uint32_t path, uint32_t mode)
    {
        if (mode == kPadsRecording)
        {
            const ee::Ptr<ZFile> file = openFile(path, kOpenWrite, 0).file();
            recorder().file = file;
            if (!file)
                return 0;
            const uint32_t buffer = heapAllocate(kPadRecordingBufferBytes);
            recorder().mode = mode;
            recorder().buffer = buffer;
        }
        else if (mode == kPadsPlayback)
        {
            const ee::Ptr<ZFile> file = openFile(path, kOpenRead, 0).file();
            recorder().file = file;
            if (!file)
                return 0;
            const uint32_t buffer = heapAllocate(kPadRecordingBufferBytes);
            recorder().mode = mode;
            recorder().buffer = buffer;
            recorder().filled = static_cast<uint32_t>(
                fileRead(recorder().file, buffer, kPadRecordingBufferBytes, 0));
            recorder().position = 0;
        }
        if constexpr (!game::readable::kIsDm2) // (ZGE2 neither fixes the time step nor counts the frames)
        {
            setGlobalTimeScale(*g_recordingTimeStep);
            recorder().frames = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(openPadRecording, Verified);

    // readPadRecording: the next `bytes` of the recording played back into dst
    // (the buffer refilled from the file as it runs out; past the file's
    // end, zeros). Bug kept: each copy takes all the bytes still wanted
    // from the buffer, not only those left in it (the next ones read past
    // the data, and the position moves as far).
    void readPadRecording(ee::Address dst, uint32_t bytes)
    {
        uint32_t out = dst;
        uint32_t wanted = bytes;
        if (wanted != 0)
        {
            for (;;)
            {
                PadRecorder &state = recorder();
                const uint32_t position = state.position;
                const uint32_t left = state.filled - position;
                const uint32_t taken = left < wanted ? left : wanted;
                const uint32_t to = out;
                if (taken != 0)
                {
                    out += taken;
                    memoryCopy(to, recorder().buffer + position, wanted);
                    recorder().position = recorder().position + wanted;
                    wanted -= taken;
                }
                if (wanted == 0)
                    break;
                if (recorder().filled != kPadRecordingBufferBytes)
                {
                    if constexpr (game::readable::kIsBmx)
                    {
                        // ZGE4: the file ran out: the recording ends (the
                        // rest of dst as it was, the frame not counted).
                        closePadRecording();
                        return;
                    }
                    // The file ran out: the rest zeros.
                    memoryFill(out, 0, wanted);
                    return;
                }
                recorder().filled = static_cast<uint32_t>(
                    fileRead(recorder().file, recorder().buffer, kPadRecordingBufferBytes, 0));
                recorder().position = 0;
            }
        }
        if constexpr (!game::readable::kIsDm2) // (ZGE2 counts no frames)
            recorder().frames = recorder().frames + 1;
    }
    READABLE_FUNCTION_WAITS(readPadRecording, Verified);

    // writePadRecording: `bytes` from src appended to the recording (the buffer
    // written to the file first when they would not fit).
    void writePadRecording(ee::Address src, uint32_t bytes)
    {
        uint32_t used = recorder().position;
        if (kPadRecordingBufferBytes < used + bytes)
        {
            fileWrite(recorder().file, recorder().buffer, used, 0);
            recorder().position = 0;
            used = 0;
        }
        memoryCopy(recorder().buffer + used, src, bytes);
        if constexpr (game::readable::kIsDm2) // (ZGE2 counts no frames)
            recorder().position = recorder().position + bytes;
        else
        {
            const uint32_t frames = recorder().frames + 1;
            recorder().position = recorder().position + bytes;
            recorder().frames = frames;
        }
    }
    READABLE_FUNCTION_WAITS(writePadRecording, Verified);

    // closePadRecording: a recording ended: what is left of a written one goes to
    // the file, the file is closed and the buffer freed (the mode stays). 1.
    uint32_t closePadRecording()
    {
        const ee::Ptr<ZFile> file = recorder().file;
        if (!file)
            return 1;
        if (recorder().mode == kPadsRecording)
        {
            const uint32_t used = recorder().position;
            if (used != 0)
                fileWrite(file, recorder().buffer, used, 0);
        }
        closeFile(recorder().file, 0);
        const uint32_t buffer = recorder().buffer;
        recorder().file = {};
        heapFree(buffer);
        if constexpr (game::readable::kIsBmx) // (ZGE4 ends the mode too)
            recorder().mode = kPadsLive;
        recorder().buffer = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(closePadRecording, Verified);

    // padRecordMode: the recorder's PadRecordMode.
    uint32_t padRecordMode()
    {
        return recorder().mode;
    }
    READABLE_FUNCTION(padRecordMode, Verified);

    // padRecordingFrames (no caller): the frames written or played back.
    uint32_t padRecordingFrames()
    {
        return recorder().frames;
    }
    READABLE_FUNCTION(padRecordingFrames, Verified);
}
