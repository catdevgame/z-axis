// A material filled from its MATI chunk (readable round gamehand4, handed
// over from the engine's sweep for one round).

#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/render/part.h"
#include "zge/render/material.h"
#include "zge/render/texture.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/assets/tag_table.h"

#include <cstddef>
#include "sym/zge/render/material_load.cpp.h"

namespace zge
{
    // (below)
    ee::Ptr<Material> addMaterialReference(ee::Ptr<Material> material);
    ee::Ptr<MaterialTable> setMaterialOwner(ee::Ptr<Material> material, ee::Ptr<MaterialTable> table);
}

namespace zge
{
    namespace
    {
        constexpr uint64_t kFlagBit39 = 1ull << 39;
    }

    // loadMaterial: `material` filled from `mati`: its name hash, colours
    // and the two words after them, flag bit 39 from MATI flags bit 10, its
    // buffer object (a tag table of one record, descriptor 0x4083 in the
    // frame) and that record from MATI +0x1C, the type's own part
    // (loadMaterialRest), then the first two layers' texture ids. Returns 0
    // (the loop's last test).
    //
    // By engine version: ZGE2 materials have no name hash and no leave-out
    // bit, take all four layers' texture ids and keep each layer's two MATI
    // words after its id; ZGE4 keeps no colours in the material, makes the
    // tag table an object of its own (only when the record has anything in
    // it) and takes one layer's texture id.
    //
    // (The original builds the descriptor and the flags word in argument
    // registers past the third and leaves them there through makeBuffer,
    // memcpy and loadMaterialRest. None of the three reads them: makeBuffer
    // and memcpy take three arguments, and loadMaterialRest sets every
    // register it reads before reading it.)
    int32_t loadMaterial(ee::Ptr<Material> material, ee::Ptr<const MatiChunk> mati)
    {
        // The descriptor's doubleword: the original sets its fields over the
        // frame's word and clears its upper half, so it is always this.
        constexpr uint64_t kTagTableDescriptor = 0x4083;
        ee::Local<uint64_t> descriptor;
        if constexpr (!game::readable::kIsDm2)
            *materialNameHashSlot(material) = matiNameHash(*mati);
        if constexpr (!game::readable::kIsBmx)
        {
            const ee::Ptr<uint8_t> colours = materialColours(material);
            for (uint32_t i = 0; i < kMaterialColourBytes; ++i)
                colours[i] = mati->colours[i / 4][i % 4];
            const ee::Ptr<ee::Float> unreadValues = materialUnreadValues(material);
            unreadValues[0] = mati->unreadValues[0];
            unreadValues[1] = mati->unreadValues[1];
        }
        if constexpr (!game::readable::kIsDm2)
            material->flags = (material->flags & ~kFlagBit39) | (static_cast<uint64_t>((mati->flags >> 10) & 1u) << 39);
        *descriptor = kTagTableDescriptor;
        if constexpr (game::readable::kIsBmx)
        {
            bool recordEmpty = true;
            for (uint32_t i = 0; i < sizeof(mati->tagRecord) && recordEmpty; ++i)
                recordEmpty = mati->tagRecord[i] == 0;
            if (!recordEmpty)
            {
                setMaterialTagTable(material, newBuffer(descriptor));
                const ee::Ptr<TagRecord> record = tagRecordAt(materialTagTable(material), 0);
                memoryCopy(record, mati->tagRecord, static_cast<uint32_t>(fieldOf(*descriptor, 4, 10))); // the descriptor's record size
            }
        }
        else
        {
            makeBuffer(materialTagTable(material), descriptor, 0);
            memoryCopy(materialTagRecord(material), mati->tagRecord, sizeof(mati->tagRecord));
        }
        loadMaterialRest(material, mati);
        uint32_t textureId = 0;
        for (uint32_t i = 0;;)
        {
            textureId = mati->layers[i].textureId;
            *textureSlotOf(material, i) = textureId;
            if (++i >= kMatiLoadedLayers)
                break;
        }
        if constexpr (game::readable::kIsDm2)
            for (uint32_t i = 0; i < kMatiLoadedLayers; ++i)
                keepMatiLayerWords(material, i, mati->layers[i]);
        // What each original leaves in the result register: ZGE4 the last texture id copied, the others their loop's last test.
        return game::readable::kIsBmx ? static_cast<int32_t>(textureId) : 0;
    }
    READABLE_FUNCTION(loadMaterial, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_materialLoadedCallback> g_materialLoadedCallbackWord; // g_materialLoadedCallbackWord (g_materialLoadedCallbackWord)
        constexpr uint32_t kPlayerHigh = ee::high(sym::g_materialPoolReady);
        inline ee::Global<int32_t, sym::g_queuedMaterialTextureCount> g_queuedMaterialTextureCount; // g_queuedMaterialTextureCount; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_materialTablesReady> g_materialTablesReady;
    }

