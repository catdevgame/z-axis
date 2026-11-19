// The park editor's camera: it orbits a look-at point (a selection's, when it
// has a target) at a heading, a tilt from the vertical and a distance, kept
// inside the park's corners, and slides from one view to another.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_camera.cpp.

#include "ee/abi.h"
#include "zge/editor/park_camera.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "aggressive/level/edit_records.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "aggressive/level/park_camera.h"
#include "aggressive/level/park_cells.h"
#include "aggressive/level/park_classes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/level/park_camera.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kAngleSin = sym::angleSin;           // (likewise)
        constexpr uint32_t kLookFromAt = sym::lookFromAt;         // (node, eye *, look-at *, 0)
        constexpr uint32_t kSetEditCameraHeading = sym::setEditCameraHeading;
        constexpr uint32_t kSetEditCameraTilt = sym::setEditCameraTilt;
        constexpr uint32_t kSetEditCameraDistance = sym::setEditCameraDistance;
        constexpr uint32_t kEditCameraOrbitPosition = sym::editCameraOrbitPosition;
        constexpr uint32_t kClampEditCameraToBounds = sym::clampEditCameraToBounds;
        constexpr uint32_t kUpdateEditCameraTransition = sym::updateEditCameraTransition;
        constexpr uint32_t kResetEditCameraLimits = sym::resetEditCameraLimits;
        constexpr uint32_t kSetCameraFarClip = sym::setCameraFarClip;

        // The camera's view now: where it looks and how.
        EditCameraView viewNow(const EditCamera &camera)
        {
            return EditCameraView{camera.lookAt, camera.heading, camera.tilt, camera.distance};
        }
    }

    void setCameraFarClip(uint32_t object, ee::Float distance);

    void resetEditCameraLimits(ee::Address address);

    uint32_t editCameraSceneObject(uint32_t camera)
    {
        return ee::Ptr<EditCamera>(camera)->camera;
    }
    READABLE_FUNCTION(editCameraSceneObject, Draft);

    // editCameraView: the view now, for a slide back to it.
    void editCameraView(ee::Ptr<const EditCamera> camera, ee::Ptr<EditCameraView> view)
    {
        *view = viewNow(*camera);
    }
    READABLE_FUNCTION(editCameraView, Verified);

    // startEditCameraTransition: a slide to a view: from the view given, or, breaking off one
    // under way, from where it got to. The result is what the original's last copy leaves
    // in its result register, the target view's heading and tilt as one doubleword: the
    // tools that start a slide end with it, so they hand it on.
    uint64_t startEditCameraTransition(ee::Ptr<EditCamera> camera, ee::Ptr<const EditCameraView> fromView, ee::Ptr<const EditCameraView> toView, ee::Float seconds)
    {
        if (camera->inTransition != 0)
            camera->from = viewNow(*camera);
        else
            camera->from = *fromView;
        camera->transitionTime = seconds;
        camera->inTransition = 1;
        camera->transitionElapsed = ee::Float(0.0f);
        camera->to = *toView;
        uint64_t headingAndTilt;
        std::memcpy(&headingAndTilt, &toView->heading, sizeof(headingAndTilt));
        return headingAndTilt;
    }
    READABLE_FUNCTION(startEditCameraTransition, Verified);
}
