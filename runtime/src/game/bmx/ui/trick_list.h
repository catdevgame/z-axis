#pragma once
// BMX XXX's trick list (bmx.c; trick_list.cpp): the current player's tricks
// of a category in the "FrameTrickList" frame's tree view, each grey until
// landed, with the selected one's buttons. The trick list screen opens and
// closes it; the tree view's script functions update it. DM2's
// bmxtrickSetUpdateCategory and bmxtrickSetGetTrick (game/dm2/overlay/
// screen_trick_list.cpp) are its lineage: BMX XXX's take the frame, and
// read what was landed from the saved game.

#include "ee/ee.h"
#include "zge/zgui/widget.h"

#include <cstdint>

namespace game::bmx
{
    // The frame's Init played, then the tree view filled.
    void openTrickList(ee::Ptr<zge::zgui::Node> frame);
    // The tree view's items freed and its RootNode cleared, then the frame's
    // Shutdown played.
    void closeTrickList(ee::Ptr<zge::zgui::Node> frame);
    // The selected line's buttons shown under the list (nothing past the
    // last line).
    void updateTrickListTrick(ee::Ptr<zge::zgui::Node> frame);
    // The tree view filled for its category: its title, then a line for
    // each trick of it.
    void updateTrickListCategory(ee::Ptr<zge::zgui::Node> frame);
}
