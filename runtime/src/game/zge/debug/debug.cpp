// Debug log and assertions.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/platform/platform.h"
#include "sce/libgcc.h"
#include <cstring>
#include <cstddef>
#include <cstdint>
#include "sym/zge/debug/debug.cpp.h"

namespace zge
{

    namespace
    {
        inline ee::Global<int32_t, sym::g_debugLogPath> g_debugLogPath;
        inline ee::Global<int32_t, sym::g_debugLogFile> g_debugLogFile;
        constexpr uint32_t kEngineStateHigh = ee::high(sym::g_debugEnabled);
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
    }

    namespace
    {
        namespace routed
        {
        }
    }

    // openDebugLog (nothing calls it in AI): the log file opened for
    // writing, when a log path is set (g_debugLogPath).
    void openDebugLog()
    {
        const uint32_t path = static_cast<uint32_t>(*g_debugLogPath);
        if (path == 0)
            return;
        *g_debugLogFile = static_cast<int32_t>(openFile(path, kFileModeWrite, 0));
    }
    READABLE_FUNCTION_WAITS(openDebugLog, Draft);

    // debugChecksum (nothing calls it in AI): the bytes' sum (unsigned), 0
    // for a count of 0 or less.
    uint32_t debugChecksum(ee::Ptr<uint8_t> bytes, int32_t count)
    {
        uint32_t sum = 0;
        for (; count > 0; --count, ++bytes)
            sum += *bytes;
        return sum;
    }
    READABLE_FUNCTION(debugChecksum, Draft);

    // Zdebug: the log file (g_debugLogFile) closed.
    uint32_t closeDebugLog()
    {
        const int32_t debugLogFile = *g_debugLogFile;
        if (debugLogFile != 0)
            return game::tail(zge::closeFile, ee::Ptr<ZFile>(static_cast<uint32_t>(debugLogFile)), 0);
        return kEngineStateHigh;
    }
    READABLE_FUNCTION_WAITS(closeDebugLog, Draft);

    int32_t debugAssertFailed(uint32_t why, uint32_t file, uint32_t line, uint32_t function)
    {
        return game::tail(platformAssertFailed, why, file, line, function);
    }
    READABLE_FUNCTION_WAITS(debugAssertFailed, Draft);

}
