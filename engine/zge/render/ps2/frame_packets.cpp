// The frame's packets as the PS2 sends them (zge/render/renderer.cpp is the
// renderer around them): the frame packet built in one of two buffers while
// DMA sends the other (the mesh packet the views append to, its texture
// requests' queue), begun, closed and sent to VIF1 with the double buffer's
// fix-ups applied; and the PS2 steps of the renderer's own (the
// microprograms sent, ZGE2's chain closed, the prologue's FLUSHA).
//
// The fix-ups: words of the packet that depend on which display buffer the
// frame is drawn in, listed while it is built (addFrameFixup) and written
// just before it is sent (applyFrameFixups); two lists, one being built
// while the other is applied.

#include "zge/render/ps2/frame_packets.h"
#include "zge/render/display.h"
#include "zge/render/renderer.h"
#include "zge/render/ps2/dma.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/mirror.h"
#include "ee/io.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/eeregs.h"
#include "sce/packet_calls.h"
#include "zge/platform/platform.h"
#include "zge/render/fx_manager.h"
#include "zge/render/primitives.h"
#include "zge/render/shadow.h"
#include "zge/render/gs_registers.h"
#include "zge/render/texture.h"
#include "zge/render/vif_codes.h"
#include "zge/scene/camera.h"
#include "zge/memory/regions.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/ps2/frame_packets.cpp.h"

namespace zge
{

    namespace
    {
        constexpr uint32_t kHeapFree = sym::heapFree;        // (block)
        constexpr uint32_t kOnFramePacketSent = sym::onFramePacketSent; // the frame's DMA done handler

        constexpr uint32_t kMeshPacket = kRendererAddress + offsetof(Renderer, meshPacket);
        // The renderer's words' base: setVu1StateOwner's result.
        constexpr uint32_t kRendererBase = ee::high(sym::g_renderer);

        // The VIFs' ERR registers' bit 1 (ME0: DMA tag mismatches not
        // reported), GIF_MODE's bit 2 (IMT: intermittent PATH3).
        constexpr int32_t kErrMaskDmaTag = 0x2;
        constexpr int32_t kGifModeIntermittent = 0x4;

        // The VU1 microprograms' DMA chain sent at the set-up (837 qwords).
        constexpr uint32_t kMicroprogramChain = sym::codeEnd;

        // The vsync handler's state: the frame being sent, and whether it
        // has gone (onFramePacketSent sets it).
        inline ee::Global<uint32_t, sym::g_sentFrame> g_sentFrame;
        inline ee::Global<uint8_t, sym::g_frameSent> g_frameSent;
        // The display's two draw environments and which one the frame is drawn with.
        inline ee::Global<uint32_t, sym::g_drawEnvironment> g_drawEnvironment;
        inline ee::Global<GsDrawEnvironment, sym::g_drawEnvironment0> g_drawEnvironment0;
        inline ee::Global<GsDrawEnvironment, sym::g_drawEnvironment1> g_drawEnvironment1;

        // The two lists (0x180 bytes each), the one being built (start and
        // end) and the one to apply.
        inline ee::Global<ee::Ptr<FrameFixup>[2], sym::g_fixupLists> g_fixupLists;
        inline ee::Global<ee::Ptr<FrameFixup>, sym::g_fixupsStart> g_fixupsStart;
        inline ee::Global<ee::Ptr<FrameFixup>, sym::g_fixupsEnd> g_fixupsEnd;
        inline ee::Global<ee::Ptr<FrameFixup>, sym::g_sendFixupsStart> g_sendFixupsStart;
        inline ee::Global<ee::Ptr<FrameFixup>, sym::g_sendFixupsEnd> g_sendFixupsEnd;
        inline ee::Global<uint32_t, sym::g_fixupList> g_fixupList;
        constexpr uint32_t kFixupListBytes = 0x180u;

        namespace // the game functions below call: ee/plain.h
        {
            void addCode(ee::Ptr<sce::Vif1Packet> packet, uint32_t code) { ee::game(sym::vif1PacketAddCode, packet, code); }
        }

        ee::Ptr<sce::Vif1Packet> meshPacketRecord() { return &g_renderer->meshPacket; }

        // The frame being built: ZGE4's is the display's frame parity (its beginFramePackets starts the frame's
        // packet there), ZGE2's and ZGE3's the renderer's own (flipMeshPacket).
        uint32_t frameBeingBuilt()
        {
            if constexpr (game::readable::kIsBmx)
                return g_display->frameParity;
            else
                return g_renderer->frame;
        }
    }

