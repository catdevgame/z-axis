#include "collision_check.h"

#include "collision_data.h"
#include "geo_meshes.h"

#include "ee/ee.h"
#include "zge/collision/collision.h"

#include <cstddef>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>

namespace zge::native
{
    namespace
    {
        // What was compared, frame by frame.
        struct Totals
        {
            uint32_t frames = 0, noOrigin = 0, identical = 0;
            uint32_t partId = 0, positions = 0, faces = 0, normals = 0, tags = 0, edges = 0, tree = 0;
            uint32_t treesFromFile = 0, treesMadeInGame = 0, treesNotYet = 0;
            uint32_t fromDisc = 0, nearestEdges = 0, searchedFaces = 0, taggedFaces = 0, edgeElements = 0;
            uint32_t undetermined = 0, facesPastPositions = 0, partsWithFacesPast = 0;
            uint32_t vertices = 0, faceCount = 0;
        };

        struct State
        {
            std::mutex mutex;
            std::map<uint32_t, const HostCollisionPart *> checked; // part address -> the host part it was checked with
            Totals totals;
            int described = 0;
        };
        State &state()
        {
            static State s;
            return s;
        }

        int level()
        {
            static const int v = []
            {
                const char *e = std::getenv("ZX_NATIVE_COLLISION_CHECK");
                return e && *e ? std::atoi(e) : 0;
            }();
            return v;
        }

        template <typename T>
        const T *guest(uint32_t address)
        {
            return address ? ee::at<T>(address) : nullptr;
        }

        // A stream's data (ZgeStream: 16-byte header) or null.
        const uint8_t *streamData(uint32_t stream) { return stream ? ee::at<uint8_t>(stream + 0x10u) : nullptr; }

        // The one-leaf tree the face tests make for a part without a TGCI
        // (zge/collision/world.cpp queryTestShape).
        bool isWholeLeaf(const uint8_t *tree, uint32_t bytes, uint32_t faces)
        {
            if (bytes != faces * 2u + 8u)
                return false;
            const FaceTreeLeaf *leaf = reinterpret_cast<const FaceTreeLeaf *>(tree);
            if (leaf->axis != kFaceTreeLeaf || leaf->count != faces || leaf->faces != 4u)
                return false;
            const uint16_t *index = reinterpret_cast<const uint16_t *>(tree + 8);
            for (uint32_t i = 0; i < faces; ++i)
                if (index[i] != i)
                    return false;
            return true;
        }
    }

    bool collisionCheckEnabled() { return level() > 0; }

    void checkCollisionFrameOnce(ee::Address frame, ee::Address part) { checkCollisionFrameOnce(frame.address, part.address); }

