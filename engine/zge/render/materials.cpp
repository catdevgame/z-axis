// Materials: their pool and references.



#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/images.h"
#include "zge/assets/tag_table.h"
#include "zge/memory/pool.h"
#include "zge/render/material.h"
#include "zge/render/texture.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/render/materials.cpp.h"


namespace zge
{

    namespace
    {
        inline ee::Global<int32_t, sym::g_textureChunkHandlersReady> g_textureChunkHandlersReady;
        inline ee::Global<ee::Ptr<Pool>, sym::g_loadedTexturePool> g_loadedTexturePool; // g_loadedTexturePool ((anonymous)::g_loadedTexturePool)
        inline ee::Global<int32_t, sym::g_queuedMaterialTextureCount> g_queuedMaterialTextureCount; // g_queuedMaterialTextureCount; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_materialPoolReady> g_materialPoolReady;
        inline ee::Global<ee::Ptr<Pool>, sym::g_materialPool> g_materialPool;

        // The materials' state words initTextures clears: from g_materialPoolReady to the end of the texture queue
        // (ZGE3, ZGE4); ZGE2 has seven words there and no queue. Its pool's block: 2048 materials (ZGE2 1024, ZGE4 1800).
        constexpr uint32_t kMaterialStateBytes = game::readable::kIsDm2
            ? 0x1Cu
            : sym::g_queuedMaterialTextures + kMaxQueuedMaterialTextures * sizeof(QueuedMaterialTexture) - sym::g_materialPoolReady;
        constexpr uint32_t kMaterialPoolBlock = game::readable::perGame(0x800u, 0x400u, 0x708u);
    }

    namespace
    {
        namespace routed
        {
        }
    }

