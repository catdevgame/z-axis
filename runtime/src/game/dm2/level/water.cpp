// The water (water.h): a water surface's record and its mesh's waves, and
// the node's draw check that maps the water to the view. DM2's own code (its
// game block, beside the level objects); the profile bars' names
// ("WaterProcess", "WaterDraw") are the game's. The Xbox build has the same
// record and setters, and full height tables (the PS2 build's keep one
// entry: see setWaterWaves).

#include "water.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/collision/collision.h"
#include "zge/debug/profiler.h"
#include "zge/math/angle.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/memory/regions.h"
#include "zge/render/draw.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/render/primitives.h"
#include "zge/render/terrain.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/level/water.cpp.h"

namespace game::dm2
{
    namespace
    {
        namespace calls
        {
            // Scaffolding (address-call): the engine's reflection of a vector
            // off a plane by its normal (no zge header declares it).
            void reflectVector(ee::Ptr<zge::Vec4> out, ee::Ptr<zge::Vec4> vector, ee::Ptr<zge::Vec4> normal)
            {
                ee::game(sym::reflectVector, out, vector, normal);
            }
        }

        // The water's mesh: half blended (the material state's blend mode
        // 1: (source - frame) * 0x40 / 0x80 + frame, zge/render/ps2/material_state.cpp),
        // in the water's colour (R 0xDF, G 0xDF, B 0xFF, A 0x9F).
        constexpr uint32_t kBlendHalf = 1;
        inline ee::Global<zge::VertexColour, sym::g_waterColour> g_waterColour;
        // The surface's height: the first vertex's, less this (0.1).
        inline ee::Global<ee::Float, sym::g_waterHeightOffset> g_waterHeightOffset;

        // The profile bars (a white one, "WaterProcess", which the waves'
        // step runs under, and the same with no red, "WaterDraw", which
        // nothing starts), made again for every water.
        inline ee::Global<zge::VertexColour, sym::g_waterProfileColour> g_waterProfileColour;
        constexpr uint32_t kWaterProcessName = sym::waterProcessBarName; // "WaterProcess"
        constexpr uint32_t kWaterDrawName = sym::waterDrawBarName;       // "WaterDraw"
        inline ee::Global<uint32_t, sym::g_waterProcessBar> g_waterProcessBar;
        inline ee::Global<uint32_t, sym::g_waterDrawBar> g_waterDrawBar;
        inline ee::Global<uint32_t, sym::g_waterSteps> g_waterSteps; // stepWaterWaves' calls

        // The height tables: a vertex's colour and its height in the mesh,
        // by its wave height. The PS2 build fills (and has room for) the
        // entry for height 0 only, so any other height reads past them: kept
        // (the waves never run in this build).
        constexpr int32_t kWaveTableHeights = 1;
        inline ee::Global<uint32_t, sym::g_waveTablesReady> g_waveTablesReady;
        constexpr ee::Ptr<zge::VertexColour> g_waveColours{sym::g_waveColours};
        constexpr ee::Ptr<ee::Float> g_waveLifts{sym::g_waveLifts};
        // A height's colour: blue 210 less 800 a unit of height (0 .. 255),
        // the rest 100; its height in the mesh: twice the angle of atan(20 per
        // unit) over sqrt 2.
        constexpr ee::Float kBlueAtFlat = 210.0f, kBluePerHeight = 800.0f;
        constexpr uint8_t kWaveGrey = 100;
        constexpr ee::Float kSlopePerHeight = 20.0f;
        constexpr ee::Float kTwo = 2.0f;

        // The waves' step: per vertex the neighbours' heights summed and
        // twice the faces it is in (the engine's scratch, 1024 vertices).
        constexpr ee::Ptr<int32_t> g_waveNeighbourSums{sym::g_waveNeighbourSums};
        constexpr ee::Ptr<uint32_t> g_waveNeighbourCounts{sym::g_waveNeighbourCounts};
        constexpr uint32_t kInnerVertexCount = 12; // six faces
        constexpr uint32_t kNeighbourShare = 6;    // every neighbour comes twice
        constexpr int32_t kDampingShift = 4;       // a sixteenth lost a step
        constexpr uint32_t kSettled = 0x400;       // the heights' sum under which the waves stop

