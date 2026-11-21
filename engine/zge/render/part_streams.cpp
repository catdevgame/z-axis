// A part's vertex streams changed one vertex at a time (copyMeshVertices and the
// effects that animate a mesh): each setter marks the stream in the part's
// flags, and the packet builder (buildPartPacket with first = 0) rebuilds the
// part's packet from the changed streams (READABLE.md, next slice 1).

#include "part.h"

#include "ee/readable.h"
#include "zge/math/vec.h"
#include "zge/render/fixed_point.h"
#include "sym/zge/render/part_streams.cpp.h"

namespace zge
{
    namespace
    {
        // ZgePart::flags: the streams to rebuild the packet from.
        constexpr uint16_t kRebuildPositions = 0x1u;
        constexpr uint16_t kRebuildColours = 0x2u;
        constexpr uint16_t kRebuildUvs = 0x4u;
    }

    // setPartPosition: vertex `index`'s position (a quadword: x, y, z, w).
    void setPartPosition(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<Vec4> position)
    {
        aligned(streamRecords<Vec4>(part->positions) + index) = quadwordOf(position);
        part->flags = static_cast<uint16_t>(part->flags | kRebuildPositions);
    }
    READABLE_FUNCTION(setPartPosition, Verified);

    // setPartColour: vertex `index`'s colour, RGBA8 as it is.
    void setPartColour(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<const VertexColour> colour)
    {
        streamRecords<VertexColour>(part->colours)[index] = *colour;
        part->flags = static_cast<uint16_t>(part->flags | kRebuildColours);
    }
    READABLE_FUNCTION(setPartColour, Verified);

    // partNormal (ZGE2 only): vertex `index`'s normal (x, y, z; w 1) into
    // out; out. ZGE3's vertexNormal reads its packed normal streams instead.
    inline ee::Ptr<Vec4> partNormal(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<Vec4> out)
    {
        const Vec3 &normal = streamRecords<Vec3>(part->normals)[index];
        out->x = normal.x;
        out->y = normal.y;
        out->w = 1.0f;
        out->z = normal.z;
        return out;
    }
    READABLE_FUNCTION_ONLY(dm2, partNormal);

    // partColour (ZGE2 only): vertex `index`'s colour, RGBA8 as it is, into
    // out; out.
    inline ee::Ptr<VertexColour> partColour(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<VertexColour> out)
    {
        *out = streamRecords<VertexColour>(part->colours)[index];
        return out;
    }
    READABLE_FUNCTION_ONLY(dm2, partColour);

    // setPartColourFloats: vertex `index`'s colour from four floats 0 .. 1 (each
    // times 255, truncated).
    void setPartColourFloats(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<ee::Float> rgba)
    {
        const ee::Ptr<uint8_t> out = streamRecords<uint8_t>(part->colours) + (index << 2);
        for (int i = 0; i < 4; ++i)
            out[i] = static_cast<uint8_t>(ee::toInt(rgba[i] * ee::Float(255.0f)));
        part->flags = static_cast<uint16_t>(part->flags | kRebuildColours);
    }
    READABLE_FUNCTION(setPartColourFloats, Verified);

    // setPartUv: vertex `index`'s texture coordinates from two floats
    // (s16, 4096 = 1.0; ZGE2's UV stream keeps the two floats).
    void setPartUv(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<ee::Float> uv)
    {
        if constexpr (game::readable::kIsDm2)
        {
            const ee::Ptr<ee::Float> floats = streamRecords<ee::Float>(part->uvs) + (index << 1);
            floats[0] = uv[0];
            floats[1] = uv[1];
            part->flags = static_cast<uint16_t>(part->flags | kRebuildUvs);
            return;
        }
        const ee::Ptr<uint16_t> out = streamRecords<uint16_t>(part->uvs) + (index << 1);
        out[0] = static_cast<uint16_t>(fixedUv(uv[0]));
        out[1] = static_cast<uint16_t>(fixedUv(uv[1]));
        part->flags = static_cast<uint16_t>(part->flags | kRebuildUvs);
    }
    READABLE_FUNCTION(setPartUv, Verified);
}
