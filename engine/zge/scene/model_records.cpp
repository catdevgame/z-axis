// ZGE4's models (model_records.h; BMX XXX only): the MODEL script's records, their entries by aspect and rank,
// and the scene objects wearing them. It sits after the particle emitter objects in the link order and before
// the script values. The script reader is the original's.

#include "model_records.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/assets/image_layers.h"
#include "zge/assets/library_assets.h"
#include "zge/assets/images.h"
#include "zge/container/tree.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/platform/platform.h"
#include "zge/render/material.h"
#include "zge/render/texture.h"
#include "zge/scene/ffd.h"
#include "zge/scene/message.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/text/cstring.h"
#include "zge/text/script_blocks.h"
#include "zge/text/script_values.h"
#include "zge/text/strings.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/model_records.cpp.h"

namespace zge
{
    void poolGiveDestroying(ee::Ptr<ee::Ptr<Pool>> slot, ee::Address record); // (pool.cpp)
    ee::Ptr<SearchTreeNode> searchTreeLastBefore(ee::Ptr<SearchTree> tree, ee::Address key); // (tree.cpp)
    ee::Ptr<Material> findNodeMaterialByHash(ee::Ptr<Node> node, uint32_t nameHash, uint32_t skip); // (terrain.cpp)
    int32_t releaseTextureRef(ee::Ptr<Texture> texture); // (loaded_textures.cpp)
    int32_t newTextureFromPixels(ee::Ptr<const Rgba8> pixels, uint32_t width, uint32_t height, uint32_t quantize); // (images.cpp)
    uint32_t releaseScene(ee::Ptr<Scene> scene); // (pose.cpp)
    uint32_t unregisterLoadedAsset(uint32_t kind, uint32_t form, ee::Address asset, ee::Ptr<LoadedAsset> record); // (scene_database.cpp)
    ee::Ptr<Scene> mergeSceneMeshes(ee::Ptr<ee::Ptr<Scene>> scenes, uint32_t count, ee::Ptr<Scene> scene);
    namespace
    {
        // The lattices (ffd.cpp, which BMX XXX's instance does not compile: the original's, by address).
        inline void freeFfdLattice(ee::Ptr<FfdLattice> lattice) { ee::game<void>(sym::freeFfdLattice, lattice); }
        inline void copyFfdDimensions(ee::Ptr<FfdLattice> from, ee::Ptr<FfdLattice> to) { ee::game<void>(sym::copyFfdDimensions, from, to); }
        inline ee::Ptr<FfdLattice> newFfdLattice(uint32_t countX, uint32_t countY, uint32_t countZ)
        {
            return ee::Ptr<FfdLattice>(ee::game<uint32_t>(sym::newFfdLattice, countX, countY, countZ));
        }
        inline void freeFfdVertexWeights(ee::Ptr<FfdVertexWeights> record) { ee::game<void>(sym::freeFfdVertexWeights, record); }
        inline ee::Ptr<FfdVertexWeights> bindPartToFfd(ee::Ptr<ZgePart> part, ee::Ptr<FfdLattice> lattice)
        {
            return ee::Ptr<FfdVertexWeights>(ee::game<uint32_t>(sym::bindPartToFfd, part, lattice));
        }
        inline void deformPartByFfd(ee::Ptr<ZgePart> part, ee::Ptr<FfdVertexWeights> weights, ee::Ptr<FfdLattice> lattice)
        {
            ee::game<void>(sym::deformPartByFfd, part, weights, lattice);
        }
        namespace tail
        {
            inline void blendFfdLattice(ee::Ptr<FfdLattice> out, ee::Ptr<FfdLattice> a, ee::Ptr<FfdLattice> b, ee::Float t)
            {
                ee::tailCall<void>(sym::blendFfdLattice, out, a, b, t);
            }
        }
    }

    namespace
    {
        inline ee::Global<uint32_t, sym::bmx::g_modelRecordsReady> g_modelRecordsReady;
        constexpr uint32_t kModelPool = sym::bmx::g_modelPool;
        constexpr uint32_t kModelEntryPool = sym::bmx::g_modelEntryPool;
        constexpr uint32_t kModelBindingPool = sym::bmx::g_modelBindingPool;
        constexpr uint32_t kModelBindings = sym::bmx::g_modelBindings; // a SearchTree of ModelBinding by object
        inline ee::Ptr<ee::Ptr<Pool>> poolSlot(uint32_t slot) { return ee::Ptr<ee::Ptr<Pool>>(slot); }
        inline ee::Ptr<SearchTree> modelBindings() { return ee::Ptr<SearchTree>(kModelBindings); }
        inline ee::Ptr<const char> text(uint32_t symbol) { return ee::Ptr<const char>(symbol); }

        // The script's weights: 0 .. 200, 50 at first.
        constexpr float kDefaultModelWeight = 50.0f;
        constexpr float kMostModelWeight = 200.0f;
        // A weight as a share of the way along its three lattices (two constants of the original's, equal).
        constexpr float kWeightShare = 0.01f;
        // The base lattice's rank.
        constexpr int32_t kBaseLatticeRank = 50;
        // addModelEntry's rank for "after the last".
        constexpr int32_t kRankAfterLast = -1;
        // LoadedAsset::form of a model's scene.
        constexpr uint32_t kModelSceneForm = 1;
        // The second scene state bit, which changeSceneState switches nothing for; set off once a mesh is merged.
        constexpr uint32_t kSceneStateUnused = 1u << 1;

        // The original's tail jump to the lattices' blend.
        namespace tail
        {
            // out = onto + (the blend of a and b by t) - base, point by point (ffd.cpp's ZGE4 addition; the original's).
            inline void addFfdLatticeBlend(ee::Ptr<FfdLattice> out, ee::Ptr<FfdLattice> a, ee::Ptr<FfdLattice> b, ee::Ptr<FfdLattice> base,
                                           ee::Ptr<FfdLattice> onto, ee::Float t)
            {
                ee::tailCall<void>(sym::bmx::addFfdLatticeBlend, out, a, b, base, onto, t);
            }
        }
        inline void memset(ee::Address destination, int32_t byte, uint32_t bytes) { ee::game<void>(sym::memset, destination, byte, bytes); }
        inline void remakeObjectAnimator(ee::Ptr<SceneObject> object) { ee::game<void>(sym::bmx::remakeObjectAnimator, object); }
        // The texture slot's materials' name, "TEXTURE00" .. "TEXTURE15", in the scratch text.
        inline uint32_t textureSlotNameHash(uint32_t aspect) { return nameHash(ee::Ptr<const char>(formatScratch(sym::bmx::textureAspectFormat, aspect - kModelTextures))); }

        // The script's clamp of a weight to 0 .. 200 reads the value again at each test.
        inline ee::Float clampedWeight(ee::Ptr<ScriptValue> value)
        {
            if (scriptValueFloat(value) < kMostModelWeight && !(ee::Float(0.0f) < scriptValueFloat(value)))
                return ee::Float(0.0f);
            if (!(scriptValueFloat(value) < kMostModelWeight))
                return ee::Float(kMostModelWeight);
            return scriptValueFloat(value);
        }

