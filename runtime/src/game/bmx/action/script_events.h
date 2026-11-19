#pragma once
// BMX XXX's level-script commands of action.c (script_events.cpp): the
// handlers the game's start registers under "SUBTITLE" and "VIBRATION".

#include "ee/ee.h"
#include "zge/scene/animation.h"

#include <cstdint>

namespace game::bmx
{
    // "SUBTITLE": the subtitle display told the command's STRINGID, unless
    // a cut scene is being run to its end at once.
    void subtitleEvent(uint32_t object, ee::Ptr<zge::AnimationEvent> event);
    // "VIBRATION": every player's pad rumbles by the command's MOTOR, TYPE
    // and AMOUNT (a percentage of the full strength), unless a cut scene is
    // being run to its end at once.
    void vibrationEvent(uint32_t object, ee::Ptr<zge::AnimationEvent> event);
}
