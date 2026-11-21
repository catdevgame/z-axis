// A node's geometry frame (zglTerrainRender, nodeCollisionFrame .. partStream; the
// original calls it GeomFrame, zge/collision/collision.h CollisionFrame):
// the parts the node is drawn with, one a level of detail (FramePart), and
// its collision tree and part. Frames and frame parts are pooled and
// reference counted: a copied scene's nodes share their source's parts,
// and the last release frees them.
//
// Also here: the node's per-view level of detail, its material set, and its
// shown and collides flags, set on one node or its whole subtree.
//

#include "collision.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/assets/game_address.h"
#include "zge/container/array.h"
#include "zge/assets/tag_table.h"
#include "zge/math/matrix.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/render/part_clone.h"
#include "zge/scene/query.h"
#include "zge/text/cstring.h"
#include "zge/assets/geo.h"
#include "zge/render/part.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/collision/frame.cpp.h"

namespace zge
{

    namespace
    {
        // The frames' pool (40-byte records, releaseCollisionFrame when one
        // is freed) and the frame parts' (12-byte records).
        inline ee::Global<int32_t, sym::g_collisionFramesReady> g_collisionFramesReady;
        inline ee::Global<uint32_t, sym::g_collisionFramePool> g_collisionFramePool;
        inline ee::Global<int32_t, sym::g_framePartsReady> g_framePartsReady;
        inline ee::Global<uint32_t, sym::g_framePartPool> g_framePartPool;

        constexpr uint32_t kBoxUnion = sym::boxUnion;     // (out, a, b)
        constexpr uint32_t kCreatePool = sym::createPool; // (record bytes, records a block, free callback) -> pool

        // This file's functions as the originals call them, by address.
        constexpr uint32_t kFrameLevelPart = sym::frameLevelPart;      // frameLevelPart below
        constexpr uint32_t kFrameLevels = sym::frameLevels;         // frameLevels below
        constexpr uint32_t kSetNodeLevelOfDetail = sym::setNodeLevelOfDetail; // setNodeLevelOfDetail below, the subtree visit
        constexpr uint32_t kSetNodeShownFlag = sym::setNodeShownFlag;    // the subtree visits below
        constexpr uint32_t kSetNodeCollidesFlag = sym::setNodeCollidesFlag;
        constexpr uint32_t kNewCollisionFrame = sym::newCollisionFrame;
        constexpr uint32_t kCloneFrameCollision = sym::cloneFrameCollision;
        constexpr uint32_t kAddFrameLevel = sym::addFrameLevel;
        constexpr uint32_t kNewFramePart = sym::newFramePart;
        constexpr uint32_t kReleaseFramePart = sym::releaseFramePart;
        constexpr uint32_t kReleaseCollisionFrame = sym::releaseCollisionFrame;

        // The frame parts' functions below, and the parts' (zge/render/part.cpp).
        constexpr uint32_t kCloneFramePart = sym::cloneFramePart;   // cloneFramePart below
        constexpr uint32_t kBuildFramePartPackets = sym::buildFramePartPackets; // buildFramePartPackets below
        constexpr uint32_t kSetFramePartMesh = sym::setFramePartMesh; // setFramePartMesh below
        constexpr uint32_t kFramePartMesh = sym::framePartMesh;    // framePartMesh below

        // clonePart's flags that ask for any copy (a copy of the mesh).
        constexpr uint32_t kCopyAnyStream = game::readable::perGame(0x7FFFu, 0x1FFFu);

        // The frame's tag table: a copy of one (copyTagTable) and the set
        // (setFrameTagTable, releasing the one it had).
        constexpr uint32_t kSetFrameTags = sym::setFrameTagTable;

        // cloneCollisionFrame's flags: a copy of each mesh (any of the low 15
        // bits, clonePart's), a copy of the tag table.
        constexpr uint32_t kCloneMeshes = 0x0000FFFFu;
        constexpr uint32_t kCloneTags = 0x00010000u;

