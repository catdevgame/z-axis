#pragma once

// The engine's disc device's state (g_dvd), as both of its forms keep it:
// the CD/DVD client over the IOP (zge/assets/dvd_device.cpp, the PS2 form)
// and the host's (zge/host/disc_device.cpp). The host form keeps the
// objects other code uses (the lock, which the movie stream code waits on,
// the read semaphore, the completion server's thread) and the words its
// callers read (mounted, the buffer's); the rest is the client's.

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/assets/dvd_client.h.h"

namespace zge
{
    constexpr uint32_t kDvdBufferBytes = 0x8000; // the client's read-ahead buffer: 16 sectors

    // A read straight to memory, or into the buffer: the requests are
    // these words as they stand.
    struct DvdReadRequest
    {
        uint32_t file;        // +0x00
        uint32_t destination; // +0x04
        uint32_t bytes;       // +0x08
        uint32_t viaBuffer;   // +0x0C not aligned for the SIF
        uint32_t mode;        // +0x10 a DvdReadMode
        uint32_t notify;      // +0x14 the callback, or the semaphore
        uint32_t buffer;      // +0x18
    };
    static_assert(sizeof(DvdReadRequest) == 0x1C);
    struct DvdBufferRequest
    {
        uint32_t file;        // +0x00
        uint32_t position;    // +0x04
        uint32_t end;         // +0x08
        uint32_t buffer;      // +0x0C
        uint32_t mode;        // +0x10 a DvdReadMode
        uint32_t notify;      // +0x14
    };
    static_assert(sizeof(DvdBufferRequest) == 0x18);

    // The device's state (0x9B40 bytes).
    struct DvdClient
    {
        uint32_t initialised;     // +0x0000 dvdInit sets it, clearDvdBusy clears it
        uint32_t lock;            // +0x0004 the semaphore
        int32_t lockCount;        // +0x0008
        uint32_t mounted;         // +0x000C the last mount's result
        uint8_t rpc[0x24];        // +0x0010 the SIF RPC client
        uint32_t server;          // +0x0034 what the bind found
        uint8_t serverQueue[0x18]; // +0x0038
        uint8_t serverData[0x50]; // +0x0050
        uint8_t serverStack[0x1000]; // +0x00A0
        uint32_t serverThread;    // +0x10A0
        uint32_t rpcWaitSpins;    // +0x10A4
        uint8_t _10a8[0x08];      // +0x10A8
        uint8_t serverBuffer[0x890]; // +0x10B0
        int32_t reply;            // +0x1940 a call's result; a position
        int32_t fileSize;         // +0x1944
        uint8_t _1948[0x78];      // +0x1948
        char path[0xFC];          // +0x19C0 sent with its mode at the end
        uint32_t openMode;        // +0x1ABC
        uint32_t requestFile;     // +0x1AC0
        uint8_t _1ac4[0x0C];      // +0x1AC4
        uint32_t seekFile;        // +0x1AD0
        uint32_t seekOffset;      // +0x1AD4
        uint32_t seekOrigin;      // +0x1AD8
        uint32_t _1adc;           // +0x1ADC
        DvdReadRequest read;      // +0x1AE0
        uint32_t _1afc;           // +0x1AFC
        DvdBufferRequest bufferRead; // +0x1B00
        uint32_t resultOut;       // +0x1B18
        uint32_t resultCallback;  // +0x1B1C
        uint32_t callbackFile;    // +0x1B20
        uint32_t readSema;        // +0x1B24
        uint32_t readPosition;    // +0x1B28
        uint32_t readBytes;       // +0x1B2C
        uint32_t readDestination; // +0x1B30
        uint32_t bufferValid;     // +0x1B34
        uint32_t bufferFile;      // +0x1B38
        uint32_t bufferPosition;  // +0x1B3C
        uint8_t buffer[kDvdBufferBytes]; // +0x1B40
    };
    static_assert(offsetof(DvdClient, server) == 0x34 && offsetof(DvdClient, rpcWaitSpins) == 0x10A4 && offsetof(DvdClient, reply) == 0x1940 &&
                  offsetof(DvdClient, path) == 0x19C0 && offsetof(DvdClient, requestFile) == 0x1AC0 && offsetof(DvdClient, read) == 0x1AE0 &&
                  offsetof(DvdClient, resultOut) == 0x1B18 && offsetof(DvdClient, buffer) == 0x1B40 && sizeof(DvdClient) == 0x9B40);
    inline ee::Global<DvdClient, sym::g_dvd> g_dvd;

    // The completion server's thread: its stack in g_dvd, a priority above
    // the client's (dvdInit drops its caller, the game's main thread, to it).
    constexpr uint32_t kDvdServerStackBytes = 0x1000;
    constexpr uint32_t kDvdServerPriority = 1, kDvdClientPriority = 2;
}
