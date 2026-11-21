#pragma once

// Materials of the ZGE3 engine: MATI chunks on
// disc (160 bytes, in a TGEO's TMTB), material objects in RAM (0x290 bytes,
// newMaterial, filled by loadMaterial) and the per-part material table.
//
// The material's GS block is what VU1 kicks before the material's strips:
// an A+D GIF tag and seven registers. The texture cache patches its TEX0
// and MIPTBP1 whenever the texture gets GS memory (zge/render/texture.h).
//
// Fields marked [inferred] are named from how the loaders fill them only.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table_header.h"
#include "zge/render/gs_registers.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // One layer of a MATI (28 bytes; runtime/ps2_zaxis_xbox.h converts them).
    struct MatiLayer
    {
        // +0x00 the layer's render state: the GS fields loadMaterialRest
        // hands to setMaterialState (PRIM's bits, the wrap and clamp
        // modes, TEX1, ALPHA, TEST), packed (material_state.cpp kMatiFields).
        uint32_t renderState[4];
        uint32_t textureId;      // +0x10 TXRH id
        uint32_t unknown14[2];   // +0x14
    };
    static_assert(sizeof(MatiLayer) == 28);

    constexpr int kMatiMaxLayers = 4;

#if ZX_GAME_DM2
    // ZGE2's MATI: a 16-bit flags word (bits 2-9 its type, as DM2's loadMaterialChunk packs them), the colours
    // right after it, no name hash, the layers from +0x2C. Only what DM2's loadMaterial and loadMaterialChunk
    // read is named; the record's size is not read by either (zge/assets/geo.h's AI size is not asserted here).
    struct MatiChunk
    {
        uint16_t flags;          // +0x00 bits 2-9: material type
        uint8_t colours[3][4];   // +0x02 RGBA x 3 [inferred: ambient, diffuse, specular]
        uint8_t _0E[2];          // +0x0E
        ee::Float unreadValues[2]; // +0x10 (as AI's below)
        uint8_t tagRecord[8];    // +0x18 the material's tag record, to material +0x1C
        uint8_t _20[8];          // +0x20
        uint32_t layerCount;     // +0x28 (loadMaterialRest's loop bound)
        MatiLayer layers[kMatiMaxLayers]; // +0x2C (loadMaterial reads all four)
    };
    static_assert(offsetof(MatiChunk, unreadValues) == 0x10 && offsetof(MatiChunk, tagRecord) == 0x18 &&
                  offsetof(MatiChunk, layerCount) == 0x28 && offsetof(MatiChunk, layers) == 0x2C);
#else
    // A material on disc ("MATI", 160 bytes).
    struct MatiChunk
    {
        uint32_t flags;          // +0x00 bits 2-9: material type; bit 10: [inferred] material flag bit 39
        uint32_t nameHash;       // +0x04 nameHash of the material's name
        uint8_t colours[3][4];   // +0x08 RGBA x 3 [inferred: ambient, diffuse, specular]
        // +0x14 two floats copied to the material (0.1 and 1.0 on most of the
        // PS2 disc's MATIs). No reader in AI, DM2 or BMX XXX on any
        // platform: named for that, not for a meaning.
        ee::Float unreadValues[2];
        uint8_t tagRecord[8];    // +0x1C the material's tag record (the surface sound first) to material +0x20
        uint8_t _24[8];    // +0x24
        uint32_t layerCount;     // +0x2C
        MatiLayer layers[kMatiMaxLayers]; // +0x30
    };
    static_assert(sizeof(MatiChunk) == 0xA0 && offsetof(MatiChunk, layers) == 0x30);
#endif
    // How many layers' texture ids loadMaterial takes from the MATI: ZGE2 four (one a layer record), ZGE3 two,
    // ZGE4 one (its material has one layer).
    constexpr uint32_t kMatiLoadedLayers = game::readable::perGame(2u, 4u, 1u);

    // A GS register written through A+D: data, then the register's address.
    struct GsAdRegister
    {
        uint64_t data;
        uint64_t gsRegister;
    };
    static_assert(sizeof(GsAdRegister) == 16);

    struct MaterialTable; // below
    struct Texture;       // zge/render/texture.h
    struct Vec4;          // zge/math/types.h
    struct Matrix;        // zge/math/types.h

    // Every engine object starts with its type: a material's (ZGE2 numbers its object types one lower: a
    // loaded texture is 4 there, 5 in ZGE3 and ZGE4).
    constexpr uint8_t kMaterialType = game::readable::perGame(4, 3, 4);

    // The record a material can name: what a new texture clears.
    struct MaterialTextureRecord
    {
        uint8_t header[0x10];
        int32_t textureCurrent; // +0x10 0: the material's texture changed
    };

    // A material in RAM: a header, then its layers (MaterialLayer below), each a DMA tag, VIF codes, the GS
    // registers and the effect routines VU1 is sent before the material's strips. Each engine version lays it out
    // its own way (the layouts are read in each game's newMaterial, loadMaterial, pictureLevel, setLayerTextureRegisters,
    // setMaterialPacketSize and setMaterialState): ZGE2 (DM2) a 0x50 header and four layers of 0xB0 (0x310); ZGE3 (AI)
    // a 0x50 header and two layers of 0x120 (0x290), the first laid out again in Material itself; ZGE4 (BMX XXX) a
    // 0x20 header and one layer of 0xF0 (0x110). Only the fields known so far.
