#pragma once
// BMX XXX's splash (fx.c; splash.cpp): the "splash" effect class's draw.
// The class spawns and moves its drops as the sparks do (particles.cpp);
// its start is startSplashEffect (spawns.cpp).

#include "particles.h"

#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // The splash class's draw: each drop a grey square facing the camera,
    // as light as its alpha; returns how many it drew.
    int32_t drawSplash(uint32_t effect, ee::Ptr<Emitter> emitter);
}
