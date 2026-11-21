#pragma once

// The scene node: a placed object (level mesh, skater, NPC, prop), as the
// engine's culling and the per-class renderers read it
// (game_overrides/aggressive_inline_native.cpp).
// Offsets are verified where the renderers use them; the rest is unknown
// and left as padding.

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/math/box.h"
#include "zge/assets/geo.h"
#include "zge/scene/light.h"

#include <cstddef>
#include "sym/zge/scene/node.h.h"

namespace zge
{
    struct ZgePart;       // a mesh (TGPT at run time): zge/render/part.h
    struct MaterialTable; // the node's materials: zge/render/material.h
    struct BonePose; // a skinned model's bone matrices: zge/scene/skeleton.h
    struct CollisionFrame; // the geometry frame: levels of detail and collision (zge/collision/collision.h)

    // The node's flags word (read as 64 bits by some code; only the low
    // ones are known).
    enum NodeFlags : uint32_t
    {
        kNodeShown = 1u << 0,            // drawn (setNodeShown; the script's visibility command sets it)
        kNodeCollides = 1u << 1,         // sphere queries test it (sphereQueryTestNode)
        // Its model's bones are nodes: this one and those below it with the
        // flag (setupSceneModel makes a scene bone of each).
        kNodeBoneTree = 1u << 2,
        kNodeSecondBucket = 1u << 3,     // drawn in bucket 1 (after the opaque one)
        kNodeSky = 1u << 4,              // its rigid parts are drawn as the sky (resolveRenderType: RenderType::Sky)
        // The node stands for its whole subtree: a query tests it alone, a draw
        // takes the tree together (the scene draw culls here; drawNodeTree
        // culls nothing below it; kNodeNotDrawn reads the tree's root's view).
        kNodeTreeIsOneUnit = 1u << 6,
        kNodeOwnLights = 1u << 7,        // lit by its own light block (+0x160), else the global lights
        // Set on a skinned scene's node when its bones were posed
        // (markBonesPosed) [inferred: what reads it].
        kNodeBonesPosed = 1u << 10,
        // On the moved list (g_movedNodes): its matrices and bounds are stale. ZGE2 keeps it and kNodeNotMoved in
        // the flags' upper word at bits 7 and 11 (nodeStateFlags: markNodeMoved, updateMovedNode).
        kNodeMoved = game::readable::perGame(1u << 9, 1u << 7, 1u << 9),
        // Set by a caller that places a node as it loads it (loadGeometryInstance), so
        // that setting its matrix does not put it on the moved list; the
        // matrix set clears it.
        kNodeNotMoved = game::readable::perGame(1u << 13, 1u << 11, 1u << 13),
        kNodeLightsChanged = 1u << 14,   // its lights changed: the draw entry updates them (drawNodeInView)
        // Its world matrix is its parent's, its own local matrix not applied (updateWorldMatrices2; ZGE3's bit, set
        // from an instance chunk's flags by loadGeometryInstance). ZGE2 keeps it in the flags' upper word at bit 13
        // (nodeStateFlags: the doubleword's bit 45).
        kNodeTakesParentWorld = game::readable::perGame(1u << 15, 1u << 13, 1u << 15),
        kNodeInWorld = 1u << 11,         // on the world's list and in its grid (addWorldNode)
        kNodeOutOfGrid = 1u << 12,       // a sky: addToCells leaves it out
        kNodeMirrored = 1u << 17,        // a mirrored instance: its matrix cannot be set
        kNodeKeepsRegion = 1u << 19,     // its region (+0x120) is not looked up again when it moves
    };

