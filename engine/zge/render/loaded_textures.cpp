// The records of loaded textures (newLoadedTexture, releaseLoadedTexture) and finding
// them by hash (matchTextureHash .. textureStubCheckC, the small ones left of the module
// of the sweep's blocks_00128100).

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "ee/field.h"
#include "zge/memory/pool.h"
#include "zge/render/texture.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/loaded_textures.cpp.h"


namespace zge
{
    namespace
    {
        constexpr uint32_t kMatchTextureHash = sym::matchTextureHash;
        constexpr uint32_t kTextureState = sym::g_textureChunkHandlersReady;    // the module's globals, kept in s0

        // The search forEach hands its callback.
        constexpr uint32_t kTextureSearch = sym::g_loadedTextureSearchHash;
        inline ee::Global<int32_t, kTextureSearch> g_textureSearchHash;
        inline ee::Global<uint32_t, sym::g_textureSearchFound> g_textureSearchFound;
        inline ee::Global<uint32_t, sym::g_loadedTexturePool> g_loadedTexturePool;
    }


    // newLoadedTexture: a new texture, one reference, zeroed.
    ee::Ptr<Texture> newLoadedTexture()
    {
        const ee::Ptr<Texture> texture(poolAlloc(ee::Ptr<Pool>(*g_loadedTexturePool)));
        memoryFill(texture, 0, sizeof(Texture));
        texture->refCount = 1;
        texture->type = kLoadedTextureType;
        onTextureCreated();
        return texture;
    }
    READABLE_FUNCTION_WAITS(newLoadedTexture, Verified);

    // releaseLoadedTexture: a reference dropped; the last one frees its GS side and
    // its record (ZGE3 and ZGE4 clear its type first). 1 when it went.
    int32_t releaseLoadedTexture(ee::Ptr<Texture> texture)
    {
        const uint32_t references = --texture->refCount;
        if (references == 0)
        {
            freeTextureGsSide(texture);
            if constexpr (!game::readable::kIsDm2)
                texture->type = 0;
            poolFree(ee::Ptr<Pool>(*g_loadedTexturePool), texture);
            return 1;
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(releaseLoadedTexture, Verified);


    namespace
    {
        // What a search by hash hands its callback.
        struct TextureSearch
        {
            int32_t hash;
            uint32_t found;
        };
    }

    // matchTextureHash: poolForEach's callback: 0 stops it at the texture of the hash.
    int32_t matchTextureHash(uint32_t texture, ee::Ptr<TextureSearch> search)
    {
        if (textureHash(ee::Ptr<Texture>(texture)) != search->hash)
            return 1;
        search->found = texture;
        return 0;
    }
    READABLE_FUNCTION(matchTextureHash, Verified);

    // findLoadedTextureByHash
    uint32_t findLoadedTextureByHash(uint32_t hash)
    {
        *g_textureSearchHash = static_cast<int32_t>(hash);
        const uint32_t pool = *g_loadedTexturePool;
        *g_textureSearchFound = 0;
        poolForEach(ee::Ptr<Pool>(pool), kMatchTextureHash, kTextureSearch);
        return *g_textureSearchFound;
    }
    READABLE_FUNCTION_WAITS(findLoadedTextureByHash, Verified);

    // textureStubCheckC
    int32_t textureStubCheckC()
    {
        return textureStubZeroC() != 0u;
    }
    READABLE_FUNCTION(textureStubCheckC, Verified);

    namespace
    {
        inline ee::Global<ee::Ptr<Pool>, sym::g_loadedTexturePool> g_loadedTexturePoolWord; // g_loadedTexturePoolWord ((anonymous)::g_loadedTexturePoolWord)
        inline ee::Global<int32_t, sym::g_textureLoadedCallback> g_textureLoadedCallbackWord; // g_textureLoadedCallbackWord (g_textureLoadedCallbackWord)
    }


    // A loaded texture pool callback: textureHook.
    uint32_t touchLoadedTexture(uint32_t /*param_1*/)
    {
        textureHook();
        return 1;
    }
    READABLE_FUNCTION(touchLoadedTexture, Verified);

    namespace
    {
        // newProceduralTexture's names: "Procedural:" and the count in hex.
        inline ee::Global<int32_t, sym::g_proceduralTextureCount> g_proceduralTextureCount;
        constexpr uint32_t kProceduralPrefix = sym::g_textProcedural;
        constexpr uint32_t kProceduralNameFormat = sym::g_textProceduralFormat; // "%s%x"
        constexpr uint32_t kTextureNameBytes = sizeof(Texture::name);

        namespace tail
        {
            uint32_t stringCopyN(ee::Ptr<char> dst, ee::Address src, uint32_t bytes)
            {
                return ee::tailCall<uint32_t>(sym::stringCopyN, dst, src, bytes);
            }
        }
    }


    // nameTexture: a texture's size, bits per texel, id and the header's
    // reserved word set, and its name copied (32 bytes, strncpy).
    uint32_t nameTexture(ee::Ptr<Texture> texture, uint32_t width, uint32_t height, uint32_t bitsPerTexel, uint32_t id,
                         uint32_t reserved, ee::Address name)
    {
        texture->width = static_cast<uint16_t>(width);
        texture->height = static_cast<uint16_t>(height);
        texture->bitsPerTexel = static_cast<uint8_t>(bitsPerTexel);
        texture->id = id;
        texture->reserved = static_cast<uint16_t>(reserved);
        return tail::stringCopyN(ee::field(texture, &Texture::name), name, kTextureNameBytes);
    }
    READABLE_FUNCTION(nameTexture, Verified);

    // findLoadedTexture: the loaded texture with this id (the newest first),
    // or 0. Its name is compared with `name` but the result is not used (bug
    // kept): any texture of the id matches.
    ee::Ptr<Texture> findLoadedTexture(uint32_t id, ee::Ptr<const char> name)
    {
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_loadedTexturePoolWord); link; link = nextPoolLink(link))
        {
            const ee::Ptr<Texture> texture = poolRecordAs<Texture>(link);
            if (texture->id == id)
            {
                stringCompareNoCase(ee::field(texture, &Texture::name), name);
                return texture;
            }
        }
        return ee::Ptr<Texture>();
    }
    READABLE_FUNCTION(findLoadedTexture, Verified);

