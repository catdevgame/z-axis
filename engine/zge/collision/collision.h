#pragma once

// Collision: how the engine finds what a shape touches (collisionQuery and
// below), and the data it reads.
//
// The world (a grid of cells over the level) holds the scene's nodes; a
// node that collides has a collision frame (Node +0x174) with a part (the
// mesh: positions, faces, face normals, as a GEO part, zge/render/part.h)
// and a face tree (a TGCI chunk): an axis-aligned BSP over the part's faces
// whose leaves list face indices.
//
// A query (SphereQuery, zge/scene/query.h, built in collisionQuery's frame)
// has a kind and a shape:
//   0 sphere          QuerySphere   the cells under the sphere's box, each
//                                   node's box, then its faces
//   1 segment         QuerySegment  the cells under the segment's box, each
//                                   node's sphere and box (QueryHits sorted
//                                   along the segment), then the faces of
//                                   each node in that order, nearest first
//   2 box             QueryBox      (no caller in the game)
//   3 swept box       QuerySweptBox (no caller in the game)
// The faces hit go to QueryResults: per node a HitNode with its FaceHits.
//
// Data on disc (GEO, zge/assets/geo.h), per object:
//   TGFR   the frame: CollisionFrame's flags
//   TGCI   {u32 part id, u32 bytes, the face tree} (loadCollisionTreeChunk): the tree
//          as it is in RAM; the part is the TGPT whose TGPH +0x10 is the id
//   TGPT   the part: TGVP positions (float3 -> float4, w = 1), TGFP faces
//          (Face, 8 bytes, the same in RAM), TGFN / TIFN face normals
//          (float3, TIFN through an index table), TGFT face tags, TGFE face
//          edges (buffer streams)

#include "ee/ee.h"
#include "ee/readable.h"
#include "zge/container/heap.h"
#include "zge/math/types.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"

#include <cstddef>
#include "sym/zge/collision/collision.h.h"

namespace zge
{
    // ---- the world grid ------------------------------------------------------

    // The world (newCollisionWorld: 0x70 bytes): a grid of cells,
    // each a Sector (zge/scene/query.h) with the nodes in it.
#if !ZX_GAME_DM2
    struct CollisionWorld
    {
        Vec4 cellSize;           // +0x00
        Vec4 inverseCellSize;    // +0x10 1 / cellSize
        Vec4 boundsMin;          // +0x20 the grid's corner
        Vec4 boundsMax;          // +0x30
        int32_t cellCount[3];    // +0x40 cells along x, y, z ((max - min) / size, truncated)
        int32_t cells;           // +0x4C cellCount x * y * z
        ee::Ptr<Sector> cell;    // +0x50 [x][y][z], kSectorBytes each
        // +0x54 counts the scene draws: a node's view keeps the last that culled it.
        uint32_t frameStamp;
        ee::Ptr<PointerArray> lists[3]; // +0x58 the world's nodes, objects and textures (cursor arrays of 0x400, 0x100, 0x200)
        ee::Ptr<Node> skyNode; // +0x64 setSkyNode: the node drawSky draws
        uint8_t _68[0x08];
    };
#else
    // ZGE2's world (newCollisionWorld: 0x60 bytes), every offset read in DM2's code (newCollisionWorld, buildWorldGrid,
    // forEachWorldSector, forEachCellInBox, forEachCellOnSegment, addNodeToGrid, reportWorldCells, assignCellRegions,
    // the list walkers, setSkyNode and skyNode, drawScene and its sector visitor's node draw): no inverse cell size
    // (its code divides by cellSize) and no cell total (forEachWorldSector multiplies the three counts), and the area
    // the camera is in kept here (drawScene stores markRegionsOutOfView's result; the node draw hands it to
    // regionInView with the node's region) where ZGE3 keeps it in the draw's SceneFrame. The words of AI's it does
    // not have stand past the end, for the functions with no DM2 counterpart.
    struct CollisionWorld
    {
        Vec4 cellSize;           // DM2 +0x00
        Vec4 boundsMin;          // DM2 +0x10 the grid's corner
        Vec4 boundsMax;          // DM2 +0x20
        int32_t cellCount[3];    // DM2 +0x30 cells along x, y, z ((max - min) / size, truncated)
        ee::Ptr<Sector> cell;    // DM2 +0x3C [x][y][z], kSectorBytes (0x60) each
        uint32_t area;           // DM2 +0x40 the area the camera is in (drawScene; ZGE3: SceneFrame::area)
        uint32_t frameStamp;     // DM2 +0x44 counts the scene draws (drawScene adds 1; a node's view keeps the last)
        ee::Ptr<PointerArray> lists[3]; // DM2 +0x48 the world's nodes, objects and textures (cursor arrays of 0x400, 0x100, 0x200)
        ee::Ptr<Node> skyNode;   // DM2 +0x54 setSkyNode: the node drawSky draws
        uint8_t _58[0x08];
        int32_t cells;           // (ZGE3 and ZGE4 only: the cell total; past DM2's 0x60 bytes)
        Vec4 inverseCellSize;    // (ZGE3 and ZGE4 only; past DM2's 0x60 bytes)
    };
    static_assert(offsetof(CollisionWorld, area) == 0x40 && offsetof(CollisionWorld, cells) == 0x60);
#endif
    static_assert(offsetof(CollisionWorld, boundsMin) == game::readable::perGame(0x20u, 0x10u, 0x20u) &&
                  offsetof(CollisionWorld, boundsMax) == game::readable::perGame(0x30u, 0x20u, 0x30u) &&
                  offsetof(CollisionWorld, cellCount) == game::readable::perGame(0x40u, 0x30u, 0x40u) &&
                  offsetof(CollisionWorld, cell) == game::readable::perGame(0x50u, 0x3Cu, 0x50u) &&
                  offsetof(CollisionWorld, frameStamp) == game::readable::perGame(0x54u, 0x44u, 0x54u) &&
                  offsetof(CollisionWorld, lists) == game::readable::perGame(0x58u, 0x48u, 0x58u) &&
                  offsetof(CollisionWorld, skyNode) == game::readable::perGame(0x64u, 0x54u, 0x64u));
    // (DM2's world is 0x60 bytes; AI's cell total and inverse cell size stand past its end)
    static_assert(sizeof(CollisionWorld) == game::readable::perGame(0x70u, 0x80u, 0x70u));
    constexpr uint32_t kSectorBytes = sizeof(Sector); // (0x50; ZGE2's 0x60: zge/scene/query.h)