        // The level of detail drawn: ZGE3 keeps it in the view's word (NodeView::lodAndFlags bits 1-4), ZGE4 in the
        // same word's bits 28-31; ZGE2 has one level a node, in Node::drawFlags bits 8-11.
        constexpr uint32_t kViewLevelShift = game::readable::perGame(1u, 8u, 28u);
        constexpr uint32_t kViewLevelMask = 0xFu << kViewLevelShift;
        constexpr uint32_t kDrawMaterialSetShift = 4; // Node::drawFlags bits 4-7
        constexpr uint32_t kDrawMaterialSetMask = 0xFu << kDrawMaterialSetShift;

        NodeView &currentView(ee::Ptr<Node> node) { return node->views[*g_currentView]; }

        // The node's material table for its draws: ZGE4 keeps one a view (NodeView::materials, what its draws read),
        // ZGE2 and ZGE3 one a node. (A template, so that the view's word is named only in ZGE4's build.)
        template <typename NodeType = Node>
        void setNodeMaterials(ee::Ptr<NodeType> node, ee::Ptr<MaterialTable> materials)
        {
            if constexpr (game::readable::kIsBmx)
                node->views[*g_currentView].materials = materials;
            else
                node->materials = materials;
        }

        // The word with the level of detail bits (kViewLevelMask), and the part drawn: ZGE2's node has one of each,
        // ZGE3's one a view.
        uint32_t &levelWord(ee::Ptr<Node> node)
        {
            if constexpr (game::readable::kIsDm2)
                return node->drawFlags;
            else
                return currentView(node).lodAndFlags;
        }
        // (A template, so that the word the other layouts lack is named only in ZGE2's build.)
        template <typename NodeType = Node>
        void setDrawnPart(ee::Ptr<NodeType> node, ee::Ptr<ZgePart> part)
        {
            if constexpr (game::readable::kIsDm2)
                node->viewPart = part.address;
            else
                currentView(node).part = part;
        }
    }

    // nodeCollisionFrame (kNodeCollisionFrame): the node's geometry frame.
    ee::Ptr<CollisionFrame> nodeCollisionFrame(ee::Ptr<Node> node)
    {
        return node->lods;
    }
    READABLE_FUNCTION(nodeCollisionFrame, Verified);

    // setNodeMaterialSet: which of its mesh's material sets the node is drawn with.
    void setNodeMaterialSet(ee::Ptr<Node> node, uint32_t set)
    {
        node->drawFlags = (node->drawFlags & ~kDrawMaterialSetMask) | (set & 0xFu) << kDrawMaterialSetShift;
    }
    READABLE_FUNCTION(setNodeMaterialSet, Verified);

    // setLodTable: which of its class's geometry frames the node has
    // (drawFlags' low four bits). The result is the frame.
    ee::Ptr<CollisionFrame> setLodTable(ee::Ptr<Node> node, uint32_t table)
    {
        const uint32_t which = table & 0xFFu;
        const ee::Ptr<GeoObject> model = node->model;
        node->drawFlags = (node->drawFlags & ~0xFu) | (which & 0xFu);
        const ee::Ptr<CollisionFrame> frame = classModel(model, which);
        node->lods = frame;
        return frame;
    }
    READABLE_FUNCTION(setLodTable, Verified);

