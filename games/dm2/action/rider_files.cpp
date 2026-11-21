// The riders' and the bikes' data files (rider_files.h): DM2's own code
// (action.c).

#include "rider_files.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/action/rider_files.cpp.h"

namespace game::dm2
{
    namespace
    {
        // Each file's bytes (its copy on the heap: rider_files.h).
        inline ee::Global<uint32_t, sym::g_riderDataFileBytes> g_riderDataFileBytes;
        inline ee::Global<uint32_t, sym::g_bikeDataFileBytes> g_bikeDataFileBytes;
        constexpr uint32_t kRiderDataPath = sym::riderDataPath; // "assets\riders\rider.zad"
        constexpr uint32_t kBikeDataPath = sym::bikeDataPath;   // "assets\riders\bike.zad"
    }

    uint32_t riderDataFile(ee::Ptr<uint32_t> bytesOut)
    {
        if (*g_riderDataFile == 0)
        {
            const uint32_t loaded = zge::loadFile(kRiderDataPath, sym::g_riderDataFileBytes);
            if (loaded)
            {
                // The loader's block goes back; the game keeps a heap copy.
                *g_riderDataFile = zge::heapAllocate(*g_riderDataFileBytes);
                zge::memoryCopy(*g_riderDataFile, loaded, *g_riderDataFileBytes);
                zge::freeLoadedFile(loaded);
            }
        }
        if (bytesOut)
            *bytesOut = *g_riderDataFileBytes;
        return *g_riderDataFile;
    }
    READABLE_FUNCTION(riderDataFile, Verified);

    uint32_t bikeDataFile(ee::Ptr<uint32_t> bytesOut)
    {
        if (*g_bikeDataFile == 0)
        {
            const uint32_t loaded = zge::loadFile(kBikeDataPath, sym::g_bikeDataFileBytes);
            if (loaded)
            {
                *g_bikeDataFile = zge::heapAllocate(*g_bikeDataFileBytes);
                zge::memoryCopy(*g_bikeDataFile, loaded, *g_bikeDataFileBytes);
                zge::freeLoadedFile(loaded);
            }
        }
        if (bytesOut)
            *bytesOut = *g_bikeDataFileBytes;
        return *g_bikeDataFile;
    }
    READABLE_FUNCTION(bikeDataFile, Verified);
}
