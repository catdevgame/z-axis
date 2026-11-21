// DM2's park editor: the view. The player turns, tilts and zooms the
// editor's camera; the editor's record keeps what was asked for.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"

#include <cstdint>

namespace game::dm2
{
    // The view's pitch, heading and distance moved by `change` (degrees,
    // degrees, world units): the pitch kept within 5 .. 85, the heading
    // wrapped into -180 .. 180, the distance kept within 15 .. 100.
    void tiltEditView(ee::Ptr<ParkEditor> editor, ee::Float change);
    void turnEditView(ee::Ptr<ParkEditor> editor, ee::Float change);
    void zoomEditView(ee::Ptr<ParkEditor> editor, ee::Float change);

    // The view set up for a park: the menus' scene camera (made if there is
    // none) with the park's draw distance, looking at the cursor's cell
    // from 40 units at a pitch of 60 degrees.
    void setUpEditView(ee::Ptr<ParkEditor> editor);
    // The view's eye and target cleared (`keepCamera` not 0), or its camera
    // released.
    void endEditView(ee::Ptr<ParkEditor> editor, uint32_t keepCamera);
    // The view's frame: the looked-at cell eases towards the cursor's (the
    // selection's corner while it follows the cursor, the middle of the
    // selection with the terrain tool), the eye is put at the asked
    // distance, pitch and heading from it and kept over the park and a
    // cell around it, and the camera looks from the eye at the target.
    void updateEditView(ee::Ptr<ParkEditor> editor, ee::Float seconds);
}