    // setNodeLevelOfDetail: the node's level of detail in the view being drawn: its
    // frame's part of that level drawn there (none if the frame has no such
    // level: 0), and a mesh part's material table for the node's material
    // set taken. 1 when it has the level.
    uint32_t setNodeLevelOfDetail(ee::Ptr<Node> node, uint32_t level)
    {
        levelWord(node) = (levelWord(node) & ~kViewLevelMask) | (level & 0xFu) << kViewLevelShift;
        const ee::Ptr<FramePart> part = frameLevelPart(node->lods, level);
        if (!part)
        {
            setDrawnPart(node, ee::Ptr<ZgePart>());
            return 0;
        }
        setDrawnPart(node, part->mesh);
        const ee::Ptr<ZgePart> mesh = part->mesh;
        if (mesh->header == 0)
        {
            const uint32_t set = (node->drawFlags & kDrawMaterialSetMask) >> kDrawMaterialSetShift;
            setNodeMaterials(node, meshMaterials(mesh, set));
        }
        return 1;
    }
    READABLE_FUNCTION(setNodeLevelOfDetail, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int forEachNodeInSubtree(ee::Ptr<Node> root, uint32_t visit, uint32_t context) { return game::tail(zge::forEachNodeInSubtree, root, visit, context); }
        }
    }

    // setTreeLevelOfDetail: the level of detail set on the node's whole subtree
    // when it is not the one the view has (a tail call); else that level.
    int32_t setTreeLevelOfDetail(ee::Ptr<Node> node, uint32_t level)
    {
        level &= 0xFFu;
        const uint32_t current = (levelWord(node) & kViewLevelMask) >> kViewLevelShift;
        if (level != current)
            return tail::forEachNodeInSubtree(node, kSetNodeLevelOfDetail, level);
        return static_cast<int32_t>(current);
    }
    READABLE_FUNCTION_WAITS(setTreeLevelOfDetail, Verified);

    namespace
    {
        // The word with the node's shown and collides bits (kNodeShown, kNodeCollides): ZGE2 keeps them in the flags'
        // upper word, ZGE3 in the flags.
        uint32_t &stateFlags(Node &node)
        {
            if constexpr (game::readable::kIsDm2)
                return node.flagsHigh;
            else
                return node.flags;
        }
    }

    // setNodeShownFlag: kNodeShown set or cleared (a subtree visit). 1.
    uint32_t setNodeShownFlag(ee::Ptr<Node> node, uint32_t shown)
    {
        stateFlags(*node) = (stateFlags(*node) & ~kNodeShown) | (shown & 1u);
        return 1;
    }
    READABLE_FUNCTION(setNodeShownFlag, Verified);

    // setNodeShown (kObjectSetA): the node shown or hidden, alone or with
    // its subtree; whether it was shown.
    uint32_t setNodeShown(ee::Ptr<Node> node, uint32_t shown, uint32_t wholeTree)
    {
        const uint32_t wasShown = stateFlags(*node) & kNodeShown;
        if (wholeTree)
        {
            forEachNodeInSubtree(node, kSetNodeShownFlag, shown);
            return wasShown;
        }
        stateFlags(*node) = (stateFlags(*node) & ~kNodeShown) | (shown & 1u);
        return wasShown;
    }
    READABLE_FUNCTION_WAITS(setNodeShown, Verified);

    // setNodeCollidesFlag: kNodeCollides set or cleared (a subtree visit). 1.
    uint32_t setNodeCollidesFlag(ee::Ptr<Node> node, uint32_t collides)
    {
        stateFlags(*node) = (stateFlags(*node) & ~kNodeCollides) | (collides & 1u) << 1;
        return 1;
    }
    READABLE_FUNCTION(setNodeCollidesFlag, Verified);

    // setNodeCollides (kObjectSetB): whether queries test the node, alone or
    // with its subtree; whether it was shown (the original returns bit 0).
    uint32_t setNodeCollides(ee::Ptr<Node> node, uint32_t collides, uint32_t wholeTree)
    {
        const uint32_t wasShown = stateFlags(*node) & kNodeShown;
        if (wholeTree)
        {
            forEachNodeInSubtree(node, kSetNodeCollidesFlag, collides);
            return wasShown;
        }
        stateFlags(*node) = (stateFlags(*node) & ~kNodeCollides) | (collides & 1u) << 1;
        return wasShown;
    }
    READABLE_FUNCTION_WAITS(setNodeCollides, Verified);

    // nodeShown: whether the node is shown.
    uint32_t nodeShown(ee::Ptr<Node> node)
    {
        return node->flags & kNodeShown;
    }
    READABLE_FUNCTION(nodeShown, Verified);

    // nodeCollides: whether queries test the node (kNodeCollides; ZGE2's, from the flags doubleword's upper word:
    // ZGE3 and ZGE4 test the flag where they query).
    inline uint32_t nodeCollides(ee::Ptr<Node> node)
    {
        return stateFlags(*node) >> 1 & 1u;
    }
    READABLE_FUNCTION_ONLY(dm2, nodeCollides);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        Matrix * matrixIdentity(ee::Ptr<Matrix> m) { return game::tail(zge::matrixIdentity, m); }
        }
    }

    // resetMatrices: three matrices in a row (a node's local, world and
    // inverse) made the identity (the last a tail call).
    void resetMatrices(ee::Ptr<Matrix> first)
    {
        matrixIdentity(first.get());
        matrixIdentity(first.get() + 1);
        tail::matrixIdentity(first.get() + 2);
    }
    READABLE_FUNCTION(resetMatrices, Verified);

    // initCollisionFrames: the frames' pool made (256 a block; ZGE4's 400). 1.
    uint32_t initCollisionFrames()
    {
        *g_collisionFramesReady = 1;
        *g_collisionFramePool = createPool(kCollisionFrameBytes, game::readable::perGame(0x100u, 0x100u, 0x190u), kReleaseCollisionFrame);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initCollisionFrames, Verified);

    // shutdownCollisionFramePool.
    uint32_t shutdownCollisionFramePool()
    {
        if (*g_collisionFramesReady != 0)
        {
            destroyPool(ee::Ptr<Pool>(*g_collisionFramePool));
            *g_collisionFramePool = 0;
            *g_collisionFramesReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownCollisionFramePool, Verified);

    // newCollisionFrame: a new frame, cleared, with one reference; 0 if the pool
    // gave none.
    uint32_t newCollisionFrame()
    {
        const uint32_t made = poolAlloc(ee::Ptr<Pool>(*g_collisionFramePool));
        if (made)
        {
            memoryFill(made, 0, kCollisionFrameBytes);
            ee::Ptr<CollisionFrame>(made)->references = 1;
        }
        return made;
    }
    READABLE_FUNCTION_WAITS(newCollisionFrame, Verified);

    uint32_t releaseFramePart(ee::Ptr<FramePart> part);

    // releaseCollisionFrame (also the pool's free callback): one reference less; with
    // the last, its levels' parts released (last first), its tree freed, its
    // collision part released and the record handed back. 1.
    uint32_t releaseCollisionFrame(ee::Ptr<CollisionFrame> frame)
    {
        if (--frame->references != 0)
            return 1;
        const uint32_t levels = frameLevels(frame);
        for (uint32_t level = levels; level-- != 0;)
        {
            releaseFramePart(frameLevelPart(frame, level));
            if (level == 0)
                break;
        }
        if (frame->tree)
            heapFree(frame->tree);
        if (frame->part)
            releasePart(frame->part);
        frame->freeLevels();
        poolFree(ee::Ptr<Pool>(*g_collisionFramePool), frame);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseCollisionFrame, Verified);

    // cloneFrameCollision (the original's GeomFrameCollisionClone): a copy's
    // collision data from its source. When the source has a tree: the copy's
    // collision part is the copy's level part standing where the source's
    // collision part stands among the source's levels (one more reference),
    // and the tree is copied; with no such level, the source's own
    // collision part is shared (no tree then: tree and size 0). A source
    // without a tree leaves the copy's alone.
    void cloneFrameCollision(ee::Ptr<CollisionFrame> copy, ee::Ptr<CollisionFrame> source)
    {
        if (source->treeBytes)
        {
            const uint32_t levels = frameLevels(source);
            copy->part = ee::Ptr<ZgePart>();
            for (int32_t level = static_cast<int32_t>(levels) - 1;; --level)
            {
                if (level == -1)
                    break;
                const ee::Ptr<FramePart> sourcePart = frameLevelPart(source, level);
                if (sourcePart->mesh != source->part)
                    continue;
                const ee::Ptr<FramePart> copyPart = frameLevelPart(copy, level);
                copy->part = copyPart->mesh;
                addPartReference(copy->part);
                break;
            }
            if (copy->part)
            {
                copy->treeBytes = source->treeBytes;
                copy->tree = ee::Ptr<uint8_t>(heapAllocate(source->treeBytes));
                if constexpr (game::readable::kIsBmx) // (ZGE4's ends in the copy, as a tail call)
                    game::tail(memoryCopy, copy->tree, source->tree, copy->treeBytes);
                else
                    memoryCopy(copy->tree, source->tree, copy->treeBytes);
                return;
            }
            // AI shares the source's collision part when no level holds it; DM2
            // leaves the copy with none.
            const ee::Ptr<ZgePart> shared = source->part;
            if (!game::readable::kIsDm2 && shared)
            {
                copy->part = shared;
                game::tail(addPartReference, shared);
                return;
            }
            copy->tree = ee::Ptr<uint8_t>();
            copy->treeBytes = 0;
        }
    }
    READABLE_FUNCTION_WAITS(cloneFrameCollision, Verified);

    ee::Ptr<FramePart> cloneFramePart(ee::Ptr<FramePart> source, uint32_t copyFlags);

    ee::Ptr<TagTableHeader> setFrameTagTable(ee::Ptr<CollisionFrame> collisionFrame, ee::Ptr<TagTableHeader> table);

    // cloneCollisionFrame: a copy of a frame: its TGFR bits, each level's part
    // (with a copy of the mesh when `flags` asks, kCloneMeshes), its tag
    // table (copied with kCloneTags) and its collision data.
    ee::Ptr<CollisionFrame> cloneCollisionFrame(ee::Ptr<CollisionFrame> source, uint32_t flags)
    {
        const uint32_t meshFlags = flags & kCloneMeshes;
        const ee::Ptr<CollisionFrame> copy(newCollisionFrame());
        copy->flags = static_cast<uint16_t>((copy->flags & ~kFrameFromDiscMask) | (source->flags & kFrameFromDiscMask));
        const auto copyLevel = [&](uint32_t level)
        {
            ee::Ptr<FramePart> part = frameLevelPart(source, level);
            if (meshFlags)
                part = cloneFramePart(part, flags);
            addFrameLevel(copy, part);
        };
        // ZGE2 copies the levels last first (its copy's array holds them reversed), with the count asked once; ZGE3
        // first first, the count asked before each.
        if constexpr (game::readable::kIsDm2)
        {
            for (int32_t level = static_cast<int32_t>(frameLevels(source)) - 1; level >= 0; --level)
                copyLevel(static_cast<uint32_t>(level));
        }
        else
        {
            for (uint32_t level = 0; level < frameLevels(source); ++level)
                copyLevel(level);
        }
        if (ee::Ptr<TagTableHeader> tags = source->tags)
        {
            if (flags & kCloneTags)
                tags = copyTagTable(tags);
            setFrameTagTable(copy, tags);
        }
        cloneFrameCollision(copy, source);
        return copy;
    }
    READABLE_FUNCTION_WAITS(cloneCollisionFrame, Verified);

    int32_t buildFramePartPackets(ee::Ptr<FramePart> part);

    // buildFramePackets: each level's parts' packets built (last level first);
    // the last call's result.
    int32_t buildFramePackets(ee::Ptr<CollisionFrame> frame)
    {
        const int32_t levels = static_cast<int32_t>(frameLevels(frame));
        int32_t result = levels;
        if (levels != 0)
        {
            for (int32_t level = levels - 1;;)
            {
                result = buildFramePartPackets(frameLevelPart(frame, level));
                if (--level == -1)
                    break;
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(buildFramePackets, Verified);

    namespace
    {
        // The pointer array functions ZGE2's level array goes through (zge/container/array.cpp has their bodies). They
        // are declared here, not in array.h: nine other sources still wrap the same names by address, and a declaration
        // there would clash with theirs.
    }
    ee::Ptr<PointerArray> newPointerArray(uint32_t capacity);
    void freePointerArray(ee::Ptr<PointerArray> array);
    uint32_t pointerArrayCount(ee::Ptr<PointerArray> array);
    uint32_t pointerArrayAt(ee::Ptr<PointerArray> array, int32_t index);

    // ZGE2's level array (CollisionFrame::levels; collision.h): the pointer array functions as the frame's code
    // calls them, a null array asked for its count too, as the original does.
    uint32_t frameLevelArrayCount(ee::Ptr<PointerArray> levels) { return pointerArrayCount(levels); }
    ee::Ptr<FramePart> frameLevelArrayAt(ee::Ptr<PointerArray> levels, uint32_t index)
    {
        return ee::Ptr<FramePart>(pointerArrayAt(levels, static_cast<int32_t>(index)));
    }
    ee::Ptr<PointerArray> frameLevelArrayAdd(ee::Ptr<PointerArray> levels, ee::Ptr<FramePart> part)
    {
        if (!levels)
            levels = newPointerArray(4);
        pointerArrayAdd(levels, part);
        return levels;
    }
    void freeFrameLevelArray(ee::Ptr<PointerArray> levels) { freePointerArray(levels); }

    // addFrameLevel: a level added after the frame's last. (The original hands
    // back the frame's address plus four bytes a level; no caller reads it.)
    void addFrameLevel(ee::Ptr<CollisionFrame> frame, ee::Ptr<FramePart> part)
    {
        frame->addLevel(part);
    }
    READABLE_FUNCTION(addFrameLevel, Verified);

    // frameLevelPart (kModelPart): the frame's part of a level of detail.
    ee::Ptr<FramePart> frameLevelPart(ee::Ptr<CollisionFrame> frame, uint32_t level)
    {
        return frame->level(level);
    }
    READABLE_FUNCTION(frameLevelPart, Verified);

    // frameLevels (kModelHasParts): how many levels of detail the frame has.
    uint32_t frameLevels(ee::Ptr<CollisionFrame> frame)
    {
        return frame->levelCount();
    }
    READABLE_FUNCTION(frameLevels, Verified);

    // frameCollisionPart (kFramePart): the frame's collision part.
    ee::Ptr<ZgePart> frameCollisionPart(ee::Ptr<CollisionFrame> frame)
    {
        return frame->part;
    }
    READABLE_FUNCTION(frameCollisionPart, Verified);

    // frameTree (kFrameTree): where the frame's tree and its size are.
    ee::Ptr<ee::Ptr<uint8_t>> frameTree(ee::Ptr<CollisionFrame> frame)
    {
        return &frame->tree;
    }
    READABLE_FUNCTION(frameTree, Verified);

    void framePartBounds(ee::Ptr<FramePart> part, ee::Address box);

    namespace // the game functions below call: ee/plain.h
    {
        void boxUnion(uint32_t box, uint32_t box2, ee::Ptr<uint8_t> box3) { ee::game(kBoxUnion, box, box2, box3); }
    }

    // collisionFrameBounds: the box round all its levels.
    uint32_t collisionFrameBounds(ee::Ptr<CollisionFrame> collisionFrame, ee::Address box)
    {
        ee::Local<uint8_t[0x60]> box2;
        int32_t level = 0;
        if ((collisionFrame->flags & kFrameLevelsMask) != 0)
        {
            for (;;)
            {
                if (level == 0)
                {
                    framePartBounds(frameLevelPart(collisionFrame, 0), box);
                }
                else
                {
                    framePartBounds(frameLevelPart(collisionFrame, level), box2);
                    boxUnion(box, box, box2);
                }
                ++level;
                if (level >= (collisionFrame->flags & kFrameLevelsMask) >> kFrameLevelsShift)
                    break;
            }
        }
        return 0; // (no part left)
    }
    READABLE_FUNCTION(collisionFrameBounds, Verified);

    // setFrameTagTable: a reference to the table in place of its old one. As
    // in setGeometryTags the release is of the new table.
    ee::Ptr<TagTableHeader> setFrameTagTable(ee::Ptr<CollisionFrame> collisionFrame, ee::Ptr<TagTableHeader> table)
    {
        if (collisionFrame->tags)
            releaseTagTable(table);
        collisionFrame->tags = table;
        return game::tail(addTagTableReference, table);
    }
    READABLE_FUNCTION_WAITS(setFrameTagTable, Draft);

    // initFrameParts: the frame parts' pool made (256 a block; ZGE4's 384). 1.
    uint32_t initFrameParts()
    {
        *g_framePartsReady = 1;
        *g_framePartPool = createPool(sizeof(FramePart), game::readable::perGame(0x100u, 0x100u, 0x180u), kReleaseFramePart);
        return 1;
    }
    READABLE_FUNCTION_WAITS(initFrameParts, Verified);

    // shutdownFramePartPool.
    uint32_t shutdownFramePartPool()
    {
        if (*g_framePartsReady != 0)
        {
            destroyPool(ee::Ptr<Pool>(*g_framePartPool));
            *g_framePartPool = 0;
            *g_framePartsReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownFramePartPool, Verified);

    // newFramePart: a new frame part, cleared, with one reference; 0 if the
    // pool gave none.
    uint32_t newFramePart()
    {
        const uint32_t made = poolAlloc(ee::Ptr<Pool>(*g_framePartPool));
        if (made)
        {
            memoryFill(made, 0, sizeof(FramePart));
            ee::Ptr<FramePart>(made)->references = 1;
        }
        return made;
    }
    READABLE_FUNCTION_WAITS(newFramePart, Verified);

    // releaseFramePart (also the pool's free callback): one reference less; with
    // the last, its mesh released and the record handed back. 1.
    uint32_t releaseFramePart(ee::Ptr<FramePart> part)
    {
        if (--part->references == 0)
        {
            releasePart(framePartMesh(part));
            poolFree(ee::Ptr<Pool>(*g_framePartPool), part);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseFramePart, Verified);

    void setFramePartMesh(ee::Ptr<FramePart> part, ee::Ptr<ZgePart> mesh);

    // cloneFramePart: a copy of a frame part (its distance; its mesh copied by
    // clonePart when the flags ask for any stream's copy, else shared).
    ee::Ptr<FramePart> cloneFramePart(ee::Ptr<FramePart> source, uint32_t copyFlags)
    {
        const ee::Ptr<FramePart> clone(newFramePart());
        clone->distance = source->distance;
        const ee::Ptr<ZgePart> mesh = framePartMesh(source);
        if (copyFlags & kCopyAnyStream)
        {
            // The copy's one reference goes to the clone.
            const ee::Ptr<ZgePart> copy = clonePart(mesh, copyFlags);
            setFramePartMesh(clone, copy);
            releasePart(copy);
            return clone;
        }
        setFramePartMesh(clone, mesh);
        return clone;
    }
    READABLE_FUNCTION_WAITS(cloneFramePart, Verified);

    // addFramePartReference: one more reference to a frame part; the count.
    int32_t addFramePartReference(ee::Ptr<FramePart> part)
    {
        const int32_t references = part->references + 1;
        part->references = static_cast<uint16_t>(references);
        return references;
    }
    READABLE_FUNCTION(addFramePartReference, Verified);

    namespace // the game functions below call: ee/plain.h
    {
    namespace tail
    {
        // buildPartChainPackets by address: it is void, and buildFramePartPackets
        // returns what it leaves in v0.
        int32_t buildPartChainPacketsKeepingResult(ee::Ptr<ZgePart> part) { return ee::tailCall<int32_t>(sym::buildPartChainPackets, part); }
    }
    }

    // buildFramePartPackets: the packets of the frame part's mesh chain built where
    // they are new (a tail call).
    int32_t buildFramePartPackets(ee::Ptr<FramePart> part)
    {
        return tail::buildPartChainPacketsKeepingResult(framePartMesh(part));
    }
    READABLE_FUNCTION_WAITS(buildFramePartPackets, Verified);

    // framePartBounds: the box grown to hold the frame part's mesh, if it is a
    // mesh (header 0; a tail call).
    void framePartBounds(ee::Ptr<FramePart> part, ee::Address box)
    {
        const ee::Ptr<ZgePart> mesh = part->mesh;
        if (mesh->header == 0)
        {
            game::tail(partBounds, mesh, box);
            return;
        }
    }
    READABLE_FUNCTION(framePartBounds, Verified);

    // setFramePartMesh: the frame part's mesh set, with a reference to it. When
    // the frame part had one, the original releases the new mesh, not the
    // old one (as it is: the old one keeps the reference).
    void setFramePartMesh(ee::Ptr<FramePart> part, ee::Ptr<ZgePart> mesh)
    {
        // (With a mesh already set, the new one loses a reference, not the
        // old one: the original's bug, kept.)
        if (part->mesh)
            releasePart(mesh);
        part->mesh = mesh;
        game::tail(addPartReference, mesh);
    }
    READABLE_FUNCTION_WAITS(setFramePartMesh, Verified);

    // framePartMesh: the frame part's mesh.
    ee::Ptr<ZgePart> framePartMesh(ee::Ptr<FramePart> part)
    {
        return part->mesh;
    }
    READABLE_FUNCTION(framePartMesh, Verified);
}
