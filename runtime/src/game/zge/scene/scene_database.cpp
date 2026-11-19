// The scene database (zScene.c, newSceneEntry .. lzDecompressOldAlias; scene_database.h):
// Scene.Txt parsed into its entries, the entries found by kind and id, the
// scene entered and left, and the loaded assets' records with the model,
// animation library, light, camera and particle emitter makers.
//
// Scene.Txt is read with zge's tokenizer (ztoken.c, beginScript ..: the
// script's #defines and $variables are resolved there): SCENE_COMMON { ... }
// first, then SCENE { ... } sections until END. A section holds NAME, FILE,
// TYPE, SUBTYPE and nested MODEL { ... } / ANIM { ... } entries.

#include "scene_database.h"

#include "scene.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "zge/collision/collision.h"
#include "zge/memory/pool.h"
#include "zge/text/strings.h"
#include "zge/text/cstring.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/chunk_reader.h"
#include "zge/assets/file.h"
#include "zge/collision/dynamics.h"
#include "zge/scene/animation.h"
#include "zge/scene/particle_types.h"
#include "zge/game.h"
#include "zge/platform/platform.h"
#include "zge/memory/regions.h"
#include "zge/scene/camera.h"
#include "zge/scene/locators.h"
#include "zge/text/token_reader.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/scene_database.cpp.h"

namespace zge
{

    namespace
    {
        // The module's globals (0x8C bytes from g_sceneDatabaseReady, cleared by
        // initSceneDatabase).
        inline ee::Global<int32_t, sym::g_sceneDatabaseReady> g_sceneDatabaseReady;
        inline ee::Global<uint32_t, sym::g_loadedAssetPool> g_loadedAssetPool; // LoadedAsset records, 256 a block
        inline ee::Global<uint32_t, sym::g_sceneEntryPool> g_sceneEntryPool;  // SceneEntry records, 256 a block
        // The first SCENE read (the others in its next list).
        inline ee::Global<ee::Ptr<SceneEntry>, sym::g_firstScene> g_firstScene;
        // The scene entered (0: none; g_unlistedScene for one made without
        // an entry).
        inline ee::Global<ee::Ptr<SceneEntry>, sym::g_currentScene> g_currentScene;
        inline ee::Global<SceneEntry, sym::g_sceneCommon> g_sceneCommon; // SCENE_COMMON
        constexpr uint32_t kSceneDatabaseBytes = 0x8C;
        static_assert(g_sceneCommon.address + sizeof(SceneEntry) == g_sceneDatabaseReady.address + kSceneDatabaseBytes);
        // The scene enterUnlistedScene enters (an entry no file fills).
        inline ee::Global<SceneEntry, sym::g_unlistedScene> g_unlistedScene;
        // The loaders by kind (ELF data): SCENE none, MODEL ZIT
        // (loadZitModel) and ZAL (loadLibraryModel), ANIM NAD and IFF
        // (loadAnimationFile) and ZAL (loadAnimationLibrary).
        inline ee::Global<AssetKindLoaders[3], sym::g_assetKindLoaders> g_assetKindLoaders;

        // Scene.Txt's keywords (ELF strings).
        constexpr uint32_t kOpenBrace = sym::openBrace;    // "{" (copied to the stack: expectToken writes it upper case)
        constexpr uint32_t kCloseBrace = sym::closeBrace;   // "}"
        constexpr uint32_t kKeywordName = sym::keywordName;  // "NAME"
        constexpr uint32_t kKeywordFile = sym::keywordFile;  // "FILE"
        constexpr uint32_t kKeywordType = sym::keywordType;  // "TYPE"
        constexpr uint32_t kKeywordSubtype = sym::keywordSubtype; // "SUBTYPE"
        constexpr uint32_t kKeywordAnim = sym::keywordAnim;  // "ANIM"
        constexpr uint32_t kKeywordModel = sym::keywordModel; // "MODEL"
        constexpr uint32_t kKeywordScene = sym::keywordScene; // "SCENE"
        constexpr uint32_t kKeywordSceneCommon = sym::keywordSceneCommon; // "SCENE_COMMON"
        constexpr uint32_t kKeywordEnd = sym::keywordEnd;   // "END"
        constexpr uint32_t kKeywordsHigh = ee::high(sym::keywordAnim); // %hi of the keywords, as the parsers keep it
        constexpr uint32_t kDefaultSceneFile = sym::defaultSceneFile; // "Scene.Txt"
        // newSceneModelByHash's name for the model (newSceneModel reads no
        // name in this build).
        // " fname:\MOTION.%d": a library item's name, after its memory file's.
        constexpr uint32_t kLibraryMotionName = sym::libraryMotionName;
        // A scene's chunk file's tag ('FSCN', its chunks handled by the
        // world's readers).
        constexpr uint32_t kSceneChunkTag = 0x4E435346u;
        // An animation file's IFF form ('FORM').
        constexpr uint32_t kFormTag = 0x4D524F46u;

        // Files (zfile.c) and the asset library (zassetLib.c).
        // A packed animation library is read through the game's zfile library
        // (zge/game.h: items read whole, decompressed by zcompres.c).
        constexpr uint32_t kStrcat = sym::appendString;             // (dst, src)

        // Chunk files (ziff.c) and the animations (zanim.c).
        // (hook, data) -> the old hook: hook(motion, data) for each motion
        // the animation chunks' reader makes.

        // The models' module (zobjModel.c) and the others'.
        constexpr uint32_t kNewCameraOf = sym::newSceneCameraWith;         // (setting) -> a camera object of the second kind
        // The collision world (zge/collision).

