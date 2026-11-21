#include "txr.h"
#include "game_address.h"

#include "zge/assets/images.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/file.h"
#include "zge/memory/pool.h"
#include "zge/native/native_textures.h"
#include "zge/native/host.h"
#include "zge/assets/chunk_reader.h"
#include "zge/render/texture.h"

#include <cstring>
#include "sym/zge/assets/txr.cpp.h"

namespace zge
{
    namespace
    {
        void callTextureLoaded(uint32_t callback, ee::Ptr<Texture> texture, ee::Ptr<TextureChunkLocals> locals) { ee::game(callback, texture, locals); }
    }

    int32_t loadTextureChunk(ChunkReader *reader, void *context)
    {
        // Native textures: the Xbox file of this library is read on the host
        // (not under zx-fncheck, whose recordings do not hold the file).
        if (native::hostExtrasOn())
            if (const ZFile *file = reader->file)
                native::noteTextureFile(std::string(file->path, strnlen(file->path, sizeof(file->path))));

        ee::Local<TextureChunkLocals> locals;
        locals->outerContext = context;
        locals->isNew = 1;
        const ee::Ptr<Texture> texture = newLoadedTexture();
        if (!texture)
            return 0;
        locals->children.texture = texture;
        if (!enterChunk(reader))
            return 1;
        if (!parseChunks(reader, &locals->children))
        {
            // TXRH stopped the chunk: the texture was loaded already and
            // locals->children.texture is that one now. Drop the new one.
            releaseLoadedTexture(texture);
            locals->isNew = 0;
        }
        leaveChunk(reader);
        if (const uint32_t callback = *g_textureLoadedCallback)
            callTextureLoaded(callback, locals->children.texture, locals);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadTextureChunk, Verified);

    int32_t loadTextureHeaderChunk(ChunkReader *reader, TextureLoadContext *context)
    {
        ee::Local<TextureHeaderChunk> header;
        const ee::Ptr<Texture> texture = context->texture;
        readChunkData(reader, header);
        const ee::Ptr<Texture> loaded = findLoadedTexture(header->id, header->name);
        if (loaded)
        {
            context->texture = loaded;
            return 0;
        }
        nameTexture(texture, header->width, header->height, header->reserved08, header->id, header->reserved0A, header->name);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadTextureHeaderChunk, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_textureChunkHandlersReady> g_textureChunkHandlersReady;
        inline ee::Global<int32_t, sym::g_loadedTexturePool> g_loadedTexturePool; // g_loadedTexturePool ((anonymous)::g_loadedTexturePool)
    }

    namespace
    {
        namespace routed
        {
        }
        // The loaded textures' pool: 0x180 records a block, in the uncached
        // mirror (createPool takes the pool's memory flags above the count),
        // each released through releaseLoadedTexture when the pool goes.
        constexpr uint32_t kTexturesABlock = game::readable::perGame(0x180u, 0x180u, 0x100u); // (BMX's createPool call: 0x10100)
    }

    // registerTextureHeaderChunkHandlers: the image and texture header
    // systems started (0 if either fails), the loaded textures' pool made,
    // and the TXRC and TXRH chunks given their readers. 1 when done.
    uint32_t registerTextureHeaderChunkHandlers()
    {
        if (zge::initImages() == 0 || zge::initTextureHeaders() == 0)
            return 0;
        *g_loadedTexturePool = static_cast<int32_t>(
            createPool(sizeof(Texture), kTexturesABlock | static_cast<uint32_t>(kPoolUncached) << 16, sym::releaseLoadedTexture));
        addChunkHandler(chunkTag("TXRC"), sym::loadTextureChunk);
        addChunkHandler(chunkTag("TXRH"), sym::loadTextureHeaderChunk);
        *g_textureChunkHandlersReady = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(registerTextureHeaderChunkHandlers, Verified);

}