    uint32_t setMaterialCallback(uint32_t previousOne)
    {
        const int32_t materialLoadedCallback = *g_materialLoadedCallbackWord;
        *g_materialLoadedCallbackWord = previousOne;
        return materialLoadedCallback;
    }
    READABLE_FUNCTION(setMaterialCallback, Verified);

    // G_queuedMaterialTextureCount cleared; releaseCollisionWorld.
    int32_t resetMaterialTableCount()
    {
        *g_queuedMaterialTextureCount = 0;
        return kPlayerHigh;
    }
    READABLE_FUNCTION(resetMaterialTableCount, Verified);

    // G_materialTablesReady = 1.
    uint32_t initMaterialTables()
    {
        *g_materialTablesReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initMaterialTables, Draft);

    // G_materialTablesReady cleared, 1.
    uint32_t shutdownMaterialTables()
    {
        if (*g_materialTablesReady == 0)
            return 1;
        *g_materialTablesReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutdownMaterialTables, Draft);

    // materialCount: how many materials the table holds.
    uint32_t materialCount(ee::Ptr<MaterialTable> table)
    {
        return table->count;
    }
    READABLE_FUNCTION(materialCount, Verified);

    // materialAt: the table's material `index` (0: an empty slot).
    ee::Ptr<Material> materialAt(ee::Ptr<MaterialTable> table, uint32_t index)
    {
        return ee::Ptr<ee::Ptr<Material>>(table->materials)[index];
    }
    READABLE_FUNCTION(materialAt, Verified);

    // A material's queries and its table's flags.

    // addMaterialReference: one user more. Returns the material.
    ee::Ptr<Material> addMaterialReference(ee::Ptr<Material> material)
    {
        material->references += 1;
        return material;
    }
    READABLE_FUNCTION(addMaterialReference, Verified);

    // materialNameHash: the hash the material is found by. ZGE2 materials
    // have no name of their own: the first texture's hash stands for one
    // (0 without a texture).
    uint32_t materialNameHash(ee::Ptr<Material> material)
    {
        if constexpr (game::readable::kIsDm2)
        {
            const uint32_t texture = pictureLevel(material, 0);
            return texture != 0 ? textureHash(ee::Ptr<Texture>(texture)) : 0u;
        }
        else
            return *materialNameHashSlot(material);
    }
    READABLE_FUNCTION(materialNameHash, Verified);

    // materialTags: the material's tag table.
    ee::Ptr<TagTableHeader> materialTags(ee::Ptr<Material> material)
    {
        return materialTagTable(material);
    }
    READABLE_FUNCTION(materialTags, Verified);

    // materialTextureSlot: the material's texture slot `index`.
    uint32_t materialTextureSlot(ee::Ptr<Material> material, uint32_t index)
    {
        return *textureSlotOf(material, index);
    }
    READABLE_FUNCTION(materialTextureSlot, Verified);

    // setMaterialOwner: the table the material is in. Returns the table.
    ee::Ptr<MaterialTable> setMaterialOwner(ee::Ptr<Material> material, ee::Ptr<MaterialTable> table)
    {
        material->owner = table;
        return table;
    }
    READABLE_FUNCTION(setMaterialOwner, Verified);

    // materialFlagBit36: the material's kMaterialFlagTwoSided bit (its
    // name [inferred], from setMaterialState's kStateTwoSided). Nothing
    // calls it.
    uint32_t materialFlagBit36(ee::Ptr<Material> material)
    {
        return (material->flags >> kMaterialFlagTwoSided) & 1u;
    }
    READABLE_FUNCTION(materialFlagBit36, Draft);

    // The material's flags refreshMaterialTableFlags reads.
    uint32_t materialHasAlpha(ee::Ptr<Material> material)
    {
        return (material->flags >> kMaterialFlagTextureAlpha) & 1u;
    }
    READABLE_FUNCTION(materialHasAlpha, Verified);

    uint32_t materialIsBlended(ee::Ptr<Material> material)
    {
        return (material->flags >> kMaterialFlagBlended) & 1u;
    }
    READABLE_FUNCTION(materialIsBlended, Verified);

