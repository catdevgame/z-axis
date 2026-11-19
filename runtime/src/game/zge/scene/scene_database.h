#pragma once

// The scene database (zScene.c, newSceneEntry .. lzDecompressOldAlias; Scene.Txt's header
// names the source "$Header: /NgEngine/src/zge/zScene.c"; DM2's ELF keeps
// the object in its link map between zobj.o and zcompres.o): Scene.Txt read
// into a tree of entries (a SCENE_COMMON section and the SCENE sections,
// each with its MODEL and ANIM entries, a MODEL with its own ANIMs), the
// scene entered, and the assets loaded from the entries (models, animation
// libraries) or made for the scene (lights, cameras, particle emitters),
// kept as LoadedAsset records so a second request shares the first's data
// and leaving the scene releases them.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable_waits.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    struct Motion; // (zge/scene/animation.h)
    // A scene entry's kind, and a loaded asset's (LoadedAsset::kind; the
    // last three are scene objects made by their modules: zobjLight,
    // zobjCamera, zobjPEmit).
    enum AssetKind : uint32_t
    {
        kAssetScene = 0,           // a SCENE or SCENE_COMMON section
        kAssetModel = 1,           // MODEL
        kAssetAnimations = 2,      // ANIM: an animation library
        kAssetLight = 3,           // the light's maker (lightObjectStubZero) returns 0 in this build
        kAssetCamera = 4,
        kAssetParticleEmitter = 5,
    };

    // One of Scene.Txt's sections (0x78 bytes, a record of the entry pool):
    // its keywords' values, its place in the tree.
    struct SceneEntry
    {
        char name[0x20];                // +0x00 NAME
        char file[0x40];                // +0x20 FILE (the scene's chunk file, the model's or the library's)
        uint32_t kind;                  // +0x60 AssetKind
        uint32_t id;                    // +0x64 nameHash(name): what the game asks for
        int32_t type;                   // +0x68 TYPE (a model's: kModelTypePlayer 0, Bike 1, Misc 2, NPC 3)
        int32_t subtype;                // +0x6C SUBTYPE
        ee::Ptr<SceneEntry> next;       // +0x70 the next entry in the same section (or the next SCENE)
        ee::Ptr<SceneEntry> firstChild; // +0x74 the first entry inside its braces (the last one read)
    };
    static_assert(sizeof(SceneEntry) == 0x78);
    static_assert(offsetof(SceneEntry, file) == 0x20 && offsetof(SceneEntry, kind) == 0x60);
    static_assert(offsetof(SceneEntry, id) == 0x64 && offsetof(SceneEntry, type) == 0x68);
    static_assert(offsetof(SceneEntry, next) == 0x70 && offsetof(SceneEntry, firstChild) == 0x74);

    // What findSceneEntry looks for (on its stack): an entry of `kind` whose
    // id is `id`, or, with id 0, the index-th entry of that kind in the
    // walk's order (children first, then the next ones).
    struct SceneEntryQuery
    {
        uint32_t kind;                  // +0x00
        uint32_t id;                    // +0x04 0: by index
        int32_t index;                  // +0x08 counted down to -1 by the walk
        ee::Ptr<SceneEntry> found;      // +0x0C
    };
    static_assert(sizeof(SceneEntryQuery) == 0x10);

    // LoadedAsset::flags.
    constexpr uint16_t kAssetLeftWithScene = 1u << 0; // released when the scene is left (else at shutdown)

    // An asset loaded or made for the scene (16 bytes, a record of the
    // loaded assets' pool).
    struct LoadedAsset
    {
        ee::Ptr<SceneEntry> entry;      // +0x00 the entry it came from (0: made without one)
        uint32_t id;                    // +0x04 the entry's id (0: none)
        uint8_t kind;                   // +0x08 AssetKind
        // +0x09 what `asset` is: a model's 0: a model object (a SceneObject),
        // other: the model's scene (zge/scene/scene.h Scene); an animation
        // library's 2; 0 for the others.
        uint8_t form;
        uint16_t flags;                 // +0x0A kAssetLeftWithScene; the other bits as the pool left them
        uint32_t asset;                 // +0x0C the object, scene or library (MotionLink list)
    };
    static_assert(sizeof(LoadedAsset) == 0x10);
    static_assert(offsetof(LoadedAsset, kind) == 0x08 && offsetof(LoadedAsset, flags) == 0x0A);

    // What findLoadedAsset looks for (on its stack).
    struct LoadedAssetQuery
    {
        uint32_t kind;                  // +0x00
        uint32_t id;                    // +0x04
        ee::Ptr<LoadedAsset> found;     // +0x08
    };

    // An animation library's motions (12-byte heap blocks, the newest
    // first): each motion read from the library's file (zanim.c's records).
    struct MotionLink
    {
        uint32_t id;                    // +0x00 the motion's id (its record's first word)
        ee::Ptr<Motion> motion;         // +0x04
        ee::Ptr<MotionLink> next;       // +0x08
    };
    static_assert(sizeof(MotionLink) == 0x0C);

    // A loader's request (on newSceneModel's stack): the entry's file and
    // id, the model the loader made.
    struct ModelLoadRequest
    {
        ee::Ptr<char> file;             // +0x00 SceneEntry::file
#if PS2X_GAME_DM2
        // DM2: ZGE2's request is three words, with no id (its newSceneModel sets file, reserved and model; its
        // loadZitModel reads +0x04 and stores the model at +0x08). The id stands past them: nothing of DM2's reads it.
        uint32_t reserved;              // DM2 +0x04
        uint32_t model;                 // DM2 +0x08
        uint32_t id;
#else
        uint32_t id;                    // +0x04 the model's id (a .ZAL library's item)
        uint32_t reserved;              // +0x08 0, read by no loader
        uint32_t model;                 // +0x0C the model's scene (0: failed)
#endif
    };
    // An animation loader's request (on loadSceneAnimations's stack).
    struct AnimationLoadRequest
    {
        ee::Ptr<char> file;             // +0x00 SceneEntry::file
        ee::Ptr<MotionLink> motions;    // +0x04 the motions read (0: none)
    };

    // The loaders of a kind, by the file's extension (upper case): a list
    // ended by a 0 extension.
    struct AssetLoader
    {
        uint32_t extension;             // +0x00 "ZIT", "ZAL", "NAD", "IFF"
        uint32_t load;                  // +0x04 (request) -> non-zero when it loaded
    };
    struct AssetKindLoaders
    {
        uint32_t name;                  // +0x00 "SCENE", "MODEL", "ANIM"
        ee::Ptr<AssetLoader> loaders;   // +0x04 0: none (the scenes')
    };

    // newSceneModel's flags (and the cameras' and the others' makers': only
    // kKeepAsset).
    constexpr uint32_t kKeepAsset = 1u << 0;     // kept when the scene is left (released at shutdown)
    constexpr uint32_t kModelNoObject = 1u << 1; // the model loaded, no object made (0 returned)
}