    // newProceduralTexture: a texture made of a picture the game built (zgui,
    // the fonts, the effects' materials): named "Procedural:" and a count in
    // hex, its id that name's hash, the picture its first level.
    ee::Ptr<Texture> newProceduralTexture(ee::Ptr<TextureImage> image)
    {
        const ee::Ptr<Texture> texture = newLoadedTexture();
        ee::Local<char[kTextureNameBytes]> name;
        formatString(name, kProceduralNameFormat, kProceduralPrefix, (*g_proceduralTextureCount)++);
        const uint32_t id = nameHash(name);
        nameTexture(texture, image->width, image->height, image->bitsPerTexel, id, 0, name);
        setTextureImage(texture, image);
        return texture;
    }
    READABLE_FUNCTION_WAITS(newProceduralTexture, Verified);

    // textureBitsPerTexel: a texture's bits per texel (its first image's).
    uint32_t textureBitsPerTexel(ee::Ptr<Texture> texture)
    {
        return texture->bitsPerTexel;
    }
    READABLE_FUNCTION(textureBitsPerTexel, Draft);

    // textureName: a texture's name (in the mirror the texture is in: the
    // loaded textures' pool hands out uncached addresses).
    ee::Ptr<char> textureName(ee::Ptr<Texture> texture)
    {
        return ee::field(texture, &Texture::name);
    }
    READABLE_FUNCTION(textureName, Verified);

    // retainTexture: a reference more; the count after it (as the original:
    // the sum, before it is cut to 16 bits).
    int32_t retainTexture(ee::Ptr<Texture> texture)
    {
        const int32_t references = texture->refCount + 1;
        texture->refCount = static_cast<uint16_t>(references);
        return references;
    }
    READABLE_FUNCTION(retainTexture, Verified);

    // releaseTextureRef: a reference less, the texture kept even at 0; the
    // count after it, as retainTexture's.
    int32_t releaseTextureRef(ee::Ptr<Texture> texture)
    {
        const int32_t references = texture->refCount - 1;
        texture->refCount = static_cast<uint16_t>(references);
        return references;
    }
    READABLE_FUNCTION(releaseTextureRef, Draft);

