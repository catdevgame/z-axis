// The engine's random numbers and its math set-up.

#include "random.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include <cstring>
#include <cstddef>
#include <cstdint>
#include "sym/zge/math/random.cpp.h"

namespace zge
{

    namespace
    {
        inline ee::Global<int32_t, sym::g_randomSeed> g_randomSeed; // g_randomSeed; its name is a low-confidence guess (readable_names.tsv)
    }

    uint32_t initMath()
    {
        initMathUtilities();
        return 1;
    }
    READABLE_FUNCTION(initMath, Verified);

    uint32_t shutdownMath()
    {
        shutDownMathUtilities();
        return 1;
    }
    READABLE_FUNCTION(shutdownMath, Verified);

    uint32_t randomNumber()
    {
        const int32_t randomSeed = *g_randomSeed;
        const int32_t mixed = (randomSeed << 4 | static_cast<uint32_t>(randomSeed) >> 28) + (static_cast<uint32_t>(randomSeed) >> 0x10 | randomSeed << 16) ^ static_cast<int32_t>(0x87654321u);
        const int32_t randomSeed2 = mixed << 16 | static_cast<uint32_t>(mixed) >> 0x10;
        *g_randomSeed = randomSeed2;
        return randomSeed2;
    }
    READABLE_FUNCTION(randomNumber, Verified);
}
