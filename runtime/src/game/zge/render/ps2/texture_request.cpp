// The texture request the class renderers make for every node they draw
// (requestTextures): the GS texture cache is filled from the VIF1 interrupt.
// The draw list keeps a marker, a VIF NOP in the mesh packet, placed after
// the last request. A node whose textures are not yet requested for the
// cache's current generation gets that NOP's interrupt bit set and goes on
// the draw list's queue: when VIF1 reaches the NOP, the handler
// (vif1Interrupt) uploads the queued node's texture levels. Every request
// then places a new marker for the next one.
//
// A node seen from further than 30 units (its view depth less its radius)
// asks for its far levels only (the marker's immediate bit 0 [inferred]);
// nearer, for all. Each is asked once per cache generation.

#include "zge/render/draw.h"
#include "zge/render/renderer.h"
#include "zge/render/material.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "sce/packet_calls.h"
#include "zge/render/texture.h"
#include "zge/render/vif_codes.h"
#include "sym/zge/render/ps2/texture_request.cpp.h"

namespace zge
{
    namespace
    {
        // The view's draw list (g_renderer): the request queue (appended: ZGE3's nodes, ZGE4's material tables),
        // the marker, and a word set when the next request has to place a marker first (the list was started).
        inline ee::Global<ee::Ptr<ee::Address>, kRendererAddress + offsetof(Renderer, requestQueueEnd)> g_requestQueueEnd;
        inline ee::Global<ee::Ptr<uint32_t>, kRendererAddress + offsetof(Renderer, marker)> g_marker;
        inline ee::Global<uint32_t, kRendererAddress + offsetof(Renderer, markerNeeded)> g_markerNeeded;
        // The texture cache's generation [inferred]: material tables keep the
        // one they last requested in (MaterialTable::textureEpoch).
        inline ee::Global<uint32_t, sym::g_textureEpoch> g_textureEpoch;

        // Bits of MaterialTable::flags this sets (ZGE4's one higher).
        constexpr uint32_t kRequestedFar = game::readable::perGame(1u << 19, 1u << 19, 1u << 20); // the far levels asked for
        constexpr uint32_t kRequestedAll = game::readable::perGame(1u << 20, 1u << 20, 1u << 21);  // every level asked for

        const ee::Float kNearDepth(30.0f);

        // A VIF code appended (sceVif1PkAddCode; the library has three copies
        // of it, this the one at its own symbol).
        void addCode(sce::Vif1Packet *packet, uint32_t code) { sce::vif1PacketAddCode(packet, code); }
        namespace tail
        {
            void addCode(sce::Vif1Packet *packet, uint32_t code) { ee::tailCall(sym::vif1PacketAddCode, packet, code); }
        }

        // The node's MARK, the marker (a NOP the next request may patch), FLUSHA.
        void placeMarker(sce::Vif1Packet *packet)
        {
            addCode(packet, vif::mark(kMarkNodeTextures));
            *g_marker = packet->current;
            addCode(packet, vif::nop());
            addCode(packet, vif::flushA());
        }
    }

    // requestTextures: (packet, node, the node's materials).
    void requestTextures(sce::Vif1Packet *packet, ee::Ptr<Node> node, ee::Ptr<MaterialTable> materials)
    {
        if (*g_markerNeeded)
        {
            *g_markerNeeded = 0;
            placeMarker(packet);
        }
        MaterialTable &table = *materials;
        if (table.textureEpoch != *g_textureEpoch)
        {
            table.flags &= ~(kRequestedAll | kRequestedFar);
            table.textureEpoch = *g_textureEpoch;
        }
        const Node &n = *node;
        const ee::Float depth = n.views[*g_currentView].viewCentre.z - n.radius;
        const uint32_t flags = table.flags;
        // Asked for the far levels only and still further than 30: nothing new.
        const bool farStill = !(flags & kRequestedAll) && (flags & kRequestedFar) && !(depth <= kNearDepth);
        if (!farStill)
        {
            // Not yet asked for all: ask now.
            if (!(flags & kRequestedAll))
            {
                const ee::Ptr<uint32_t> marker = *g_marker;
                if (kNearDepth < depth)
                {
                    *marker |= kMarkerFar;
                    table.flags |= kRequestedFar;
                }
                else
                    table.flags = flags | kRequestedFar | kRequestedAll;
                *marker |= vif::kInterrupt;
                const ee::Ptr<ee::Address> queueEnd = *g_requestQueueEnd;
                if constexpr (game::readable::kIsBmx) // ZGE4 queues the node's material table, ZGE3 the node
                    *queueEnd = materials;
                else
                    *queueEnd = node;
                *g_requestQueueEnd = queueEnd + 1;
            }
        }
        placeMarker(packet);
        tail::addCode(packet, vif::mark(kMarkNoTextures)); // the original's tail call
    }
    READABLE_FUNCTION(requestTextures, Verified);
}
