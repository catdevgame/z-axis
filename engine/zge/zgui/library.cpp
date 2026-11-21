// UI libraries: a library file (HLIB chunks) holds UI assets: materials,
// fonts, motions and scenes; zguiScript_Load's other kinds are an image or a
// scene on its own, made into a library of one asset. A load is asynchronous
// from the asset archive: what comes back goes through a pending record until
// the finish function (its callback) parses it.

#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/assets/images.h"
#include "zge/assets/txr.h"
#include "zge/container/list.h"
#include "zge/memory/heaps.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/render/material.h"
#include "zge/render/texture.h"
#include "zge/scene/animation.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"
#include "zge/text/strings.h"
#include "zge/assets/asset_library.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/zgui/library.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kHandlerNodeSize = 28; // a Handler: the Node up to its handler
        constexpr uint32_t kAllocateImageData = sym::allocateImageData;
        constexpr uint32_t kBuildMaterialState = sym::setMaterialState;     // the same, the whole register
        constexpr uint32_t kNewFontRecord = sym::newFontRecord;
        constexpr uint32_t kFreeFont = sym::freeFont;
        constexpr uint32_t kReadFontHeader = sym::readFontHeader;
        constexpr uint32_t kNewUiAssetEntry = sym::newUiAssetEntry;
        constexpr uint32_t kNewUiLibrary = sym::newUiLibrary;
        constexpr uint32_t kNewUiDefaultMaterial = sym::newUiDefaultMaterial;
        constexpr uint32_t kSetupUiMaterial = sym::setupUiMaterial;
        constexpr uint32_t kZguiScript_Load = sym::zguiScript_Load;
        constexpr uint32_t kLoadUiLibrary = sym::loadZguiLibrary;
        constexpr uint32_t kCancelUiLibrary = sym::findUiLibrary;        // (name) a library's load cancelled
        constexpr uint32_t kUnloadLibrary = sym::unloadUiLibrary;
        constexpr uint32_t kFreeUiAsset = sym::freeUiAsset;
        constexpr uint32_t kCommandPlay = sym::commandPlay;
        constexpr uint32_t kMemset = sym::memset; // libc
        constexpr uint32_t kFrameStateHigh = ee::high(sym::g_uiLibraries);
        constexpr uint32_t kZguiGlobals = sym::g_zgui;
        constexpr uint32_t kUiLibraries = sym::g_uiLibraries;            // a zge::List of UiLibrary
        constexpr uint32_t kPendingLoads = sym::g_pendingLoads;           // a zge::List of PendingLoad

        enum AssetKind : uint32_t
        {
            MaterialAsset = 0,
            FontAsset = 1,
            DataAsset = 2,
            MotionAsset = 3,
            SceneAsset = 4,
        };

        struct UiLibrary
        {
            UiAsset head;
            uint32_t references;    // +0xB8
            uint32_t unloadWhenFree; // +0xBC
        };
        static_assert(sizeof(UiLibrary) == 0xC0);

        // A file on its way in: what its finish function needs.
        struct PendingLoad
        {
            ee::Ptr<PendingLoad> next; // +0x00
            uint32_t _04;
            // +0x08 the library (ZGE4 keeps 0x40 characters; the fields after
            // it 0x20 further on there)
            char name[game::readable::perGame(0x20u, 0x20u, 0x40u)];
            uint32_t data;         // +0x28 the file's bytes
            uint32_t bytes;        // +0x2C
            uint32_t file;         // +0x30 a ZFile still open
            ee::Ptr<Node> owner;   // +0x34 the script's widget
            char onLoaded[0x100];  // +0x38 the widget to play
            uint32_t cancelled;    // +0x138
            char extension[4];     // +0x13C
        };
        static_assert(sizeof(PendingLoad) == game::readable::perGame(0x140u, 0x140u, 0x160u));
        static_assert(offsetof(PendingLoad, cancelled) == game::readable::perGame(0x138u, 0x138u, 0x158u));

        constexpr uint32_t kLibraryForm = 0x42494C55u; // "ULIB"
        constexpr uint32_t kSceneForm = 0x4C444D46u;   // "FMDL"
        constexpr uint32_t kListenerName = sym::g_textListener; // "Listener"
        constexpr uint32_t kZmpExtension = sym::g_textZmp, kBmpExtension = sym::g_textBmp, kTgaExtension = sym::g_textTga;

        inline ee::Global<uint32_t, sym::g_uiAssets> g_assetArchive;  // the UI's archive (assets\uilib.zal)
        inline ee::Global<uint32_t, sym::g_uiRoot> g_uiRootNode;
        inline ee::Global<ee::Ptr<UiLibrary>, sym::g_uiLibraries> g_uiLibraries;
        inline ee::Global<ee::Ptr<PendingLoad>, sym::g_pendingLoads> g_pendingLoads;

        ee::Ptr<zge::Heap> uiHeap() { return ee::Ptr<zge::Heap>(*g_heap); }

        // A record given back to zgui's memory (ZGE2's DMM region, ZGE3's heap).
        void giveBackUiRecord(ee::Address record)
        {
            if constexpr (game::readable::kIsDm2)
                zguiRelease(*g_heap, record);
            else
                zge::heapGive(uiHeap(), record);
        }

        // The next library in g_uiLibraries.
        ee::Ptr<UiLibrary> nextLibrary(ee::Ptr<UiLibrary> library) { return classCast<UiLibrary>(library->head.next); }

        // Whole: a library's name against a name given (either may be missing to the loops below).
        int32_t sameName(ee::Address address, ee::Address name)
        {
            return zge::stringCompareNoCase(address, name);
        }

        // A load's record off the list and back to the heap: the caller's
        void freePending(uint32_t pending, uint32_t data)
        {
            zge::heapFree(data);
            zge::listRemove(ee::Ptr<zge::List>(kPendingLoads), ee::Ptr<zge::ListNode>(pending));
            if constexpr (game::readable::kIsDm2)
                zguiRelease(*g_heap, pending);
            else
                game::tail(zge::heapGive, uiHeap(), pending);
        }

        // freePending as its caller's last step: ZGE2's library load ends in the release (its tail call), its image
        // load does not.
        void freePendingLast(uint32_t pending, uint32_t data)
        {
            zge::heapFree(data);
            zge::listRemove(ee::Ptr<zge::List>(kPendingLoads), ee::Ptr<zge::ListNode>(pending));
            if constexpr (game::readable::kIsDm2)
                zguiReleaseLast(*g_heap, pending);
            else
                game::tail(zge::heapGive, uiHeap(), pending);
        }
    }

    // registerZguiLibraryChunkHandlers.
    void registerZguiLibraryChunkHandlers()
    {
        constexpr uint32_t kHandlers[][2] = {
            {0x42494C48u, sym::readHlibChunk}, // "HLIB"
            {0x54534155u, sym::readUastChunk}, // "UAST"
            {0x54534148u, sym::readHastChunk}, // "HAST"
            {0x4C544D55u, sym::readUmtlChunk}, // "UMTL"
            {0x4C544D48u, sym::readHmtlChunk}, // "HMTL"
            {0x544E4648u, sym::readHfntChunk}, // "HFNT"
            {0x544E4655u, sym::readUfntChunk}, // "UFNT"
            {0x44414E55u, sym::readUnadChunk}, // "UNAD"
        };
        for (const auto &handler : kHandlers)
            zge::addChunkHandler(handler[0], handler[1]);
        game::tail(zge::addChunkHandler, 0x4C444D55u, sym::readSceneChunks); // "UMDL"
    }
    READABLE_FUNCTION_WAITS(registerZguiLibraryChunkHandlers, Verified);

    // newUiAssetEntry.
    uint32_t newUiAssetEntry(uint32_t library)
    {
        const uint32_t asset = zguiAllocate(*g_heap, sizeof(UiAsset));
        listPush(library + offsetof(UiAsset, list), asset);
        return asset;
    }
    READABLE_FUNCTION(newUiAssetEntry, Verified);

    // newUiLibrary.
    uint32_t newUiLibrary(ee::Address name)
    {
        const uint32_t library = zguiAllocate(*g_heap, sizeof(UiLibrary));
        const ee::Ptr<UiLibrary> record(library);
        initList(library + offsetof(UiAsset, list));
        record->head.header[0] = 0;
        ee::Ptr<UiAsset>(library)->kind = 0x00010001u;
        if (name)
            zge::stringCopy(library + offsetof(UiAsset, name), name);
        else
            record->head.name[0] = 0;
        record->references = 0;
        record->unloadWhenFree = 0;
        listPush(kUiLibraries, library);
        return library;
    }
    READABLE_FUNCTION(newUiLibrary, Verified);

    // readHlibChunk: "HLIB": a library and the chunks inside it.
    uint32_t readHlibChunk(uint32_t reader, ee::Ptr<uint32_t> result)
    {
        const uint32_t library = newUiLibrary(0);
        zge::readChunkData(ee::Ptr<zge::ChunkReader>(reader), &ee::Ptr<UiAsset>(library)->header);
        bool first = true;
        for (;;)
        {
            if (!first)
                zge::dispatchChunk(ee::Ptr<zge::ChunkReader>(reader), library);
            first = false;
            if (zge::nextChunk(ee::Ptr<zge::ChunkReader>(reader)) == -1)
                break;
        }
        if (result)
            *result = library;
        return 1;
    }
    READABLE_FUNCTION_WAITS(readHlibChunk, Verified);

    // readHastChunk: "HAST": an asset's head.
    uint32_t readHastChunk(uint32_t reader, uint32_t asset)
    {
        zge::readChunkData(ee::Ptr<zge::ChunkReader>(reader), &ee::Ptr<UiAsset>(asset)->header);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readHastChunk, Verified);

    // readUastChunk: "UAST": an asset.
    uint32_t readUastChunk(uint32_t reader, uint32_t library)
    {
        if (zge::enterChunk(ee::Ptr<zge::ChunkReader>(reader)))
        {
            const uint32_t asset = newUiAssetEntry(library);
            zge::parseChunks(ee::Ptr<zge::ChunkReader>(reader), asset);
            zge::leaveChunk(ee::Ptr<zge::ChunkReader>(reader));
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readUastChunk, Verified);

    // setSceneChunkResult: the motion hook of "UNAD": the asset is that motion.
    uint32_t setSceneChunkResult(uint32_t motion, uint32_t asset)
    {
        const ee::Ptr<UiAsset> record(asset);
        record->object = motion;
        record->kind = MotionAsset;
        return 1;
    }
    READABLE_FUNCTION(setSceneChunkResult, Verified);

    // readUnadChunk: "UNAD": a motion (MENU/ANIMS.ZIF, a leftover).
    uint32_t readUnadChunk(uint32_t reader, uint32_t asset)
    {
        if (zge::enterChunk(ee::Ptr<zge::ChunkReader>(reader)))
        {
            const uint32_t hook = zge::setMotionLoadedHook(sym::setSceneChunkResult, asset);
            zge::parseChunks(ee::Ptr<zge::ChunkReader>(reader), 0);
            zge::leaveChunk(ee::Ptr<zge::ChunkReader>(reader));
            zge::setMotionLoadedHook(hook, 0);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readUnadChunk, Verified);

    // addLoadedTextureToMaterial: the texture-loaded callback while a
    // material asset is read (readHmtlChunk): the texture onto the material
    // (the texture chunk's outer context); one this load made new is
    // released after (the material keeps it), one loaded already is not.
    uint32_t addLoadedTextureToMaterial(uint32_t texture, ee::Ptr<zge::TextureChunkLocals> chunk)
    {
        addMaterialTexture(ee::Ptr<Material>(chunk->outerContext), texture);
        if (chunk->isNew)
            releaseLoadedTexture(ee::Ptr<Texture>(texture));
        return 1;
    }
    READABLE_FUNCTION_WAITS(addLoadedTextureToMaterial, Verified);

    // newUiDefaultMaterial: the material for an asset with none: a white 8 by 8 texture.
    ee::Ptr<zge::Material> newUiDefaultMaterial()
    {
        ee::Local<uint32_t> colour;
        constexpr uint32_t kPaletteEntries = 16;
        const uint32_t image = zge::newImageRecord();
        const ee::Ptr<Material> material = newMaterial();
        const ee::Ptr<zge::Image> record(image);
        record->rowBytes = 4;
        record->height = 8;
        record->pixelBytes = 0x20;
        record->width = 8;
        const uint32_t pixels = zge::allocateImageData(image, 0x20);
        record->pixels = pixels;
        zge::memoryFill(pixels, 0, 0x20);
        record->bits = 4;
        record->paletteCount = kPaletteEntries;
        record->palette = zge::allocateImageData(image, 0x40);
        *colour = 0xFFFFFFFFu;
        for (uint32_t index = 0;;)
        {
            zge::setImagePaletteEntry(image, index, colour);
            if (++index >= kPaletteEntries)
                break;
        }
        const ee::Ptr<Texture> texture = newProceduralTexture(ee::Ptr<TextureImage>(image));
        addMaterialTexture(material, texture);
        releaseLoadedTexture(texture);
        zge::freeImageRecord(image);
        return material;
    }
    READABLE_FUNCTION_WAITS_OWN(newUiDefaultMaterial, Verified, dm2, newUiDefaultMaterialInPlace, bmx,
                                newUiDefaultMaterialInFrame);

    // newUiDefaultMaterialInFrame: ZGE4's newUiDefaultMaterial: the same 8 by
    // 8 image of palette entry 0 with sixteen white entries, its pixels and
    // palette in the frame (nothing allocated for them): the texture made
    // from them, the image record let go of them and freed, then the texture
    // given to the material.
    ee::Ptr<zge::Material> newUiDefaultMaterialInFrame()
    {
        ee::Local<uint8_t[4]> colour;
        ee::Local<uint8_t[0x20]> pixels;
        ee::Local<uint32_t[16]> palette;
        constexpr uint32_t kPaletteEntries = 16;
        const ee::Ptr<Material> material = newMaterial();
        const uint32_t image = zge::newImageRecord();
        const ee::Ptr<zge::Image> record(image);
        record->pixels = pixels.address();
        record->rowBytes = 4;
        record->height = 8;
        record->pixelBytes = 0x20;
        record->width = 8;
        zge::memoryFill(pixels.address(), 0, 0x20);
        record->bits = 4;
        record->paletteCount = kPaletteEntries;
        record->palette = palette.address();
        for (uint32_t channel = 0; channel < 4; ++channel)
            colour[channel] = 0xFF;
        for (uint32_t index = 0; index < kPaletteEntries; ++index)
            zge::setImagePaletteEntry(image, index, colour);
        const ee::Ptr<Texture> texture = newProceduralTexture(ee::Ptr<TextureImage>(image));
        record->pixels = 0;
        record->palette = 0;
        zge::freeImageRecord(image);
        addMaterialTexture(material, texture);
        releaseLoadedTexture(texture);
        return material;
    }

    // newUiDefaultMaterialInPlace: ZGE2's newUiDefaultMaterial: the same 8 by
    // 8 image of palette entry 0, its record in the frame (ZGE2 has no
    // newImageRecord: allocateImageData takes only the size) and its sixteen
    // palette entries written here: white, alpha 0x80.
    ee::Ptr<zge::Material> newUiDefaultMaterialInPlace()
    {
        ee::Local<zge::Image> image;
        constexpr uint32_t kPaletteEntries = 16;
        const ee::Ptr<Material> material = newMaterial();
        image->height = 8;
        image->pixelBytes = 0x20;
        image->width = 8;
        image->rowBytes = 4;
        const uint32_t pixels = zge::allocateImageDataFromMainRegion(0x20);
        image->pixels = pixels;
        zge::memoryFill(pixels, 0, 0x20);
        image->bits = 4;
        image->paletteCount = kPaletteEntries;
        image->palette = zge::allocateImageDataFromMainRegion(0x40);
        for (uint32_t index = 0; index < kPaletteEntries; ++index)
        {
            // (the palette is read again for each byte, as the original reads it)
            const auto entry = [&] { return ee::Ptr<uint8_t>(image->palette + index * 4); };
            entry()[0] = 0xFF;
            entry()[1] = 0xFF;
            entry()[2] = 0xFF;
            entry()[3] = 0x80;
        }
        const ee::Ptr<Texture> texture = newProceduralTexture(ee::Ptr<TextureImage>(image.address()));
        addMaterialTexture(material, texture);
        releaseLoadedTexture(texture);
        zge::freeImageRecord(image.address());
        return material;
    }

    // setupUiMaterial: a UI material's GS registers from the library's flags:
    // bits 0-3 the clamp (0 on, 1 off), 4-7 the blend (0 none, 1, 2, 3 three
    // modes), 8-9 the filter, 10 and 11 two switches.
    void setupUiMaterial(ee::Ptr<zge::Material> material, uint32_t flags)
    {
        const auto set = [&](uint32_t state, uint32_t value) {
            rebuildMaterialRegisters(material, 0, state, value);
        };
        const auto build = [&](uint32_t state, uint32_t value) {
            zge::setMaterialState(material, 0, state, value);
        };
        set(11, 1);
        set(3, 1);
        build(49, 2);
        const uint32_t clamp = flags & 0xFu;
        if (clamp == 0)
        {
            set(12, 1);
            set(13, 1);
        }
        else if (clamp == 1)
        {
            set(12, 0);
            set(13, 0);
        }
        const uint32_t filter = (flags >> 8) & 3u;
        if (filter == 1)
            set(10, 1);
        else if (filter == 0)
            set(10, 0);
        else
        {
            if (filter == 2)
                set(10, 2);
        }
        const uint32_t blend = (flags >> 4) & 0xFu;
        const auto blendOn = [&](uint32_t mode) {
            set(0, 1);
            set(5, mode);
            build(54, 0);
        };
        if (blend == 1)
            blendOn(4);
        else if (blend == 0)
            set(0, 0);
        else
        {
            if (blend == 2)
                blendOn(2);
            else if (blend == 3)
            {
                set(0, 1);
                set(5, 3);
                build(54, 0);
            }
        }
        set(8, (flags >> 11) & 1u);
        game::tail(rebuildMaterialRegisters, material, 0, 9, (flags >> 10) & 1u);
    }
    READABLE_FUNCTION_OWN(setupUiMaterial, Verified, dm2, setupUiMaterialStates);

    // setupUiMaterialStates: ZGE2's setupUiMaterial: its material states for
    // the same flags (filter in bits 8-9, blend in bits 4-7, bits 11 and 10
    // into states 8 and 9). ZGE2 sets state 4 off besides, has no wrap states
    // for the low four bits, builds state 46 where ZGE3 builds 49, and its
    // blend modes are the blend's own number (1, 2, 3) with nothing built
    // after.
    void setupUiMaterialStates(ee::Ptr<zge::Material> material, uint32_t flags)
    {
        const auto set = [&](uint32_t state, uint32_t value) {
            rebuildMaterialRegisters(material, 0, state, value);
        };
        set(11, 1);
        set(3, 1);
        set(4, 0);
        zge::setMaterialState(material, 0, 46, 2);
        const uint32_t filter = (flags >> 8) & 3u;
        if (filter <= 2)
            set(10, filter);
        const uint32_t blend = (flags >> 4) & 0xFu;
        if (blend == 0)
            set(0, 0);
        else if (blend <= 3)
        {
            set(0, 1);
            set(5, blend);
        }
        set(8, (flags >> 11) & 1u);
        game::tail(rebuildMaterialRegisters, material, 0, 9, (flags >> 10) & 1u);
    }

    namespace
    {
        // What nextChunk returns past the last chunk: the loops that dispatch
        // chunks keep it to compare against.
        constexpr uint32_t kNoMoreChunks = 0xFFFFFFFFu;

        // The word the loop that dispatched an HMTL chunk keeps in the place
        // readHmtlChunk's flags live (the kept bug there). A font's pages
        // (readHfntChunk) are dispatched with addLoadedTextureToMaterial
        // already the texture callback, and that loop keeps the chunk reader
        // there; a library's chunks (readHlibChunk) and a file's
        // (parseChunks) are dispatched with no texture callback, and those
        // loops keep their kNoMoreChunks. ZGE4's font page loop (BMX XXX)
        // keeps the font's own asset there instead, which the page's handler
        // is not handed: there the word is as that loop left it.
        uint32_t dispatchingLoopWord(uint32_t fontPageWord, uint32_t textureCallbackBefore)
        {
            return textureCallbackBefore == sym::addLoadedTextureToMaterial ? fontPageWord : kNoMoreChunks;
        }
    }

    // readHmtlChunk: "HMTL": a material asset: the flags, then its textures.
    // A kept bug: the flags are 12 bit-fields of a word the original never
    // sets, so the asset's flag word above them is what the dispatching loop
    // kept in that place: all ones for a library's or a file's material, the
    // chunk reader's address for a font page's (in BMX XXX the font's
    // asset). setupUiMaterial reads only the 12.
    uint32_t readHmtlChunk(uint32_t reader, uint32_t asset)
    {
        ee::Local<uint32_t> headerSlot;
        const ee::Ptr<UiAsset> record(asset);
        zge::readChunkData(ee::Ptr<zge::ChunkReader>(reader), headerSlot);
        const uint32_t header = *headerSlot;
        record->object = 0;
        const uint32_t callback = setTextureLoadedCallback(sym::addLoadedTextureToMaterial);
        if constexpr (game::readable::kIsBmx)
        {
            const ee::LeftByCaller<uint32_t> fontAsset; // the font's asset, as ZGE4's font page loop left it
            record->textureFlags = (dispatchingLoopWord(fontAsset, callback) & ~0xFFFu) | (header & 0xFFFu);
        }
        else
            record->textureFlags = (dispatchingLoopWord(reader, callback) & ~0xFFFu) | (header & 0xFFFu);
        const int32_t next = zge::nextChunk(ee::Ptr<zge::ChunkReader>(reader));
        if (next != -1)
        {
            if (record->object == 0)
                record->object = ee::Address(newMaterial());
            zge::dispatchChunk(ee::Ptr<zge::ChunkReader>(reader), record->object);
        }
        if (record->object == 0)
            record->object = ee::Address(newUiDefaultMaterial());
        setupUiMaterial(ee::Ptr<Material>(record->object), record->textureFlags);
        setTextureLoadedCallback(callback);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(readHmtlChunk, Verified, dm2, readHmtlChunks);

    // readHmtlChunks: ZGE2's readHmtlChunk: the same material asset, but every
    // chunk that follows is dispatched to it (ZGE3 dispatches one), and the
    // flag word is made before the texture callback is set. The kept bug is
    // the same: the bits above the 12 are a register the caller left.
    uint32_t readHmtlChunks(uint32_t reader, uint32_t asset)
    {
        const ee::LeftByCaller<int32_t> flagWord; // never initialised: as the caller left it
        ee::Local<uint32_t> headerSlot;
        const ee::Ptr<UiAsset> record(asset);
        zge::readChunkData(ee::Ptr<zge::ChunkReader>(reader), headerSlot);
        const uint32_t header = *headerSlot;
        record->object = 0;
        record->textureFlags = (static_cast<uint32_t>(flagWord) & ~0xFFFu) | (header & 0xFFFu);
        const uint32_t callback = setTextureLoadedCallback(sym::addLoadedTextureToMaterial);
        while (zge::nextChunk(ee::Ptr<zge::ChunkReader>(reader)) != -1)
        {
            if (record->object == 0)
                record->object = ee::Address(newMaterial());
            zge::dispatchChunk(ee::Ptr<zge::ChunkReader>(reader), record->object);
        }
        if (record->object == 0)
            record->object = ee::Address(newUiDefaultMaterial());
        setupUiMaterial(ee::Ptr<Material>(record->object), record->textureFlags);
        setTextureLoadedCallback(callback);
        return 1;
    }

    namespace
    {
        // "UMTL", "UFNT": a chunk that only holds others.
        uint32_t readInnerChunks(uint32_t reader, uint32_t context, uint32_t join)
        {
            if (zge::enterChunk(ee::Ptr<zge::ChunkReader>(reader)))
            {
                zge::parseChunks(ee::Ptr<zge::ChunkReader>(reader), context);
                zge::leaveChunk(ee::Ptr<zge::ChunkReader>(reader));
            }
            return 1;
        }
    }

    // readUmtlChunk.
    uint32_t readUmtlChunk(uint32_t reader, uint32_t context)
    {
        namespace here = points::readUmtlChunk;
        return readInnerChunks(reader, context, here::afterLeaveChunk);
    }
    READABLE_FUNCTION_WAITS(readUmtlChunk, Verified);

    namespace
    {
        uint32_t newFontRecord() { return ee::game<uint32_t>(kNewFontRecord); }
        void readFontHeader(uint32_t font, ee::Ptr<uint8_t> header) { ee::game(kReadFontHeader, font, header); }
    }

    // readHfntChunk: "HFNT": a font asset: its header, then a texture per
    // page. Each page's chunk is read as a material asset into a scratch
    // asset: its material becomes the page's texture, its flags the font
    // asset's (the last page's stay). ZGE4 reads the chunk's data into main
    // memory and makes the font from it (newFontFromData); ZGE3 makes an
    // empty font first and reads the data through the heap into it.
    uint32_t readHfntChunk(uint32_t reader, ee::Ptr<UiAsset> record)
    {
        ee::Local<UiAsset> page;
        if constexpr (game::readable::kIsBmx)
        {
            const uint32_t bytes = zge::chunkDataSize(ee::Ptr<zge::ChunkReader>(reader));
            const ee::Ptr<uint8_t> data(zge::allocateMemory(bytes));
            zge::readChunkData(ee::Ptr<zge::ChunkReader>(reader), data);
            record->object = ee::Address(zge::newFontFromData(data));
            zge::freeMemory(data);
        }
        else
        {
            record->object = newFontRecord();
            const uint32_t bytes = zge::chunkDataSize(ee::Ptr<zge::ChunkReader>(reader));
            const ee::Ptr<uint8_t> data(zge::heapAllocate(bytes));
            zge::readChunkData(ee::Ptr<zge::ChunkReader>(reader), data);
            readFontHeader(record->object, data);
            zge::heapFree(data);
        }
        const uint32_t callback = setTextureLoadedCallback(sym::addLoadedTextureToMaterial);
        for (uint32_t pageIndex = 0; zge::nextChunk(ee::Ptr<zge::ChunkReader>(reader)) != -1; ++pageIndex)
        {
            zge::dispatchChunk(ee::Ptr<zge::ChunkReader>(reader), page);
            const ee::Ptr<zge::Font> font(record->object);
            record->textureFlags = page->textureFlags;
            font->pages[pageIndex].texture = page->object;
        }
        setTextureLoadedCallback(callback);
        return 1;
    }
    READABLE_FUNCTION_WAITS(readHfntChunk, Verified);

    // readUfntChunk.
    uint32_t readUfntChunk(uint32_t reader, uint32_t context)
    {
        namespace here = points::readUfntChunk;
        return readInnerChunks(reader, context, here::afterLeaveChunk);
    }
    READABLE_FUNCTION_WAITS(readUfntChunk, Verified);

    // readSceneChunks: "UMDL": a scene asset. No shipped file has one.
    uint32_t readSceneChunks(uint32_t reader, uint32_t asset)
    {
        const ee::Ptr<UiAsset> record(asset);
        record->kind = SceneAsset;
        if (zge::enterChunk(ee::Ptr<zge::ChunkReader>(reader)))
        {
            zge::parseChunks(ee::Ptr<zge::ChunkReader>(reader), &record->object);
            zge::leaveChunk(ee::Ptr<zge::ChunkReader>(reader));
            changeSceneState(ee::Ptr<Scene>(record->object), 1, 0); // the asset's word
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(readSceneChunks, Verified);

    // loadUiLibraryByName: a library by name, counted. No shipped script reaches it.
    int32_t loadUiLibraryByName(uint32_t name, uint32_t file)
    {
        int32_t notLoaded = 1;
        ee::Ptr<UiLibrary> library = *g_uiLibraries;
        for (;;)
        {
            if (!library || !name)
                break;
            if (sameName(library->head.name, name) == 0)
            {
                notLoaded = 0;
                break;
            }
            library = nextLibrary(library);
        }
        int32_t result = loadZguiLibrary(name, file, 0);
        library = *g_uiLibraries;
        if (!library || !name)
            return result;
        for (;;)
        {
            result = sameName(library->head.name, name);
            if (result == 0)
            {
                if (notLoaded)
                    library->unloadWhenFree = 1;
                result = static_cast<int32_t>(++library->references);
                break;
            }
            library = nextLibrary(library);
            if (!library)
                return result;
            if (!name)
                break;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(loadUiLibraryByName, Verified);

    // unloadUiLibraryByName: its count back; the last one away unloads what it loaded.
    uint32_t unloadUiLibraryByName(uint32_t name)
    {
        ee::Ptr<UiLibrary> library = *g_uiLibraries;
        int32_t result = 0;
        if (library && name)
        {
            for (;;)
            {
                if (sameName(library->head.name, name) == 0)
                {
                    if (--library->references == 0)
                    {
                        result = 1;
                        if (library->unloadWhenFree != 1)
                            break;
                        return static_cast<int32_t>(unloadUiLibrary(name));
                    }
                    result = 1;
                    break;
                }
                library = nextLibrary(library);
                if (!library)
                    break;
                if (!name)
                    break;
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(unloadUiLibraryByName, Verified);

    namespace
    {
        // What a load's finish is called with: the pending load's address.
        using FinishLoad = ee::FunctionPtr<void(ee::Ptr<uint32_t>)>;

        // readAsset's callback: called with the pending load when the
        // background read is done.
        struct ReadDone
        {
            FinishLoad finish;
            uint32_t pending;
        };

        constexpr char kArchivePrefix[] = {'U', 'I', 'L', 'I', 'B', ':'};
    }

    // zguiScript_Load: LOADLIB and its kin: a library already loaded only runs
    // the onLoaded script. A "UILIB:" path comes from the archive, read in
    // the background; any other from a file. `finish` gets the pending record.
    void zguiScript_Load(ee::Ptr<const char> name, ee::Ptr<char> path, ee::Ptr<char> onLoaded, FinishLoad finish)
    {
        ee::Local<ReadDone> readDone;
        ee::Local<uint32_t> readArgument; // the pending load, for an archive read with no onLoaded
        ee::Local<uint32_t> fileBytes;
        if (!path)
            return;
        ee::Ptr<UiLibrary> library = *g_uiLibraries;
        if (library && name)
        {
            for (;;)
            {
                if (sameName(library->head.name, name) == 0)
                {
                    if (onLoaded && *onLoaded != 0)
                        // ZGE2 queues the play here (QUEUE); ZGE3 plays at once.
                        if constexpr (game::readable::kIsDm2)
                            commandQueue(onLoaded);
                        else
                            commandPlay(onLoaded);
                    return;
                }
                library = nextLibrary(library);
                if (!library || !name)
                    break;
            }
        }
        zge::stringToUpper(path);
        bool archive = true;
        for (uint32_t index = 0; archive && index < 6; ++index)
            archive = path[index] == kArchivePrefix[index];
        if (archive)
        {
            if (*g_assetArchive == 0)
                return;
            const ee::Ptr<const char> colon(zge::stringFind(path, ':'));
            const uint32_t id = zge::nameHash(colon + 1);
            if (zge::findAsset(*g_assetArchive, id) == 0)
                return;
            const uint32_t pending = zguiAllocate(*g_heap, sizeof(PendingLoad));
            const ee::Ptr<PendingLoad> record(pending);
            listPush(kPendingLoads, pending);
            zge::stringCopy(record->name, name);
            record->cancelled = 0;
            record->file = 0;
            const uint32_t bytes = zge::currentAssetLength(*g_assetArchive);
            record->extension[0] = 0;
            record->bytes = bytes;
            zge::pathExtension(path, record->extension);
            // The buffer's size rounded up to the asset's alignment.
            const uint64_t alignment = zge::currentAssetAlignment(*g_assetArchive);
            const uint32_t size = record->bytes;
            const uint32_t align = static_cast<uint32_t>(alignment);
            record->data = zge::allocateAligned(alignment, static_cast<int32_t>((size - 1 + align) / align * align));
            record->owner = currentScriptOwner();
            if (onLoaded && *onLoaded != 0)
            {
                readDone->finish = finish;
                readDone->pending = pending;
                zge::stringCopy(record->onLoaded, onLoaded);
                zge::readAsset(*g_assetArchive, record->data, readDone);
                return;
            }
            *readArgument = pending;
            record->onLoaded[0] = 0;
            zge::readAsset(*g_assetArchive, record->data, 0);
            finish(readArgument);
            return;
        }
        const uint32_t data = zge::loadFile(path, fileBytes);
        if (data)
        {
            ee::Local<uint32_t> finishArgument; // the pending load (where the archive's callback was)
            const uint32_t pending = zguiAllocate(*g_heap, sizeof(PendingLoad));
            const ee::Ptr<PendingLoad> record(pending);
            listPush(kPendingLoads, pending);
            if (name)
                zge::stringCopy(record->name, name);
            else
                record->name[0] = 0;
            record->cancelled = 0;
            record->file = 0;
            record->bytes = *fileBytes;
            const uint32_t copy = zge::heapAllocate(*fileBytes);
            record->data = copy;
            zge::memoryCopy(copy, data, *fileBytes);
            zge::freeLoadedFile(data);
            record->owner = currentScriptOwner();
            record->onLoaded[0] = 0;
            *finishArgument = pending;
            finish(finishArgument);
        }
    }
    READABLE_FUNCTION_WAITS(zguiScript_Load, Verified);

    // finishUiLibraryLoad: a library file in: parsed, its onLoaded widget played.
    void finishUiLibraryLoad(ee::Ptr<uint32_t> argument)
    {
        ee::Local<int32_t> context;
        const uint32_t pending = *argument; // the load
        const ee::Ptr<PendingLoad> record(pending);
        *context = 0;
        if (record->file)
            zge::closeFile(ee::Ptr<zge::ZFile>(record->file), 0);
        if (record->cancelled == 0)
        {
            const ee::Ptr<zge::ZFile> file = zge::openMemoryFile(record->data, record->bytes);
            const ee::Ptr<zge::ChunkReader> reader = zge::openChunkFile(file, kLibraryForm, 0);
            if (reader)
            {
                zge::parseChunks(reader, context);
                const uint32_t library = static_cast<uint32_t>(*context);
                if (record->name[0])
                    zge::stringCopy(ee::Ptr<UiAsset>(library)->name, record->name);
                zge::closeChunkFile(reader);
            }
            zge::closeFile(file, 0);
            if (record->onLoaded[0])
            {
                // ZGE2 plays it as PLAY would, for the load's widget.
                if constexpr (game::readable::kIsDm2)
                {
                    setScriptOwner(record->owner);
                    commandPlay(record->onLoaded);
                }
                else
                {
                    const ee::Ptr<Node> animation = findNode(record->owner, record->onLoaded, static_cast<uint64_t>(NodeKind::Animation));
                    if (animation)
                        playWidget(classCast<Animation>(animation), record->owner);
                }
            }
        }
        zge::heapFree(record->data);
        zge::listRemove(ee::Ptr<zge::List>(kPendingLoads), ee::Ptr<zge::ListNode>(pending));
        giveBackUiRecord(pending);
    }
    READABLE_FUNCTION_WAITS(finishUiLibraryLoad, Verified);

    namespace
    {
        namespace tail
        {
        // zguiScript_Load is void: the loaders return what it leaves in v0.
        int32_t zguiScriptLoadKeepingResult(ee::Address name, ee::Address path, ee::Address onLoaded, uint32_t finish) { return ee::tailCall<int32_t>(kZguiScript_Load, name, path, onLoaded, finish); }
        }
    }

    // loadZguiLibrary.
    int32_t loadZguiLibrary(ee::Address name, ee::Address path, ee::Address onLoaded)
    {
        return tail::zguiScriptLoadKeepingResult(name, path, onLoaded, sym::finishUiLibraryLoad);
    }
    READABLE_FUNCTION_WAITS(loadZguiLibrary, Verified);

    // finishUiAssetLibraryLoad: a scene file in: a library of one asset. Only DM2's UI
    // scripts load one.
    void finishUiAssetLibraryLoad(ee::Ptr<uint32_t> argument)
    {
        const uint32_t pending = *argument; // the load
        const ee::Ptr<PendingLoad> record(pending);
        if (record->file)
            zge::closeFile(ee::Ptr<zge::ZFile>(record->file), 0);
                if (record->cancelled == 0)
        {
            const uint32_t library = newUiLibrary(record->name);
            const uint32_t asset = newUiAssetEntry(library);
            const ee::Ptr<UiAsset> assetRecord(asset);
            assetRecord->kind = SceneAsset;
            zge::stringCopy(assetRecord->name, record->name);
            const ee::Ptr<zge::ZFile> file = zge::openMemoryFile(record->data, record->bytes);
            const ee::Ptr<zge::ChunkReader> reader = zge::openChunkFile(file, kSceneForm, 0);
            if (reader)
            {
                zge::parseChunks(reader, &assetRecord->object);
                zge::closeChunkFile(reader);
                changeSceneState(ee::Ptr<Scene>(assetRecord->object), 1, 0); // the asset's word
            }
            zge::closeFile(file, 0);
            if (record->onLoaded[0])
            {
                if constexpr (game::readable::kIsDm2)
                    setScriptOwner(record->owner); // ZGE2: PLAY runs for the load's widget
                commandPlay(record->onLoaded);
            }
        }
        freePendingLast(pending, record->data);
    }
    READABLE_FUNCTION_WAITS(finishUiAssetLibraryLoad, Verified);

    // loadUiAssetLibraryFile.
    int32_t loadUiAssetLibraryFile(ee::Address name, ee::Address path, ee::Address onLoaded)
    {
        return tail::zguiScriptLoadKeepingResult(name, path, onLoaded, sym::finishUiAssetLibraryLoad);
    }
    READABLE_FUNCTION_WAITS(loadUiAssetLibraryFile, Verified);

    // finishUiImageLoad: an image file in: a material of one texture. Only DM2's
    // UI scripts load one. A kept bug: the material's flags are bit-fields of a
    // word the original never initialises, so the bits it does not set are
    // whatever its caller left in that word.
    void finishUiImageLoad(ee::Ptr<uint32_t> argument)
    {
        const ee::LeftByCaller<int32_t> imageFlags; // never initialised: as the caller left it
        const uint32_t pending = *argument; // the load
        const ee::Ptr<PendingLoad> record(pending);
        if (record->file)
            zge::closeFile(ee::Ptr<zge::ZFile>(record->file), 0);
        if (record->cancelled == 0)
        {
            const uint32_t library = newUiLibrary(record->name);
            const uint32_t asset = newUiAssetEntry(library);
            const ee::Ptr<UiAsset> assetRecord(asset);
            assetRecord->kind = MaterialAsset;
            zge::stringCopy(assetRecord->name, record->name);
            // The image's kind by its extension.
            int32_t kind = sameName(record->extension, kZmpExtension) != 0 ? 0 : 3;
            if (sameName(record->extension, kBmpExtension) == 0)
                kind = 1;
            if (sameName(record->extension, kTgaExtension) == 0)
                kind = 2;
            const uint32_t image = zge::newImageRecord();
            zge::readImage(image, record->data, kind);
            const ee::Ptr<Material> material = newMaterial();
            assetRecord->object = ee::Address(material);
            // Blend 1, filter 1, the switches off.
            setupUiMaterial(material, ((imageFlags & ~0x7FF) | 0x111) & ~0x800);
            const ee::Ptr<Texture> texture = newProceduralTexture(ee::Ptr<TextureImage>(image));
            addMaterialTexture(ee::Ptr<Material>(assetRecord->object), texture);
            releaseLoadedTexture(texture);
            zge::freeImageRecord(image);
            if (record->onLoaded[0])
                commandPlay(record->onLoaded);
        }
        freePending(pending, record->data);
    }
    READABLE_FUNCTION_WAITS_OWN(finishUiImageLoad, Verified, dm2, finishFrameImageLoad);

    // finishFrameImageLoad: ZGE2's finishUiImageLoad: the image is read into
    // a record in the frame, by its extension (zmp 2, bmp 0, tga 1; a file of
    // another kind makes an asset with no material), and the load's script
    // is played for its owner. The flags' kept bug is the same.
    void finishFrameImageLoad(ee::Ptr<uint32_t> argument)
    {
        const ee::LeftByCaller<int32_t> imageFlags; // never initialised: as the caller left it
        ee::Local<zge::Image> image;
        constexpr int32_t kNoReader = 4;
        const uint32_t pending = *argument; // the load
        const ee::Ptr<PendingLoad> record(pending);
        if (record->file)
            zge::closeFile(ee::Ptr<zge::ZFile>(record->file), 0);
        if (record->cancelled == 0)
        {
            const uint32_t library = newUiLibrary(record->name);
            const uint32_t asset = newUiAssetEntry(library);
            const ee::Ptr<UiAsset> assetRecord(asset);
            assetRecord->kind = MaterialAsset;
            zge::stringCopy(assetRecord->name, record->name);
            int32_t kind = kNoReader;
            if (sameName(record->extension, kZmpExtension) == 0)
                kind = 2;
            if (sameName(record->extension, kBmpExtension) == 0)
                kind = 0;
            if (sameName(record->extension, kTgaExtension) == 0)
                kind = 1;
            if (kind != kNoReader)
            {
                zge::readImage(image.address(), record->data, kind);
                const ee::Ptr<Material> material = newMaterial();
                assetRecord->object = ee::Address(material);
                // Blend 1, filter 1, the switches off.
                setupUiMaterial(material, ((imageFlags & ~0x7FF) | 0x111) & ~0x800);
                const ee::Ptr<Texture> texture = newProceduralTexture(ee::Ptr<TextureImage>(image.address()));
                addMaterialTexture(ee::Ptr<Material>(assetRecord->object), texture);
                releaseLoadedTexture(texture);
                zge::freeImageRecord(image.address());
            }
            if (record->onLoaded[0])
            {
                setScriptOwner(record->owner);
                commandPlay(record->onLoaded);
            }
        }
        freePending(pending, record->data);
    }

    // loadUiImageFile.
    int32_t loadUiImageFile(ee::Address name, ee::Address path, ee::Address onLoaded)
    {
        return tail::zguiScriptLoadKeepingResult(name, path, onLoaded, sym::finishUiImageLoad);
    }
    READABLE_FUNCTION_WAITS(loadUiImageFile, Verified);

    // findUiLibrary: a load on its way is cancelled: its finish frees it unread.
    uint32_t findUiLibrary(uint32_t name)
    {
        ee::Ptr<PendingLoad> pending = *g_pendingLoads;
        for (;;)
        {
            if (!pending)
                break;
            if (sameName(pending->name, name) == 0)
            {
                pending->cancelled = 1;
                break;
            }
            pending = pending->next;
        }
        return 1;
    }
    READABLE_FUNCTION(findUiLibrary, Verified);

    uint32_t freeUiAsset(uint32_t asset);

    // unloadUiLibrary: a library and its assets gone; the widgets refresh.
    uint32_t unloadUiLibrary(uint32_t name)
    {
        findUiLibrary(name);
        ee::Ptr<UiLibrary> library = *g_uiLibraries;
        if (library)
        {
            for (;;)
            {
                if (sameName(library->head.name, name) == 0)
                {
                    // Its assets, first to last.
                    uint32_t *const assets = library->head.list;
                    for (uint32_t asset = assets[0]; asset; asset = assets[0])
                    {
                        freeUiAsset(asset);
                        zge::listRemove(assets, ee::Ptr<zge::ListNode>(asset));
                        giveBackUiRecord(asset);
                    }
                    zge::listRemove(ee::Ptr<zge::List>(kUiLibraries), library);
                    giveBackUiRecord(library);
                    if constexpr (game::readable::kIsBmx)
                        zge::requestTextureCacheFlushThunk(); // (ZGE4 calls it through its thunk)
                    else
                        zge::requestTextureCacheFlush();
                    fireGuiEventHandlers(ee::Ptr<Node>(*g_uiRootNode), static_cast<uint32_t>(Message::Refresh), 0);
                    return 1;
                }
                library = nextLibrary(library);
                if (!library)
                    break;
            }
        }
        return 0;
    }
    READABLE_FUNCTION_WAITS(unloadUiLibrary, Verified);

    namespace
    {
        namespace tail
        {
        int32_t freeFont(uint32_t font) { return ee::tailCall<int32_t>(kFreeFont, font); }
        }
    }

    // freeUiAsset: an asset's object freed by its kind. The result of a kind
    // past the table is what the table's address left.
    uint32_t freeUiAsset(uint32_t asset)
    {
        const ee::Ptr<UiAsset> record(asset);
        const uint32_t kind = record->kind;
        switch (kind)
        {
        case MaterialAsset:
            return game::tail(releaseMaterial, record->object);
        case FontAsset:
            return tail::freeFont(record->object);
        case DataAsset:
            return game::tail(zge::heapFree, record->object);
        case MotionAsset:
            return game::tail(zge::freeMotion, ee::Ptr<zge::Motion>(record->object));
        case SceneAsset:
            return game::tail(releaseScene, ee::Ptr<Scene>(record->object)); // the asset's word
        default:
            break;
        }
        return ee::high(sym::freeUiAssetCases);
    }
    READABLE_FUNCTION_WAITS(freeUiAsset, Verified);

    namespace
    {
        void memset(ee::Ptr<int8_t> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // findGuiAsset: "library.asset" by name; the kind must match.
    ee::Ptr<UiAsset> findGuiAsset(ee::Address pathName, uint32_t kind)
    {
        ee::Local<int8_t> libraryName;
        ee::Local<int8_t> assetName;
        memset(libraryName, 0, 0x20);
        memset(assetName, 0, 0x20);
        const ee::Ptr<const char> path(pathName);
        if (!path)
            return {};
        // Split at each dot in turn: the last dot decides.
        for (uint32_t bytes = 0; path[bytes]; ++bytes)
        {
            if (path[bytes] == '.')
            {
                zge::stringCopyN(libraryName, path, bytes);
                (ee::Ptr<int8_t>(libraryName))[bytes] = 0;
                zge::stringCopy(assetName, path + bytes + 1);
            }
        }
        if (*libraryName == 0 || *assetName == 0)
            return {};
        for (ee::Ptr<UiLibrary> library = *g_uiLibraries; library; library = classCast<UiLibrary>(library->head.next))
        {
            if (sameName(library->head.name, libraryName) != 0)
                continue;
            for (ee::Ptr<UiAsset> asset(library->head.list[0]); asset; asset = asset->next)
            {
                if (sameName(asset->name, assetName) == 0)
                    return kind == asset->kind ? asset : ee::Ptr<UiAsset>();
            }
            return {};
        }
        return {};
    }
    READABLE_FUNCTION(findGuiAsset, Verified);

    // reportUiLibraries: the REPORTLIBS command with its prints compiled out: only
    // the walks are left. 0, or with no library what the original leaves.
    int32_t reportUiLibraries()
    {
        if (!*g_uiLibraries)
            return static_cast<int32_t>(ee::high(sym::g_uiLibraries)); // what the original's result register holds
        // Each library and each of its assets (the prints are compiled out).
        for (ee::Ptr<UiLibrary> library = *g_uiLibraries; library; library = nextLibrary(library))
            for (ee::Ptr<UiAsset> asset(library->head.list[0]); asset; asset = asset->next)
                continue;
        return 0;
    }
    READABLE_FUNCTION(reportUiLibraries, Verified);

    // addListener: a child node that handles `message`.
    void addListener(ee::Address node, Message message, uint32_t handler)
    {
        const ee::Ptr<Node> listener(zge::heapTake(uiHeap(), kHandlerNodeSize));
        initNode(listener, ee::Ptr<const char>(kListenerName), kInitStaticName | static_cast<uint32_t>(NodeKind::Handler));
        listener->message = static_cast<uint16_t>(message);
        listener->handler = handler;
        game::tail(zge::zgui::appendChildNode, ee::Ptr<Node>(node), listener);
    }
    READABLE_FUNCTION(addListener, Verified);

    int32_t rebuildMaterialRegisters(ee::Ptr<zge::Material> material, uint32_t materialTable, uint32_t layer, uint32_t value)
    {
        return game::tail(zge::setMaterialState, material, materialTable, layer, value);
    }
    READABLE_FUNCTION(rebuildMaterialRegisters, Verified);
}