        // The parts' table of asset words, as the collecting visit fills it.
        ee::Ptr<uint32_t> wordsOf(ee::Ptr<ee::Ptr<Scene>> parts) { return parts.cast<uint32_t>(); }

        // A new entry put in the aspect's tree (addModelEntry's and copyModelEntryTo's, the same code twice).
        inline void newModelEntry(ee::Ptr<SearchTree> tree, ee::Ptr<const char> library, ee::Ptr<const char> asset, int32_t rank)
        {
            const ee::Ptr<ModelEntry> entry(poolTakeCreating(poolSlot(kModelEntryPool), sizeof(ModelEntry)));
            stringCopy(entry->asset, asset);
            entry->assetHash = nameHash(asset);
            entry->rank = rank;
            entry->loaded = ee::Ptr<LibraryAsset>();
            entry->library = useLibrary(library);
            searchTreeInsert(tree, &entry->node);
        }

        // The object's position and orientation kept while it is put at the origin with no turn (a zero
        // quaternion), and given back after.
        struct HeldPlace
        {
            ee::Local<Vec4> position;
            ee::Local<Quaternion> orientation;
        };
        inline void holdPlace(ee::Ptr<SceneObject> object, HeldPlace &held)
        {
            *held.position = *objectPosition(object);
            quaternionCopy(held.orientation, objectOrientation(object));
        }
        inline void restorePlace(ee::Ptr<SceneObject> object, HeldPlace &held)
        {
            setObjectPosition(object, held.position);
            setObjectOrientation(object, held.orientation);
        }
    }

    uint32_t bindingAspectLoaded(ee::Ptr<ModelBinding> binding, uint32_t aspect);
    void keepObjectTextures(ee::Ptr<SceneObject> object, ee::Ptr<uint32_t> textures);
    uint32_t restoreObjectTextures(ee::Ptr<SceneObject> object, ee::Ptr<const uint32_t> textures);

    // freeModelValues: the model's values unwatched and freed.
    inline void freeModelValues(ee::Ptr<ModelRecord> model)
    {
        unwatchScriptValues(model->values);
        game::tail(freeScriptValues, model->values);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeModelValues);

