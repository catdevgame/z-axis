// ZGE4's library assets (library_assets.h; BMX XXX only): the libraries used by path, the assets read from them by
// name, the loaders that make something of an asset's bytes, and the three loaders the models use (an image, a
// scene, a lattice). It sits after the script values and before the animators in the link order.
//
// Original quirks kept: useLibrary copies 128 bytes of the path whatever its length; loadLibraryAsset, when the
// read cannot start, takes the asset out of the tree but keeps its record and its use of the library.

#include "library_assets.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/assets/images.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/assets/library_assets.cpp.h"

namespace zge
{
    namespace
    {
        // The libraries by their path's hash, the assets by their id, and the loaders in the order registered;
        // each tree set up with its first use (the assets' with the first loader).
        inline ee::Ptr<SearchTree> libraryUses() { return ee::Ptr<SearchTree>(sym::bmx::g_libraryUses); }
        inline ee::Global<uint32_t, sym::bmx::g_libraryUsesReady> g_libraryUsesReady;
        inline ee::Ptr<SearchTree> libraryAssets() { return ee::Ptr<SearchTree>(sym::bmx::g_libraryAssets); }
        inline ee::Ptr<List> libraryAssetLoaders() { return ee::Ptr<List>(sym::bmx::g_libraryAssetLoaders); }
        inline ee::Global<uint32_t, sym::bmx::g_libraryAssetsReady> g_libraryAssetsReady;

        // The records' pools, each made with its first record and destroyed with its last.
        inline ee::Ptr<ee::Ptr<Pool>> libraryUsePool() { return ee::Ptr<ee::Ptr<Pool>>(sym::bmx::g_libraryUsePool); }
        inline ee::Ptr<ee::Ptr<Pool>> libraryAssetPool() { return ee::Ptr<ee::Ptr<Pool>>(sym::bmx::g_libraryAssetPool); }
        inline ee::Ptr<ee::Ptr<Pool>> libraryAssetReadPool() { return ee::Ptr<ee::Ptr<Pool>>(sym::bmx::g_libraryAssetReadPool); }
        inline ee::Ptr<ee::Ptr<Pool>> libraryAssetLoaderPool() { return ee::Ptr<ee::Ptr<Pool>>(sym::bmx::g_libraryAssetLoaderPool); }

        // The forms the scene and lattice loaders read: a model file is an FMDL chunk, a lattice a FORM of ZFFD.
        constexpr uint32_t kModelFileTag = chunkTag("FMDL");
        constexpr uint32_t kFormTag = chunkTag("FORM");
        constexpr uint32_t kLatticeFormType = chunkTag("ZFFD");
        constexpr uint32_t kLatticeFormTypeAt = 2;   // the FORM's type, its third word
        constexpr uint32_t kLatticeFormHeaderBytes = 0x0C;
        // The second scene state bit, which changeSceneState switches nothing for (loadSceneAsset sets it off too).
        constexpr uint32_t kSceneStateUnused = 1u << 1;

        constexpr uint32_t kLibraryPathBytes = sizeof(AssetLibraryUse::path);
        constexpr uint32_t kFileReportDone = 1; // FileReport::succeeded
    }

    // keepLibrary: one more user of the library.
    inline void keepLibrary(ee::Ptr<AssetLibraryUse> library)
    {
        ++library->users;
    }
    READABLE_FUNCTION_ONLY(bmx, keepLibrary);

