// The trees' fade by distance (tree_fade.h). DM2's own code (fx.c).

#include "tree_fade.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/render/draw.h"
#include "zge/render/material.h"
#include "zge/render/terrain.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/fx/tree_fade.cpp.h"

namespace game::dm2
{
    namespace
    {
        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;
        // The two levels whose trees fade.
        constexpr uint8_t kLevelZero = 0, kLevelSix = 6;

        // The fade's record: 9 and 13 units, "terrain_tree_alpher".
        constexpr uint32_t kTreeFade = sym::treeFade;

        // A material's alpha when it is drawn as it is: the fade's top.
        constexpr ee::Float kFullAlpha = 128.0f;
        constexpr int32_t kSolidAlpha = 0x80;
        // Below this alpha a material is blended with the alpha as its fixed
        // value; from it on blending is off.
        constexpr int32_t kBlendedBelow = 0x7F;

        // Node::drawFlags, ZGE2: the bucket the node is drawn in whatever its
        // class says, plus 1 (0: its class's bucket). ZGE3 keeps it four bits lower.
        constexpr uint32_t kDrawBucketOverride = 0xF000u;
    }

    uint32_t fadeTreeByDistance(ee::Ptr<zge::Node> node, ee::Address, ee::Ptr<TreeFade> fade)
    {
        const ee::Float hiddenWithin = fade->hiddenWithin;
        // How far ahead of the camera the node is (ZGE2's node keeps one view's centre).
        ee::Float distance = node->views[0].viewCentre.z;
        if (distance < hiddenWithin)
            return 0;
        const ee::Float solidFrom = fade->solidFrom;
        if (solidFrom < distance)
            distance = solidFrom;
        const ee::Float alphaPerUnit = kFullAlpha / (solidFrom - hiddenWithin);
        const int32_t alpha = ee::toInt((distance - hiddenWithin) * alphaPerUnit);

        const ee::Ptr<zge::MaterialTable> materials = node->materials;
        for (uint32_t i = 0; i < materials->count; ++i)
        {
            const ee::Ptr<zge::Material> material = materials->materials[i];
            if (alpha < kBlendedBelow)
            {
                zge::setMaterialState(material, 0, zge::kStateSourceColourGequal2, 0);
                zge::setMaterialState(material, 0, zge::kStateAlphaFix, static_cast<uint32_t>(alpha));
            }
            else
                zge::setMaterialState(material, 0, zge::kStateBlending, 0);
        }
        // A solid tree goes back to its class's bucket; a fading one is drawn in bucket 6.
        if (alpha == kSolidAlpha)
            node->drawFlags &= ~kDrawBucketOverride;
        else
            zge::setNodeBucket6(node);
        return 1;
    }
    READABLE_FUNCTION_WAITS(fadeTreeByDistance, Verified);

    uint32_t setTreeFadeCheck(ee::Ptr<zge::Node> node, ee::Ptr<TreeFade> fade)
    {
        const ee::Ptr<char> name = zge::nodeName(node);
        if (zge::compareStringsNoCaseN(name, ee::Address(fade->nodeName), fade->nodeNameLength) == 0)
            zge::setNodeDrawCheck(node, sym::fadeTreeByDistance, fade);
        return 1;
    }
    READABLE_FUNCTION_WAITS(setTreeFadeCheck, Verified);

    void setupTreeFade()
    {
        const uint8_t level = *g_sessionLevel;
        if (level != kLevelZero && level != kLevelSix)
            return;
        game::tail(zge::forEachWorldMesh, sym::setTreeFadeCheck, ee::Address(kTreeFade));
    }
    READABLE_FUNCTION_WAITS(setupTreeFade, Verified);
}