    // What the culling keeps per view (two views in split screen).
    struct NodeView
    {
        Vec4 viewCentre;      // +0x00 the bounding sphere's centre in view space (BMX: drawTerrain reads its z)
        ee::Ptr<ZgePart> part; // +0x10 the part drawn in this view (LOD choice; BMX: submitNodeDraw, drawTerrain)
#if ZX_GAME_BMX
        // +0x14 ZGE4 keeps the view's material table here: its draws read node + view * 0x20 + 0x194
        // (nodeDrawBucket, drawTerrain) where ZGE3's read Node::materials.
        ee::Ptr<MaterialTable> materials;
        // +0x18 ZGE4 packs the frame stamp into the level-of-detail word: bits 0-26 the frame the view was last
        // culled in (kViewFrameStampBits), bit 27 drawn (kViewDrawn), bits 28-31 the level of detail (drawScene
        // stamps it, clears drawn and reads the level; nodeNotDrawn reads the stamp and drawn; setNodeLevelOfDetail
        // writes the level). drawScene reads and writes it as a doubleword with +0x1C, whose bits it keeps.
        uint32_t lodAndFlags;
#else
        uint32_t frameStamp;  // +0x14 the frame it was last culled in
        uint32_t lodAndFlags; // +0x18 bit 0 drawn (kViewDrawn), bits 1-4 the level of detail
#endif
        uint32_t _1c;
    };
    static_assert(sizeof(NodeView) == 0x20);
    static_assert(offsetof(NodeView, lodAndFlags) == 0x18);

    // NodeView::lodAndFlags: the view was drawn (the scene draw sets it when it queues the node, clears it when it
    // culls it again). ZGE4 keeps it at bit 27, the frame stamp below it.
    inline constexpr uint32_t kViewDrawn = game::readable::perGame(1u << 0, 1u << 0, 1u << 27);
    // NodeView::lodAndFlags: the view's level of detail (ZGE3 bits 1-4, ZGE4 bits 28-31).
    inline constexpr uint32_t kViewLevelOfDetail = game::readable::perGame(0xFu << 1, 0xFu << 1, 0xFu << 28);
    // ZGE4: the frame stamp's bits of NodeView::lodAndFlags (BMX drawScene masks the stamp it writes and compares).
    inline constexpr uint32_t kViewFrameStampBits = 0x07FFFFFFu;
    // The frame the view was last culled in: ZGE3's own word (NodeView::frameStamp), ZGE4's the low 27 bits of the
    // level-of-detail word. (Templates, so that the word the other layout lacks is named only in its build.)
    template <typename ViewType = NodeView>
    uint32_t viewFrameStamp(const ViewType &view)
    {
        if constexpr (game::readable::kIsBmx)
            return view.lodAndFlags & kViewFrameStampBits;
        else
            return view.frameStamp;
    }
    template <typename ViewType = NodeView>
    void setViewFrameStamp(ViewType &view, uint32_t frame)
    {
        if constexpr (game::readable::kIsBmx)
            view.lodAndFlags = (view.lodAndFlags & ~kViewFrameStampBits) | (frame & kViewFrameStampBits);
        else
            view.frameStamp = frame;
    }

