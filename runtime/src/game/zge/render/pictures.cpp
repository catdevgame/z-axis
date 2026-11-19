// Pictures: animated texture levels.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/pool.h"
#include "zge/render/material.h"
#include "zge/render/texture.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/render/pictures.cpp.h"

namespace zge
{

    namespace
    {
        inline ee::Global<ee::Ptr<Pool>, sym::g_materialPool> g_materialPool;
    }

    // A material's pictures are its textures: pictureLevelCount of them,
    // pictureLevel(material, i) the i-th (none while the material has no
    // textures yet).
    uint32_t pictureLevelCount(ee::Ptr<Material> material);
    void refreshPictureLevels(ee::Ptr<Material> material, uint32_t hookArgument);

    // A pool callback: refreshPictureLevels(picture, 0)
    uint32_t refreshPictureCallback(uint32_t picture)
    {
        refreshPictureLevels(ee::Ptr<Material>(picture), 0);
        return 1;
    }
    READABLE_FUNCTION(refreshPictureCallback, Verified);

    void pictureNoop()
    {
    }
    READABLE_FUNCTION(pictureNoop, Draft);

    uint32_t pictureLevelCount(ee::Ptr<Material> material)
    {
        return material->textureCount;
    }
    READABLE_FUNCTION(pictureLevelCount, Verified);

    uint32_t pictureLevel(ee::Ptr<Material> material, uint32_t index)
    {
        if (material->textureCount == 0)
            return 0;
        return *textureSlotOf(material, index);
    }
    READABLE_FUNCTION(pictureLevel, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        // setMaterialTexture and queueMaterialTexture by address: they are
        // void, and setMaterialPicture returns what they leave in v0.
        int32_t setMaterialTextureKeepingResult(ee::Ptr<Material> material, uint32_t texture, uint32_t index, uint32_t markTableDirty)
        {
            return ee::game<int32_t>(sym::setMaterialTexture, material, texture, index, markTableDirty);
        }
        int32_t queueMaterialTextureKeepingResult(ee::Ptr<Material> material, uint32_t texture, uint32_t index)
        {
            return ee::game<int32_t>(sym::queueMaterialTexture, material, texture, index);
        }

        // findTextureByHash's search (three globals in a row): the texture
        // hash asked for, how many matches to pass over first, and the
        // material found.
        struct TextureSearch
        {
            uint32_t textureHash;
            int32_t skip;
            ee::Ptr<Material> found;
        };
    }

    // matchPictureByHash: a material-pool walk's visitor: a material in use
    // whose first texture has the hash asked for is a match; the search's
    // skip-th match is kept, which stops the walk.
    uint32_t matchPictureByHash(ee::Ptr<Material> material, ee::Ptr<TextureSearch> search)
    {
        const uint32_t texture = pictureLevel(material, 0);
        if (texture == 0 || material->references == 0)
            return 1;
        if (textureHash(ee::Ptr<Texture>(texture)) != search->textureHash)
            return 1;
        search->skip = search->skip - 1;
        if (search->skip != -1)
            return 1;
        search->found = material;
        return 0;
    }
    READABLE_FUNCTION(matchPictureByHash, Verified);

    namespace
    {
        constexpr uint32_t kTextureSearchHash = sym::g_textureSearchHash;
        inline ee::Global<int32_t, sym::g_textureSearchHash> g_textureSearchHash;
        inline ee::Global<int32_t, sym::g_textureSearchArgument> g_textureSearchArgument; // g_textureSearchArgument; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_textureSearchResult> g_textureSearchResult;
    }

    // findTextureByHash: the material whose first texture has the hash asked
    // for (the skip-th such, matchPictureByHash's walk), 0 for none. Was
    // game/aggressive's (fx/texture_setup.cpp); the blocks say it is the
    // engine's (layer2), DM2 and BMX XXX (PS2) have it word for word.
    uint32_t findTextureByHash(uint32_t nameHash, uint32_t textureSearchArgument)
    {
        *g_textureSearchHash = nameHash;
        *g_textureSearchArgument = textureSearchArgument;
        const ee::Ptr<Pool> materialPool = *g_materialPool;
        *g_textureSearchResult = 0;
        zge::poolForEach(materialPool, sym::matchPictureByHash, kTextureSearchHash);
        return *g_textureSearchResult;
    }
    READABLE_FUNCTION_WAITS(findTextureByHash, Verified);

    // RefreshPictureCallback for each record of the material pool (g_materialPool); reportEngineObjects.
    uint32_t refreshAllPictures()
    {
        return game::tail(zge::poolForEach, *g_materialPool, sym::refreshPictureCallback, 0);
    }
    READABLE_FUNCTION_WAITS(refreshAllPictures, Verified);

    // Shows another texture in a material's slot: an empty slot takes it at
    // once (its table marked dirty), a slot in use has the change queued
    // (applyQueuedMaterialTextures makes it). The same texture again does
    // nothing and returns it.
    int32_t setMaterialPicture(ee::Ptr<Material> material, ee::Address texture, uint32_t index)
    {
        const uint32_t shown = *textureSlotOf(material, index);
        if (shown == texture)
            return static_cast<int32_t>(shown);
        if (shown == 0)
            return setMaterialTextureKeepingResult(material, texture, index, 1);
        return queueMaterialTextureKeepingResult(material, texture, index);
    }
    READABLE_FUNCTION_WAITS(setMaterialPicture, Verified);

    // Each of the material's textures handed to the texture hook, an empty
    // stub in the shipped game (the original passes it the texture and
    // hookArgument), then the closing stub.
    void refreshPictureLevels(ee::Ptr<Material> material, uint32_t hookArgument)
    {
        const uint32_t count = pictureLevelCount(material);
        for (uint32_t i = 0; i < count; ++i)
            if (pictureLevel(material, i) != 0)
                textureHook();
        game::tail(textureNoop);
    }
    READABLE_FUNCTION(refreshPictureLevels, Verified);
}