    // ---- the packets' buffers ------------------------------------------------

    // flipMeshPacket: the other frame buffer taken for the frame to build: the
    // mesh packet started in it (uncached) and its request queue emptied.
    void flipMeshPacket()
    {
        Renderer &renderer = *g_renderer;
        const uint32_t frame = renderer.frame ^ 0x1u;
        renderer.frame = frame;
        const uint32_t buffer = renderer.frameBuffers[frame];
        renderer.frameBuffer = buffer;
        vif1PacketInit(meshPacketRecord(), ee::uncachedMirror(buffer));
        vif1PacketReset(meshPacketRecord());
        renderer.requestQueueEnd = renderer.requestQueues[renderer.frame];
    }
    READABLE_FUNCTION(flipMeshPacket, Verified);

    void resetFrameFixups();

    // newFrameFixups: the fix-up lists made and the first taken
    // (resetFrameFixups, the original's tail call).
    void newFrameFixups()
    {
        (*g_fixupLists)[0] = ee::Ptr<FrameFixup>(heapAllocate(kFixupListBytes));
        (*g_fixupLists)[1] = ee::Ptr<FrameFixup>(heapAllocate(kFixupListBytes));
        game::tail(resetFrameFixups);
    }
    READABLE_FUNCTION_WAITS(newFrameFixups, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void heapFree(ee::Ptr<FrameFixup> block) { ee::tailCall(kHeapFree, block); }
        }
    }

    // freeFrameFixups: the fix-up lists freed (the second free the original's
    // tail call).
    void freeFrameFixups()
    {
        heapFree((*g_fixupLists)[0]);
        tail::heapFree((*g_fixupLists)[1]);
    }
    READABLE_FUNCTION_WAITS(freeFrameFixups, Verified);

    // resetFrameFixups: the first list taken, empty, and none to apply.
    void resetFrameFixups()
    {
        const ee::Ptr<FrameFixup> first = (*g_fixupLists)[0];
        *g_fixupList = 0;
        *g_fixupsEnd = first;
        *g_fixupsStart = first;
        *g_sendFixupsStart = ee::Ptr<FrameFixup>();
        *g_sendFixupsEnd = ee::Ptr<FrameFixup>();
    }
    READABLE_FUNCTION(resetFrameFixups, Verified);

    // flipFrameFixups: the list built becomes the one to apply, and the other
    // is taken, empty (returns the slot of the list taken).
    ee::Ptr<ee::Ptr<FrameFixup>> flipFrameFixups()
    {
        const uint32_t list = *g_fixupList ^ 0x1u;
        const ee::Ptr<ee::Ptr<FrameFixup>> slot = &(*g_fixupLists)[list];
        *g_sendFixupsStart = *g_fixupsStart;
        *g_sendFixupsEnd = *g_fixupsEnd;
        *g_fixupList = list;
        *g_fixupsEnd = *slot;
        *g_fixupsStart = *slot;
        return slot;
    }
    READABLE_FUNCTION(flipFrameFixups, Verified);

    // addFrameFixup: a fix-up added to the list built (the list's new end).
    ee::Ptr<FrameFixup> addFrameFixup(uint32_t kind, ee::Address address, uint32_t value)
    {
        const ee::Ptr<FrameFixup> fixup = *g_fixupsEnd;
        *g_fixupsEnd = fixup + 1;
        fixup->value = ee::uncachedMirror(value);
        fixup->kind = kind;
        fixup->word = ee::Ptr<uint64_t>(ee::uncachedMirror(address));
        return *g_fixupsEnd;
    }
    READABLE_FUNCTION(addFrameFixup, Verified);

    // applyFrameFixups: the list to apply written into the frame's packet, for
    // the draw environment the display uses next.
    void applyFrameFixups()
    {
        const bool other = (*g_drawEnvironment ^ 0x1u) != 0;
        const GsDrawEnvironment &drawn = other ? *g_drawEnvironment1 : *g_drawEnvironment0;
        const uint64_t drawOffset = drawn.xyOffset;
        const uint64_t frameRegister = (other ? *g_drawEnvironment0 : *g_drawEnvironment1).frame;
        const uint64_t textureBase = gs::blockOfPage(gs::frameBasePage(frameRegister));
        for (ee::Ptr<FrameFixup> fixup = *g_sendFixupsStart; before(fixup, *g_sendFixupsEnd); ++fixup)
        {
            if (game::readable::kIsBmx && fixup->kind == kFixupDrawFrame) // (ZGE4's kind)
                *fixup->word = *fixup->word & ~uint64_t{gs::kFrameBaseBits} | gs::frameBasePage(drawn.frame);
            else if (fixup->kind == kFixupLastFrameTexture)
                *fixup->word = *fixup->word & ~gs::Tex0::kTbp0.mask() | textureBase;
            else if (fixup->kind == kFixupDrawOffset)
                *fixup->word = drawOffset;
        }
    }
    READABLE_FUNCTION(applyFrameFixups, Verified);

    // sendFramePacket: a frame packet sent to VIF1 (the frame being built
    // when `other` is 0, else the other one): its fix-ups applied, PATH3
    // intermittent, the vsync handler told (not sent yet), and the chain
    // started with onFramePacketSent to be called when done (the original's
    // tail call).
    int32_t sendFramePacket(uint32_t other)
    {
        const uint32_t frame = frameBeingBuilt() ^ other;
        applyFrameFixups();
        ee::io::store<int32_t>(sce::GIF_MODE, ee::io::load<int32_t>(sce::GIF_MODE) | kGifModeIntermittent);
        *g_frameSent = 0;
        const uint32_t chain = ee::physical(g_renderer->frameBuffers[frame]);
        *g_sentFrame = chain;
        // (The original hands the display's address on too, a fifth word dmaSendChain does not read.)
        return game::tail(dmaSendChain, 1, chain, 581, kOnFramePacketSent);
    }

    // sendFrameChain: ZGE2's sendFramePacket: the frame's chain (the one
    // being built when `other` is 0, else the other one) sent to VIF1 with
    // PATH3 intermittent, as ZGE3's, but with no fix-ups applied, nothing
    // told to the vsync handler and no call when it is done (the original's
    // tail call).
    int32_t sendFrameChain(uint32_t other)
    {
        const uint32_t frame = frameBeingBuilt() ^ other;
        ee::io::store<int32_t>(sce::GIF_MODE, ee::io::load<int32_t>(sce::GIF_MODE) | kGifModeIntermittent);
        const uint32_t chain = ee::physical(g_renderer->frameBuffers[frame]);
        *g_sentFrame = chain;
        return game::tail(dmaSendChain, 1, chain, 581, 0);
    }
    READABLE_FUNCTION_OWN(sendFramePacket, Verified, dm2, sendFrameChain);

    // meshPacket: the view's mesh packet.
    uint32_t meshPacket() { return kMeshPacket; }
    READABLE_FUNCTION(meshPacket, Verified);

    // setVu1StateOwner: the class renderer whose VU1 state went out last (the
    // shadow pass's); returns kRendererBase.
    int32_t setVu1StateOwner(uint32_t owner)
    {
        g_renderer->vu1StateOwner = owner;
        return static_cast<int32_t>(kRendererBase);
    }
    READABLE_FUNCTION(setVu1StateOwner, Verified);

    // beginningFrameNoop (ZGE4 only): an empty function beginFramePackets calls last (no build has a body or a name
    // for it; closingFrameNoop's neighbour).
    inline void beginningFrameNoop()
    {
    }
    READABLE_FUNCTION_ONLY(bmx, beginningFrameNoop);

    // beginFramePackets: the frame's packets begun (renderFrame): no VU1 state
    // sent yet, a marker due, the viewport's packet and the packet buffer
    // flipped. Always 1. ZGE4 starts the frame's mesh packet here, in the
    // buffer of the display's frame parity, with its request queue and the
    // primitives' buffers (ZGE2 and ZGE3: finishFrame, for the next frame).
    uint32_t beginFramePackets()
    {
        Renderer &renderer = *g_renderer;
        if constexpr (game::readable::kIsBmx)
        {
            const uint32_t frame = frameBeingBuilt();
            const uint32_t buffer = renderer.frameBuffers[frame];
            renderer.frameBuffer = buffer;
            vif1PacketInit(meshPacketRecord(), ee::uncachedMirror(buffer));
            vif1PacketReset(meshPacketRecord());
            const ee::Ptr<uint32_t> queue = renderer.requestQueues[frame];
            startFrameRequestQueue(renderer, queue);
            renderer.requestQueueEnd = queue;
        }
        renderer.vu1StateOwner = 0;
        renderer.markerNeeded = 1;
        if constexpr (game::readable::kIsBmx)
            resetPrimitiveBuffersIntoPacket(meshPacketRecord());
        addViewportPacket();
        flipShadowPacket();
        if constexpr (game::readable::kIsBmx)
            beginningFrameNoop();
        return 1;
    }
    READABLE_FUNCTION(beginFramePackets, Verified);

    // closingFrameNoop (ZGE4 only): an empty function closeFramePacket calls first (no build has a body or a name
    // for it).
    inline void closingFrameNoop()
    {
    }
    READABLE_FUNCTION_ONLY(bmx, closingFrameNoop);

    // closeFramePacket: the frame's mesh packet closed (renderFrame): its open
    // DMA tag, then NEXT to the screen primitives' packets (or END when
    // there are none), aligned and closed, FLUSHA; the request queue ended
    // (0).
    void closeFramePacket()
    {
        const ee::Ptr<sce::Vif1Packet> packet = meshPacketRecord();
        if constexpr (game::readable::kIsBmx)
            closingFrameNoop();
        closeShadowPacket();
        const uint32_t screenPrimitives = closeScreenPrimitivePackets();
        if (screenPrimitives != 0)
            sce::vif1PacketNext(packet, screenPrimitives, 0);
        else
            sce::vif1PacketEnd(packet, 0);
        if constexpr (game::readable::kIsBmx) // (ZGE4 adds the FLUSHA before the tag is aligned and closed)
            addCode(packet, vif::flushA());
        sce::vif1PacketAlign(packet, 2, 0);
        vif1PacketCloseDmaTag(packet);
        if constexpr (!game::readable::kIsBmx)
            addCode(packet, vif::flushA());
        Renderer &renderer = *g_renderer;
        const ee::Ptr<uint32_t> queueEnd = renderer.requestQueueEnd;
        *queueEnd = 0;
        renderer.vu1StateOwner = 0;
        renderer.requestQueueEnd = queueEnd + 1;
    }
    READABLE_FUNCTION(closeFramePacket, Verified);

    // clearFramePackets: both frame buffers given an empty chain (END), with the
    // fix-ups reset first. Always 1.
    uint32_t clearFramePackets()
    {
        resetFrameFixups();
        const ee::Ptr<sce::Vif1Packet> packet = &g_renderer->clearPacket;
        for (const uint32_t buffer : g_renderer->frameBuffers)
        {
            vif1PacketInit(packet, ee::uncachedMirror(buffer));
            vif1PacketReset(packet);
            sce::vif1PacketEnd(packet, 0);
            sce::vif1PacketAlign(packet, 2, 0);
            vif1PacketCloseDmaTag(packet);
        }
        return 1;
    }
    READABLE_FUNCTION(clearFramePackets, Verified);

    namespace ps2
    {
        // loadMicroprograms (initRenderer): VU1's microprograms sent to VIF1 (a
        // DMA chain), and DMA tag mismatches masked in both VIFs.
        void loadMicroprograms()
        {
            dmaSendChain(1, kMicroprogramChain, 837, 0);
            ee::io::store<int32_t>(sce::VIF0_ERR, ee::io::load<int32_t>(sce::VIF0_ERR) | kErrMaskDmaTag);
            ee::io::store<int32_t>(sce::VIF1_ERR, ee::io::load<int32_t>(sce::VIF1_ERR) | kErrMaskDmaTag);
        }

        // closeMeshPacket (ZGE2's finishFrame): the frame's mesh packet ended with
        // NEXT to the screen primitives' packets (END when there are none),
        // aligned and its tag closed.
        void closeMeshPacket(uint32_t screenPrimitives)
        {
            const ee::Ptr<sce::Vif1Packet> packet = meshPacketRecord();
            if (screenPrimitives != 0)
                sce::vif1PacketNext(packet, screenPrimitives, 0);
            else
                sce::vif1PacketEnd(packet, 0);
            sce::vif1PacketAlign(packet, 2, 0);
            vif1PacketCloseDmaTag(packet);
        }

        // waitForFramePrologue (beginViewPacket): FLUSHA in the mesh packet: the
        // view's draws wait until the prologue is drawn.
        void waitForFramePrologue()
        {
            addCode(meshPacketRecord(), vif::flushA());
        }
    }
}