    struct Node
    {
        uint32_t flags;          // +0x000 NodeFlags
        uint32_t flagsHigh;      // +0x004 the upper half of the flags' doubleword
#if ZX_GAME_BMX
        // ZGE4 moved the node's name hash and model up here (setNodeNameHash, setNodeGeometry).
        uint32_t nameHash;       // BMX +0x008
        ee::Ptr<GeoObject> model; // BMX +0x00C
#else
        uint8_t _008[0x08];
#endif
        Matrix local;            // +0x010 relative to the parent
        // +0x050 in the world: rows X, Y, Z (axes) and T (position). Row X's
        // w (+0x5C) is scratch: the terrain renderer (drawTerrain) keeps
        // this view's distance minus the radius there for the grass shells.
        Matrix world;
        Matrix inverseWorld;     // +0x090 world's inverse (rigid)
        // +0x0D0 the bounding box in world space (sphere queries test it).
        Vec4 boxMin;
        Vec4 boxMax;             // +0x0E0
        Vec4 sphereCentre;       // +0x0F0 bounding sphere
        ee::Float radius;        // +0x100
        uint8_t _104[0x0C];
#if ZX_GAME_DM2
        // DM2's fields here, at the places its machine code uses; the last ones are in no
        // function read so far, and stand where nothing else is.
        // DM2 +0x110 the region whose box holds the node's box (updateMovedNode writes it; drawSky's node visit
        // hands it to the region's view test).
        uint32_t region;
        uint32_t tags;                        // DM2 +0x114
        uint32_t nameHash;                    // DM2 +0x118
        char name[0x20];                      // DM2 +0x11C
        uint32_t geometryId;                  // DM2 +0x13C
        uint32_t instanceId;                  // DM2 +0x140
        ee::Ptr<GeoObject> model;             // DM2 +0x144
        uint32_t levelObject;                 // DM2 +0x148
        ee::Ptr<Node> firstChild;             // DM2 +0x14C
        ee::Ptr<Node> nextSibling;            // DM2 +0x150
        ee::Ptr<Node> parent;                 // DM2 +0x154
        uint32_t queryStamp;                  // DM2 +0x158
        ee::Ptr<CollisionFrame> lods;         // DM2 +0x15C
        uint32_t drawFlags;                   // DM2 +0x160
        uint32_t _17c;                        // DM2 +0x164
        uint32_t _168;
        // [unplaced: ZGE2 keeps no position key (its updateMovedNode looks the region up at every move); the word
        // stands where nothing read so far is, for the shared code that names it.]
        uint32_t positionKey;
        // DM2 +0x170 the scene draw (CollisionWorld::frameStamp) that last listed it (its listed-node draw; ZGE3 and
        // ZGE4 stamp each view: NodeView::frameStamp).
        uint32_t listedFrame;
        // DM2 +0x174 bit 0 (kNodeShadowViewTest's place in DM2): the caster is
        // tested against the view at its next queueing; setSceneShadowViewTest
        // sets it, the shadow queue tests and clears it before the view test of
        // shadowViewSphere.
        uint32_t shadowViewTest;
        // The word a shadow view test is asked for in (setSceneShadowViewTest): DM2's own.
        uint32_t &shadowViewTestRequest() { return shadowViewTest; }
        ee::Ptr<MaterialTable> materials;     // DM2 +0x178 (drawRigid, drawSkinned, setNodeLevelOfDetail)
        uint32_t viewPart;                    // DM2 +0x17C AI's views[0].part
        ee::Ptr<BonePose> bonePoses;          // DM2 +0x180 (drawSkinned)
        // +0x184 asked before the scene draw walks a tree (the data with it at +0x1B0: setNodeCallback).
        uint32_t drawCheck;
        uint8_t colour[4];                    // DM2 +0x188 R, G, B, A (AI's +0x180)
        uint8_t _18c[0x04];                   // DM2 +0x18C
        Vec4 shadowViewSphere;                // DM2 +0x190 (DM2's too, ahead of its view test)
        uint8_t _1a0[0x10];                   // DM2 +0x1A0
        uint32_t drawCheckData;               // DM2 +0x1B0
        ee::Ptr<LightBlock> lights;           // DM2 +0x1B4 [unproven: only drawSkinned, which is level 1]
        uint8_t _1b8[0x08];
#elif ZX_GAME_BMX
        // ZGE4's node (BMX XXX), 0x1F0 bytes: the name hash and model sit at +0x08 and +0x0C, so
        // every field from the name on is 4 or 8 bytes lower than ZGE3's; the two views come
        // before the shadow test words and the colour, and the bone poses end the record. Each
        // offset is read in BMX XXX's own code (the function named).
        Vec4 origin;                          // BMX +0x110 (newAppInstanceNode)
        uint32_t region;                      // BMX +0x120 (updateMovedNode)
        uint32_t positionKey;                 // BMX +0x124 (updateMovedNode)
        char name[0x20];                      // BMX +0x128 (addModelBone, loadGeometryInstance)
        uint32_t geometryId;                  // BMX +0x148 (setNodeGeometry)
        uint32_t instanceId;                  // BMX +0x14C (findInstanceById)
        uint32_t tags;                        // BMX +0x150 (setNodeTags)
        uint32_t levelObject;                 // BMX +0x154 (setNodeLevelObject)
        ee::Ptr<LightBlock> lights;           // BMX +0x158 (drawSkinned)
        ee::Ptr<Node> firstChild;             // BMX +0x15C (addGeometryChild)
        ee::Ptr<Node> nextSibling;            // BMX +0x160
        ee::Ptr<Node> parent;                 // BMX +0x164
        uint32_t queryStamp;                  // BMX +0x168 (sphereQueryVisit)
        ee::Ptr<CollisionFrame> lods;         // BMX +0x16C (nodeCollisionFrame)
        uint32_t drawFlags;                   // BMX +0x170 (setNodeCastsShadow, setNodeMaterialSet)
        uint32_t _174;
        // The word a shadow view test is asked for in (setSceneShadowViewTest): drawFlags.
        uint32_t &shadowViewTestRequest() { return drawFlags; }
        uint8_t _178[0x08];                   // BMX +0x178
        NodeView views[2];                    // BMX +0x180 (newAppInstanceNode's loop, drawTerrain, drawRigid, submitNodeDraw)
        Vec4 shadowViewSphere;                // BMX +0x1C0 (setSceneShadowViewTest: the 0x20 bytes from here; queuePartDraw hands it to partInView)
        // [unplaced: ZGE4 keeps the materials per view (NodeView::materials); the shared bodies that read the node's
        // table stand on this word until they take the view's. The 0x10 bytes from +0x1D0 are not read yet.]
        ee::Ptr<MaterialTable> materials;     // BMX +0x1D0
        uint8_t _1d4[0x0C];                   // BMX +0x1D4
        uint8_t colour[4];                    // BMX +0x1E0 (setSceneRootColour, newAppInstanceNode; queuePartDraw's test, buildShadowVolume's bytes)
        uint32_t drawCheck;                   // BMX +0x1E4 (setNodeDrawCheck)
        uint32_t drawCheckData;               // BMX +0x1E8 (setNodeDrawCheck)
        ee::Ptr<BonePose> bonePoses;          // BMX +0x1EC (setupSceneModel; buildShadowVolume)
#else
        // +0x110 (0, 0, 0, 1) when the node is made [unknown: what reads it].
        Vec4 origin;
        // +0x120 the region whose box holds the node's box (findRegion's
        // pool, g_regions), 0 if none [inferred: what the region is for].
        uint32_t region;
        // +0x124 its position's grid key: x / 2 and z / 2 truncated, 16 bits each
        // (z high); the region is looked up again when it changes.
        uint32_t positionKey;
        uint32_t nameHash;       // +0x128 nameHash(name): what the game finds it by
        char name[0x20];         // +0x12C
        // +0x14C its model (a GeoObject, zge/assets/geo.h): its bounds hold its mesh in the node's space.
        ee::Ptr<GeoObject> model;
        uint32_t geometryId;                  // +0x150 its object's id (GeoObject::id)
        uint32_t instanceId;                  // +0x154 TGIH's: what a child's instance names as its parent
        uint32_t tags;                        // +0x158 its tag table (zge/assets/tag_table.h), 0: none
        uint32_t levelObject;                 // +0x15C the level object it is part of (zge/scene/objects.cpp), 0: none
        ee::Ptr<LightBlock> lights;           // +0x160 used with kNodeOwnLights
        ee::Ptr<Node> firstChild;             // +0x164 the scene tree (forEachNodeInSubtree walks it)
        ee::Ptr<Node> nextSibling;            // +0x168
        ee::Ptr<Node> parent;                 // +0x16C 0 at the top of the tree
        uint32_t queryStamp;                  // +0x170 the last sphere query that visited it
        // +0x174 its geometry frame: the parts of its levels of detail and
        // its collision data (zge/collision/frame.cpp).
        ee::Ptr<CollisionFrame> lods;
        // +0x178 bits 4-7: which of its mesh's material sets it is drawn
        // with (setNodeMaterialSet; the originals read and write it as a
        // doubleword with +0x17C); bit 12 (kNodeCastsShadow): it casts a
        // shadow (set over a scene's tree by setNodeCastsShadow; queuePartDraw,
        // called by the node draws drawScene and drawNodeTree, queues only such
        // nodes, with a +0x180 word,).
        uint32_t drawFlags;
        uint32_t _17c;
        // The word a shadow view test is asked for in (setSceneShadowViewTest): drawFlags.
        uint32_t &shadowViewTestRequest() { return drawFlags; }
        // +0x180 R, G, B, A: (0x20, 0x20, 0x20, 0x80) when made [inferred:
        // the word the shadow queue keeps].
        uint8_t colour[4];
        uint8_t _184[0x0C];                   // +0x184
        // +0x190 what a caster's test against the view takes (queuePartDraw).
        Vec4 shadowViewSphere;
        uint8_t _1a0[0x10];                   // +0x1A0
        ee::Ptr<MaterialTable> materials;     // +0x1B0
        // +0x1B4 asked before the scene draw walks a listed node's tree:
        // (node, &level of detail, drawCheckData), 0 leaves the tree undrawn;
        // none when 0.
        uint32_t drawCheck;
        uint32_t drawCheckData;               // +0x1B8
        // +0x1BC skinned models: the bone poses, 0xC0 bytes each; the pose
        // matrix is at +0x40 of each (zge/scene/skeleton.h).
        ee::Ptr<BonePose> bonePoses;
#endif
#if !ZX_GAME_BMX
        NodeView views[2];                    // +0x1C0 (BMX +0x180, above)
#endif
#if ZX_GAME_DM2
        // AI's: DM2 keeps one drawn part (viewPart), and no pivot here. Only functions with no DM2 counterpart use these.
        Vec4 origin;
#endif

