// A collision mesh's faces and vertices, as the game reads them one at a
// time (the skater's rail search, findWall, and the debug draw of the
// faces hit): zge/collision/collision.h Face, the part's streams
// (zge/render/part.h).

#include "collision.h"

#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/game_address.h"
#include "zge/native/collision_data.h"
#include "zge/native/host.h"

#include <algorithm>
#include <cstring>
#include "sym/zge/collision/mesh.cpp.h"

namespace zge
{
    namespace
    {
        // PS2X_NATIVE_COLLISION (on by default; outside ps2xFnCheck): the part as the
        // host made it from its GEO file, if it has it.
        ee::Ptr<ZgeStream> streamOf(ee::Ptr<uint8_t> field) { return field.cast<ZgeStream>(); }

        // One element of a stream: its data follows the stream's header.
        template <typename T>
        ee::Ptr<T> streamElement(ee::Ptr<uint8_t> stream, uint32_t index)
        {
            return (stream.cast<ZgeStream>() + 1).cast<T>() + static_cast<ptrdiff_t>(index);
        }

        // The element a stream's indices give it, when it has indices. (ZGE2's streams have none: the element
        // itself.)
        uint32_t streamIndex(ee::Ptr<ZgeStream> stream, uint32_t element)
        {
            if constexpr (game::readable::kIsDm2)
                return element;
            if (!stream->indices)
                return element;
            if (stream->wideIndices & 1u)
                return stream->indices.cast<uint16_t>()[element];
            return stream->indices[element];
        }

        void storeNormal(ee::Ptr<Vec4> normal, const Vec3 &n)
        {
            normal->x = n.x;
            normal->y = n.y;
            normal->z = n.z;
            normal->w = ee::Float(0.0f);
        }

        std::shared_ptr<const native::HostCollisionPart> hostPart(ee::Ptr<ZgePart> part)
        {
            if (!native::hostExtrasOn() || !native::nativeCollisionEnabled())
                return nullptr;
            return native::hostCollisionPart(part);
        }
    }

    uint32_t faceVertices(ee::Ptr<ZgePart> part, uint32_t face, ee::Ptr<uint16_t> *vertices)
    {
        // The face's three vertex indices, and its vertex count (the flags'
        // bits 8-11). The indices are handed back where they are in RAM (the
        // callers read them there).
        const ee::Ptr<Face> record = streamElement<Face>(part->faces, face);
        *vertices = &record->vertex[0];
        if (const auto host = hostPart(part); host && face < host->faces.size())
        {
            const uint16_t flags = host->faces[face].flags;
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(&flags, &record->flags, sizeof(flags));
            return (flags >> 8) & 0xFu;
        }
        return (record->flags >> 8) & 0xFu;
    }
    READABLE_FUNCTION(faceVertices, Verified);

    uint16_t faceVertexIndex(ee::Ptr<ZgePart> part, uint32_t face, ee::Ptr<uint16_t> handed, uint32_t corner)
    {
        const ee::Ptr<uint16_t> ram = handed + static_cast<ptrdiff_t>(corner);
        if (corner < 3u)
            if (const auto host = hostPart(part); host && face < host->faces.size())
            {
                const uint16_t index = host->faces[face].vertex[corner];
                if (native::nativeCollisionVerify())
                    native::noteVerifiedRead(&index, ram.get(), sizeof(index));
                return index;
            }
        return *ram;
    }

    namespace
    {
        // Where edgeVertices leaves an edge's two vertex indices.
        inline ee::Global<uint16_t[2], sym::g_edgeVertices> g_edgeVertices;
    }

    // edgeVertices: the vertex indices of edge `corner` of a face (from
    // that corner to the next, round the face's vertex count), put in a
    // static pair; *vertices points at it.
    void edgeVertices(ee::Ptr<ZgePart> part, int32_t face, int32_t corner, ee::Ptr<uint16_t> *vertices)
    {
        const ee::Ptr<Face> record = streamElement<Face>(part->faces, static_cast<uint32_t>(face));
        *vertices = ee::Ptr<uint16_t>(g_edgeVertices.get()[0]);
        Face f;
        if (const auto host = hostPart(part); host && static_cast<uint32_t>(face) < host->faces.size())
        {
            f = host->faces[face];
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(&f, record.get(), sizeof(f));
        }
        else
            f = *record;
        // (A face of no vertices would trap in the original's division.)
        const int32_t count = std::max((f.flags >> 8) & 0xF, 1);
        (*g_edgeVertices)[0] = f.vertex[corner % count];
        (*g_edgeVertices)[1] = f.vertex[(corner + 1) % count];
    }
    READABLE_FUNCTION(edgeVertices, Verified);

    // elementEdgeVertices: the same for a face-edge element (three a face: the
    // face, then the corner).
    void elementEdgeVertices(ee::Ptr<ZgePart> part, int32_t element, ee::Ptr<uint16_t> *vertices)
    {
        game::tail(edgeVertices, part, element / 3, element % 3, vertices);
    }
    READABLE_FUNCTION(elementEdgeVertices, Verified);

    ee::Ptr<Vec4> faceNormal(ee::Ptr<ZgePart> part, uint32_t face, ee::Ptr<Vec4> normal)
    {
        // A normal per face, through the stream's indices when it has them; w = 0.
        const ee::Ptr<ZgeStream> stream = streamOf(part->faceNormals);
        if (const auto host = hostPart(part); host && host->hasFaceNormals && face < host->faceNormals.size())
        {
            const Vec3 &n = host->faceNormals[face];
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(&n, streamElement<Vec3>(part->faceNormals, streamIndex(stream, face)).get(), sizeof(Vec3));
            storeNormal(normal, n);
            return normal;
        }
        storeNormal(normal, *streamElement<Vec3>(part->faceNormals, streamIndex(stream, face)));
        return normal;
    }
    READABLE_FUNCTION(faceNormal, Verified);

    ee::Ptr<Vec4> vertexPosition(ee::Ptr<ZgePart> part, uint32_t vertex, ee::Ptr<Vec4> position)
    {
        const ee::Ptr<Vec4> stored = streamElement<Vec4>(part->positions, vertex);
        if (const auto host = hostPart(part); host && vertex < host->positions.size())
        {
            if (native::nativeCollisionVerify())
                native::noteVerifiedRead(&host->positions[vertex], stored.get(), sizeof(Vec4));
            std::memcpy(position.get(), &host->positions[vertex], sizeof(Vec4));
            return position;
        }
        std::memcpy(position.get(), stored.get(), sizeof(Vec4));
        return position;
    }
    READABLE_FUNCTION(vertexPosition, Verified);
}