    uint32_t materialFlagBit39(ee::Ptr<Material> material)
    {
        return (material->flags >> kMaterialFlagBit39) & 1u;
    }
    READABLE_FUNCTION(materialFlagBit39, Verified);

    // markMaterialTableDirty: the table's flags to be worked out again.
    // Returns its flags.
    uint32_t markMaterialTableDirty(ee::Ptr<MaterialTable> table)
    {
        table->flags |= kMaterialTableDirty;
        return table->flags;
    }
    READABLE_FUNCTION(markMaterialTableDirty, Verified);

    // markMaterialOwnerDirty: the material's table, if it is in one, marked
    // dirty.
    void markMaterialOwnerDirty(ee::Ptr<Material> material)
    {
        if (material->owner)
            game::tail(zge::markMaterialTableDirty, material->owner);
    }
    READABLE_FUNCTION(markMaterialOwnerDirty, Verified);

    // refreshedTableNoop (ZGE4 only): an empty function refreshMaterialTableFlags hands the table it refreshed
    // (1; no build has a body or a name for it).
    inline uint32_t refreshedTableNoop([[maybe_unused]] ee::Ptr<MaterialTable> table)
    {
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, refreshedTableNoop);

    // refreshMaterialTableFlags: a dirty table's flags worked out again
    // from its materials (those without kMaterialFlagBit39; ZGE2 has no
    // such bit and takes every material): whether one's texture has alpha
    // (kMaterialTableBlended), whether one blends (kMaterialTableSecondClass).
    void refreshMaterialTableFlags(ee::Ptr<MaterialTable> table)
    {
        if (!(table->flags & kMaterialTableDirty))
            return;
        table->flags &= ~kMaterialTableDirty;
        bool textureAlpha = false;
        bool blended = false;
        const ee::Ptr<ee::Ptr<Material>> materials(table->materials);
        for (int32_t i = table->count - 1; i >= 0; --i)
        {
            const ee::Ptr<Material> material = materials[i];
            if constexpr (!game::readable::kIsDm2)
                if (materialFlagBit39(material) != 0)
                    continue;
            if (materialHasAlpha(material))
                textureAlpha = true;
            if (materialIsBlended(material))
                blended = true;
        }
        table->flags &= ~(kMaterialTableBlended | kMaterialTableSecondClass);
        if (textureAlpha)
            table->flags |= kMaterialTableBlended;
        if (blended)
            table->flags |= kMaterialTableSecondClass;
        if constexpr (game::readable::kIsBmx)
            game::tail(refreshedTableNoop, table); // (ZGE4: its tail call)
    }
    READABLE_FUNCTION(refreshMaterialTableFlags, Verified);

    // materialTableIsBlended: whether the table's materials need the
    // blended draw: a texture with alpha or a blending material.
    uint32_t materialTableIsBlended(ee::Ptr<MaterialTable> table)
    {
        // (ZGE4 looks at the dirty flag itself before asking)
        if (!game::readable::kIsBmx || (table->flags & kMaterialTableDirty) != 0)
            refreshMaterialTableFlags(table);
        return ((table->flags & kMaterialTableSecondClass) ? 1u : 0u) | ((table->flags & kMaterialTableBlended) ? 1u : 0u);
    }
    READABLE_FUNCTION(materialTableIsBlended, Verified);

    namespace
    {
        // A part clone's copyFlags bit: the materials are copied too, not
        // shared (ZGE2's bit is the next one up).
        constexpr uint32_t kCopyMaterials = game::readable::perGame(0x1000u, 0x2000u, 0x1000u);

    }

    // newMaterialTable: a table of count empty slots (its size kept in its
    // flags' low half), one reference; 0 if there is no room.
    ee::Ptr<MaterialTable> newMaterialTable(uint32_t count)
    {
        const uint32_t bytes = (offsetof(MaterialTable, materials) + count * sizeof(ee::Ptr<Material>)) & kMaterialTableBytes;
        // (ZGE2 has no sized pools: the heap)
        const ee::Ptr<MaterialTable> table(game::readable::kIsDm2 ? heapAllocate(bytes) : allocateSized(bytes));
        if (!table)
            return table;
        memoryFill(table, 0, bytes);
        table->count = static_cast<uint16_t>(count);
        table->references = 1;
        if constexpr (kMaterialTableKeepsSize)
            table->flags = bytes;
        const ee::Ptr<ee::Ptr<Material>> materials(table->materials);
        for (int32_t i = static_cast<int32_t>(count) - 1; i >= 0; --i)
            materials[i] = ee::Ptr<Material>();
        zge::clearMaterialChains(table);
        return table;
    }
    READABLE_FUNCTION_WAITS(newMaterialTable, Verified);

