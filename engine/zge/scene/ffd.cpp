// Free-form deformation (FFD) lattices (zge/scene/ffd.h): a grid of points
// that bends a part's vertices. BMX XXX keeps its riders' body shapes this
// way (CREATE/LATTICE.ZAL: fatLattice, normalLattice, skinnyLattice,
// smallBreast, largeBreast, smallCrotch, largeCrotch, each a FORM with one
// ZFFD chunk). Aggressive Inline carries the code and only ever registers
// the ZFFD chunk handler.
//
// A part is bound to a lattice once (bindPartToFfd): each vertex gets the
// eight points of the lattice cell it lies in, and a weight for each.
// Deforming it (deformPartByFfd) puts each vertex at the weighted sum of
// those points as the lattice stands now, so moving the lattice's points (or
// blending two lattices, blendFfdLattice) moves the part with them.
//
// A vertex's weights: each corner of its cell makes a tetrahedron with its
// three neighbours along the cell's edges. The vertex's barycentric
// coordinates in each tetrahedron, scaled by how close the vertex is to that
// corner (one over the distance), are added to the four points' weights,
// and the eight weights are then divided by the sum of the scales.

#include "ffd.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/collision/collision.h"
#include "zge/collision/dynamics.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/render/part.h"
#include "zge/text/cstring.h"
#include "zge/assets/asset_library.h"
#include "zge/debug/debug.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/ffd.cpp.h"

namespace zge
{
    namespace
    {
        // What readFfdLattice calls with each lattice it reads, and with
        // what (setFfdCallback).
        using FfdReadCallback = ee::FunctionPtr<void(ee::Ptr<FfdLattice> lattice, uint32_t context)>;

        inline ee::Global<int32_t, sym::g_ffdChunkHandlerReady> g_ffdChunkHandlerReady;
        inline ee::Global<FfdReadCallback, sym::g_ffdCallback> g_ffdCallback;
        inline ee::Global<uint32_t, sym::g_ffdCallbackContext> g_ffdCallbackContext;
        // The lattice readFfdLattice made last (findFfdCell's searches step
        // by its size).
        inline ee::Global<ee::Ptr<FfdLattice>, sym::g_loadingFfdLattice> g_loadingFfdLattice;

        constexpr uint32_t kZffdTag = 0x4446465Au;  // "ZFFD"
        constexpr uint32_t kFormTag = 0x4D524F46u;  // "FORM": a lattice file's form

    }

    // ---- reading lattices -------------------------------------------------------

    // setFfdCallback: the function readFfdLattice calls with each lattice it
    // reads, and the context it hands it; the old function returned.
    FfdReadCallback setFfdCallback(FfdReadCallback callback, uint32_t context)
    {
        const FfdReadCallback old = *g_ffdCallback;
        *g_ffdCallback = callback;
        *g_ffdCallbackContext = context;
        return old;
    }
    READABLE_FUNCTION(setFfdCallback, Draft);

    // readFfdLattice: a ZFFD chunk's lattice: its three counts, then each
    // point's x, y and z. The callback (setFfdCallback) gets it, if one is set.
    ee::Ptr<FfdLattice> readFfdLattice(ChunkReader *reader)
    {
        const ee::Ptr<FfdLattice> lattice(heapAllocate(sizeof(FfdLattice)));
        memoryFill(lattice, 0, sizeof(FfdLattice));
        *g_loadingFfdLattice = lattice;
        readChunkU32(reader, &lattice->countX);
        readChunkU32(reader, &lattice->countY);
        readChunkU32(reader, &lattice->countZ);
        const uint32_t count = lattice->countX * lattice->countY * lattice->countZ;
        const uint32_t bytes = count * static_cast<uint32_t>(sizeof(Vec4));
        lattice->points = ee::Ptr<Vec4>(heapAllocate(bytes));
        memoryFill(lattice->points, 0, bytes);
        for (uint32_t i = 0; i < count; ++i)
        {
            readChunkWord(reader, &lattice->points[i].x);
            readChunkWord(reader, &lattice->points[i].y);
            readChunkWord(reader, &lattice->points[i].z);
        }
        const FfdReadCallback callback = *g_ffdCallback;
        if (callback)
            callback(*g_loadingFfdLattice, *g_ffdCallbackContext);
        return *g_loadingFfdLattice;
    }
    READABLE_FUNCTION_WAITS(readFfdLattice, Draft);

