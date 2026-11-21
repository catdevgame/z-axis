#pragma once

// (declared for its callers in other files)

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive::skater { struct Controls; }

namespace aggressive
{
    // (pads.cpp)
    int32_t buttonLayoutIndex(uint32_t button, uint32_t layout);
    void freeCameraFrameDone();
    void initPlayerPads();
    uint32_t layoutButton(uint32_t index, uint32_t layout);
    int32_t layoutButtonHeld(ee::Ptr<skater::Controls> controls);
    ee::Float padAxis(int32_t axis, uint32_t port);
    uint32_t padDeviceBit(uint32_t bit, uint32_t port);
    uint32_t padLayoutOption(uint32_t port);
    uint32_t padVibrationOption(uint32_t port);
    int32_t resetControls(ee::Ptr<skater::Controls> controls, uint32_t port);
    uint64_t setControllerConfigLayout(uint32_t port, uint32_t layout);
    uint64_t setControllerConfigVibration(uint32_t port, uint32_t on);
    void shutdownPlayerPads();
    uint32_t updatePads();
    int32_t vibrate(ee::Ptr<skater::Controls> controls, uint32_t time, ee::Float small, ee::Float big);
}
