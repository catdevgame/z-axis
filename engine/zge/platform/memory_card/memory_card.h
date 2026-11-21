#pragma once

// The memory card: the PS2 form of the save system's jobs (save_jobs.cpp:
// the slot asked for, the card checked, the save's files written and read
// with libmc's commands), the card's "mc00" file device (file_device.cpp) and
// what a recording needs of a job's end (job_recording.cpp). The save system
// (zge/platform/save_system.h) starts the jobs by name; on the host the
// native saves run their host form instead (zge/host/save_jobs.cpp).

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace zge
{
    struct SaveClient;

    // ASCII text as icon.sys's title: big-endian Shift-JIS, 0 ended (DM2's
    // card layer calls it too). (save_jobs.cpp)
    void toSjisSaveName(ee::Ptr<const char> text, ee::Ptr<uint16_t> out);

    // (file_device.cpp) The 1 KB blocks a client's save needs on the card,
    // and the device up (the platform's device list).
    int32_t saveBlocksNeeded(ee::Ptr<SaveClient> client);
    uint32_t initMemoryCardDevice();
}
