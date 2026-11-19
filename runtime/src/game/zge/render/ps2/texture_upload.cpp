// The VIF1 interrupt handler (vif1Interrupt): uploads the textures the
// texture request (ps2/texture_request.cpp) queued. The request marks a VIF NOP
// in the mesh packet with the interrupt bit, inside a MARK 1 block; when
// VIF1 reaches it, this handler takes the next queued node, builds a GIF
// packet of the texture levels (and CLUTs) its materials still have to
// upload, placing each in the GS texture cache first if it has no GS address
// yet, starts it on the GIF DMA channel (D2) and lets VIF1 go on.
//
// MARK 2 does the same for the primitive API's textures (the half of its
// double-buffered list the last frame filled, primitiveTextures).

#include "zge/render/material.h"
#include "zge/render/renderer.h"
#include "zge/render/texture.h"
#include "zge/render/gs_registers.h"

#include "sce/eeregs.h"
#include "sce/packet_calls.h"
#include "zge/scene/node.h"
#include "ee/mirror.h"
#include "ee/io.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/platform/platform.h"
#include "zge/render/display.h"
#include "zge/render/ps2/dma.h"
#include "zge/render/primitives.h"
#include "zge/debug/profiler.h"

#include <cstddef>
#include <cstring>
#include <type_traits>
#include "sym/zge/render/ps2/texture_upload.cpp.h"

namespace zge
{
    namespace
    {
        // EE registers (I/O).
        // (sce/eeregs.h: VIF1_FBRST, 8 (STC) cancels the stall at the
        // interrupt; VIF1_MARK, which block the interrupt is in; VIF1_CODE,
        // the interrupting NOP (its immediate); GIF_STAT; the GIF DMA
        // channel's D2_CHCR, D2_QWC, D2_TADR.)
        constexpr uint32_t kChcrStart = 0x100u;           // STR
        constexpr uint32_t kChcrSourceChain = 0x145u;     // STR, chain mode, from memory, tag transfer
        constexpr uint32_t kFbrstCancelStall = 8u;
        constexpr uint32_t kGifStatActivePath = 0xC00u;   // GIF_STAT's APATH (bits 10-11): the path transferring, 0 none
        // The upload packet's last GIF tag: no data, the end of the packet (EOP).
        constexpr uint64_t kEndGifTag = gs::GifTag{.endOfPacket = 1}.bits();

        // libgifpk [names inferred from the library's order]: a GIF packet
        // in the handler's frame.
        constexpr uint32_t kGifPacketInit = sym::gifPacketInit;      // (packet, buffer)
        constexpr uint32_t kGifPacketReset = sym::gifPacketReset;     // (packet)
        constexpr uint32_t kGifPacketTerminate = sym::gifPacketTerminate; // (packet)
        constexpr uint32_t kGifPacketCnt = sym::gifPacketCnt;       // (packet, 0, 0, 0): a DMA cnt tag
        constexpr uint32_t kGifPacketCall = sym::gifPacketCall;      // sceGifPkCall (packet, chain, 0, 0, 0)
        constexpr uint32_t kGifPacketEnd = sym::gifPacketEnd;       // (packet, 0, 0, 0): the DMA end tag
        constexpr uint32_t kGifPacketAddDword = sym::gifPacketAddDword;  // (packet, a doubleword)
        constexpr uint32_t kPlaceTextureLevel = sym::placeTextureLevel;  // placeTextureLevel (texture.cpp; a game override hooks it)

        // The view's draw list (g_renderer), its texture upload part.
        // What requestTextures queues: the node (ZGE4: the node's material table), and its materials.
        using TextureRequest = std::conditional_t<game::readable::kIsBmx, MaterialTable, Node>;
        [[maybe_unused]] MaterialTable &requestMaterials(ee::Ptr<Node> node) { return *node->materials; }
        [[maybe_unused]] MaterialTable &requestMaterials(ee::Ptr<MaterialTable> materials) { return *materials; }
        inline ee::Global<ee::Ptr<ee::Ptr<TextureRequest>>, kRendererAddress + offsetof(Renderer, requestQueueRead)> g_requestQueueRead; // +0x70 the next queued request
        inline ee::Global<ee::Ptr<uint32_t>[2], sym::g_uploadBuffers> g_uploadBuffers;    // +0x74, +0x78 the GIF packets' buffers
        inline ee::Global<uint32_t, sym::g_uploadBuffer> g_uploadBuffer;     // +0x7C which one this upload uses
        inline ee::Global<uint32_t, sym::g_materialsSeen> g_materialsSeen;    // +0x98 statistics: materials looked at
        inline ee::Global<uint32_t, sym::g_interrupts> g_interrupts;       // +0x9C interrupts handled
        inline ee::Global<uint32_t, sym::g_clutBytes> g_clutBytes;        // +0xA0 CLUT bytes uploaded
        inline ee::Global<uint32_t, sym::g_texelBytes> g_texelBytes;       // +0xA4 texel bytes uploaded
        // Set while texture uploads are held off [inferred: a byte of the
        // render state at g_displaySetupFlags].
        inline ee::Global<uint8_t, sym::g_uploadsOff> g_uploadsOff;
        // ZGE2 keeps none of these statistics, and no hold on the uploads.
        constexpr bool kCountsUploads = !game::readable::kIsDm2;
        constexpr bool kHasUploadsOff = !game::readable::kIsDm2;

