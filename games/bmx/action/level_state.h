#pragma once
// BMX XXX's level game state (action.c, level_state.cpp): the parts of its
// enter, loading and exit written so far.

#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // The action task's frame (the level's loading makes the task): it does
    // nothing. Always 1 (the task goes on).
    uint32_t actionTaskFrame();
    // The level's loading asks every rider through forEachRider whether its
    // model is ready: a rider whose model is not clears `allReady`. Always 1
    // (the walk goes on).
    uint32_t checkRiderModelReady(uint32_t riderIndex, ee::Ptr<uint32_t> allReady);
    // The level state's enter: the replay a watching session plays made
    // and its level taken, the loading's first step. Always 1.
    int32_t levelStateEnter();
    // The level state's loading, a step a frame: 0 once the level is in
    // play (or when a step fails), else 1.
    int32_t levelStateLoad();
    // The level state's exit: everything the level's loading made freed, in
    // turn, and the sound's mode put back to 0.
    int32_t levelStateExit();
    // The exit's empty hook (inferred name). Always 1.
    int32_t emptyQuitHook();
}
