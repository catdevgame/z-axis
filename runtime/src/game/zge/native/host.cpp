// The emulated clock for the PC's traces (host.h).

#include "host.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"

namespace host_clock
{
    using zge::native::hostExtrasOn;

    uint64_t vsyncTick()
    {
        const ee::Frame *frame = ee::t_ee.frame;
        return frame && frame->runtime && hostExtrasOn() ? frame->runtime->eeScheduler().currentVSyncTick() : 0u;
    }

    uint64_t eeCycle()
    {
        const ee::Frame *frame = ee::t_ee.frame;
        return frame && frame->runtime ? frame->runtime->eeScheduler().eeCycle() : 0u;
    }
}