#if ZX_GAME_DM2
    struct Material
    {
        union
        {
            uint64_t flags;          // +0x000 bit 33: its first texture has alpha (updateMaterialAlphaFlag); bit 34: layer 0 blends
            struct
            {
                uint8_t objectType;  // +0x000 3 (every engine object starts with its type)
                uint8_t _001;
                uint16_t references; // +0x002 its users, as AI's
            };
        };
        ee::Ptr<MaterialTable> owner; // +0x008 the table it is in (setMaterialOwner), 0: none
        TagTableHeader tags;         // +0x00C its tag table (a buffer object: makeBuffer, descriptor 0x4083)
        uint8_t tagRecord[8];        // +0x01C MATI +0x18: the one record of the tag table `tags` describes
        uint8_t colours[3][4];       // +0x024 0xFF until loaded
        ee::Float unreadValues[2];   // +0x030 MATI +0x10 (1.0 until loaded; nothing reads them)
        uint32_t textureCount;       // +0x038 the textures in use (pictureLevelCount); none while 0
        uint32_t stateChanged;       // +0x03C bit 0: setMaterialState clears it as ZGE3's clears its type's bit 0 (its one DM2 reader)
        uint32_t type;               // +0x040 bits 1-8 the kind, as AI's (ZGE2 has no kStateKind: no DM2 function read so far)
        uint32_t _044[3];            // +0x044
        // +0x050 the four layers (materialLayer); the texture ids live in them.
    };
    static_assert(sizeof(Material) == 0x50 && offsetof(Material, references) == 0x002 && offsetof(Material, owner) == 0x008 &&
                  offsetof(Material, tags) == 0x00C && offsetof(Material, tagRecord) == 0x01C && offsetof(Material, colours) == 0x024 &&
                  offsetof(Material, unreadValues) == 0x030 && offsetof(Material, textureCount) == 0x038 && offsetof(Material, stateChanged) == 0x03C && offsetof(Material, type) == 0x040);
#elif ZX_GAME_BMX
    struct Material
    {
        union
        {
            uint64_t flags;          // +0x000 bits 34, 35, 39 as AI's (updateMaterialAlphaFlag, refreshMaterialTableFlags, loadMaterial)
            struct
            {
                uint8_t objectType;  // +0x000 4
                uint8_t _001;
                uint16_t references; // +0x002 its users, as AI's
                uint16_t _004;
                uint16_t textureCount; // +0x006 the textures in use (pictureLevelCount, addMaterialTexture)
            };
        };
        ee::Ptr<MaterialTable> owner; // +0x008 the table it is in, 0: none (updateMaterialAlphaFlag's tail call)
        uint32_t nameHash;           // +0x00C (loadMaterial)
        ee::Ptr<TagTableHeader> tags; // +0x010 its tag table, its own object (newBuffer in loadMaterial; copied and released with the material), 0: none
        uint16_t type;               // +0x014 bits 1-8 the kind, as AI's (setMaterialState's kStateKind; hasTextureMatrix reads 16 bits)
        uint16_t qwordCount;         // +0x016 the packet's size in quadwords (setMaterialPacketSize), as AI's layer +0x110
        uint32_t listedFrame;        // +0x018 the primitive renderer's frame the material was last listed in (listPrimitiveMaterial)
        uint32_t textures[1];        // +0x01C the layer's texture id (addMaterialTexture indexes it by textureCount), then the layer
    };
    static_assert(sizeof(Material) == 0x20 && offsetof(Material, references) == 0x002 && offsetof(Material, textureCount) == 0x006 &&
                  offsetof(Material, owner) == 0x008 && offsetof(Material, nameHash) == 0x00C && offsetof(Material, tags) == 0x010 &&
                  offsetof(Material, type) == 0x014 && offsetof(Material, qwordCount) == 0x016 && offsetof(Material, listedFrame) == 0x018 &&
                  offsetof(Material, textures) == 0x01C);