    // The sectors a box covers on one axis (16 bytes; only the first two
    // words are written): nodeGridCells fills three, x, y and z.
    struct AxisCells
    {
        int32_t low;
        int32_t high;
        uint32_t _08[2];
    };
    static_assert(sizeof(AxisCells) == 0x10);
    inline ee::Global<ee::Ptr<CollisionWorld>, sym::g_collisionWorld> g_collisionWorld;

    // ---- a node's collision mesh ---------------------------------------------

    // A node of the face tree, 8 bytes. A split halves space on one axis at
    // `plane` (s16, times g_faceTreePlaneScale); its children are word
    // offsets from the tree's start. A leaf lists `count` face indices (u16)
    // at the halfword offset `faces` from the tree's start.
    struct FaceTreeSplit
    {
        uint8_t axis;      // +0x00 1 x, 2 y, 3 z
        uint8_t pad01;
        int16_t plane;     // +0x02
        uint16_t greater;  // +0x04 word offset of the child where the coordinate is greater than the plane
        uint16_t lesser;   // +0x06 word offset of the child on the lesser side
    };
    struct FaceTreeLeaf
    {
        uint8_t axis;      // +0x00 0
        uint8_t pad01;
        uint16_t count;    // +0x02 faces in the leaf
        uint32_t faces;    // +0x04 halfword offset of its u16 face indices
    };
    static_assert(sizeof(FaceTreeSplit) == 8 && sizeof(FaceTreeLeaf) == 8);
    enum : uint8_t
    {
        kFaceTreeLeaf = 0,
        kFaceTreeSplitX = 1,
        kFaceTreeSplitY = 2,
        kFaceTreeSplitZ = 3,
    };
    // A split's plane is plane * this [inferred value: read at run time].
    inline ee::Global<ee::Float, sym::g_faceTreePlaneScale> g_faceTreePlaneScale;

    // A face (TGFP, 8 bytes): its flags and three vertex indices into the
    // part's positions (float4).
    struct Face
    {
        uint16_t flags;     // FaceFlags
        uint16_t vertex[3];
    };
    static_assert(sizeof(Face) == 8);
    enum FaceFlags : uint16_t
    {
        kFaceLowByte = 0x00FF,    // [inferred: the face's surface or material]
        kFaceVertexCount = 0x0F00, // the face's vertex count (3; faceVertices returns it, the loader walks it)
        kFaceFlag1000 = 0x1000,
        kFaceCollides = 0x2000,   // tested unless the query asks for all faces (QueryFlags::kQueryAllFaces)
        kFaceTwoSided = 0x4000,   // hit from behind too
        kFaceFlag8000 = 0x8000,
    };

    // A level of detail's part, as a frame holds it (12 bytes, a pool's
    // record: newFramePart makes one, releaseFramePart releases it, freeing the
    // part with the last reference).
    struct FramePart
    {
        uint16_t references;          // +0x00
        uint16_t alignment02;         // +0x02 distance's alignment
        ee::Float distance;           // +0x04 the file's: the level draws out to this depth (levelForDepth takes the first past it)
        ee::Ptr<ZgePart> mesh;        // +0x08
    };
    static_assert(sizeof(FramePart) == 12 && offsetof(FramePart, mesh) == 0x08);

