#pragma once

// A level's own settings (level_globals.cpp): the scene point "Globals" and
// the tags the camera set-up reads from it.

#include "ee/ee.h"
#include "zge/math/types.h"

#include <cstdint>

namespace game::dm2
{
    // Each reads one tag of the level's "Globals" point into `out` and
    // answers whether the level has the point (nothing is written without
    // it): the scene light's colour, the clear colour, the fog colour (words
    // of red, green, blue, alpha bytes), the fog's start and the far clip.
    uint32_t globalsLightColour(ee::Ptr<uint32_t> out);
    uint32_t globalsClearColour(ee::Ptr<uint32_t> out);
    uint32_t globalsFogColour(ee::Ptr<uint32_t> out);
    uint32_t globalsFogStart(ee::Ptr<ee::Float> out);
    uint32_t globalsFarClip(ee::Ptr<ee::Float> out);
    // The level's scene point "LensFlare": its position and its colour word
    // (the lens flare effect's), and whether the level has one.
    uint32_t lensFlarePoint(ee::Ptr<zge::Vec4> position, ee::Ptr<uint32_t> colour);
}
