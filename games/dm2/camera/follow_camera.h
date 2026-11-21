#pragma once

// DM2's follow camera (camera.c; follow_camera.cpp): every frame, where the
// camera of the player being shown wants to be and to look by its modes, its
// move there, and the camera state the camera node is given.

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace game::dm2
{
    // How a camera moves, one row for each thing it can be doing: where it
    // stands in the rider's space, and how fast it gets to where it wants
    // to be.
    struct FollowRates
    {
        ee::Float pull;            // +0x00 the speed gained toward the wanted place, for each unit it is away
        ee::Float drag;            // +0x04 the share of its speed lost each frame
        ee::Float positionEase;    // +0x08 the share of the way the eased place goes to the wanted one each frame
        ee::Float targetEase;      // +0x0C the same for the point looked at
        ee::Float fieldOfViewEase; // +0x10 the same for the field of view
        ee::Float fieldOfView;     // +0x14 the field of view wanted, in degrees
    };
    static_assert(sizeof(FollowRates) == 0x18);

    struct CameraSettings
    {
        zge::Vec4 offset;  // +0x00 the camera's place from the rider: right, down, forward
        FollowRates rates; // +0x10
        uint8_t _28[0x30 - 0x28];
    };
    static_assert(offsetof(CameraSettings, rates) == 0x10 && sizeof(CameraSettings) == 0x30);

    // The follow camera's frame: the shown player's camera worked out and
    // applied to the camera node.
    void followCamera();
}