    void checkCollisionFrameOnce(uint32_t frameAddress, uint32_t partAddress)
    {
        if (!collisionCheckEnabled())
            return;
        const std::shared_ptr<const HostCollisionPart> host = hostCollisionPart(partAddress);
        State &s = state();
        std::lock_guard<std::mutex> lock(s.mutex);
        auto seen = s.checked.find(partAddress);
        if (seen != s.checked.end() && seen->second == host.get())
            return;
        s.checked[partAddress] = host.get();
        Totals &t = s.totals;
        ++t.frames;

        const CollisionFrame *frame = guest<CollisionFrame>(frameAddress);
        const ZgePart *part = guest<ZgePart>(partAddress);
        std::ostringstream why;
        bool same = true;
        auto differs = [&](const char *what)
        {
            if (!same)
                why << ", ";
            why << what;
            same = false;
        };
        if (!host || !frame || !part)
        {
            ++t.noOrigin;
            if (level() >= 2 || s.described < 5)
                std::cout << "[native-collision] frame 0x" << std::hex << frameAddress << " part 0x" << partAddress
                          << std::dec << ": no host data (no file origin)" << std::endl;
            return;
        }
        t.fromDisc += host->fromDisc;

        const uint32_t vertices = part->vertexCount, faces = part->faceCount;
        t.vertices += vertices;
        t.faceCount += faces;
        // The part's id (TGPH +0x10) and counts.
        if (part->id == host->partId && vertices == host->vertexCount && faces == host->faceCount)
            ++t.partId;
        else
            differs("part id or counts");
        // Positions: float4 each, w included.
        if (const uint8_t *p = streamData(part->positions.address); p && host->positions.size() == vertices)
        {
            if (std::memcmp(p, host->positions.data(), vertices * sizeof(Vec4)) == 0)
                ++t.positions;
            else
                differs("positions");
        }
        else
            differs("positions missing");
        // Faces: the same 8 bytes each.
        if (const uint8_t *f = streamData(part->faces.address); f && host->faces.size() == faces)
        {
            if (std::memcmp(f, host->faces.data(), faces * sizeof(Face)) == 0)
                ++t.faces;
            else
                differs("faces");
        }
        else
            differs("faces missing");
        // Face normals: a float3 per face, through the stream's indices
        // when it has them (u16 when its wide bit is set, else u8).
        if (const ZgeStream *n = guest<ZgeStream>(part->faceNormals.address); n && host->hasFaceNormals)
        {
            const uint8_t *table = streamData(part->faceNormals.address);
            const uint8_t *indices = n->indices ? ee::at<uint8_t>(n->indices.address) : nullptr;
            bool ok = host->faceNormals.size() == faces;
            for (uint32_t f = 0; f < faces && ok; ++f)
            {
                uint32_t index = f;
                if (indices)
                    index = (n->wideIndices & 1u) ? reinterpret_cast<const uint16_t *>(indices)[f] : indices[f];
                ok = std::memcmp(table + index * 12u, &host->faceNormals[f], 12) == 0;
            }
            if (ok)
                ++t.normals;
            else
                differs("face normals");
        }
        else if (part->faceNormals.address || host->hasFaceNormals)
            differs("face normals missing");
        else
            ++t.normals;
        // The tag tables, whole: the words the level's setup wrote in RAM
        // against the host model's (collision_data.h LevelSetupStep).
        auto tagTable = [&](uint32_t address, const HostTagTable &table, uint32_t &sameTables, const char *what)
        {
            if (!address && !table.present)
            {
                ++sameTables;
                return;
            }
            const TagTable *ram = guest<TagTable>(address);
            if (!ram || !table.present)
            {
                differs(what);
                why << (ram ? " (not in the file)" : " (not in RAM)");
                return;
            }
            if (ram->descriptor != table.descriptor || ram->count != table.count)
            {
                differs(what);
                why << " (header)";
                return;
            }
            const uint8_t *data = streamData(address);
            size_t wrong = 0;
            size_t first = 0;
            for (size_t i = 0; i < table.body.size(); ++i)
                if (data[i] != table.body[i] && wrong++ == 0)
                    first = i;
            if (!wrong)
            {
                ++sameTables;
                return;
            }
            differs(what);
            char text[160];
            std::snprintf(text, sizeof(text), " (%zu of %zu bytes, first at %zu, EE 0x%08x: RAM 0x%02x, host 0x%02x)",
                          wrong, table.body.size(), first, static_cast<uint32_t>(address + 0x10u + first), data[first],
                          table.body[first]);
            why << text;
        };
        tagTable(part->faceTags.address, host->faceTags, t.tags, "face tags");
        tagTable(part->faceEdges.address, host->faceEdges, t.edges, "face edges");
        t.nearestEdges += host->setup.nearestEdges;
        t.searchedFaces += host->setup.searchedFaces;
        t.taggedFaces += host->setup.taggedFaces;
        t.edgeElements += host->setup.edgeElements;
        t.undetermined += host->setup.undetermined;
        t.facesPastPositions += host->facesPastPositions;
        t.partsWithFacesPast += host->facesPastPositions != 0;
        if (host->facesPastPositions)
        {
            // Where the vertices past the positions are in RAM: what follows
            // the positions stream (the part's other streams' addresses).
            uint32_t highest = 0;
            for (const Face &f : host->faces)
                for (uint16_t v : f.vertex)
                    highest = std::max<uint32_t>(highest, v);
            const uint32_t end = part->positions.address + 0x10u + 16u * host->vertexCount;
            std::fprintf(stderr,
                         "[collision-check] part 0x%08x: %u faces past its %u positions (indices up to %u); positions "
                         "0x%08x..0x%08x; faces 0x%08x, face normals 0x%08x, normals 0x%08x, uvs 0x%08x, colours "
                         "0x%08x, tags 0x%08x, edges 0x%08x\n",
                         partAddress, host->facesPastPositions, host->vertexCount, highest, part->positions.address,
                         end, part->faces.address, part->faceNormals.address, part->normals.address,
                         part->uvs.address, part->colours.address, part->faceTags.address, part->faceEdges.address);
            const uint32_t *past = ee::at<uint32_t>(end);
            std::fprintf(stderr, "[collision-check]   past the positions: %08x %08x %08x %08x  %08x %08x %08x %08x\n",
                         past[0], past[1], past[2], past[3], past[4], past[5], past[6], past[7]);
        }
        // The face tree: the object's TGCI, or the one leaf made in the game.
        const uint8_t *tree = frame->tree ? ee::at<uint8_t>(frame->tree.address) : nullptr;
        if (host->hasTree)
        {
            if (tree && frame->treeBytes == host->tree.size() &&
                std::memcmp(tree, host->tree.data(), host->tree.size()) == 0)
            {
                ++t.tree;
                ++t.treesFromFile;
            }
            else
                differs("face tree");
        }
        else if (!tree)
        {
            ++t.tree;
            ++t.treesNotYet; // made by the face tests on their first query
        }
        else if (isWholeLeaf(tree, frame->treeBytes, faces))
        {
            ++t.tree;
            ++t.treesMadeInGame;
        }
        else
            differs("face tree (no TGCI for this part in its object)");

        if (same)
            ++t.identical;
        if (level() >= 2 || (!same && s.described < 8))
        {
            std::string path;
            uint64_t offset = 0;
            partDiscOrigin(partAddress, path, offset);
            std::cout << "[native-collision] frame 0x" << std::hex << frameAddress << " part 0x" << partAddress
                      << std::dec << " (" << path << " +0x" << std::hex << offset << std::dec << "): " << vertices
                      << " vertices, " << faces << " faces, tree " << (host->hasTree ? "TGCI" : "none in file")
                      << ", nearest edges " << host->setup.nearestEdges << " of " << host->setup.searchedFaces
                      << " faces: " << (same ? std::string("identical") : "differs: " + why.str()) << std::endl;
            if (!same)
                ++s.described;
        }
        // The running totals after every frame (runs end by being stopped).
        std::cout << "[native-collision] " << t.frames << " frames checked, " << t.identical
                  << " identical (no host data " << t.noOrigin << ", read from the disc file " << t.fromDisc
                  << "); part ids " << t.partId << ", positions " << t.positions << ", faces " << t.faces
                  << ", face normals " << t.normals << ", face tags " << t.tags << ", face edges " << t.edges
                  << ", trees " << t.tree << " (TGCI " << t.treesFromFile << ", one leaf made in the game "
                  << t.treesMadeInGame << ", not made yet " << t.treesNotYet << "); load-time words: nearest edges "
                  << t.nearestEdges << " (faces of kind 1 or 2: " << t.searchedFaces << "), kind-0xB faces "
                  << t.taggedFaces << ", edge elements " << t.edgeElements << " (candidates past the positions "
                  << t.undetermined << "); faces past the positions " << t.facesPastPositions << " in "
                  << t.partsWithFacesPast << " parts; " << t.vertices << " vertices, "
                  << t.faceCount << " faces" << std::endl;
    }
}