        // The flags as the original's bit fields reach them, each with the word
        // after it.
        uint64_t &flagsDoubleword() { return *reinterpret_cast<uint64_t *>(&flags); }
        uint64_t &drawFlagsDoubleword() { return *reinterpret_cast<uint64_t *>(&drawFlags); }
        // The box in the world as one record, and the bounds in the model's.
        Box &box() { return *reinterpret_cast<Box *>(&boxMin); }
        Box &modelBounds() { return model->bounds(); }
    };
    static_assert(offsetof(Node, local) == 0x10);
    static_assert(offsetof(Node, world) == 0x50);
    static_assert(offsetof(Node, boxMin) == 0xD0 && offsetof(Node, boxMax) == 0xE0);
    static_assert(offsetof(Node, sphereCentre) == 0xF0);
    static_assert(offsetof(Node, inverseWorld) == 0x90);
    static_assert(offsetof(Node, radius) == 0x100);
    // DM2's and BMX XXX's as their machine code uses them (DM2's origin, AI's word, stands past its views; its lights are unproven).
    static_assert(offsetof(Node, nameHash) == game::readable::perGame(0x128u, 0x118u, 0x008u) && offsetof(Node, name) == game::readable::perGame(0x12Cu, 0x11Cu, 0x128u) &&
                  offsetof(Node, colour) == game::readable::perGame(0x180u, 0x188u, 0x1E0u));
    static_assert(offsetof(Node, region) == game::readable::perGame(0x120u, 0x110u, 0x120u) && offsetof(Node, positionKey) == game::readable::perGame(0x124u, 0x16Cu, 0x124u));
    static_assert(offsetof(Node, model) == game::readable::perGame(0x14Cu, 0x144u, 0x00Cu) && offsetof(Node, parent) == game::readable::perGame(0x16Cu, 0x154u, 0x164u));
    static_assert(offsetof(Node, tags) == game::readable::perGame(0x158u, 0x114u, 0x150u));
#if ZX_GAME_DM2
    static_assert(offsetof(Node, listedFrame) == 0x170 && offsetof(Node, viewPart) == 0x17C);
#endif
    static_assert(offsetof(Node, geometryId) == game::readable::perGame(0x150u, 0x13Cu, 0x148u) && offsetof(Node, instanceId) == game::readable::perGame(0x154u, 0x140u, 0x14Cu));
    static_assert(offsetof(Node, levelObject) == game::readable::perGame(0x15Cu, 0x148u, 0x154u));
    static_assert(offsetof(Node, firstChild) == game::readable::perGame(0x164u, 0x14Cu, 0x15Cu) && offsetof(Node, nextSibling) == game::readable::perGame(0x168u, 0x150u, 0x160u));
    static_assert(offsetof(Node, queryStamp) == game::readable::perGame(0x170u, 0x158u, 0x168u));
    static_assert(offsetof(Node, lods) == game::readable::perGame(0x174u, 0x15Cu, 0x16Cu) && offsetof(Node, drawFlags) == game::readable::perGame(0x178u, 0x160u, 0x170u));
    static_assert(offsetof(Node, materials) == game::readable::perGame(0x1B0u, 0x178u, 0x1D0u) && offsetof(Node, drawCheck) == game::readable::perGame(0x1B4u, 0x184u, 0x1E4u));
    static_assert(offsetof(Node, drawCheckData) == game::readable::perGame(0x1B8u, 0x1B0u, 0x1E8u) && offsetof(Node, bonePoses) == game::readable::perGame(0x1BCu, 0x180u, 0x1ECu));
#if ZX_GAME_BMX
    static_assert(offsetof(NodeView, materials) == 0x14); // (the view's table: node + view * 0x20 + 0x194)
#endif
    static_assert(offsetof(Node, lights) == game::readable::perGame(0x160u, 0x1B4u, 0x158u));
    static_assert(game::readable::perGame(offsetof(Node, origin) == 0x110 && offsetof(Node, views[1]) + offsetof(NodeView, part) == 0x1F0,
                                          offsetof(Node, origin) > offsetof(Node, views),
                                          offsetof(Node, origin) == 0x110 && offsetof(Node, views[0]) + offsetof(NodeView, part) == 0x190));
    static_assert(offsetof(Node, shadowViewSphere) == game::readable::perGame(0x190u, 0x190u, 0x1C0u));
    static_assert(offsetof(Node, views) == game::readable::perGame(0x1C0u, 0x1C0u, 0x180u));
    // A node record's size: what the node pool is made with (initGeometryNodes) and newAppInstanceNode clears. DM2's
    // record is shorter than the struct here: of its views only the first's view centre (+0x1C0, its listed-node draw's
    // transformPoint) is DM2's; the rest and the origin stand past its end, and nothing of DM2's reads them.
    inline constexpr uint32_t kNodeBytes = game::readable::perGame(0x200u, 0x1D0u, 0x1F0u);
    static_assert(game::readable::kIsDm2 || sizeof(Node) == kNodeBytes);

