#pragma once

// A level's geometry file (.GEO) as typed records, read from its chunk tree
// and written back as the bytes the game loads: objects (TGEO) with their
// header, parts (TGPT: positions, faces, triangle strips, vertex streams,
// tag buffers), levels of detail, material tables and collision trees. The
// layouts are the engine's loaders' (game/zge/assets/geo.cpp,
// scene/geometry_nodes.cpp, collision/collision.h); every stock level of
// the three games reads and writes back byte for byte (the tooling's
// ai_level_write.py, dm2_level_write.py and bmx_level_write.py, the
// reference, prove the same).
//
// What differs by engine (Engine):
// - ZGE3 (AI): tag records of 24 (edges) and 28 (faces) bytes, indexed
//   streams (TIV*, TIFN), collision trees whose leaves point at face lists
//   after the nodes.
// - ZGE2 (DM2): tag records of 8 and 12 bytes (BikeEdgeTag, BikeFaceTag),
//   plain streams only, strips without the winding mark, trees whose leaves
//   hold their faces inline (preorder, the greater side first).
// - ZGE4 (BMX XXX): ZGE3's layout with the bike games' tag records and the
//   corner tags (TGFV).

#include "collection/levels/chunks.h"

#include <array>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ps2x::app::levels
{
    using Vec3 = std::array<double, 3>;
    using Quat = std::array<double, 4>; // x, y, z, w

    // q's rotation of v (the engines' and the Python tools' formula, in the
    // same order of operations).
    Vec3 quatRotate(const Quat &q, const Vec3 &v);
    Quat quatMultiply(const Quat &a, const Quat &b);

    // A TGFP record (8 bytes): flags (the low byte the material, 0x300 three
    // vertices, COLLIDES, TWO_SIDED) and three vertex indices.
    struct Face
    {
        static constexpr uint16_t kCollides = 0x2000;
        static constexpr uint16_t kTwoSided = 0x4000;

        uint16_t flags = 0;
        std::array<uint16_t, 3> vertices{};

        int material() const { return flags & 0xFF; }
        static Face make(int material, int a, int b, int c, bool collides, bool twoSided);
    };

    // A TGFM group: the material it draws with, a mode byte and its entries
    // (a vertex index in bits 0-13, kFlipped: the triangle's winding
    // reversed, kNoTriangle: no triangle ends there, a strip's restart).
    struct StripGroup
    {
        static constexpr uint16_t kFlipped = 0x4000;
        static constexpr uint16_t kNoTriangle = 0x8000;
        static constexpr size_t kGroupEntries = 60; // the VU1 buffer's

        uint8_t submesh = 0;
        uint8_t mode = 0;
        std::vector<uint16_t> entries;

        static std::vector<StripGroup> readAll(const Bytes &data);
        static Bytes writeAll(const std::vector<StripGroup> &groups);
        // One strip (vertex indices) cut into groups of at most
        // `maxEntries`: odd entries flipped, a group that continues a strip
        // starting with the two entries before it, drawing nothing.
        static std::vector<StripGroup> fromStrip(int submesh, const std::vector<int> &strip, int mode = 0,
                                                 size_t maxEntries = kGroupEntries);
    };

    // A tag buffer (TGFE edges, TGFT faces, TGFV corners): fixed-size
    // records and one index per element (-1: none). The records are kept as
    // their bytes; the typed views below read and make them.
    struct TagBuffer
    {
        std::vector<Bytes> records;
        std::vector<int> indices;

        // Throws LevelError unless the records are `recordSize` bytes.
        static TagBuffer read(const Bytes &data, size_t recordSize);
        Bytes write() const;
    };

    // AI's TGFE record (24 bytes): a face edge's grind data. kind 1 or 9 a
    // quarter pipe's lip, 2 a spine's, 3 a vertical pole, 4 a horizontal
    // one; flags bit 0 grindable; material the grind sound (1 wood, 2 and 8
    // metal).
    struct EdgeTag
    {
        static constexpr size_t kSize = 24;
        static constexpr uint8_t kGrindable = 1;

        uint32_t eventId = 0;
        uint32_t element = 0;
        uint8_t kind = 0;
        uint8_t flags = 0;
        uint8_t byte0a = 3; // 3 in every stock record, read by nothing
        uint8_t material = 0;
        double launchMin = 0.0, launchMax = 0.0, launchSpeed = 0.0;

        static EdgeTag read(const Bytes &record);
        Bytes write() const;
    };

    // AI's TGFT record (28 bytes): a face's surface kind (1 a quarter pipe,
    // 2 a spine, 3 a slope with `gravity`, 8 a wall ride).
    struct FaceTag
    {
        static constexpr size_t kSize = 28;

        uint32_t eventKind = 0;
        uint32_t nearestEdge = 0;
        uint32_t face = 0;
        uint8_t kind = 0;
        std::array<uint8_t, 3> leftover{};
        double gravity = 0.0, launchCap = 0.0, launchPush = 0.0;

        static FaceTag read(const Bytes &record);
        Bytes write() const;
    };

    // The bike games' TGFE record (8 bytes; zge_tags.py EdgeTag8): link kind
    // (1 a quarter pipe's lip, 2 a spine's), flags bit 0 grindable, the
    // grind side (3: by the face's normal) and the wall kind.
    struct BikeEdgeTag
    {
        static constexpr size_t kSize = 8;
        static constexpr uint8_t kGrindable = 1;

        uint32_t tag = 0;
        uint8_t linkKind = 0;
        uint8_t flags = 0;
        uint8_t grindSide = 3;
        uint8_t wallKind = 0;

        bool grindable() const { return flags & kGrindable; }
        static BikeEdgeTag read(const Bytes &record);
        Bytes write() const;
    };

    // The bike games' TGFT record (12 bytes; zge_tags.py FaceTag12): the
    // surface kind (1 a quarter pipe, 2 a spine, 3 a kicker, BMX XXX's 8 a
    // wall) and the launch byte.
    struct BikeFaceTag
    {
        static constexpr size_t kSize = 12;

        uint32_t tag = 0;
        uint32_t link = 0;
        uint8_t surfaceKind = 0;
        uint8_t launch = 50;
        std::array<uint8_t, 2> leftover{};

        static BikeFaceTag read(const Bytes &record);
        Bytes write() const;
    };

    // A collision tree node: a split (axis 1 x, 2 y, 3 z; the plane in
    // hundredths; the children where the coordinate is greater and lesser)
    // or a leaf (axis 0) listing faces of the collision part. `leftover` is
    // the node's second byte and `tail` an inline leaf's two bytes after an
    // odd face count: the exporter's, read by nothing, kept for the round
    // trip.
    struct FaceTreeNode
    {
        uint8_t axis = 0;
        uint8_t leftover = 0;
        int16_t plane = 0;
        std::unique_ptr<FaceTreeNode> greater, lesser;
        std::vector<uint16_t> faces;
        Bytes tail;

        bool leaf() const { return axis == 0; }
    };

    // TGCI: the part it is for and the face tree. ZGE3 and ZGE4 keep the
    // leaves' face lists after the nodes; ZGE2 inline.
    struct CollisionTree
    {
        static constexpr size_t kLeafFaces = 8;
        static constexpr int kDepth = 11;

        uint32_t partId = 0;
        std::unique_ptr<FaceTreeNode> root;

        static CollisionTree read(const Bytes &data, Engine engine);
        Bytes write(Engine engine) const;
        // The faces the tree lists.
        std::set<int> faces() const;
        // A tree as the games' exporter built theirs: the faces that collide,
        // split at the median of their centres on their bounds' longest axis
        // (in hundredths), a face on both sides in both, leaves of at most 8
        // or at depth 11. Empty when nothing collides.
        static std::optional<CollisionTree> build(uint32_t partId, const std::vector<Vec3> &positions,
                                                  const std::vector<Face> &faces);
    };

    // An indexed vertex or face stream (TIVC, TIVN, TIVU, TIFN): a table of
    // distinct elements and one index per vertex (face) into it.
    struct IndexedStream
    {
        std::vector<Bytes> table;
        std::vector<uint16_t> indices;
        bool wide = false; // TII6 (16-bit indices), else TII8

        static IndexedStream fromChunk(const Chunk &chunk, size_t stride, size_t count);
        Chunk write(Tag t) const;
        Bytes expand() const;
        // The elements, equal ones shared.
        static IndexedStream make(const std::vector<Bytes> &elements);
    };

    // A per-vertex (per-face) stream, plain or indexed.
    struct Stream
    {
        bool indexed = false;
        Bytes plain;
        IndexedStream table;

        static Stream ofPlain(Bytes bytes);
        static Stream ofIndexed(IndexedStream s);
        Bytes bytes() const { return indexed ? table.expand() : plain; }
    };

    // The streams a part may have: their plain and indexed tags, element
    // size, and whether there is one per vertex or one per face.
    enum class StreamKind
    {
        Normals,
        Colours,
        Uvs,
        FaceNormals,
    };

    // TGPT: a mesh. Positions float3, faces, strip groups, the optional
    // streams and tag buffers; the TGPH words kept; chunks this reader does
    // not type kept as they are, in the file's order.
    struct Part
    {
        uint32_t id = 0;
        std::vector<Vec3> positions;
        std::vector<Face> faces;
        std::vector<StripGroup> groups;
        std::array<std::optional<Stream>, 4> streams; // by StreamKind
        std::optional<TagBuffer> edgeTags;
        std::optional<TagBuffer> faceTags;
        std::optional<TagBuffer> cornerTags; // ZGE4's TGFV
        uint16_t flags = 0;                  // bits 0-2 the render class
        uint8_t reserved = 0;
        uint8_t boneCount = 0;
        uint8_t materialCount = 0;
        std::vector<Chunk> other;
        std::vector<Tag> order;

        std::optional<Stream> &stream(StreamKind kind) { return streams[static_cast<size_t>(kind)]; }
        const std::optional<Stream> &stream(StreamKind kind) const { return streams[static_cast<size_t>(kind)]; }

        static Part read(const Chunk &chunk, Engine engine);
        Chunk write(Engine engine) const;
        // ZGE3's winding mark cleared from the strips (DM2's have none).
        void unwind();
    };

    // TGLO: the part drawn out to `distance`.
    struct LevelOfDetail
    {
        double distance = 0.0;
        uint32_t partId = 0;
        uint32_t word04 = 0;

        static LevelOfDetail read(const Bytes &data);
        Bytes write() const;
    };

    // TGHD (0x40 bytes): the object's bounds, id, name and name hash. The
    // name field keeps at most kNameChars characters and its NUL; the hash is
    // of the whole name, as the games' exporter wrote it (on every stock
    // object of the three discs; for a name cut at 31 characters, such as
    // AI's PropDef_vert-truss_short_carplant, the uncut name's), and it is
    // what the game finds an object by (findWorldObjectByHash, and
    // cloneDynamicsDefinition a prop's definition).
    struct GeoHeader
    {
        static constexpr size_t kNameChars = 31;

        Vec3 boundsMin{}, boundsMax{};
        uint32_t id = 0;   // what an instance's geometryId names
        std::string name;  // the name field
        uint32_t nameHash = 0;

        static GeoHeader read(const Bytes &data);
        Bytes write() const;
        // A header made for an object named `name`: with no hash given, the
        // hash of the whole name and the field the name cut to kNameChars;
        // with one (a stock object's word, kept through an export), the name
        // as given.
        static GeoHeader make(const Vec3 &boundsMin, const Vec3 &boundsMax, uint32_t id, const std::string &name,
                              std::optional<uint32_t> nameHash = std::nullopt);
    };

    // "DynamicsObject": the TGTD word of a dynamics prop's object (a trash
    // can, a cone, a chair the rider knocks about). The prop's definition is
    // the GEO's ZDYN chunk whose DDEF holds the object's name hash: DDEF,
    // then the bodies (DBDY) and links (DCON), the same form in all three
    // games.
    extern const uint32_t kDynamicsObjectClass;

    struct ObjectPart
    {
        Part part;
        std::optional<LevelOfDetail> lod;
        std::optional<Chunk> materials; // TMTB
    };

    // TGEO: a geometry object.
    struct GeoObject
    {
        GeoHeader header;
        Bytes frame; // TGFR
        std::vector<ObjectPart> parts;
        std::optional<Bytes> typeData; // TGTD: the object's class hash, then its data
        std::optional<CollisionTree> collision;

        static GeoObject read(const Chunk &chunk, Engine engine);
        Chunk write(Engine engine) const;
        // The part the tree names, else the last (AI's query without a tree).
        const ObjectPart *collisionPart() const;
    };

    // A GEO file: FORM { TGEO ..., and other chunks kept where they were }.
    struct Geometry
    {
        std::vector<GeoObject> objects;
        std::vector<std::pair<size_t, Chunk>> other; // (its place among the FORM's kids, the chunk)

        static Geometry read(const Bytes &data, Engine engine);
        Bytes write(Engine engine) const;
    };
}