    // Node +0x174: the node's geometry frame (the original's "GeomFrame",
    // its messages name it so; 40 bytes, a pool's record, newCollisionFrame): the
    // parts drawn, one a level of detail (zge/collision/frame.cpp), and the
    // collision data (TGFR makes it, newCollisionFrame; TGCI fills +0x08 ..
    // +0x10). The tree and part may be null. A part without a tree gets one
    // leaf with every face on its first query (testNodeFaces / queryTestShape:
    // 8 bytes + 2 a face, allocated then).
    constexpr uint16_t kFrameLevelsShift = 4;
    constexpr uint16_t kFrameLevelsMask = 0x00F0u;
    struct FramePart;
    struct PointerArray;
    // ZGE2's level array (CollisionFrame::levels below) through the pointer array functions (zge/container/array.cpp;
    // frame.cpp): how many items, the item at an index, an item added (the array made four wide at the first: the array
    // then), and the array freed.
    uint32_t frameLevelArrayCount(ee::Ptr<PointerArray> levels);
    ee::Ptr<FramePart> frameLevelArrayAt(ee::Ptr<PointerArray> levels, uint32_t index);
    ee::Ptr<PointerArray> frameLevelArrayAdd(ee::Ptr<PointerArray> levels, ee::Ptr<FramePart> part);
    void freeFrameLevelArray(ee::Ptr<PointerArray> levels);
    // The levels of detail live where each engine keeps them: ZGE3 and ZGE4 up to four in the record (parts[]), ZGE2 in
    // a pointer array made when the first is added (levels; its record is 0x1C bytes). Each layout has the same four
    // accessors, so frame.cpp reads the same either way.
    struct CollisionFrame
    {
        // +0x00 bits 0-3, 8-11, 12-15 from TGFR's bytes 0, 2, 3; bits 4-7
        // the levels of detail in use.
        uint16_t flags;
        uint16_t references;          // +0x02 nodes sharing it (1 when made)
#if ZX_GAME_DM2
        ee::Ptr<PointerArray> levels; // DM2 +0x04 the levels of detail, in the order added (zge/container/array.h)
        uint32_t _08;                 // DM2 +0x08
#else
        uint32_t _04;
#endif
        ee::Ptr<uint8_t> tree;        // +0x08 (DM2 +0x0C) the face tree (FaceTreeSplit / FaceTreeLeaf at its start)
        uint32_t treeBytes;           // +0x0C (DM2 +0x10)
        union
        {
            ee::Ptr<ZgePart> part;    // +0x10 (DM2 +0x14) the collision mesh (header 0: a mesh part)
            uint32_t meshWord;        // the same, as the word the skater code hands round
        };
        ee::Ptr<TagTableHeader> tags; // +0x14 (DM2 +0x18) its tag table (zge/assets/tag_table.h), 0: none
#if ZX_GAME_DM2
        // How many levels (asked of the array, a null one too, as the original), the part of one, one added after the
        // last (the array made four wide at the first; bits 4-7 then say how many), and the array freed with the frame.
        // The array's functions are frame.cpp's (frameLevelArrayCount and the others below).
        uint32_t levelCount() const { return frameLevelArrayCount(levels); }
        ee::Ptr<FramePart> level(uint32_t index) const { return frameLevelArrayAt(levels, index); }
        void addLevel(ee::Ptr<FramePart> part)
        {
            levels = frameLevelArrayAdd(levels, part);
            flags = static_cast<uint16_t>((flags & ~kFrameLevelsMask) | (frameLevelArrayCount(levels) & 0xFu) << kFrameLevelsShift);
        }
        void freeLevels()
        {
            if (levels)
                freeFrameLevelArray(levels);
        }
        // AI's level slots, past ZGE2's 0x1C-byte record: only ZGE3 code with no DM2 counterpart names them
        // (zge/render/node_tree.cpp's level pick).
        ee::Ptr<FramePart> parts[4];
#else
        ee::Ptr<FramePart> parts[4];  // +0x18 the levels of detail, nearest first
        // How many levels (bits 4-7), the part of one, one added after the last; the levels go with the record.
        uint32_t levelCount() const { return (flags & 0xFFu) >> kFrameLevelsShift; }
        ee::Ptr<FramePart> level(uint32_t index) { return ee::Ptr<ee::Ptr<FramePart>>(parts)[index]; }
        void addLevel(ee::Ptr<FramePart> part)
        {
            const uint32_t count = (flags >> kFrameLevelsShift) & 0xFu;
            flags = static_cast<uint16_t>((flags & ~kFrameLevelsMask) | ((count + 1) & 0xFu) << kFrameLevelsShift);
            parts[count] = part;
        }
        void freeLevels() {}
#endif
    };
    // The frame record as the game's pool allocates it (DM2's sizeof has AI's slots past the record).
    constexpr uint32_t kCollisionFrameBytes = game::readable::perGame(0x28u, 0x1Cu, 0x28u);
    static_assert(game::readable::kIsDm2 ? offsetof(CollisionFrame, tags) + 4 == kCollisionFrameBytes : sizeof(CollisionFrame) == kCollisionFrameBytes);
    static_assert(offsetof(CollisionFrame, tree) == game::readable::perGame(0x08u, 0x0Cu, 0x08u) &&
                  offsetof(CollisionFrame, part) == game::readable::perGame(0x10u, 0x14u, 0x10u) &&
                  offsetof(CollisionFrame, tags) == game::readable::perGame(0x14u, 0x18u, 0x14u));
    // The TGFR bits a copy keeps: ZGE2's copies take the levels nibble too (its level adds then set it again).
    constexpr uint16_t kFrameFromDiscMask = game::readable::perGame(0xFF0Fu, 0xFFFFu, 0xFF0Fu);