    // Node::drawFlags: the node casts a shadow (ZGE2 keeps it four bits higher: setNodeCastsShadow).
    inline constexpr uint32_t kNodeCastsShadow = game::readable::perGame(1u << 12, 1u << 16, 1u << 12);
    // Two of NodeFlags as the doubleword the originals read and write them in: ZGE2 keeps them in the upper word
    // (changeSceneState's own-lights switch, markBonesPosed), ZGE3 and ZGE4 in the lower (kNodeOwnLights, kNodeBonesPosed).
    inline constexpr uint64_t kNodeOwnLightsBit = game::readable::perGame(1ull << 7, 1ull << 38, 1ull << 7);
    inline constexpr uint64_t kNodeBonesPosedBit = game::readable::perGame(1ull << 10, 1ull << 40, 1ull << 10);
    // The word that holds kNodeBoneTree and kNodeSecondBucket (setupSceneModel, countModelNodes, addModelBone) and
    // kNodeShown (DM2's setNodeShown reads bit 32): ZGE2 keeps them in the upper word of the flags doubleword (bits 32,
    // 34 and 35), ZGE3 and ZGE4 in the lower word, at the same positions in the word.
#if ZX_GAME_DM2
    inline uint32_t &nodeTreeFlags(Node &node) { return node.flagsHigh; }
#else
    inline uint32_t &nodeTreeFlags(Node &node) { return node.flags; }
#endif
    // The word that holds kNodeMoved and kNodeNotMoved: ZGE2's upper word of the flags doubleword (bits 39 and 43),
    // ZGE3's and ZGE4's lower word.
    inline uint32_t &nodeStateFlags(Node &node) { return game::readable::kIsDm2 ? node.flagsHigh : node.flags; }

