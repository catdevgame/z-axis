#pragma once

// The park editor's camera (park_camera.cpp): it orbits a look-at point inside
// the park and slides from one view to another.

#include "zge/editor/edit_records.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge::editor
{
    struct EditCamera
    {
        uint32_t word00;      // +0x00 1 when made
        uint32_t camera;      // +0x04 the scene's camera object
        uint8_t _08[8];
        zge::Vec4 eye;        // +0x10
        ee::Ptr<const zge::Vec4> target; // +0x20 a position to look at, when set
        uint8_t _24[0xC];
        zge::Vec4 lookAt;     // +0x30
        uint8_t _40[0x10];
        ee::Float heading;    // +0x50 degrees
        ee::Float distance;   // +0x54 from the look-at point
        ee::Float tilt;       // +0x58 degrees from straight up
        uint32_t _5c;
        ee::Float turnSpeed;  // +0x60
        ee::Float zoomSpeed;  // +0x64
        ee::Float tiltSpeed;  // +0x68
        ee::Float minTilt;    // +0x6C
        ee::Float maxTilt;    // +0x70
        ee::Float minDistance; // +0x74
        ee::Float maxDistance; // +0x78
        uint32_t _7c;
        zge::Vec4 boundsHigh; // +0x80
        zge::Vec4 boundsLow;  // +0x90
        uint8_t moved;        // +0xA0
        uint8_t _a1[3];
        uint32_t inTransition; // +0xA4
        ee::Float transitionTime; // +0xA8 seconds
        ee::Float transitionElapsed; // +0xAC
        EditCameraView from;  // +0xB0
        EditCameraView to;    // +0xD0
    };
    static_assert(sizeof(EditCamera) == 0xF0 && offsetof(EditCamera, heading) == 0x50 && offsetof(EditCamera, boundsLow) == 0x90 &&
                  offsetof(EditCamera, inTransition) == 0xA4 && offsetof(EditCamera, to) == 0xD0);

    // (park_camera.cpp)
    ee::Float editCameraHeading(ee::Ptr<EditCamera> camera);
    void freeEditCamera(ee::Address camera);
    void loadEditCamera(ee::Address address, uint32_t stream);
    ee::Ptr<EditCamera> newEditCamera();
    void saveEditCamera(uint32_t address, uint32_t stream);
    void setEditCameraBounds(ee::Ptr<EditCamera> camera, ee::Ptr<const zge::Vec4> low, ee::Ptr<const zge::Vec4> high);
    int32_t setEditCameraDistance(ee::Address address, ee::Float distance);
    int32_t setEditCameraHeading(ee::Address address, ee::Float heading);
    void setEditCameraTarget(ee::Ptr<EditCamera> camera, ee::Ptr<const zge::Vec4> target);
    int32_t setEditCameraTilt(ee::Address address, ee::Float tilt);
    int32_t updateEditCamera(ee::Ptr<EditCamera> camera, ee::Float seconds);

    // (park_camera.cpp, for the games' own parts)
    int32_t clampEditCameraToBounds(ee::Address address);
    void editCameraOrbitPosition(ee::Address address);
    void resetEditCameraLimits(ee::Address address);
    void setCameraFarClip(uint32_t object, ee::Float distance);
    void updateEditCameraTransition(ee::Address address, ee::Float seconds);
}