    // setTreeLevelOfDetail (zge/collision/frame.cpp): the level of detail of a node's tree.
    int32_t setTreeLevelOfDetail(ee::Ptr<Node> node, uint32_t level);

    // The part's faces, one at a time (zge/collision/mesh.cpp).
    uint32_t faceVertices(ee::Ptr<ZgePart> part, uint32_t face, ee::Ptr<uint16_t> *vertices);
    ee::Ptr<Vec4> faceNormal(ee::Ptr<ZgePart> part, uint32_t face, ee::Ptr<Vec4> normal);
    ee::Ptr<Vec4> vertexPosition(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<Vec4> position);
    // An edge's two vertex indices into a static pair; *vertices
    // points at it. edgeVertices: edge `corner` of a face; elementEdgeVertices:
    // a face-edge element (three a face).
    void edgeVertices(ee::Ptr<ZgePart> part, int32_t face, int32_t corner, ee::Ptr<uint16_t> *vertices);
    void elementEdgeVertices(ee::Ptr<ZgePart> part, int32_t element, ee::Ptr<uint16_t> *vertices);
    // Vertex index `corner` of a face, as faceVertices' callers read it at
    // the address it handed them (`handed`, the face's indices in the game's
    // RAM copy): from the host part when it has the face
    // (ZX_NATIVE_COLLISION; =verify compares with RAM), else at `handed`.
    // Readable callers read the indices through this instead of RAM.
    uint16_t faceVertexIndex(ee::Ptr<ZgePart> part, uint32_t face, ee::Ptr<uint16_t> handed, uint32_t corner);

    // ---- face tags and edges -------------------------------------------------

    // Face tags (TGFT, ZgePart::faceTags) and face edges (TGFE,
    // ZgePart::faceEdges) are tag tables (newBuffer makes one): a 16-byte
    // header, an index per element (a face; a face's edge, three a face),
    // then at the next 16 bytes the records the indices pick (0xFF / 0xFFFF:
    // none). tagElementRecord (table, element) gives an element's record or 0;
    // tagByte (record, _, offset) reads a byte of it, setTagWord
    // (record, _, offset, value) writes a word. On disc a chunk is the
    // descriptor and the index count, then the table's data as it is in RAM.
    struct TagTable
    {
        uint8_t type;           // +0x00 6 (the engine's object type; a stream's is 7)
        uint8_t _01;
        uint16_t references;    // +0x02 addTagTableReference adds one, releaseTagTable takes one
        uint32_t descriptor;    // +0x04 bits 0-3: the index width's kind, 4-13: a record's bytes, 14-31: records
        uint32_t count;         // +0x08 indices
        uint32_t heap;          // +0x0C the heap it came from
        // +0x10 the indices, then the records at the next 16 bytes
    };
    static_assert(sizeof(TagTable) == 0x10);
    // Index bytes by the descriptor's kind (kind 0: u8, 1: u16).
    inline ee::Global<uint32_t[16], sym::g_tagIndexWidth> g_tagIndexWidth;

    // A face's tag record (28 bytes). The level's setup (setupCollisionPart, the
    // game side) fills two words when it loads the level: faces of kind 1 or
    // 2 get the nearest edge of a matching kind, kind 0xB faces their own
    // index.
    struct FaceTagRecord
    {
        uint32_t eventKind;     // +0x00 the surface's kind as the "Jump" and "Land" events carry it
        uint32_t nearestEdge;   // +0x04 filled at load (kinds 1, 2): a face-edge element
        uint32_t face;          // +0x08 filled at load (kind 0xB): the face
        uint8_t kind;           // +0x0C the surface's kind (the rail search findWall skips 1, 2 and 13) [inferred]
        uint8_t _0d[3];
        ee::Float gravity;      // +0x10 a slope's gravity (a pipe's level speed scale)
        ee::Float launchCap;    // +0x14 a take-off's cap on velocity.y
        ee::Float launchPush;   // +0x18 a take-off's push up
    };
    static_assert(sizeof(FaceTagRecord) == 28);

