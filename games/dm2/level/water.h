#pragma once
// The water (water.cpp): a water surface's record, made for its node's
// mesh. At every draw its node's draw check can set each vertex's texture
// coordinates and alpha from where the camera is; with its waves on, the
// mesh's vertices ripple (a touch pushes one down, the waves spread and
// settle). DM2's own code (its game block, beside the level objects).
//
// The waves are never turned on in this build (newWaterSurface makes every
// surface without them), and the PS2 build's height tables keep only the
// entry for height 0 (the Xbox build's have one for every height).

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/math/types.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct Matrix;
    struct Node;
    struct VertexColour;
    struct ZgePart;
}

namespace game::dm2
{
    // The waves of a water's mesh: a height for every vertex, now and a
    // step before (16-bit, 0 flat).
    struct WaterWaves
    {
        ee::Ptr<int16_t> heights;   // +0x00
        ee::Ptr<int16_t> previous;  // +0x04
        uint16_t vertexCount;       // +0x08 the mesh's
        uint16_t faceCount;         // +0x0A
        // +0x0C 1 while the waves move: set when made and at every push,
        // cleared by the step that finds them settled.
        uint32_t moving;
    };
    static_assert(offsetof(WaterWaves, vertexCount) == 0x08 && sizeof(WaterWaves) == 0x10);

    // A water surface's record (0x34 bytes, from the heap).
    struct Water
    {
        ee::Ptr<WaterWaves> waves;          // +0x00 while wavesOn
        ee::Ptr<zge::ZgePart> part;         // +0x04 the node's mesh (its collision frame's part)
        ee::Ptr<zge::Matrix> inverseWorld;  // +0x08 the node's (takes the camera into the mesh's space)
        uint32_t spareWord;                 // +0x0C cleared when made; no DM2 build (PS2, Xbox) reads it
        // +0x10 the height of the mesh's first vertex in the world, less
        // 0.1: the rider's water level when it touches the surface.
        ee::Float surfaceHeight;
        // +0x14 1: at each draw every vertex's texture coordinates are the
        // direction from it to the camera (x and z of the unit vector) and
        // its alpha grows with the distance (waterDrawCheck).
        uint32_t uvFromView;
        uint32_t wavesOn;                   // +0x18 the waves made (setWaterWaves)
        // +0x1C three words and +0x28 two bytes only setters write (none
        // cleared but the bytes and the first word); no DM2 build reads them.
        uint32_t spareWords[3];
        uint8_t spareFlags[2];
        uint8_t _2a[2];
        ee::Ptr<int16_t> heights;           // +0x2C waves->heights, the ones a push sets
        uint32_t draws;                     // +0x30 the draw check's count of its calls
    };
    static_assert(offsetof(Water, surfaceHeight) == 0x10 && offsetof(Water, wavesOn) == 0x18);
    static_assert(offsetof(Water, spareFlags) == 0x28 && offsetof(Water, heights) == 0x2C && sizeof(Water) == 0x34);

    // Every material of the part's sets given the blend mode (the material
    // state kStateBlendMode).
    void setPartBlendMode(ee::Ptr<zge::ZgePart> part, uint32_t mode);
    // Every vertex of the part given the colour.
    void setPartVertexColours(ee::Ptr<zge::ZgePart> part, ee::Ptr<const zge::VertexColour> colour);

    // A node's water: its mesh half blended and coloured, the node drawn in
    // bucket 5 with waterDrawCheck as its draw check. Everything else off.
    // Also makes two profile bars ("WaterProcess", "WaterDraw") at every call.
    ee::Ptr<Water> newWater(ee::Ptr<zge::Node> node);
    void setWaterUvFromView(ee::Ptr<Water> water, uint32_t on);
    void setWaterSpareFlag0(ee::Ptr<Water> water, uint8_t value);
    void setWaterSpareFlag1(ee::Ptr<Water> water, uint8_t value);
    // The waves on (made, flat and moving) or off (the record kept as it
    // is); the first time on, the height tables are filled.
    void setWaterWaves(ee::Ptr<Water> water, uint32_t on);
    ee::Ptr<WaterWaves> newWaterWaves(ee::Ptr<Water> water);
    void freeWaterWaves(ee::Ptr<WaterWaves> waves);
    // The water's record freed, its waves first when on.
    void freeWater(ee::Ptr<Water> water);
    // A step of the waves (with the waves on and moving): each inner vertex
    // (in six faces or more) moves to a third of its neighbours' heights
    // less its height a step before, less a sixteenth; an edge vertex is
    // flat. Each vertex's colour and height come from the tables by its new
    // height. The waves stop moving once the heights add up to under 0x400.
    void stepWaterWaves(ee::Ptr<Water> water);
    // With the waves on: the face's first vertex set to the height, the
    // waves moving again.
    void setFaceWaveHeight(ee::Ptr<Water> water, uint32_t face, int16_t height);
    // The node's draw check (setNodeDrawCheck): 0, the node not drawn, when
    // there is no camera; else 1, the vertices first set from the view when
    // uvFromView is on.
    int32_t waterDrawCheck(ee::Ptr<zge::Node> node, ee::Address level, ee::Ptr<Water> water);
}