namespace zge
{
    struct SceneObject; // (zge/scene/scene.h)

    // (scene_database.cpp)
    uint32_t registerLoadedAsset(ee::Ptr<SceneEntry> entry, uint32_t id, uint32_t kind, uint32_t form, ee::Address asset READABLE_PARAM_EXCEPT(dm2, uint32_t leftWithScene));
    int32_t initSceneDatabase(uint32_t path);
    int32_t shutDownSceneDatabase();
    uint32_t enterScene(uint32_t name);
    int32_t addSceneFile(uint32_t name);
    int32_t enterUnlistedScene(ee::Address cellSize, ee::Address bounds);
    uint32_t leaveScene();
    ee::Ptr<SceneObject> newSceneModelByName(uint32_t flags, ee::Ptr<const char> name);
    ee::Ptr<SceneObject> newSceneModelByHash(uint32_t flags, uint32_t id);
    // ZGE2's newSceneModel (DM2's): the model object by the model's name (its id the name's hash), or 0.
    ee::Ptr<SceneObject> newSceneModelFromName(uint32_t flags, ee::Ptr<const char> name);
    uint32_t releaseSceneModel(ee::Ptr<SceneObject> object);
    ee::Ptr<Motion> findMotion(ee::Ptr<LoadedAsset> record, uint32_t id);
    ee::Ptr<LoadedAsset> loadSceneAnimations(uint32_t flags, uint32_t name);
    int32_t listMotions(ee::Ptr<LoadedAsset> record, ee::Ptr<ee::Ptr<Motion>> out, int32_t count);
    ee::Ptr<SceneObject> newSceneCamera(uint32_t flags);
    uint32_t releaseSceneCamera(ee::Ptr<SceneObject> camera);
    uint32_t newSceneLight(uint32_t flags, uint32_t first, uint32_t second);
    ee::Ptr<SceneObject> newSceneEmitter(uint32_t flags);
}