    // ZGE2's listed-node draw stamps the node itself with the scene draw that listed it (Node::listedFrame) and draws
    // its one drawn part (Node::viewPart); ZGE3 and ZGE4 stamp each view and draw each view's part (NodeView), so the
    // stamp's accessors do nothing there and the part is the first view's.
    inline uint32_t nodeListedFrame([[maybe_unused]] const Node &node)
    {
#if ZX_GAME_DM2
        return node.listedFrame;
#else
        return 0;
#endif
    }
    inline void setNodeListedFrame([[maybe_unused]] Node &node, [[maybe_unused]] uint32_t frame)
    {
#if ZX_GAME_DM2
        node.listedFrame = frame;
#endif
    }
    inline ee::Ptr<ZgePart> nodeDrawnPart(const Node &node)
    {
#if ZX_GAME_DM2
        return ee::Ptr<ZgePart>(node.viewPart);
#else
        return node.views[0].part;
#endif
    }

    // A TGIH chunk: an instance of a geometry object, as loadGeometryInstance reads it. ZGE2's has no region word
    // (its name follows the instance id), and its flags are bit fields of their own (loadGeometryInstance).
    struct InstanceChunk
    {
        uint32_t flags;          // +0x00 bits 0-7 as the node's, 8 and 9 its bits 15, 16, kInstanceMirrored, kInstanceKeepsRegion
        uint8_t level;           // +0x04
        uint8_t materialSet;     // +0x05
        uint8_t _06[2];
        ee::Float rotation[4];   // +0x08 a quaternion
        ee::Float position[3];   // +0x18
        uint8_t _24[0x18];
        uint32_t nameHash;       // +0x3C
        uint32_t geometryId;     // +0x40
        uint32_t parent;         // +0x44 the parent's instance id, 0: at the top
        uint32_t instanceId;     // +0x48
#if ZX_GAME_DM2
        char name[0x20];         // DM2 +0x4C
        uint32_t region;         // [unplaced: ZGE2's chunk has none; for the shared code that names it]
#else
        uint32_t region;         // +0x4C
        char name[0x20];         // +0x50
#endif
    };
    static_assert(sizeof(InstanceChunk) == 0x70 && offsetof(InstanceChunk, nameHash) == 0x3C && offsetof(InstanceChunk, instanceId) == 0x48);
    static_assert(offsetof(InstanceChunk, name) == game::readable::perGame(0x50u, 0x4Cu, 0x50u));