#else
    struct Material
    {
        union
        {
            uint64_t flags;          // +0x000 bit 39: MATI flags bit 10 [inferred]
            struct
            {
                uint8_t objectType;  // +0x000 4 (every engine object starts with its type)
                uint8_t _001;
                // +0x002 its users: 1 when made (newMaterial), addMaterialReference
                // adds one, dropMaterialReference takes one and frees it at 0.
                uint16_t references;
            };
        };
        ee::Ptr<MaterialTable> owner; // +0x008 the table it is in (setMaterialOwner), 0: none
        uint32_t nameHash;           // +0x00C
        TagTableHeader tags;         // +0x010 its tag table (a buffer object: makeBuffer, descriptor 0x4083)
        uint8_t tagRecord[8];        // +0x020 MATI +0x1C: the one record of the tag table `tags` describes (the surface sound first)
        uint8_t colours[3][4];       // +0x028 0xFF until loaded
        ee::Float unreadValues[2];   // +0x034 MATI +0x14 (1.0 until loaded; nothing reads them)
        uint32_t textureCount;       // +0x03C the textures in use (pictureLevelCount); none while 0
        uint32_t type;               // +0x040 (MATI flags >> 2) & 0xFF: 0, 1 grass shells, 9 UV transform, 11 ...
        uint32_t listedFrame;        // +0x044 the primitive renderer's frame the material was last listed in (bindTexture)
        uint32_t textures[2];        // +0x048 the first two layers' texture ids, then the texture records
        uint8_t _050[0x10];    // +0x050
        uint64_t gsGifTag[2];        // +0x060 A+D, NLOOP 7
        GsAdRegister clamp;          // +0x070 CLAMP_1
        GsAdRegister tex1;           // +0x080 TEX1_1
        GsAdRegister alpha;          // +0x090 ALPHA_1
        GsAdRegister test;           // +0x0A0 TEST_1
        GsAdRegister miptbp1;        // +0x0B0 MIPTBP1_1 (patched by the texture cache)
        GsAdRegister texflush;       // +0x0C0 TEXFLUSH
        GsAdRegister tex0;           // +0x0D0 TEX0_1 (patched by the texture cache)
        uint32_t effectRoutines[32]; // +0x0E0 VU1 effect routines, 0x4D4 ends the list [inferred length]
        uint32_t qwordCount;         // +0x160
        uint8_t _164[0x12C];   // +0x164
    };
    static_assert(sizeof(Material) == 0x290 && offsetof(Material, references) == 0x002);
    static_assert(offsetof(Material, type) == 0x40 && offsetof(Material, textures) == 0x48 &&
                  offsetof(Material, miptbp1) == 0xB0 && offsetof(Material, tex0) == 0xD0 &&
                  offsetof(Material, effectRoutines) == 0xE0 && offsetof(Material, qwordCount) == 0x160);
#endif

    // Bits of Material::flags (the word at +0x000, read as 64 bits). ZGE2 has every bit from the state flag up one
    // lower (DM2's setMaterialState, updateMaterialAlphaFlag and refreshMaterialTableFlags); ZGE3 added the leave-out
    // bit 39 (loadMaterial, from the MATI's flag bit 10), which ZGE4 keeps.
    constexpr uint32_t kMaterialFlagState = game::readable::perGame(33u, 32u, 33u);        // kStateFlag's (ZGE2: the MATI's flag bit 0); nothing reads it
    constexpr uint32_t kMaterialFlagTextureAlpha = game::readable::perGame(34u, 33u, 34u); // its first texture's flag bit 0 (updateMaterialAlphaFlag)
    constexpr uint32_t kMaterialFlagBit39 = 39;    // MATI flags bit 10: the table's flags leave the material out (not in ZGE2)
    constexpr uint32_t kMaterialFlagBlended = game::readable::perGame(35u, 34u, 35u);      // layer 0 blends
    constexpr uint32_t kMaterialFlagTwoSided = game::readable::perGame(36u, 35u, 36u);     // [inferred] kStateTwoSided (ZGE2: the MATI's flag bit 1)
    constexpr uint32_t kMaterialFlagCullMode = game::readable::perGame(37u, 36u, 37u);     // [inferred] two bits, kStateCullMode's value

    // A material's layer in RAM: the DMA tag, VIF codes and GS registers VU1
    // is sent before the material's strips, then its effect routines.
#if ZX_GAME_DM2
    // ZGE2's layer (0xB0; four from Material +0x050): the DMA tag, VIF codes, GIF tag and the seven GS registers as
    // ZGE3's (DM2's initMaterialLayers and setLayerTextureRegisters write them at the same offsets), then four words
    // not read yet, the layer's texture id and the two MATI layer words after it. ZGE2 has no material routines
    // (no buildMaterialLayers); its quadword count is the word after the GS block.
    struct MaterialLayer
    {
        uint16_t dmaQwords;    // +0x00
        uint8_t _02;           // +0x02
        uint8_t dmaId;         // +0x03
        uint32_t dmaAddress;   // +0x04
        uint32_t stcycl;       // +0x08 VIF STCYCL
        uint32_t unpack;       // +0x0C VIF UNPACK
        uint64_t gifTag[2];    // +0x10
        GsAdRegister clamp;    // +0x20
        GsAdRegister tex1;     // +0x30
        GsAdRegister alpha;    // +0x40
        GsAdRegister test;     // +0x50
        GsAdRegister miptbp1;  // +0x60
        GsAdRegister texflush; // +0x70
        GsAdRegister tex0;     // +0x80
        uint32_t qwordCount;   // +0x90 the packet's quadwords after the GS block (initMaterialLayers: 8; buildMaterialChain reads layer 0's)
        uint32_t _94[3];       // +0x94 (initMaterialLayers clears the layer to its end)
        uint32_t texture;      // +0xA0 the texture id shown in this layer (textureSlotOf)
        uint32_t matiWords[2]; // +0xA4 MATI layer +0x14 and +0x18 (MatiLayer::unknown14), copied by loadMaterial
        uint32_t _0AC;
    };
    static_assert(sizeof(MaterialLayer) == 0xB0 && offsetof(MaterialLayer, gifTag) == 0x10 && offsetof(MaterialLayer, tex1) == 0x30 &&
                  offsetof(MaterialLayer, miptbp1) == 0x60 && offsetof(MaterialLayer, tex0) == 0x80 && offsetof(MaterialLayer, texture) == 0xA0);
    constexpr uint32_t kMaterialLayersStart = 0x50, kMaterialLayerCount = 4;
    // The bytes of a layer initMaterialLayers clears: ZGE2's up to its texture id (the id and MATI words stay).
    constexpr uint32_t kMaterialLayerClearedBytes = offsetof(MaterialLayer, texture);
