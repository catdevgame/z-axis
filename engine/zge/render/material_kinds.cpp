// A material's kind and what it needs (zge/render/material.h): the routines
// a kind's effect runs and the states it sets (the routines as VU1 takes
// them: ps2/material_packet.cpp), its texture matrix; the materials' start
// and end and the platform's material hooks. BMX XXX (PS2) has the same
// functions.

#include "material.h"
#include "zge/render/ps2/material_packet.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "ee/readable_waits.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/material_kinds.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kMaterialKindOffset = 1;  // Material::type's kind, above bit 0
        constexpr uint32_t kMaterialKindMask = 0xFF;
        enum : uint32_t // the kinds with routines
        {
            kKindGrassShells = 1,
            kKindGrassShells2 = 2,
            kKindEnvironmentMap = 6,
            kKindEnvironmentMap2 = 7,
            kKindUvTransform = 9,
            kKindSphereMap = 10,
            kKindTextureMatrix = kKindUvTransform,
        };
        inline ee::Global<int32_t, sym::g_materialsReady> g_materialsReady;
        inline ee::Global<uint64_t[2], sym::g_materialStackWords> g_materialStackWords;
    }

    // buildMaterialLayers: the routines of the material's kind, and what its kind
    // needs of its first layer: grass shells have a value routine and a fixed
    // blend (the only kinds that set states here). A kind with no routines
    // leaves the packet as it is. Returns what the last call left, the table's
    // address with no routines, its page past the table's last kind.
    int32_t buildMaterialLayers(ee::Ptr<Material> material)
    {
        ee::Local<PacketWriter> writer;
        ee::Local<Matrix> matrix;
        ee::Local<uint64_t> materialStackWordsSlot;
        ee::Local<uint64_t> materialStackWordsSlot2;
        const uint32_t kind = (material->type >> kMaterialKindOffset) & kMaterialKindMask;
        constexpr uint32_t kKindTable = sym::kindTable, kKindTablePage = ee::high(kKindTable), kKindsInTable = 12;
        if (kind >= kKindsInTable)
            return static_cast<int32_t>(kKindTablePage);
        switch (kind)
        {
        case kKindGrassShells:
        case kKindGrassShells2:
        {
            beginMaterialPacket(writer, material);
            addMaterialPacketValue(writer, 6, 15, ee::Float(0.02f));
            finishMaterialPacket(writer);
            static constexpr struct { MaterialState state; uint32_t value; } kShellStates[] = {
                {kStateTcc, 1},    {kStateTfx, 0},    {kStateAbe, 0},    {kStateMmag, 0},
                {kStateAlphaA, 0}, {kStateAlphaB, 1}, {kStateAlphaC, 0}, {kStateAlphaD, 1},
                {kStateAte, 0},    {kStateAtst, 6},   {kStateAref, 0x10}, {kStateAfail, 1},
            };
            int32_t result = 0;
            for (const auto &entry : kShellStates)
                result = static_cast<int32_t>(setMaterialState(material, 0, entry.state, entry.value));
            return result;
        }
        case kKindUvTransform:
            matrixIdentity(ee::Ptr<Matrix>(matrix));
            beginMaterialPacket(writer, material);
            addMaterialPacketMatrix(writer, &matrix->row(0));
            return finishMaterialPacket(writer);
        case kKindSphereMap:
            beginMaterialPacket(writer, material);
            addSphereMapRoutine(writer);
            return finishMaterialPacket(writer);
        case kKindEnvironmentMap:
        case kKindEnvironmentMap2:
            // A quadword copied to the stack that nothing reads.
            *materialStackWordsSlot = (*g_materialStackWords)[0];
            *materialStackWordsSlot2 = (*g_materialStackWords)[1];
            beginMaterialPacket(writer, material);
            addEnvironmentMapRoutine(writer);
            return finishMaterialPacket(writer);
        default:
            return static_cast<int32_t>(kKindTable);
        }
    }
    READABLE_FUNCTION(buildMaterialLayers, Verified);

    // initMaterials, shutdownMaterials: the materials' start and end. Always 1.
    uint32_t initMaterials()
    {
        *g_materialsReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initMaterials, Verified);

    uint32_t shutdownMaterials()
    {
        if (*g_materialsReady != 0)
            *g_materialsReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownMaterials, Verified);

    // setupMaterial: a new material's layers set up.
    void setupMaterial(ee::Ptr<Material> material)
    {
        initMaterialLayers(material);
    }
    READABLE_FUNCTION(setupMaterial, Verified);

    // materialFreeHook, materialCopyHook: what freeing and copying a material would do
    // here: nothing.
    void materialFreeHook() {}
    READABLE_FUNCTION(materialFreeHook, Verified);

    void materialCopyHook() {}
    READABLE_FUNCTION(materialCopyHook, Verified);

    // hasTextureMatrix: 1 if the material's kind has a texture matrix.
    int32_t hasTextureMatrix(ee::Ptr<Material> material)
    {
        return ((material->type >> kMaterialKindOffset) & kMaterialKindMask) == kKindTextureMatrix ? 1 : 0;
    }
    READABLE_FUNCTION(hasTextureMatrix, Verified);

    // setTextureMatrix: the material's texture matrix set, if it has one. Returns
    // where it goes.
    ee::Ptr<Vec4> setTextureMatrix(ee::Ptr<Material> material, ee::Ptr<Vec4> matrix)
    {
        const int32_t has = hasTextureMatrix(material);
        const ee::Ptr<Vec4> rows = textureMatrixRows(material);
        if (has != 0)
            copyRows(rows, matrix);
        return rows;
    }
    READABLE_FUNCTION(setTextureMatrix, Verified);

    uint32_t textureStubZero()
    {
        return 0;
    }
    READABLE_FUNCTION(textureStubZero, Draft);

    // Empty; refreshPictureLevels.
    void textureNoop()
    {
    }
    READABLE_FUNCTION(textureNoop, Verified);
}
