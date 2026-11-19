// A park's bytes on their way to or from a save (park_buffer.h):
// made, freed and read and written field by field.

#include "aggressive/level/park_buffer.h"

#include "ee/readable.h"
#include "zge/memory/regions.h"

#include <cstdint>
#include "sym/aggressive/level/park_buffer.cpp.h"

namespace aggressive::level
{
    // A buffer of `bytes`.
    ee::Ptr<ParkBuffer> newParkBuffer(uint32_t bytes)
    {
        const ee::Ptr<ParkBuffer> buffer(zge::heapAllocate(sizeof(ParkBuffer)));
        buffer->bytes = bytes;
        buffer->data = ee::Ptr<uint8_t>(zge::heapAllocate(bytes));
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

    // Nothing calls it.
    void setParkBufferSize(ee::Ptr<ParkBuffer> buffer, uint32_t bytes)
    {
        buffer->bytes = bytes;
    }
    READABLE_FUNCTION(setParkBufferSize, Draft);

    // Nothing calls it.
    void setParkBufferData(ee::Ptr<ParkBuffer> buffer, ee::Ptr<uint8_t> data)
    {
        buffer->data = data;
    }
    READABLE_FUNCTION(setParkBufferData, Draft);

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

    // Nothing calls it.
    uint32_t parkBufferClient(ee::Ptr<ParkBuffer> buffer)
    {
        return buffer->client;
    }
    READABLE_FUNCTION(parkBufferClient, Draft);
}