    // copyMaterialTable: a new table of the same materials (copies of them
    // with kCopyMaterials), filled from the last. ZGE4 releases each copy
    // once it is in the table.
    ee::Ptr<MaterialTable> copyMaterialTable(ee::Ptr<MaterialTable> table, uint32_t copyFlags)
    {
        const ee::Ptr<MaterialTable> copy = newMaterialTable(table->count);
        const ee::Ptr<ee::Ptr<Material>> materials(table->materials);
        for (int32_t i = table->count - 1; i >= 0; --i)
        {
            ee::Ptr<Material> material = materials[i];
            if (copyFlags & kCopyMaterials)
                material = copyMaterial(material);
            replaceTableMaterial(copy, material, i);
            if constexpr (game::readable::kIsBmx) // (ZGE4 drops the copy's own reference: the new table holds it)
                if (copyFlags & kCopyMaterials)
                    releaseMaterial(material);
        }
        return copy;
    }
    READABLE_FUNCTION_WAITS(copyMaterialTable, Verified);

    // releaseMaterialTable: a reference less; the last drops its
    // materials' (from the last), frees its packet chains and the table.
    uint32_t releaseMaterialTable(ee::Ptr<MaterialTable> table)
    {
        table->references -= 1;
        if (table->references != 0)
            return 1;
        const ee::Ptr<ee::Ptr<Material>> materials(table->materials);
        for (int32_t i = table->count - 1; i >= 0; --i)
            dropMaterialReference(materials[i], 0);
        zge::freeMaterialChains(table);
        if constexpr (kMaterialTableKeepsSize)
            freeSized(table, table->flags & kMaterialTableBytes);
        else
            heapFree(table);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseMaterialTable, Verified);

    // retainMaterialTable: one reference more. Returns the new count as the
    // original's register has it, before the store cuts it to 16 bits.
    // Nothing calls it.
    uint32_t retainMaterialTable(ee::Ptr<MaterialTable> table)
    {
        const uint32_t references = table->references + 1u;
        table->references = static_cast<uint16_t>(references);
        return references;
    }
    READABLE_FUNCTION(retainMaterialTable, Draft);

    // replaceTableMaterial: slot `index` of the table given `material`. The
    // one there (if any) is released, to be freed after the frame, and
    // taken out of the table; the new one is put in the table with a user
    // more, and the table marked dirty.
    void replaceTableMaterial(ee::Ptr<MaterialTable> table, ee::Ptr<Material> material, uint32_t index)
    {
        const ee::Ptr<ee::Ptr<Material>> slot = ee::Ptr<ee::Ptr<Material>>(table->materials) + index;
        if (*slot)
        {
            releaseMaterial(*slot);
            zge::setMaterialOwner(*slot, ee::Ptr<MaterialTable>());
        }
        *slot = material;
        zge::setMaterialOwner(material, table);
        zge::addMaterialReference(material);
        game::tail(zge::markMaterialTableDirty, table);
    }
    READABLE_FUNCTION_WAITS(replaceTableMaterial, Verified);

    // findMaterialByHash: the table's material whose name has this hash,
    // looked for from the last; 0 if none. Empty slots are passed over.
    // Nothing calls it.
    ee::Ptr<Material> findMaterialByHash(ee::Ptr<MaterialTable> table, uint32_t nameHash)
    {
        for (int32_t index = static_cast<int32_t>(materialCount(table)) - 1; index >= 0; --index)
        {
            const ee::Ptr<Material> material = materialAt(table, index);
            if (material && zge::materialNameHash(material) == nameHash)
                return material;
        }
        return ee::Ptr<Material>();
    }
    READABLE_FUNCTION(findMaterialByHash, Draft);

    // applyQueuedMaterialTextures: the queued texture changes made, in the
    // order they came, and the queue emptied.
    void applyQueuedMaterialTextures()
    {
        const ee::Ptr<QueuedMaterialTexture> queue(sym::g_queuedMaterialTextures);
        const int32_t count = *g_queuedMaterialTextureCount;
        for (int32_t i = 0; i < count; ++i)
            setMaterialTexture(queue[i].material, queue[i].texture, queue[i].layer, 0);
        *g_queuedMaterialTextureCount = 0;
    }
    READABLE_FUNCTION_WAITS(applyQueuedMaterialTextures, Verified);
}
