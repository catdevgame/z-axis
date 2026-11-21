// A material's render state from its MATI (zge/assets' material chunk): each
// layer's words, field by field, as the states setMaterialState sets
// (ps2/material_state.cpp writes them into the layer's GS registers), then
// the material's kind. ZGE2's (DM2) is the own body loadMaterialLayerStates.

#include "material.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>
#include <iterator>
#include "sym/zge/render/material_state.cpp.h"

namespace zge
{
    namespace
    {
        // Where a state's value lies in a MATI layer's words.
        struct MatiField
        {
            uint32_t state;
            uint32_t word, shift, bits; // bits 0: always 0
        };
        constexpr MatiField kMatiFields[] = {
            {kStatePrimType, 0, 0, 3}, {kStateIip, 0, 3, 1},    {kStateFge, 0, 0, 0},
            {kStateAbe, 0, 5, 1},      {kStateWms, 0, 10, 2},   {kStateWmt, 0, 12, 2},
            {kStateMinU, 0, 14, 10},   {kStateMaxU, 1, 0, 10},  {kStateMinV, 1, 10, 10},
            {kStateMaxV, 1, 20, 10},   {kStateTfx, 2, 0, 2},    {kStateTcc, 1, 30, 1},
            {kStateLcm, 2, 2, 1},      {kStateMxl, 2, 3, 3},    {kStateMmag, 2, 6, 1},
            {kStateMmin, 2, 7, 3},     {kStateMtba, 2, 10, 1},  {kStateL, 2, 11, 2},
            {kStateK, 2, 13, 12},      {kStateAlphaA, 2, 25, 2}, {kStateAlphaB, 2, 27, 2},
            {kStateAlphaC, 2, 29, 2},  {kStateAlphaD, 3, 0, 2}, {kStateAlphaFix, 3, 2, 8},
            {kStateAte, 3, 10, 1},     {kStateAtst, 3, 11, 3},  {kStateAref, 3, 14, 8},
            {kStateAfail, 3, 22, 2},   {kStateDate, 3, 24, 1},  {kStateDatm, 3, 25, 1},
            {kStateZte, 3, 26, 1},     {kStateZtst, 3, 27, 2},
        };
    }

    // loadMaterialRest: every layer's states from its MATI words (the fog bit
    // left out: always off), then the kind from the MATI's flags. Returns what
    // setMaterialState does.
    int32_t loadMaterialRest(ee::Ptr<Material> material, ee::Ptr<const MatiChunk> mati)
    {
        for (uint32_t layer = 0;;)
        {
            const MatiLayer &words = mati->layers[layer];
            ++layer;
            for (const MatiField &field : kMatiFields)
            {
                const uint32_t value = field.bits == 0 ? 0 : words.renderState[field.word] >> field.shift & ((1u << field.bits) - 1);
                setMaterialState(material, layer - 1, field.state, value);
            }
            if constexpr (game::readable::kIsBmx) // (ZGE4's material has one layer: the MATI's first layer's states alone)
                break;
            if (layer < mati->layerCount)
                continue;
            break;
        }
        return game::tail(setMaterialState, material, 0, static_cast<uint32_t>(kStateKind), (mati->flags & 0x3FCu) >> 2);
    }

    // loadMaterialLayerStates: ZGE2's loadMaterialRest: every layer's two-sided state and state flag from the
    // MATI's own flags (bits 1 and 0), then its states from its MATI words as ZGE3's; no kind (ZGE3 added
    // kStateKind and the layers built for it) and nothing returned.
    void loadMaterialLayerStates(ee::Ptr<Material> material, ee::Ptr<const MatiChunk> mati)
    {
        for (uint32_t layer = 0;;)
        {
            const MatiLayer &words = mati->layers[layer];
            setMaterialState(material, layer, kStateTwoSided, (mati->flags >> 1) & 1u);
            setMaterialState(material, layer, kStateFlag, mati->flags & 1u);
            ++layer;
            for (const MatiField &field : kMatiFields)
            {
                const uint32_t value = field.bits == 0 ? 0 : words.renderState[field.word] >> field.shift & ((1u << field.bits) - 1);
                setMaterialState(material, layer - 1, field.state, value);
            }
            if (layer < mati->layerCount)
                continue;
            break;
        }
    }
    READABLE_FUNCTION_OWN(loadMaterialRest, Verified, dm2, loadMaterialLayerStates);
}