#elif ZX_GAME_BMX
    // ZGE4's layer (0xF0; one from Material +0x020): as ZGE3's up to the routines, of which it has room for 24; its
    // quadword count is in the material's header (Material::qwordCount).
    struct MaterialLayer
    {
        uint16_t dmaQwords;    // +0x00 (setMaterialPacketSize)
        uint8_t _02;           // +0x02
        uint8_t dmaId;         // +0x03
        uint32_t dmaAddress;   // +0x04
        uint32_t stcycl;       // +0x08 VIF STCYCL
        uint32_t unpack;       // +0x0C VIF UNPACK
        uint64_t gifTag[2];    // +0x10 (setMaterialPacketSize writes the second's low nibble)
        GsAdRegister clamp;    // +0x20
        GsAdRegister tex1;     // +0x30
        GsAdRegister alpha;    // +0x40
        GsAdRegister test;     // +0x50
        GsAdRegister miptbp1;  // +0x60
        GsAdRegister texflush; // +0x70
        GsAdRegister tex0;     // +0x80
        uint32_t routines[24]; // +0x90 (beginMaterialPacket, setTextureMatrix)
    };
    static_assert(sizeof(MaterialLayer) == 0xF0 && offsetof(MaterialLayer, gifTag) == 0x10 && offsetof(MaterialLayer, routines) == 0x90);
    constexpr uint32_t kMaterialLayersStart = 0x20, kMaterialLayerCount = 1;
    constexpr uint32_t kMaterialLayerClearedBytes = sizeof(MaterialLayer);
#else
    // ZGE3's layer (0x120): two from Material +0x050; Material itself lays out the first.
    struct MaterialLayer
    {
        uint16_t dmaQwords;    // +0x00
        uint8_t _02;           // +0x02
        uint8_t dmaId;         // +0x03
        uint32_t dmaAddress;   // +0x04
        uint32_t stcycl;       // +0x08 VIF STCYCL
        uint32_t unpack;       // +0x0C VIF UNPACK
        uint64_t gifTag[2];    // +0x10
        GsAdRegister clamp;    // +0x20
        GsAdRegister tex1;     // +0x30
        GsAdRegister alpha;    // +0x40
        GsAdRegister test;     // +0x50
        GsAdRegister miptbp1;  // +0x60
        GsAdRegister texflush; // +0x70
        GsAdRegister tex0;     // +0x80
        uint32_t routines[32]; // +0x90 as Material::effectRoutines
        uint32_t qwordCount;   // +0x110
        uint8_t _114[0xC];     // +0x114
    };
    static_assert(sizeof(MaterialLayer) == 0x120 && offsetof(MaterialLayer, gifTag) == 0x10 &&
                  offsetof(MaterialLayer, tex1) == 0x30 && offsetof(MaterialLayer, miptbp1) == 0x60 &&
                  offsetof(MaterialLayer, tex0) == 0x80 && offsetof(MaterialLayer, qwordCount) == 0x110);
    constexpr uint32_t kMaterialLayersStart = 0x50, kMaterialLayerCount = 2;
    constexpr uint32_t kMaterialLayerClearedBytes = sizeof(MaterialLayer);
#endif
    // The primitive renderer's frame a material was last listed in for the texture uploads (bindTexture; ZGE4's
    // listPrimitiveMaterial): ZGE3's +0x044, ZGE4's +0x018; ZGE2's bindTexture stamps +0x040, the word named type
    // above (no DM2 function read so far reads it as the kind).
    inline uint32_t &materialListedFrame(Material &material)
    {
#if ZX_GAME_DM2
        return material.type;
#else
        return material.listedFrame;
#endif
    }
    static_assert(kMaterialLayersStart <= sizeof(Material)); // the header ends where the layers start (ZGE3 lays the first out again)
    // The whole material object: the header and its layers (newMaterial's fill, copyMaterial's copy, the pool's record).
    constexpr uint32_t kMaterialBytes = kMaterialLayersStart + kMaterialLayerCount * sizeof(MaterialLayer);
    static_assert(kMaterialBytes == game::readable::perGame(0x290u, 0x310u, 0x110u));

    inline ee::Ptr<MaterialLayer> materialLayerAt(ee::Ptr<Material> material, uint32_t layer)
    {
        return (material.cast<uint8_t>() + kMaterialLayersStart).cast<MaterialLayer>() + layer;
    }
    inline MaterialLayer &materialLayer(ee::Ptr<Material> material, uint32_t layer) { return *materialLayerAt(material, layer); }

    // A layer's effect routines (ZGE3 and ZGE4; ZGE2 has none) and the packet's quadword count after the GS block:
    // in ZGE2's and ZGE3's layer, in ZGE4's header.