    // instanceGeometryNode's flags, above copyGeoObject's (the low bits): the
    // copy takes the node's name, and the node's children and next siblings
    // are copied too.
    // ZGE4 only: the instance gets a copy of the node's tag table (ZGE2 and ZGE3 share the table).
    constexpr uint32_t kInstanceCopyTags = (1u << 19);
    constexpr uint32_t kInstanceKeepName = (1u << 20);
    constexpr uint32_t kInstanceWithFamily = (2u << 20);

    // The scene tree's transforms (zge/scene/transform.cpp).
    ee::Ptr<Node> rootNode(ee::Ptr<Node> node);
    ee::Ptr<Matrix> nodeMatrix(ee::Ptr<Node> node, uint32_t which);        // 0 local, 1 world, 2 inverse
    void setNodeLocalMatrix(ee::Ptr<Node> node, ee::Ptr<const Matrix> matrix);
    void updateWorldMatrices(ee::Ptr<Node> node);
    void updateMovedNode(ee::Ptr<Node> node);
    void updateMovedNodes();
    // updatedNodeVisibleInView (geometry_nodes.cpp): a moved node brought up to date, then nodeVisibleInView's
    // cull flags for it (ZGE4's scene draw culls a listed node with it).
    uint32_t updatedNodeVisibleInView(ee::Ptr<Node> node);
    // setNodeMaterialSet (collision/frame.cpp): which of its mesh's material sets
    // the node is drawn with.
    void setNodeMaterialSet(ee::Ptr<Node> node, uint32_t set);