    // loadedTextureAt: the loaded texture `index` from the newest (0 the
    // newest), or 0 past the oldest.
    ee::Ptr<Texture> loadedTextureAt(int32_t index)
    {
        ee::Ptr<ListNode> link = firstPoolLink(*g_loadedTexturePoolWord);
        if (!link)
            return ee::Ptr<Texture>();
        for (int32_t left = index - 1; left != -1; --left)
        {
            link = nextPoolLink(link);
            if (!link)
                return ee::Ptr<Texture>();
        }
        return poolRecordAs<Texture>(link);
    }
    READABLE_FUNCTION(loadedTextureAt, Draft);

    // setTextureFlagsByte: a texture's flags set (bit 0: it has alpha;
    // materials copy it).
    void setTextureFlagsByte(ee::Ptr<Texture> texture, uint32_t flags)
    {
        texture->flags = static_cast<uint8_t>(flags);
    }
    READABLE_FUNCTION(setTextureFlagsByte, Verified);

    // textureFlagsByte: a texture's flags.
    uint32_t textureFlagsByte(ee::Ptr<Texture> texture)
    {
        return texture->flags;
    }
    READABLE_FUNCTION(textureFlagsByte, Verified);

    uint32_t setTextureLoadedCallback(uint32_t setTextureLoadedCallbackResult)
    {
        const int32_t textureLoadedCallback = *g_textureLoadedCallbackWord;
        *g_textureLoadedCallbackWord = setTextureLoadedCallbackResult;
        return textureLoadedCallback;
    }
    READABLE_FUNCTION(setTextureLoadedCallback, Verified);

    uint32_t textureHash(ee::Ptr<Texture> texture)
    {
        return static_cast<int32_t>(texture->id);
    }
    READABLE_FUNCTION(textureHash, Verified);

    // TextureStubZeroA.
    int32_t textureStubCallA()
    {
        return zge::textureStubZeroA();
    }
    READABLE_FUNCTION(textureStubCallA, Draft);

    // TextureStubZeroB.
    int32_t textureStubCallB()
    {
        return zge::textureStubZeroB();
    }
    READABLE_FUNCTION(textureStubCallB, Draft);

    uint32_t forEachLoadedTexture(uint32_t visit, uint32_t data)
    {
        return game::tail(zge::poolForEach, *g_loadedTexturePoolWord, visit, data);
    }
    READABLE_FUNCTION_WAITS(forEachLoadedTexture, Draft);

    void textureHook()
    {
    }
    READABLE_FUNCTION(textureHook, Verified);

    // TouchLoadedTexture for each texture of g_loadedTexturePoolWord (poolForEach); reportEngineObjects.
    uint32_t touchLoadedTextures()
    {
        return game::tail(zge::poolForEach, *g_loadedTexturePoolWord, sym::touchLoadedTexture, 0);
    }
    READABLE_FUNCTION_WAITS(touchLoadedTextures, Verified);

    uint32_t setTextureImage(ee::Ptr<Texture> texture, ee::Ptr<TextureImage> textureImage)
    {
        if (texture->imagesSet == 0)
            texture->bitsPerTexel = static_cast<uint8_t>(textureImage->bitsPerTexel);
        const int32_t setGsTextureImageResult = zge::setGsTextureImage(texture, textureImage);
        int32_t imagesSet = 0;
        int32_t result = 0;
        if (setGsTextureImageResult == 0)
        {
            return 0;
        }
        imagesSet = texture->imagesSet;
        texture->imagesSet = static_cast<uint8_t>(imagesSet + 1);
        result = 1;
        if ((static_cast<int32_t>(textureImage->flags) & 0x1u) != 0)
        {
            const int32_t flags = static_cast<int32_t>(textureFlagsByte(texture));
            setTextureFlagsByte(texture, flags | 0x1u);
            return 1;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(setTextureImage, Verified);

    int32_t loadedTextureCount()
    {
        return zge::poolCount(*g_loadedTexturePoolWord);
    }
    READABLE_FUNCTION(loadedTextureCount, Draft);


    // A texture's size in texels.
    uint32_t levelWidth(ee::Ptr<Texture> texture)
    {
        return texture->width;
    }
    READABLE_FUNCTION(levelWidth, Verified);

    uint32_t levelHeight(ee::Ptr<Texture> texture)
    {
        return texture->height;
    }
    READABLE_FUNCTION(levelHeight, Verified);
}