        constexpr uint32_t kCreatePool = sym::createPool;  // (record bytes, records a block, free callback) -> pool
        constexpr uint32_t kDestroyPool = sym::destroyPool; // (pool): its records freed (the callback first), then it
        constexpr uint32_t kPoolForEach = sym::poolForEach; // (pool, visit, data): visit(record, data) until it returns 0

        // This module's functions as the originals call them, by address.
        constexpr uint32_t kNewSceneEntry = sym::newSceneEntry;
        constexpr uint32_t kFreeSceneEntry = sym::freeSceneEntry;
        constexpr uint32_t kInsertSceneEntryAfter = sym::insertSceneEntryAfter;
        constexpr uint32_t kAddSceneEntryChild = sym::addSceneEntryChild;
        constexpr uint32_t kParseSceneEntry = sym::parseSceneEntry;
        constexpr uint32_t kLoadSceneDatabase = sym::loadSceneDatabase;
        constexpr uint32_t kSearchSceneEntries = sym::searchSceneEntries;
        constexpr uint32_t kFindSceneEntry = sym::findSceneEntry;
        constexpr uint32_t kMatchLoadedAsset = sym::matchLoadedAsset;
        constexpr uint32_t kFindLoadedAsset = sym::findLoadedAsset;
        constexpr uint32_t kRegisterLoadedAsset = sym::registerLoadedAsset;
        constexpr uint32_t kUnregisterLoadedAsset = sym::unregisterLoadedAsset;
        constexpr uint32_t kReleaseSceneAssets = sym::releaseSceneAssets;
        constexpr uint32_t kFindAssetLoader = sym::findAssetLoader;
        constexpr uint32_t kAddMotion = sym::addMotion;
        constexpr uint32_t kReadMotions = sym::readMotions;
        constexpr uint32_t kLeaveScene = sym::leaveScene;
        constexpr uint32_t kReleaseSceneModel = sym::releaseSceneModel;
        constexpr uint32_t kReleaseAnimations = sym::releaseAnimations;
        constexpr uint32_t kReleaseSceneCamera = sym::releaseSceneCamera;
        constexpr uint32_t kReleaseSceneLight = sym::releaseSceneLight;
        constexpr uint32_t kReleaseSceneEmitter = sym::releaseSceneEmitter;

        // A maker's record: the object registered (no entry, form 0) unless
        // the caller keeps it itself (kKeepAsset: not registered at all).
        template <typename Object>
        Object registerMadeObject(uint32_t flags, Object object, uint32_t kind)
        {
            if (object)
            {
                if (flags & kKeepAsset)
                    return object;
                registerLoadedAsset(0, 0, kind, 0, object READABLE_PARAM_EXCEPT(dm2, 1));
            }
            return object;
        }
    }

    // ---- the entries ---------------------------------------------------

    // newSceneEntry: a cleared entry of `kind` from the pool.
    ee::Ptr<SceneEntry> newSceneEntry(uint32_t kind)
    {
        const ee::Ptr<SceneEntry> entry(poolAlloc(ee::Ptr<Pool>(*g_sceneEntryPool)));
        memoryFill(entry, 0, sizeof(SceneEntry));
        entry->kind = kind;
        return entry;
    }
    READABLE_FUNCTION_WAITS(newSceneEntry, Verified);

    // freeSceneEntry: the entry pool's free callback: the entry handed back. 1.
    uint32_t freeSceneEntry(uint32_t entry)
    {
        poolFree(ee::Ptr<Pool>(*g_sceneEntryPool), entry);
        return 1;
    }
    READABLE_FUNCTION(freeSceneEntry, Verified);

    // insertSceneEntryAfter: `added` linked right after `entry` (the scenes' list).
    ee::Ptr<SceneEntry> insertSceneEntryAfter(ee::Ptr<SceneEntry> entry, ee::Ptr<SceneEntry> added)
    {
        added->next = entry->next;
        entry->next = added;
        return entry;
    }
    READABLE_FUNCTION(insertSceneEntryAfter, Verified);

    // addSceneEntryChild: `child` made the parent's first child.
    ee::Ptr<SceneEntry> addSceneEntryChild(ee::Ptr<SceneEntry> parent, ee::Ptr<SceneEntry> child)
    {
        child->next = parent->firstChild;
        parent->firstChild = child;
        return parent;
    }
    READABLE_FUNCTION(addSceneEntryChild, Verified);

    // nextSceneEntry (nothing calls it): the entry's next.
    ee::Ptr<SceneEntry> nextSceneEntry(ee::Ptr<SceneEntry> entry) { return entry->next; }
    READABLE_FUNCTION(nextSceneEntry, Verified);

    // firstSceneEntryChild (nothing calls it): the entry's first child.
    ee::Ptr<SceneEntry> firstSceneEntryChild(ee::Ptr<SceneEntry> entry) { return entry->firstChild; }
    READABLE_FUNCTION(firstSceneEntryChild, Verified);

    namespace
    {
        // parseSceneEntry's locals: the token buffer and what it compares and reads.
        struct SceneSectionLocals
        {
            char token[0x80];
            char openBrace[0x10];
            char closeBrace[0x10];
            int32_t type;
            int32_t subtype;
        };
    }

    uint32_t parseSceneEntry(ee::Ptr<SceneEntry> entry);