    // A face edge's record (24 bytes); the level's setup sets +0x04 to the
    // last face-edge element that picks it. The game reads a rail's fields
    // through the tag accessors (aggressive/skater/surface.cpp: the grinds,
    // the poles).
    struct EdgeRecord
    {
        uint32_t eventId;       // +0x00 the word its Grind events carry (the edge's id in its object)
        uint32_t element;       // +0x04 filled at load: the face-edge element (3 x face + edge)
        uint8_t kind;           // +0x08 1 or 9 go with kind-1 faces, 2 with kind 2 [inferred: rails, ledges]
        uint8_t flags;          // +0x09 bit 0: a search of no kind takes this edge
        uint8_t _0a;
        uint8_t material;       // +0x0B the grind sound and rumble's: 0, 2, 8 metal, 1 wood
        ee::Float launchMin;    // +0x0C launched faster than this off the end (a pole: the exit heading)
        ee::Float launchMax;    // +0x10 and slower than this, 0 for no limit (a pole: its lift)
        ee::Float launchSpeed;  // +0x14 then at this speed, 0 for none (a pole: the exit speed)
    };
    static_assert(sizeof(EdgeRecord) == 24 && offsetof(EdgeRecord, material) == 0x0B &&
                  offsetof(EdgeRecord, launchSpeed) == 0x14);

    // ---- a query ---------------------------------------------------------------

    // SphereQuery::kind (zge/scene/query.h QueryKind names them by walker).
    enum class CollisionKind : uint32_t
    {
        Sphere = 0,
        Segment = 1,
        Box = 2,
        SweptBox = 3,
    };

    struct QuerySphere
    {
        Vec4 centre;          // +0x00
        ee::Float radius;     // +0x10
        uint8_t _14[0x0C];
    };
    static_assert(sizeof(QuerySphere) == 0x20);
    struct QueryBox
    {
        Vec4 min;
        Vec4 max;
    };
    // A box swept along a segment: start, end, and its half width in
    // halfWidth.x (collisionQuery turns it into an oriented box).
    struct QuerySweptBox
    {
        Vec4 start;
        Vec4 end;
        Vec4 halfWidth;
    };
    static_assert(sizeof(QuerySweptBox) == 0x30);

    // A tree made at run time for a part without one: a single leaf, its faces after it.
    struct WholePartTree
    {
        FaceTreeLeaf leaf;
        uint16_t faces[1];
    };
    // ZGE2's leaf as its trees keep it (queryTestShape's whole-part tree; sphereFaceTree reads it so): the face
    // indices right after the count, no offset word. (ZGE3 added the offset, FaceTreeLeaf::faces.)
    struct InlineLeafTree
    {
        uint8_t axis;      // +0x00 kFaceTreeLeaf
        uint8_t pad01;
        uint16_t count;    // +0x02 faces in the leaf
        uint16_t faces[1]; // +0x04 their indices
    };
    static_assert(offsetof(InlineLeafTree, count) == 0x02 && offsetof(InlineLeafTree, faces) == 0x04);

    // collisionQuery's oriented box around a swept box.
    struct QueryOrientedBox
    {
        uint8_t _00[0x30];
        Vec4 start;      // +0x30
        Vec4 end;        // +0x40
        Matrix matrix;   // +0x50
        Vec4 centre;     // +0x90
        Vec4 extents;    // +0xA0
    };
    static_assert(sizeof(QueryOrientedBox) == 0xB0 && offsetof(QueryOrientedBox, matrix) == 0x50 && offsetof(QueryOrientedBox, extents) == 0xA0);

    // The query's shape, by its kind (SphereQuery::segment).
    template <typename T, typename Query>
    const T &queryShape(const Query &query) { return *query.segment.template cast<T>(); }

    // SphereQuery::flags (the low ones are zge/scene/query.h's).
    enum QueryFlags : uint32_t
    {
        kQueryWorldPoints = 0x0004,    // FaceHit::point in world space (else the node's)
        kQueryRefresh = 0x0020,        // a cache is out of date: query again
        kQueryCacheLimits = 0x0040,    // the cache's own move and age limits (QueryCache +0x14 .. +0x1C)
        kQueryAllFaces = 0x0080,       // faces without kFaceCollides too
        kQueryDrawHitsMask = 0x1C00,   // debug: draw the faces hit, in a colour from these bits (drawQueryHits)
        kQueryDrawColourMask = 0xE000, // debug: the query's colour [inferred: written, never read]
    };

