#pragma once

// TXR files: the ZGE3 engine's textures (host side:
// runtime/ps2_zaxis_xbox.h). A FORM of TXRC chunks, one texture
// each:
//
//   TXRC
//     TXRH          header: id, size, name (TextureHeaderChunk)
//     TBMP | TZMP   the picture: an 8- or 4-bit BMP (TZMP with alpha)
//     TBMP ...      optional smaller mip levels
//
// TXRC makes a texture object (zge/render/texture.h) and hands it to its
// children; TXRH names it, or, when a texture with that id and name is
// loaded already, swaps that one in and stops the TXRC.

#include "ee/ee.h"
#include "zge/assets/chunk_reader.h"
#include "zge/render/texture.h"
#include "sym/zge/assets/txr.h.h"

namespace zge
{
    // TXRH (52 bytes).
    struct TextureHeaderChunk
    {
        uint32_t id;         // +0x00
        uint16_t width;      // +0x04
        uint16_t height;     // +0x06
        uint16_t reserved08; // +0x08 0 in every PS2 file; Texture keeps it, nothing reads it
        uint16_t reserved0A; // +0x0A 0 in every PS2 file
        uint8_t zero0C[8];   // +0x0C
        char name[32];       // +0x14
    };
    static_assert(sizeof(TextureHeaderChunk) == 0x34 && offsetof(TextureHeaderChunk, name) == 0x14);

    // What a TXRC's children get as context: the texture being loaded.
    struct TextureLoadContext
    {
        ee::Ptr<Texture> texture;
    };

    // TXRC's locals (its stack frame), handed to the "texture loaded"
    // callback as well.
    struct TextureChunkLocals
    {
        ee::Address outerContext;    // +0x00 the TXR loader's context (a material asset's material)
        uint32_t isNew;              // +0x04 1; 0 when TXRH found the texture loaded already
        uint32_t _08[2];       // +0x08
        TextureLoadContext children; // +0x10
    };
    static_assert(offsetof(TextureChunkLocals, children) == 0x10);

    // The callback a loaded texture is handed to: (texture, &locals).
    inline ee::Global<uint32_t, sym::g_textureLoadedCallback> g_textureLoadedCallback;

    int32_t loadTextureChunk(ChunkReader *reader, void *context);                     // TXRC loadTextureChunk
    int32_t loadTextureHeaderChunk(ChunkReader *reader, TextureLoadContext *context); // TXRH loadTextureHeaderChunk
}

namespace zge
{
    // (txr.cpp)
    uint32_t registerTextureHeaderChunkHandlers();
}
