// How BMX XXX makes a level object's engine object (the create hook of
// most of its level object types) and finds its model. BMX XXX's own code
// (its game block); names from what each function does.

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/types.h"
#include "zge/memory/pool.h"
#include "zge/scene/locators.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/level/entity_models.cpp.h"

namespace game::bmx
{
    void setNodeTagTable(ee::Ptr<zge::ScenePoint> point, ee::Ptr<zge::Node> rootNode);
    void setObjectTagsFromPoint(ee::Ptr<zge::ScenePoint> point, ee::Ptr<zge::SceneObject> object);
    ee::Ptr<zge::Scene> cachedEntityModel(uint32_t nameHash);
    ee::Ptr<zge::SceneObject> entityModelObject(ee::Ptr<char> name);
    ee::Ptr<zge::SceneObject> modelObjectFromFile(ee::Ptr<char> fileName);
    ee::Ptr<zge::SceneObject> modelObjectFromLibrary(ee::Ptr<char> libraryName, ee::Ptr<char> entryName);

    namespace
    {
        // What the engine asks a type's create hook for (its spawnLevelObjects).
        struct SpawnRequest
        {
            uint32_t from;      // +0x00 SpawnFrom
            ee::Address source; // +0x04 the node, the spawn point or the object
        };
        enum SpawnFrom : uint32_t
        {
            kSpawnFromNode = 0,   // a node of the level
            kSpawnFromPoint = 1,  // a spawn point (a locator's record)
            kSpawnFromObject = 2, // an object that exists already
        };
        // The one kind of spawn point made as a bare object with no model: "SoundEmitter"'s name hash.
        constexpr uint32_t kSoundEmitterKind = 0x0D9A3A22u; // not an address
        constexpr uint32_t kModelNameTag = 0;               // a point's tags: text, its model's name
        constexpr uint32_t kTagText = 0x10;                 // the tag accessors' second argument (the value's kind)

        // The game's files (Aggressive Inline's line), by address.
        // Scaffolding (address-call): plain calls once BMX XXX's are written.
        uint32_t openZfileLibrary(ee::Address name, uint32_t flags) { return ee::game<uint32_t>(sym::openZfileLibrary, name, flags); }
        void closeZfileLibrary(uint32_t library, uint32_t flags) { ee::game<void>(sym::closeZfileLibrary, library, flags); }
    }

    // createEntity (the create hook of the plain level object types): the
    // engine object for a node of the level (the engine's plain entity),
    // for a spawn point (its model by the name in its tags, the point's tags
    // and name given to the model's root node; a "SoundEmitter" point is a
    // bare object holding the tags), placed and turned as the point is; or
    // the object handed in. 0 when the model is not found.
    ee::Ptr<zge::SceneObject> createEntity(ee::Ptr<SpawnRequest> request)
    {
        ee::Local<char[0x80]> modelName;
        ee::Local<char> unused;
        if (request->from == kSpawnFromPoint)
        {
            const ee::Ptr<zge::ScenePoint> point(request->source);
            ee::Ptr<zge::SceneObject> object;
            if (point->kindHash == kSoundEmitterKind)
            {
                object = zge::newSceneObject();
                setObjectTagsFromPoint(point, object);
            }
            else
            {
                zge::tagString(point->tags, kTagText, kModelNameTag, modelName.address());
                object = entityModelObject(ee::Ptr<char>(modelName.address()));
                if (!object)
                    return object;
                const ee::Ptr<zge::Node> root = zge::sceneRootNode(zge::sceneOf(object));
                setNodeTagTable(point, root);
                zge::setNodeNameHash(root, point->nameHash);
            }
            zge::setObjectPosition(object, &point->position);
            zge::setObjectOrientation(object, &point->orientation);
            return object;
        }
        if (request->from == kSpawnFromNode)
            return zge::createEntityObject(ee::Ptr<zge::Node>(request->source));
        if (request->from == kSpawnFromObject)
            return ee::Ptr<zge::SceneObject>(request->source);
        *unused = 0;
        return ee::Ptr<zge::SceneObject>();
    }
    READABLE_FUNCTION_WAITS(createEntity, Verified);

    namespace
    {
        // A model loaded from the entity model library, kept for the next object of its name.
        struct CachedModel
        {
            uint32_t nameHash;         // +0x00
            ee::Ptr<zge::Scene> model; // +0x04
        };
        // The cache (made by the first model kept), and the library the
        // models come from (0: none open).
        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_entityModelPool> g_entityModelPool;
        inline ee::Global<uint32_t, sym::g_entityModelLibrary> g_entityModelLibrary;
        // What poolTakeCreating is given for a record's size.
        constexpr uint32_t kCachedModelTake = 4;
    }