#if ZX_GAME_DM2
    inline ee::Ptr<uint32_t> layerRoutines(ee::Ptr<Material>, uint32_t) { return ee::Ptr<uint32_t>(); }
    inline uint32_t layerQwordCount(ee::Ptr<Material> material, uint32_t layer) { return materialLayer(material, layer).qwordCount; }
    inline void setLayerQwordCount(ee::Ptr<Material> material, uint32_t layer, uint32_t quadwords) { materialLayer(material, layer).qwordCount = quadwords; }
#elif ZX_GAME_BMX
    inline ee::Ptr<uint32_t> layerRoutines(ee::Ptr<Material> material, uint32_t layer) { return ee::Ptr<uint32_t>(materialLayer(material, layer).routines); }
    inline uint32_t layerQwordCount(ee::Ptr<Material> material, uint32_t) { return material->qwordCount; }
    inline void setLayerQwordCount(ee::Ptr<Material> material, uint32_t, uint32_t quadwords) { material->qwordCount = static_cast<uint16_t>(quadwords); }
#else
    inline ee::Ptr<uint32_t> layerRoutines(ee::Ptr<Material> material, uint32_t layer) { return ee::Ptr<uint32_t>(materialLayer(material, layer).routines); }
    inline uint32_t layerQwordCount(ee::Ptr<Material> material, uint32_t layer) { return materialLayer(material, layer).qwordCount; }
    inline void setLayerQwordCount(ee::Ptr<Material> material, uint32_t layer, uint32_t quadwords) { materialLayer(material, layer).qwordCount = quadwords; }
#endif

    // The material's texture slot `layer`: the id of the texture shown in that layer. ZGE2 keeps it in the layer's
    // record, ZGE3 and ZGE4 in the header (Material::textures).
    inline ee::Ptr<uint32_t> textureSlotOf(ee::Ptr<Material> material, uint32_t layer)
    {
#if ZX_GAME_DM2
        return ee::Ptr<uint32_t>(&materialLayer(material, layer).texture);
#else
        return ee::Ptr<uint32_t>(material->textures) + layer;
#endif
    }

    // The material's tag table: inside the material in ZGE2 and ZGE3, an object of its own in ZGE4 (0: none), which
    // loadMaterial sets. The header fields one version has and another has not are reached through these, so one
    // body serves the three layouts: a version without the field gets a null pointer, on a path it never takes.
#if ZX_GAME_BMX
    inline ee::Ptr<TagTableHeader> materialTagTable(ee::Ptr<Material> material) { return material->tags; }
    inline void setMaterialTagTable(ee::Ptr<Material> material, ee::Ptr<TagTableHeader> table) { material->tags = table; }
    inline ee::Ptr<uint8_t> materialTagRecord(ee::Ptr<Material>) { return ee::Ptr<uint8_t>(); }  // (in the tag table object)
    inline ee::Ptr<uint8_t> materialColours(ee::Ptr<Material>) { return ee::Ptr<uint8_t>(); }    // (ZGE4 keeps no colours)
    inline ee::Ptr<ee::Float> materialUnreadValues(ee::Ptr<Material>) { return ee::Ptr<ee::Float>(); }
#else
    inline ee::Ptr<TagTableHeader> materialTagTable(ee::Ptr<Material> material) { return ee::Ptr<TagTableHeader>(&material->tags); }
    inline void setMaterialTagTable(ee::Ptr<Material>, ee::Ptr<TagTableHeader>) {} // (laid out inside the material)
    inline ee::Ptr<uint8_t> materialTagRecord(ee::Ptr<Material> material) { return ee::Ptr<uint8_t>(material->tagRecord); }
    inline ee::Ptr<uint8_t> materialColours(ee::Ptr<Material> material) { return ee::Ptr<uint8_t>(&material->colours[0][0]); }
    inline ee::Ptr<ee::Float> materialUnreadValues(ee::Ptr<Material> material) { return ee::Ptr<ee::Float>(material->unreadValues); }
#endif
    // The material's name hash and the MATI's: ZGE3 and ZGE4 only (ZGE2 materials have no name of their own).
#if ZX_GAME_DM2
    inline ee::Ptr<uint32_t> materialNameHashSlot(ee::Ptr<Material>) { return ee::Ptr<uint32_t>(); }
    inline uint32_t matiNameHash(const MatiChunk &) { return 0; }
#else
    inline ee::Ptr<uint32_t> materialNameHashSlot(ee::Ptr<Material> material) { return ee::Ptr<uint32_t>(&material->nameHash); }
    inline uint32_t matiNameHash(const MatiChunk &mati) { return mati.nameHash; }
