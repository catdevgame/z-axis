// For recording only (docs/DEVTOOLS.md): a memory card job ends by deleting its
// thread, so it never returns and the recorder keeps none of it. Under
// ZX_JOB_RETURNS, with endSaveJob switched on, the end returns instead,
// so that the job's call comes to its end. Nothing follows that call in a job
// but the return, except at the auto-load job's usual end, whose code after the
// call never runs in the game and would walk the slots again. There the first
// slot is told it has no room, which sends the walk to the space check and on
// to the job's other end.

#include "ee/abi.h"
#include "ee/ee.h"

#include <cstdint>
#include <cstdlib>
#include "sym/zge/platform/memory_card/job_recording.cpp.h"

namespace zge
{
    bool saveJobReturnsForRecording()
    {
        static const bool jobReturns = std::getenv("ZX_JOB_RETURNS") != nullptr;
        if (!jobReturns)
            return false;
        constexpr uint32_t kAutoLoadFirstSlotFits = 0x20; // its frame's saveFits
        if (ee::callerRegister(ee::reg::ra) == points::autoLoadJob::afterEndSaveJob)
            *ee::at<int32_t>(static_cast<uint32_t>(ee::callerRegister(ee::reg::sp)) + kAutoLoadFirstSlotFits) = 0;
        return true;
    }
}
