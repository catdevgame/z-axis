#pragma once

// Draw submission: how a scene node gets drawn, and the draw records the
// game's submissions leave on the host.
//
// The terrain flush (drawQueuedNodes) calls drawNodeInView(node, cullFlags) for
// every node in a bucket, in order. It keeps the node's lights, builds the
// part's VIF packet on its first draw, swaps the packet's double buffer and
// goes on to submitNodeDraw, which picks the renderer by the part's render
// type. Readable, both are also where a draw record is made: the node, its
// part, the view and the render type in submission order, the input a
// native renderer needs without decoding VIF packets (the strict packet
// path still runs as before; the records are host-side only).

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/render/part.h"
#include "zge/render/renderer.h"
#include "zge/scene/node.h"

#include <array>
#include <cstdint>
#include <vector>
#include "sym/zge/render/draw.h.h"

namespace zge
{
    // ZgePart::typeWord >> 26 (setPartRenderClass sets it from the TGPH flags).
    enum class RenderType : uint32_t
    {
        None = 0,          // resolved at the first draw (resolveRenderType)
        RigidPrelit = 0x04,
        Skinned = 0x08,    // 4-bone skinning, 3 directional lights
        SkinnedPrelit = 0x0C,
        // drawn at constant depth; ZGE2's is 0x14 (DM2's resolveRenderType gives it to a sky node's rigid part, and its
        // part builder sends 4 and 0x14 to buildRigid as AI's sends 4 and 0x18)
        Sky = game::readable::perGame(0x18u, 0x14u, 0x18u),
        Terrain = 0x20,    // normals and grass shells
    };

    inline RenderType renderTypeOf(const ZgePart &part) { return static_cast<RenderType>(part.typeWord >> 26); }

    // The class renderers (node, cullFlags, packet).
    constexpr uint32_t kDrawRigid = sym::drawRigid;
    constexpr uint32_t kDrawTerrain = sym::drawTerrain;
    constexpr uint32_t kDrawSkinned = sym::drawSkinned;
    // The per-view mesh packet (sceVif1Packet) the renderers append to.
    constexpr uint32_t kMeshPacket = kRendererAddress + offsetof(Renderer, meshPacket);

    // The cull result a draw carries (the view cull's, node_tree.cpp): 1 outside the view, 2 across the near
    // plane, 4 across the far, 8 across a side; 0x20 every corner of the box in front of the first world plane.
    enum CullFlags : uint32_t
    {
        kCullOutside = 0x01,
        kCullNear = 0x02,
        kCullFar = 0x04,
        kCullSide = 0x08,
        kCullInFrontOfFirst = 0x20,
    };

    // One node's part drawn in the current view: what a class renderer hands its encoder (the strict one,
    // zge/render/ps2/class_draws.h, writes the mesh packet's VU1 work from it), and what the host's draw list
    // keeps. The class renderers' records below add what each class draws with.
    struct DrawRecord
    {
        ee::Ptr<Node> node;               // its world matrix, lights and bone poses
        ee::Ptr<ZgePart> part;            // the part drawn (its packet already built)
        uint32_t view = 0;                // the view drawn (ZGE2: none, 0)
        uint32_t cullFlags = 0;           // kCull*
        RenderType type = RenderType::None;
        ee::Ptr<MaterialTable> materials; // the material table the node is drawn with

        // Across the near plane, and not every corner in front of the first world plane: drawn with the
        // clipping microprogram (a box crossing the near plane gets kCullNear | kCullSide).
        bool clipped() const { return (cullFlags & (kCullNear | kCullInFrontOfFirst)) == kCullNear; }
        // Across neither the far plane nor a side.
        bool insideFarAndSides() const { return (cullFlags & (kCullFar | kCullSide)) == 0; }
    };

    // The part a node is drawn with: the current view's (ZGE3, ZGE4); ZGE2 keeps one on the node (Node::viewPart)
    // and has no view index. The material table: ZGE4 keeps one a view, ZGE2 and ZGE3 one on the node.
    // (Templates: the layout the version lacks is not instantiated.)
    template <typename N>
    ee::Ptr<ZgePart> drawnPart(const N &node)
    {
        if constexpr (game::readable::kIsDm2)
            return ee::Ptr<ZgePart>(node.viewPart);
        else
            return node.views[*g_currentView].part;
    }
    template <typename N>
    ee::Ptr<MaterialTable> drawnMaterials(const N &node)
    {
        if constexpr (game::readable::kIsBmx)
            return node.views[*g_currentView].materials;
        else
            return node.materials;
    }
    template <typename N>
    uint32_t drawnView([[maybe_unused]] const N &node)
    {
        if constexpr (game::readable::kIsDm2)
            return 0;
        else
            return *g_currentView;
    }

