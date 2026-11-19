// The level's Globals point's settings (level_globals.h). BMX XXX's own code
// (bmx.c).

#include "level_globals.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/locators.h"

#include <cstdint>
#include "sym/bmx/game/level_globals.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The level's settings point: the scene point of the kind "Globals".
        constexpr uint32_t kGlobalsKindHash = 0x0C136653; // nameHash("Globals"): not an address
        // Its tags: the ambient colour and the fog's (a word of four bytes
        // each), the fog's depth and the far clip.
        constexpr uint32_t kWordTag = 0x20, kFloatTag = 8;
        constexpr uint32_t kAmbientColourTag = 0x00, kFogColourTag = 0x08;
        constexpr uint32_t kFogDepthTag = 0x0C, kFarClipTag = 0x10;

        uint32_t globalsWord(uint32_t tag, ee::Address out)
        {
            const ee::Ptr<zge::ScenePoint> globals = zge::findScenePoint(kGlobalsKindHash, 0, 0);
            if (!globals)
                return 0;
            zge::copyTagWord(globals->tags, kWordTag, tag, out);
            return 1;
        }

        uint32_t globalsFloat(uint32_t tag, ee::Float *out)
        {
            const ee::Ptr<zge::ScenePoint> globals = zge::findScenePoint(kGlobalsKindHash, 0, 0);
            if (!globals)
                return 0;
            *out = zge::tagFloat(globals->tags, kFloatTag, tag);
            return 1;
        }
    }

    uint32_t levelAmbientColour(ee::Address colour)
    {
        return globalsWord(kAmbientColourTag, colour);
    }
    READABLE_FUNCTION(levelAmbientColour, Verified);

    uint32_t levelFogColour(ee::Address colour)
    {
        return globalsWord(kFogColourTag, colour);
    }
    READABLE_FUNCTION(levelFogColour, Verified);

    uint32_t levelFogDepth(ee::Float *depth)
    {
        return globalsFloat(kFogDepthTag, depth);
    }
    READABLE_FUNCTION(levelFogDepth, Verified);

    uint32_t levelFarClip(ee::Float *distance)
    {
        return globalsFloat(kFarClipTag, distance);
    }
    READABLE_FUNCTION(levelFarClip, Verified);
}