        // A GsTexture's pending upload bits (GsTexture::pendingUploads).
        constexpr uint32_t kPendingClut = 0x100u;

        // An upload chain's BITBLTBUF with DBP set to the GS word address's
        // block.
        uint64_t withDestination(uint64_t bitbltbuf, uint32_t gsAddress)
        {
            return gs::withField(bitbltbuf, gs::BitBltBuf::kDbp, gs::blockOf(gsAddress));
        }

        // What the handler keeps in its frame, as the original lays it out:
        // the upload's GIF packet, the materials still to look at (a count
        // and a cursor, which primitiveTextures fills for the primitives'
        // textures), whether the packet is open, the first level to upload
        // and the packet once opened.
        struct UploadFrame
        {
            sce::GifPacket packet;                        // +0x00
            int32_t materialsLeft;                        // +0x10
            ee::Ptr<ee::Ptr<Material>> materialCursor;    // +0x14
            int32_t open;                                 // +0x18
            uint32_t firstLevel;                          // +0x1C
            ee::Ptr<sce::GifPacket> openPacket;           // +0x20
        };
        static_assert(offsetof(UploadFrame, materialsLeft) == 0x10 && offsetof(UploadFrame, openPacket) == 0x20);

        // The handler reaches the textures through the uncached mirror: what
        // it patches, DMA reads straight from RAM.
        template <typename T>
        ee::Ptr<T> uncachedPointer(T &object) { return ee::uncachedMirror(ee::Ptr<T>(&object)); }

        // libgifpk's calls, by the game's dispatch (sce/gifpk.h).
        namespace gif
        {
            void init(ee::Ptr<sce::GifPacket> packet, ee::Ptr<uint32_t> buffer) { ee::game(kGifPacketInit, packet, buffer); }
            void reset(ee::Ptr<sce::GifPacket> packet) { ee::game(kGifPacketReset, packet); }
            void cnt(ee::Ptr<sce::GifPacket> packet, uint32_t word2, uint32_t word3, uint32_t flags) { ee::game(kGifPacketCnt, packet, word2, word3, flags); }
            void addDword(ee::Ptr<sce::GifPacket> packet, uint64_t value) { ee::game(kGifPacketAddDword, packet, value); }
            void call(ee::Ptr<sce::GifPacket> packet, ee::Ptr<uint8_t> chain, uint32_t word2, uint32_t word3, uint32_t flags) { ee::game(kGifPacketCall, packet, chain, word2, word3, flags); }
            void end(ee::Ptr<sce::GifPacket> packet, uint32_t word2, uint32_t word3, uint32_t flags) { ee::game(kGifPacketEnd, packet, word2, word3, flags); }
            void terminate(ee::Ptr<sce::GifPacket> packet) { ee::game(kGifPacketTerminate, packet); }
        }

        // The upload packet, opened at the first texture with something to
        // upload: the other buffer, a DMA cnt tag, a GIF tag (A+D, NLOOP 1)
        // setting TRXPOS to 0.
        void openUploadPacket(UploadFrame &f)
        {
            const ee::Ptr<sce::GifPacket> packet = &f.packet;
            f.openPacket = packet;
            const uint32_t buffer = *g_uploadBuffer ^ 1u;
            f.open = 1;
            *g_uploadBuffer = buffer;
            gif::init(packet, (*g_uploadBuffers)[buffer]);
            gif::reset(packet);
            gif::cnt(packet, 0, 0, 0);
            gif::addDword(packet, uint64_t{gs::GifTag{.loops = 1, .registers = 1}.bits()}); // GIF tag: one pair,
            gif::addDword(packet, uint64_t{gs::kAddressData});                             //   an A+D
            gif::addDword(packet, uint64_t{0});                                            // TRXPOS = 0
            gif::addDword(packet, uint64_t{gs::kTrxPos});
        }

        // An upload chain called from the packet, by its physical address.
        void callUploadChain(const UploadFrame &f, uint8_t *chain)
        {
            gif::call(f.openPacket, ee::physical(ee::Ptr<uint8_t>(chain)), 0, 0, 0);
        }