    uint32_t shutdownTextures()
    {
        int32_t result = 0;
        if (*g_textureChunkHandlersReady != 0)
        {
            const int32_t shutDownTextureHeaders = zge::shutDownTextureHeaders();
            result = 0;
            if (shutDownTextureHeaders == 0)
                return result;
            const int32_t shutdownImageReaders = zge::shutdownImageReaders();
            result = 0;
            if (shutdownImageReaders == 0)
                return result;
            destroyPool(*g_loadedTexturePool);
            *g_textureChunkHandlersReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownTextures, Verified);



    namespace tail
    {
        // The table's flags to be worked out again (the original's tail call).
    }

    // updateMaterialAlphaFlag: the material's first layer decides whether it
    // has alpha: its texture-alpha flag follows the texture's (no texture:
    // left as it is). With markTableDirty its table's flags are worked out
    // again. Other layers change nothing.
    void updateMaterialAlphaFlag(ee::Ptr<Material> material, uint32_t layer, uint32_t markTableDirty)
    {
        if (layer != 0)
            return;
        const uint32_t texture = *textureSlotOf(material, 0);
        if (texture == 0)
            return;
        constexpr uint64_t kTextureAlpha = 1ull << kMaterialFlagTextureAlpha;
        if (textureFlagsByte(ee::Ptr<Texture>(texture)) & 1)
            material->flags |= kTextureAlpha;
        else
            material->flags &= ~kTextureAlpha;
        if (markTableDirty && material->owner)
            game::tail(zge::markMaterialTableDirty, material->owner);
    }

    // updateMaterialAlphaFlagAndTable: ZGE2's updateMaterialAlphaFlag, two words: the first layer's texture-alpha
    // flag follows its texture's and the material's table is always told (ZGE3 added markTableDirty).
    void updateMaterialAlphaFlagAndTable(ee::Ptr<Material> material, uint32_t layer)
    {
        if (layer != 0)
            return;
        const uint32_t texture = *textureSlotOf(material, 0);
        if (texture == 0)
            return;
        constexpr uint64_t kTextureAlpha = 1ull << kMaterialFlagTextureAlpha;
        if (textureFlagsByte(ee::Ptr<Texture>(texture)) & 1)
            material->flags |= kTextureAlpha;
        else
            material->flags &= ~kTextureAlpha;
        if (material->owner)
            game::tail(zge::markMaterialTableDirty, material->owner);
    }
    READABLE_FUNCTION_OWN(updateMaterialAlphaFlag, Verified, dm2, updateMaterialAlphaFlagAndTable);


    namespace
    {
        void setLayerTexture(ee::Ptr<Material> material, uint32_t texture, uint32_t layer)
        {
            zge::setLayerTextureRegisters(material, ee::Ptr<Texture>(texture), layer);
        }
    }

    // setMaterialTexture: the texture shown in one of the material's layers
    // at once: the texture it had released, the new one put in its slot and
    // into the layer's GS registers; a texture (not 0) then decides the
    // material's alpha (updateMaterialAlphaFlag, its table marked with
    // markTableDirty) and gets a user more.
    void setMaterialTexture(ee::Ptr<Material> material, uint32_t texture, uint32_t layer, uint32_t markTableDirty)
    {
        uint32_t &slot = *textureSlotOf(material, layer);
        if (slot != 0)
            releaseLoadedTexture(ee::Ptr<Texture>(slot));
        slot = texture;
        setLayerTexture(material, texture, layer);
        if (texture == 0)
            return;
        if constexpr (game::readable::kIsDm2)
            updateMaterialAlphaFlagAndTable(material, layer); // ZGE2's two words
        else
            updateMaterialAlphaFlag(material, layer, markTableDirty);
        game::tail(retainTexture, ee::Ptr<Texture>(texture));
    }
    READABLE_FUNCTION_WAITS(setMaterialTexture, Verified);

    // markedTableNoop (ZGE4 only): an empty function queueMaterialTexture hands the material's table after
    // marking it (no build has a body or a name for it).
    inline void markedTableNoop([[maybe_unused]] ee::Ptr<MaterialTable> table)
    {
    }
    READABLE_FUNCTION_ONLY(bmx, markedTableNoop);

    // queueMaterialTexture: a texture change kept for the frame's
    // applyQueuedMaterialTextures (setMaterialTexture then, its table not
    // marked). The material's table has its GS blocks patched again
    // (textureEpoch 0) and is marked. With the queue full (128 changes) the
    // change is dropped.
    void queueMaterialTexture(ee::Ptr<Material> material, uint32_t texture, uint32_t layer)
    {
        if (material->owner)
        {
            material->owner->textureEpoch = 0;
            markMaterialTableDirty(material->owner);
            if constexpr (game::readable::kIsBmx)
                markedTableNoop(material->owner);
        }
        const int32_t count = *g_queuedMaterialTextureCount;
        if (count >= static_cast<int32_t>(kMaxQueuedMaterialTextures))
            return;
        *g_queuedMaterialTextureCount = count + 1;
        QueuedMaterialTexture &queued = ee::Ptr<QueuedMaterialTexture>(sym::g_queuedMaterialTextures)[count];
        queued.layer = layer;
        queued.material = material;
        queued.texture = texture;
    }
    READABLE_FUNCTION(queueMaterialTexture, Verified);


    // initTextures: the materials' side set up (named for the textures; it
    // makes the material pool): the materials' state cleared (from
    // g_materialPoolReady to the end of the texture queue), the pool of
    // materials made (2048 a block, each released by releaseMaterial when
    // the pool goes), MATI chunks handed to loadMaterialChunk, then the
    // platform's part (initMaterials). 1, or 0 if that fails.
    uint32_t initTextures()
    {
        memoryFill(sym::g_materialPoolReady, 0, kMaterialStateBytes);
        *g_materialPoolReady = 1;
        *g_materialPool = ee::Ptr<Pool>(createPool(kMaterialBytes, kMaterialPoolBlock, sym::releaseMaterial));
        addChunkHandler(chunkTag("MATI"), sym::loadMaterialChunk);
        return zge::initMaterials() != 0 ? 1 : 0;
    }
    READABLE_FUNCTION_WAITS(initTextures, Verified);

    uint32_t shutdownMaterialPool()
    {
        int32_t result = 0;
        if (*g_materialPoolReady != 0)
        {
            const int32_t shutdownMaterials = zge::shutdownMaterials();
            result = 0;
            if (shutdownMaterials == 0)
                return result;
            destroyPool(*g_materialPool);
            *g_materialPoolReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownMaterialPool, Verified);


    // A material's record: made with one user, filled by loadMaterial,
    // freed when its last user drops it.

    // newMaterial: a material from the pool: one user, its colours white
    // and its two [unknown] factors 1 until it is loaded; set up for the
    // platform (setupMaterial).
    ee::Ptr<Material> newMaterial()
    {
        const ee::Ptr<Material> material(poolAlloc(*g_materialPool));
        memoryFill(material, 0, kMaterialBytes);
        material->objectType = kMaterialType;
        material->references = 1;
        if constexpr (!game::readable::kIsBmx) // (ZGE4 keeps no colours in the material)
        {
            const ee::Ptr<uint8_t> colours = materialColours(material);
            for (uint32_t i = 0; i < kMaterialColourBytes; ++i)
                colours[i] = 0xFF;
            const ee::Ptr<ee::Float> unreadValues = materialUnreadValues(material);
            unreadValues[1] = 1.0f;
            unreadValues[0] = 1.0f;
        }
        setupMaterial(material);
        return material;
    }
    READABLE_FUNCTION_WAITS(newMaterial, Verified);

    // freeMaterial: the material's textures released, the platform's part
    // freed (an empty hook on the PS2) and the record given back.
    uint32_t freeMaterial(ee::Ptr<Material> material)
    {
        for (uint32_t i = 0; i < material->textureCount; ++i)
            if (const uint32_t texture = *textureSlotOf(material, i))
                releaseLoadedTexture(ee::Ptr<Texture>(texture));
        if constexpr (game::readable::kIsBmx) // (ZGE4's tag table is an object of its own)
            if (materialTagTable(material))
                releaseTagTable(materialTagTable(material));
        zge::materialFreeHook();
        poolFree(*g_materialPool, material);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeMaterial, Verified);


    // dropMaterialReference: one user less; the last frees the material,
    // after the frame when `deferred`. 1 if it went, else 0.
    uint32_t dropMaterialReference(ee::Ptr<Material> material, uint32_t deferred)
    {
        material->references -= 1;
        if (material->references != 0)
            return 0;
        if (deferred)
            deferRelease(material, 1);
        else
            freeMaterial(material);
        return 1;
    }
    READABLE_FUNCTION_WAITS(dropMaterialReference, Verified);

    // releaseMaterial: the material pool's destroy callback: a user less,
    // freed after the frame.
    int32_t releaseMaterial(ee::Address material)
    {
        return static_cast<int32_t>(dropMaterialReference(ee::Ptr<Material>(material), 1));
    }
    READABLE_FUNCTION_WAITS(releaseMaterial, Verified);

    // addMaterialTexture: the texture put in the material's next layer (a
    // user more for it, when not 0), into that layer's GS registers, and
    // that layer's alpha decided (updateMaterialAlphaFlag, its table marked).
    void addMaterialTexture(ee::Ptr<Material> material, ee::Address texture)
    {
        *textureSlotOf(material, material->textureCount) = texture;
        material->textureCount += 1;
        if (texture != 0)
            retainTexture(ee::Ptr<Texture>(texture));
        setLayerTexture(material, texture, material->textureCount - 1);
        if constexpr (game::readable::kIsDm2)
            return game::tail(zge::updateMaterialAlphaFlagAndTable, material, material->textureCount - 1); // ZGE2's two words
        else
            return game::tail(zge::updateMaterialAlphaFlag, material, material->textureCount - 1, 1u);
    }
    READABLE_FUNCTION(addMaterialTexture, Verified);


    namespace
    {
        // The platform's part of a copied material (an empty hook on the PS2).
        void copyForPlatform() { zge::materialCopyHook(); }
    }

    // copyMaterial: a new material with everything of `source` (its user
    // count and table too, as the original copies the whole record; ZGE4
    // gives the copy one user and a copy of the source's tag table), each of
    // its textures with a user more; 0 if there is no room.
    ee::Ptr<Material> copyMaterial(ee::Ptr<Material> source)
    {
        const ee::Ptr<Material> material = newMaterial();
        if (!material)
            return material;
        memoryCopy(material, source, kMaterialBytes);
        if constexpr (game::readable::kIsBmx)
            material->references = 1;
        for (uint32_t i = 0; i < source->textureCount; ++i)
            if (const uint32_t texture = pictureLevel(material, i))
                retainTexture(ee::Ptr<Texture>(texture));
        if constexpr (game::readable::kIsBmx)
            if (materialTagTable(source))
                setMaterialTagTable(material, copyTagTable(materialTagTable(source)));
        copyForPlatform();
        return material;
    }
    READABLE_FUNCTION_WAITS(copyMaterial, Draft);

}