    // useLibrary: the library at that path ('/' read as '\'), with one more user; made, with one and not yet open,
    // the first time.
    inline ee::Ptr<AssetLibraryUse> useLibrary(ee::Ptr<const char> path)
    {
        if (*g_libraryUsesReady == 0)
        {
            initSearchTree(libraryUses(), sym::lessWord);
            *g_libraryUsesReady = 1;
        }
        ee::Local<char[kLibraryPathBytes]> backslashed;
        ee::Local<uint32_t> pathHash;
        for (uint32_t i = 0; i < kLibraryPathBytes; ++i)
        {
            const char c = path[i];
            backslashed[i] = c == '/' ? '\\' : c;
        }
        *pathHash = nameHash(backslashed);
        ee::Ptr<AssetLibraryUse> library = treeRecordOf<AssetLibraryUse>(searchTreeFind(libraryUses(), pathHash));
        if (library)
        {
            keepLibrary(library);
            return library;
        }
        library = ee::Ptr<AssetLibraryUse>(poolTakeCreating(libraryUsePool(), sizeof(AssetLibraryUse)));
        library->pathHash = *pathHash;
        stringCopy(library->path, backslashed);
        library->library = 0;
        library->users = 1;
        searchTreeInsert(libraryUses(), &library->node);
        return library;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, useLibrary);

    // closeLibraryFile: the library closed if it is open (it opens again when an asset is read from it).
    inline void closeLibraryFile(ee::Ptr<AssetLibraryUse> library)
    {
        if (library->library == 0)
            return;
        closeAssetLibrary(library->library);
        library->library = 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, closeLibraryFile);

    // releaseLibrary: one user less; with none left the library closed and its record freed.
    inline void releaseLibrary(ee::Ptr<AssetLibraryUse> library)
    {
        if (--library->users != 0)
            return;
        searchTreeRemove(libraryUses(), &library->pathHash);
        closeLibraryFile(library);
        game::tail(poolGiveDestroying, libraryUsePool(), library);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, releaseLibrary);

    // closeLibraryFileOf (closeLibraryFiles' visit): the library's file closed. 1: on.
    inline int32_t closeLibraryFileOf(ee::Ptr<AssetLibraryUse> library)
    {
        closeLibraryFile(library);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, closeLibraryFileOf);

    // closeLibraryFiles: every library's file closed, the libraries kept (the walk's 1; 0 with none ever used).
    inline int32_t closeLibraryFiles()
    {
        if (*g_libraryUsesReady == 0)
            return 0;
        return game::tail(searchTreeForEach, libraryUses(), SearchTreeVisit(sym::bmx::closeLibraryFileOf), 0);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, closeLibraryFiles);

    // openedAssetLibrary: the library, opened the first time (asset_library.h's handle).
    inline uint32_t openedAssetLibrary(ee::Ptr<AssetLibraryUse> library)
    {
        if (library->library == 0)
            library->library = openAssetLibrary(library->path);
        return library->library;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, openedAssetLibrary);

    inline ee::Ptr<const char> libraryPath(ee::Ptr<AssetLibraryUse> library)
    {
        return ee::Ptr<const char>(library->path);
    }
    READABLE_FUNCTION_ONLY(bmx, libraryPath);

    // registerLibraryAssetLoader: a loader put last on the list (the assets' tree and the list set up with the
    // first).
    inline void registerLibraryAssetLoader(LibraryAssetLoad load, LibraryAssetFree free)
    {
        if (*g_libraryAssetsReady == 0)
        {
            initSearchTree(libraryAssets(), sym::lessWord);
            initList(libraryAssetLoaders());
            *g_libraryAssetsReady = 1;
        }
        const ee::Ptr<LibraryAssetLoader> loader(poolTakeCreating(libraryAssetLoaderPool(), sizeof(LibraryAssetLoader)));
        loader->load = load;
        loader->free = free;
        game::tail(listPush, libraryAssetLoaders(), loader);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, registerLibraryAssetLoader);