        // The draw check: the camera at least 9 above the vertex (y is down),
        // the alpha nothing within 10 and up to 200 far off.
        constexpr ee::Float kLowestView = -9.0f;
        constexpr ee::Float kClearWithin = 100.0f; // squared
        constexpr ee::Float kOne = 1.0f, kHalf = 0.5f;
        constexpr ee::Float kFarAlpha = 200.0f;
        constexpr uint8_t kViewRed = 0xF0, kViewGreen = 0xF0, kViewBlue = 0xFF;
        constexpr uint32_t kWorldMatrix = 1, kInverseMatrix = 2;
    }

    void setPartBlendMode(ee::Ptr<zge::ZgePart> part, uint32_t mode)
    {
        for (uint32_t set = 0; set < static_cast<uint32_t>(zge::materialSetCount(part)); ++set)
        {
            const ee::Ptr<zge::MaterialTable> materials = zge::meshMaterials(part, set);
            if (!materials)
                continue;
            const uint32_t count = materials->count;
            for (uint32_t i = 0; i < count; ++i)
                zge::zgui::rebuildMaterialRegisters(materials->materials[i], 0, zge::kStateBlendMode, mode);
        }
    }
    READABLE_FUNCTION(setPartBlendMode, Verified);

    void setPartVertexColours(ee::Ptr<zge::ZgePart> part, ee::Ptr<const zge::VertexColour> colour)
    {
        for (uint32_t vertex = 0; vertex < part->vertexCount; ++vertex)
            zge::setPartColour(part, vertex, colour);
    }
    READABLE_FUNCTION(setPartVertexColours, Verified);

    ee::Ptr<Water> newWater(ee::Ptr<zge::Node> node)
    {
        ee::Local<zge::VertexColour> colour;
        ee::Local<zge::Vec4> first;
        ee::Local<zge::VertexColour[1]> barColour;
        *colour = *g_waterColour;
        barColour[0] = *g_waterProfileColour;
        *g_waterProcessBar = zge::newProfileBar(0, 0, barColour, kWaterProcessName);
        barColour[0].red = 0;
        *g_waterDrawBar = zge::newProfileBar(0, 0, barColour, kWaterDrawName);
        zge::setNodeBucket5(node);

        const ee::Ptr<Water> water(zge::heapAllocate(sizeof(Water)));
        water->part = node->lods->part;
        setPartBlendMode(water->part, kBlendHalf);
        setPartVertexColours(water->part, colour);
        water->inverseWorld = zge::nodeMatrix(node, kInverseMatrix);
        zge::vertexPosition(water->part, 0, first);
        zge::transformPoint(first, zge::nodeMatrix(node, kWorldMatrix), first);
        water->surfaceHeight = first->y - *g_waterHeightOffset;
        zge::setNodeDrawCheck(node, sym::waterDrawCheck, water);
        water->waves = ee::Ptr<WaterWaves>();
        water->spareWord = 0;
        water->heights = ee::Ptr<int16_t>();
        water->spareFlags[1] = 0;
        water->spareFlags[0] = 0;
        water->wavesOn = 0;
        water->uvFromView = 0;
        water->spareWords[0] = 0;
        water->draws = 0;
        return water;
    }
    READABLE_FUNCTION_WAITS(newWater, Verified);

    void setWaterUvFromView(ee::Ptr<Water> water, uint32_t on)
    {
        water->uvFromView = on;
    }
    READABLE_FUNCTION(setWaterUvFromView, Verified);

    void setWaterSpareFlag1(ee::Ptr<Water> water, uint8_t value)
    {
        water->spareFlags[1] = value;
    }
    READABLE_FUNCTION(setWaterSpareFlag1, Verified);

    void setWaterSpareFlag0(ee::Ptr<Water> water, uint8_t value)
    {
        water->spareFlags[0] = value;
    }
    READABLE_FUNCTION(setWaterSpareFlag0, Verified);

    void setWaterWaves(ee::Ptr<Water> water, uint32_t on)
    {
        water->wavesOn = on;
        if (!on)
            return;
        const ee::Ptr<WaterWaves> waves = newWaterWaves(water);
        water->waves = waves;
        water->heights = waves->heights;
        if (*g_waveTablesReady)
            return;
        for (int32_t height = 0; height < kWaveTableHeights; ++height)
        {
            const ee::Float level = ee::toFloat(height);
            int32_t blue = ee::toInt(kBlueAtFlat - level * kBluePerHeight);
            if (blue >= 0x100)
                blue = 0xFF;
            if (blue < 0)
                blue = 0;
            zge::VertexColour &colour = g_waveColours[height];
            colour.blue = static_cast<uint8_t>(blue);
            colour.red = kWaveGrey;
            colour.green = kWaveGrey;
            colour.alpha = kWaveGrey;
            const zge::Angle slope = zge::angleAtan(level * kSlopePerHeight);
            const ee::Float half = ee::toFloat(slope) / ee::sqrt(kTwo);
            g_waveLifts[height] = half + half;
        }
        *g_waveTablesReady = 1;
    }
    READABLE_FUNCTION_WAITS(setWaterWaves, Verified);