    // registerModelBlocks: the MODEL block (its key NAME, a text), which makes a record as it begins.
    inline void registerModelBlocks()
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>> keyName;
        ee::Local<uint32_t> keyKind;
        *keyName = text(sym::bmx::modelNameKey);
        setup->begin = text(sym::bmx::modelBeginKeyword);
        setup->end = text(sym::bmx::modelEndKeyword);
        setup->parentBegin = ee::Ptr<const char>();
        setup->keyNames = keyName;
        setup->keyKinds = keyKind;
        setup->keyCount = 1;
        setup->onBegin = sym::bmx::beginModelBlock;
        setup->onEnd = sym::bmx::endModelBlock;
        *keyKind = kScriptValueText;
        registerBlockClass(setup);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, registerModelBlocks);

    // beginModelBlock: a new record, the block's owner, and the blocks inside a MODEL: COMPONENT (ASPECT, LIBRARY
    // and ASSET texts, RANK an integer) and PROPERTIES, both owned by the record too.
    inline int32_t beginModelBlock(uint32_t begin)
    {
        constexpr uint32_t kComponentKeys = 4, kRankKey = 3;
        ee::Local<ee::Ptr<const char>[kComponentKeys]> keyNames;
        ee::Local<uint32_t[kComponentKeys]> keyKinds;
        ee::Local<BlockClassSetup> component;
        ee::Local<BlockClassSetup> properties;
        const ee::Ptr<ee::Ptr<const char>> names(sym::bmx::componentKeyNames);
        for (uint32_t i = 0; i < kComponentKeys; ++i)
            keyNames[i] = names[i];
        memset(keyKinds, 0, sizeof(uint32_t[kComponentKeys]));
        keyKinds[kRankKey] = kScriptValueInteger;
        const ee::Ptr<ModelRecord> model = newModelRecord();
        setBlockClassOwner(ee::Ptr<const char>(begin), model);

        component->begin = text(sym::bmx::componentBeginKeyword);
        component->end = text(sym::bmx::componentEndKeyword);
        component->parentBegin = ee::Ptr<const char>(begin);
        component->keyNames = keyNames;
        component->keyKinds = keyKinds;
        component->keyCount = kComponentKeys;
        component->onBegin = 0;
        component->onEnd = sym::bmx::endModelComponentBlock;
        registerBlockClass(component);
        setBlockClassOwner(text(sym::bmx::componentBeginKeyword), model);

        properties->begin = text(sym::bmx::propertiesBeginKeyword);
        properties->end = text(sym::bmx::propertiesEndKeyword);
        properties->parentBegin = ee::Ptr<const char>(begin);
        properties->keyNames = ee::Ptr<ee::Ptr<const char>>();
        properties->keyKinds = ee::Ptr<uint32_t>();
        properties->keyCount = 0;
        properties->onBegin = sym::bmx::beginModelPropertiesBlock;
        properties->onEnd = sym::bmx::endModelPropertiesBlock;
        registerBlockClass(properties);
        setBlockClassOwner(text(sym::bmx::propertiesBeginKeyword), model);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, beginModelBlock);

    // endModelBlock: the record named, the block's owner cleared, its weights read from its values, and the
    // inner blocks gone.
    inline int32_t endModelBlock(uint32_t begin)
    {
        const ee::Ptr<ModelRecord> model(blockClassOwner(ee::Ptr<const char>(begin)));
        const ee::Ptr<ScriptValueSet> blockValues = blockClassValues(ee::Ptr<const char>(begin));
        scriptValueText(findScriptValue(blockValues, text(sym::bmx::modelNameKey)), ee::Ptr<char>(model->name),
                        sizeof(model->name));
        setBlockClassOwner(ee::Ptr<const char>(begin), 0);
        model->weights[0] = scriptValueFloat(findScriptValue(model->values, text(sym::bmx::modelWeightKey)));
        model->weights[1] = scriptValueFloat(findScriptValue(model->values, text(sym::bmx::modelWeight01Key)));
        model->weights[2] = scriptValueFloat(findScriptValue(model->values, text(sym::bmx::modelWeight02Key)));
        unregisterBlockClass(text(sym::bmx::componentBeginKeyword));
        unregisterBlockClass(text(sym::bmx::propertiesBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, endModelBlock);

    // beginModelPropertiesBlock: PROPERTY blocks inside it read into the record's values.
    inline int32_t beginModelPropertiesBlock(uint32_t begin)
    {
        const ee::Ptr<ModelRecord> model(blockClassOwner(ee::Ptr<const char>(begin)));
        registerPropertyBlock(modelValues(model), ee::Ptr<const char>(begin));
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, beginModelPropertiesBlock);

    inline int32_t endModelPropertiesBlock(uint32_t /*begin: not read*/)
    {
        unregisterPropertyBlock();
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, endModelPropertiesBlock);

    inline void unregisterModelBlocks() { game::tail(unregisterBlockClass, text(sym::bmx::modelBeginKeyword)); }
    READABLE_FUNCTION_WAITS_ONLY(bmx, unregisterModelBlocks);

    // entryHasAssetName (a visit): the entry of the search's asset found (case aside), which stops the walk.
    inline int32_t entryHasAssetName(ee::Ptr<ModelEntry> entry, ee::Ptr<ModelEntrySearch> search)
    {
        if (entry->assetHash != search->assetHash || stringCompareNoCase(search->asset, entry->asset) != 0)
            return 1;
        search->found = entry;
        return 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, entryHasAssetName);

    // visitModelEntry (a visit): forEachModelEntry's visit given the entry's library path, asset and rank.
    inline int32_t visitModelEntry(ee::Ptr<ModelEntry> entry, ee::Ptr<ModelEntryVisitContext> context)
    {
        const ee::Ptr<const char> library = libraryPath(entry->library);
        return context->visit(context->model, context->aspect, library, ee::Ptr<const char>(entry->asset),
                              entry->rank, context->user) == 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, visitModelEntry);

    // endModelComponentBlock: the component's asset added to the record under its aspect: PART, LATTICE,
    // SKELETON or TEXTURE00 .. TEXTURE15 (case aside), else the aspect's number as written.
    inline int32_t endModelComponentBlock(uint32_t begin)
    {
        ee::Local<char[0x20]> aspectName;
        ee::Local<char[0x80]> library;
        ee::Local<char[0x40]> asset;
        ee::Local<char[0x20]> slotName;
        const ee::Ptr<ModelRecord> model(blockClassOwner(ee::Ptr<const char>(begin)));
        const ee::Ptr<ScriptValueSet> values = blockClassValues(ee::Ptr<const char>(begin));
        // The callers' buffer sizes (scriptValueText reads none of them).
        constexpr uint32_t kNameBytes = 0x80, kAssetBytes = 0x40;
        scriptValueText(findScriptValue(values, text(sym::bmx::componentAspectKey)), aspectName, kNameBytes);
        uint32_t aspect = kModelAspects;
        if (stringCompareNoCase(aspectName, sym::bmx::partAspectName) == 0)
            aspect = kModelParts;
        else if (stringCompareNoCase(aspectName, sym::bmx::latticeAspectName) == 0)
            aspect = kModelLattices;
        else if (stringCompareNoCase(aspectName, sym::bmx::skeletonAspectName) == 0)
            aspect = kModelSkeleton;
        else
        {
            for (uint32_t slot = 0; slot < kModelTextureSlots; ++slot)
            {
                formatString(slotName, sym::bmx::textureAspectFormat, slot);
                if (stringCompareNoCase(aspectName, slotName) == 0)
                {
                    aspect = kModelTextures + slot;
                    break;
                }
            }
        }
        if (aspect == kModelAspects)
            aspect = static_cast<uint32_t>(stringToInt(aspectName, 10));
        scriptValueText(findScriptValue(values, text(sym::bmx::componentLibraryKey)), library, kNameBytes);
        scriptValueText(findScriptValue(values, text(sym::bmx::componentAssetKey)), asset, kAssetBytes);
        const int32_t rank = scriptValueInteger(findScriptValue(values, text(sym::bmx::componentRankKey)));
        addModelEntry(model, aspect, library, asset, rank);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, endModelComponentBlock);

    // releaseEntryAssetVisit (a visit): the entry's asset let go.
    inline int32_t releaseEntryAssetVisit(ee::Ptr<ModelEntry> entry, uint32_t /*context: not read*/)
    {
        releaseModelEntryAsset(entry);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, releaseEntryAssetVisit);

    inline void releaseModelEntryAsset(ee::Ptr<ModelEntry> entry)
    {
        if (!entry->loaded)
            return;
        releaseLibraryAsset(entry->loaded);
        entry->loaded = ee::Ptr<LibraryAsset>();
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, releaseModelEntryAsset);

    // modelAspectChanged: the aspect's change counted; a change of the parts or the skeleton is one of the
    // lattices' too (the shape is bound to the mesh).
    inline void modelAspectChanged(ee::Ptr<ModelRecord> model, uint32_t aspect)
    {
        ++model->aspectChanges[aspect];
        if (aspect == kModelParts || aspect == kModelSkeleton)
            ++model->aspectChanges[kModelLattices];
    }
    READABLE_FUNCTION_ONLY(bmx, modelAspectChanged);

    // modelValueChanged (the values' watcher): a weight set is kept clamped in the record's field; a weight
    // removed is put back at the field's value, so the record always has its three.
    inline void modelValueChanged(ee::Ptr<ScriptValueSet> values, ee::Ptr<ScriptValue> value, uint32_t event, ee::Ptr<ModelRecord> model)
    {
        constexpr uint32_t kWeightKeys[kModelWeights] = {sym::bmx::modelWeightKey, sym::bmx::modelWeight01Key, sym::bmx::modelWeight02Key};
        for (uint32_t weight = 0; weight < kModelWeights; ++weight)
        {
            if (stringCompareNoCase(scriptValueKey(value), kWeightKeys[weight]) != 0)
                continue;
            if (event == kScriptValueChanged)
                model->weights[weight] = clampedWeight(value);
            else if (event == kScriptValueRemoved)
                game::tail(setScriptValueFloat, addScriptValue(values, text(kWeightKeys[weight]), kScriptValueFloat), model->weights[weight]);
            return;
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, modelValueChanged);

    // modelEntryAsset: the entry's asset's data, its load asked for the first time (0 until it is in).
    inline uint32_t modelEntryAsset(ee::Ptr<ModelEntry> entry)
    {
        if (!entry->loaded)
            entry->loaded = loadLibraryAsset(libraryPath(entry->library), ee::Ptr<const char>(entry->asset));
        return libraryAssetData(entry->loaded);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, modelEntryAsset);

    inline ee::Ptr<SearchTree> modelAspect(ee::Ptr<ModelRecord> model, uint32_t aspect)
    {
        return &model->aspects[aspect];
    }
    READABLE_FUNCTION_ONLY(bmx, modelAspect);

    inline void freeBindingVertexWeights(ee::Ptr<ModelBinding> binding)
    {
        if (!binding->vertexWeights)
            return;
        freeFfdVertexWeights(binding->vertexWeights);
        binding->vertexWeights = ee::Ptr<FfdVertexWeights>();
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeBindingVertexWeights);

    // modelObjectDeleted (the object's deleting message): its binding gone.
    inline int32_t modelObjectDeleted(ee::Ptr<EngineMessage> message)
    {
        unbindObjectModel(ee::Ptr<SceneObject>(message->target));
        return 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, modelObjectDeleted);

    // copyModelEntryTo (copyModelRecord's visit): the entry made again in the destination (user).
    inline int32_t copyModelEntryTo(ee::Ptr<ModelRecord> /*model: the source*/, uint32_t aspect, ee::Ptr<const char> library,
                                    ee::Ptr<const char> asset, int32_t rank, ee::Ptr<ModelRecord> destination)
    {
        newModelEntry(modelAspect(destination, aspect), library, asset, rank);
        modelAspectChanged(destination, aspect);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, copyModelEntryTo);

    // copyModelValueTo (copyModelRecord's visit): the value made again in the destination's values.
    inline int32_t copyModelValueTo(ee::Ptr<ScriptValue> value, ee::Ptr<ScriptValueSet> destination)
    {
        const ee::Ptr<char> key = scriptValueKey(value);
        const uint32_t kind = scriptValueKind(value);
        copyScriptValue(addScriptValue(destination, key, kind), value);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, copyModelValueTo);

    // What of the model the binding has not built yet: the mesh (the parts or the skeleton changed), the shape
    // (the lattices or a weight), an aspect.
    inline uint32_t bindingMeshOutOfDate(ee::Ptr<ModelBinding> binding)
    {
        const ee::Ptr<ModelRecord> model = binding->model;
        return model->aspectChanges[kModelParts] != binding->aspectChanges[kModelParts] ||
               model->aspectChanges[kModelSkeleton] != binding->aspectChanges[kModelSkeleton];
    }
    READABLE_FUNCTION_ONLY(bmx, bindingMeshOutOfDate);
    inline uint32_t bindingShapeOutOfDate(ee::Ptr<ModelBinding> binding)
    {
        const ee::Ptr<ModelRecord> model = binding->model;
        return !(model->weights[0] == binding->weights[0] && model->aspectChanges[kModelLattices] == binding->aspectChanges[kModelLattices] &&
                 model->weights[1] == binding->weights[1] && model->weights[2] == binding->weights[2]);
    }
    READABLE_FUNCTION_ONLY(bmx, bindingShapeOutOfDate);
    inline uint32_t bindingAspectOutOfDate(ee::Ptr<ModelBinding> binding, uint32_t aspect)
    {
        return binding->model->aspectChanges[aspect] != binding->aspectChanges[aspect];
    }
    READABLE_FUNCTION_ONLY(bmx, bindingAspectOutOfDate);

    // entryAssetLoadedVisit (a visit): the walk stopped at an entry whose asset is not in yet.
    inline int32_t entryAssetLoadedVisit(ee::Ptr<ModelEntry> entry, ee::Ptr<uint32_t> loaded)
    {
        if (!modelEntryAsset(entry))
            *loaded = 0;
        return static_cast<int32_t>(*loaded);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, entryAssetLoadedVisit);

    // bindingAssetsLoaded: every asset of the model in (each asked for), aspect by aspect.
    inline uint32_t bindingAssetsLoaded(ee::Ptr<ModelBinding> binding)
    {
        for (uint32_t aspect = 0; aspect < kModelAspects; ++aspect)
        {
            if (!bindingAspectLoaded(binding, aspect))
                return 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, bindingAssetsLoaded);
    inline uint32_t bindingAspectLoaded(ee::Ptr<ModelBinding> binding, uint32_t aspect)
    {
        ee::Local<uint32_t> loaded;
        *loaded = 1;
        searchTreeForEach(modelAspect(binding->model, aspect), sym::bmx::entryAssetLoadedVisit, loaded.address());
        return *loaded;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, bindingAspectLoaded);

    // collectEntryAssetVisit (a visit): the entry's asset's data at the cursor, which moves on.
    inline int32_t collectEntryAssetVisit(ee::Ptr<ModelEntry> entry, ee::Ptr<ee::Ptr<uint32_t>> cursor)
    {
        **cursor = modelEntryAsset(entry);
        *cursor = *cursor + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, collectEntryAssetVisit);

    // rebuildBindingMesh: the object's scene made again from the model's parts merged over its skeleton (the
    // skeleton tree's top entry), the object's place and textures kept; 0 while an asset is not in. Nothing to do
    // without a skeleton or parts.
    inline uint32_t rebuildBindingMesh(ee::Ptr<ModelBinding> binding)
    {
        HeldPlace held;
        ee::Local<uint32_t[kModelTextureSlots]> textures;
        ee::Local<ee::Ptr<uint32_t>> cursor;
        const ee::Ptr<ModelRecord> model = binding->model;
        const ee::Ptr<SearchTree> partsTree = modelAspect(model, kModelParts);
        const ee::Ptr<SearchTree> skeletonTree = modelAspect(model, kModelSkeleton);
        if (skeletonTree->count == 0)
            return 1;
        const uint32_t count = static_cast<uint32_t>(partsTree->count);
        if (count == 0)
            return 1;
        const ee::Ptr<Scene> skeleton(modelEntryAsset(treeRecordOf<ModelEntry>(skeletonTree->root)));
        if (!skeleton)
            return 0;
        const ee::Ptr<ee::Ptr<Scene>> parts(heapAllocate(count * sizeof(uint32_t)));
        memoryFill(parts, 0, count * sizeof(uint32_t));
        *cursor = wordsOf(parts);
        searchTreeForEach(partsTree, sym::bmx::collectEntryAssetVisit, cursor.address());
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!parts[i])
            {
                heapFree(parts);
                return 0;
            }
        }
        const ee::Ptr<SceneObject> object = binding->object;
        holdPlace(object, held);
        keepObjectTextures(object, textures);
        unregisterLoadedAsset(kAssetModel, kModelSceneForm, object->scene, ee::Ptr<LoadedAsset>());
        releaseScene(object->scene);
        object->scene = mergeSceneMeshes(parts, count, skeleton);
        setSceneObject(object->scene, object);
        remakeObjectAnimator(object);
        restorePlace(object, held);
        restoreObjectTextures(object, textures);
        changeSceneState(object->scene, 0, kSceneShown);
        changeSceneState(object->scene, kSceneStateUnused, 0);
        heapFree(parts);
        freeBindingVertexWeights(binding);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, rebuildBindingMesh);

    // bindBindingLattice: the object's mesh bound to the base lattice (rank 50), at the origin with no turn; 0
    // with no base lattice or while it is not in.
    inline uint32_t bindBindingLattice(ee::Ptr<ModelBinding> binding)
    {
        HeldPlace origin;
        HeldPlace held;
        ee::Local<int32_t> rank;
        if (binding->vertexWeights)
            return 1;
        const ee::Ptr<SearchTree> lattices = modelAspect(binding->model, kModelLattices);
        const ee::Ptr<SceneObject> object = binding->object;
        const ee::Ptr<ZgePart> mesh = sceneFirstMesh(sceneOf(object));
        *rank = kBaseLatticeRank;
        const ee::Ptr<ModelEntry> base = treeRecordOf<ModelEntry>(searchTreeFind(lattices, rank));
        if (!base)
            return 0;
        const ee::Ptr<FfdLattice> lattice(modelEntryAsset(base));
        if (!lattice)
            return 0;
        holdPlace(object, held);
        // The origin's w is left as the frame had it (the original sets x, y and z).
        origin.position->x = origin.position->y = origin.position->z = ee::Float(0.0f);
        origin.orientation->x = origin.orientation->y = origin.orientation->z = origin.orientation->w = ee::Float(0.0f);
        restorePlace(object, origin);
        binding->vertexWeights = bindPartToFfd(mesh, lattice);
        restorePlace(object, held);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, bindBindingLattice);

    // blendLatticeByWeight: out the blend of low and mid (weight under 50) or of mid and high (over 50).
    inline void blendLatticeByWeight(ee::Ptr<FfdLattice> out, ee::Ptr<FfdLattice> low, ee::Ptr<FfdLattice> mid, ee::Ptr<FfdLattice> high,
                                     ee::Float weight)
    {
        ee::Float t = weight * kWeightShare;
        if (ee::Float(0.5f) < t)
        {
            low = mid;
            const ee::Float over = t - ee::Float(0.5f);
            t = over + over;
        }
        else
        {
            high = mid;
            t = t + t;
        }
        if (t < ee::Float(0.0f))
            t = ee::Float(0.0f);
        tail::blendFfdLattice(out, low, high, t);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, blendLatticeByWeight);

    // addLatticeByWeight: onto, moved by how the weight's blend (as blendLatticeByWeight's) differs from base.
    inline void addLatticeByWeight(ee::Ptr<FfdLattice> out, ee::Ptr<FfdLattice> low, ee::Ptr<FfdLattice> mid, ee::Ptr<FfdLattice> high,
                                   ee::Ptr<FfdLattice> base, ee::Ptr<FfdLattice> onto, ee::Float weight)
    {
        ee::Float t = weight * kWeightShare;
        if (ee::Float(0.5f) < t)
        {
            low = mid;
            const ee::Float over = t - ee::Float(0.5f);
            t = over + over;
        }
        else
        {
            high = mid;
            t = t + t;
        }
        if (t < ee::Float(0.0f))
            t = ee::Float(0.0f);
        tail::addFfdLatticeBlend(out, low, high, base, onto, t);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, addLatticeByWeight);

    // applyBindingShape: the object's mesh bent by its model's weights: weight01 blends the lattices of ranks
    // 200, 50 and 300, then weight02's blend of 400, 50 and 500 and weight's of 0, 50 and 100 add how they differ
    // from the base (50). Nothing to do with no lattices; 0 with fewer than three, or while one is not in.
    inline uint32_t applyBindingShape(ee::Ptr<ModelBinding> binding)
    {
        ee::Local<FfdLattice> dimensions;
        HeldPlace origin;
        HeldPlace held;
        ee::Local<int32_t> rank;
        const ee::Ptr<ModelRecord> model = binding->model;
        const ee::Ptr<SearchTree> lattices = modelAspect(model, kModelLattices);
        if (lattices->count == 0)
            return 1;
        if (lattices->count < 3)
            return 0;
        const ee::Ptr<SceneObject> object = binding->object;
        const ee::Ptr<ZgePart> mesh = sceneFirstMesh(sceneOf(object));
        if (!bindBindingLattice(binding))
            return 0;
        const auto byRank = [&](int32_t wanted) {
            *rank = wanted;
            return treeRecordOf<ModelEntry>(searchTreeFind(lattices, rank));
        };
        const ee::Ptr<ModelEntry> rank0 = byRank(0), rank50 = byRank(50), rank100 = byRank(100), rank200 = byRank(200),
                                  rank300 = byRank(300), rank400 = byRank(400), rank500 = byRank(500);
        const auto asset = [](ee::Ptr<ModelEntry> entry) { return ee::Ptr<FfdLattice>(modelEntryAsset(entry)); };
        const ee::Ptr<FfdLattice> lattice200 = asset(rank200);
        if (!lattice200)
            return 0;
        const ee::Ptr<FfdLattice> lattice300 = asset(rank300);
        if (!lattice300)
            return 0;
        const ee::Ptr<FfdLattice> lattice400 = asset(rank400);
        if (!lattice400)
            return 0;
        const ee::Ptr<FfdLattice> lattice500 = asset(rank500);
        if (!lattice500)
            return 0;
        const ee::Ptr<FfdLattice> lattice0 = asset(rank0);
        if (!lattice0)
            return 0;
        const ee::Ptr<FfdLattice> lattice100 = asset(rank100);
        if (!lattice100)
            return 0;
        const ee::Ptr<FfdLattice> base = asset(rank50);
        if (!base)
            return 0;
        copyFfdDimensions(lattice0, dimensions);
        const ee::Ptr<FfdLattice> shape = newFfdLattice(dimensions->countX, dimensions->countY, dimensions->countZ);
        holdPlace(object, held);
        // The origin's w is left as the frame had it (the original sets x, y and z).
        origin.position->x = origin.position->y = origin.position->z = ee::Float(0.0f);
        origin.orientation->x = origin.orientation->y = origin.orientation->z = origin.orientation->w = ee::Float(0.0f);
        restorePlace(object, origin);
        blendLatticeByWeight(shape, lattice200, base, lattice300, model->weights[1]);
        addLatticeByWeight(shape, lattice400, base, lattice500, base, shape, model->weights[2]);
        addLatticeByWeight(shape, lattice0, base, lattice100, base, shape, model->weights[0]);
        deformPartByFfd(mesh, binding->vertexWeights, shape);
        freeFfdLattice(shape);
        restorePlace(object, held);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, applyBindingShape);

    // addEntryImageLayer (a visit): the entry's image as the layer of its rank; the walk stopped while it is not
    // in.
    inline int32_t addEntryImageLayer(ee::Ptr<ModelEntry> entry, ee::Ptr<ImageLayers> layers)
    {
        const uint32_t image = modelEntryAsset(entry);
        if (!image)
            return 0;
        setImageLayer(layers, entry->rank, ee::Ptr<Image>(image));
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, addEntryImageLayer);

    // rebuildBindingTexture: every material of the object named for the texture slot given one new texture: the
    // slot's one image, or its images layered by rank and flattened (quantized to a palette when asked, else the
    // texture cache is told to flush). Each such material has its alpha flag set and its cull mode and repeats
    // rebuilt. Nothing to do with no images or no such material; 0 with no scene, or while an image is not in.
    inline uint32_t rebuildBindingTexture(ee::Ptr<ModelBinding> binding, uint32_t aspect, uint32_t quantize)
    {
        const ee::Ptr<SearchTree> images = modelAspect(binding->model, aspect);
        if (images->count == 0)
            return 1;
        const ee::Ptr<Node> root = sceneRootNode(sceneOf(binding->object));
        if (!root)
            return 0;
        const uint32_t hash = textureSlotNameHash(aspect);
        uint32_t count = 0;
        while (findNodeMaterialByHash(root, hash, count))
            ++count;
        if (count == 0)
            return 1;
        const ee::Ptr<ee::Ptr<Material>> materials(allocateMemory(count * sizeof(uint32_t)));
        for (uint32_t i = 0; i < count; ++i)
            materials[i] = findNodeMaterialByHash(root, hash, i);
        ee::Ptr<Texture> texture;
        if (images->count >= 2)
        {
            const ee::Ptr<ImageLayers> layers = newImageLayers();
            searchTreeForEach(images, sym::bmx::addEntryImageLayer, layers);
            if (imageLayerCount(layers) != images->count)
            {
                freeImageLayers(layers);
                freeMemory(materials);
                return 0;
            }
            const uint32_t width = imageLayersWidth(layers);
            const uint32_t height = imageLayersHeight(layers);
            constexpr uint32_t kPixelBytes = 4; // red, green, blue and alpha
            const ee::Ptr<Rgba8> pixels(allocateMemory(width * height * kPixelBytes));
            flattenImageLayers(layers, 0, pixels);
            freeImageLayers(layers);
            texture = ee::Ptr<Texture>(static_cast<uint32_t>(newTextureFromPixels(pixels, width, height, quantize)));
            freeMemory(pixels);
            if (!quantize)
                requestTextureCacheFlushThunk();
        }
        else
        {
            const uint32_t image = modelEntryAsset(treeRecordOf<ModelEntry>(images->root));
            if (!image)
            {
                freeMemory(materials);
                return 0;
            }
            texture = newProceduralTexture(ee::Ptr<TextureImage>(image));
        }
        for (uint32_t i = count; i != 0; --i)
        {
            const ee::Ptr<Material> material = materials[i - 1];
            setMaterialPicture(material, texture, 0);
            material->flags |= 1ull << kMaterialFlagTextureAlpha;
            zgui::rebuildMaterialRegisters(materials[i - 1], 0, kStateCullMode, 1);
            zgui::rebuildMaterialRegisters(materials[i - 1], 0, kStateRepeatU, 1);
            zgui::rebuildMaterialRegisters(materials[i - 1], 0, kStateRepeatV, 1);
        }
        freeMemory(materials);
        releaseTextureRef(texture);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, rebuildBindingTexture);

    // copyEntryNameVisit, copyEntryRankVisit (visits): the entry's asset name (0x20 bytes a name) or its rank
    // (a halfword) at the cursor, which moves on.
    inline int32_t copyEntryNameVisit(ee::Ptr<ModelEntry> entry, ee::Ptr<ee::Ptr<char>> cursor)
    {
        stringCopy(*cursor, entry->asset);
        *cursor = *cursor + sizeof(entry->asset);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, copyEntryNameVisit);
    inline int32_t copyEntryRankVisit(ee::Ptr<ModelEntry> entry, ee::Ptr<ee::Ptr<uint16_t>> cursor)
    {
        **cursor = static_cast<uint16_t>(entry->rank);
        *cursor = *cursor + 1;
        return 1;
    }
    READABLE_FUNCTION_ONLY(bmx, copyEntryRankVisit);

    // listModelKindEntries: the aspect's entries' asset names, and their ranks when asked for, in rank order.
    inline void listModelKindEntries(ee::Ptr<ModelRecord> model, ee::Ptr<char> names, ee::Ptr<uint16_t> ranks, uint32_t aspect)
    {
        ee::Local<ModelEntryLists> lists;
        lists->names = names;
        lists->ranks = ranks;
        const ee::Ptr<SearchTree> tree = modelAspect(model, aspect);
        searchTreeForEach(tree, sym::bmx::copyEntryNameVisit, &lists->names);
        if (lists->ranks)
            searchTreeForEach(tree, sym::bmx::copyEntryRankVisit, &lists->ranks);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, listModelKindEntries);

    // setModelKindEntries: the aspect emptied and given up to count named assets of the library (0x20 bytes a
    // name, an empty name ends them), each at its rank.
    inline void setModelKindEntries(ee::Ptr<ModelRecord> model, ee::Ptr<const char> names, ee::Ptr<const uint16_t> ranks, uint32_t aspect,
                                    ee::Ptr<const char> library, uint32_t count)
    {
        constexpr uint32_t kNameBytes = 0x20;
        clearModelAspect(model, aspect);
        for (uint32_t i = 0; i < count && names[i * kNameBytes] != 0; ++i)
            addModelEntry(model, aspect, library, names + i * kNameBytes, ranks[i]);
        game::tail(modelAspectChanged, model, aspect);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setModelKindEntries);

    // modelWeight: the weight's value (0 when the index is past the names or the model has no such value).
    inline ee::Float modelWeight(ee::Ptr<ModelRecord> model, uint8_t index)
    {
        ee::Float weight(0.0f);
        if (index >= kModelWeightNames)
            return weight;
        const ee::Ptr<ScriptValue> value = findScriptValue(model->values, ee::Ptr<ee::Ptr<const char>>(sym::bmx::g_modelWeightKeys)[index]);
        if (!value)
            return weight;
        return scriptValueFloat(value);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, modelWeight);

    // setModelWeight: the weight's value set (made when the model has none); 0 when the index is past the names.
    inline uint32_t setModelWeight(ee::Ptr<ModelRecord> model, uint8_t index, ee::Float weight)
    {
        if (index >= kModelWeightNames)
            return 0;
        const ee::Ptr<const char> key = ee::Ptr<ee::Ptr<const char>>(sym::bmx::g_modelWeightKeys)[index];
        ee::Ptr<ScriptValue> value = findScriptValue(model->values, key);
        if (!value)
        {
            value = addScriptValue(model->values, key, kScriptValueFloat);
            if (!value)
                return 0;
        }
        setScriptValueFloat(value, weight);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setModelWeight);

    // newModelRecord: an empty model with its three weights at 50. The first one sets up the bindings' tree and
    // the libraries' loaders of images, scenes and lattices.
    inline ee::Ptr<ModelRecord> newModelRecord()
    {
        if (*g_modelRecordsReady == 0)
        {
            initSearchTree(modelBindings(), sym::lessWord);
            registerLibraryAssetLoader(sym::bmx::loadImageAsset, sym::bmx::freeImageAsset);
            registerLibraryAssetLoader(sym::bmx::loadSceneAsset, sym::bmx::freeSceneAsset);
            registerLibraryAssetLoader(sym::bmx::loadLatticeAsset, sym::bmx::freeLatticeAsset);
            *g_modelRecordsReady = 1;
        }
        const ee::Ptr<ModelRecord> model(poolTakeCreating(poolSlot(kModelPool), sizeof(ModelRecord)));
        memoryFill(model, 0, sizeof(ModelRecord));
        for (uint32_t aspect = 0; aspect < kModelAspects; ++aspect)
            initSearchTree(modelAspect(model, aspect), sym::lessWord);
        model->values = newScriptValues();
        watchScriptValues(model->values, sym::bmx::modelValueChanged, model);
        setScriptValueFloat(addScriptValue(model->values, text(sym::bmx::modelWeightKey), kScriptValueFloat), ee::Float(kDefaultModelWeight));
        setScriptValueFloat(addScriptValue(model->values, text(sym::bmx::modelWeight01Key), kScriptValueFloat), ee::Float(kDefaultModelWeight));
        setScriptValueFloat(addScriptValue(model->values, text(sym::bmx::modelWeight02Key), kScriptValueFloat), ee::Float(kDefaultModelWeight));
        return model;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, newModelRecord);

    // freeModelRecord: the model's entries and values freed and the record back to the pool; the last one gone
    // (the pool with it), the loaders go too.
    inline void freeModelRecord(ee::Ptr<ModelRecord> model)
    {
        for (uint32_t aspect = 0; aspect < kModelAspects; ++aspect)
            clearModelAspect(model, aspect);
        freeModelValues(model);
        memoryFill(model, 0, sizeof(ModelRecord));
        poolGiveDestroying(poolSlot(kModelPool), model);
        if (*poolSlot(kModelPool))
            return;
        unregisterLibraryAssetLoader(sym::bmx::loadImageAsset, sym::bmx::freeImageAsset);
        unregisterLibraryAssetLoader(sym::bmx::loadSceneAsset, sym::bmx::freeSceneAsset);
        unregisterLibraryAssetLoader(sym::bmx::loadLatticeAsset, sym::bmx::freeLatticeAsset);
        *g_modelRecordsReady = 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeModelRecord);

    // copyModelRecord: the destination's entries and values given up, then the source's copied in (asked for
    // again when used). The values are copied before the destination's watcher is set, so its weights' fields
    // keep what they had.
    inline void copyModelRecord(ee::Ptr<ModelRecord> destination, ee::Ptr<ModelRecord> source)
    {
        for (uint32_t aspect = 0; aspect < kModelAspects; ++aspect)
            clearModelAspect(destination, aspect);
        freeModelValues(destination);
        for (uint32_t aspect = 0; aspect < kModelAspects; ++aspect)
            forEachModelEntry(source, aspect, sym::bmx::copyModelEntryTo, destination);
        destination->values = newScriptValues();
        forEachScriptValue(source->values, sym::bmx::copyModelValueTo, destination->values);
        game::tail(watchScriptValues, destination->values, ScriptValueWatcher(sym::bmx::modelValueChanged), destination);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, copyModelRecord);

    // bindingIsVisit (a visit): whether the binding is the search's object's and model's. Each binding visited
    // overwrites the answer, and the walk stops at the one that matches.
    inline int32_t bindingIsVisit(ee::Ptr<ModelBinding> binding, ee::Ptr<ModelBindingSearch> search)
    {
        search->found = binding->object == search->object && binding->model == search->model;
        return search->found ^ 1u;
    }
    READABLE_FUNCTION_ONLY(bmx, bindingIsVisit);

    inline uint32_t objectWearsModel(ee::Ptr<SceneObject> object, ee::Ptr<ModelRecord> model)
    {
        ee::Local<ModelBindingSearch> search;
        search->object = object;
        search->model = model;
        search->found = 0;
        searchTreeForEach(modelBindings(), sym::bmx::bindingIsVisit, search.address());
        return search->found;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, objectWearsModel);

    // bindModelToObject: the object wears the model from now on, with nothing built yet; its deleting message
    // unbinds it.
    inline void bindModelToObject(ee::Ptr<SceneObject> object, ee::Ptr<ModelRecord> model)
    {
        const ee::Ptr<ModelBinding> binding(poolTakeCreating(poolSlot(kModelBindingPool), sizeof(ModelBinding)));
        binding->object = object;
        binding->model = model;
        // The copy runs on over the weights, set just below.
        memoryCopy(binding->aspectChanges, model->aspectChanges,
                   sizeof(binding->aspectChanges) + sizeof(binding->weights));
        for (uint32_t aspect = 0; aspect < kModelAspects; ++aspect)
            --binding->aspectChanges[aspect];
        binding->vertexWeights = ee::Ptr<FfdVertexWeights>();
        for (uint32_t weight = 0; weight < kModelWeights; ++weight)
            binding->weights[weight] = ee::Float(-1.0f);
        subscribe(object, kDeletingMessage, sym::bmx::modelObjectDeleted);
        searchTreeInsert(modelBindings(), &binding->node);
        ++model->bindings;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, bindModelToObject);

    // unbindObjectModel: the object's binding gone (its deleting message's subscription stays); the model's last
    // binding gone, its assets are let go.
    inline void unbindObjectModel(ee::Ptr<SceneObject> object)
    {
        ee::Local<uint32_t> key;
        *key = ee::Address(object);
        const ee::Ptr<ModelBinding> binding = treeRecordOf<ModelBinding>(searchTreeRemove(modelBindings(), key));
        if (!binding)
            return;
        freeBindingVertexWeights(binding);
        if (--binding->model->bindings == 0)
        {
            for (uint32_t aspect = 0; aspect < kModelAspects; ++aspect)
                searchTreeForEach(modelAspect(binding->model, aspect), sym::bmx::releaseEntryAssetVisit, 0);
        }
        poolGiveDestroying(poolSlot(kModelBindingPool), binding);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, unbindObjectModel);

    // updateObjectModel: what of its model changed built on the object: the mesh, then the shape, then each
    // texture slot (every slot is tried; 0 when any failed). Unless told to skip it, 0 at once while an asset is
    // not in. 1 for an object wearing no model.
    inline uint32_t updateObjectModel(ee::Ptr<SceneObject> object, uint32_t skipLoadedCheck, uint32_t quantize)
    {
        ee::Local<uint32_t> key;
        *key = ee::Address(object);
        const ee::Ptr<ModelBinding> binding = treeRecordOf<ModelBinding>(searchTreeFind(modelBindings(), key));
        uint32_t built = 1;
        if (!binding)
            return built;
        const ee::Ptr<ModelRecord> model = binding->model;
        if (!skipLoadedCheck && !bindingAssetsLoaded(binding))
            return 0;
        if (bindingMeshOutOfDate(binding))
        {
            if (!rebuildBindingMesh(binding))
                built = 0;
            else
            {
                binding->aspectChanges[kModelParts] = model->aspectChanges[kModelParts];
                binding->aspectChanges[kModelSkeleton] = model->aspectChanges[kModelSkeleton];
            }
        }
        if (!built)
            return built;
        if (bindingShapeOutOfDate(binding))
        {
            if (applyBindingShape(binding) != 1)
                built = 0;
            else
            {
                binding->aspectChanges[kModelLattices] = model->aspectChanges[kModelLattices];
                for (uint32_t weight = 0; weight < kModelWeights; ++weight)
                    binding->weights[weight] = model->weights[weight];
            }
        }
        if (!built)
            return built;
        for (uint32_t aspect = kModelTextures; aspect < kModelAspects; ++aspect)
        {
            if (!bindingAspectOutOfDate(binding, aspect))
                continue;
            if (rebuildBindingTexture(binding, aspect, quantize) != 1)
                built = 0;
            else
                binding->aspectChanges[aspect] = model->aspectChanges[aspect];
        }
        return built;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, updateObjectModel);

    // addModelEntry: the library's asset added to the aspect at the rank (-1: after the last), in place of the
    // entry at that rank. Nothing when the aspect has the asset already.
    inline void addModelEntry(ee::Ptr<ModelRecord> model, uint32_t aspect, ee::Ptr<const char> library, ee::Ptr<const char> asset, int32_t rank)
    {
        ee::Local<ModelEntrySearch> search;
        ee::Local<int32_t> at;
        *at = rank;
        const ee::Ptr<SearchTree> tree = modelAspect(model, aspect);
        search->assetHash = nameHash(asset);
        search->asset = asset;
        search->found = ee::Ptr<ModelEntry>();
        searchTreeForEach(tree, sym::bmx::entryHasAssetName, search.address());
        if (search->found)
            return;
        if (*at == kRankAfterLast)
        {
            if (tree->count == 0)
                *at = 0;
            else
                *at = treeRecordOf<ModelEntry>(searchTreeLastBefore(tree, at))->rank + 1;
        }
        const ee::Ptr<ModelEntry> replaced = treeRecordOf<ModelEntry>(searchTreeFind(tree, at));
        if (replaced)
            removeModelEntry(model, aspect, ee::Ptr<const char>(replaced->asset));
        newModelEntry(tree, library, asset, *at);
        modelAspectChanged(model, aspect);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, addModelEntry);

    // removeModelEntry: the aspect's entry of the asset gone (its asset let go, its library's use given back).
    inline void removeModelEntry(ee::Ptr<ModelRecord> model, uint32_t aspect, ee::Ptr<const char> asset)
    {
        ee::Local<ModelEntrySearch> search;
        const ee::Ptr<SearchTree> tree = modelAspect(model, aspect);
        search->assetHash = nameHash(asset);
        search->asset = asset;
        search->found = ee::Ptr<ModelEntry>();
        searchTreeForEach(tree, sym::bmx::entryHasAssetName, search.address());
        const ee::Ptr<ModelEntry> entry = search->found;
        if (!entry)
            return;
        searchTreeRemove(tree, &entry->rank);
        releaseModelEntryAsset(search->found);
        releaseLibrary(search->found->library);
        poolGiveDestroying(poolSlot(kModelEntryPool), search->found);
        modelAspectChanged(model, aspect);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, removeModelEntry);

    // clearModelAspect: the aspect's entries removed, the tree's top one each time.
    inline void clearModelAspect(ee::Ptr<ModelRecord> model, uint32_t aspect)
    {
        const ee::Ptr<SearchTree> tree = modelAspect(model, aspect);
        while (tree->count != 0)
            removeModelEntry(model, aspect, ee::Ptr<const char>(treeRecordOf<ModelEntry>(tree->root)->asset));
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, clearModelAspect);

    inline ee::Ptr<ScriptValueSet> modelValues(ee::Ptr<ModelRecord> model) { return model->values; }
    READABLE_FUNCTION_ONLY(bmx, modelValues);

    // loadModelScript: the file's MODEL blocks read, a record made for each.
    inline void loadModelScript(uint32_t file)
    {
        registerModelBlocks();
        readScriptBlocks(file);
        game::tail(unregisterModelBlocks);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, loadModelScript);

    // forEachModelEntry: visit(model, aspect, library path, asset, rank, user) for each of the aspect's entries
    // in rank order, while it answers 1.
    inline int32_t forEachModelEntry(ee::Ptr<ModelRecord> model, uint32_t aspect, ModelEntryVisit visit, ee::Address user)
    {
        ee::Local<ModelEntryVisitContext> context;
        const ee::Ptr<SearchTree> tree = modelAspect(model, aspect);
        context->model = model;
        context->aspect = aspect;
        context->visit = visit;
        context->user = user;
        return searchTreeForEach(tree, sym::bmx::visitModelEntry, context.address());
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, forEachModelEntry);

    // findModelByName: the model of that name (case aside), the newest first; 0 when none.
    inline ee::Ptr<ModelRecord> findModelByName(ee::Ptr<const char> name)
    {
        const ee::Ptr<Pool> pool = *poolSlot(kModelPool);
        if (!pool)
            return ee::Ptr<ModelRecord>();
        for (ee::Ptr<ListNode> link = firstPoolLink(pool); link; link = nextPoolLink(link))
        {
            const ee::Ptr<ModelRecord> model = poolRecordAs<ModelRecord>(link);
            if (stringCompareNoCase(model, name) == 0)
                return model;
        }
        return ee::Ptr<ModelRecord>();
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, findModelByName);

    // keepObjectTextures: each texture slot's first material's texture, held (0 for a slot with no material or
    // no texture).
    inline void keepObjectTextures(ee::Ptr<SceneObject> object, ee::Ptr<uint32_t> textures)
    {
        const ee::Ptr<Node> root = sceneRootNode(sceneOf(object));
        for (uint32_t aspect = kModelTextures; aspect < kModelAspects; ++aspect, textures = textures + 1)
        {
            const ee::Ptr<Material> material = findNodeMaterialByHash(root, textureSlotNameHash(aspect), 0);
            if (!material)
            {
                *textures = 0;
                continue;
            }
            *textures = pictureLevel(material, 0);
            if (*textures)
                retainTexture(ee::Ptr<Texture>(*textures));
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, keepObjectTextures);

    // restoreObjectTextures: each slot's kept texture given back to all its materials (their cull mode rebuilt)
    // and let go.
    inline uint32_t restoreObjectTextures(ee::Ptr<SceneObject> object, ee::Ptr<const uint32_t> textures)
    {
        const ee::Ptr<Node> root = sceneRootNode(sceneOf(object));
        for (uint32_t aspect = kModelTextures; aspect < kModelAspects; ++aspect)
        {
            const uint32_t slot = aspect - kModelTextures;
            const uint32_t hash = textureSlotNameHash(aspect);
            for (uint32_t i = 0;; ++i)
            {
                const ee::Ptr<Material> material = findNodeMaterialByHash(root, hash, i);
                if (!material)
                    break;
                if (textures[slot])
                {
                    setMaterialPicture(material, textures[slot], 0);
                    zgui::rebuildMaterialRegisters(material, 0, kStateCullMode, 1);
                }
            }
            if (textures[slot])
                releaseTextureRef(ee::Ptr<Texture>(textures[slot]));
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, restoreObjectTextures);
}