    // A face a query hit (0x30 bytes, in a pool's buffer).
    struct FaceHit
    {
        uint32_t face;         // +0x00 the face's index in the part
        uint8_t _04[0x0C];
        Vec4 point;            // +0x10 where (QueryFlags::kQueryWorldPoints: in world space)
        ee::Float distance;    // +0x20 segments: how far along; spheres: from the face's plane
        uint8_t _24[0x0C];
    };
    static_assert(sizeof(FaceHit) == 0x30 && offsetof(FaceHit, point) == 0x10 && offsetof(FaceHit, distance) == 0x20);

    // A node with faces hit (a pool record).
    struct HitNode
    {
        ee::Ptr<Node> node;        // +0x00
        ee::Ptr<ZgePart> part;     // +0x04 its collision part
        uint32_t faceCount;        // +0x08
        ee::Ptr<FaceHit> faces;    // +0x0C
    };
    static_assert(sizeof(HitNode) == 0x10);

    // What a query found (queryResultsAddress, copied to the caller's).
    struct QueryResults
    {
        int32_t count;                 // +0x00
        ee::Ptr<HitNode> nodes[128];   // +0x04
    };
    static_assert(sizeof(QueryResults) == 0x204);
    // ZGE2's query results (8 bytes at queryResultsAddress and in every caller's results record): the count and
    // where the HitNodes are, the query state's own array (QueryState::hitNodes). Read in DM2's collisionQuery
    // (the copy to the caller's) and updateDynamics (its results record, 8 bytes). ZGE3 made it the count and
    // 128 pointers (QueryResults); DM2's instance keeps that type for the shared code that does not run there.
    struct HitNodeTable
    {
        int32_t count;                 // DM2 +0x00
        ee::Ptr<HitNode> nodes;        // DM2 +0x04
    };
    static_assert(sizeof(HitNodeTable) == 0x08);
    constexpr uint32_t kQueryResultsAddress = sym::queryResultsAddress;

    // A caller's cache of a query (collisionQuery's last argument): the last
    // shape and results, used again while the shape moves less than a limit
    // and the results are young enough.
    struct QueryCache
    {
        uint32_t valid;            // +0x00
        uint32_t hasResults;       // +0x04
        uint32_t kind;             // +0x08
        uint32_t flags;            // +0x0C without kQueryRefresh
        uint32_t time;             // +0x10 collisionClock's clock when the results were kept
        ee::Float moveLimit;       // +0x14 with kQueryCacheLimits (else g_cacheMoveLimit)
        ee::Float sizeLimit;       // +0x18 with kQueryCacheLimits (else 0.25)
        uint32_t maxAge;           // +0x1C with kQueryCacheLimits (else 100)
        uint8_t shape[0x30];       // +0x20 the shape (0x20 bytes, 0x30 for kind 3)
        QueryResults results;      // +0x50 copies of the HitNodes, in the cache's pools
        ee::Ptr<FaceHit> faces;    // +0x254 their FaceHits: every copy points at its start (collisionQuery), so with several nodes they overwrite each other
    };
    static_assert(offsetof(QueryCache, shape) == 0x20 && offsetof(QueryCache, results) == 0x50 &&
                  offsetof(QueryCache, faces) == 0x254);