    // parseSceneEntry: an entry's section read, from its "{" to its "}":
    // NAME (and the id hashed from it), FILE, TYPE, SUBTYPE, and each MODEL
    // or ANIM section inside it read into a new child (whether that read
    // worked is not looked at). A keyword whose value is missing is passed
    // over. 0 when the "{" or a token is missing, or a word is none of these
    // (the script's error report first); 1 at the "}". The "}" is an error
    // only for an entry at address 0 (the check of the entry's file's
    // address, always true here).
    uint32_t parseSceneEntry(ee::Ptr<SceneEntry> entry)
    {
        ee::Local<SceneSectionLocals> locals;
        char *token = locals->token;
        // The braces as two-byte strings (the word and its 0).
        locals->openBrace[0] = ee::Ptr<char>(kOpenBrace)[0];
        locals->openBrace[1] = ee::Ptr<char>(kOpenBrace)[1];
        locals->closeBrace[0] = ee::Ptr<char>(kCloseBrace)[0];
        locals->closeBrace[1] = ee::Ptr<char>(kCloseBrace)[1];
        if (!expectToken(locals->openBrace))
            return 0u;
        bool closed = false;
        for (;;)
        {
            if (!nextToken(token, kTokenBytes))
                return 0u;
            if (compareStrings(token, kKeywordName) == 0)
            {
                if (readStringToken(token, kTokenBytes))
                {
                    stringCopy(entry->name, token);
                    entry->id = nameHash(entry->name);
                }
            }
            else if (compareStrings(token, kKeywordFile) == 0)
            {
                if (readStringToken(token, kTokenBytes))
                    stringCopy(entry->file, token);
            }
            else if (compareStrings(token, kKeywordType) == 0)
            {
                if (readNumberToken(&locals->type))
                    entry->type = locals->type;
            }
            else if (compareStrings(token, kKeywordSubtype) == 0)
            {
                if (readNumberToken(&locals->subtype))
                    entry->subtype = locals->subtype;
            }
            else
            {
                uint32_t kind = kAssetAnimations;
                bool section = compareStrings(token, kKeywordAnim) == 0;
                if (!section && compareStrings(token, kKeywordModel) == 0)
                {
                    kind = kAssetModel;
                    section = true;
                }
                if (section)
                {
                    const ee::Ptr<SceneEntry> child = newSceneEntry(kind);
                    addSceneEntryChild(entry, child);
                    parseSceneEntry(child);
                }
                else
                {
                    bool accepted = false;
                    if (compareStrings(token, locals->closeBrace) == 0)
                    {
                        closed = true;
                        accepted = static_cast<bool>(entry);
                    }
                    if (!accepted)
                    {
                        reportTokenPosition();
                        return 0;
                    }
                }
            }
            if (closed)
                break;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(parseSceneEntry, Verified);

    // loadSceneDatabase (kLoadSceneDatabase): the database read from a script
    // file: SCENE_COMMON's section into g_sceneCommon, then each SCENE's into
    // a new entry (the first is g_firstScene, each later one linked right
    // after it) until END. 0 when the file is missing or a section fails;
    // a word other than SCENE or END ends the script and returns 0 with the
    // file left open and its data not freed (as the original does).
    uint32_t loadSceneDatabase(uint32_t path)
    {
        ee::Local<uint8_t[0x80]> token;
        ee::Local<uint32_t> bytesOut;
        const uint32_t text = loadFile(path, bytesOut);
        if (!text)
            return 0;
        const ee::Ptr<ZFile> file = openMemoryFile(text, *bytesOut);
        beginScript(filePath(file));
        // The script ended and the file let go.
        const auto finish = [&](uint32_t result) {
            endScript();
            closeFile(file, 0);
            freeLoadedFile(text);
            return result;
        };
        if (!expectToken(kKeywordSceneCommon) ||
            (!static_cast<int32_t>(parseSceneEntry(g_sceneCommon.get()))))
            return finish(0);
        for (;;)
        {
            if (!nextToken(token, kTokenBytes))
                return finish(0);
            if (compareStrings(token, kKeywordScene) == 0)
            {
                const ee::Ptr<SceneEntry> scene = newSceneEntry(kAssetScene);
                if (*g_firstScene)
                    insertSceneEntryAfter(*g_firstScene, scene);
                else
                    *g_firstScene = scene;
                if (!static_cast<int32_t>(parseSceneEntry(scene)))
                    return finish(0);
                continue;
            }
            if (compareStrings(token, kKeywordEnd) != 0)
            {
                endScript();
                return 0;
            }
            return finish(1);
        }
    }
    READABLE_FUNCTION_WAITS(loadSceneDatabase, Verified);

    // searchSceneEntries: the walk of findSceneEntry from `entry`: the entry
    // itself, its children, then the entries after it. 0 when the query
    // found its entry (the walk stops), else the entry.
    ee::Ptr<SceneEntry> searchSceneEntries(ee::Ptr<SceneEntry> entry, ee::Ptr<SceneEntryQuery> query);

    ee::Ptr<SceneEntry> searchSceneEntries(ee::Ptr<SceneEntry> entry, ee::Ptr<SceneEntryQuery> query)
    {
        if (entry->kind == query->kind)
        {
            int32_t wanted = static_cast<int32_t>(query->id);
            int32_t seen;
            if (wanted != 0)
                seen = static_cast<int32_t>(entry->id);
            else
            {
                wanted = -1;
                seen = --query->index;
            }
            if (seen == wanted)
            {
                query->found = entry;
                return ee::Ptr<SceneEntry>();
            }
        }
        if (const ee::Ptr<SceneEntry> child = entry->firstChild; child)
            if (!searchSceneEntries(child, query))
                return ee::Ptr<SceneEntry>();
        if (const ee::Ptr<SceneEntry> next = entry->next; next)
            return searchSceneEntries(next, query) ? entry : ee::Ptr<SceneEntry>();
        return entry;
    }
    READABLE_FUNCTION(searchSceneEntries, Verified);

    // findSceneEntry: the entry of `kind` with `id` (or, id 0, the index-th
    // one) under `first` (0: every SCENE) and, if not there and alsoCommon,
    // under SCENE_COMMON. 0 when none is.
    ee::Ptr<SceneEntry> findSceneEntry(ee::Ptr<SceneEntry> first, uint32_t kind, uint32_t id, int32_t index, uint32_t alsoCommon)
    {
        ee::Local<SceneEntryQuery> query;
        query->kind = kind;
        query->id = id;
        query->index = index;
        query->found = ee::Ptr<SceneEntry>();
        if (!first)
            first = *g_firstScene;
        if (first)
        {
            searchSceneEntries(first, query);
            if (query->found)
                return query->found;
        }
        if (alsoCommon)
            searchSceneEntries(g_sceneCommon.get(), query);
        return query->found;
    }
    READABLE_FUNCTION(findSceneEntry, Verified);

    // ---- the loaded assets ---------------------------------------------

    // matchLoadedAsset (findLoadedAsset's visit): 0 (the walk stops) when the
    // record is the query's kind and id, kept in the query.
    uint32_t matchLoadedAsset(ee::Ptr<LoadedAsset> record, ee::Ptr<LoadedAssetQuery> query)
    {
        if (record->kind == query->kind && record->id == query->id)
        {
            query->found = record;
            return 0;
        }
        return 1;
    }
    READABLE_FUNCTION(matchLoadedAsset, Verified);

    // findLoadedAsset: the loaded asset of `kind` with `id`, or 0.
    ee::Ptr<LoadedAsset> findLoadedAsset(uint32_t kind, uint32_t id)
    {
        ee::Local<LoadedAssetQuery> query;
        query->kind = kind;
        query->id = id;
        query->found = ee::Ptr<LoadedAsset>();
        poolForEach(ee::Ptr<Pool>(*g_loadedAssetPool), kMatchLoadedAsset, query);
        return query->found;
    }
    READABLE_FUNCTION_WAITS(findLoadedAsset, Verified);

    // registerLoadedAsset: an asset recorded (only while a scene is entered: else
    // 0). leftWithScene: released when the scene is left.
    // (ZGE2 takes no leftWithScene: every record it makes is left with the scene.)
    uint32_t registerLoadedAsset(ee::Ptr<SceneEntry> entry, uint32_t id, uint32_t kind, uint32_t form, ee::Address asset READABLE_PARAM_EXCEPT(dm2, uint32_t leftWithScene = 1))
    {
        if (!*g_currentScene)
            return 0;
        const uint32_t address = poolAlloc(ee::Ptr<Pool>(*g_loadedAssetPool));
        const ee::Ptr<LoadedAsset> record(address);
        record->kind = static_cast<uint8_t>(kind);
        record->form = static_cast<uint8_t>(form);
        record->entry = entry;
        record->id = id;
        record->asset = asset;
        record->flags = static_cast<uint16_t>((record->flags & ~kAssetLeftWithScene) | (leftWithScene & 1u));
        return address;
    }
    READABLE_FUNCTION_WAITS(registerLoadedAsset, Verified);

    // unregisterLoadedAsset: an asset's record handed back: `record`, or (0) the
    // first of `kind` holding `asset` (a model's of `form` too). 0 when none
    // does, or kind is not 1 .. 5.
    uint32_t unregisterLoadedAsset(uint32_t kind, uint32_t form, ee::Address asset, ee::Ptr<LoadedAsset> record)
    {
        const ee::Ptr<Pool> pool(*g_loadedAssetPool);
        if (!record)
        {
            bool found = false;
            for (ee::Ptr<ListNode> link = firstPoolLink(pool); link && !found; link = nextPoolLink(link))
            {
                record = poolRecordAs<LoadedAsset>(link);
                if (record->kind != kind)
                    continue;
                if (kind < kAssetAnimations && kind != kAssetModel)
                    return 0;
                if (kind > kAssetParticleEmitter)
                    return 0;
                const bool sameForm = kind != kAssetModel || record->form == form;
                if (sameForm && record->asset == asset)
                    found = true;
            }
            if (!found)
                return 0;
        }
        poolFree(pool, record);
        return 1;
    }
    READABLE_FUNCTION(unregisterLoadedAsset, Verified);

    uint32_t releaseAnimations(ee::Ptr<LoadedAsset> record);

    uint32_t releaseSceneLight(ee::Ptr<SceneObject> light);

    uint32_t releaseSceneEmitter(ee::Ptr<SceneObject> emitter);

    // releaseSceneAssets (kReleaseSceneAssets): the assets left with the scene
    // (every asset: all) released by kind, their records with them.
    void releaseSceneAssets(uint32_t all)
    {
        ee::Ptr<ListNode> link = firstPoolLink(ee::Ptr<Pool>(*g_loadedAssetPool));
        if (!link)
            return;
        for (;;)
        {
            const ee::Ptr<ListNode> next = nextPoolLink(link);
            const ee::Ptr<LoadedAsset> record = poolRecordAs<LoadedAsset>(link);
            if ((record->flags & kAssetLeftWithScene) || all)
            {
                switch (record->kind)
                {
                case kAssetModel:
                    if (record->form == 0)
                        releaseSceneModel(ee::Ptr<SceneObject>(record->asset));
                    else
                    {
                        releaseSceneAssetsScene(ee::Ptr<Scene>(record->asset));
                        unregisterLoadedAsset(record->kind, 1, record->asset, record);
                    }
                    break;
                case kAssetAnimations:
                    releaseAnimations(record);
                    break;
                case kAssetLight:
                    releaseSceneLight(ee::Ptr<SceneObject>(record->asset));
                    break;
                case kAssetCamera:
                    releaseSceneCamera(ee::Ptr<SceneObject>(record->asset));
                    break;
                case kAssetParticleEmitter:
                    releaseSceneEmitter(ee::Ptr<SceneObject>(record->asset));
                    break;
                default:
                    break;
                }
            }
            link = next;
            if (!link)
                break;
        }
    }
    READABLE_FUNCTION_WAITS(releaseSceneAssets, Verified);

    // findAssetLoader: the loader of `kind` for a file extension (upper case),
    // or 0.
    uint32_t findAssetLoader(uint32_t kind, ee::Address extension)
    {
        ee::Ptr<AssetLoader> loader = (*g_assetKindLoaders)[kind].loaders;
        if (!loader)
            return 0;
        for (; loader->extension; loader = loader + 1)
            if (compareStrings(loader->extension, extension) == 0)
                return loader->load;
        return 0;
    }
    READABLE_FUNCTION(findAssetLoader, Verified);

    // ---- the loaders ---------------------------------------------------

    // loadZitModel (MODEL's ZIT loader): the model read from its .ZIT file.
    int32_t loadZitModel(ee::Ptr<ModelLoadRequest> request)
    {
        const uint32_t model = loadModelFile(request->file);
        request->model = model;
        return model != 0;
    }
    READABLE_FUNCTION_WAITS(loadZitModel, Verified);

    // loadLibraryModel (MODEL's ZAL loader): the model of the request's id read
    // from an asset library.
    int32_t loadLibraryModel(ee::Ptr<ModelLoadRequest> request)
    {
        if (const uint32_t library = openAssetLibrary(request->file))
        {
            request->model = loadLibraryModelEntry(library, request->id);
            closeAssetLibrary(library);
        }
        return request->model != 0;
    }
    READABLE_FUNCTION_WAITS(loadLibraryModel, Verified);

    // addMotion (readMotions' hook): a motion read put first in the list.
    // 1.
    uint32_t addMotion(ee::Ptr<Motion> motion, ee::Ptr<ee::Ptr<MotionLink>> list)
    {
        const ee::Ptr<MotionLink> link(heapAllocate(sizeof(MotionLink)));
        link->motion = motion;
        link->id = motionId(motion);
        link->next = *list;
        *list = link;
        return 1;
    }
    READABLE_FUNCTION_WAITS(addMotion, Verified);

    // readMotions: the motions of an animation file's FORM added to `motions`
    // (the list, with them first).
    uint32_t readMotions(ee::Ptr<ZFile> file, uint32_t motions)
    {
        ee::Local<uint32_t> context;
        *context = motions;
        if (const ee::Ptr<ChunkReader> reader = openChunkFile(file, kFormTag, 0))
        {
            setMotionLoadedHook(kAddMotion, context);
            parseChunks(reader, 0);
            closeChunkFile(reader);
        }
        return *context;
    }
    READABLE_FUNCTION_WAITS(readMotions, Verified);

    // loadAnimationFile (ANIM's NAD and IFF loader): the file read whole and its
    // motions listed.
    int32_t loadAnimationFile(ee::Ptr<AnimationLoadRequest> request)
    {
        ee::Local<uint32_t> bytesOut;
        const uint32_t data = loadFile(request->file, bytesOut);
        if (!data)
            return 0;
        const ee::Ptr<ZFile> file = openMemoryFile(data, *bytesOut);
        request->motions = ee::Ptr<MotionLink>(readMotions(file, 0));
        closeFile(file, 0);
        freeLoadedFile(data);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadAnimationFile, Verified);

    // loadAnimationLibrary (ANIM's ZAL loader): every item of a packed library
    // read, opened as a memory file named after it (" fname:\MOTION.<n>",
    // counting down) and its motions listed.
    uint32_t loadAnimationLibrary(ee::Ptr<AnimationLoadRequest> request)
    {
        ee::Local<uint8_t[0xA0]> out;
        const ee::Ptr<ZfileLibrary> library = openZfileLibrary(request->file);
        if (!library)
            return 0;
        uint32_t motions = 0;
        int32_t items = zfileLibraryGroupCount(library);
        if (items != 0)
        {
            for (;;)
            {
                --items;
                const uint32_t bytes = zfileLibraryAssetBytes(library);
                const uint32_t data = heapAllocate(bytes);
                if (readZfileLibraryAsset(library, data))
                {
                    const ee::Ptr<ZFile> file = openMemoryFile(data, bytes);
                    formatString(out, kLibraryMotionName, items);
                    appendString(filePath(file), out);
                    motions = readMotions(file, motions);
                    closeFile(file, 0);
                }
                heapFree(data);
                nextZfileLibraryGroup(library);
                if (items == 0)
                    break;
            }
        }
        closeZfileLibrary(library);
        request->motions = ee::Ptr<MotionLink>(motions);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadAnimationLibrary, Verified);

    // ---- the module and the scene ----------------------------------------

    // initSceneDatabase: the module set up: its globals cleared, the two pools
    // (the entries' freed by freeSceneEntry), the database read from `path`
    // (0: Scene.Txt), then the models' set-up. 1 when all of it worked.
    int32_t initSceneDatabase(uint32_t path)
    {
        memoryFill(g_sceneDatabaseReady.get(), 0, kSceneDatabaseBytes);
        *g_loadedAssetPool = createPool(sizeof(LoadedAsset), 0x100, 0);
        *g_sceneEntryPool = createPool(sizeof(SceneEntry), 0x100, kFreeSceneEntry);
        if (!path)
            path = kDefaultSceneFile;
        if (!static_cast<int32_t>(loadSceneDatabase(path)))
            return 0;
        *g_sceneDatabaseReady = 1;
        const int32_t ready = registerScenePointChunkHandlers() ? 1 : 0;
        return ready;
    }
    READABLE_FUNCTION_WAITS(initSceneDatabase, Verified);

    // shutDownSceneDatabase: the module shut down: the scene left, every asset
    // released, the models' shutdown, the pools destroyed. 0 when a step
    // fails (the module left up), else 1.
    int32_t shutDownSceneDatabase()
    {
        if (*g_sceneDatabaseReady != 0)
        {
            if (!static_cast<int32_t>(leaveScene()))
                return 0;
            releaseSceneAssets(1);
            if (!shutdownScenePoints())
                return 0;
            destroyPool(ee::Ptr<Pool>(*g_loadedAssetPool));
            destroyPool(ee::Ptr<Pool>(*g_sceneEntryPool));
            *g_sceneDatabaseReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownSceneDatabase, Verified);

    // lookUpSceneEntry: an entry by id (or, id 0, by index): kind 0 among the
    // SCENEs, else in the scene entered and SCENE_COMMON.
    ee::Ptr<SceneEntry> lookUpSceneEntry(int32_t index, uint32_t kind, uint32_t id)
    {
        if (kind == kAssetScene)
        {
            return findSceneEntry(*g_firstScene, kAssetScene, id, index, 0);
        }
        return findSceneEntry(*g_currentScene, kind, id, index, 1);
    }
    READABLE_FUNCTION(lookUpSceneEntry, Verified);

    // enterScene: the SCENE named `name` entered (none entered yet): its
    // chunk file read (the collision world and the level's parts), the
    // world finished. 1 when it was; 0 when a scene is entered already, the
    // name is not a SCENE or the file is not read (the scene stays entered).
    uint32_t enterScene(uint32_t name)
    {
        if (*g_currentScene)
            return 0;
        const uint32_t id = nameHash(ee::Ptr<const char>(name));
        const ee::Ptr<SceneEntry> scene(findSceneEntry(0, kAssetScene, id, 0, 0));
        if (!scene)
            return 0;
        *g_currentScene = scene;
        if (!readChunkFile(scene->file, scene->file, kSceneChunkTag))
            return 0;
        finishCollisionWorld();
        // ZGE4 counts the next frame as the first again (its first frame places the world meshes' nodes).
        if constexpr (game::readable::kIsBmx)
            restartFrameRate();
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterScene, Verified);

    // addSceneFile: a SCENE's chunk file read into the scene entered (its
    // collision world kept: kKeepCollisionWorld while it is read), the world
    // finished again. 1 when it was read.
    int32_t addSceneFile(uint32_t name)
    {
        if (!*g_currentScene)
            return 0;
        const uint32_t id = nameHash(ee::Ptr<const char>(name));
        const ee::Ptr<SceneEntry> scene(findSceneEntry(0, kAssetScene, id, 0, 0));
        if (!scene)
            return 0;
        setSceneFileMode(1);
        if (!readChunkFile(scene->file, scene->file, kSceneChunkTag))
            return 0;
        finishCollisionWorld();
        setSceneFileMode(0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(addSceneFile, Verified);

    // enterUnlistedScene: a scene with no entry entered (g_unlistedScene), its
    // collision world made with the given cell size and bounds. 1 when it
    // was; 0 when a scene is entered already.
    int32_t enterUnlistedScene(ee::Address cellSize, ee::Address bounds)
    {
        if (*g_currentScene)
            return 0;
        *g_currentScene = g_unlistedScene.get();
        newCollisionWorld(ee::Ptr<Vec4>(cellSize), ee::Ptr<Box>(bounds));
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterUnlistedScene, Verified);

    // leaveScene (kLeaveScene): the scene left: the models told, the
    // assets left with it released, its collision world freed, the effects
    // reset. 1.
    uint32_t leaveScene()
    {
        if (*g_currentScene)
        {
            leaveModelScenes();
            releaseSceneAssets(0);
            releaseCollisionWorld(*g_collisionWorld);
            resetDynamicsDefinitions();
            // ZGE4 frees the particle emitters' definitions with the scene.
            if constexpr (game::readable::kIsBmx)
                clearEmitterList();
            *g_currentScene = ee::Ptr<SceneEntry>();
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(leaveScene, Verified);

    // ---- the assets' makers and their releases -----------------------------

    // newSceneModel: a model object of the MODEL entry `id`:
    // the model shared with a loaded one (its scene cloned), else loaded by
    // the entry's file's loader; the object recorded as a loaded asset
    // (left with the scene unless kKeepAsset). kModelNoObject: no object
    // (0 recorded and returned). 0 when the model is not found or not
    // loaded. `name` (the caller's, for messages) is read by nothing in
    // this build; newSceneModelByName is the by-name entry.
    ee::Ptr<SceneObject> newSceneModel(uint32_t flags, uint32_t id)
    {
        ee::Local<uint8_t[0x10]> extension;
        ee::Local<ModelLoadRequest> request;
        ee::Ptr<SceneEntry> entry;
        ee::Ptr<Scene> model;
        ee::Ptr<SceneObject> object;
        if (const ee::Ptr<LoadedAsset> loaded(findLoadedAsset(kAssetModel, id)); loaded)
        {
            if (loaded->form == 0)
                model = cloneModelScene(sceneOf(ee::Ptr<SceneObject>(loaded->asset)));
            else
                model = cloneModelScene(ee::Ptr<Scene>(loaded->asset));
            entry = loaded->entry;
        }
        else
        {
            entry = findSceneEntry(*g_currentScene, kAssetModel, id, 0, 1);
            if (!entry)
                return {};
            char *file = entry->file;
            pathExtension(file, extension);
            stringToUpper(extension);
            const uint32_t loader = findAssetLoader(kAssetModel, extension);
            if (!loader)
                return {};
            request->file = file;
            request->reserved = 0;
            request->model = 0;
            request->id = id;
            const int32_t loadedOk = ee::FunctionPtr<int32_t(ee::Ptr<ModelLoadRequest> request)>(loader)(request);
            model = ee::Ptr<Scene>(request->model);
            if (!loadedOk)
                return {};
        }
        if (!model)
            return {};
        if (!(flags & kModelNoObject))
        {
            object = newModelObject(model, 0);
            if (!object)
                return {};
        }
        registerLoadedAsset(entry, id, kAssetModel, 0, object READABLE_PARAM_EXCEPT(dm2, (flags ^ kKeepAsset) & 1u));
        return object;
    }

    // newSceneModelFromName: ZGE2's newSceneModel, which takes the model's
    // name (the id its hash; ZGE2 has no form by id): the same model object,
    // but a kKeepAsset one is not recorded at all (ZGE2's records have no
    // keep flag), and the loader's request has no id.
    ee::Ptr<SceneObject> newSceneModelFromName(uint32_t flags, ee::Ptr<const char> name)
    {
        ee::Local<uint8_t[0x10]> extension;
        ee::Local<ModelLoadRequest> request;
        const uint32_t id = nameHash(name);
        ee::Ptr<SceneEntry> entry;
        ee::Ptr<Scene> model;
        if (const ee::Ptr<LoadedAsset> loaded(findLoadedAsset(kAssetModel, id)); loaded)
        {
            if (loaded->form == 0)
                model = cloneModelScene(sceneOf(ee::Ptr<SceneObject>(loaded->asset)));
            else
                model = cloneModelScene(ee::Ptr<Scene>(loaded->asset));
            entry = loaded->entry;
        }
        else
        {
            entry = findSceneEntry(*g_currentScene, kAssetModel, id, 0, 1);
            if (!entry)
                return {};
            char *file = entry->file;
            pathExtension(file, extension);
            stringToUpper(extension);
            const uint32_t loader = findAssetLoader(kAssetModel, extension);
            if (!loader)
                return {};
            request->file = file;
            request->reserved = 0;
            request->model = 0;
            if (!ee::FunctionPtr<int32_t(ee::Ptr<ModelLoadRequest> request)>(loader)(request))
                return {};
            model = ee::Ptr<Scene>(request->model);
        }
        if (!model)
            return {};
        ee::Ptr<SceneObject> object;
        if (!(flags & kModelNoObject))
        {
            object = newModelObject(model, 0);
            if (!object)
                return {};
        }
        if (!(flags & kKeepAsset))
            registerLoadedAsset(entry, id, kAssetModel, 0, object READABLE_PARAM_EXCEPT(dm2, 1u));
        return object;
    }
    READABLE_FUNCTION_WAITS_OWN(newSceneModel, Verified, dm2, newSceneModelFromName);

    // newSceneModelByName: newSceneModel by name (its id the name's hash). The model
    // object, or 0.
    ee::Ptr<SceneObject> newSceneModelByName(uint32_t flags, ee::Ptr<const char> name)
    {
        const int32_t id = nameHash(name);
        return newSceneModel(flags, id);
    }
    READABLE_FUNCTION_WAITS(newSceneModelByName, Verified);

    // newSceneModelByHash: newSceneModel for a caller that has only the id ("_HASH_"
    // as the name). The model object, or 0.
    ee::Ptr<SceneObject> newSceneModelByHash(uint32_t flags, uint32_t id)
    {
        return newSceneModel(flags, id);
    }
    READABLE_FUNCTION_WAITS(newSceneModelByHash, Verified);

    // releaseSceneModel (kReleaseSceneModel): a model object's record handed back
    // (when it has a scene) and the object deleted. 1.
    uint32_t releaseSceneModel(ee::Ptr<SceneObject> object)
    {
        if (object->scene)
            unregisterLoadedAsset(kAssetModel, 0, object, 0);
        deleteSceneObject(object);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseSceneModel, Verified);

    // loadSceneAnimations: the animation library of the ANIM entry `name`: the
    // loaded one, else read by its file's loader and recorded (left with
    // the scene unless kKeepAsset). Its record, or 0.
    ee::Ptr<LoadedAsset> loadSceneAnimations(uint32_t flags, uint32_t name)
    {
        ee::Local<uint8_t[0x10]> extension;
        ee::Local<AnimationLoadRequest> request;
        const uint32_t id = nameHash(ee::Ptr<const char>(name));
        const ee::Ptr<LoadedAsset> known = findLoadedAsset(kAssetAnimations, id);
        if (known)
            return known;
        const ee::Ptr<SceneEntry> entry = findSceneEntry(*g_currentScene, kAssetAnimations, id, 0, 1);
        bool loaded = false;
        if (entry)
        {
            char *file = entry->file;
            pathExtension(file, extension);
            stringToUpper(extension);
            if (const uint32_t loader = findAssetLoader(kAssetAnimations, extension))
            {
                request->file = file;
                request->motions = ee::Ptr<MotionLink>();
                loaded = ee::FunctionPtr<int32_t(ee::Ptr<AnimationLoadRequest> request)>(loader)(request) != 0;
            }
        }
        if (!loaded)
            return ee::Ptr<LoadedAsset>();
        const ee::Ptr<LoadedAsset> record(registerLoadedAsset(entry, id, kAssetAnimations, kAssetAnimations, request->motions READABLE_PARAM_EXCEPT(dm2, (flags ^ kKeepAsset) & 1u)));
        // ZGE2's record is always left with the scene: one to keep has the flag taken off after (through the record
        // as it is: no test for none).
        if constexpr (game::readable::kIsDm2)
            if (flags & kKeepAsset)
                record->flags &= ~kAssetLeftWithScene;
        return record;
    }
    READABLE_FUNCTION_WAITS(loadSceneAnimations, Draft);

    // releaseAnimations (kReleaseAnimations): an animation library released: each
    // motion and its link freed, then its record handed back. 1 (0 without
    // a record).
    uint32_t releaseAnimations(ee::Ptr<LoadedAsset> record)
    {
        if (!record)
            return 0;
        ee::Ptr<MotionLink> link(record->asset);
        if (link)
        {
            for (;;)
            {
                const ee::Ptr<MotionLink> next = link->next;
                freeMotion(link->motion);
                heapFree(link);
                link = next;
                if (!link)
                    break;
            }
        }
        unregisterLoadedAsset(kAssetAnimations, kAssetAnimations, record->asset, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseAnimations, Verified);

    // findMotion: the motion of `id` in an animation library, or 0.
    ee::Ptr<Motion> findMotion(ee::Ptr<LoadedAsset> record, uint32_t id)
    {
        if (!record)
            return {};
        ee::Ptr<MotionLink> link(record->asset);
        if (!link)
            return {};
        for (;;)
        {
            if (link->id == id)
                return link->motion;
            link = link->next;
            if (!link)
                break;
        }
        return {};
    }
    READABLE_FUNCTION(findMotion, Verified);

    // motionAt: the index-th motion of an animation library (the newest
    // first), or 0.
    ee::Ptr<Motion> motionAt(ee::Ptr<LoadedAsset> record, uint32_t index)
    {
        if (!record)
            return {};
        ee::Ptr<MotionLink> link(record->asset);
        for (;;)
        {
            if (!link || index-- == 0)
                break;
            link = link->next;
        }
        const ee::Ptr<Motion> motion = link ? link->motion : ee::Ptr<Motion>();
        return motion;
    }
    READABLE_FUNCTION(motionAt, Verified);

    // listMotions: an animation library's motions into out[count], the
    // oldest first; the slots after them filled with out[0]. The number of
    // motions; 0 (nothing written) when they do not fit.
    int32_t listMotions(ee::Ptr<LoadedAsset> record, ee::Ptr<ee::Ptr<Motion>> out, int32_t count)
    {
        int32_t motions = 0;
        for (ee::Ptr<MotionLink> link(record->asset); link; link = link->next)
            ++motions;
        if (count < motions)
            return 0;
        const ee::Ptr<ee::Ptr<Motion>> slots = out;
        int32_t slot = motions;
        for (ee::Ptr<MotionLink> link(record->asset); link; link = link->next)
            slots[static_cast<uint32_t>(--slot)] = link->motion;
        for (int32_t i = motions; i < count; ++i)
            slots[static_cast<uint32_t>(i)] = slots[0];
        return motions;
    }
    READABLE_FUNCTION(listMotions, Verified);

    // newSceneCamera: a camera object made and (unless kKeepAsset) recorded.
    ee::Ptr<SceneObject> newSceneCamera(uint32_t flags)
    {
        const ee::Ptr<SceneObject> camera = newCamera();
        return registerMadeObject(flags, camera, kAssetCamera);
    }
    READABLE_FUNCTION_WAITS(newSceneCamera, Verified);

    namespace
    {
        // Scaffolding (address-call): newSceneCameraWith is a Draft (zge/scene/camera_objects.cpp), by its
        // address until it is proven.
        namespace calls
        {
            uint32_t newSceneCameraWith(uint32_t owner) { return ee::game<uint32_t>(kNewCameraOf, owner); }
        }
    }

    // newSceneCameraOf: newSceneCamera for the second kind of camera (its
    // setting handed on).
    uint32_t newSceneCameraOf(uint32_t flags, uint32_t setting)
    {
        const uint32_t camera = calls::newSceneCameraWith(setting);
        return registerMadeObject(flags, camera, kAssetCamera);
    }
    READABLE_FUNCTION_WAITS(newSceneCameraOf, Verified);

    // releaseSceneCamera (kReleaseSceneCamera): a camera's record handed back and
    // the object deleted. 1.
    uint32_t releaseSceneCamera(ee::Ptr<SceneObject> camera)
    {
        unregisterLoadedAsset(kAssetCamera, 0, camera, 0);
        deleteSceneObject(camera);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseSceneCamera, Verified);

    // newSceneLight: a light made (its two arguments handed on) and, unless
    // kKeepAsset, recorded; the maker makes none in this build (0).
    uint32_t newSceneLight(uint32_t flags, uint32_t first, uint32_t second)
    {
        const uint32_t light = lightObjectStubZero();
        return registerMadeObject(flags, light, kAssetLight);
    }
    READABLE_FUNCTION_WAITS(newSceneLight, Verified);

    // releaseSceneLight (kReleaseSceneLight): a light's record handed back and
    // the object deleted. 1.
    uint32_t releaseSceneLight(ee::Ptr<SceneObject> light)
    {
        unregisterLoadedAsset(kAssetLight, 0, light, 0);
        deleteSceneObject(light);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseSceneLight, Verified);

    // newSceneEmitter: a particle emitter object made and (unless kKeepAsset)
    // recorded.
    ee::Ptr<SceneObject> newSceneEmitter(uint32_t flags)
    {
        const ee::Ptr<SceneObject> emitter = newParticleEmitterObject();
        return registerMadeObject(flags, emitter, kAssetParticleEmitter);
    }
    READABLE_FUNCTION_WAITS(newSceneEmitter, Verified);

    // releaseSceneEmitter (kReleaseSceneEmitter): an emitter's record handed back
    // and the object deleted. 1.
    uint32_t releaseSceneEmitter(ee::Ptr<SceneObject> emitter)
    {
        unregisterLoadedAsset(kAssetParticleEmitter, 0, emitter, 0);
        deleteSceneObject(emitter);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseSceneEmitter, Verified);
}
