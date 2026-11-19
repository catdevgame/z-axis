// BMX XXX's park buffer: Aggressive Inline's game/aggressive/level/park_buffer.cpp, copied for BMX XXX
// (its game code's side of the park editor). A park's bytes on their way to or from a save
// (park_buffer.h): made, freed and read field by field.

#include "bmx/save/park_buffer.h"

#include "ee/readable.h"
#include "zge/memory/regions.h"

#include <cstdint>
#include "sym/bmx/save/park_buffer.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The buffer and its bytes are allocated on this boundary (BMX XXX; Aggressive Inline takes plain heap blocks).
        constexpr uint32_t kParkBufferAlignment = 0x20;
    }

    // A buffer of `bytes`.
    ee::Ptr<ParkBuffer> newParkBuffer(uint32_t bytes)
    {
        const ee::Ptr<ParkBuffer> buffer(zge::allocateAligned(kParkBufferAlignment, sizeof(ParkBuffer)));
        buffer->bytes = bytes;
        buffer->data = ee::Ptr<uint8_t>(zge::allocateAligned(kParkBufferAlignment, bytes));
        buffer->client = 1;
        return buffer;
    }
    READABLE_FUNCTION_WAITS(newParkBuffer, Verified);

    // The buffer and its bytes back to the heap. Returns what the heap's free
    // returns (the front end's park load hands it on).
    int32_t freeParkBuffer(ee::Ptr<ParkBuffer> buffer)
    {
        zge::heapFree(buffer->data);
        return game::tail(zge::heapFree, buffer);
    }
    READABLE_FUNCTION_WAITS(freeParkBuffer, Verified);

    void setParkBufferClient(ee::Ptr<ParkBuffer> buffer, uint32_t client)
    {
        buffer->client = client;
    }
    READABLE_FUNCTION(setParkBufferClient, Verified);

    uint32_t parkBufferSize(ee::Ptr<ParkBuffer> buffer)
    {
        return buffer->bytes;
    }
    READABLE_FUNCTION(parkBufferSize, Verified);

    ee::Address parkBufferData(ee::Ptr<ParkBuffer> buffer)
    {
        return buffer->data;
    }
    READABLE_FUNCTION(parkBufferData, Verified);
}