    // The query's working state (g_queryState ..; zge/scene/query.h kQueryResults). ZGE2 keeps the HitNodes,
    // the FaceHits and the tree walk's stack in the state itself (one 0xD850-byte block, initQueries clears it)
    // and has one pool, the QueryHits'; ZGE3 moved the records into pools and added the cache's. Each version's
    // fields the other has not got stand past the end of its block, for the functions with no counterpart there.
    constexpr uint32_t kQueryStateHitNodes = 128; // ZGE2's HitNode array (ZGE3: 0x80 records a pool block)
    constexpr uint32_t kQueryStateFaceHits = 0x400; // ZGE2's FaceHit array (ZGE3: 0x200 a pool record)
    constexpr uint32_t kQueryWalkStackBytes = 0x1000; // ZGE2's tree walk stack: 64 entries of 0x40
#if ZX_GAME_DM2
    struct QueryState
    {
        uint32_t ready;                      // DM2 +0x0000 1 from initQueries to shutdownCollisionQueries
        ee::Ptr<Node> node;                  // DM2 +0x0004 the node being tested (filters get it)
        HitNode hitNodes[kQueryStateHitNodes]; // DM2 +0x0008 the nodes hit, in order (QueryResults points here)
        ee::Ptr<HitNode> hitNode;            // DM2 +0x0808 the HitNode being filled (the last recorded)
        int32_t hitCount;                    // DM2 +0x080C HitNodes recorded (0 at a query's start)
        FaceHit faces[kQueryStateFaceHits];  // DM2 +0x0810 the FaceHits, every node's in turn
        uint32_t faceCount;                  // DM2 +0xC810 FaceHits in it
        uint8_t _c814[0x0C];
        uint8_t walkStack[kQueryWalkStackBytes]; // DM2 +0xC820 the face trees' walk stack (sphereFaceTree, segmentFaceTree)
        ee::Float nearest;                   // DM2 +0xD820 the nearest fraction yet (10 at the start)
        PriorityQueue leafList;              // DM2 +0xD824 tree leaves to test, sorted (priorityQueueInsert)
        PriorityQueue hitList;               // DM2 +0xD834 QueryHits, sorted along the segment
        uint32_t queryHitPool;               // DM2 +0xD844
        uint8_t _d848[0x08];
        uint32_t hitNodePool;                // (ZGE3 and ZGE4 only, from here on: past ZGE2's 0xD850 bytes)
        uint32_t cacheHitNodePool;
        uint32_t faceBufferPool;
        uint32_t cacheFaceBufferPool;
        ee::Ptr<FaceHit> facesBuffer;
    };
    static_assert(offsetof(QueryState, hitNode) == 0x808 && offsetof(QueryState, hitCount) == 0x80C &&
                  offsetof(QueryState, faces) == 0x810 && offsetof(QueryState, faceCount) == 0xC810 &&
                  offsetof(QueryState, walkStack) == 0xC820 && offsetof(QueryState, nearest) == 0xD820 &&
                  offsetof(QueryState, hitList) == 0xD834 && offsetof(QueryState, queryHitPool) == 0xD844 &&
                  offsetof(QueryState, hitNodePool) == 0xD850);
    constexpr uint32_t kQueryStateBytes = 0xD850;
#else
    struct QueryState
    {
        uint32_t ready;                      // +0x00 1 from initQueries to shutdownCollisionQueries
        ee::Ptr<Node> node;                  // +0x04 the node being tested (filters get it)
        ee::Ptr<HitNode> hitNode;            // +0x08 the HitNode being filled (0: none yet)
        uint32_t hitNodePool;                // +0x0C pools (poolAlloc allocates, poolReset resets)
        uint32_t cacheHitNodePool;           // +0x10
        uint32_t faceBufferPool;             // +0x14
        uint32_t cacheFaceBufferPool;        // +0x18
        ee::Ptr<FaceHit> facesBuffer;        // +0x1C this query's FaceHit buffer (a pool record)
        uint32_t faceCount;                  // +0x20 FaceHits in it
        ee::Float nearest;                   // +0x24 the nearest fraction yet (10 at the start)
        PriorityQueue leafList;              // +0x28 tree leaves to test, sorted (priorityQueueInsert)
        PriorityQueue hitList;               // +0x38 QueryHits, sorted along the segment
        uint32_t queryHitPool;               // +0x48
        HitNode hitNodes[kQueryStateHitNodes]; // (ZGE2 only, from here on: past ZGE3's 0x4C bytes)
        int32_t hitCount;
        FaceHit faces[kQueryStateFaceHits];
        uint8_t walkStack[kQueryWalkStackBytes];
    };
    static_assert(offsetof(QueryState, nearest) == 0x24 && offsetof(QueryState, hitList) == 0x38 &&
                  offsetof(QueryState, queryHitPool) == 0x48 && offsetof(QueryState, hitNodes) == 0x4C);
    constexpr uint32_t kQueryStateBytes = 0x110; // (what initQueries clears: the state and the words after it)
#endif
    inline ee::Global<QueryState, sym::g_queryState> g_queryState;
    // The query's number (SphereQuery::stamp), one more per query.
    inline ee::Global<uint8_t, sym::g_queryStamp> g_queryStamp;
}

namespace zge
{
    // (frame.cpp)
    ee::Ptr<CollisionFrame> nodeCollisionFrame(ee::Ptr<Node> node);
    void setNodeMaterialSet(ee::Ptr<Node> node, uint32_t set);
    ee::Ptr<CollisionFrame> setLodTable(ee::Ptr<Node> node, uint32_t table);
    uint32_t setNodeShown(ee::Ptr<Node> node, uint32_t shown, uint32_t wholeTree);
    uint32_t setNodeCollides(ee::Ptr<Node> node, uint32_t collides, uint32_t wholeTree);
    uint32_t nodeShown(ee::Ptr<Node> node);
    uint32_t nodeCollides(ee::Ptr<Node> node); // (DM2 only)
    void resetMatrices(ee::Ptr<Matrix> first);
    uint32_t initCollisionFrames();
    uint32_t newCollisionFrame();
    uint32_t releaseCollisionFrame(ee::Ptr<CollisionFrame> frame);
    ee::Ptr<CollisionFrame> cloneCollisionFrame(ee::Ptr<CollisionFrame> source, uint32_t flags);
    int32_t buildFramePackets(ee::Ptr<CollisionFrame> frame);
    void addFrameLevel(ee::Ptr<CollisionFrame> frame, ee::Ptr<FramePart> part);
    ee::Ptr<FramePart> frameLevelPart(ee::Ptr<CollisionFrame> frame, uint32_t level);
    uint32_t frameLevels(ee::Ptr<CollisionFrame> frame);
    ee::Ptr<ZgePart> frameCollisionPart(ee::Ptr<CollisionFrame> frame);
    ee::Ptr<ee::Ptr<uint8_t>> frameTree(ee::Ptr<CollisionFrame> frame);
    uint32_t collisionFrameBounds(ee::Ptr<CollisionFrame> collisionFrame, ee::Address box);
    uint32_t initFrameParts();
    uint32_t newFramePart();
    ee::Ptr<ZgePart> framePartMesh(ee::Ptr<FramePart> part);
}

