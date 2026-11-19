#pragma once
// ZGE4's models (zge/scene/model_records.cpp; BMX XXX only): a rider's look as a script describes it.
//
//   MODEL_BEGIN
//     NAME ...
//     COMPONENT_BEGIN  ASPECT PART | LATTICE | SKELETON | TEXTURE00 .. TEXTURE15
//                      LIBRARY ... ASSET ... RANK ...  COMPONENT_END
//     PROPERTIES_BEGIN  PROPERTY_BEGIN NAME weight TYPE FLOAT VALUE 50 PROPERTY_END ...  PROPERTIES_END
//   MODEL_END
//
// A model record keeps, for each aspect, the assets that make it, by rank: the parts merged into one mesh over
// the skeleton, the lattices that bend the mesh by the model's three weights, and each texture slot's images,
// layered by rank into one picture. A scene object wearing a model has a binding, which remembers what of the
// model it has built so far; updateObjectModel builds what changed since (the mesh, the shape, each texture).
// The assets come from asset libraries by name, loaded the first time they are asked for.
//
// Create-a-rider, the saves and the game set-up hold riders' models; the original's word for one in the script
// is MODEL. listModelKindEntries and setModelKindEntries name the aspects "kinds" (their callers' word).

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "zge/container/tree.h"
#include "zge/text/script_values.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct SceneObject; // zge/scene/scene.h
    struct EngineMessage; // zge/scene/message.h
    struct FfdVertexWeights; // zge/scene/ffd.h
    struct FfdLattice;

    // An asset library opened by its path, kept while anything uses it (a record of the library code's).
    struct AssetLibraryUse;
    // An asset of a library, read in once for all who ask for it by name (a record of the library code's).
    struct LibraryAsset;

    // What each of a model's asset trees makes (the script's ASPECT).
    enum ModelAspect : uint32_t
    {
        kModelParts = 0,      // PART: the meshes, merged into the object's scene over the skeleton
        kModelLattices = 1,   // LATTICE: by rank, the shapes the weights blend between (rank 50 the base)
        kModelSkeleton = 2,   // SKELETON: the skeleton's scene (the tree's top entry)
        kModelTextures = 3,   // TEXTURE00 .. TEXTURE15: the images of the materials named so, layered by rank
        kModelTextureSlots = 16,
        kModelAspects = kModelTextures + kModelTextureSlots,
    };

    // A model's weights (its values "weight", "weight01", "weight02"; modelWeight's index). Each is 0 .. 200 and
    // 50 at first: weight blends the lattices of ranks 0, 50 and 100, weight01 200, 50 and 300, weight02 400, 50
    // and 500.
    constexpr uint32_t kModelWeights = 3;
    // modelWeight and setModelWeight take an index under this (the names' table has a fourth, "weight03", which no
    // record keeps).
    constexpr uint32_t kModelWeightNames = 4;

    // An asset of an aspect (0x38 bytes, from a pool of its own), a node of the aspect's tree by rank.
    struct ModelEntry
    {
        SearchTreeNode node;                // +0x00
        int32_t rank;                       // +0x08 what the tree sorts by; a texture slot's layer order
        char asset[0x20];                   // +0x0C the asset's name in its library
        uint32_t assetHash;                 // +0x2C nameHash(asset)
        ee::Ptr<AssetLibraryUse> library;   // +0x30 the library it comes from (useLibrary)
        ee::Ptr<LibraryAsset> loaded;       // +0x34 0 until the asset is asked for (modelEntryAsset)
    };
    static_assert(offsetof(ModelEntry, rank) == 0x08 && offsetof(ModelEntry, asset) == 0x0C && offsetof(ModelEntry, assetHash) == 0x2C &&
                  offsetof(ModelEntry, library) == 0x30 && offsetof(ModelEntry, loaded) == 0x34 && sizeof(ModelEntry) == 0x38);

    // A model (0x170 bytes, from a pool of its own).
    struct ModelRecord
    {
        char name[0x20];                     // +0x000 the script's NAME (findModelByName)
        uint32_t aspectChanges[kModelAspects]; // +0x020 counted up at each change of an aspect (modelAspectChanged)
        // Nothing writes these but the record's clear: a binding's copy of the changes runs on over them (into its
        // weights, which it then sets).
        uint32_t unused[kModelWeights];      // +0x06C
        SearchTree aspects[kModelAspects];   // +0x078 ModelEntry nodes by rank
        ee::Ptr<ScriptValueSet> values;      // +0x15C the PROPERTIES: the weights and any other value
        ee::Float weights[kModelWeights];    // +0x160 the values' weights, as their watcher keeps them
        uint32_t bindings;                   // +0x16C the objects wearing it
    };
    static_assert(offsetof(ModelRecord, aspectChanges) == 0x020 && offsetof(ModelRecord, unused) == 0x06C &&
                  offsetof(ModelRecord, aspects) == 0x078 && offsetof(ModelRecord, values) == 0x15C &&
                  offsetof(ModelRecord, weights) == 0x160 && offsetof(ModelRecord, bindings) == 0x16C && sizeof(ModelRecord) == 0x170);

    // A scene object wearing a model (0x6C bytes, from a pool of its own), a node of the bindings' tree by object.
    struct ModelBinding
    {
        SearchTreeNode node;                    // +0x00
        ee::Ptr<SceneObject> object;            // +0x08 what the tree sorts by
        ee::Ptr<ModelRecord> model;             // +0x0C
        uint32_t aspectChanges[kModelAspects];  // +0x10 the model's changes each aspect was last built at
        ee::Float weights[kModelWeights];       // +0x5C the weights the shape was last built with (-1: never)
        ee::Ptr<FfdVertexWeights> vertexWeights; // +0x68 the mesh bound to the base lattice, 0 until shaped
    };
    static_assert(offsetof(ModelBinding, object) == 0x08 && offsetof(ModelBinding, aspectChanges) == 0x10 &&
                  offsetof(ModelBinding, weights) == 0x5C && offsetof(ModelBinding, vertexWeights) == 0x68 && sizeof(ModelBinding) == 0x6C);

    // forEachModelEntry's visit: (model, aspect, library path, asset, rank, user) -> 1 to go on.
    using ModelEntryVisit = ee::FunctionPtr<int32_t(ee::Ptr<ModelRecord> model, uint32_t aspect, ee::Ptr<const char> library,
                                                    ee::Ptr<const char> asset, int32_t rank, uint32_t user)>;

    // The module's visits' contexts.
    // An entry's search by asset name (entryHasAssetName's context).
    struct ModelEntrySearch
    {
        uint32_t assetHash;
        ee::Ptr<const char> asset;
        ee::Ptr<ModelEntry> found; // 0: none
    };
    // forEachModelEntry's context for visitModelEntry.
    struct ModelEntryVisitContext
    {
        ee::Ptr<ModelRecord> model;
        uint32_t aspect;
        ModelEntryVisit visit;
        uint32_t user;
    };
    // objectWearsModel's context for bindingIsVisit.
    struct ModelBindingSearch
    {
        ee::Ptr<SceneObject> object;
        ee::Ptr<ModelRecord> model;
        uint32_t found;
    };
    // listModelKindEntries' cursors.
    struct ModelEntryLists
    {
        ee::Ptr<char> names;
        ee::Ptr<uint16_t> ranks; // 0: no ranks wanted
    };

    // (model_records.cpp, BMX XXX only)
    // The model script: loadModelScript reads a script's MODEL blocks, a new record for each.
    void loadModelScript(uint32_t file);
    void registerModelBlocks();
    void unregisterModelBlocks();
    ee::Ptr<ModelRecord> findModelByName(ee::Ptr<const char> name);

    // Records.
    ee::Ptr<ModelRecord> newModelRecord();
    void freeModelRecord(ee::Ptr<ModelRecord> model);
    // copyModelRecord: the destination given the source's entries and values (its name and its weights' fields
    // stay as they were).
    void copyModelRecord(ee::Ptr<ModelRecord> destination, ee::Ptr<ModelRecord> source);
    ee::Ptr<ScriptValueSet> modelValues(ee::Ptr<ModelRecord> model);
    ee::Float modelWeight(ee::Ptr<ModelRecord> model, uint8_t index);
    uint32_t setModelWeight(ee::Ptr<ModelRecord> model, uint8_t index, ee::Float weight);

    // Entries.
    ee::Ptr<SearchTree> modelAspect(ee::Ptr<ModelRecord> model, uint32_t aspect);
    void modelAspectChanged(ee::Ptr<ModelRecord> model, uint32_t aspect);
    void addModelEntry(ee::Ptr<ModelRecord> model, uint32_t aspect, ee::Ptr<const char> library, ee::Ptr<const char> asset, int32_t rank);
    void removeModelEntry(ee::Ptr<ModelRecord> model, uint32_t aspect, ee::Ptr<const char> asset);
    void clearModelAspect(ee::Ptr<ModelRecord> model, uint32_t aspect);
    int32_t forEachModelEntry(ee::Ptr<ModelRecord> model, uint32_t aspect, ModelEntryVisit visit, ee::Address user);
    void listModelKindEntries(ee::Ptr<ModelRecord> model, ee::Ptr<char> names, ee::Ptr<uint16_t> ranks, uint32_t aspect);
    void setModelKindEntries(ee::Ptr<ModelRecord> model, ee::Ptr<const char> names, ee::Ptr<const uint16_t> ranks, uint32_t aspect,
                             ee::Ptr<const char> library, uint32_t count);
    uint32_t modelEntryAsset(ee::Ptr<ModelEntry> entry);
    void releaseModelEntryAsset(ee::Ptr<ModelEntry> entry);

    // Objects wearing models.
    void bindModelToObject(ee::Ptr<SceneObject> object, ee::Ptr<ModelRecord> model);
    void unbindObjectModel(ee::Ptr<SceneObject> object);
    uint32_t objectWearsModel(ee::Ptr<SceneObject> object, ee::Ptr<ModelRecord> model);
    uint32_t updateObjectModel(ee::Ptr<SceneObject> object, uint32_t skipLoadedCheck, uint32_t quantize);
}
