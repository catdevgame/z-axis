#pragma once
// What a pane of glass and an exploder keep in a saved level state
// (glass.cpp, exploders.cpp): whether it is broken, where it stands and how
// it is turned.

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    struct BreakableSave
    {
        uint32_t broken;              // +0x00
        uint8_t _04[0x0C];
        zge::Vec4 position;           // +0x10
        zge::Quaternion orientation;  // +0x20
    };
    static_assert(offsetof(BreakableSave, position) == 0x10 && sizeof(BreakableSave) == 0x30);

    // The activate message's data as an exploder reads it (0x20 bytes, its
    // senders clear them): its first word set, nothing happens; else it
    // explodes, its debris flying with the velocity at +0x10 (a touch's).
    constexpr uint32_t kActivateMessage = 0x100;
    struct ActivationData
    {
        uint32_t quiet;      // +0x00
        uint8_t _04[0x0C];
        zge::Vec4 velocity;  // +0x10
    };
    static_assert(offsetof(ActivationData, velocity) == 0x10 && sizeof(ActivationData) == 0x20);
}