#endif
    constexpr uint32_t kMaterialColourBytes = 3 * 4; // RGBA x 3

    // ZGE2 keeps the two MATI layer words after the layer's texture id (loadMaterial); the later engines drop them.
    inline void keepMatiLayerWords(ee::Ptr<Material> material, uint32_t layer, const MatiLayer &mati)
    {
#if ZX_GAME_DM2
        materialLayer(material, layer).matiWords[0] = mati.unknown14[0];
        materialLayer(material, layer).matiWords[1] = mati.unknown14[1];
#else
        (void)material, (void)layer, (void)mati;
#endif
    }

    // Material::type's doubleword, as the originals read and write it: the type and the word after it (ZGE2, ZGE3);
    // in ZGE4 the type is the upper half of the doubleword that starts with the tag table pointer.
    inline uint64_t &typeDoubleword(Material &material)
    {
        if constexpr (game::readable::kIsBmx)
            return *reinterpret_cast<uint64_t *>(&material.tags);
        else
            return *reinterpret_cast<uint64_t *>(&material.type);
    }

    // initMaterialLayers' mark that the material's state changed, and the doubleword it is in, as the original returns
    // it: the type's bit 0 (ZGE3, ZGE4); ZGE2 keeps it in a word of its own (Material::stateChanged, bit 0), read as
    // the doubleword from the texture count before it.
    inline uint64_t markMaterialStateChanged(Material &material)
    {
#if ZX_GAME_DM2
        uint64_t &word = *reinterpret_cast<uint64_t *>(&material.textureCount);
        word |= 1ull << 32;
        return word;
#elif ZX_GAME_BMX
        uint64_t &typeWord = typeDoubleword(material); // (ZGE4: the type is the doubleword's upper half)
        typeWord |= 1ull << 32;
        return typeWord;
#else
        uint64_t &typeWord = typeDoubleword(material);
        typeWord |= 1;
        return typeWord;
#endif
    }

    // A word's bit field.
    inline uint64_t withField(uint64_t word, uint32_t shift, uint32_t bits, uint64_t value)
    {
        const uint64_t mask = ((1ull << bits) - 1) << shift;
        return (word & ~mask) | ((value << shift) & mask);
    }
    inline uint64_t fieldOf(uint64_t word, uint32_t shift, uint32_t bits) { return word >> shift & ((1ull << bits) - 1); }
    // A GS register's field: gs::withField and gs::fieldOf (gs_registers.h).
    using gs::fieldOf;
    using gs::withField;

    // A layer's GIF tag: its PRIM, and the bits below PRIM that the VU1
    // program reads (the tag's bits 32-45).
    constexpr gs::Field kTagPrim{47, 11}, kTagProgram{32, 14};
    // Its second doubleword (REGS) lists A+D alone; the VU1 program reads
    // more of it: the cull bits kStateCullMode sets [inferred] and the low
    // four bits of the routines' quadwords (setMaterialPacketSize).
    constexpr gs::Field kTagCull{12, 4}, kTagRoutineQuadwords{32, 4};
    // The cull bits for kStateCullMode's values 2 and 3 (1: none); what VU1
    // does with them is not known.
    constexpr uint32_t kTagCullMode2 = 0xC, kTagCullMode3 = 0x8;
    enum : uint32_t // PRIM's bits above its type
    {
        kPrimGouraud = gs::kGouraud, // IIP
        kPrimTextured = gs::kTextured,
        kPrimFogged = gs::kFogged,
        kPrimBlended = gs::kBlended,
    };

    // PRIM's upper bits copied into the program bits, the low three kept
    // (returned). What every change of a layer's registers ends with.
    inline uint64_t copyPrimToProgramBits(MaterialLayer &layer)
    {
        const uint64_t tag = layer.gifTag[0];
        const uint64_t kept = fieldOf(tag, kTagProgram) & 7u;
        layer.gifTag[0] = withField(tag, kTagProgram, (fieldOf(tag, kTagPrim) & gs::kPrimAttributes) | kept);
        return kept;
    }

    // setMaterialState's states. The first group and the last are the
    // engine's own settings; from kStatePrimType on each is one GS field.
    // ZGE2 (DM2) has 53 states: no kStateRepeatU, kStateRepeatV or kStateKind (ZGE3 added them as 12-14, which moved
    // every later state up by three; its 12 is the empty state); the three it lacks stand past its count, so a
    // caller that names one sets nothing. ZGE4 (BMX XXX) numbers them as ZGE3. Read in the three setMaterialStates'
    // jump tables case by case.
    enum MaterialState : uint32_t
    {
        kStateBlending = 0,        // the same as kStateAbe
        kStateTextured = 1,        // TME
        kStateModulate = 2,        // TFX: modulate; 0: decal
        kStateDepthGreater = 3,    // ZTST: greater; 0: always
        kStateDepthWrite = 4,      // 0: an alpha test that always fails and writes the frame buffer only, blending on (ZGE2: nothing)
        kStateBlendMode = 5,       // kBlendNone ..
        kStateGouraud = 6,         // IIP
        kStateTwoSided = 7,        // [inferred] flag bit kMaterialFlagTwoSided, and kStateCullMode 1; 0: kStateCullMode 3
        kStateDestinationAlpha = 8, // DATE, DATM 1
        kStateUnused9 = 9,
        kStateFilter = 10,         // 0: nearest; 1: linear; 2: linear, mipmaps linear
        kStateCullMode = 11,       // [inferred] flag bits kMaterialFlagCullMode and the GIF tag's second word, bits 12-15 (VU1 reads them)
        kStateRepeatU = game::readable::perGame(12u, 53u, 12u), // WMS 0: clamped is 1 (not in ZGE2)
        kStateRepeatV = game::readable::perGame(13u, 54u, 13u), // WMT (not in ZGE2)
        kStateKind = game::readable::perGame(14u, 55u, 14u),    // Material::type's kind, and the layers built again for it (not in ZGE2)
        kStateUnused15 = game::readable::perGame(15u, 12u, 15u),
        kStateFlag = game::readable::perGame(16u, 13u, 16u),    // the flags' bit kMaterialFlagState
        kStatePrimType = game::readable::perGame(17u, 14u, 17u),
        kStateIip = kStatePrimType + 1,
        kStateTme = kStatePrimType + 2,
        kStateFge = kStatePrimType + 3,
        kStateAbe = kStatePrimType + 4,            // and layer 0's kMaterialFlagBlended, and the owner told
        kStateWms = kStatePrimType + 5,
        kStateWmt = kStatePrimType + 6,
        kStateMinU = kStatePrimType + 7,
        kStateMaxU = kStatePrimType + 8,
        kStateMinV = kStatePrimType + 9,
        kStateMaxV = kStatePrimType + 10,
        kStateTfx = kStatePrimType + 11,
        kStateTcc = kStatePrimType + 12,
        kStateLcm = kStatePrimType + 13,
        kStateMxl = kStatePrimType + 14,
        kStateMmag = kStatePrimType + 15,
        kStateMmin = kStatePrimType + 16,
        kStateMtba = kStatePrimType + 17,
        kStateL = kStatePrimType + 18,
        kStateK = kStatePrimType + 19,
        kStateAlphaA = kStatePrimType + 20,
        kStateAlphaB = kStatePrimType + 21,
        kStateAlphaC = kStatePrimType + 22,
        kStateAlphaD = kStatePrimType + 23,
        kStateAlphaFix = kStatePrimType + 24,
        kStateAte = kStatePrimType + 25,
        kStateAtst = kStatePrimType + 26,
        kStateAref = kStatePrimType + 27,
        kStateAfail = kStatePrimType + 28,
        kStateDate = kStatePrimType + 29,
        kStateDatm = kStatePrimType + 30,
        kStateZte = kStatePrimType + 31,
        kStateZtst = kStatePrimType + 32,
        kStateSourceColourGequal = kStatePrimType + 33, // the source colour as it is, depth test GEQUAL, blending on
        kStateSourceColourGequal2 = kStatePrimType + 34, // the same
        kStateSourceColourGreater = kStatePrimType + 35, // the same with depth test GREATER, blending off
        kStateCutout = kStatePrimType + 36,         // alpha test above 0x40, blended by alpha, TCC; 0: test off
        kStateAlphaReference = kStatePrimType + 37, // alpha test above the value; -1: test off
        kStateSourceColour = kStatePrimType + 38,   // the source colour as it is; 0: kStateBlendMode's 1
        kMaterialStateCount = game::readable::perGame(56u, 53u, 56u),
    };
    static_assert(kStateZtst == game::readable::perGame(49u, 46u, 49u) && kStateSourceColour == kMaterialStateCount - 1);

    // The word whose bit 0 setMaterialState clears (a layer has changed): ZGE3's and ZGE4's is Material::type, ZGE2's
    // the word after its texture count.
    inline auto &materialStateChangedWord(Material &material)
    {
#if ZX_GAME_DM2
        return material.stateChanged;
#else
        return material.type;
#endif
    }

    // buildMaterialLayers (material_kinds.cpp): the routines and states the material's kind needs.
    int32_t buildMaterialLayers(ee::Ptr<Material> material);
    // setMaterialState (ps2/material_state.cpp): a state of a layer set, in the layer's GS registers.
    uint64_t setMaterialState(ee::Ptr<Material> material, uint32_t layer, uint32_t state, uint32_t value);

    // Bits of MaterialTable::flags. ZGE2 has them at the bottom of the word; ZGE3 moved them above the table's size,
    // which it keeps in the low half (kMaterialTableBytes), and ZGE4 keeps that.
    enum : uint32_t
    {
        kMaterialTableBlended = game::readable::perGame(0x10000u, 0x1u, 0x10000u),     // a material's texture has alpha (refreshMaterialTableFlags)
        kMaterialTableSecondClass = game::readable::perGame(0x20000u, 0x2u, 0x20000u), // a material blends (kMaterialFlagBlended)
        kMaterialTableDirty = game::readable::perGame(0x40000u, 0x4u, 0x40000u),
    };
    constexpr uint32_t kMaterialTableBytes = 0xFFFF;
    // Whether the table's flags hold its size (ZGE3 and ZGE4: allocateSized and freeSized; ZGE2 frees without one).
    constexpr bool kMaterialTableKeepsSize = !game::readable::kIsDm2;

    // A part's materials (newMaterialTable(count); at node + 0x1B0 once placed). ZGE4's header is 0x2C bytes longer
    // (its materials from +0x40: newMaterialTable, materialAt and the rest), its texture epoch the header's last word;
    // what the other extra words hold is not read yet.
    struct MaterialTable
    {
        uint16_t references;              // +0x00 1 when made, releaseMaterialTable takes one
        uint16_t count;                   // +0x02 the TMTB's MATI count
        uint32_t flags;                   // +0x04 the low half its own size in bytes (kMaterialTableBytes), then kMaterialTable*
        uint32_t packetChains[2];         // +0x08
#if ZX_GAME_BMX
        uint32_t _10[0xB];                // BMX +0x10 (not read yet)
        uint32_t textureEpoch;            // BMX +0x3C (requestTextures, setLayerTextureRegisters, queueMaterialTexture)
#else
        uint32_t textureEpoch;            // +0x10 cache generation the GS blocks were patched in
#endif
        ee::Ptr<Material> materials[1];   // +0x14 (BMX +0x40) [count]
    };
    static_assert(offsetof(MaterialTable, count) == 2 &&
                  offsetof(MaterialTable, materials) == game::readable::perGame(0x14u, 0x14u, 0x40u) &&
                  offsetof(MaterialTable, textureEpoch) == game::readable::perGame(0x10u, 0x10u, 0x3Cu));

    // A texture change queueMaterialTexture keeps until the frame's
    // applyQueuedMaterialTextures (g_queuedMaterialTextures, at most
    // kMaxQueuedMaterialTextures of them).
    struct QueuedMaterialTexture
    {
        ee::Ptr<Material> material;
        uint32_t texture;
        uint32_t layer;
    };
    static_assert(sizeof(QueuedMaterialTexture) == 12);
    constexpr uint32_t kMaxQueuedMaterialTextures = 128;
}