    // The view being drawn (D_273964) and its camera (D_273960).
    inline ee::Global<uint32_t, sym::g_currentView> g_currentView;
    inline ee::Global<uint32_t, sym::g_currentCamera> g_currentCamera;

    // Per view, uploaded to VU1 by beginViewDraws: pointers to the camera's
    // world -> view (VU1 1012-1015), world -> GS screen (1016-1019) and
    // world -> clip volume (guard band, 1020-1023) matrices.
    inline ee::Global<ee::Ptr<Matrix>, sym::g_viewMatrix> g_viewMatrix;
    inline ee::Global<ee::Ptr<Matrix>, sym::g_screenMatrix> g_screenMatrix;
    inline ee::Global<ee::Ptr<Matrix>, sym::g_clipMatrix> g_clipMatrix;
}

namespace zge
{
    // (geometry_nodes.cpp)
    uint32_t freeGeometryNode(ee::Ptr<Node> node);
    uint32_t releaseNode(ee::Ptr<Node> node, uint32_t family);
    ee::Ptr<Node> instanceGeometryNode(ee::Ptr<Node> node, uint32_t flags);
    void retainNode(ee::Ptr<Node> node);
    int32_t refreshNodeBounds(ee::Ptr<Node> node);
    int32_t refreshNodeBoxAndSphere(ee::Ptr<Node> node); // (ZGE2's refreshNodeBounds)
    ee::Ptr<char> nodeName(ee::Ptr<Node> node);
    uint32_t nodeNameHash(ee::Ptr<Node> node);
    void setNodeNameHash(ee::Ptr<Node> node, uint32_t nameHash);
    ee::Ptr<TagTableHeader> setNodeTags(ee::Ptr<Node> node, ee::Address table);
    void setNodeGeometry(ee::Ptr<Node> node, uint32_t object);
    ee::Ptr<GeoObject> objectClass(ee::Ptr<Node> node);
    // The node's class as the engine's own code gets it: ZGE4 reads Node::model itself (sceneFirstMesh), ZGE2 and ZGE3
    // ask objectClass.
    inline ee::Ptr<GeoObject> nodeClass(ee::Ptr<Node> node)
    {
        if constexpr (game::readable::kIsBmx)
            return node->model;
        else
            return objectClass(node);
    }
}

namespace zge
{
    struct AxisCells; // (zge/collision/collision.h)

    // (transform.cpp)
    uint32_t nodeNotDrawn(ee::Ptr<Node> node);
    ee::Ptr<Box> nodeBox(ee::Ptr<Node> node);
    int32_t forEachChild(ee::Ptr<Node> node, uint32_t visitFunction, ee::Address context);
    void nodeGridCells(ee::Ptr<Node> node, ee::Ptr<AxisCells> cells);
    void updateNodeBox(ee::Ptr<Node> node, ee::Ptr<Matrix> matrix);
    uint32_t findRegion(ee::Ptr<Box> box);
}

namespace zge
{
    struct CollisionWorld; // (zge/collision/collision.h)

    // (world_mesh.cpp)
    uint32_t setupWorldMeshModels();
    int32_t matchWorldName(ee::Ptr<CollisionWorld> world);
}