    ee::Ptr<WaterWaves> newWaterWaves(ee::Ptr<Water> water)
    {
        const ee::Ptr<zge::ZgePart> part = water->part;
        const ee::Ptr<WaterWaves> waves(zge::heapAllocate(sizeof(WaterWaves)));
        waves->vertexCount = part->vertexCount;
        waves->faceCount = part->faceCount;
        waves->previous = ee::Ptr<int16_t>(zge::heapAllocate(waves->vertexCount * 2u));
        waves->heights = ee::Ptr<int16_t>(zge::heapAllocate(waves->vertexCount * 2u));
        for (uint32_t vertex = 0; vertex < waves->vertexCount; ++vertex)
        {
            waves->heights[vertex] = 0;
            waves->previous[vertex] = 0;
        }
        waves->moving = 1;
        return waves;
    }
    READABLE_FUNCTION_WAITS(newWaterWaves, Draft);

    void freeWaterWaves(ee::Ptr<WaterWaves> waves)
    {
        zge::heapFree(waves->heights);
        zge::heapFree(waves->previous);
        game::tail(zge::heapFree, waves);
    }
    READABLE_FUNCTION_WAITS(freeWaterWaves, Draft);

    void freeWater(ee::Ptr<Water> water)
    {
        if (water->wavesOn)
            freeWaterWaves(water->waves);
        game::tail(zge::heapFree, water);
    }
    READABLE_FUNCTION_WAITS(freeWater, Verified);

    // Kept from the original: waves that have settled return before the
    // profile bar is stopped (the bar is left running).
    void stepWaterWaves(ee::Ptr<Water> water)
    {
        ee::Local<zge::Vec4> position;
        ee::Local<zge::VertexColour> colour;
        ee::Local<ee::Ptr<uint16_t>> corners;
        uint32_t activity = 0;
        ++*g_waterSteps;
        zge::startProfileBar(*g_waterProcessBar);
        if (water->wavesOn)
        {
            const ee::Ptr<WaterWaves> waves = water->waves;
            if (!waves->moving)
                return;
            const ee::Ptr<int16_t> previous = waves->previous;
            const ee::Ptr<int16_t> heights = waves->heights;
            for (uint32_t vertex = 0; vertex < waves->vertexCount; ++vertex)
            {
                g_waveNeighbourSums[vertex] = 0;
                g_waveNeighbourCounts[vertex] = 0;
            }

            const uint32_t faces = water->part->faceCount;
            for (uint32_t face = 0; face < faces; ++face)
            {
                zge::faceVertices(water->part, face, corners);
                const uint16_t a = (*corners)[0], b = (*corners)[1], c = (*corners)[2];
                g_waveNeighbourCounts[a] += 2;
                g_waveNeighbourCounts[b] += 2;
                g_waveNeighbourCounts[c] += 2;
                g_waveNeighbourSums[b] += heights[a];
                g_waveNeighbourSums[c] += heights[a];
                g_waveNeighbourSums[a] += heights[b];
                g_waveNeighbourSums[c] += heights[b];
                g_waveNeighbourSums[a] += heights[c];
                g_waveNeighbourSums[b] += heights[c];
            }

            for (uint32_t vertex = 0; vertex < waves->vertexCount; ++vertex)
            {
                int32_t height = 0;
                int32_t was = 0;
                if (g_waveNeighbourCounts[vertex] >= kInnerVertexCount)
                {
                    const int32_t pulled = static_cast<int32_t>(static_cast<uint32_t>(g_waveNeighbourSums[vertex]) / kNeighbourShare) - previous[vertex];
                    was = heights[vertex];
                    height = pulled - (pulled >> kDampingShift);
                }
                heights[vertex] = static_cast<int16_t>(height);
                previous[vertex] = static_cast<int16_t>(was);
                const int16_t entry = static_cast<int16_t>(height);
                *colour = g_waveColours[entry];
                zge::setPartColour(water->part, vertex, colour);
                zge::vertexPosition(water->part, vertex, position);
                position->y = g_waveLifts[entry];
                zge::setPartPosition(water->part, vertex, position);
                activity += static_cast<uint32_t>(height + was);
            }
            if (activity < kSettled)
                waves->moving = 0;
        }
        zge::stopProfileBar(*g_waterProcessBar);
    }
    READABLE_FUNCTION_WAITS(stepWaterWaves, Verified);

