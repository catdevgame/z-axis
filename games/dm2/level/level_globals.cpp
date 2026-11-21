// A level's own settings (level_globals.h): DM2's own code (its game block).
// Aggressive Inline reads the same point inside its camera set-up
// (games/aggressive/camera.cpp, with ZGE3's tag layout); DM2 has one small
// function a tag, which its camera set-up calls.

#include "level_globals.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/scene/locators.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/level/level_globals.cpp.h"

namespace game::dm2
{
    namespace
    {
        inline ee::Global<char, sym::g_globalsName> g_globalsName;     // "Globals"
        inline ee::Global<char, sym::g_lensFlareName> g_lensFlareName; // "LensFlare"

        // The point's tag groups and the fields by offset: the colours
        // (words) and the floats.
        constexpr uint32_t kGlobalsColours = 0x20, kGlobalsFloats = 8;
        constexpr uint32_t kGlobalsLightColour = 0, kGlobalsClearColour = 4, kGlobalsFogColour = 8;
        constexpr uint32_t kGlobalsFogStart = 0xC, kGlobalsFarClip = 0x10;

        // The level's scene point of this name, or 0 without one (DM2's
        // findScenePoint takes the name's hash as the kind).
        ee::Ptr<zge::ScenePoint> namedPoint(const char *name)
        {
            return zge::findScenePoint(zge::nameHash(name), 0, 0);
        }
        ee::Ptr<zge::ScenePoint> globalsPoint()
        {
            return namedPoint(g_globalsName.get());
        }

        uint32_t globalsColour(uint32_t offset, ee::Ptr<uint32_t> out)
        {
            const ee::Ptr<zge::ScenePoint> globals = globalsPoint();
            if (!globals)
                return 0;
            zge::copyTagWord(globals->tags, kGlobalsColours, offset, out);
            return 1;
        }

        uint32_t globalsFloat(uint32_t offset, ee::Ptr<ee::Float> out)
        {
            const ee::Ptr<zge::ScenePoint> globals = globalsPoint();
            if (!globals)
                return 0;
            *out = zge::tagFloat(globals->tags, kGlobalsFloats, offset);
            return 1;
        }
    }

    uint32_t globalsLightColour(ee::Ptr<uint32_t> out)
    {
        return globalsColour(kGlobalsLightColour, out);
    }
    READABLE_FUNCTION(globalsLightColour, Verified);

    uint32_t globalsClearColour(ee::Ptr<uint32_t> out)
    {
        return globalsColour(kGlobalsClearColour, out);
    }
    READABLE_FUNCTION(globalsClearColour, Verified);

    uint32_t globalsFogColour(ee::Ptr<uint32_t> out)
    {
        return globalsColour(kGlobalsFogColour, out);
    }
    READABLE_FUNCTION(globalsFogColour, Verified);

    uint32_t globalsFogStart(ee::Ptr<ee::Float> out)
    {
        return globalsFloat(kGlobalsFogStart, out);
    }
    READABLE_FUNCTION(globalsFogStart, Verified);

    uint32_t globalsFarClip(ee::Ptr<ee::Float> out)
    {
        return globalsFloat(kGlobalsFarClip, out);
    }
    READABLE_FUNCTION(globalsFarClip, Verified);

    // lensFlarePoint: the "LensFlare" point's colour word (colours group,
    // offset 0) and position into `colour` and `position`; 1 with the point,
    // else 0 and nothing written.
    uint32_t lensFlarePoint(ee::Ptr<zge::Vec4> position, ee::Ptr<uint32_t> colour)
    {
        const ee::Ptr<zge::ScenePoint> point = namedPoint(g_lensFlareName.get());
        if (!point)
            return 0;
        zge::copyTagWord(point->tags, kGlobalsColours, kGlobalsLightColour, colour);
        *position = point->position;
        return 1;
    }
    READABLE_FUNCTION(lensFlarePoint, Verified);
}
