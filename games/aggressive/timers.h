#pragma once

// (declared for its callers in other files)

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive
{
    // (timers.cpp)
    uint32_t createTimer(uint32_t onExpire, ee::Address expireData, uint32_t onTick, ee::Address tickData, ee::Float seconds, ee::Float tickTime);
    int32_t hideVisibleTimer();
    uint32_t initTimers();
    void releaseTimer(uint32_t timer);
    int32_t setTimer(uint32_t timer, ee::Float seconds);
    void setTimerRunning(uint32_t timer, uint32_t running);
    int32_t setVisibleTimer(uint32_t timer);
    void shutdownTimers();
    int32_t stepParticleEmitters();
    void stepTimer(uint32_t timerAddress, ee::Float step);
    uint32_t timerRunning(uint32_t timer);
    ee::Float timerTime(uint32_t timer);
    uint32_t visibleTimer();
}

namespace aggressive
{
    // (timers.cpp)
    int32_t updateTimers(ee::Float seconds);
}
