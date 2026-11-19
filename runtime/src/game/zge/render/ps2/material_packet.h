#pragma once

// A material's packet's PS2 form (ps2/material_packet.cpp): the routines VU1
// runs before the material's strips, written by a writer at the first
// layer's routines (beginMaterialPacket .. finishMaterialPacket, which sets
// the packet's size), and the layers' first GS state.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/render/material.h"

#include <cstdint>

namespace zge
{
    // The VU1 routines, a quadword each: the first word says which.
    struct Routine
    {
        uint32_t words[4];
    };

    // The writer of a material's routines.
    struct PacketWriter
    {
        ee::Ptr<Material> material; // +0x00
        ee::Ptr<Routine> start;     // +0x04
        ee::Ptr<Routine> cursor;    // +0x08
    };

    // The first layer's routines (Material::effectRoutines in ZGE3's header, which lays that layer out).
    inline ee::Ptr<Routine> routinesOf(ee::Ptr<Material> material) { return layerRoutines(material, 0).cast<Routine>(); }

    // Where the matrix routine (the first, a quadword) has its rows: the texture matrix (kind 9).
    inline ee::Ptr<Vec4> rowsAt(ee::Ptr<Routine> routine) { return routine.cast<Vec4>(); }
    inline ee::Ptr<Vec4> textureMatrixRows(ee::Ptr<Material> material) { return rowsAt(routinesOf(material) + 1); }

    // The four rows of a matrix copied whole, each to and from the quadword it
    // sits in.
    inline void copyRows(ee::Ptr<Vec4> to, ee::Ptr<Vec4> from)
    {
        for (uint32_t row = 0; row < 4; ++row)
            aligned(to + row) = quadwordOf(from + row);
    }

    // A material's effect as its routines give it to VU1 (material_kinds.cpp writes them by kind, setTextureMatrix
    // keeps the matrix current), by what it does to the strips: for a renderer that draws them without VU1 (the
    // class draws' host form, zge/host/class_draws.cpp).
    struct MaterialEffect
    {
        // Grass shells (kinds 1 and 2): up to `shells` copies of the strips over the base, shell k moved k *
        // shellStep along the vertex normal and its alpha 0x80 - k * alphaStep (the shells' alpha test and blend
        // are the material's states). VU1 draws fewer of them further away (the host form's grassShellsAt).
        bool grass = false;
        int32_t shells = 0;
        int32_t alphaStep = 0;
        float shellStep = 0.0f;
        // The UV transforms, in order: a texture matrix (kind 9: uv' = s * row 0 + t * row 1 + row 2 + row 3, xy of
        // each row) or the sphere map (kind 10: UVs from the normal turned into view space).
        struct UvTransform
        {
            bool sphereMap = false;
            float rows[4][4] = {};
        };
        static constexpr uint32_t kMaxUvTransforms = 4;
        UvTransform uvTransforms[kMaxUvTransforms];
        uint32_t uvTransformCount = 0;
    };
    // The effect of the material's routines; false for a routine list this does not know (or that does not end).
    // The environment map's routine (kinds 6, 7) changes nothing in the strips.
    bool materialEffectOf(ee::Ptr<Material> material, MaterialEffect &out);

    // (ps2/material_packet.cpp)
    uint64_t setMaterialPacketSize(ee::Ptr<Material> material, uint32_t quadwords);
    ee::Ptr<Routine> beginMaterialPacket(ee::Ptr<PacketWriter> writer, ee::Ptr<Material> material);
    int32_t finishMaterialPacket(ee::Ptr<PacketWriter> writer);
    uint64_t addMaterialPacketMatrix(ee::Ptr<PacketWriter> writer, ee::Ptr<Vec4> matrix);
    ee::Ptr<Routine> addMaterialPacketValue(ee::Ptr<PacketWriter> writer, uint32_t word1, uint32_t word2, ee::Float value);
    ee::Ptr<Routine> addSphereMapRoutine(ee::Ptr<PacketWriter> writer);
    ee::Ptr<Routine> addEnvironmentMapRoutine(ee::Ptr<PacketWriter> writer);
    uint64_t initMaterialLayers(ee::Ptr<Material> material);
}
