#pragma once

// Z-Axis GEO geometry read on the host (native asset loading). A level's
// .GEO, and the geometry inside a .ZIT model (FMDL > MODL), is a FORM of
// "TGEO" objects (zge/assets/geo.h has the chunk list and the game's
// loaders):
//
//   TGEO   TGHD header (bounds, name), TGTD/TGFR/TGLO/TGCI, then
//     TGPT parts: TGPH header (flags: render class in bits 0-2, the
//          skinned bit; vertex, face, group, bone and material counts),
//          then a chunk per stream: TGVP positions (float3), TGVU UVs
//          (float2), TGVC colours (RGBA8), TGVN normals; TIVU / TIVC /
//          TIVN the same indexed ({TIDA table, TII8 | TII6 one index per
//          vertex}), TGFM strip groups ({u16 material << 8, u16 count,
//          count u16 vertex entries, padded to 4}), faces, bones, ...
//     TMTB the object's MATI materials
//
// readGeoFile() keeps what drawing needs, per part, with every stream
// resolved to one value per vertex as the game resolves it (UVs to the s16
// the VIF unpacks, with the game's rounding).

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zge::native
{
    struct GeoGroup
    {
        uint8_t mode = 0;              // TGFM header byte 0, bits 0-1 (1: a triangle list)
        uint8_t material = 0;          // its submesh (TGFM header byte 1)
        std::vector<uint16_t> entries; // vertex index | 0x8000 strip start | 0x4000 winding
    };

    // An object's collision chunks (zge/collision/collision.h): TGFR's
    // bytes (the collision frame's flags) and TGCI, the face tree of the
    // object's collision part, named by its part id.
    struct GeoCollisionChunks
    {
        bool hasFrame = false;
        uint8_t frame[4] = {};
        bool hasTree = false;
        uint32_t treePartId = 0;   // the TGPT whose TGPH +0x10 is this
        std::vector<uint8_t> tree; // as in RAM (FaceTreeSplit / FaceTreeLeaf, then face indices)
    };

    struct GeoFilePart
    {
        uint32_t chunkOffset = 0; // the TGPT chunk's header in the file
        uint16_t flags = 0;       // TGPH +0 (render class in bits 0-2, 0x800 skinned)
        uint16_t vertexCount = 0, faceCount = 0, groupCount = 0;
        uint8_t boneCount = 0, materialCount = 0;
        std::vector<float> positions;   // x, y, z per vertex
        std::vector<int16_t> uvs;       // s, t per vertex (4096 = 1.0)
        std::vector<float> uvFloats;    // the same as the file's floats (ZGE2's skinned packets send these)
        std::vector<uint8_t> colours;   // RGBA per vertex (as on disc)
        std::vector<uint8_t> normalBytes; // the TGVN / TIVN elements per vertex, as on disc
        uint32_t normalStride = 0;        // bytes per normal
        std::vector<uint8_t> skinWeights; // TGVI: 8 bytes per vertex, {bone (1-based, 0 none), weight} x 4
        std::vector<GeoGroup> groups;
        bool hasUvs = false, hasColours = false, hasNormals = false;
        // Collision: the part's id (TGPH +0x10), its faces as in RAM (TGFP,
        // 8 bytes each: flags, three vertex indices), a normal per face
        // (TGFN, or TIFN through its table), and the buffer streams' bodies
        // after their 8-byte headers (TGFT face tags, TGFE face edges).
        uint32_t partId = 0;
        std::vector<uint8_t> faces;
        std::vector<float> faceNormals; // x, y, z per face
        bool hasFaceNormals = false;
        std::vector<uint8_t> faceTags, faceEdges;
        bool hasFaceTags = false, hasFaceEdges = false;
        // Their chunk headers: the descriptor (bits 0-2 the index width's
        // kind, 4-13 a record's bytes, 14-31 the records) and the index count.
        uint32_t faceTagsDescriptor = 0, faceTagsCount = 0, faceEdgesDescriptor = 0, faceEdgesCount = 0;
        // The collision chunks of the object the part is in.
        std::shared_ptr<const GeoCollisionChunks> objectCollision;
    };

    struct GeoFileObject
    {
        uint32_t chunkOffset = 0;
        std::string name; // TGHD
        std::vector<GeoFilePart> parts;
        std::shared_ptr<const GeoCollisionChunks> collision;
    };

    // Every object of a GEO file, or of a ZIT model's geometry, in order; or
    // the one object of a TGEO body on its own (starting at its TGHD; offsets
    // from its start).
    // `error` (optional) gets the first chunk that did not add up; what was
    // read before it is returned.
    std::vector<GeoFileObject> readGeoFile(const std::vector<uint8_t> &file, std::string *error = nullptr);
}