namespace zge
{
    // (terrain.cpp)
    uint32_t initTerrain();
    uint32_t shutdownTerrain();
    uint32_t shutdownWorldCellTree(); // (ZGE2 only)
    uint32_t initWorldCellTree();     // (ZGE2 only)
    ee::Ptr<CollisionWorld> newCollisionWorld(ee::Ptr<Vec4> cellSize, ee::Ptr<Box> bounds);
    uint32_t releaseCollisionWorld(ee::Ptr<CollisionWorld> world);
    void setSceneFileMode(uint32_t mode);
    uint32_t finishCollisionWorld();
    uint32_t forEachWorldObject(uint32_t callback, ee::Address context);
    uint32_t forEachWorldMesh(uint32_t callback, ee::Address context);
    ee::Ptr<Vec4> collisionWorldBounds();
    void addWorldObject(ee::Address object);
    uint32_t removeWorldObject(ee::Address object);
    uint32_t findWorldObjectByHash(uint32_t nameHash);
    int32_t addWorldNode(ee::Ptr<Node> node);
    int32_t removeWorldNode(ee::Ptr<Node> node);
    ee::Ptr<Node> findWorldNodeByHash(uint32_t nameHash, int32_t nth, uint32_t subtrees);
    void addWorldTexture(uint32_t texture);
    uint32_t zglSceneFindTextureFromHash(uint32_t nameHash);
    int32_t moveNodeCells(ee::Ptr<Node> node, ee::Ptr<AxisCells> from, ee::Ptr<AxisCells> to);
    int32_t addNodeToGrid(ee::Ptr<Node> node, uint32_t add);
    void terrainNoop();
    int32_t drawSky();
}

namespace zge
{
    // (world.cpp)
    uint32_t collisionClock();
    int32_t queryOverlapsBox(uint32_t kind, ee::Ptr<SphereQuery> query, ee::Address box, ee::Ptr<Matrix> matrix);
    int32_t queryTestShape(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query);
    // ZGE2's queryOverlapsBox and queryTestShape (world.cpp): the shape and no matrix; the node's stamp tested there.
    int32_t shapeBoundsOverlapBox(uint32_t kind, ee::Ptr<QuerySegment> shape, ee::Address box);
    int32_t queryTestShapeOnce(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query);
    int32_t testHitNodeFaces(ee::Ptr<Node> node, ee::Ptr<SphereQuery> query);
    // testNodeFaces (faces.cpp): a node against a query's shape, in the node's space; whether any face was hit.
    int32_t testNodeFaces(ee::Ptr<Node> node, ee::Ptr<CollisionFrame> collision, ee::Ptr<ZgePart> part, ee::Ptr<SphereQuery> query);
    int32_t forEachCellOnSegment(uint32_t visit, ee::Ptr<QuerySegment> segment, ee::Address context);
    int32_t forEachCellInSphere(uint32_t visit, ee::Ptr<QuerySphere> sphere, ee::Address context);
    int32_t forEachCellInBox(uint32_t visit, const QueryBox *box, ee::Address context);
    int32_t forEachCellInOrientedBox(uint32_t visit, const QueryBox *localBox, const Matrix *boxToWorld, ee::Address context);
    // boxExtentAlong (faces.cpp): a box's centre and reach along an axis.
    void boxExtentAlong(const Vec4 *centre, const Vec4 *halfSize, const Matrix *boxToWorld, const Vec4 *axis, ee::Float *centreOut, ee::Float *extentOut);
    int32_t setGlobalTimeScale(ee::Float fixedFrameStep);
    void setGlobalColourScale(ee::Float clutRedScale, ee::Float clutGreenScale, ee::Float clutBlueScale);
    uint32_t collisionWorldHook();
}

namespace zge
{
    // (query.cpp)
    bool collisionQuery(uint32_t kind, ee::Address shape, uint32_t flags, uint32_t filter, ee::Address filterContext, ee::Address nodes, ee::Ptr<QueryResults> results, ee::Ptr<QueryCache> cache);
    // ZGE2's collisionQuery (zge/collision/query.cpp): no cache argument.
    int32_t collisionQueryUncached(uint32_t kind, ee::Address shape, uint32_t flags, uint32_t filter, ee::Address filterContext, ee::Address nodes, ee::Ptr<HitNodeTable> results);
    uint32_t initQueries();
    uint32_t shutdownCollisionQueries();
}
