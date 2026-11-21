// The class renderers submitNodeDraw picks by render type (draw.h): each
// makes the node's draw record, says what the class draws it with (the
// lights, the view, the grass shells' depth) and hands it to the encoder:
// on the PS2 (zge/render/ps2/class_draws.h) a DMA call of the part's
// prebuilt VIF packet and the node's matrices for VU1, in the view's mesh
// packet. On the native draw path they still run
// (game_overrides/aggressive_inline_native.cpp wraps them and replaces the
// VU1 runs of the packets they send); on the strict path their packets are
// what VU1 draws. With render_path = records the host's form takes the
// record first (zge/host/class_draws.h: drawn natively, no VU1 work in the
// packet) and leaves to the encoder only what it does not draw.

#include "draw.h"
#include "zge/render/renderer.h"
#include "zge/render/ps2/class_draws.h"
#include "zge/host/class_draws.h"

#include "ee/readable.h"
#include "zge/scene/light.h"
#include "zge/scene/camera.h"
#include "sym/zge/render/class_renderers.cpp.h"

namespace zge
{
    namespace
    {
        // The scene's lights (a node lit by no light block of its own).
        inline ee::Global<LightBlock, sym::g_globalLights> g_globalLights;

        ee::Ptr<Matrix> rowsOf(Vec4 (&rows)[4]) { return ee::Ptr<Vec4>(&rows[0]).cast<Matrix>(); }
        ee::Ptr<Matrix> directionsOf(ee::Ptr<LightBlock> lights) { return lights.cast<Matrix>(); }
        ee::Ptr<Matrix> coloursOf(ee::Ptr<LightBlock> lights) { return rowsOf(lights->colours); }
    }

    // drawRigid: a rigid part (render types 4 and 0x18; ZGE2: 4 and 0x14).
    // ZGE4 lights a node lit by its own lights with the first draw bucket's
    // default lights, turned by the node's world.
    void drawRigid(ee::Ptr<Node> node, uint32_t cullFlags, sce::Vif1Packet *packet)
    {
        RigidDraw draw{drawRecordOf(node, cullFlags), {}};
        if (game::readable::kIsBmx && (node->flags & kNodeOwnLights) != 0)
            draw.lights = {&firstBucketNormalLights(*g_renderer), &firstBucketLightColours(*g_renderer), true};
        if (!host::drawFromRecord(packet, draw))
            ps2::encodeRigidDraw(packet, draw);
    }
    READABLE_FUNCTION(drawRigid, Verified);

    // drawTerrain: a terrain part (render type 0x20; normals, grass shells).
    void drawTerrain(ee::Ptr<Node> node, uint32_t cullFlags, sce::Vif1Packet *packet)
    {
        TerrainDraw draw{drawRecordOf(node, cullFlags, RenderType::Terrain), {}};
        const Node &n = *node;
        draw.shellDepth = n.views[draw.record.view].viewCentre.z - n.radius;
        if (!host::drawFromRecord(packet, draw))
            ps2::encodeTerrainDraw(packet, draw);
    }
    READABLE_FUNCTION(drawTerrain, Verified);

    // drawSkinned: a skinned part (render types 8 and 0xC), with its bones,
    // in the view's screen and clip matrices, lit by the node's own light
    // block (turned by its world) or by the scene's lights.
    void drawSkinned(ee::Ptr<Node> node, uint32_t cullFlags, sce::Vif1Packet *packet)
    {
        SkinnedDraw draw{drawRecordOf(node, cullFlags), *g_screenMatrix, *g_clipMatrix, {}};
        const Node &n = *node;
        if (n.flags & kNodeOwnLights)
            draw.lights = {directionsOf(n.lights), coloursOf(n.lights), true};
        else
            draw.lights = {rowsOf(g_globalLights->directions), rowsOf(g_globalLights->colours), false};
        if (!host::drawFromRecord(packet, draw, ps2::encodeSkinnedDraw))
            ps2::encodeSkinnedDraw(packet, draw);
    }

    // drawSkinnedInDefaultLights: ZGE2's drawSkinned: lit by the renderer's
    // default lights turned by the node's world for every node, their colours
    // the default lights' for a node lit by its own lights, else the scene
    // lights' (ZGE2 keeps no light block on the node).
    void drawSkinnedInDefaultLights(ee::Ptr<Node> node, uint32_t cullFlags, sce::Vif1Packet *packet)
    {
        SkinnedDraw draw{drawRecordOf(node, cullFlags), *g_screenMatrix, *g_clipMatrix, {}};
        Renderer &renderer = *g_renderer;
        const bool ownLights = (node->flagsDoubleword() & kNodeOwnLightsBit) != 0;
        draw.lights = {&renderer.normalLightMatrix, ownLights ? ee::Ptr<Matrix>(&renderer.lightColourMatrix) : rowsOf(renderer.globalLightColours), true};
        if (!host::drawFromRecord(packet, draw, ps2::encodeSkinnedDrawInDefaultLights))
            ps2::encodeSkinnedDrawInDefaultLights(packet, draw);
    }
    READABLE_FUNCTION_OWN(drawSkinned, Verified, dm2, drawSkinnedInDefaultLights);
}
