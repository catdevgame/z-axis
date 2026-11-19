// The engine's debug drawing and text (ngengine/zge3/zdebug.c): a list of
// primitives added during the frame and drawn and freed by a task, and a
// buffer of strings printed by another. The hooks at g_debugInitHook are the
// interface debug_text.cpp fills.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/debug/debug.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/render/draw.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "zge/render/primitives.h"
#include "sce/libgcc.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/debug/debug_draw.cpp.h"

namespace zge
{
    // ZGE2's debug primitives (DM2 only): the array's count, its full mark,
    // the records themselves (kDebugPrimitiveBytes each). ZGE3 has a pool.
    inline ee::Global<int32_t, sym::dm2::g_debugPrimitiveCount> g_debugPrimitiveCount;
    inline ee::Global<uint32_t, sym::dm2::g_debugPrimitivesFull> g_debugPrimitivesFull;

    // debugPrimitiveRoom: whether ZGE2's array has room for one more
    // primitive (1); full, it is marked so (0).
    inline uint32_t debugPrimitiveRoom()
    {
        if (*g_debugPrimitiveCount == 512)
        {
            *g_debugPrimitivesFull = 1;
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, debugPrimitiveRoom);

    namespace
    {
        constexpr uint32_t kDrawLines = sym::drawLines;      // (flags, line list, lines, matrix)
        constexpr uint32_t kAngleSin = sym::angleSin;       // by address: a game override hooks it
        constexpr uint32_t kDebugDrawFlags = sym::debugDrawFlags;
        constexpr uint32_t kDrawDebugLines = sym::drawDebugLines;
        constexpr uint32_t kDrawDebugAxes = sym::drawDebugAxes;
        constexpr uint32_t kDrawDebugTriangles = sym::drawDebugTriangles;
        constexpr uint32_t kDrawDebugSpheres = sym::drawDebugSpheres;
        constexpr uint32_t kDrawDebugBoxes = sym::drawDebugBoxes;
        constexpr uint32_t kDrawDebugGeometry = sym::drawDebugGeometry;
        constexpr uint32_t kDrawDebugText = sym::drawDebugText;
        constexpr uint32_t kFreeDebugPrimitive = sym::freeDebugPrimitive;
        constexpr uint32_t kDebugPrint = sym::debugPrint;

        constexpr uint32_t kSourceFile = sym::g_textNgengineZge3ZdebugC;     // "ngengine/zge3/zdebug.c"
        constexpr uint32_t kVectorWithLabelFormat = sym::vectorWithLabelFormat, kVectorFormat = sym::vectorFormat, kAnglesFormat = sym::anglesFormat;

        // A vertex of the draws (0x20 bytes).
        struct DebugVertex
        {
            Vec4 position;     // +0x00
            uint32_t u, v;     // +0x10
            uint8_t colour[4]; // +0x18
            uint32_t _1c;      // +0x1C
        };
        static_assert(sizeof(DebugVertex) == 0x20);

        // What kDrawLines takes for a line: a word, then its two vertices.
        struct DebugLineRecord
        {
            uint32_t start;
            ee::Ptr<DebugVertex> vertices[2];
        };

        // A primitive of the list: its kind, then by kind.
        enum DebugPrimitiveKind : uint32_t
        {
            kDebugAxes = 0,
            kDebugLine = 1,
            kDebugTriangle = 2,
            kDebugSphere = 3,
            kDebugBox = 4,
        };
        constexpr uint32_t kDebugPrimitiveBytes = 0x80;
        constexpr uint32_t kDebugPrimitivesABlock = 0x20;
        constexpr uint32_t kDebugBody = 0x10;      // of a record, where its kind's fields start

        struct DebugAxes
        {
            Matrix axes;
            uint16_t flags;     // +0x40 bit 0: the camera's rows at a fixed place on screen, not these
            uint16_t drawFlags; // +0x42
        };
        static_assert(sizeof(DebugAxes) == 0x50);
        constexpr uint16_t kAxesOnScreen = 1;
        constexpr uint32_t kOnScreenDrawFlag = 1u << 24; // debugDrawFlags' second argument
        constexpr float kAxesOnScreenDepth = 5.0f, kAxesLength = 0.5f;

        // A line and a triangle: their vertices, then colour and flags.
        template <uint32_t Corners>
        struct DebugPolygon
        {
            Vec4 corners[Corners];
            uint8_t colour[4];
            uint16_t flags;
        };
        using DebugLine = DebugPolygon<2>;
        using DebugTriangle = DebugPolygon<3>;
        static_assert(offsetof(DebugLine, flags) == 0x24 && offsetof(DebugTriangle, flags) == 0x34);

        struct DebugSphere
        {
            Vec4 centre;       // +0x00
            ee::Float radius;  // +0x10
            uint8_t colour[4]; // +0x14
            uint16_t flags;    // +0x18
        };
        static_assert(offsetof(DebugSphere, flags) == 0x18);

        struct DebugBox
        {
            Vec4 high, low;    // +0x00
            Matrix transform;  // +0x20
            uint8_t colour[4]; // +0x60
            uint16_t flags;    // +0x64
        };
        static_assert(offsetof(DebugBox, flags) == 0x64);

        // The draws' vertices, kept from one to the next.
        struct DebugPolygonDraw
        {
            uint32_t start;
            ee::Ptr<DebugVertex> vertices[3];
        };
        inline ee::Global<DebugPolygonDraw, sym::g_debugLineDraw> g_debugLineDraw;
        inline ee::Global<DebugPolygonDraw, sym::g_debugTriangleDraw> g_debugTriangleDraw;
        inline ee::Global<DebugVertex[2], sym::g_debugLineVertices> g_debugLineVertices;
        inline ee::Global<DebugVertex[6], sym::g_debugAxesVertices> g_debugAxesVertices;
        inline ee::Global<DebugLineRecord[3], sym::g_debugAxesLines> g_debugAxesLines;
        inline ee::Global<DebugVertex[3], sym::g_debugTriangleVertices> g_debugTriangleVertices;
        inline ee::Global<DebugVertex[8], sym::g_debugBoxVertices> g_debugBoxVertices;
        constexpr uint32_t kBoxCorners = 8, kBoxEdges = 12;
        inline ee::Global<DebugLineRecord[kBoxEdges], sym::g_debugBoxLines> g_debugBoxLines;
        inline ee::Global<uint8_t[kBoxEdges][2], sym::g_boxEdges> g_boxEdges; // pairs of g_debugBoxVertices

        inline ee::Global<uint32_t, sym::g_debugLinesFirst> g_debugLinesFirst;
        inline ee::Global<uint32_t, sym::g_debugAxesFirst> g_debugAxesFirst;
        inline ee::Global<uint32_t, sym::g_debugTrianglesFirst> g_debugTrianglesFirst;
        inline ee::Global<uint32_t, sym::g_debugBoxesFirst> g_debugBoxesFirst;

        // A sphere's bands: 8 of 22.5 degrees, each 9 quads' ends around at
        // 45, as a strip of two vertices a step.
        inline ee::Global<uint32_t, sym::g_debugSphereVertices> g_debugSphereVertices;
        inline ee::Global<uint32_t, sym::g_debugSpherePacket> g_debugSpherePacket; // a pointer to each vertex
        constexpr uint32_t kSphereBands = 8, kSphereSteps = 9;
        constexpr uint32_t kSphereVertices = kSphereBands * kSphereSteps * 2;
        constexpr float kSphereBandDegrees = 22.5f, kSphereStepDegrees = 45.0f, kAngleUnitsPerDegree = 182.04445f;

        // The current camera (debug drawing's on-screen axes).
        inline ee::Global<uint32_t, sym::g_currentCamera> g_debugCamera;

        // What debug_text.cpp fills in.
        inline ee::Global<uint32_t, sym::g_debugInitHook> g_debugInitHook;
        inline ee::Global<uint32_t, sym::g_debugShutdownHook> g_debugShutdownHook;
        inline ee::Global<uint32_t, sym::g_debugDrawTextHook> g_debugDrawTextHook; // (x, y, text)
        inline ee::Global<uint32_t, sym::g_debugTextSizeHook> g_debugTextSizeHook; // (width out, height out)

        inline ee::Global<uint32_t, sym::g_debugReady> g_debugReady;
        inline ee::Global<ee::Ptr<Task>, sym::g_debugGeometryTask> g_debugGeometryTask;
        inline ee::Global<ee::Ptr<Task>, sym::g_debugTextTask> g_debugTextTask;
        constexpr uint32_t kDebugTextStart = sym::debugTextStart;  // strings to draw: x, y (16 bits each), text, to even
        constexpr uint32_t kDebugTextWidth = sym::debugTextWidth;
        inline ee::Global<int32_t, sym::g_debugTextLineHeight> g_debugTextLineHeight;
        inline ee::Global<uint32_t, sym::g_debugTextCount> g_debugTextCount;
        inline ee::Global<uint32_t, sym::g_debugPrimitivePool> g_debugPrimitivePool;

        // (ZGE2 draws the text a stage earlier, 260, and its tasks are made at lines 642 and 646; ZGE4's at 676 and 680)
        constexpr uint32_t kGeometryTaskPriority = 258, kTextTaskPriority = game::readable::perGame(261u, 260u);
        constexpr uint32_t kGeometryTaskLine = game::readable::perGame(648u, 642u, 676u);
        constexpr uint32_t kTextTaskLine = game::readable::perGame(652u, 646u, 680u);

        constexpr float kAngleUnitsToDegrees = 0.005493164f;

        // The upper half of the addresses here: saved registers hold it,
        // and it is what the functions return when debugging is off.
        constexpr uint32_t kDebugAddressHigh = ee::high(sym::g_debugReady);

        // A point copied as the game moves it: the 16-byte quadword that
        // holds it (the low four bits of each address ignored).
        template <typename To, typename From>
        void copyQuad(ee::Ptr<To> to, ee::Ptr<From> from)
        {
            quadwordOf(to) = quadwordOf(from);
        }

        // A line's or a triangle's draw: its vertices' fixed fields and the
        // list of them the first time, then its colour and corners.

        template <uint32_t Corners>
        void fillPolygonDraw(ee::Ptr<DebugPolygon<Corners>> polygon, uint32_t &first, DebugVertex *vertices, DebugPolygonDraw &draw, uint32_t code)
        {
            if (first != 0)
            {
                first = 0;
                for (uint32_t i = 0; i < Corners; ++i)
                {
                    vertices[i].u = 0;
                    vertices[i].v = 0;
                }
                draw.start = 0;
                for (uint32_t i = 0; i < Corners; ++i)
                    draw.vertices[i] = &vertices[i];
            }
            for (uint32_t i = 0; i < Corners; ++i)
            {
                std::memcpy(vertices[i].colour, polygon->colour, sizeof(polygon->colour));
                copyQuad(ee::Ptr<DebugVertex>(&vertices[i]), ee::Ptr<Vec4>(&polygon->corners[i]));
            }
        }

        // ZGE2 keeps the primitives in a fixed array of 512 records, counted
        // (debugPrimitiveRoom), where ZGE3 takes them from a pool: one added
        // to a full array is not added (0).
        // A primitive's record if debugging is on, with its kind.
        uint32_t newPrimitive(DebugPrimitiveKind kind)
        {
            if constexpr (game::readable::kIsDm2)
            {
                if (debugPrimitiveRoom() == 0)
                    return 0;
                const int32_t index = *g_debugPrimitiveCount;
                *g_debugPrimitiveCount = index + 1;
                const uint32_t record = sym::dm2::g_debugPrimitives + static_cast<uint32_t>(index) * kDebugPrimitiveBytes;
                *ee::Ptr<uint32_t>(record) = kind;
                return record;
            }
            const uint32_t record = newDebugPrimitive();
            *ee::Ptr<uint32_t>(record) = kind;
            return record;
        }

        // A colour as the callers give it: four bytes, any alignment.
        void copyColour(uint8_t *to, uint32_t from) { std::memcpy(to, ee::Ptr<uint8_t>(from).get(), 4); }
    }

    // debugDrawFlags: the draw flags from a primitive's. (ZGE4 hands on the
    // primitive's bit 1 inverted, in place of the caller's bit 1.)
    uint32_t debugDrawFlags(uint32_t flags, uint32_t more)
    {
        constexpr uint32_t kKept = 0x2;
        if constexpr (game::readable::kIsBmx)
            return (flags & kKept) == 0 ? more | kKept : more & ~kKept;
        return flags & kKept | more;
    }
    READABLE_FUNCTION(debugDrawFlags, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        int32_t drawLines(uint32_t primitiveFlags, ee::Address vertices, uint32_t vertexCount, ee::Address vertexMatrix) { return ee::tailCall<int32_t>(kDrawLines, primitiveFlags, vertices, vertexCount, vertexMatrix); }
        }
    }

