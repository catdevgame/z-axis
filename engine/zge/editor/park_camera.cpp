// The park editor's camera: it orbits a look-at point (a selection's, when it
// has a target) at a heading, a tilt from the vertical and a distance, kept
// inside the park's corners, and slides from one view to another.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/editor/edit_records.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "zge/editor/park_camera.h"
#include "zge/editor/park_cells.h"
#include "zge/editor/park_classes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/editor/park_camera.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kLookFromAt = sym::lookFromAt;         // (node, eye *, look-at *, 0)

        constexpr ee::Float kFarClip = 1024.0f;
        constexpr ee::Float kDegreesToAngle = 182.04445f;     // 65536 / 360
        constexpr ee::Float kInverse360 = 0.0027777778f;

        inline ee::Global<uint32_t, sym::g_editCameraObject> g_uiCamera;

        // Degrees as the engine's sign-extended 16-bit angle.
        int32_t angleOf(ee::Float degrees, ee::Float scale)
        {
            return static_cast<int16_t>(ee::toInt(degrees * scale));
        }
    }

    void setCameraFarClip(uint32_t object, ee::Float distance);

    void resetEditCameraLimits(ee::Address address);

    // newEditCamera: over the scene's camera, far clip 1024, heading 90, tilt 80,
    // 20 units away.
    ee::Ptr<EditCamera> newEditCamera()
    {
        const ee::Ptr<EditCamera> camera(editAllocate(static_cast<uint32_t>(sizeof(EditCamera))));
        camera->inTransition = 0;
        camera->word00 = 1;
        const uint32_t uiCamera = *g_uiCamera;
        camera->camera = uiCamera;
        setCameraFarClip(uiCamera, kFarClip);
        camera->distance = ee::Float(20.0f);
        camera->tilt = ee::Float(80.0f);
        camera->heading = ee::Float(90.0f);
        resetEditCameraLimits(camera);
        return camera;
    }
    READABLE_FUNCTION_WAITS(newEditCamera, Verified);

    void freeEditCamera(ee::Address camera)
    {
        game::tail(editFree, camera);
    }
    READABLE_FUNCTION_WAITS(freeEditCamera, Verified);

    // The left stick's y.
    int32_t tiltEditCamera(uint32_t address, ee::Float rate)
    {
        const ee::Ptr<EditCamera> camera(address);
        return game::tail(setEditCameraTilt, address, camera->tilt + rate * camera->tiltSpeed);
    }
    READABLE_FUNCTION(tiltEditCamera, Verified);

    int32_t turnEditCamera(uint32_t address, ee::Float rate)
    {
        const ee::Ptr<EditCamera> camera(address);
        return game::tail(setEditCameraHeading, address, camera->heading + rate * camera->turnSpeed);
    }
    READABLE_FUNCTION(turnEditCamera, Verified);

    // The right stick's y.
    int32_t zoomEditCamera(uint32_t address, ee::Float rate)
    {
        const ee::Ptr<EditCamera> camera(address);
        return game::tail(setEditCameraDistance, address, camera->distance + rate * camera->zoomSpeed);
    }
    READABLE_FUNCTION(zoomEditCamera, Verified);

    // setEditCameraHeading: the heading's turns taken off, truncated: it stays between
    // -360 and 360.
    int32_t setEditCameraHeading(ee::Address address, ee::Float heading)
    {
        const ee::Ptr<EditCamera> camera(address);
        camera->moved = 1;
        camera->heading = heading - ee::toFloat(ee::toInt(heading * kInverse360)) * ee::Float(360.0f);
        return 1;
    }
    READABLE_FUNCTION(setEditCameraHeading, Verified);

    // setEditCameraTilt: the tilt, kept to its limits.
    int32_t setEditCameraTilt(ee::Address address, ee::Float tilt)
    {
        const ee::Ptr<EditCamera> camera(address);
        ee::Float limit = camera->minTilt;
        camera->tilt = tilt;
        int32_t moved = 0;
        bool beyond = true;
        if (!(tilt < limit))
        {
            limit = camera->maxTilt;
            moved = 1;
            beyond = limit < tilt;
        }
        if (beyond)
        {
            camera->tilt = limit;
            moved = 1;
        }
        camera->moved = static_cast<uint8_t>(moved);
        return moved;
    }
    READABLE_FUNCTION(setEditCameraTilt, Verified);

    int32_t setEditCameraDistance(ee::Address address, ee::Float distance)
    {
        const ee::Ptr<EditCamera> camera(address);
        ee::Float limit = camera->minDistance;
        camera->distance = distance;
        int32_t moved = 0;
        bool beyond = true;
        if (!(distance < limit))
        {
            limit = camera->maxDistance;
            moved = 1;
            beyond = limit < distance;
        }
        if (beyond)
        {
            camera->distance = limit;
            moved = 1;
        }
        camera->moved = static_cast<uint8_t>(moved);
        return moved;
    }
    READABLE_FUNCTION(setEditCameraDistance, Verified);

    // editCameraOrbitPosition: the eye on its sphere about the look-at point.
    void editCameraOrbitPosition(ee::Address address)
    {
        const ee::Ptr<EditCamera> camera(address);
        camera->eye = camera->lookAt;
        const ee::Float scale = kDegreesToAngle;
        const ee::Float sinTilt = zge::angleSin(angleOf(camera->tilt, scale));
        const ee::Float sinHeading = zge::angleSin(angleOf(camera->heading, scale));
        const int32_t tiltForCos = angleOf(camera->tilt, scale);
        camera->eye.x = camera->eye.x + camera->distance * sinTilt * sinHeading;
        const ee::Float cosTilt = zge::angleCos(static_cast<zge::Angle>(tiltForCos));
        const int32_t tiltAgain = angleOf(camera->tilt, scale);
        camera->eye.y = camera->eye.y - camera->distance * cosTilt;
        const ee::Float sinTiltAgain = zge::angleSin(tiltAgain);
        const ee::Float cosHeading = zge::angleCos(static_cast<zge::Angle>(angleOf(camera->heading, scale)));
        camera->eye.z = camera->eye.z + camera->distance * sinTiltAgain * cosHeading;
    }
    READABLE_FUNCTION(editCameraOrbitPosition, Verified);

    void setEditCameraBounds(ee::Ptr<EditCamera> camera, ee::Ptr<const zge::Vec4> low, ee::Ptr<const zge::Vec4> high)
    {
        camera->boundsHigh = *high;
        camera->boundsLow = *low;
    }
    READABLE_FUNCTION(setEditCameraBounds, Verified);

    // clampEditCameraToBounds: an eye outside the park's corners brought back, the distance
    // made true again. 1 when it moved.
    int32_t clampEditCameraToBounds(ee::Address address)
    {
        const ee::Ptr<EditCamera> camera(address);
        int32_t result = 0;
        const ee::Float x = camera->eye.x;
        ee::Float limit = camera->boundsLow.x;
        bool beyond = true;
        if (!(x < limit))
        {
            limit = camera->boundsHigh.x;
            beyond = limit < x;
        }
        if (beyond)
        {
            camera->eye.x = limit;
            result = 1;
        }
        const ee::Float z = camera->eye.z;
        const ee::Float lowZ = camera->boundsLow.z;
        beyond = true;
        if (z < lowZ)
            camera->eye.z = lowZ;
        else if (camera->boundsHigh.z < z)
            camera->eye.z = camera->boundsHigh.z;
        else
            beyond = false;
        if (beyond)
            result = 1;
        const ee::Float y = camera->eye.y;
        const ee::Float lowY = camera->boundsLow.y;
        beyond = true;
        if (y < lowY)
            camera->eye.y = lowY;
        else if (camera->boundsHigh.y < y)
            camera->eye.y = camera->boundsHigh.y;
        else
            beyond = false;
        if (beyond)
            result = 1;
        if (result != 0)
        {
            const ee::Float dx = camera->eye.x - camera->lookAt.x;
            const ee::Float dy = camera->eye.y - camera->lookAt.y;
            const ee::Float dz = camera->eye.z - camera->lookAt.z;
            camera->distance = ee::sqrt(dx * dx + dy * dy + dz * dz);
        }
        return result;
    }
    READABLE_FUNCTION(clampEditCameraToBounds, Verified);

    // updateEditCameraTransition: the look-at point and the angles part way from one view to
    // the other.
    void updateEditCameraTransition(ee::Address address, ee::Float seconds)
    {
        const ee::Ptr<EditCamera> camera(address);
        const ee::Float time = camera->transitionTime;
        const ee::Float elapsed = camera->transitionElapsed + seconds;
        camera->transitionElapsed = elapsed;
        if (time <= elapsed)
        {
            camera->inTransition = 0;
            camera->transitionElapsed = time;
        }
        const ee::Float share = camera->transitionElapsed / time;
        const EditCameraView &from = camera->from, &to = camera->to;
        camera->lookAt.x = from.lookAt.x + (to.lookAt.x - from.lookAt.x) * share;
        camera->lookAt.y = from.lookAt.y + (to.lookAt.y - from.lookAt.y) * share;
        camera->lookAt.z = from.lookAt.z + (to.lookAt.z - from.lookAt.z) * share;
        camera->heading = from.heading + (to.heading - from.heading) * share;
        camera->tilt = from.tilt + (to.tilt - from.tilt) * share;
        camera->distance = from.distance + (to.distance - from.distance) * share;
    }
    READABLE_FUNCTION(updateEditCameraTransition, Verified);

    namespace
    {
        namespace tail
        {
        int32_t lookFromAt(uint32_t object, ee::Ptr<const zge::Vec4> eye, ee::Ptr<const zge::Vec4> lookAt, uint32_t value) { return ee::tailCall<int32_t>(kLookFromAt, object, eye, lookAt, value); }
        }
    }

    // updateEditCamera: the frame's camera: in a slide, the slide; else on the
    // target; the eye; inside the park unless sliding; the scene's camera set.
    int32_t updateEditCamera(ee::Ptr<EditCamera> camera, ee::Float seconds)
    {
        if (camera->inTransition != 0)
            updateEditCameraTransition(camera, seconds);
        else if (camera->target)
            camera->lookAt = zge::quadwordOf(camera->target);
        editCameraOrbitPosition(camera);
        if (camera->inTransition == 0)
            clampEditCameraToBounds(camera);
        return tail::lookFromAt(camera->camera, &camera->eye, &camera->lookAt, 0);
    }
    READABLE_FUNCTION_WAITS(updateEditCamera, Verified);

    void resetEditCameraLimits(ee::Address address)
    {
        const ee::Ptr<EditCamera> camera(address);
        camera->turnSpeed = ee::Float(2.0f);
        camera->zoomSpeed = ee::Float(1.0f);
        camera->maxTilt = ee::Float(85.0f);
        camera->minDistance = ee::Float(5.0f);
        camera->maxDistance = ee::Float(35.0f);
        camera->lookAt.z = ee::Float(0.0f);
        camera->tiltSpeed = ee::Float(1.0f);
        camera->minTilt = ee::Float(5.0f);
        camera->lookAt.x = ee::Float(0.0f);
        camera->lookAt.y = ee::Float(0.0f);
    }
    READABLE_FUNCTION(resetEditCameraLimits, Verified);

    void setEditCameraTarget(ee::Ptr<EditCamera> camera, ee::Ptr<const zge::Vec4> target)
    {
        camera->target = target;
    }
    READABLE_FUNCTION(setEditCameraTarget, Verified);

    ee::Float editCameraHeading(ee::Ptr<EditCamera> camera)
    {
        return camera->heading;
    }
    READABLE_FUNCTION(editCameraHeading, Verified);

    void saveEditCamera(uint32_t address, uint32_t stream)
    {
        parkStreamWrite(stream, address + static_cast<uint32_t>(offsetof(EditCamera, distance)), 4);
        parkStreamWrite(stream, address + static_cast<uint32_t>(offsetof(EditCamera, heading)), 4);
        game::tail(parkStreamWrite, stream, address + static_cast<uint32_t>(offsetof(EditCamera, tilt)), 4);
    }
    READABLE_FUNCTION(saveEditCamera, Verified);

    void loadEditCamera(ee::Address address, uint32_t stream)
    {
        parkStreamRead(stream, address + static_cast<uint32_t>(offsetof(EditCamera, distance)), 4);
        parkStreamRead(stream, address + static_cast<uint32_t>(offsetof(EditCamera, heading)), 4);
        game::tail(parkStreamRead, stream, address + static_cast<uint32_t>(offsetof(EditCamera, tilt)), 4);
    }
    READABLE_FUNCTION(loadEditCamera, Verified);

    namespace
    {
        // A camera object's record: its camera.
        ee::Ptr<zge::Camera> cameraOf(uint32_t object) { return zge::sceneOf(ee::Ptr<zge::SceneObject>(object)).cast<zge::Camera>(); }
    }

    void setCameraFarClip(uint32_t object, ee::Float distance)
    {
        game::tail(zge::setCameraFarPlane, cameraOf(object), distance);
    }
    READABLE_FUNCTION(setCameraFarClip, Verified);
}