    // freeEntityModelCache: the cache's pool destroyed, when there is one.
    void freeEntityModelCache()
    {
        if (!*g_entityModelPool)
            return;
        zge::destroyPool(*g_entityModelPool);
        *g_entityModelPool = ee::Ptr<zge::Pool>();
    }
    READABLE_FUNCTION_WAITS(freeEntityModelCache, Verified);

    // cachedEntityModel: the model kept under the name's hash, or 0.
    ee::Ptr<zge::Scene> cachedEntityModel(uint32_t nameHash)
    {
        if (!*g_entityModelPool)
            return ee::Ptr<zge::Scene>();
        for (uint32_t link = zge::poolFirst(g_entityModelPool->cast<zge::PoolList>()); link != 0; link = zge::poolNext(ee::Ptr<zge::PoolLink>(link)))
        {
            const ee::Ptr<CachedModel> cached = zge::poolRecordAs<CachedModel>(link);
            if (cached->nameHash == nameHash)
                return cached->model;
        }
        return ee::Ptr<zge::Scene>();
    }
    READABLE_FUNCTION(cachedEntityModel, Verified);

    // entityModelObject: a model object for the name. With the entity model
    // library open: a clone of the model kept under the name, or the
    // library's entry of that name, loaded and kept. Failing that, a name
    // "library!entry" is that entry of that library file (the '!' in the
    // name is overwritten with '0'), and a name of four characters or more
    // is taken as a model file when its end has any character of ".ZIT" in
    // its place (the original's test: one match is enough). 0: no model.
    ee::Ptr<zge::SceneObject> entityModelObject(ee::Ptr<char> name)
    {
        ee::Local<char[0x80]> libraryName;
        ee::Local<char[0x80]> entryName;
        if (*g_entityModelLibrary != 0)
        {
            ee::Ptr<zge::Scene> model = cachedEntityModel(zge::nameHash(name));
            if (model)
            {
                model = zge::cloneModelScene(model);
            }
            else
            {
                const ee::Ptr<CachedModel> kept(zge::poolTakeCreating(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_entityModelPool), kCachedModelTake));
                model = ee::Ptr<zge::Scene>(zge::loadLibraryModelEntry(*g_entityModelLibrary, zge::nameHash(name)));
                kept->nameHash = zge::nameHash(name);
                kept->model = model;
            }
            if (model)
                return zge::newModelObject(model, 0);
        }
        const ee::Ptr<char> mark(zge::stringFind(name, '!'));
        if (mark)
        {
            const uint32_t libraryLength = static_cast<uint32_t>(ee::Address(mark) - ee::Address(name));
            *mark = '0';
            zge::stringCopyN(libraryName, name, libraryLength);
            libraryName[libraryLength] = 0;
            zge::stringCopy(entryName, mark + 1);
            return modelObjectFromLibrary(ee::Ptr<char>(libraryName.address()), ee::Ptr<char>(entryName.address()));
        }
        const uint32_t length = zge::stringLength(name);
        if (length < 4)
            return ee::Ptr<zge::SceneObject>();
        const ee::Ptr<char> end = name + length;
        if (end[-4] == '.' || end[-3] == 'Z' || end[-2] == 'I' || end[-1] == 'T')
            return modelObjectFromFile(name);
        return ee::Ptr<zge::SceneObject>();
    }
    READABLE_FUNCTION_WAITS(entityModelObject, Verified);

    // modelObjectFromFile: an object of the model file, 0 when it does not load.
    ee::Ptr<zge::SceneObject> modelObjectFromFile(ee::Ptr<char> fileName)
    {
        const ee::Ptr<zge::Scene> model(zge::loadModelFile(fileName));
        if (!model)
            return ee::Ptr<zge::SceneObject>();
        return zge::newModelObject(model, 0);
    }
    READABLE_FUNCTION_WAITS(modelObjectFromFile, Draft);

    // modelObjectFromLibrary: an object of the named entry of a library
    // file, 0 when the library does not open.
    ee::Ptr<zge::SceneObject> modelObjectFromLibrary(ee::Ptr<char> libraryName, ee::Ptr<char> entryName)
    {
        const uint32_t library = openZfileLibrary(libraryName, 0);
        if (library == 0)
            return ee::Ptr<zge::SceneObject>();
        const ee::Ptr<zge::Scene> model(zge::loadLibraryModelEntry(library, zge::nameHash(entryName)));
        closeZfileLibrary(library, 0);
        return zge::newModelObject(model, 0);
    }
    READABLE_FUNCTION_WAITS(modelObjectFromLibrary, Draft);
}
