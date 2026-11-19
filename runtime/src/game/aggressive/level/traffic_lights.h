#pragma once

// The traffic lights, AI's side (traffic_lights.cpp; traffic_light_state.h
// has the rest).

#include "aggressive/level/traffic_light_state.h"

#include <cstdint>

namespace aggressive
{
    // (traffic_lights.cpp)
    uint32_t lightState(uint32_t segment);
    int32_t setSegmentJunctionLights(uint32_t segment, LightState state);
}
