// The host collision part made from a GEO file's part, with the running
// game's level setup on it when it registered one (collision_data.h
// LevelSetupStep). No runtime state: unit tests build it directly.

#include "collision_data.h"

#include "geo_file.h"

#include "ee/vu0.h"
#include "zge/collision/closest.h"

#include <cstring>

namespace zge::native
{
    // ---- tag tables -----------------------------------------------------------

    int64_t HostTagTable::recordOffset(uint32_t element) const
    {
        if (!present || width == 0 || element >= count)
            return -1;
        const uint8_t *p = body.data() + element * width;
        uint32_t index = p[0];
        if (width == 2)
        {
            index |= uint32_t(p[1]) << 8;
            if (index == 0xFFFFu)
                return -1;
        }
        else if (index == 0xFFu)
            return -1;
        const uint64_t at = uint64_t(recordsAt) + uint64_t(index) * recordBytes;
        return at + recordBytes <= body.size() ? static_cast<int64_t>(at) : -1;
    }

    namespace
    {
        HostTagTable tagTable(bool present, uint32_t descriptor, uint32_t count, const std::vector<uint8_t> &body,
                              const uint32_t indexWidth[16])
        {
            HostTagTable t;
            if (!present)
                return t;
            t.present = true;
            // The table's header keeps the descriptor without bit 3 (newBuffer).
            t.descriptor = descriptor & ~0x8u;
            t.count = count;
            // tagElementRecord knows index kinds 0 (u8) and 1 (u16) only.
            const uint32_t kind = descriptor & 0xFu;
            t.width = kind <= 1 ? indexWidth[kind] : 0;
            t.recordsAt = (t.width * count + 15u) & ~15u;
            t.recordBytes = (descriptor >> 4) & 0x3FFu;
            t.records = descriptor >> 14;
            t.body = body;
            return t;
        }

    }


    std::shared_ptr<HostCollisionPart> makeHostCollisionPart(const GeoFilePart &file, const uint32_t indexWidth[16],
                                                             LevelSetupStep setup)
    {
        auto part = std::make_shared<HostCollisionPart>();
        part->partId = file.partId;
        part->vertexCount = file.vertexCount;
        part->faceCount = file.faceCount;
        // float3 on disc, float4 (w = 1) in RAM (the TGVP loader).
        part->positions.resize(file.vertexCount);
        for (uint32_t v = 0; v < file.vertexCount && v * 3u + 2u < file.positions.size(); ++v)
            part->positions[v] = {file.positions[v * 3u], file.positions[v * 3u + 1u], file.positions[v * 3u + 2u],
                                  1.0f};
        part->faces.resize(file.faces.size() / sizeof(Face));
        if (!part->faces.empty())
            std::memcpy(part->faces.data(), file.faces.data(), part->faces.size() * sizeof(Face));
        part->hasFaceNormals = file.hasFaceNormals;
        part->faceNormals.resize(file.faceNormals.size() / 3u);
        for (size_t f = 0; f < part->faceNormals.size(); ++f)
            part->faceNormals[f] = {file.faceNormals[f * 3u], file.faceNormals[f * 3u + 1u], file.faceNormals[f * 3u + 2u]};
        if (const GeoCollisionChunks *chunks = file.objectCollision.get();
            chunks && chunks->hasTree && chunks->treePartId == file.partId)
        {
            part->hasTree = true;
            part->tree = chunks->tree;
        }
        part->faceTags = tagTable(file.hasFaceTags, file.faceTagsDescriptor, file.faceTagsCount, file.faceTags, indexWidth);
        part->faceEdges =
            tagTable(file.hasFaceEdges, file.faceEdgesDescriptor, file.faceEdgesCount, file.faceEdges, indexWidth);
        // Faces whose vertex indices or count the arrays do not hold would
        // make the setup read out of bounds: such a part is not modelled.
        if (part->faces.size() != part->faceCount || part->positions.size() != part->vertexCount ||
            (part->hasFaceNormals && part->faceNormals.size() != part->faceCount))
            return nullptr;
        for (const Face &f : part->faces)
            part->facesPastPositions += !part->vertexInFile(f.vertex[0]) || !part->vertexInFile(f.vertex[1]) ||
                                        !part->vertexInFile(f.vertex[2]);
        if (setup)
            setup(*part);
        return part;
    }

}