    // drawDebugLines: a line. Its vertices and the draw's list are set up on
    // the first one.
    void drawDebugLines(ee::Ptr<DebugLine> line)
    {
        fillPolygonDraw<2>(line, *g_debugLinesFirst, *g_debugLineVertices, *g_debugLineDraw, 0);
        const uint32_t flags = debugDrawFlags(line->flags, 0);
        tail::drawLines(flags, g_debugLineDraw.get(), 1, 0);
    }
    READABLE_FUNCTION_WAITS(drawDebugLines, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t debugCameraObject() { return cameraObject(ee::Ptr<Camera>(*g_debugCamera)); }
        ee::Ptr<Vec4> rowOf(Matrix &matrix, uint32_t row) { return ee::Ptr<Vec4>(reinterpret_cast<Vec4 *>(matrix.m[row])); }
    }

    // drawDebugAxes: axes as three lines from a point: red, green and blue,
    // half a unit long, the given rows at the given place, or the camera's
    // in front of it.
    void drawDebugAxes(ee::Ptr<DebugAxes> axes)
    {
        ee::Local<Vec4> place;
        DebugVertex *vertices = *g_debugAxesVertices;
        if (*g_debugAxesFirst != 0)
        {
            *g_debugAxesFirst = 0;
            constexpr uint8_t kChannels[3][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
            for (uint32_t line = 0; line < 3; ++line)
            {
                DebugLineRecord &record = (*g_debugAxesLines)[line];
                record.start = 0;
                for (uint32_t end = 0; end < 2; ++end)
                {
                    DebugVertex &vertex = vertices[line * 2 + end];
                    std::memcpy(vertex.colour, kChannels[line], 3);
                    vertex.colour[3] = 255;
                    vertex.u = 0;
                    vertex.v = 0;
                    record.vertices[end] = &vertex;
                }
            }
        }
        for (uint32_t line = 0; line < 3; ++line)
        {
            Vec4 &origin = vertices[line * 2].position;
            origin.x = ee::Float(0.0f);
            origin.y = ee::Float(0.0f);
            origin.z = ee::Float(0.0f);
        }

        // Each line's far end starts as a row of the axes.
        const auto rowsTo = [&](Matrix &rows)
        {
            for (uint32_t row = 0; row < 3; ++row)
                copyQuad(ee::Ptr<DebugVertex>(&vertices[row * 2 + 1]), rowOf(rows, row));
        };
        uint32_t onScreen = 0;
        if ((axes->flags & kAxesOnScreen) != 0)
        {
            rowsTo(*objectMatrix(ee::Ptr<SceneObject>(debugCameraObject()), 0u));
            onScreen = kOnScreenDrawFlag;
            place->x = ee::Float(0.0f);
            place->y = ee::Float(0.0f);
            place->z = ee::Float(kAxesOnScreenDepth);
        }
        else
        {
            rowsTo(axes->axes);
            copyQuad(ee::Ptr<Vec4>(place.address()), rowOf(axes->axes, 3));
        }
        for (uint32_t index = 0; index < 6; ++index)
        {
            Vec4 &position = vertices[index].position;
            position = position * ee::Float(kAxesLength);
            position = position + *place;
        }
        const uint32_t flags = debugDrawFlags(axes->drawFlags, onScreen);
        drawLines(flags, ee::Address(g_debugAxesLines.get()), 3, 0);
    }
    READABLE_FUNCTION_WAITS(drawDebugAxes, Verified);

    // drawDebugTriangles: a triangle, as drawDebugLines a line.
    void drawDebugTriangles(ee::Ptr<DebugTriangle> triangle)
    {
        fillPolygonDraw<3>(triangle, *g_debugTrianglesFirst, *g_debugTriangleVertices, *g_debugTriangleDraw, 0x370);
        const uint32_t flags = debugDrawFlags(triangle->flags, 0);
        game::tail(drawPrimitiveStrips, flags, g_debugTriangleDraw.get(), 1, 0);
    }
    READABLE_FUNCTION_WAITS(drawDebugTriangles, Verified);

    // drawDebugSpheres: a sphere: a unit sphere's strips, made afresh each time
    // in vertices made once, drawn under a matrix of its radius and centre.
    void drawDebugSpheres(ee::Ptr<DebugSphere> sphere)
    {
        ee::Local<uint32_t> vertices2;
        ee::Local<uint32_t> debugSpherePacketSlot;
        ee::Local<uint32_t> sphereVerticesSlot;
        ee::Local<uint8_t[0x30]> matrix;
        ee::Local<uint8_t[0x10]> buffer;
        ee::Local<uint8_t[0x10]> size;
        ee::Local<uint32_t> translation;
        ee::Float rimSinCos(0.0f);
        ee::Float rimSinSin(0.0f);
        ee::Float nextRimSinSin(0.0f);
        ee::Float nextBandSin(0.0f);
        ee::Float bandSin(0.0f);
        ee::Float nextBandCos(0.0f);
        ee::Float bandCos(0.0f);
        const ee::Float bandDegrees(kSphereBandDegrees), stepDegrees(kSphereStepDegrees), unitsPerDegree(kAngleUnitsPerDegree);

        uint32_t vertices = *g_debugSphereVertices;
        if (vertices == 0)
        {
            constexpr uint32_t kVertexBytes = kSphereVertices * sizeof(DebugVertex), kPacketBytes = kSphereVertices * 4;
            const uint32_t memory = heapAllocate(kVertexBytes);
            *g_debugSphereVertices = memoryFill(memory, 0, kVertexBytes);
            const uint32_t packet = heapAllocate(kPacketBytes);
            const uint32_t firstVertex = *g_debugSphereVertices;
            *g_debugSpherePacket = packet;
            *translation = buffer.address();
            const ee::Ptr<ee::Ptr<DebugVertex>> packetEntries(packet);
            for (uint32_t i = kSphereVertices; i-- > 0;)
                packetEntries[i] = ee::Ptr<DebugVertex>(firstVertex) + i;
            vertices = *g_debugSphereVertices;
        }
        else
            *translation = buffer.address();

        // An index of steps as a 16-bit angle.
        const auto angleOf = [&](ee::Float index, ee::Float degrees) { return signed16(static_cast<uint32_t>(ee::toInt(index * degrees * unitsPerDegree))); };

        int32_t band = 0;
        int32_t nextBand = 0;
        int32_t step = 0;
        ee::Ptr<DebugVertex> pair(vertices); // this step's two vertices, on this band and the next
        for (;;)
        {
            nextBand = band + 1;
            step = 0;
            const int32_t bandAngle = angleOf(ee::toFloat(band), bandDegrees);
            bandSin = angleSin(bandAngle);
            const int32_t nextBandAngle = angleOf(ee::toFloat(nextBand), bandDegrees);
            nextBandSin = angleSin(nextBandAngle);
            bandCos = angleCos(static_cast<Angle>(bandAngle));
            nextBandCos = angleCos(static_cast<Angle>(nextBandAngle));
            for (;;)
            {
                const ee::Float index = ee::toFloat(step);
                ++step;
                const int32_t aroundAngle = angleOf(index, stepDegrees);
                rimSinSin = bandSin * angleSin(aroundAngle);
                rimSinCos = bandSin * angleCos(static_cast<Angle>(aroundAngle));
                nextRimSinSin = nextBandSin * angleSin(aroundAngle);
                const ee::Float nextRimCos = angleCos(static_cast<Angle>(aroundAngle));
                DebugVertex &onBand = pair[0];
                DebugVertex &onNextBand = pair[1];
                std::memcpy(onBand.colour, sphere->colour, sizeof(onBand.colour));
                onBand.position.x = rimSinSin;
                onBand.position.z = rimSinCos;
                onBand.position.y = bandCos;
                std::memcpy(onNextBand.colour, sphere->colour, sizeof(onNextBand.colour));
                onNextBand.position.x = nextRimSinSin;
                onNextBand.position.z = nextBandSin * nextRimCos;
                onNextBand.position.y = nextBandCos;
                pair += 2;
                if (step < static_cast<int32_t>(kSphereSteps))
                    continue;
                break;
            }
            band = nextBand;
            if (band < static_cast<int32_t>(kSphereBands))
                continue;
            break;
        }
        matrixIdentity(ee::Ptr<Matrix>(matrix.address()));
        Vec3 &scale = *ee::Ptr<Vec3>(size.address());
        scale.z = sphere->radius;
        scale.x = sphere->radius;
        scale.y = sphere->radius;
        matrixScale(ee::Ptr<Matrix>(matrix.address()), ee::Ptr<const Matrix>(matrix.address()), ee::Ptr<const Vec3>(size.address()));
        copyQuad(ee::Ptr<Vec4>(*translation), sphere);
        const uint32_t sphereFlags = sphere->flags;
        *debugSpherePacketSlot = *g_debugSpherePacket;
        *sphereVerticesSlot = kSphereVertices;
        *vertices2 = 0;
        const uint32_t flags = debugDrawFlags(sphereFlags, 0);
        drawPrimitives(flags, vertices2, 1u, matrix);
    }
    READABLE_FUNCTION_WAITS(drawDebugSpheres, Verified);

    // drawDebugBoxes: a box: its eight corners, high or low in each direction
    // by the bits of their number, and twelve lines (g_boxEdges) under its
    // transform.
    int32_t drawDebugBoxes(ee::Ptr<DebugBox> box)
    {
        DebugVertex *vertices = *g_debugBoxVertices;
        if (*g_debugBoxesFirst != 0)
        {
            *g_debugBoxesFirst = 0;
            for (uint32_t i = 0;;)
            {
                vertices[i].u = 0;
                vertices[i].v = 0;
                if (++i < kBoxCorners)
                    continue;
                break;
            }
            // The one line's start cleared, by the corners' count left in
            // the index; the others are zero from the start.
            (*g_debugBoxLines)[kBoxCorners].start = 0;
            for (uint32_t edge = 0;;)
            {
                DebugLineRecord &line = (*g_debugBoxLines)[edge];
                line.vertices[0] = &(*g_debugBoxVertices)[(*g_boxEdges)[edge][0]];
                line.vertices[1] = &(*g_debugBoxVertices)[(*g_boxEdges)[edge][1]];
                if (++edge < kBoxEdges)
                    continue;
                break;
            }
        }
        for (uint32_t corner = 0;;)
        {
            DebugVertex &vertex = vertices[corner];
            ee::Float x = box->low.x;
            if ((corner & 1) != 0)
                x = box->high.x;
            vertex.position.x = x;
            ee::Float y = box->high.y;
            if ((corner & 2) == 0)
                y = box->low.y;
            vertex.position.y = y;
            ee::Float z = box->high.z;
            if ((corner & 4) == 0)
                z = box->low.z;
            vertex.position.z = z;
            std::memcpy(vertex.colour, box->colour, sizeof(vertex.colour));
            if (++corner < kBoxCorners)
                continue;
            break;
        }
        const uint32_t flags = debugDrawFlags(box->flags, 0);
        // The lines go under the box's transform (0x10); ZGE2 sets no such flag.
        constexpr uint32_t kUnderTransform = game::readable::perGame(0x10u, 0u);
        return tail::drawLines(flags | kUnderTransform, g_debugBoxLines.get(), kBoxEdges, &box->transform);
    }
    READABLE_FUNCTION_WAITS(drawDebugBoxes, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t firstPrimitiveLink() { return poolFirst(ee::Ptr<PoolList>(*g_debugPrimitivePool)); }
        uint32_t primitiveOfLink(uint32_t link) { return poolRecord(link); }
        void drawPrimitive(uint32_t draw, uint32_t body) { ee::game(draw, body); }
        // The text back end's three methods.
        int32_t runDebugHook(uint32_t hook) { return ee::game<int32_t>(hook); }
        void drawTextThroughHook(int32_t x, int32_t y, uint32_t text) { ee::game(*g_debugDrawTextHook, x, y, text); }
        void askTextSize(ee::Address width, ee::Address height) { ee::game(*g_debugTextSizeHook, width, height); }
    }

    // drawDebugGeometry: the task of the primitives: each drawn and freed.
    uint32_t drawDebugGeometry()
    {
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2: the array's primitives drawn in order, then the array
            // emptied (debugging on or not).
            if (*g_debugEnabled != 0)
            {
                uint32_t primitive = sym::dm2::g_debugPrimitives;
                for (int32_t left = *g_debugPrimitiveCount - 1; left != -1; --left)
                {
                    const uint32_t body = primitive + kDebugBody;
                    switch (*ee::Ptr<uint32_t>(primitive))
                    {
                    case kDebugAxes:
                        drawPrimitive(kDrawDebugAxes, body);
                        break;
                    case kDebugLine:
                        drawPrimitive(kDrawDebugLines, body);
                        break;
                    case kDebugTriangle:
                        drawPrimitive(kDrawDebugTriangles, body);
                        break;
                    case kDebugSphere:
                        drawPrimitive(kDrawDebugSpheres, body);
                        break;
                    case kDebugBox:
                        drawPrimitive(kDrawDebugBoxes, body);
                        break;
                    default:
                        break;
                    }
                    primitive += kDebugPrimitiveBytes;
                }
            }
            *g_debugPrimitivesFull = 0;
            *g_debugPrimitiveCount = 0;
            return 1;
        }
        if (*g_debugEnabled != 0)
        {
            // Drawing frees each one, so the first left is always the next.
            for (uint32_t link = firstPrimitiveLink(); link != 0; link = firstPrimitiveLink())
            {
                const uint32_t primitive = primitiveOfLink(link);
                const uint32_t body = primitive + kDebugBody;
                switch (*ee::Ptr<uint32_t>(primitive))
                {
                case kDebugAxes:
                    drawPrimitive(kDrawDebugAxes, body);
                    break;
                case kDebugLine:
                    drawPrimitive(kDrawDebugLines, body);
                    break;
                case kDebugTriangle:
                    drawPrimitive(kDrawDebugTriangles, body);
                    break;
                case kDebugSphere:
                    drawPrimitive(kDrawDebugSpheres, body);
                    break;
                case kDebugBox:
                    drawPrimitive(kDrawDebugBoxes, body);
                    break;
                default:
                    break;
                }
                poolFree(ee::Ptr<Pool>(*g_debugPrimitivePool), primitive);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawDebugGeometry, Verified);

    // resetDebugText (ZGE4 only; its initDebug and drawDebugText end with it): the
    // strings' buffer emptied, none counted, no overflow, and the colour the
    // next strings are printed in back to grey (192, 192, 192, opaque).
    inline void resetDebugText()
    {
        const ee::Ptr<uint8_t> colour(sym::bmx::g_debugPrintColour);
        constexpr uint8_t kGrey = 0xC0, kOpaque = 0xFF;
        colour[2] = kGrey;
        colour[3] = kOpaque;
        *g_debugTextCursor = kDebugTextStart;
        *g_debugTextCount = 0;
        colour[0] = kGrey;
        colour[1] = kGrey;
        *g_debugTextOverflowed = 0;
    }
    READABLE_FUNCTION_ONLY(bmx, resetDebugText);

    // drawDebugText: the task of the strings: each drawn, and the buffer
    // emptied. A negative y counts lines down from the last string's.
    uint32_t drawDebugText()
    {
        uint32_t entry = kDebugTextStart;
        if (*g_debugEnabled != 0 && entry < *g_debugTextCursor)
        {
            int32_t nextY = 0;
            for (;;)
            {
                const ee::Ptr<DebugTextEntry> line(entry);
                int32_t y = line->y;
                const int32_t x = line->x;
                if (y < 0)
                    y = static_cast<int32_t>(static_cast<uint32_t>(~y) * static_cast<uint32_t>(*g_debugTextLineHeight)) + nextY;
                uint32_t text = entry + 4;
                nextY = y + *g_debugTextLineHeight;
                drawTextThroughHook(x, signed16(static_cast<uint32_t>(y)), text);
                while (*ee::Ptr<int8_t>(text++) != 0)
                {
                }
                entry = text + 1 & ~1u;
                if (entry < *g_debugTextCursor)
                    continue;
                break;
            }
        }
        *g_debugTextOverflowed = 0;
        *g_debugTextCursor = kDebugTextStart;
        return 1;
    }
    READABLE_EXCEPT(bmx, READABLE_FUNCTION(drawDebugText, Verified)); // (BMX XXX's: game/bmx/debug/debug_print.cpp)

    // newDebugPrimitive: a record for a primitive.
    uint32_t newDebugPrimitive()
    {
        return poolAlloc(ee::Ptr<Pool>(*g_debugPrimitivePool));
    }
    READABLE_FUNCTION_WAITS(newDebugPrimitive, Verified);

    // freeDebugPrimitive: the pool's free function for its records.
    uint32_t freeDebugPrimitive(uint32_t record)
    {
        poolFree(ee::Ptr<Pool>(*g_debugPrimitivePool), record);
        return 1;
    }
    READABLE_FUNCTION(freeDebugPrimitive, Draft);

    // initDebug: debug drawing up: the pool, its two tasks, the text's
    // size. 0 if the text interface wouldn't start.
    int32_t initDebug()
    {
        if (runDebugHook(*g_debugInitHook) == 0)
            return 0;
        *g_debugEnabled = 1;
        if constexpr (!game::readable::kIsDm2) // (ZGE2's primitives are an array: no pool)
            *g_debugPrimitivePool = createPool(kDebugPrimitiveBytes, kDebugPrimitivesABlock, kFreeDebugPrimitive);
        *g_debugGeometryTask = newTask(kGeometryTaskPriority, kDrawDebugGeometry, 0, 1, kSourceFile, kGeometryTaskLine);
        *g_debugTextTask = newTask(kTextTaskPriority, kDrawDebugText, 0, 1, kSourceFile, kTextTaskLine);
        askTextSize(kDebugTextWidth, g_debugTextLineHeight.get());
        *g_debugReady = 1;
        if constexpr (game::readable::kIsBmx)
            resetDebugText();
        else
        {
            *g_debugTextCursor = kDebugTextStart;
            *g_debugTextCount = 0;
        }
        if constexpr (game::readable::kIsDm2)
            *g_debugPrimitiveCount = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initDebug, Verified);

    // shutdownDebugSystem: debug drawing down; the text interface may refuse.
    uint32_t shutdownDebugSystem()
    {
        if (*g_debugReady != 0)
        {
            if (runDebugHook(*g_debugShutdownHook) == 0)
                return 0;
            if constexpr (!game::readable::kIsDm2) // (ZGE2 has no pool)
            {
                destroyPool(ee::Ptr<Pool>(*g_debugPrimitivePool));
                *g_debugPrimitivePool = 0;
            }
            if (*g_debugGeometryTask != 0)
            {
                deleteTask(*g_debugGeometryTask);
                *g_debugGeometryTask = 0;
            }
            if (*g_debugTextTask != 0)
            {
                deleteTask(*g_debugTextTask);
                *g_debugTextTask = 0;
            }
            *g_debugReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownDebugSystem, Verified);

    // setDebugMenuSelection: drawing on or off; returns what it was.
    uint32_t setDebugMenuSelection(uint32_t enabled)
    {
        const uint32_t was = *g_debugEnabled;
        *g_debugEnabled = enabled;
        return was;
    }
    READABLE_FUNCTION(setDebugMenuSelection, Verified);

    // addDebugMarker: axes on the list: a matrix, or the camera's if none.
    uint32_t addDebugMarker(uint32_t matrix, uint32_t drawFlags)
    {
        if (*g_debugEnabled == 0)
            return kDebugAddressHigh;
        const uint32_t record = newPrimitive(kDebugAxes);
        if (game::readable::kIsDm2 && record == 0) // (ZGE2: the array is full)
            return 0;
        const ee::Ptr<DebugAxes> axes(record + kDebugBody);
        axes->drawFlags = static_cast<uint16_t>(drawFlags);
        axes->flags = 0;
        uint32_t result = record;
        if (matrix != 0)
        {
            result = record + kDebugBody;
            for (uint32_t row = 0; row < 4; ++row)
                copyQuad(rowOf(axes->axes, row), ee::Ptr<Vec4>(matrix) + row);
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(addDebugMarker, Verified);

    // addDebugLine: a line on the list.
    uint32_t addDebugLine(ee::Address start, ee::Address end, ee::Address colour, uint32_t flags)
    {
        if (*g_debugEnabled == 0)
            return kDebugAddressHigh;
        const uint32_t record = newPrimitive(kDebugLine);
        if (game::readable::kIsDm2 && record == 0) // (ZGE2: the array is full)
            return 0;
        const ee::Ptr<DebugLine> line(record + kDebugBody);
        copyQuad(ee::Ptr<Vec4>(&line->corners[0]), ee::Ptr<Vec4>(start));
        copyQuad(ee::Ptr<Vec4>(&line->corners[1]), ee::Ptr<Vec4>(end));
        if constexpr (game::readable::kIsBmx) // (ZGE4: a line with no colour gets the default)
            copyColour(line->colour, colour != 0 ? static_cast<uint32_t>(colour) : uint32_t{sym::bmx::g_defaultDebugLineColour});
        else
            copyColour(line->colour, colour);
        line->flags = static_cast<uint16_t>(flags);
        return record;
    }
    READABLE_FUNCTION_WAITS(addDebugLine, Verified);

    // addDebugScreenAxes (nothing calls it in AI): the camera's axes on the
    // list, at their fixed place on screen (kAxesOnScreen). The record, or
    // kDebugAddressHigh with debugging off.
    uint32_t addDebugScreenAxes()
    {
        if (*g_debugEnabled == 0)
            return kDebugAddressHigh;
        const uint32_t record = newPrimitive(kDebugAxes);
        if (game::readable::kIsDm2 && record == 0) // (ZGE2: the array is full)
            return 0;
        const ee::Ptr<DebugAxes> axes(record + kDebugBody);
        axes->drawFlags = 0;
        axes->flags = kAxesOnScreen;
        return record;
    }
    READABLE_FUNCTION_WAITS(addDebugScreenAxes, Draft);

    // addDebugText (its name is older than what it was found to do: a box;
    // reportRegions and the world reports call it): a box on the list, its
    // two corners (high, then low) through the transform (the identity when
    // none is given). Returns the colour's word, which the original leaves
    // last (kDebugAddressHigh with debugging off).
    int32_t addDebugText(ee::Address corners, ee::Ptr<Matrix> transform, ee::Address colour, uint32_t flags)
    {
        if (*g_debugEnabled == 0)
            return kDebugAddressHigh;
        const uint32_t record = newPrimitive(kDebugBox);
        if (game::readable::kIsDm2 && record == 0) // (ZGE2: the array is full)
            return 0;
        const ee::Ptr<DebugBox> box(record + kDebugBody);
        std::memcpy(&box->high, ee::Ptr<const Vec4>(corners).get(), 2 * sizeof(Vec4));
        if (transform)
            std::memcpy(&box->transform, transform.get(), sizeof(Matrix));
        else
            matrixIdentity(&box->transform);
        copyColour(box->colour, colour);
        box->flags = static_cast<uint16_t>(flags);
        int32_t colourWord;
        std::memcpy(&colourWord, box->colour, sizeof(colourWord));
        return colourWord;
    }
    READABLE_FUNCTION_WAITS(addDebugText, Draft);

    // drawTriangle: a triangle on the list.
    uint32_t drawTriangle(ee::Address corner0, ee::Address corner1, ee::Address corner2, ee::Address colour, uint32_t flags)
    {
        if (*g_debugEnabled == 0)
            return kDebugAddressHigh;
        const uint32_t record = newPrimitive(kDebugTriangle);
        if (game::readable::kIsDm2 && record == 0) // (ZGE2: the array is full)
            return 0;
        const ee::Ptr<DebugTriangle> triangle(record + kDebugBody);
        copyQuad(ee::Ptr<Vec4>(&triangle->corners[0]), ee::Ptr<Vec4>(corner0));
        copyQuad(ee::Ptr<Vec4>(&triangle->corners[1]), ee::Ptr<Vec4>(corner1));
        copyQuad(ee::Ptr<Vec4>(&triangle->corners[2]), ee::Ptr<Vec4>(corner2));
        if constexpr (game::readable::kIsBmx) // (ZGE4: a triangle with no colour gets the default)
            copyColour(triangle->colour, colour != 0 ? static_cast<uint32_t>(colour) : uint32_t{sym::bmx::g_defaultDebugTriangleColour});
        else
            copyColour(triangle->colour, colour);
        triangle->flags = static_cast<uint16_t>(flags);
        return record;
    }
    READABLE_FUNCTION_WAITS(drawTriangle, Verified);

    // addDebugPoint: a sphere on the list.
    uint32_t addDebugPoint(ee::Address position, ee::Address colour, uint32_t flags, uint32_t, ee::Float radius)
    {
        if (*g_debugEnabled == 0)
            return kDebugAddressHigh;
        const uint32_t record = newPrimitive(kDebugSphere);
        if (game::readable::kIsDm2 && record == 0) // (ZGE2: the array is full)
            return 0;
        const ee::Ptr<DebugSphere> sphere(record + kDebugBody);
        copyQuad(sphere, ee::Ptr<Vec4>(position));
        sphere->radius = radius;
        if constexpr (game::readable::kIsBmx) // (ZGE4: a point with no colour gets the default)
            copyColour(sphere->colour, colour != 0 ? static_cast<uint32_t>(colour) : uint32_t{sym::bmx::g_defaultDebugPointColour});
        else
            copyColour(sphere->colour, colour);
        sphere->flags = static_cast<uint16_t>(flags);
        return record;
    }
    READABLE_FUNCTION_WAITS(addDebugPoint, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        // debugPrint's tail call with three numbers, and with a label before them.
        namespace tail
        {
        int32_t printThree(int32_t x, int32_t y, uint32_t format, uint64_t first, uint64_t second, uint64_t third) { return ee::tailCall<int32_t>(kDebugPrint, x, y, format, first, second, third); }
        int32_t printLabelled(int32_t x, int32_t y, uint32_t format, uint64_t label, uint64_t first, uint64_t second, uint64_t third) { return ee::tailCall<int32_t>(kDebugPrint, x, y, format, label, first, second, third); }
        }
    }

    // debugPrintVector: a vector's three numbers, with a label if it is given.
    int32_t debugPrintVector(uint32_t x, uint32_t y, uint64_t label, ee::Ptr<Vec3> vector)
    {
        const int32_t screenX = signed16(x), screenY = signed16(y);
        const uint32_t format = label != 0 ? kVectorWithLabelFormat : kVectorFormat;
        const uint64_t first = sce::libgcc::fptodp(vector->x);
        const uint64_t second = sce::libgcc::fptodp(vector->y);
        const uint64_t third = sce::libgcc::fptodp(vector->z);
        if (label != 0)
            return tail::printLabelled(screenX, screenY, format, label, first, second, third);
        return tail::printThree(screenX, screenY, format, first, second, third);
    }
    READABLE_FUNCTION(debugPrintVector, Verified);

    // debugPrintAngles: three 16-bit angles in degrees, with a label if given.
    int32_t debugPrintAngles(uint32_t x, uint32_t y, uint64_t label, ee::Ptr<EulerAngles> angles)
    {
        const int32_t screenX = signed16(x), screenY = signed16(y);
        const ee::Float degrees(kAngleUnitsToDegrees);
        const uint32_t format = label != 0 ? kVectorWithLabelFormat : kAnglesFormat;
        const uint64_t first = sce::libgcc::fptodp(ee::toFloat(static_cast<int32_t>(angles->x)) * degrees);
        const uint64_t second = sce::libgcc::fptodp(ee::toFloat(static_cast<int32_t>(angles->y)) * degrees);
        const uint64_t third = sce::libgcc::fptodp(ee::toFloat(static_cast<int32_t>(angles->z)) * degrees);
        if (label != 0)
            return tail::printLabelled(screenX, screenY, format, label, first, second, third);
        return tail::printThree(screenX, screenY, format, first, second, third);
    }
    READABLE_FUNCTION(debugPrintAngles, Verified);

    namespace
    {
        constexpr uint32_t kFloatWithLabelFormat = sym::debugFloatWithLabelFormat; // "%s% 10.4f"
        constexpr uint32_t kFloatFormat = sym::debugFloatFormat;                   // "a:% 10.4f"
        constexpr uint32_t kVector4WithLabelFormat = sym::vector4WithLabelFormat;  // "%s" and four "% 10.4f"
        constexpr uint32_t kVector4Format = sym::vector4Format;                    // four "% 10.4f"
        constexpr ee::Float kAngleUnitDegrees = ee::Float(0.0054931640625f);         // 360 / 65536

        namespace tail
        {
            int32_t printOne(int32_t x, int32_t y, uint32_t format, uint64_t value) { return ee::tailCall<int32_t>(kDebugPrint, x, y, format, value); }
            int32_t printLabelledOne(int32_t x, int32_t y, uint32_t format, uint64_t label, uint64_t value) { return ee::tailCall<int32_t>(kDebugPrint, x, y, format, label, value); }
            int32_t printLabelledFour(int32_t x, int32_t y, uint32_t format, uint64_t label, uint64_t first, uint64_t second, uint64_t third, uint64_t fourth)
            {
                return ee::tailCall<int32_t>(kDebugPrint, x, y, format, label, first, second, third, fourth);
            }
        }
    }

    // debugPrintFloat (nothing calls it in AI): a number, after the label
    // if it is given ("a:" before it if not).
    int32_t debugPrintFloat(uint32_t x, uint32_t y, uint64_t label, ee::Float value)
    {
        const int32_t screenX = signed16(x), screenY = signed16(y);
        if (label != 0)
            return tail::printLabelledOne(screenX, screenY, kFloatWithLabelFormat, label, sce::libgcc::fptodp(value));
        return tail::printOne(screenX, screenY, kFloatFormat, sce::libgcc::fptodp(value));
    }
    READABLE_FUNCTION(debugPrintFloat, Draft);

    // debugPrintAngle (was debugPrintFloatAt; nothing calls it in AI): a
    // 16-bit angle in degrees, as debugPrintFloat prints a number.
    int32_t debugPrintAngle(uint32_t x, uint32_t y, uint64_t label, uint32_t angle)
    {
        const int32_t screenX = signed16(x), screenY = signed16(y);
        const ee::Float degrees = ee::toFloat(static_cast<int16_t>(angle)) * kAngleUnitDegrees;
        if (label != 0)
            return tail::printLabelledOne(screenX, screenY, kFloatWithLabelFormat, label, sce::libgcc::fptodp(degrees));
        return tail::printOne(screenX, screenY, kFloatFormat, sce::libgcc::fptodp(degrees));
    }
    READABLE_FUNCTION(debugPrintAngle, Draft);

    // debugPrintVec4 (nothing calls it in AI): a vector's four numbers, after
    // the label if it is given. Kept bug: with no label the 0 stays in the
    // label's place, so the format's four numbers are 0, x, y and z.
    int32_t debugPrintVec4(uint32_t x, uint32_t y, uint64_t label, ee::Ptr<Vec4> vector)
    {
        const int32_t screenX = signed16(x), screenY = signed16(y);
        const uint32_t format = label != 0 ? kVector4WithLabelFormat : kVector4Format;
        const uint64_t first = sce::libgcc::fptodp(vector->x);
        const uint64_t second = sce::libgcc::fptodp(vector->y);
        const uint64_t third = sce::libgcc::fptodp(vector->z);
        const uint64_t fourth = sce::libgcc::fptodp(vector->w);
        return tail::printLabelledFour(screenX, screenY, format, label, first, second, third, fourth);
    }
    READABLE_FUNCTION(debugPrintVec4, Draft);

    // debugPrintMatrix: a matrix, four lines of three numbers (y -1: the next
    // line), to the right of the label if it is given.
    int32_t debugPrintMatrix(uint32_t x, uint32_t y, uint32_t text, ee::Ptr<Matrix> matrix)
    {
        ee::Local<uint32_t> charWidth;
        ee::Local<uint32_t> charHeight;
        const int32_t screenX = signed16(x), screenY = signed16(y);
        const auto number = [&](uint32_t row, uint32_t column) { return sce::libgcc::fptodp(matrix->m[row][column]); };
        // The lines after the first go straight below it.
        const auto printLine = [&](int32_t atX, uint32_t row0, uint32_t column0, uint32_t row1, uint32_t column1, uint32_t row2, uint32_t column2)
        {
            const uint64_t first = number(row0, column0);
            const uint64_t second = number(row1, column1);
            const uint64_t third = number(row2, column2);
            return debugPrint(atX, -1, kVectorFormat, first, second, third);
        };

        int32_t numbersX = screenX;
        if (text != 0)
        {
            const int32_t length = stringLength(text);
            askTextSize(charWidth, charHeight);
            const int32_t labelEnd = static_cast<int32_t>(static_cast<uint32_t>(length) * *charWidth) + screenX;
            const uint64_t first = number(0, 0);
            const uint64_t second = number(1, 0);
            const uint64_t third = number(2, 0);
            debugPrint(screenX, screenY, kVectorWithLabelFormat, text, first, second, third);
            numbersX = signed16(static_cast<uint32_t>(labelEnd));
        }
        else
        {
            const uint64_t first = number(0, 0);
            const uint64_t second = number(1, 0);
            const uint64_t third = number(2, 0);
            debugPrint(screenX, screenY, kVectorFormat, first, second, third);
        }
        printLine(numbersX, 0, 1, 1, 1, 2, 1);
        printLine(numbersX, 0, 2, 1, 2, 2, 2);
        return printLine(numbersX, 3, 0, 3, 1, 3, 2);
    }
    READABLE_FUNCTION(debugPrintMatrix, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_litoRecords> g_litoRecords;
        template <typename T> T loadUnaligned(uint32_t address) { T v; std::memcpy(&v, ee::at<uint8_t>(address), sizeof(T)); return v; }
        template <typename T> void storeUnaligned(uint32_t address, T v) { std::memcpy(ee::at<uint8_t>(address), &v, sizeof(T)); }
    }

    namespace
    {
        namespace routed
        {
        }
    }

    void debugPrintNoop()
    {
    }
    READABLE_FUNCTION(debugPrintNoop, Verified);

    uint32_t debugStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(debugStubZero, Draft);

    uint32_t sceneObjectsHook()
    {
        return 0;
    }
    READABLE_FUNCTION(sceneObjectsHook, Verified);

    void debugStubNoop()
    {
    }
    READABLE_FUNCTION(debugStubNoop, Draft);
}