    // unregisterLibraryAssetLoader: the first loader of that pair taken off the list and freed.
    inline void unregisterLibraryAssetLoader(LibraryAssetLoad load, LibraryAssetFree free)
    {
        for (ee::Ptr<ListNode> node = libraryAssetLoaders()->head; node; node = node->next)
        {
            const ee::Ptr<LibraryAssetLoader> loader = recordOfNode<LibraryAssetLoader>(node);
            if (loader->load == load && loader->free == free)
            {
                listRemove(libraryAssetLoaders(), loader);
                poolGiveDestroying(libraryAssetLoaderPool(), loader);
                return;
            }
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, unregisterLibraryAssetLoader);

    // finishLibraryAssetRead (loadLibraryAsset's read's callback): once the entry is read, the asset still kept
    // and a loader makes something of its bytes, that and the loader kept in the asset; then the read's use of the
    // library given up and its buffer and record freed.
    inline void finishLibraryAssetRead(ee::Ptr<FileReport> report)
    {
        const ee::Ptr<LibraryAssetRead> read(report->context);
        if (report->succeeded == kFileReportDone)
        {
            const uint32_t library = openedAssetLibrary(read->library);
            selectAsset(library, read->entry);
            ee::Local<uint32_t> id;
            *id = currentAssetId(library);
            const ee::Ptr<LibraryAsset> asset = treeRecordOf<LibraryAsset>(searchTreeFind(libraryAssets(), id));
            if (asset)
            {
                const uint32_t length = currentAssetLength(library);
                for (ee::Ptr<ListNode> node = libraryAssetLoaders()->head; node; node = node->next)
                {
                    const ee::Ptr<LibraryAssetLoader> loader = recordOfNode<LibraryAssetLoader>(node);
                    const uint32_t data = loader->load(read->buffer, length);
                    if (data != 0)
                    {
                        asset->loader = loader;
                        asset->data = data;
                        break;
                    }
                }
            }
        }
        releaseLibrary(read->library);
        heapFree(read->buffer);
        poolGiveDestroying(libraryAssetReadPool(), read);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, finishLibraryAssetRead);

    // releaseLibraryAsset: one user less; with none left the asset taken out, what its loader made freed, its
    // library given up and its record freed.
    inline void releaseLibraryAsset(ee::Ptr<LibraryAsset> asset)
    {
        if (--asset->users != 0)
            return;
        searchTreeRemove(libraryAssets(), &asset->id);
        if (asset->data != 0)
            asset->loader->free(asset->data);
        releaseLibrary(asset->library);
        game::tail(poolGiveDestroying, libraryAssetPool(), asset);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, releaseLibraryAsset);

    // loadLibraryAsset: the asset of that name in the library at that path, with one more user. The first time,
    // its entry's read is started (into a buffer of the entry's length rounded up to its alignment, aligned so)
    // and the asset kept with nothing made yet: finishLibraryAssetRead makes it once read. 0 when the read cannot
    // start.
    inline ee::Ptr<LibraryAsset> loadLibraryAsset(ee::Ptr<const char> libraryPath, ee::Ptr<const char> name)
    {
        ee::Local<uint32_t> id;
        *id = nameHash(name);
        ee::Ptr<LibraryAsset> asset = treeRecordOf<LibraryAsset>(searchTreeFind(libraryAssets(), id));
        if (asset)
        {
            ++asset->users;
            return asset;
        }
        const ee::Ptr<AssetLibraryUse> use = useLibrary(libraryPath);
        const uint32_t library = openedAssetLibrary(use);
        findAsset(library, *id);
        asset = ee::Ptr<LibraryAsset>(poolTakeCreating(libraryAssetPool(), sizeof(LibraryAsset)));
        asset->id = *id;
        stringCopy(asset->name, name);
        asset->library = use;
        asset->data = 0;
        asset->loader = ee::Ptr<LibraryAssetLoader>();
        asset->users = 1;
        searchTreeInsert(libraryAssets(), &asset->node);
        keepLibrary(use);

        const ee::Ptr<LibraryAssetRead> read(poolTakeCreating(libraryAssetReadPool(), sizeof(LibraryAssetRead)));
        read->entry = currentAsset(library);
        read->library = use;
        const uint32_t length = currentAssetLength(library);
        // (the entry's alignment asked for each time it is used)
        const uint32_t roundUp = static_cast<uint32_t>(currentAssetAlignment(library));
        const uint32_t roundTo = static_cast<uint32_t>(currentAssetAlignment(library));
        const uint32_t padded = (length - 1 + roundUp) & -roundTo;
        read->buffer = allocateAligned(static_cast<uint32_t>(currentAssetAlignment(library)), padded);
        ee::Local<FileCallback> whenRead;
        whenRead->function = sym::bmx::finishLibraryAssetRead;
        whenRead->context = read.address;
        if (readAsset(library, read->buffer, whenRead) == 1)
            return asset;
        searchTreeRemove(libraryAssets(), &asset->id);
        heapFree(read->buffer);
        poolGiveDestroying(libraryAssetReadPool(), read);
        return ee::Ptr<LibraryAsset>();
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, loadLibraryAsset);

    // libraryAssetData: what the asset's loader made of it, 0 until it is read.
    inline uint32_t libraryAssetData(ee::Ptr<LibraryAsset> asset)
    {
        return asset->data;
    }
    READABLE_FUNCTION_ONLY(bmx, libraryAssetData);

    // ---- The models' loaders (model_records.cpp registers them) -------------------------------------------------

    // loadImageAsset: an image read from the bytes when an image reader knows them (kImageMainMemory taken off
    // the new record: its data from the heap), else 0.
    inline uint32_t loadImageAsset(uint32_t bytes, uint32_t length)
    {
        static_cast<void>(length);
        const int32_t kind = findImageReader(bytes);
        if (kind == 0)
            return 0;
        const uint32_t image = newImageRecord();
        if (image == 0)
            return 0;
        ee::Ptr<Image>(image)->flags &= ~kImageMainMemory;
        readImage(image, bytes, static_cast<uint32_t>(kind));
        return image;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, loadImageAsset);

    inline uint32_t freeImageAsset(uint32_t image)
    {
        return game::tail(freeImageRecord, image);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeImageAsset);

    // loadSceneAsset: a scene read from a model file's bytes (its chunks into it), not shown (and the unused state
    // bit off); 0 when the bytes are no model file or make no scene.
    inline ee::Ptr<Scene> loadSceneAsset(uint32_t bytes, uint32_t length)
    {
        ee::Local<ee::Ptr<Scene>> scene;
        *scene = ee::Ptr<Scene>();
        if (length < sizeof(uint32_t) || *ee::Ptr<uint32_t>(bytes) != kModelFileTag)
            return ee::Ptr<Scene>();
        const ee::Ptr<ZFile> file = openMemoryFile(bytes, length);
        const ee::Ptr<ChunkReader> reader = openChunkFile(file, kModelFileTag, 0);
        parseChunks(reader, scene);
        closeChunkFile(reader);
        closeFile(file, 0);
        if (!*scene)
            return ee::Ptr<Scene>();
        changeSceneState(*scene, kSceneShown, 0);
        changeSceneState(*scene, kSceneStateUnused, 0);
        return *scene;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, loadSceneAsset);

    inline uint32_t freeSceneAsset(ee::Ptr<Scene> scene)
    {
        return game::tail(releaseScene, scene);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeSceneAsset);

    // loadLatticeAsset: a lattice read from a ZFFD form's bytes (its chunks into it); 0 when the bytes are none.
    inline ee::Ptr<FfdLattice> loadLatticeAsset(uint32_t bytes, uint32_t length)
    {
        ee::Local<ee::Ptr<FfdLattice>> lattice;
        *lattice = ee::Ptr<FfdLattice>();
        if (length < kLatticeFormHeaderBytes || ee::Ptr<uint32_t>(bytes)[kLatticeFormTypeAt] != kLatticeFormType)
            return *lattice;
        const ee::Ptr<ZFile> file = openMemoryFile(bytes, length);
        const ee::Ptr<ChunkReader> reader = openChunkFile(file, kFormTag, 0);
        parseChunks(reader, lattice);
        closeChunkFile(reader);
        closeFile(file, 0);
        return *lattice;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, loadLatticeAsset);

    // freeLatticeAsset: the lattice freed (ffd.cpp's freeFfdLattice, which BMX XXX's instance does not compile:
    // the original's, by address).
    inline void freeLatticeAsset(ee::Ptr<FfdLattice> lattice)
    {
        ee::tailCall<void>(sym::freeFfdLattice, lattice);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeLatticeAsset);
}
