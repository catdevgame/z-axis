#include "draw.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "sce/packet_calls.h"
#include "zge/render/material.h"
#include "zge/render/renderer.h"
#include "zge/native/geo_meshes.h"
#include "zge/native/host.h"
#include "zge/render/packet.h"
#include "zge/render/part.h"
#include "zge/render/shadow.h"
#include "zge/host/class_draws.h"
#include "zge/memory/regions.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include "sym/zge/render/draw.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kClosePacket = sym::vif1PacketAlign;          // libvifpk (packet, 2, 0)

        struct Records
        {
            uint64_t tick = ~0ull;
            std::vector<DrawRecord> current, last;
            int trace = -1;
        };
        Records g_records;

        // A draw record for the frame being drawn (one list per vsync).
        void record(ee::Ptr<Node> node, uint32_t cullFlags)
        {
            Records &r = g_records;
            if (r.trace < 0)
            {
                const char *v = std::getenv("PS2X_TRACE_DRAW_RECORDS");
                r.trace = v ? std::atoi(v) : 0;
            }
            const uint64_t tick = native::vsyncTick();
            if (tick != r.tick)
            {
                r.last.swap(r.current);
                r.current.clear();
                if (r.trace && r.tick != ~0ull && r.tick % 60u == 0u)
                {
                    std::map<RenderType, size_t> byType;
                    for (const DrawRecord &d : r.last)
                        ++byType[d.type];
                    std::cout << "[draw-records] vsync " << r.tick << ": " << r.last.size() << " draws";
                    for (const auto &[type, count] : byType)
                        std::cout << ", type 0x" << std::hex << static_cast<uint32_t>(type) << std::dec << " " << count;
                    std::cout << std::endl;
                }
                r.tick = tick;
            }
            r.current.push_back(drawRecordOf(node, cullFlags));
        }
    }

    const std::vector<DrawRecord> &currentDrawRecords() { return g_records.current; }
    const std::vector<DrawRecord> &lastFrameDrawRecords() { return g_records.last; }

    int32_t drawNodeInView(ee::Ptr<Node> node, uint32_t cullFlags)
    {
        const ee::Ptr<ZgePart> partAddress = node->views[*g_currentView].part;
        ZgePart &part = *partAddress;

        // Lights changed: a node with its own lights gets them redone if its
        // part is lit by normals, else it goes back to the scene's lights.
        if (node->flags & kNodeLightsChanged)
        {
            node->flags &= ~kNodeLightsChanged;
            if (node->flags & kNodeOwnLights)
            {
                if (part.flags & kPartLitByNormals)
                    updateNodeLights(node);
                else
                    node->flags &= ~kNodeOwnLights;
            }
            else if constexpr (game::readable::kIsBmx) // (ZGE4: a part lit by normals gives the node lights of its own)
            {
                if (part.flags & kPartLitByNormals)
                    node->flags |= kNodeOwnLights;
            }
        }

        // The VIF packet is built on the part's first draw, and its dirty
        // streams (the low flag bits) rebuilt on later ones.
        bool built = false;
        if (part.flags & kPartNew)
        {
            resolveRenderType(node, partAddress);
            built = true;
            buildPartPacket(partAddress, 1, 0x7FF);
            buildMaterialChain(drawnMaterials(*node), partAddress);
            host::notePartPacketBuilt(partAddress, true, 0x7FF);
        }
        if (part.flags & 7u)
        {
            built = true;
            const uint32_t streams = part.flags;
            buildPartPacket(partAddress, 0, streams);
            buildMaterialChain(drawnMaterials(*node), partAddress);
            host::notePartPacketBuilt(partAddress, false, streams);
        }
        if (built)
        {
            // Double buffer: the one just built is drawn (bit 1 = the old
            // bit 0), the other is next to build (bit 0 flipped).
            uint32_t buffers = part.bufferFlags;
            buffers = (buffers & ~2u) | ((buffers & 1u) << 1);
            buffers = (buffers & ~1u) | ((buffers & 1u) ^ 1u);
            part.bufferFlags = buffers;
        }
        return game::tail(zge::submitNodeDraw, node, cullFlags);
    }

    namespace
    {
        // ZGE2's node flag drawNodeViewPart sets once the node's part has had its first build (the upper word's bit
        // 12; what reads it is not known yet).
        constexpr uint64_t kNodePartBuiltBit = 1ull << 44;
    }

    // drawNodeViewPart: ZGE2's drawNodeInView: the node's one drawn part
    // (Node::viewPart), the bones' posed mark taken off (ZGE2 relights
    // nothing here); on the part's first draw its type resolved, its packet
    // built whole and its material chain made, and the node marked; its
    // dirty streams rebuilt; the double buffer flipped when either was
    // built; then the draw submitted (the original's tail call).
    int32_t drawNodeViewPart(ee::Ptr<Node> node, uint32_t cullFlags)
    {
        uint64_t &flags = node->flagsDoubleword();
        bool built = false;
        if (flags & kNodeBonesPosedBit)
            flags &= ~kNodeBonesPosedBit;
        const ee::Ptr<ZgePart> partAddress = nodeDrawnPart(*node);
        ZgePart &part = *partAddress;
        if (part.flags & kPartNew)
        {
            resolveRenderType(node, partAddress);
            buildPartPacketAndTable(node, partAddress, 1, 0x7FF);
            buildNodeMaterialChain(node);
            host::notePartPacketBuilt(partAddress, true, 0x7FF);
            node->flagsDoubleword() |= kNodePartBuiltBit;
            built = true;
        }
        if (part.flags & 7u)
        {
            const uint32_t streams = part.flags;
            buildPartPacketAndTable(node, partAddress, 0, streams);
            buildNodeMaterialChain(node);
            host::notePartPacketBuilt(partAddress, false, streams);
            built = true;
        }
        if (built)
        {
            uint32_t buffers = part.bufferFlags;
            buffers = (buffers & ~2u) | ((buffers & 1u) << 1);
            buffers = (buffers & ~1u) | ((buffers & 1u) ^ 1u);
            part.bufferFlags = buffers;
        }
        return game::tail(zge::submitNodeDraw, node, cullFlags);
    }
    READABLE_FUNCTION_WAITS_OWN(drawNodeInView, Verified, dm2, drawNodeViewPart);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t vif1PacketAlign(uint32_t packet, uint32_t bit, uint32_t pos) { return ee::tailCall<int32_t>(kClosePacket, packet, bit, pos); }
        }
    }

    namespace
    {
        void checkMeshOnce(const Node &node, uint32_t view, RenderType type)
        {
            ps2x::zaxis::PacketKind kind;
            if (host::packetKindOf(*node.views[view].part, type, kind))
                native::checkPartMeshOnce(native::gameRam(), node.views[view].part, static_cast<uint32_t>(type), kind,
                                          host::skinnedDrawForm());
        }
    }

    int32_t submitNodeDraw(ee::Ptr<Node> node, uint32_t cullFlags)
    {
        const uint32_t view = *g_currentView;
        const ZgePart &part = *node->views[view].part;
        const RenderType type = renderTypeOf(part);
        if (native::hostExtrasOn())
        {
            record(node, cullFlags);
            // The mesh made from the GEO file against the packet drawn, once
            // per part (PS2X_NATIVE_MESHES_CHECK).
            if (native::meshCheckEnabled())
                checkMeshOnce(*node, view, type);
        }
        switch (type)
        {
        case RenderType::RigidPrelit:
        case RenderType::Sky:
            drawRigid(node, cullFlags, &g_renderer->meshPacket);
            break;
        case RenderType::Terrain:
            drawTerrain(node, cullFlags, &g_renderer->meshPacket);
            break;
        case RenderType::Skinned:
        case RenderType::SkinnedPrelit:
            drawSkinned(node, cullFlags, &g_renderer->meshPacket);
            break;
        default:
            break;
        }
        return tail::vif1PacketAlign(kMeshPacket, 2, 0);
    }

    // submitViewPartDraw: ZGE2's submitNodeDraw: the node's one drawn part
    // (Node::viewPart) drawn by its class renderer, skinned or rigid (and
    // sky); ZGE2 has no terrain or prelit skinned class. Then the mesh
    // packet aligned (the original's tail call).
    int32_t submitViewPartDraw(ee::Ptr<Node> node, uint32_t cullFlags)
    {
        const RenderType type = renderTypeOf(*nodeDrawnPart(*node));
        if (type == RenderType::Skinned)
            drawSkinned(node, cullFlags, &g_renderer->meshPacket);
        else if (type == RenderType::RigidPrelit || type == RenderType::Sky)
            drawRigid(node, cullFlags, &g_renderer->meshPacket);
        return tail::vif1PacketAlign(kMeshPacket, 2, 0);
    }
    READABLE_FUNCTION_WAITS_OWN(submitNodeDraw, Verified, dm2, submitViewPartDraw);

    // ---- the draw buckets ---------------------------------------------------
    //
    // The nodes a frame draws are put in seven buckets (nodeDrawBucket picks
    // a node's: 0 opaque, 1 kNodeSecondBucket, 2, 3 and 6 by its blended
    // materials; bucket 4 holds the shadow casters, queuePartDraw's), drawn
    // in bucket order once the scene is walked. A bucket may hook its reset,
    // its add and its draw (bucket 2: addToDrawBucket / drawBucket, bucket
    // 4's draw drawShadowCasters, set up by initDrawBuckets); without a hook
    // a bucket is a plain array of draws.
    namespace
    {
        // A draw's hooks, as a bucket holds them.
        void runDrawHook(uint32_t hook, ee::Ptr<Node> node, uint32_t cullFlags) { ee::call(hook, node, cullFlags); }
        void runBucketHook(uint32_t hook, DrawBucket &bucket) { ee::call(hook, ee::Ptr<DrawBucket>(&bucket)); }

        void drawBucketDraws(ee::Ptr<BucketDraw> draw, ee::Ptr<BucketDraw> end)
        {
            do
            {
                if (draw->draw != 0)
                    runDrawHook(draw->draw, draw->node, draw->cullFlags);
                else
                    drawNodeInView(draw->node, draw->cullFlags);
                ++draw;
            } while (!same(draw, end));
        }
    }

    // addToDrawBucket: bucket 2's add, a draw at the end while there is room.
    void addToDrawBucket(DrawBucket *bucket, ee::Ptr<Node> node, uint32_t cullFlags)
    {
        if (bucket->hasRoom())
        {
            BucketDraw *draw = bucket->end;
            draw->node = node;
            draw->cullFlags = cullFlags;
            draw->part = ee::Ptr<ZgePart>();
            draw->draw = 0;
            bucket->end = bucket->end + 1;
        }
    }
    READABLE_FUNCTION(addToDrawBucket, Verified);

    // drawBucket: bucket 2's draw, its draws in order.
    void drawBucket(DrawBucket *bucket)
    {
        if (!same(bucket->begin, bucket->end))
            drawBucketDraws(bucket->begin, bucket->end);
    }
    READABLE_FUNCTION_WAITS(drawBucket, Verified);

    // resetDrawBuckets: every bucket emptied (or its reset hook called), for
    // the next frame.
    void resetDrawBuckets()
    {
        for (DrawBucket &bucket : *g_drawBuckets)
        {
            if (bucketResetHook(bucket))
                runBucketHook(bucketResetHook(bucket), bucket);
            else
                bucket.end = bucket.begin;
        }
    }
    READABLE_FUNCTION_WAITS(resetDrawBuckets, Verified);

    // drawQueuedNodes: the frame's queued draws, bucket by bucket.
    void drawQueuedNodes()
    {
        for (DrawBucket &bucket : *g_drawBuckets)
        {
            if (bucket.draw)
                runBucketHook(bucket.draw, bucket);
            else if (!same(bucket.begin, bucket.end))
                drawBucketDraws(bucket.begin, bucket.end);
        }
    }
    READABLE_FUNCTION_WAITS(drawQueuedNodes, Verified);
    // ---- the buckets' set-up and the shadow casters -------------------------

    namespace
    {
        constexpr uint32_t kPartInView = sym::partInView;   // (camera, sphere): bit 0 set when outside the view
        // The shadow driver (node, part): a caster's volume chain, 0 when none.
        constexpr uint32_t kPacketCall = sym::vif1PacketCall; // sceVif1PkCall (packet, chain, 0)

        // The buckets' capacities in draws (bucketCapacities: 512, 128, 384, 384,
        // 64, 64, 128).
        inline ee::Global<std::array<int32_t, kDrawBucketCount>, sym::bucketCapacities> g_bucketCapacities;
        inline ee::Global<uint32_t, sym::g_drawBucketsReady> g_drawBucketsReady;
        // Bucket 4: the frame's shadow casters (queuePartDraw adds them, its
        // draw hook drawShadowCasters draws their volumes). ZGE2's is bucket 3.
        constexpr uint32_t kShadowBucket = game::readable::perGame(4u, 3u, 4u);
        constexpr uint32_t kSortedBucket = 2; // the bucket with an add hook (addToDrawBucket / drawBucket)

        // Node::drawFlags bits 8-11: the node's bucket + 1 (0: not chosen
        // yet, nodeDrawBucket chooses it at the next draw). ZGE2 keeps it in
        // bits 12-15 (its bits 8-11 are the level of detail: queueNodeTree).
        constexpr uint32_t kDrawBucketShift = game::readable::perGame(8u, 12u, 8u);
        constexpr uint64_t kDrawBucketBits = 0xFull << kDrawBucketShift;
        // Node::drawFlags bit 21: a shadow caster tested against the view
        // at its next queueing (cleared then).
        constexpr uint64_t kShadowViewTest = 1u << 21;
        // The material type (Material::type bits 1-8) that puts the node in bucket 6.
        constexpr uint32_t kMaterialTypeBucket6 = 11;

        // The draw flags and the word after them, as the originals read and write them.
        uint64_t &drawFlagsWord(Node &node) { return *reinterpret_cast<uint64_t *>(&node.drawFlags); }
        // The node's material table a bucket is chosen by: ZGE4 keeps one per view (NodeView::materials, the view
        // drawn), ZGE2 and ZGE3 one on the node. (A template: the layout the version lacks is not instantiated.)
        template <typename N>
        ee::Ptr<MaterialTable> bucketMaterials(const ee::Ptr<N> &node)
        {
            if constexpr (game::readable::kIsBmx)
                return node->views[*g_currentView].materials;
            else
                return node->materials;
        }
        // The node's colour as one word: 0 is black, and casts no shadow.
        uint32_t colourWord(const Node &node) { return *reinterpret_cast<const uint32_t *>(node.colour); }
    }

    // initDrawBuckets (initTerrain): every bucket's draws allocated from the
    // heap by its capacity, bucket 2's add and draw hooks (not in ZGE2, which
    // has no sorted bucket) and the shadow bucket's draw hook set. Always 1.
    uint32_t initDrawBuckets()
    {
        *g_drawBucketsReady = 1;
        for (uint32_t index = 0; index < kDrawBucketCount; ++index)
        {
            const int32_t capacity = (*g_bucketCapacities)[index];
            const ee::Ptr<BucketDraw> draws(heapAllocate(static_cast<uint32_t>(capacity * sizeof(BucketDraw))));
            DrawBucket &bucket = bucketAt(index);
            bucket.begin = draws;
            bucket.limit = draws + capacity;
        }
        bucketAt(kShadowBucket).draw = sym::drawShadowCasters;
        if constexpr (!game::readable::kIsDm2)
            setSortedBucketHooks(bucketAt(kSortedBucket), sym::addToDrawBucket, sym::drawBucket);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initDrawBuckets, Verified);

    // freeDrawBuckets (shutdownTerrain): the buckets' draws freed, once
    // allocated. Always 1.
    uint32_t freeDrawBuckets()
    {
        if (*g_drawBucketsReady == 0)
            return 1;
        for (const DrawBucket &bucket : *g_drawBuckets)
            heapFree(bucket.begin);
        *g_drawBucketsReady = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeDrawBuckets, Verified);

    // emptyDrawBucket (no caller in the game): a bucket emptied (its end back
    // to its start; returns the start).
    ee::Ptr<BucketDraw> emptyDrawBucket(DrawBucket *bucket)
    {
        bucket->end = bucket->begin;
        return bucket->begin;
    }
    READABLE_FUNCTION(emptyDrawBucket, Draft);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t partInView(ee::Ptr<Camera> camera, const Vec4 &sphere) { return ee::game<uint32_t>(kPartInView, camera, &sphere); }
    }

    // queuePartDraw (the node draws drawScene and drawNodeTree, per node
    // and part): a shadow caster's part queued in bucket 4: only a node with
    // kNodeCastsShadow and a non-zero +0x180 word, in the view when asked
    // (kShadowViewTest); the part's shadow edges built at its first
    // queueing. 1 when it was queued or the bucket is full, else 0.
    uint32_t queuePartDraw(ee::Ptr<Node> node, ee::Ptr<ZgePart> part)
    {
        const uint64_t flags = drawFlagsWord(*node);
        if ((flags & kNodeCastsShadow) == 0)
            return 0;
        if (colourWord(*node) == 0)
            return 0;
        const ee::Ptr<Camera> camera(*g_currentCamera);
        if (flags & kShadowViewTest)
        {
            drawFlagsWord(*node) = flags & ~kShadowViewTest;
            if (partInView(camera, node->shadowViewSphere) & 1u)
                return 0;
        }
        if (!part->shadowEdges)
            buildPartEdges(part);
        DrawBucket &casters = bucketAt(kShadowBucket);
        if (casters.hasRoom())
        {
            BucketDraw *draw = casters.end;
            draw->part = part;
            draw->node = node;
            draw->draw = 0;
            casters.end = casters.end + 1;
        }
        return 1;
    }

    namespace
    {
        // ZGE2's word that lets parts be queued for drawing (Renderer::drawingOn, set by initRenderer); no other
        // version has one. (A template: the field the version lacks is not named.)
        template <typename R>
        bool drawingOn([[maybe_unused]] const R &renderer)
        {
            if constexpr (requires { renderer.drawingOn; })
                return renderer.drawingOn != 0;
            else
                return true;
        }
    }

    // queueShadowCaster: ZGE2's queuePartDraw: nothing while the renderer's
    // drawing word is 0; then as ZGE3's (a shadow caster with a colour, in
    // the view when asked, its edges built at its first queueing), with
    // ZGE2's view test word (Node::shadowViewTest), a skinned part posed for
    // the shadow (posePart, flags 3) and the part queued in bucket 4 with no
    // test for room. (Its caller reads no result.)
    void queueShadowCaster(ee::Ptr<Node> node, ee::Ptr<ZgePart> part)
    {
        if (!drawingOn(*g_renderer))
            return;
        if ((drawFlagsWord(*node) & kNodeCastsShadow) == 0)
            return;
        if (colourWord(*node) == 0)
            return;
        if (node->shadowViewTestRequest() & 1u)
        {
            node->shadowViewTestRequest() &= ~1u;
            if (partInView(ee::Ptr<Camera>(*g_currentCamera), node->shadowViewSphere) & 1u)
                return;
        }
        if (!part->shadowEdges)
            buildPartEdges(part);
        if (part->boneCount != 0)
            posePart(part, node->bonePoses.cast<BonePose>(), 3);
        DrawBucket &casters = bucketAt(kShadowBucket);
        BucketDraw *draw = casters.end;
        casters.end = casters.end + 1;
        draw->part = part;
        draw->node = node;
        draw->draw = 0;
    }
    READABLE_FUNCTION_WAITS_OWN(queuePartDraw, Verified, dm2, queueShadowCaster);

    // queueNodeTree (ZGE2 only; its listed-node draw's last step): the node, its children (each subtree in turn) and
    // its next siblings queued in the draw buckets with the cull flags given, none of them culled; a chain of
    // siblings ends at a node with no drawn part. A node's bucket is the one its draw flags keep, else chosen (1 for
    // kNodeSecondBucket, 2 when its materials blend, else 0) and kept. A node whose drawn part's next part is skinned
    // has that part queued as a shadow caster (queuePartDraw, whatever it returns) and is drawn from the bucket after
    // the shadow one. ZGE2 does not look at a bucket's room. The second word is not read (the caller passes -1), as
    // ZGE3's drawNodeTree's, which culls each node and walks the tree with a stack.
    inline void queueNodeTree(ee::Ptr<Node> nodeAddress, uint32_t unread, uint32_t cullFlags)
    {
        constexpr uint32_t kSecondBucket = 1, kBlendedBucket = 2, kOpaqueBucket = 0;
        constexpr uint32_t kCasterBucket = kShadowBucket + 1;
        for (; nodeAddress; nodeAddress = nodeAddress->nextSibling)
        {
            Node &node = *nodeAddress;
            const ee::Ptr<ZgePart> part = nodeDrawnPart(node);
            if (!part)
                return;
            int32_t bucket = static_cast<int32_t>((drawFlagsWord(node) & kDrawBucketBits) >> kDrawBucketShift) - 1;
            if (bucket < 0)
            {
                if ((nodeTreeFlags(node) & kNodeSecondBucket) != 0)
                    bucket = kSecondBucket;
                else
                    bucket = materialTableIsBlended(node.materials) != 0 ? kBlendedBucket : kOpaqueBucket;
                drawFlagsWord(node) = (drawFlagsWord(node) & ~kDrawBucketBits) | (static_cast<uint64_t>(bucket + 1) << kDrawBucketShift & kDrawBucketBits);
            }
            const ee::Ptr<ZgePart> shadowPart = part->next;
            if (shadowPart && (shadowPart->flags & kPartSkinned) != 0)
            {
                queuePartDraw(nodeAddress, shadowPart);
                bucket = kCasterBucket;
            }
            DrawBucket &target = bucketAt(static_cast<uint32_t>(bucket));
            BucketDraw &draw = *target.end;
            target.end = target.end + 1;
            draw.part = 0;
            draw.node = nodeAddress;
            draw.cullFlags = cullFlags;
            draw.draw = 0;
            if (node.firstChild)
                queueNodeTree(node.firstChild, unread, cullFlags);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(dm2, queueNodeTree);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void vif1PacketCall(ee::Ptr<sce::Vif1Packet> packet, ee::Ptr<uint32_t> chain, uint32_t flags) { ee::tailCall(kPacketCall, packet, chain, flags); }
        }
    }

    // drawShadowCasters (bucket 4's draw hook): the shadow casters' volumes into
    // the view's mesh packet as calls: the shadow pass begun before the
    // first, each caster's volume (the shadow driver), the shadow buffer
    // applied after the last (both only when shadows are on).
    void drawShadowCasters(DrawBucket *bucket)
    {
        ee::Ptr<BucketDraw> draw = bucket->begin;
        const ee::Ptr<BucketDraw> end = bucket->end;
        bool first = true;
        // (ZGE2 takes the mesh packet's address itself and leaves the VU1
        // state's owner as it is)
        const ee::Ptr<sce::Vif1Packet> packet = game::readable::kIsDm2 ? ee::Ptr<sce::Vif1Packet>(&g_renderer->meshPacket)
                                                                        : ee::Ptr<sce::Vif1Packet>(meshPacket());
        if constexpr (!game::readable::kIsDm2)
            setVu1StateOwner(0);
        for (; !same(draw, end); ++draw)
        {
            if (first)
            {
                first = false;
                if (const ee::Ptr<uint32_t> chain = beginShadowPassIfEnabled())
                    sce::vif1PacketCall(packet, chain, 0);
            }
            if (const ee::Ptr<uint32_t> chain = buildShadowVolume(draw->node, draw->part))
                sce::vif1PacketCall(packet, chain, 0);
        }
        if (!first)
        {
            if (const ee::Ptr<uint32_t> chain = applyShadowBufferIfEnabled())
                return tail::vif1PacketCall(packet, chain, 0);
        }
    }
    READABLE_FUNCTION(drawShadowCasters, Verified);

    // nodeDrawBucket (the node draws, for a node without a bucket): the
    // node's bucket, recorded in its draw flags: 1 for a kNodeSecondBucket
    // node; else by its materials: 0 when none blends, 2 when the table is
    // flagged kMaterialTableBlended, 6 with a material of type 11, else 3.
    uint32_t nodeDrawBucket(ee::Ptr<Node> node)
    {
        uint32_t bucket = 1;
        if ((node->flags & kNodeSecondBucket) == 0)
        {
            const ee::Ptr<MaterialTable> table = bucketMaterials(node);
            if (materialTableIsBlended(table) == 0)
                bucket = 0;
            else
            {
                bucket = 2;
                if ((table->flags & kMaterialTableBlended) == 0)
                {
                    const int32_t count = materialCount(table);
                    bucket = 3;
                    // The materials from the last; one of type 11 makes it 6.
                    for (int32_t index = count - 1; index != -1; --index)
                    {
                        if (((materialAt(table, index)->type >> 1) & 0xFFu) == kMaterialTypeBucket6)
                        {
                            bucket = 6;
                            break;
                        }
                    }
                }
            }
        }
        drawFlagsWord(*node) = (drawFlagsWord(*node) & ~kDrawBucketBits) | ((static_cast<uint64_t>(bucket) + 1u) << kDrawBucketShift);
        return bucket;
    }
    READABLE_FUNCTION(nodeDrawBucket, Verified);

    // setNodeSortedBucket, setNodeSortedBucketAgain (twins, no caller in the game): the node
    // put in bucket 2 (draw flags' bucket 2 + 1); clearNodeDrawBucket: its bucket
    // cleared, chosen again at its next draw. Each returns the draw-flags
    // doubleword.
    uint64_t setNodeSortedBucket(ee::Ptr<Node> node)
    {
        const uint64_t flags = (drawFlagsWord(*node) & ~kDrawBucketBits) | ((kSortedBucket + 1ull) << kDrawBucketShift);
        drawFlagsWord(*node) = flags;
        return flags;
    }
    READABLE_FUNCTION(setNodeSortedBucket, Draft);

    uint64_t setNodeSortedBucketAgain(ee::Ptr<Node> node)
    {
        const uint64_t flags = (drawFlagsWord(*node) & ~kDrawBucketBits) | ((kSortedBucket + 1ull) << kDrawBucketShift);
        drawFlagsWord(*node) = flags;
        return flags;
    }
    READABLE_FUNCTION(setNodeSortedBucketAgain, Draft);

    uint64_t clearNodeDrawBucket(ee::Ptr<Node> node)
    {
        const uint64_t flags = drawFlagsWord(*node) & ~kDrawBucketBits;
        drawFlagsWord(*node) = flags;
        return flags;
    }
    READABLE_FUNCTION(clearNodeDrawBucket, Draft);

    // setNodeBucket5, setNodeBucket6 (ZGE2 only, after drawShadowCasters; ZGE2's third twin puts the node in
    // bucket 2 and has no caller): the node put in bucket 5 (6), whatever its class says (draw flags' bucket +
    // 1). Each returns the draw-flags doubleword. DM2's tree fade puts a faded tree in bucket 6.
    inline uint64_t setNodeBucket5(ee::Ptr<Node> node)
    {
        const uint64_t flags = (drawFlagsWord(*node) & ~kDrawBucketBits) | ((5ull + 1ull) << kDrawBucketShift);
        drawFlagsWord(*node) = flags;
        return flags;
    }
    READABLE_FUNCTION_ONLY(dm2, setNodeBucket5);

    inline uint64_t setNodeBucket6(ee::Ptr<Node> node)
    {
        const uint64_t flags = (drawFlagsWord(*node) & ~kDrawBucketBits) | ((6ull + 1ull) << kDrawBucketShift);
        drawFlagsWord(*node) = flags;
        return flags;
    }
    READABLE_FUNCTION_ONLY(dm2, setNodeBucket6);

    namespace
    {
        // A flag no code reads, set by setUnreadDrawFlag and cleared by
        // clearUnreadDrawFlag (neither has a caller in the game).
        inline ee::Global<uint32_t, sym::g_unreadDrawFlag> g_unreadDrawFlag;
    }

    // setUnreadDrawFlag: the flag set. Always 1.
    uint32_t setUnreadDrawFlag()
    {
        *g_unreadDrawFlag = 1;
        return 1;
    }
    READABLE_FUNCTION(setUnreadDrawFlag, Verified);

    // clearUnreadDrawFlag: the flag cleared (when set). Always 1.
    uint32_t clearUnreadDrawFlag()
    {
        if (*g_unreadDrawFlag != 0)
            *g_unreadDrawFlag = 0;
        return 1;
    }
    READABLE_FUNCTION(clearUnreadDrawFlag, Draft);

    // updateMovedNodeVisitor: a scene walk's visitor: the node's matrices and bounds
    // brought up to date (updateMovedNode). Always 1 (go on).
    uint32_t updateMovedNodeVisitor(ee::Ptr<Node> node)
    {
        updateMovedNode(node);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateMovedNodeVisitor, Verified);

}