namespace zge
{
    // (material_load.cpp)
    int32_t loadMaterial(ee::Ptr<Material> material, ee::Ptr<const MatiChunk> mati);
    uint32_t setMaterialCallback(uint32_t previousOne);
    int32_t resetMaterialTableCount();
    uint32_t materialCount(ee::Ptr<MaterialTable> table);
    ee::Ptr<Material> materialAt(ee::Ptr<MaterialTable> table, uint32_t index);
    uint32_t materialNameHash(ee::Ptr<Material> material);
    ee::Ptr<TagTableHeader> materialTags(ee::Ptr<Material> material);
    uint32_t materialTextureSlot(ee::Ptr<Material> material, uint32_t index);
    uint32_t markMaterialTableDirty(ee::Ptr<MaterialTable> table);
    void markMaterialOwnerDirty(ee::Ptr<Material> material);
    uint32_t materialTableIsBlended(ee::Ptr<MaterialTable> table);
    ee::Ptr<MaterialTable> newMaterialTable(uint32_t count);
    ee::Ptr<MaterialTable> copyMaterialTable(ee::Ptr<MaterialTable> table, uint32_t copyFlags);
    uint32_t releaseMaterialTable(ee::Ptr<MaterialTable> table);
    void replaceTableMaterial(ee::Ptr<MaterialTable> table, ee::Ptr<Material> material, uint32_t index);
    void applyQueuedMaterialTextures();
}

