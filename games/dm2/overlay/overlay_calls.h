#pragma once
// What the play overlay's meters need of a text style that zge/text/font.h
// does not name (zoverlay's items are overlay_items.h's). Called qualified
// (calls::...).

#include "ee/ee.h"
#include "zge/math/quaternion.h"
#include "zge/text/font.h"

#include <cstdint>

namespace game::dm2::calls
{
    // ZGE2's text style keeps a rotation in the sixteen bytes after the
    // position (no text code reads it; the overlay sets it to none).
    inline zge::Quaternion *textStyleRotation(zge::TextStyle *style)
    {
        return reinterpret_cast<zge::Quaternion *>(&style->position + 1);
    }
}