    // A node's draw record in the current view, of a class renderer drawing it with `type`.
    inline DrawRecord drawRecordOf(ee::Ptr<Node> node, uint32_t cullFlags, RenderType type)
    {
        return {node, drawnPart(*node), drawnView(*node), cullFlags, type, drawnMaterials(*node)};
    }
    // The same, the render type read from the part.
    inline DrawRecord drawRecordOf(ee::Ptr<Node> node, uint32_t cullFlags)
    {
        DrawRecord record = drawRecordOf(node, cullFlags, RenderType::None);
        record.type = renderTypeOf(*record.part);
        return record;
    }

    // The lights a class renderer sends: three directions (a matrix's columns) and their colours with the ambient
    // (rows 0-2 and 3), as a light block or the renderer's light matrices hold them; the directions as they are,
    // or turned by the node's world first.
    struct DrawLights
    {
        ee::Ptr<Matrix> directions;
        ee::Ptr<Matrix> colours;
        bool turnedByWorld = false;
    };

    // drawRigid's: ZGE4 sends a node lit by its own lights the first draw bucket's default lights, turned by the
    // node's world (none: no lights).
    struct RigidDraw
    {
        DrawRecord record;
        DrawLights lights;
    };
    // drawTerrain's: the grass shells' depth, the node's view depth less its radius.
    struct TerrainDraw
    {
        DrawRecord record;
        ee::Float shellDepth;
    };
    // drawSkinned's: the view's world -> screen and world -> clip matrices, the lights, and the bones (the part's
    // list, each driven by one of the node's poses).
    struct SkinnedDraw
    {
        DrawRecord record;
        ee::Ptr<Matrix> screen, clip;
        DrawLights lights;
    };

    // The records of the frame being drawn (and the last one complete).
    const std::vector<DrawRecord> &currentDrawRecords();
    const std::vector<DrawRecord> &lastFrameDrawRecords();

    // drawNodeInView: draws a node in the current view.
    int32_t drawNodeInView(ee::Ptr<Node> node, uint32_t cullFlags);

    // submitNodeDraw: the renderer of the node's part, by render type.
    int32_t submitNodeDraw(ee::Ptr<Node> node, uint32_t cullFlags);

    // One queued draw: the node in its view's cull flags; `draw` (when set) is
    // called in place of drawNodeInView.
    struct BucketDraw
    {
        uint32_t cullFlags;
        ee::Ptr<Node> node;
        ee::Ptr<ZgePart> part; // the shadow bucket's (4): the caster's part (queuePartDraw); 0 in the others
        uint32_t draw;         // (node, cullFlags)
    };
    static_assert(sizeof(BucketDraw) == 0x10);

    // A bucket of queued draws. ZGE2's (DM2's initDrawBuckets, freeDrawBuckets and drawQueuedNodes) is four words with the
    // end before the limit and one hook, the draw; ZGE3 added the reset and add hooks (bucket 2's sorted add) and put the
    // limit before the end.
#if PS2X_GAME_DM2
    struct DrawBucket
    {
        ee::Ptr<BucketDraw> begin, end, limit;
        uint32_t draw;  // (bucket)

        bool hasRoom() const { return before(end, limit); }
    };
    static_assert(sizeof(DrawBucket) == 0x10 && offsetof(DrawBucket, end) == 4 && offsetof(DrawBucket, draw) == 0xC);
    inline uint32_t bucketResetHook(const DrawBucket &) { return 0; }
    inline uint32_t bucketAddHook(const DrawBucket &) { return 0; }
    inline void setSortedBucketHooks(DrawBucket &, uint32_t, uint32_t) {}
#else
    struct DrawBucket
    {
        ee::Ptr<BucketDraw> begin, limit, end;
        uint32_t reset; // (bucket)
        uint32_t add;   // (bucket, node, cullFlags)
        uint32_t draw;  // (bucket)

