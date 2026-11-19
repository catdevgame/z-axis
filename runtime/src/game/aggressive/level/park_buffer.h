// A park's bytes on their way to or from a save (park_buffer.cpp): the
// editor writes the park into one and the save menus save it; the load
// menus fill one and the editor or the front end reads the park
// back out of it.
#pragma once

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>

namespace aggressive::level
{
    struct ParkBuffer
    {
        uint32_t bytes;      // +0x00
        ee::Ptr<uint8_t> data; // +0x04 `bytes` of the heap
        // +0x08 which save client takes it (the index into g_parkSaveClient: 0
        // the park's, the only one); newParkBuffer leaves 1, which every caller
        // sets to 0 before the save job sees it.
        uint32_t client;
    };
    static_assert(offsetof(ParkBuffer, data) == 0x04 && offsetof(ParkBuffer, client) == 0x08 && sizeof(ParkBuffer) == 0x0C);
}

namespace aggressive::level
{
    // (park_buffer.cpp)
    int32_t freeParkBuffer(ee::Ptr<ParkBuffer> buffer);
    ee::Ptr<ParkBuffer> newParkBuffer(uint32_t bytes);
    ee::Address parkBufferData(ee::Ptr<ParkBuffer> buffer);
    uint32_t parkBufferSize(ee::Ptr<ParkBuffer> buffer);
    void setParkBufferClient(ee::Ptr<ParkBuffer> buffer, uint32_t client);
}