namespace zge
{
    // (material_kinds.cpp)
    uint32_t initMaterials();
    uint32_t shutdownMaterials();
    void setupMaterial(ee::Ptr<Material> material);
    void materialFreeHook();
    void materialCopyHook();
    int32_t hasTextureMatrix(ee::Ptr<Material> material);
    ee::Ptr<Vec4> setTextureMatrix(ee::Ptr<Material> material, ee::Ptr<Vec4> matrix);
    uint32_t textureStubZero();
    void textureNoop();
    // (ps2/material_packet.cpp) a layer's texture registers (TEX0, TEX1's levels, MIPTBP1, PRIM's TME).
    uint64_t setLayerTextureRegisters(ee::Ptr<Material> material, ee::Ptr<Texture> texture, uint32_t layer);
}

namespace zge
{
    // (material_state.cpp)
    int32_t loadMaterialRest(ee::Ptr<Material> material, ee::Ptr<const MatiChunk> mati);
}

namespace zge
{
    // (materials.cpp)
    uint32_t shutdownTextures();
    void setMaterialTexture(ee::Ptr<Material> material, uint32_t texture, uint32_t layer, uint32_t markTableDirty);
    void queueMaterialTexture(ee::Ptr<Material> material, uint32_t texture, uint32_t layer);
    uint32_t initTextures();
    uint32_t shutdownMaterialPool();
    ee::Ptr<Material> newMaterial();
    uint32_t freeMaterial(ee::Ptr<Material> material);
    int32_t releaseMaterial(ee::Address material);
    void addMaterialTexture(ee::Ptr<Material> material, ee::Address texture);
    ee::Ptr<Material> copyMaterial(ee::Ptr<Material> source);
    uint32_t dropMaterialReference(ee::Ptr<Material> material, uint32_t deferred);
}