        // One material's texture: its CLUT and levels still to upload are
        // called from the packet, then its GS blocks get the texture's TEX0
        // and MIPTBP1.
        void uploadMaterial(UploadFrame &f, ee::Ptr<Material> material)
        {
            const ee::Ptr<Texture> texture(*textureSlotOf(material, 0));
            if (!texture)
                return; // (vif1Interrupt marks the end)
            const ee::Ptr<GsTexture> gs = uncachedPointer(texture->gs);
            GsTexture &t = *gs;
            uint32_t pending = t.pendingUploads;
            if (pending != 0)
            {
                if (f.open == 0)
                    openUploadPacket(f);
                if ((pending & kPendingClut) && t.clut)
                {
                    pending &= ~kPendingClut;
                    if constexpr (kCountsUploads)
                        *g_clutBytes += t.clutQwords << 4;
                    if (t.clutGsAddress == 0)
                        placeTextureClut(gs);
                    t.clutUploadBitbltbuf = withDestination(t.clutUploadBitbltbuf, t.clutGsAddress);
                    callUploadChain(f, t.clutUploadHead);
                }
                const uint32_t levelCount = t.levelCount;
                // From the NOP's first level (or the last one), kept for the
                // next materials of the node.
                const uint32_t last = levelCount - 1u;
                uint32_t first = f.firstLevel;
                if (static_cast<int32_t>(last) < static_cast<int32_t>(first))
                    first = last;
                f.firstLevel = first;
                uint32_t bit = 1u << (first & 31u);
                ee::Ptr<TextureLevel> level = uncachedPointer(t.levels[first]);
                // As the original: the bit and the level move on only after a
                // level that was uploaded, so the walk stops looking past the
                // first level with nothing pending.
                for (int32_t left = static_cast<int32_t>(levelCount - first) - 1; left != -1; --left)
                {
                    if (!(pending & bit))
                        continue;
                    TextureLevel &l = *level;
                    if constexpr (kCountsUploads)
                        *g_texelBytes += l.uploadQwords << 4;
                    pending &= ~bit;
                    if (l.gsAddress == 0)
                        placeTextureLevel(gs, level);
                    l.uploadBitbltbuf = withDestination(l.uploadBitbltbuf, l.gsAddress);
                    bit <<= 1;
                    ++level;
                    callUploadChain(f, l.uploadHead);
                }
                t.pendingUploads = pending;
            }
            materialLayer(material, 0).tex0.data = t.tex0;
            materialLayer(material, 0).miptbp1.data = t.miptbp1;
        }

        // Waits for the GIF channel and the GIF to be idle (giving up after
        // 2^32 polls: then nothing is started). True when idle.
        bool waitForGif()
        {
            uint32_t polls = 0;
            bool checkChannel = (ee::io::load<uint32_t>(sce::D2_CHCR) & kChcrStart) != 0;
            if (checkChannel)
                ++polls;
            for (;;)
            {
                if (checkChannel)
                {
                    if (polls == 0)
                        return false;
                    if (ee::io::load<uint32_t>(sce::D2_CHCR) & kChcrStart)
                    {
                        ++polls;
                        continue;
                    }
                }
                const bool gifBusy = (ee::io::load<uint32_t>(sce::GIF_STAT) & kGifStatActivePath) != 0;
                ++polls;
                if (!gifBusy)
                    break;
                checkChannel = true;
            }
            return true;
        }

        // The VIF1 stall at the interrupt cancelled and interrupts back on:
        // how every way out of the handler ends.
        void resumeVif1()
        {
            ee::io::store<uint32_t>(sce::VIF1_FBRST, kFbrstCancelStall);
            ee::io::enableInterrupts();
        }
    }