    // readZffdChunk: the ZFFD chunk handler: the lattice into the parse's
    // context.
    uint32_t readZffdChunk(ChunkReader *reader, ee::Ptr<ee::Ptr<FfdLattice>> context)
    {
        *context = readFfdLattice(reader);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readZffdChunk, Draft);

    // registerZffdChunkHandler: the engine's start-up registers the ZFFD
    // chunk handler.
    uint32_t registerZffdChunkHandler()
    {
        *g_ffdChunkHandlerReady = 1;
        addChunkHandler(kZffdTag, sym::readZffdChunk);
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerZffdChunkHandler, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void heapFree(ee::Ptr<FfdLattice> block) { ee::tailCall<void>(sym::heapFree, block); }
        }
    }

    // freeFfdLattice: a lattice and its points freed (none for 0).
    void freeFfdLattice(ee::Ptr<FfdLattice> lattice)
    {
        if (!lattice)
            return;
        if (lattice->points)
            heapFree(lattice->points);
        tail::heapFree(lattice);
    }
    READABLE_FUNCTION_WAITS(freeFfdLattice, Draft);

    // loadFfdFile: the lattice of a lattice file (a FORM holding a ZFFD
    // chunk): the file read whole, copied, and parsed as a memory file.
    ee::Ptr<FfdLattice> loadFfdFile(uint32_t path)
    {
        ee::Local<uint32_t> bytes;
        // Kept as the original has it: nothing sets this before the parse,
        // so with no file, or no ZFFD chunk in it, the answer is whatever
        // the stack held there.
        ee::Local<ee::Ptr<FfdLattice>> lattice;
        const uint32_t data = loadFile(path, bytes);
        if (data != 0)
        {
            const uint32_t copy = heapAllocate(*bytes);
            memoryCopy(copy, data, *bytes);
            freeLoadedFile(data);
            const ee::Ptr<ZFile> file = openMemoryFile(copy, *bytes);
            const ee::Ptr<ChunkReader> chunks(openChunkFile(file, kFormTag, 0));
            parseChunks(chunks, lattice);
            closeChunkFile(chunks);
            closeFile(file, 0);
            heapFree(copy);
        }
        return *lattice;
    }
    READABLE_FUNCTION_WAITS(loadFfdFile, Draft);

    // loadFfdFromArchive: the lattice of an asset library's entry (one of
    // LATTICE.ZAL's), read into memory and parsed as a memory file; 0 when
    // the library has no such entry.
    ee::Ptr<FfdLattice> loadFfdFromArchive(uint32_t library, uint32_t id)
    {
        ee::Local<ee::Ptr<FfdLattice>> lattice;
        *lattice = ee::Ptr<FfdLattice>();
        if (findAsset(library, id) == 0)
            return *lattice;
        const uint32_t bytes = currentAssetLength(library);
        const uint32_t memory = allocateMemory(bytes);
        if (readAsset(library, memory, 0) != 0)
        {
            const ee::Ptr<ZFile> file = openMemoryFile(memory, bytes);
            const ee::Ptr<ChunkReader> chunks(openChunkFile(file, kFormTag, 0));
            if (chunks)
            {
                parseChunks(chunks, lattice);
                closeChunkFile(chunks);
            }
            closeFile(file, 0);
        }
        freeMemory(memory);
        return *lattice;
    }
    READABLE_FUNCTION_WAITS(loadFfdFromArchive, Draft);

    // ---- making and blending lattices -------------------------------------------

    // copyFfdDimensions: one lattice's three counts given to another.
    void copyFfdDimensions(ee::Ptr<FfdLattice> from, ee::Ptr<FfdLattice> to)
    {
        to->countX = from->countX;
        to->countY = from->countY;
        to->countZ = from->countZ;
    }
    READABLE_FUNCTION(copyFfdDimensions, Draft);

    // newFfdLattice: a lattice of that size, its points all 0.
    ee::Ptr<FfdLattice> newFfdLattice(uint32_t countX, uint32_t countY, uint32_t countZ)
    {
        const ee::Ptr<FfdLattice> lattice(heapAllocate(sizeof(FfdLattice)));
        memoryFill(lattice, 0, sizeof(FfdLattice));
        const uint32_t bytes = countX * countZ * (countY * static_cast<uint32_t>(sizeof(Vec4)));
        lattice->countX = countX;
        lattice->countY = countY;
        lattice->countZ = countZ;
        lattice->points = ee::Ptr<Vec4>(heapAllocate(bytes));
        memoryFill(lattice->points, 0, bytes);
        return lattice;
    }
    READABLE_FUNCTION_WAITS(newFfdLattice, Draft);

    // blendFfdLattice: out's points from a's towards b's by t (0: a's, 1:
    // b's), as many as a has. Returns out.
    ee::Ptr<FfdLattice> blendFfdLattice(ee::Ptr<FfdLattice> out, ee::Ptr<FfdLattice> a, ee::Ptr<FfdLattice> b, ee::Float t)
    {
        for (uint32_t i = 0; i < a->countX * a->countY * a->countZ; ++i)
            vec3Lerp(&out->points[i].xyz(), &a->points[i].xyz(), &b->points[i].xyz(), t);
        return out;
    }
    READABLE_FUNCTION(blendFfdLattice, Draft);

    // ---- binding a part's vertices ----------------------------------------------

    namespace
    {
        // Each corner of a cell (x + 2 y + 4 z) and the three corners next to
        // it along the cell's edges: the corner's tetrahedron.
        constexpr uint32_t kTetrahedronPoints = 4;
        using CornerTetrahedra = uint32_t[kFfdCellCorners][kTetrahedronPoints];
        inline ee::Global<CornerTetrahedra, sym::g_ffdCornerTetrahedra> g_ffdCornerTetrahedra;
        // The same tetrahedra as point offsets from the cell's first point in
        // the lattice setFfdTetrahedronOffsets was given (nothing in the FFD
        // code reads them).
        inline ee::Global<CornerTetrahedra, sym::g_ffdTetrahedronOffsets> g_ffdTetrahedronOffsets;

        // The tetrahedra's inverse matrices, the last four made, round a
        // ring of slots; g_ffdInverseSlot is the slot filled last.
        constexpr uint32_t kFfdInverseSlots = 4;
        inline ee::Global<SolverNodeMatrix[kFfdInverseSlots], sym::g_ffdInverses> g_ffdInverses;
        inline ee::Global<uint32_t, sym::g_ffdInverseSlot> g_ffdInverseSlot;
        // Per slot: -1 once cleared, the slot's own number once filled
        // (nothing in the FFD code reads them).
        inline ee::Global<int32_t[kFfdInverseSlots], sym::g_ffdInverseSlotUsed> g_ffdInverseSlotUsed;
        // The first point of the cell findFfdCell found last (nothing in the
        // FFD code reads it).
        inline ee::Global<uint32_t, sym::g_ffdLastCell> g_ffdLastCell;

        constexpr int32_t kSlotCleared = -1;
        // One over a vertex's distance from a corner, for a vertex closer
        // than kOnCorner: 999.99994 (the float below 1000).
        constexpr ee::Float kOnCorner = 0.001f;
        constexpr ee::Float kOnCornerWeight = 999.99994f;

        // findFfdCell's y and z searches: by rows, by layers.
        enum class FfdSearch
        {
            Rows,
            Layers,
        };

        // The points one step of a search moves: a row's or a layer's, of
        // the lattice read last (g_loadingFfdLattice).
        uint32_t searchStep(FfdSearch search)
        {
            const ee::Ptr<FfdLattice> stride = *g_loadingFfdLattice;
            return search == FfdSearch::Rows ? stride->countX : stride->countX * stride->countY;
        }

        // The point a search tests for its row (layer) `line`: ZGE3's is the
        // one along x from the line's start (point 1 for the first, with no
        // step read), ZGE4's the next line's first.
        uint32_t searchedPoint(uint32_t line, FfdSearch search)
        {
            if constexpr (game::readable::kIsBmx)
                return searchStep(search) * (line + 1);
            return line == 0 ? 1 : searchStep(search) * line + 1;
        }

        void clearInverseSlots()
        {
            for (uint32_t slot = 0; slot < kFfdInverseSlots; ++slot)
                (*g_ffdInverseSlotUsed)[slot] = kSlotCleared;
        }

    }

    // findFfdCell: the lattice cell a vertex lies in, searched along x, then
    // y, then z: the cell's eight points (by corner) become the vertex's,
    // their weights 0. The inverse slots are cleared.
    //
    // Kept as the original has it: x's search tests each next point along
    // x, but ZGE3's y and z searches test the point one along x from the
    // row's (layer's) start, which on a regular grid is the row's own line,
    // so they find the row and layer past the vertex's (ZGE4's test the next
    // row's or layer's first point: searchedPoint); both step by the size of
    // the lattice read last (g_loadingFfdLattice), not this one's; and an
    // axis one point long leaves the step of the axis before it, which is
    // added again.
    void findFfdCell(uint32_t vertex, ee::Ptr<Vec4> position, ee::Ptr<FfdVertexWeights> weights, ee::Ptr<FfdLattice> lattice)
    {
        const ee::Ptr<Vec4> points = lattice->points;
        uint32_t step = 0;
        if (lattice->countX - 1 != 0 && !(position->x < points[1].x))
        {
            for (uint32_t x = 1; x < lattice->countX - 1; ++x)
            {
                step = x;
                if (position->x < points[x + 1].x)
                    break;
            }
        }
        uint32_t first = step;
        if (lattice->countY - 1 != 0)
        {
            step = 0;
            if (!(position->y < points[searchedPoint(0, FfdSearch::Rows)].y))
            {
                for (uint32_t y = 1; y < lattice->countY - 1; ++y)
                {
                    step = searchStep(FfdSearch::Rows) * y;
                    if (position->y < points[searchedPoint(y, FfdSearch::Rows)].y)
                        break;
                }
            }
        }
        first += step;
        if (lattice->countZ - 1 != 0)
        {
            step = 0;
            if (!(position->z < points[searchedPoint(0, FfdSearch::Layers)].z))
            {
                for (uint32_t z = 1; z < lattice->countZ - 1; ++z)
                {
                    step = searchStep(FfdSearch::Layers) * z;
                    if (position->z < points[searchedPoint(z, FfdSearch::Layers)].z)
                        break;
                }
            }
        }
        first += step;

        FfdVertexCell &cell = weights->vertices[vertex];
        for (uint32_t x = 0; x < 2; ++x)
        {
            for (uint32_t y = 0; y < 2; ++y)
            {
                for (uint32_t z = 0; z < 2; ++z)
                {
                    const uint32_t corner = x + y * 2 + z * 4;
                    cell.point[corner] = y * lattice->countX + x + z * lattice->countX * lattice->countY + first;
                    cell.weight[corner] = 0.0f;
                }
            }
        }
        clearInverseSlots();
        *g_ffdLastCell = first;
    }
    READABLE_FUNCTION(findFfdCell, Draft);

    // invertFfdTetrahedron: the inverse of the matrix whose columns are a
    // tetrahedron's three edges from its first point, into an inverse slot.
    // Returns the slot's matrix.
    ee::Ptr<SolverNodeMatrix> invertFfdTetrahedron(uint32_t slot, ee::Ptr<Vec4> corners)
    {
        ee::Local<SolverNodeMatrix> edges;
        const ee::Ptr<SolverNodeMatrix> inverse = &(*g_ffdInverses)[slot];
        const Vec4 &origin = corners[0];
        edges->rows = 3;
        edges->columns = 3;
        for (uint32_t edge = 0; edge < 3; ++edge)
        {
            const Vec4 &to = corners[edge + 1];
            edges->data[0][edge] = to.x - origin.x;
            edges->data[1][edge] = to.y - origin.y;
            edges->data[2][edge] = to.z - origin.z;
        }
        invertSolverMatrix3or6(inverse, edges);
        return inverse;
    }
    READABLE_FUNCTION(invertFfdTetrahedron, Draft);

    // ffdTetrahedronWeights: a vertex's barycentric coordinates in one
    // corner's tetrahedron (the corner's first): weights[0] the corner's,
    // 1 to 3 its neighbours'. Returns one over the vertex's distance from
    // the corner (kOnCornerWeight when it is closer than kOnCorner).
    ee::Float ffdTetrahedronWeights(uint32_t vertex, uint32_t corner, ee::Ptr<Vec4> position, ee::Ptr<FfdVertexWeights> weights,
                                     ee::Ptr<FfdLattice> lattice, ee::Ptr<ee::Float> tetrahedronWeights)
    {
        ee::Local<Vec4[kTetrahedronPoints]> corners;
        ee::Local<SolverNodeMatrix> fromCorner;
        ee::Local<SolverNodeMatrix> solved;
        for (uint32_t i = 0; i < kTetrahedronPoints; ++i)
            corners[i] = lattice->points[weights->vertices[vertex].point[(*g_ffdCornerTetrahedra)[corner][i]]];
        *g_ffdInverseSlot = (*g_ffdInverseSlot + 1) & (kFfdInverseSlots - 1);
        const ee::Ptr<SolverNodeMatrix> inverse = invertFfdTetrahedron(*g_ffdInverseSlot, corners);
        const uint32_t slot = *g_ffdInverseSlot;
        (*g_ffdInverseSlotUsed)[slot] = static_cast<int32_t>(slot);
        // The vertex from the corner, as a column, through the inverse.
        fromCorner->rows = 3;
        fromCorner->columns = 1;
        fromCorner->data[0][0] = position->x - corners[0].x;
        fromCorner->data[1][0] = position->y - corners[0].y;
        fromCorner->data[2][0] = position->z - corners[0].z;
        multiplySolverMatrices(solved, inverse, fromCorner);
        const ee::Float towardFirst = solved->data[0][0];
        const ee::Float towardSecond = solved->data[1][0];
        const ee::Float towardThird = solved->data[2][0];
        tetrahedronWeights[1] = towardFirst;
        tetrahedronWeights[3] = towardThird;
        tetrahedronWeights[2] = towardSecond;
        tetrahedronWeights[0] = ee::Float(1.0f) - towardFirst - towardSecond - towardThird;
        corners[0] = corners[0] - *position;
        const ee::Float distance = vec3Length(&corners[0].xyz());
        if (distance < kOnCorner)
            return kOnCornerWeight;
        return ee::Float(1.0f) / distance;
    }
    READABLE_FUNCTION(ffdTetrahedronWeights, Draft);

    // computeFfdWeights: a vertex's cell and its eight weights (see the top).
    void computeFfdWeights(uint32_t vertex, ee::Ptr<Vec4> position, ee::Ptr<FfdVertexWeights> weights, ee::Ptr<FfdLattice> lattice)
    {
        ee::Local<ee::Float[kTetrahedronPoints]> tetrahedronWeights;
        findFfdCell(vertex, position, weights, lattice);
        ee::Float closeness = 0.0f;
        for (uint32_t corner = 0; corner < kFfdCellCorners; ++corner)
        {
            const ee::Float cornerCloseness = ffdTetrahedronWeights(vertex, corner, position, weights, lattice, tetrahedronWeights);
            closeness = closeness + cornerCloseness;
            for (uint32_t i = 0; i < kTetrahedronPoints; ++i)
            {
                ee::Float &weight = weights->vertices[vertex].weight[(*g_ffdCornerTetrahedra)[corner][i]];
                weight = weight + cornerCloseness * tetrahedronWeights[i];
            }
        }
        for (uint32_t corner = 0; corner < kFfdCellCorners; ++corner)
        {
            ee::Float &weight = weights->vertices[vertex].weight[corner];
            weight = weight / closeness;
        }
    }
    READABLE_FUNCTION(computeFfdWeights, Draft);

    // newFfdVertexWeights: a binding's record for that many vertices; 0 when
    // either block cannot be had.
    ee::Ptr<FfdVertexWeights> newFfdVertexWeights(uint32_t vertexCount)
    {
        const ee::Ptr<FfdVertexWeights> weights(heapAllocate(sizeof(FfdVertexWeights)));
        if (!weights)
            return weights;
        weights->vertexCount = vertexCount;
        weights->vertices = ee::Ptr<FfdVertexCell>(heapAllocate(vertexCount * static_cast<uint32_t>(sizeof(FfdVertexCell))));
        if (!weights->vertices)
        {
            heapFree(weights);
            return ee::Ptr<FfdVertexWeights>();
        }
        return weights;
    }
    READABLE_FUNCTION_WAITS(newFfdVertexWeights, Draft);

    // setFfdTetrahedronOffsets (was resetFfdBindings): the inverse slots
    // cleared, and the corner tetrahedra as point offsets in this lattice.
    void setFfdTetrahedronOffsets(ee::Ptr<FfdVertexWeights> /*weights: not read*/, ee::Ptr<FfdLattice> lattice)
    {
        clearInverseSlots();
        for (uint32_t corner = 0; corner < kFfdCellCorners; ++corner)
        {
            for (uint32_t i = 0; i < kTetrahedronPoints; ++i)
            {
                const uint32_t point = (*g_ffdCornerTetrahedra)[corner][i];
                const uint32_t alongZ = (point & 4u) != 0 ? lattice->countX : 0u;
                const uint32_t alongY = (point & 2u) != 0 ? lattice->countX : 0u;
                (*g_ffdTetrahedronOffsets)[corner][i] = (point & 1u) + alongY + alongZ * lattice->countY;
            }
        }
    }
    READABLE_FUNCTION(setFfdTetrahedronOffsets, Draft);

    // freeFfdVertexWeights: gives back what newFfdVertexWeights took, the
    // vertices' weights and then the record; no record is nothing to do.
    void freeFfdVertexWeights(ee::Ptr<FfdVertexWeights> record)
    {
        if (!record)
            return;
        if (record->vertices)
            heapFree(record->vertices);
        game::tail(heapFree, record);
    }
    READABLE_FUNCTION_WAITS(freeFfdVertexWeights, Draft);

    void ffdNoop()
    {
    }
    READABLE_FUNCTION(ffdNoop, Draft);

    // bindPartToFfd: a part's vertices bound to a lattice where they stand
    // now; 0 when the record cannot be had.
    ee::Ptr<FfdVertexWeights> bindPartToFfd(ee::Ptr<ZgePart> part, ee::Ptr<FfdLattice> lattice)
    {
        ee::Local<Vec4> position;
        const uint32_t vertexCount = part->vertexCount;
        const ee::Ptr<FfdVertexWeights> weights = newFfdVertexWeights(vertexCount);
        if (!weights)
            return weights;
        setFfdTetrahedronOffsets(weights, lattice);
        for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
        {
            vertexPosition(part, vertex, position);
            computeFfdWeights(vertex, position, weights, lattice);
        }
        ffdNoop();
        return weights;
    }
    READABLE_FUNCTION_WAITS(bindPartToFfd, Draft);

    // ---- deforming --------------------------------------------------------------

    // deformVertexByFfd: where a bound vertex stands in the lattice now: its
    // cell's points weighted and summed. Returns out.
    ee::Ptr<Vec4> deformVertexByFfd(ee::Ptr<Vec4> out, uint32_t vertex, ee::Ptr<FfdVertexWeights> weights, ee::Ptr<FfdLattice> lattice)
    {
        ee::Local<Vec4> weighted;
        aligned(out) = kZero;
        for (uint32_t corner = 0; corner < kFfdCellCorners; ++corner)
        {
            const FfdVertexCell &cell = weights->vertices[vertex];
            *weighted = lattice->points[cell.point[corner]] * cell.weight[corner];
            aligned(out) = *out + *weighted;
        }
        return out;
    }
    READABLE_FUNCTION(deformVertexByFfd, Draft);

    // deformPartByFfd: each of a bound part's vertices moved to where it
    // stands in the lattice now.
    void deformPartByFfd(ee::Ptr<ZgePart> part, ee::Ptr<FfdVertexWeights> weights, ee::Ptr<FfdLattice> lattice)
    {
        ee::Local<Vec4> position;
        const uint32_t vertexCount = part->vertexCount;
        for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
        {
            deformVertexByFfd(position, vertex, weights, lattice);
            setPartPosition(part, vertex, position);
        }
    }
    READABLE_FUNCTION(deformPartByFfd, Draft);

    // ---- debug drawing ----------------------------------------------------------

    namespace
    {
        // Opaque yellow (RGBA bytes FF FF 00 FF).
        inline ee::Global<uint32_t, sym::g_ffdLatticeColour> g_ffdLatticeColour;
        // addDebugLine's flags for the lattice's lines (bit 0: not one
        // debugDrawFlags keeps).
        constexpr uint32_t kLatticeLineFlags = 1;

        // A cell's twelve edges in the order they are drawn, each from one
        // corner (x, y, z steps from the cell's first point) to another: the
        // square at the cell's y, the square at y + 1, then the four edges
        // between them.
        struct CellEdge
        {
            uint8_t from[3], to[3];
        };
        constexpr CellEdge kCellEdges[] = {
            {{0, 0, 0}, {1, 0, 0}}, {{1, 0, 1}, {1, 0, 0}}, {{1, 0, 1}, {0, 0, 1}}, {{0, 0, 1}, {0, 0, 0}},
            {{0, 1, 0}, {1, 1, 0}}, {{1, 1, 1}, {1, 1, 0}}, {{1, 1, 1}, {0, 1, 1}}, {{0, 1, 1}, {0, 1, 0}},
            {{0, 1, 0}, {0, 0, 0}}, {{0, 1, 1}, {0, 0, 1}}, {{1, 1, 1}, {1, 0, 1}}, {{1, 1, 0}, {1, 0, 0}},
        };

    }

    // drawFfdLattice (debug): every cell's edges as debug lines, in the
    // colour given (0: opaque yellow). Shared edges are drawn once per cell.
    void drawFfdLattice(ee::Ptr<FfdLattice> lattice, ee::Ptr<uint32_t> colour)
    {
        ee::Local<uint32_t> defaultColour;
        *defaultColour = *g_ffdLatticeColour;
        const ee::Ptr<uint32_t> lineColour = colour ? colour : ee::Ptr<uint32_t>(defaultColour);
        const auto point = [&](uint32_t x, uint32_t y, uint32_t z) {
            return &lattice->points[x + y * lattice->countX + z * lattice->countY * lattice->countX];
        };
        for (uint32_t x = 0; x < lattice->countX - 1; ++x)
        {
            for (uint32_t y = 0; y < lattice->countY - 1; ++y)
            {
                for (uint32_t z = 0; z < lattice->countZ - 1; ++z)
                {
                    for (const CellEdge &edge : kCellEdges)
                        addDebugLine(point(x + edge.from[0], y + edge.from[1], z + edge.from[2]), point(x + edge.to[0], y + edge.to[1], z + edge.to[2]), lineColour, kLatticeLineFlags);
                }
            }
        }
    }
    READABLE_FUNCTION_WAITS(drawFfdLattice, Draft);
}
