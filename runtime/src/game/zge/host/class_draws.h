#pragma once

// The class renderers' draws as the host draws them (render_path = records): the host's form of the class draws,
// beside their PS2 form (zge/render/ps2/class_draws.h, the strict encoder). The class renderers
// (zge/render/class_renderers.cpp) hand each draw record here first; a draw this does not take goes to the strict
// encoder as ever, and one it takes still runs the encoder, without its VU1 work. game/host/native_draws.h says what
// reaches the GS and what stays in the mesh packet.

#include "zge/render/draw.h"
#include "zge/render/ps2/class_draws.h"
#include "runtime/ps2_zaxis_mesh.h"

namespace sce
{
    struct Vif1Packet;
}

namespace zge::host
{
    // The draw made natively from its record, in the view's mesh packet's place; false (nothing written) when
    // records are off or the draw is one this does not make (game/host/native_draws.h's counts say why).
    bool drawFromRecord(sce::Vif1Packet *packet, const RigidDraw &draw);
    bool drawFromRecord(sce::Vif1Packet *packet, const TerrainDraw &draw);
    // A skinned draw also takes the class's strict encoder (ZGE2's has its own), run without its VU1 work for the bones,
    // matrices and lights it computes.
    using SkinnedEncoder = void (*)(sce::Vif1Packet *packet, const SkinnedDraw &draw, ps2::SkinnedUpload *toHost);
    bool drawFromRecord(sce::Vif1Packet *packet, const SkinnedDraw &draw, SkinnedEncoder encode);

    // A part's packet built (drawNodeInView): whole, or again from the streams that changed (`streams`, the part's
    // rebuild bits: its mesh from the GEO data is then not what VU1 draws: the strict encoder draws it from then on).
    // A part whose streams the game made (zge/native/geo_meshes.h notePartStreamsMade) has its mesh made from them as
    // they are then.
    void notePartPacketBuilt(ee::Ptr<ZgePart> part, bool whole, uint32_t streams);

    // The game's skinned packet form (game/host/native_draws.h attach): for the mesh check's file meshes.
    const ps2x::zaxis::SkinnedDrawForm &skinnedDrawForm();
    // The packet a part of a render type is built as (false: a type with no packet the host form knows): the game's
    // rigid packet form says whether a part's normals go where its colours do (ZGE4's).
    bool packetKindOf(const ZgePart &part, RenderType type, ps2x::zaxis::PacketKind &kind);
}