    // vif1Interrupt (the VIF1 interrupt): a MARK in the view's packet stopped
    // VIF1 at a block of texture requests. MARK 1 is a node's request (the
    // next in the request queue: its materials, from the level the NOP's
    // immediate names), MARK 2 the primitives' textures. Every texture with
    // levels or a CLUT still to upload gets them called from one GIF packet,
    // sent on the GIF channel once the GIF is idle; VIF1 then goes on. (ZGE2: no
    // hold on the uploads, no statistics. ZGE4 queues the node's material table.)
    int32_t vif1Interrupt()
    {
        ee::Local<UploadFrame> frame;
        UploadFrame &f = *frame;
        f.open = 0;
        if constexpr (kHasUploadsOff)
            if (*g_uploadsOff != 0)
            {
                resumeVif1();
                return 0;
            }
        f.firstLevel = 0;
        f.openPacket = {};
        const uint32_t mark = ee::io::load<uint32_t>(sce::VIF1_MARK);
        if (mark == kMarkNodeTextures)
        {
            const ee::Ptr<ee::Ptr<TextureRequest>> read = *g_requestQueueRead;
            const ee::Ptr<TextureRequest> request = *read;
            *g_requestQueueRead = read + 1;
            if (!request)
            {
                *g_requestQueueRead = read; // the queue is empty: nothing
                resumeVif1();
                return 0;
            }
            MaterialTable &table = requestMaterials(request);
            f.materialCursor = table.materials;
            f.materialsLeft = table.count;
            f.firstLevel = ee::io::load<uint32_t>(sce::VIF1_CODE) & kMarkerFirstLevel;
        }
        else if (mark == kMarkPrimitiveTextures)
            primitiveTextures(&f.materialsLeft, &f.materialCursor);
        else
        {
            resumeVif1();
            return 0;
        }

        const int32_t count = f.materialsLeft;
        if constexpr (kCountsUploads)
        {
            *g_materialsSeen += count;
            *g_interrupts += 1;
        }
        for (f.materialsLeft = count - 1; f.materialsLeft != -1; --f.materialsLeft)
        {
            const ee::Ptr<Material> material = *f.materialCursor;
            ++f.materialCursor;
            uploadMaterial(f, material);
        }
        if (f.open != 0)
        {
            gif::end(f.openPacket, 0, 0, 0);
            gif::addDword(f.openPacket, kEndGifTag);
            gif::terminate(f.openPacket);
            if (waitForGif())
            {
                ee::io::store<uint32_t>(sce::D2_QWC, 0);
                ee::io::store<uint32_t>(sce::D2_TADR, ee::dmaAddress(f.openPacket->start));
                ee::io::store<uint32_t>(sce::D2_CHCR, kChcrSourceChain);
            }
        }
        resumeVif1();
        return 0;
    }
    READABLE_FUNCTION(vif1Interrupt, Verified);

    // ---- the uploads' buffers ---------------------------------------------------

    namespace
    {
        constexpr uint32_t kVblankStartHandler = sym::vif1Interrupt; // vif1Interrupt, above
        // The upper half of the renderer's address (%hi), kept in a saved register.
        constexpr uint32_t kRendererHigh = ee::high(sym::g_renderer);
        constexpr uint32_t kUploadBufferBytes = 0x2000u;
        constexpr uint32_t kRequestQueueBytes = 0xC00u;
        constexpr uint32_t kHeapPool = 0;
        // 1 once the frame packet's DMA has ended (sendFramePacket clears it).
        inline ee::Global<uint8_t, sym::g_frameSent> g_framePacketSent;
    }

    // initUploadBuffers: the vblank-start handler (vif1Interrupt) installed and,
    // a frame each, the uploads' GIF packet buffer and the texture request
    // queue (uncached, the queue empty).
    void initUploadBuffers()
    {
        Renderer &renderer = *g_renderer;
        setVblankStartHandler(kVblankStartHandler);
        for (uint32_t frame = 0; frame < 2; ++frame)
        {
            renderer.uploadBuffers[frame] = ee::uncachedMirror(allocateIn(kUploadBufferBytes, kHeapPool, 1));
            const ee::Ptr<uint32_t> queue(ee::uncachedMirror(allocateIn(kRequestQueueBytes, kHeapPool, 1)));
            renderer.requestQueues[frame] = queue;
            *queue = 0;
        }
        renderer.uploadBuffer = 0;
    }
    READABLE_FUNCTION_WAITS(initUploadBuffers, Verified);

    // freeUploadBuffers: the buffers initUploadBuffers made freed (the last:
    // the original's tail call).
    void freeUploadBuffers()
    {
        const Renderer &renderer = *g_renderer;
        freeIn(ee::physical(renderer.uploadBuffers[0]), kHeapPool);
        freeIn(ee::physical(renderer.uploadBuffers[1]), kHeapPool);
        freeIn(ee::physical(renderer.requestQueues[0]), kHeapPool);
        game::tail(freeIn, ee::physical(renderer.requestQueues[1]), kHeapPool); // the original's tail call
    }
    READABLE_FUNCTION_WAITS(freeUploadBuffers, Verified);

    // uploadBuffersHook (the renderer's table; nothing to do).
    void uploadBuffersHook()
    {
    }
    READABLE_FUNCTION(uploadBuffersHook, Verified);

    // onFramePacketSent: the frame packet's DMA completion (dmaSend's callback):
    // marked sent, and with the uploads on, the frame's profile timer stopped
    // (its time kept). 0.
    uint32_t onFramePacketSent()
    {
        *g_framePacketSent = 1;
        if (*g_uploadsOff == 0)
        {
            Renderer &renderer = *g_renderer;
            const uint32_t timer = renderer.profileTimer;
            if (timer == 0)
                return 0;
            renderer.frameDmaTime = stopProfileBar(timer);
        }
        return 0;
    }
    READABLE_FUNCTION(onFramePacketSent, Verified);
}