    void setFaceWaveHeight(ee::Ptr<Water> water, uint32_t face, int16_t height)
    {
        ee::Local<ee::Ptr<uint16_t>> corners;
        if (!water->wavesOn)
            return;
        zge::faceVertices(water->part, face, corners);
        water->heights[(*corners)[0]] = height;
        water->waves->moving = 1;
    }
    READABLE_FUNCTION_WAITS(setFaceWaveHeight, Verified);

    void setWaterSpareWords(ee::Ptr<Water> water, uint32_t second, uint32_t third, uint32_t first)
    {
        water->spareWords[0] = first;
        water->spareWords[1] = second;
        water->spareWords[2] = third;
    }
    READABLE_FUNCTION(setWaterSpareWords, Draft);

    // unusedWaterHook: nothing (no DM2 code calls or names it).
    void unusedWaterHook()
    {
    }
    READABLE_FUNCTION(unusedWaterHook, Draft);

    void setWaterSpareWord1(ee::Ptr<Water> water, uint32_t value)
    {
        water->spareWords[1] = value;
    }
    READABLE_FUNCTION(setWaterSpareWord1, Draft);

    int32_t waterDrawCheck(ee::Ptr<zge::Node>, ee::Address, ee::Ptr<Water> water)
    {
        ee::Local<zge::Vec4> camera;
        ee::Local<zge::VertexColour> colour;
        ee::Local<ee::Float[2]> uv;
        ee::Local<zge::Vec4> vertex;
        // The original keeps it in its frame too: the last vertex's direction (w 0) stays there below the
        // caller's stack pointer after the return, where later code reads it as stale stack.
        ee::Local<zge::Vec4> toCamera;
        ++water->draws;
        if (!water->uvFromView)
            return 1;
        colour->blue = kViewBlue;
        colour->green = kViewGreen;
        colour->red = kViewRed;
        const ee::Ptr<zge::ZgePart> part = water->part;
        if (!*zge::g_currentCamera)
            return 0;
        // The camera's place in the mesh's space.
        *camera = zge::cachedMatrix(*zge::g_currentCamera, kWorldMatrix)->translation();
        zge::transformPoint(camera, water->inverseWorld, camera);
        for (uint32_t index = 0; index < water->part->vertexCount; ++index)
        {
            zge::vertexPosition(part, index, vertex);
            *toCamera = *camera - *vertex;
            if (kLowestView < toCamera->y)
                toCamera->y = kLowestView;
            ee::Float distance;
            *toCamera = zge::unit(*toCamera, distance);
            const ee::Float near = kClearWithin / (distance * distance);
            if (kOne < near)
                colour->alpha = 0;
            else
                colour->alpha = static_cast<uint8_t>(ee::toInt((kOne - near) * kFarAlpha));
            uv[0] = toCamera->x;
            uv[1] = toCamera->z;
            zge::setPartUv(part, index, uv);
            zge::setPartColour(part, index, colour);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(waterDrawCheck, Verified);

    // setReflectionUv (no DM2 code calls or names it): a vertex's texture
    // coordinates from the direction from it to the eye reflected off a level
    // surface (normal (0, -1, 0)) and made unit: its x and z taken from
    // -1 .. 1 to 0 .. 1.
    void setReflectionUv(ee::Ptr<zge::PrimitiveVertex> vertex, ee::Ptr<zge::Vec4> eye)
    {
        ee::Local<zge::Vec4> toEye;
        ee::Local<zge::Vec4> reflected;
        ee::Local<zge::Vec4> level; // its w is never set
        *toEye = *eye - zge::quadwordOf(vertex);
        level->x = 0.0f;
        level->y = -1.0f;
        level->z = 0.0f;
        calls::reflectVector(reflected, toEye, level);
        *reflected = zge::unit(*reflected);
        vertex->u = (reflected->x + kOne) * kHalf;
        vertex->v = (reflected->z + kOne) * kHalf;
    }
    READABLE_FUNCTION(setReflectionUv, Draft);
}