        bool hasRoom() const { return before(end, limit); }
    };
    static_assert(sizeof(DrawBucket) == 0x18);
    // The hooks a bucket may have besides its draw: a reset, and an add (bucket, node, cullFlags) that takes the draw in.
    inline uint32_t bucketResetHook(const DrawBucket &bucket) { return bucket.reset; }
    inline uint32_t bucketAddHook(const DrawBucket &bucket) { return bucket.add; }
    inline void setSortedBucketHooks(DrawBucket &bucket, uint32_t add, uint32_t draw) { bucket.add = add; bucket.draw = draw; }
#endif

    constexpr uint32_t kDrawBucketCount = 7;
    inline ee::Global<std::array<DrawBucket, kDrawBucketCount>, sym::drawBuckets> g_drawBuckets;
    inline DrawBucket &bucketAt(uint32_t index) { return ee::Ptr<DrawBucket>(g_drawBuckets->data())[index]; }

    // The draw buckets (draw.cpp): initDrawBuckets, freeDrawBuckets their draws
    // allocated and freed (always 1); queuePartDraw a shadow caster's part
    // queued in bucket 4; nodeDrawBucket a node's bucket chosen and recorded
    // in its draw flags; setNodeSortedBucket / setNodeSortedBucketAgain / clearNodeDrawBucket (no
    // caller) bucket 2 set, the bucket cleared; updateMovedNodeVisitor a scene walk's
    // visitor bringing a moved node up to date.
    uint32_t initDrawBuckets();
    uint32_t freeDrawBuckets();
    uint32_t queuePartDraw(ee::Ptr<Node> node, ee::Ptr<ZgePart> part);
    // resetDrawBuckets every bucket emptied; drawQueuedNodes what the buckets hold drawn.
    void resetDrawBuckets();
    void drawQueuedNodes();

    // The area the camera is in, kept where the engine version keeps it for the scene draw: ZGE2 in the world
    // (CollisionWorld::area, which its listed-node draw reads); ZGE3 and ZGE4 in drawScene's frame (nothing to keep).
    // (A template: the world's type is zge/collision/collision.h's, complete where a scene draw calls this.)
    template <typename World>
    inline void keepCameraAreaInWorld([[maybe_unused]] World &world, [[maybe_unused]] uint32_t area)
    {
#if PS2X_GAME_DM2
        world.area = area;
#endif
    }
    template <typename World>
    inline uint32_t cameraAreaInWorld([[maybe_unused]] const World &world)
    {
#if PS2X_GAME_DM2
        return world.area;
#else
        return 0;
#endif
    }
    uint32_t nodeDrawBucket(ee::Ptr<Node> node);
    // ZGE2 only: a node's tree queued in the draw buckets, unculled (the second word is not read).
    void queueNodeTree(ee::Ptr<Node> node, uint32_t unread, uint32_t cullFlags);
    uint64_t setNodeSortedBucket(ee::Ptr<Node> node);
    uint64_t setNodeSortedBucketAgain(ee::Ptr<Node> node);
    uint64_t clearNodeDrawBucket(ee::Ptr<Node> node);
    uint64_t setNodeBucket5(ee::Ptr<Node> node); // (ZGE2 only)
    uint64_t setNodeBucket6(ee::Ptr<Node> node); // (ZGE2 only)
    uint32_t updateMovedNodeVisitor(ee::Ptr<Node> node);

    // node_tree.cpp: loadViewCulling the view's culling into VU0 (always 1);
    // drawNodeTree a node's tree culled and queued (the sky's; the second
    // argument is not read).
    struct Camera;
    uint32_t loadViewCulling(ee::Ptr<Camera> camera);
    void drawNodeTree(ee::Ptr<Node> root, uint32_t, uint32_t cullFlags);
    // drawScene: the view's scene culled, queued and drawn. Always 1.
    uint32_t drawScene();
}

namespace zge
{
    // (class_renderers.cpp)
    void drawRigid(ee::Ptr<Node> node, uint32_t cullFlags, sce::Vif1Packet *packet);
    void drawTerrain(ee::Ptr<Node> node, uint32_t cullFlags, sce::Vif1Packet *packet);
    void drawSkinned(ee::Ptr<Node> node, uint32_t cullFlags, sce::Vif1Packet *packet);
}
