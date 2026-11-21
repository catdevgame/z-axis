#pragma once

// BMX XXX's engine functions the rider calls whose BMX XXX bodies are not
// proven yet (enableContact: Planned in BMX XXX), called where they are
// through their zge declaration's signature until their bodies land, and
// the engine's limb rig as far as the rider reads it.

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/collision/dynamics.h"
#include "zge/math/types.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/rider/engine_calls.h.h"

namespace game::bmx::rider::engine
{
    inline void enableContact(ee::Ptr<zge::BodyContact> contact) { ee::game(sym::enableContact, contact); }

    // The engine's body rig of a model's limbs (zge::BodyRig, what
    // zge::newLimbRig makes: zge/scene/animate_system.h has both once the
    // engine lane's work is on the main line; this is its record as far
    // as the rider reads it): the hands' points.
    struct BodyRig
    {
        uint32_t _00;
        uint32_t system;                       // +0x04 the rig's animate system
        ee::Ptr<zge::AnimatePoint> leftHand;   // +0x08 the "LeftHand" bone's point
        ee::Ptr<zge::AnimatePoint> rightHand;  // +0x0C "RightHand"
        uint32_t _10[5];
        uint32_t stepping;                     // +0x24 0: the rig's step does nothing (the rider's resets clear it)
    };
    static_assert(offsetof(BodyRig, stepping) == 0x24);

    static_assert(offsetof(BodyRig, rightHand) == 0xC);
}
