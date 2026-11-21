#pragma once

// ZGE4's library assets (library_assets.cpp; BMX XXX only): an asset of an asset library (asset_library.h) asked
// for by the library's path and the asset's name, read in once for all who ask for it and kept while anyone does.
// Each library a path names is opened once, when its first asset is read, and stays open while any of its assets
// is kept. What an asset's bytes become is up to the loaders registered for it (an image, a scene, a lattice: the
// models' set-up registers those three, model_records.cpp); the first that makes something of them keeps it.

#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/file.h"
#include "zge/container/list.h"
#include "zge/container/tree.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct Scene;      // zge/scene/scene.h
    struct FfdLattice; // zge/scene/ffd.h

    // An asset library opened by its path, kept while anything uses it (0x94 bytes, from a pool of its own), a node
    // of the libraries' tree by its path's hash.
    struct AssetLibraryUse
    {
        SearchTreeNode node;  // +0x00
        uint32_t pathHash;    // +0x08 nameHash(path), what the tree sorts by
        uint32_t library;     // +0x0C the open library (openAssetLibrary), 0 until an asset is read from it
        char path[0x80];      // +0x10 as asked for, with '\' for every '/'
        int32_t users;        // +0x90
    };
    static_assert(offsetof(AssetLibraryUse, pathHash) == 0x08 && offsetof(AssetLibraryUse, library) == 0x0C &&
                  offsetof(AssetLibraryUse, path) == 0x10 && offsetof(AssetLibraryUse, users) == 0x90 &&
                  sizeof(AssetLibraryUse) == 0x94);

    // What a loader makes of an asset's bytes read into memory (0: nothing, the next loader is asked), and how what
    // it made is freed.
    using LibraryAssetLoad = ee::FunctionPtr<uint32_t(uint32_t bytes, uint32_t length)>;
    using LibraryAssetFree = ee::FunctionPtr<void(uint32_t data)>;

    // A registered loader (0x10 bytes, from a pool of its own), on the loaders' list in the order registered.
    struct LibraryAssetLoader
    {
        ListNode link;          // +0x00
        LibraryAssetLoad load;  // +0x08
        LibraryAssetFree free;  // +0x0C
    };
    static_assert(offsetof(LibraryAssetLoader, load) == 0x08 && offsetof(LibraryAssetLoader, free) == 0x0C &&
                  sizeof(LibraryAssetLoader) == 0x10);

    // An asset of a library, read in once for all who ask for it by name (0x3C bytes, from a pool of its own), a
    // node of the assets' tree by its id.
    struct LibraryAsset
    {
        SearchTreeNode node;                 // +0x00
        uint32_t id;                         // +0x08 nameHash(name), its id in the library: what the tree sorts by
        char name[0x20];                     // +0x0C
        ee::Ptr<AssetLibraryUse> library;    // +0x2C
        uint32_t data;                       // +0x30 what its loader made of it, 0 until the read is done
        ee::Ptr<LibraryAssetLoader> loader;  // +0x34 the loader that made it
        int32_t users;                       // +0x38
    };
    static_assert(offsetof(LibraryAsset, id) == 0x08 && offsetof(LibraryAsset, name) == 0x0C &&
                  offsetof(LibraryAsset, library) == 0x2C && offsetof(LibraryAsset, data) == 0x30 &&
                  offsetof(LibraryAsset, loader) == 0x34 && offsetof(LibraryAsset, users) == 0x38 &&
                  sizeof(LibraryAsset) == 0x3C);

    // An asset's read in flight (0xC bytes, from a pool of its own): its buffer, the entry it reads and its
    // library, the read's callback context.
    struct LibraryAssetRead
    {
        uint32_t buffer;                   // +0x00 allocateAligned: the entry's bytes
        uint32_t entry;                    // +0x04 the entry selected for the read (currentAsset)
        ee::Ptr<AssetLibraryUse> library;  // +0x08
    };
    static_assert(offsetof(LibraryAssetRead, entry) == 0x04 && offsetof(LibraryAssetRead, library) == 0x08 &&
                  sizeof(LibraryAssetRead) == 0x0C);

    // (library_assets.cpp)
    ee::Ptr<AssetLibraryUse> useLibrary(ee::Ptr<const char> path);
    void keepLibrary(ee::Ptr<AssetLibraryUse> library);
    void releaseLibrary(ee::Ptr<AssetLibraryUse> library);
    uint32_t openedAssetLibrary(ee::Ptr<AssetLibraryUse> library);
    void closeLibraryFile(ee::Ptr<AssetLibraryUse> library);
    int32_t closeLibraryFiles();
    ee::Ptr<const char> libraryPath(ee::Ptr<AssetLibraryUse> library);
    void registerLibraryAssetLoader(LibraryAssetLoad load, LibraryAssetFree free);
    void unregisterLibraryAssetLoader(LibraryAssetLoad load, LibraryAssetFree free);
    ee::Ptr<LibraryAsset> loadLibraryAsset(ee::Ptr<const char> library, ee::Ptr<const char> asset);
    void releaseLibraryAsset(ee::Ptr<LibraryAsset> asset);
    uint32_t libraryAssetData(ee::Ptr<LibraryAsset> asset);
    uint32_t loadImageAsset(uint32_t bytes, uint32_t length);
    uint32_t freeImageAsset(uint32_t image);
    ee::Ptr<Scene> loadSceneAsset(uint32_t bytes, uint32_t length);
    uint32_t freeSceneAsset(ee::Ptr<Scene> scene);
    ee::Ptr<FfdLattice> loadLatticeAsset(uint32_t bytes, uint32_t length);
    void freeLatticeAsset(ee::Ptr<FfdLattice> lattice);
}
