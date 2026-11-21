#pragma once
// zglTerrainRender's functions others call (zge/render/terrain.cpp): a part's
// strip groups, faces and mesh streams merged into another (zge/scene/pose.cpp),
// whether a node is in the view, and a node's draw check.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/render/part.h"

#include <cstdint>

namespace zge
{
    struct CollisionFrame; // zge/collision/collision.h
    struct Face; // zge/collision/collision.h
    struct Node; // zge/scene/node.h
    struct Sector; // zge/scene/query.h

    // The world's sector walk: visit(sector, data) for each sector of the grid until one returns 0 (then 0); else 1.
    using SectorVisit = ee::FunctionPtr<int32_t(ee::Ptr<Sector> sector, uint32_t data)>;
    int32_t forEachWorldSector(SectorVisit visit, uint32_t data);
    // A model's meshes: visit(mesh, data) for the mesh of each level of detail of each of the node's model's parts,
    // last first, until one returns 0 (then 0); else 1.
    using MeshVisit = ee::FunctionPtr<int32_t(ee::Ptr<ZgePart> mesh, uint32_t data)>;
    int32_t forEachObjectMesh(ee::Ptr<Node> node, MeshVisit visit, uint32_t data);
    uint32_t sectorInView(ee::Ptr<Sector> sector);
    // The first of the frame's levels of detail whose distance is past the position's depth (how far past it in
    // *distancePast), -1 for none.
    int32_t frameLevelForDistance(ee::Ptr<CollisionFrame> frame, ee::Ptr<const Vec3> position, ee::Ptr<ee::Float> distancePast);

    int32_t copyMaterialsInto(ee::Ptr<MaterialTable> table, ee::Ptr<MaterialTable> table2, uint32_t submeshOffset);
    uint32_t partStripGroupCount(ee::Ptr<ZgePart> part);
    uint32_t faceEntry(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<Face> out);
    uint32_t setFaceEntry(ee::Ptr<ZgePart> part, uint32_t index, ee::Ptr<const Face> from);
    void copyMeshVertices(ee::Ptr<ZgePart> from, ee::Ptr<uint32_t> fromBoneIds, uint32_t fromBoneCount, ee::Ptr<ZgePart> to, ee::Ptr<uint32_t> toBoneIds, uint32_t toBoneCount, uint32_t count, uint32_t firstIndex);
    uint32_t partStripGroupBytes(ee::Ptr<ZgePart> part);
    ee::Ptr<StripGroup> partStripGroups(ee::Ptr<ZgePart> part);
    ee::Ptr<StripGroup> nextStripGroup(ee::Ptr<StripGroup> group);
    uint32_t removePartMaterialTable(ee::Ptr<ZgePart> part, ee::Ptr<MaterialTable> table);
    int32_t allocateMeshStreams(ee::Ptr<ZgePart> zgePart, uint32_t vertexCount, uint32_t faceCount, uint32_t groupCount, uint32_t bytes);
    uint32_t nodeVisibleInView(ee::Ptr<Node> node);
    uint32_t setNodeDrawCheck(ee::Ptr<Node> node, uint32_t check, ee::Address data);
}
