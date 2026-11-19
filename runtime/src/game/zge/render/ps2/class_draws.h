#pragma once

// The strict encoder of the class renderers' draws (zge/render/class_renderers.cpp): a node's draw record
// (zge/render/draw.h) as the PS2 draws it, appended to the view's mesh packet for VU1. Each draw is the part's
// prebuilt packet called as a DMA chain (the half of its double buffer drawn), VU1's state for the class once per
// draw list, the texture request's marker, the node's matrices and lights unpacked to VU1, then the class's
// microprogram. The encoder runs in its renderer's frame: the replay sees the renderer's calls and packets as the
// original makes them.

#include "zge/render/draw.h"
#include "zge/math/types.h"

#include <vector>

namespace sce
{
    struct Vif1Packet;
}

namespace zge::ps2
{
    // Each encoder writes a draw's VU1 work to the view's mesh packet. Without it (render_path = records: the host's
    // form draws natively, zge/host/class_draws.h) the encoder begins a DMA tag where the part's call would be (what
    // follows then goes after a chain called before it, as on the PS2 path) and still does everything else the game
    // reads back or leaves behind: the class's VU1 state once per draw list (the owner word), the texture request (the request
    // queue, the material table's flags, the marker the VIF1 handler stops at), terrain's shell depth in the node's
    // matrix, the bone list's walk, and the same VU0 work into the same frame (its registers' lanes as the strict
    // path leaves them); a skinned draw's rows go to the host's SkinnedUpload in place of the packet.

    // A skinned draw's VU1 upload as the encoder computes it: the bones' rows (each pose with the bind offset taken
    // out, w 0, 0, 0, 1), world -> screen, the lights' directions (rows k: component k of lights 0-2) and colours
    // (with the ambient), world -> clip.
    struct SkinnedUpload
    {
        std::vector<Matrix> bones;
        Matrix screen, lightDirections, lightColours, clip;
    };

    // ZGE4's rigid lights as the encoder sends them: the directions turned by the node's world (rows k: component k
    // of lights 0-2) and the colours (with the ambient).
    struct RigidLightsUpload
    {
        Matrix directions, colours;
    };

    // A rigid part (render types 4 and the sky's): the world matrix, ZGE2's clip plane when clipped, ZGE4's
    // lights (to the host's RigidLightsUpload as well, when it is given one); the type's microprogram, + 2
    // clipped, ZGE2 and ZGE3's own for a prelit part inside the far plane and the sides.
    void encodeRigidDraw(sce::Vif1Packet *packet, const RigidDraw &draw, bool vu1Work = true,
                         RigidLightsUpload *toHost = nullptr);
    // A terrain part: the world matrix with the shells' depth in its spare lane; program 0x20, 0x22 clipped.
    void encodeTerrainDraw(sce::Vif1Packet *packet, const TerrainDraw &draw, bool vu1Work = true);
    // A skinned part (ZGE3, ZGE4): the bone palette, then world -> screen, the lights, world -> clip; the type's
    // program + 2.
    void encodeSkinnedDraw(sce::Vif1Packet *packet, const SkinnedDraw &draw, SkinnedUpload *toHost = nullptr);
    // ZGE2's: the texture request written in, the bones walked with arrayFirst and arrayNext, each pose copied with
    // copyRotationAndTranslation.
    void encodeSkinnedDrawInDefaultLights(sce::Vif1Packet *packet, const SkinnedDraw &draw, SkinnedUpload *toHost = nullptr);
}
