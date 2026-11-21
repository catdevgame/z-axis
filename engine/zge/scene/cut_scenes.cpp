// ZGE4's cut scenes (BMX XXX only): the store of a level's cut scene scripts, read from its ZSCD chunk (how many
// and how many bytes of script in all) and one ZSCR chunk each (the name, 0x20 bytes, then the script's words);
// the scripts linked to the level's objects, motions and one another once the level is loaded; and the
// interpreter that plays one (startCutScene, by its name's hash) and its end (endCutScene).
//
// A script is a list of commands (CutSceneCommand), each its number and then its words, ending in kCutSceneEnd.
// An object word names a level object by its name's hash until the scripts are linked, then is the object (0
// when nothing has the name); the camera's name hash stays as it is and means the cut scene's camera
// (setCutSceneCamera). A motion is one of the animation libraries' when one has it, else it is loaded from the
// cut scene library (setCutSceneLibrary) when the cut scene starts, in the library's order, and freed at its end.

#include "cut_scenes.h"

#include "ee/abi.h"
#include "ee/io.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "zge/assets/asset_library.h"
#include "zge/assets/chunk_reader.h"
#include "zge/math/types.h"
#include "zge/memory/regions.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/cut_scenes.cpp.h"

namespace zge
{
    // A cut scene (0x38 bytes, one of the store's records).
    struct CutScene
    {
        char name[0x20];      // +0x00 from its ZSCR chunk
        uint32_t nameHash;    // +0x20 nameHash(name): what startCutScene looks for
        uint32_t script;      // +0x24 its commands in the store's script block
        // +0x28 .. +0x34 the scripts of the cut scenes its commands kCutSceneNamedScript1, 2, kCutSceneSetUp and
        // kCutSceneAtOnce name, set as the scripts are linked (the first two are read by nothing).
        uint32_t namedScript1;
        uint32_t namedScript2;
        uint32_t setUpScript;  // +0x30 run once the level's scripts are linked (runCutSceneSetupScripts)
        uint32_t atOnceScript; // +0x34 played in place of the script when the cut scene is played at once
    };
    static_assert(offsetof(CutScene, nameHash) == 0x20 && offsetof(CutScene, script) == 0x24 &&
                  offsetof(CutScene, setUpScript) == 0x30 && offsetof(CutScene, atOnceScript) == 0x34 &&
                  sizeof(CutScene) == 0x38);

    // A script's commands, by their number (the first word of each).
    enum CutSceneCommand : uint32_t
    {
        kCutScenePlay = 0,          // CutScenePlay: a motion played on an object
        kCutSceneStopAnimation = 1, // CutSceneObjectCommand: the object's animator freed
        kCutScenePause = 2,         // CutSceneObjectCommand: the object's animation paused
        kCutSceneResume = 3,        // CutSceneObjectCommand: and going again
        // 4 and 9 are no command: the interpreter hangs on them (and on any number past 15).
        kCutSceneShow = 5,          // CutSceneValueCommand: the object shown (value 1) or hidden (0)
        kCutSceneIgnored = 6,       // CutSceneValueCommand: nothing done
        kCutSceneLoop = 7,          // CutSceneValueCommand: the object's animation looped (value not 0) or not
        kCutScenePlace = 8,         // CutScenePlace: the object moved and turned
        kCutSceneRun = 10,          // CutSceneScriptCommand: another cut scene's script run there and then
        kCutSceneNamedScript1 = 11, // CutSceneScriptCommand: another cut scene named (namedScript1)
        kCutSceneNamedScript2 = 12, // CutSceneScriptCommand: another cut scene named (namedScript2)
        kCutSceneSetUp = 13,        // CutSceneScriptCommand: the script run as the level is set up
        kCutScenePlayLoaded = 14,   // CutScenePlay of a motion the animation libraries do not have (the linking's)
        kCutSceneAtOnce = 15,       // CutSceneScriptCommand: the script played instead when played at once
        kCutSceneEnd = 10000,       // the script's end
    };

    // The commands' words.
    struct CutScenePlay
    {
        uint32_t command; // kCutScenePlay, kCutScenePlayLoaded
        uint32_t object;  // the object word
        // kCutScenePlay: the motion's name hash, once linked the motion; kCutScenePlayLoaded: the name hash
        uint32_t motion;
        ee::Float blend;  // seconds it blends in over (playAnimation's)
        uint32_t mode;    // playAnimation's mode
    };
    static_assert(sizeof(CutScenePlay) == 20);
    struct CutSceneObjectCommand
    {
        uint32_t command;
        uint32_t object;
    };
    struct CutSceneValueCommand
    {
        uint32_t command;
        uint32_t object;
        uint32_t value;
    };
    struct CutScenePlace
    {
        uint32_t command;
        uint32_t object;
        ee::Float position[3];    // the object's position
        ee::Float orientation[4]; // and orientation (a quaternion)
    };
    static_assert(sizeof(CutScenePlace) == 36);
    struct CutSceneScriptCommand
    {
        uint32_t command;
        uint32_t script; // the cut scene's name hash, once linked its script (0: none has the name)
    };

    // A motion the cut scene's scripts play from the cut scene library (kCutScenePlayLoaded), 12 bytes.
    struct CutSceneMotion
    {
        ee::Ptr<Motion> motion; // loaded as the cut scene starts (0: not found)
        uint32_t nameHash;      // the motion's name hash: its asset's id in the library
        uint32_t assetIndex;    // its asset's place in the library (the motions are loaded in that order)
    };
    static_assert(sizeof(CutSceneMotion) == 12);

    namespace
    {
        constexpr uint32_t kCutSceneNameBytes = sizeof(CutScene::name);
        inline ee::Global<uint32_t, sym::bmx::g_cutScenesReady> g_cutScenesReady;
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneCount> g_cutSceneCount;   // the ZSCD chunk's count
        inline ee::Global<uint32_t, sym::bmx::g_cutScenesRead> g_cutScenesRead;   // the ZSCR chunks read so far
        inline ee::Global<ee::Ptr<CutScene>, sym::bmx::g_cutScenes> g_cutScenes;
        // The scripts, one after another: the block (from the ZSCD chunk's size) and where the next one goes.
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneData> g_cutSceneData;
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneDataEnd> g_cutSceneDataEnd;
        inline ee::Global<uint32_t, sym::bmx::g_lastCutSceneScript> g_lastCutSceneScript;
        // Called with each script read and its context, when set (nothing in BMX XXX sets it).
        inline ee::Global<ee::FunctionPtr<void(uint32_t script, uint32_t context)>, sym::bmx::g_cutSceneReadHook> g_cutSceneReadHook;
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneReadHookContext> g_cutSceneReadHookContext;
        // A command's object word holding this hash means the cut scene's camera (setCutSceneCamera's object).
        inline ee::Global<uint32_t, sym::bmx::g_cameraNameHash> g_cameraNameHash;
        inline ee::Global<ee::Ptr<SceneObject>, sym::bmx::g_cutSceneCamera> g_cutSceneCamera;
        // The motions' block startCutScene hands the animation code, and its size (setCutSceneLibrary).
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneMotionBytes> g_cutSceneMotionBytes;
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneMotionBlock> g_cutSceneMotionBlock;
        // The library the scripts' motions come from (its path), open while a cut scene plays.
        constexpr uint32_t kCutSceneLibraryPath = sym::bmx::g_cutSceneLibraryPath;
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneLibrary> g_cutSceneLibrary;
        // The motions of the cut scene playing that come from it.
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneMotionCount> g_cutSceneMotionCount;
        constexpr uint32_t kCutSceneMotions = sym::bmx::g_cutSceneMotions;
        inline ee::Ptr<CutSceneMotion> cutSceneMotions() { return ee::Ptr<CutSceneMotion>(kCutSceneMotions); }
        // The objects playing those motions (endCutScene ends their animations), at most kCutSceneActorSlots.
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneActorCount> g_cutSceneActorCount;
        constexpr uint32_t kCutSceneActors = sym::bmx::g_cutSceneActors;
        constexpr uint32_t kCutSceneActorSlots = 40;
        inline ee::Ptr<ee::Ptr<SceneObject>> cutSceneActors() { return ee::Ptr<ee::Ptr<SceneObject>>(kCutSceneActors); }
        // Set while the setup scripts run (runCutSceneSetupScripts); while a cut scene plays; and while one is
        // played at once or ends (cutSceneAtOnce).
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneSettingUp> g_cutSceneSettingUp;
        inline ee::Global<uint32_t, sym::bmx::g_cutScenePlaying> g_cutScenePlaying;
        inline ee::Global<uint32_t, sym::bmx::g_cutSceneAtOnce> g_cutSceneAtOnce;
        // cutSceneList's answer: every cut scene, then 0.
        constexpr uint32_t kCutSceneList = sym::bmx::g_cutSceneList;
        constexpr uint32_t kZscrChunk = 0x5243535Au; // "ZSCR" (not an address)
        constexpr uint32_t kZscdChunk = 0x4443535Au; // "ZSCD" (not an address)
    }

    namespace
    {
        // The object a linked command's object word names: the cut scene's camera for the camera's name hash,
        // else the word is the object (0: none).
        inline ee::Ptr<SceneObject> commandObject(uint32_t word)
        {
            return word == *g_cameraNameHash ? *g_cutSceneCamera : ee::Ptr<SceneObject>(word);
        }
        // A command's words as the record its kind says they are.
        template <typename Command>
        inline ee::Ptr<Command> commandAs(ee::Ptr<uint32_t> command)
        {
            return command.template cast<Command>();
        }
        // The command after this one's words.
        template <typename Command>
        inline ee::Ptr<uint32_t> nextCommand(ee::Ptr<Command> command)
        {
            return (command + 1).template cast<uint32_t>();
        }
    }

    // readCutScene (a ZSCR chunk): the next cut scene's name and script, the script's place returned (and handed
    // to the read hook, when there is one).
    inline uint32_t readCutScene(ChunkReader *reader)
    {
        const uint32_t scriptBytes = chunkDataSize(reader) - kCutSceneNameBytes;
        const uint32_t script = *g_cutSceneDataEnd;
        *g_lastCutSceneScript = script;
        *g_cutSceneDataEnd = script + scriptBytes;
        const ee::Ptr<CutScene> scene = *g_cutScenes + *g_cutScenesRead;
        readChunk(reader, &scene->name, kCutSceneNameBytes);
        scene->nameHash = nameHash(scene->name);
        scene->script = script;
        readChunk(reader, script, scriptBytes);
        // (The original walks the script's words here doing nothing with them.)
        *g_cutScenesRead = *g_cutScenesRead + 1;
        if (*g_cutSceneReadHook)
            (*g_cutSceneReadHook)(*g_lastCutSceneScript, *g_cutSceneReadHookContext);
        return *g_lastCutSceneScript;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readCutScene);

    // readCutSceneDirectory (the ZSCD chunk): the count and the scripts' bytes; cleared records and block made
    // for them, none read yet.
    inline uint32_t readCutSceneDirectory(ChunkReader *reader)
    {
        ee::Local<uint32_t> scriptBytes;
        readChunkU32(reader, &*g_cutSceneCount);
        readChunkU32(reader, scriptBytes);
        *g_cutScenesRead = 0;
        *g_cutScenes = ee::Ptr<CutScene>(heapAllocate(*g_cutSceneCount * sizeof(CutScene)));
        const uint32_t data = heapAllocate(*scriptBytes);
        *g_cutSceneDataEnd = data;
        *g_cutSceneData = data;
        memoryFill(*g_cutScenes, 0, *g_cutSceneCount * sizeof(CutScene));
        memoryFill(*g_cutSceneData, 0, *scriptBytes);
        return 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readCutSceneDirectory);

    // The chunk handlers: the reader's answer into the context word.
    inline uint32_t readCutSceneChunk(ChunkReader *reader, ee::Ptr<uint32_t> context)
    {
        *context = readCutScene(reader);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readCutSceneChunk);
    inline uint32_t readCutSceneDirectoryChunk(ChunkReader *reader, ee::Ptr<uint32_t> context)
    {
        *context = readCutSceneDirectory(reader);
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, readCutSceneDirectoryChunk);

    // initCutScenes: the two chunk handlers registered, and the camera's name hash.
    inline uint32_t initCutScenes()
    {
        *g_cutScenesReady = 1;
        *g_cutSceneCount = 0;
        addChunkHandler(kZscrChunk, sym::bmx::readCutSceneChunk);
        addChunkHandler(kZscdChunk, sym::bmx::readCutSceneDirectoryChunk);
        *g_cameraNameHash = nameHash(ee::Ptr<const char>(sym::bmx::textCamera));
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, initCutScenes);

    // freeCutScenes: the motions' block, the records and the scripts freed.
    inline void freeCutScenes()
    {
        if (*g_cutSceneMotionBlock)
        {
            *g_cutSceneMotionBytes = 0;
            heapFree(*g_cutSceneMotionBlock);
            *g_cutSceneMotionBlock = 0;
        }
        if (*g_cutScenesRead == 0)
            return;
        *g_cutScenesRead = 0;
        if (*g_cutScenes)
            heapFree(*g_cutScenes);
        if (*g_cutSceneData)
            heapFree(*g_cutSceneData);
        *g_cutScenes = ee::Ptr<CutScene>();
        *g_cutSceneData = 0;
        *g_cutSceneDataEnd = 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, freeCutScenes);

    // shutDownCutScenes: once set up, everything freed and the store emptied.
    inline uint32_t shutDownCutScenes()
    {
        if (*g_cutScenesReady == 0)
            return 1;
        freeCutScenes();
        *g_cutScenesReady = 0;
        *g_cutScenesRead = 0;
        *g_cutSceneData = 0;
        *g_cutScenes = ee::Ptr<CutScene>();
        *g_cutSceneDataEnd = 0;
        *g_cutSceneCount = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, shutDownCutScenes);

    inline void setCutSceneCamera(ee::Ptr<SceneObject> camera) { *g_cutSceneCamera = camera; }
    READABLE_FUNCTION_ONLY(bmx, setCutSceneCamera);

    // cutSceneList: every cut scene read, then 0.
    inline ee::Ptr<ee::Ptr<CutScene>> cutSceneList()
    {
        const ee::Ptr<ee::Ptr<CutScene>> list(kCutSceneList);
        uint32_t i = 0;
        for (; i < *g_cutScenesRead; ++i)
            list[i] = *g_cutScenes + i;
        list[i] = ee::Ptr<CutScene>();
        return list;
    }
    READABLE_FUNCTION_ONLY(bmx, cutSceneList);

    // setCutSceneLibrary: the motions' library path kept, and their block: freed when its size changes, none for
    // 0 bytes. A block is made only where one is held already (the old one is lost then): with none held, none
    // is made, the original's test kept as it is.
    inline void setCutSceneLibrary(ee::Ptr<const char> path, uint32_t motionBytes)
    {
        stringCopy(kCutSceneLibraryPath, path);
        if (*g_cutSceneMotionBlock && motionBytes != *g_cutSceneMotionBytes)
        {
            heapFree(*g_cutSceneMotionBlock);
            *g_cutSceneMotionBlock = 0;
        }
        if (motionBytes == 0)
        {
            *g_cutSceneMotionBytes = 0;
            *g_cutSceneMotionBlock = 0;
            return;
        }
        if (*g_cutSceneMotionBlock == 0)
            return;
        *g_cutSceneMotionBytes = motionBytes;
        *g_cutSceneMotionBlock = heapAllocate(motionBytes);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, setCutSceneLibrary);

    // findCutSceneObject: the object of a name's hash: the level object of that name, else the model object of
    // that name (namedModelObject), else one made from the pose of that name (0 when there is none).
    inline ee::Ptr<SceneObject> findCutSceneObject(uint32_t nameHash)
    {
        ee::Local<uint32_t> name;
        *name = nameHash;
        const ee::Ptr<ee::Ptr<SceneObject>> found = findObjects(name, 1);
        if (found)
            return found[0];
        const ee::Ptr<SceneObject> model = namedModelObject(nameHash, 1);
        if (model)
            return model;
        return newModelObjectFromPose(nameHash, 0);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, findCutSceneObject);

    namespace
    {
        // A command's object word linked: the object of the name's hash (0: none); the camera's stays.
        inline void linkCommandObject(uint32_t &word)
        {
            if (word != *g_cameraNameHash)
                word = ee::Address(findCutSceneObject(word));
        }
    }

    // linkCutSceneScripts: every cut scene's commands linked to the level loaded: object words to the objects
    // (each object a play command names given an animator), motions to the animation libraries' (a play
    // command whose motion none has becomes kCutScenePlayLoaded), cut scene names to their scripts (0: none has
    // the name; the set-up and at-once ones, and the two named ones, kept in the cut scene's record too).
    inline void linkCutSceneScripts()
    {
        for (uint32_t i = 0; i < *g_cutScenesRead; ++i)
        {
            const ee::Ptr<CutScene> scene = *g_cutScenes + i;
            ee::Ptr<uint32_t> command(scene->script);
            while (*command != kCutSceneEnd)
            {
                const uint32_t number = *command;
                switch (number)
                {
                case kCutScenePlay:
                {
                    const ee::Ptr<CutScenePlay> play = commandAs<CutScenePlay>(command);
                    if (play->object != *g_cameraNameHash)
                    {
                        const ee::Ptr<SceneObject> object = findCutSceneObject(play->object);
                        if (object)
                        {
                            createObjectAnimator(object);
                            play->object = ee::Address(object);
                        }
                        else
                            play->object = 0;
                    }
                    const ee::Ptr<Motion> motion = findLibraryAnimation(play->motion);
                    if (motion)
                        play->motion = ee::Address(motion);
                    else
                        play->command = kCutScenePlayLoaded;
                    command = nextCommand(play);
                    break;
                }
                case kCutSceneStopAnimation:
                case kCutScenePause:
                case kCutSceneResume:
                {
                    const ee::Ptr<CutSceneObjectCommand> objectCommand = commandAs<CutSceneObjectCommand>(command);
                    linkCommandObject(objectCommand->object);
                    command = nextCommand(objectCommand);
                    break;
                }
                case kCutSceneShow:
                case kCutSceneIgnored:
                case kCutSceneLoop:
                {
                    const ee::Ptr<CutSceneValueCommand> valueCommand = commandAs<CutSceneValueCommand>(command);
                    linkCommandObject(valueCommand->object);
                    command = nextCommand(valueCommand);
                    break;
                }
                case kCutScenePlace:
                {
                    const ee::Ptr<CutScenePlace> place = commandAs<CutScenePlace>(command);
                    linkCommandObject(place->object);
                    command = nextCommand(place);
                    break;
                }
                case kCutSceneRun:
                case kCutSceneNamedScript1:
                case kCutSceneNamedScript2:
                case kCutSceneSetUp:
                case kCutSceneAtOnce:
                {
                    const ee::Ptr<CutSceneScriptCommand> scriptCommand = commandAs<CutSceneScriptCommand>(command);
                    uint32_t other = 0;
                    for (; other < *g_cutScenesRead; ++other)
                    {
                        const ee::Ptr<CutScene> named = *g_cutScenes + other;
                        if (scriptCommand->script != named->nameHash)
                            continue;
                        const uint32_t script = named->script;
                        scriptCommand->script = script;
                        if (number == kCutSceneNamedScript1)
                            scene->namedScript1 = script;
                        else if (number == kCutSceneNamedScript2)
                            scene->namedScript2 = script;
                        else if (number == kCutSceneSetUp)
                            scene->setUpScript = script;
                        else if (number == kCutSceneAtOnce)
                            scene->atOnceScript = script;
                        break;
                    }
                    if (other == *g_cutScenesRead)
                        scriptCommand->script = 0;
                    command = nextCommand(scriptCommand);
                    break;
                }
                default: // 4, 9, past 15 and an already linked kCutScenePlayLoaded: the number alone
                    command += 1;
                    break;
                }
            }
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, linkCutSceneScripts);

    void runCutSceneScript(ee::Ptr<uint32_t> script, uint32_t atOnce); // (below)

    // runCutSceneSetupScripts: each cut scene's set-up script (kCutSceneSetUp) run, as the level starts.
    inline void runCutSceneSetupScripts()
    {
        for (uint32_t i = 0; i < *g_cutScenesRead; ++i)
        {
            *g_cutSceneSettingUp = 1;
            const uint32_t setUp = (*g_cutScenes + i)->setUpScript;
            if (setUp)
                runCutSceneScript(ee::Ptr<uint32_t>(setUp), 0);
            *g_cutSceneSettingUp = 0;
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, runCutSceneSetupScripts);

    // addCutSceneMotion: a motion of the cut scene library noted for loading (its asset's place in the library
    // with it), unless it is noted already or there is no library; the library opened for it if it is not.
    inline void addCutSceneMotion(uint32_t nameHash)
    {
        bool noted = false;
        for (uint32_t i = 0; i < *g_cutSceneMotionCount; ++i)
            if (cutSceneMotions()[i].nameHash == nameHash)
            {
                noted = true;
                break;
            }
        if (ee::Ptr<const char>(kCutSceneLibraryPath)[0] == 0 || noted)
            return;
        if (*g_cutSceneLibrary == 0)
        {
            *g_cutSceneLibrary = openAssetLibrary(kCutSceneLibraryPath);
            if (*g_cutSceneLibrary == 0)
                return;
        }
        // (findAsset's answer is not looked at: for a motion the library has not got, the place noted is that of
        // the entry selected before.)
        findAsset(*g_cutSceneLibrary, nameHash);
        const uint32_t assetIndex = currentAsset(*g_cutSceneLibrary);
        const uint32_t slot = *g_cutSceneMotionCount;
        *g_cutSceneMotionCount = slot + 1;
        cutSceneMotions()[slot].assetIndex = assetIndex;
        cutSceneMotions()[slot].nameHash = nameHash;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, addCutSceneMotion);

    // sortCutSceneMotions: the motions noted put in their assets' order in the library (a bubble sort).
    inline void sortCutSceneMotions()
    {
        ee::Local<CutSceneMotion> held;
        const ee::Ptr<CutSceneMotion> motions = cutSceneMotions();
        for (uint32_t pass = 0; pass < *g_cutSceneMotionCount; ++pass)
            for (uint32_t i = 0; i < *g_cutSceneMotionCount - 1; ++i)
                if (motions[i + 1].assetIndex < motions[i].assetIndex)
                {
                    memoryCopy(held, motions + i, sizeof(CutSceneMotion));
                    memoryCopy(motions + i, motions + (i + 1), sizeof(CutSceneMotion));
                    memoryCopy(motions + (i + 1), held, sizeof(CutSceneMotion));
                }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, sortCutSceneMotions);

    // loadCutSceneMotions: each motion noted read from the cut scene library.
    inline void loadCutSceneMotions()
    {
        for (uint32_t i = 0; i < *g_cutSceneMotionCount; ++i)
            cutSceneMotions()[i].motion = loadLibraryMotion(*g_cutSceneLibrary, cutSceneMotions()[i].nameHash);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, loadCutSceneMotions);

    // playCutSceneMotion (kCutScenePlay): the motion played on the object (the caller's hooks none); with
    // atOnce, run to its end there and then.
    inline ee::Ptr<uint32_t> playCutSceneMotion(ee::Ptr<CutScenePlay> play, uint32_t atOnce)
    {
        ee::Local<ee::Float> blend;
        const ee::Ptr<SceneObject> object = commandObject(play->object);
        const ee::Ptr<Motion> motion(play->motion);
        memoryCopy(blend, &play->blend, sizeof(ee::Float));
        if (object && motion)
        {
            playAnimationWithHooks(object, motion, play->mode, 0, 0, *blend);
            if (atOnce)
                finishAnimation(object);
        }
        return nextCommand(play);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, playCutSceneMotion);

    // playCutSceneLoadedMotion (kCutScenePlayLoaded): the same with the motion loaded from the cut scene
    // library (nothing when it was not loaded), the object kept among the cut scene's actors (nothing once there
    // are kCutSceneActorSlots).
    inline ee::Ptr<uint32_t> playCutSceneLoadedMotion(ee::Ptr<CutScenePlay> play, uint32_t atOnce)
    {
        ee::Local<ee::Float> blend;
        const ee::Ptr<SceneObject> object = commandObject(play->object);
        const uint32_t nameHash = play->motion;
        memoryCopy(blend, &play->blend, sizeof(ee::Float));
        const uint32_t mode = play->mode;
        ee::Ptr<Motion> motion;
        for (uint32_t i = 0; i < *g_cutSceneMotionCount; ++i)
            if (cutSceneMotions()[i].nameHash == nameHash)
            {
                motion = cutSceneMotions()[i].motion;
                break;
            }
        if (object && motion && *g_cutSceneActorCount < kCutSceneActorSlots)
        {
            playAnimationWithHooks(object, motion, mode, 0, 0, *blend);
            const uint32_t actor = *g_cutSceneActorCount;
            *g_cutSceneActorCount = actor + 1;
            cutSceneActors()[actor] = object;
            if (atOnce)
                finishAnimation(object);
        }
        return nextCommand(play);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, playCutSceneLoadedMotion);

    // stopCutSceneAnimation (kCutSceneStopAnimation): the object's animator freed.
    inline ee::Ptr<uint32_t> stopCutSceneAnimation(ee::Ptr<CutSceneObjectCommand> command)
    {
        const ee::Ptr<SceneObject> object = commandObject(command->object);
        if (object)
            freeAnimatorOf(object);
        return nextCommand(command);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, stopCutSceneAnimation);

    // pauseCutSceneAnimation, resumeCutSceneAnimation (kCutScenePause, kCutSceneResume).
    inline ee::Ptr<uint32_t> pauseCutSceneAnimation(ee::Ptr<CutSceneObjectCommand> command)
    {
        const ee::Ptr<SceneObject> object = commandObject(command->object);
        if (object)
            pauseAnimation(object);
        return nextCommand(command);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, pauseCutSceneAnimation);
    inline ee::Ptr<uint32_t> resumeCutSceneAnimation(ee::Ptr<CutSceneObjectCommand> command)
    {
        const ee::Ptr<SceneObject> object = commandObject(command->object);
        if (object)
            resumeAnimation(object);
        return nextCommand(command);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, resumeCutSceneAnimation);

    // loopCutSceneAnimation (kCutSceneLoop): the object's newest track looped or not, the object animated off
    // screen too (animationMode answers 0, so the on-screen-only mode the first call sets does not last); during
    // the set-up scripts its animation time moved on by a random share of the track, so that looped objects do
    // not move together.
    inline ee::Ptr<uint32_t> loopCutSceneAnimation(ee::Ptr<CutSceneValueCommand> command)
    {
        const ee::Ptr<SceneObject> object = commandObject(command->object);
        if (object)
        {
            setAnimationMode(object, kAnimationOnScreenOnly);
            const uint32_t looped = command->value;
            uint32_t mode = animationMode(object);
            if (looped)
                mode |= kAnimationLooped;
            else
                mode &= ~kAnimationLooped;
            setAnimationMode(object, mode);
            if (*g_cutSceneSettingUp)
                randomizeAnimationTime(object);
        }
        return nextCommand(command);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, loopCutSceneAnimation);

    // placeCutSceneObject (kCutScenePlace): the object moved to the position and turned to the orientation.
    inline ee::Ptr<uint32_t> placeCutSceneObject(ee::Ptr<CutScenePlace> place)
    {
        ee::Local<Vec4> position;
        ee::Local<Quaternion> orientation;
        const ee::Ptr<SceneObject> object = commandObject(place->object);
        memoryCopy(position, &place->position, sizeof(place->position));
        memoryCopy(orientation, &place->orientation, sizeof(place->orientation));
        if (object)
        {
            setObjectPosition(object, position);
            setObjectOrientation(object, orientation);
        }
        return nextCommand(place);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, placeCutSceneObject);

    // showCutSceneObject (kCutSceneShow): the object shown or hidden.
    inline ee::Ptr<uint32_t> showCutSceneObject(ee::Ptr<CutSceneValueCommand> command)
    {
        const ee::Ptr<SceneObject> object = commandObject(command->object);
        if (object)
            setSceneObjectShown(object, command->value);
        return nextCommand(command);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, showCutSceneObject);

    // ignoreCutSceneCommand (kCutSceneIgnored): nothing.
    inline ee::Ptr<uint32_t> ignoreCutSceneCommand(ee::Ptr<CutSceneValueCommand> command)
    {
        return nextCommand(command);
    }
    READABLE_FUNCTION_ONLY(bmx, ignoreCutSceneCommand);

    // runLinkedCutScene (kCutSceneRun): the other cut scene's script run there and then (none: nothing).
    inline ee::Ptr<uint32_t> runLinkedCutScene(ee::Ptr<CutSceneScriptCommand> command, uint32_t atOnce)
    {
        if (command->script)
            runCutSceneScript(ee::Ptr<uint32_t>(command->script), atOnce);
        return nextCommand(command);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, runLinkedCutScene);

    // runCutSceneScript: a linked script's commands done in order, to its end; with atOnce each animation runs to
    // its end as it starts. Bug kept: a command number with no command (4, 9, past 15) is read again for ever (the
    // game hangs).
    inline void runCutSceneScript(ee::Ptr<uint32_t> script, uint32_t atOnce)
    {
        ee::Ptr<uint32_t> command = script;
        while (*command != kCutSceneEnd)
        {
            switch (*command)
            {
            case kCutScenePlay:
                command = playCutSceneMotion(commandAs<CutScenePlay>(command), atOnce);
                break;
            case kCutScenePlayLoaded:
                command = playCutSceneLoadedMotion(commandAs<CutScenePlay>(command), atOnce);
                break;
            case kCutSceneStopAnimation:
                command = stopCutSceneAnimation(commandAs<CutSceneObjectCommand>(command));
                break;
            case kCutScenePause:
                command = pauseCutSceneAnimation(commandAs<CutSceneObjectCommand>(command));
                break;
            case kCutSceneResume:
                command = resumeCutSceneAnimation(commandAs<CutSceneObjectCommand>(command));
                break;
            case kCutSceneIgnored:
                command = ignoreCutSceneCommand(commandAs<CutSceneValueCommand>(command));
                break;
            case kCutSceneShow:
                command = showCutSceneObject(commandAs<CutSceneValueCommand>(command));
                break;
            case kCutSceneLoop:
                command = loopCutSceneAnimation(commandAs<CutSceneValueCommand>(command));
                break;
            case kCutSceneRun:
                command = runLinkedCutScene(commandAs<CutSceneScriptCommand>(command), atOnce);
                break;
            case kCutScenePlace:
                command = placeCutSceneObject(commandAs<CutScenePlace>(command));
                break;
            case kCutSceneNamedScript1:
            case kCutSceneNamedScript2:
            case kCutSceneSetUp:
            case kCutSceneAtOnce:
                command = nextCommand(commandAs<CutSceneScriptCommand>(command));
                break;
            default:
                ee::io::hang(); // the bug kept: the command is never passed
            }
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, runCutSceneScript);

    // gatherCutSceneMotions: the cut scene library's motions a linked script plays (kCutScenePlayLoaded) noted,
    // put in the library's order and loaded.
    inline void gatherCutSceneMotions(ee::Ptr<uint32_t> script)
    {
        ee::Ptr<uint32_t> command = script;
        while (*command != kCutSceneEnd)
        {
            switch (*command)
            {
            case kCutScenePlay:
                command = nextCommand(commandAs<CutScenePlay>(command));
                break;
            case kCutScenePlayLoaded:
                addCutSceneMotion(commandAs<CutScenePlay>(command)->motion);
                command = nextCommand(commandAs<CutScenePlay>(command));
                break;
            case kCutSceneStopAnimation:
            case kCutScenePause:
            case kCutSceneResume:
                command = nextCommand(commandAs<CutSceneObjectCommand>(command));
                break;
            case kCutSceneShow:
            case kCutSceneIgnored:
            case kCutSceneLoop:
                command = nextCommand(commandAs<CutSceneValueCommand>(command));
                break;
            case kCutScenePlace:
                command = nextCommand(commandAs<CutScenePlace>(command));
                break;
            case kCutSceneRun:
            case kCutSceneNamedScript1:
            case kCutSceneNamedScript2:
            case kCutSceneSetUp:
            case kCutSceneAtOnce:
                command = nextCommand(commandAs<CutSceneScriptCommand>(command));
                break;
            default: // 4, 9 and past 15: the number alone
                command += 1;
                break;
            }
        }
        sortCutSceneMotions();
        game::tail(loadCutSceneMotions);
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, gatherCutSceneMotions);

    // endCutScene: the cut scene playing ended: its actors' animations run to their ends and their animators
    // freed, the motions loaded for it freed (those copied into the motions' block stay with it), the cut scene
    // library closed.
    inline void endCutScene()
    {
        *g_cutSceneAtOnce = 1;
        for (uint32_t i = 0; i < *g_cutSceneActorCount; ++i)
            if (objectHasAnimator(cutSceneActors()[i]))
            {
                finishAnimation(cutSceneActors()[i]);
                freeAnimatorOf(cutSceneActors()[i]);
            }
        for (uint32_t i = 0; i < *g_cutSceneMotionCount; ++i)
        {
            const ee::Ptr<Motion> motion = cutSceneMotions()[i].motion;
            if (!motion)
                continue;
            const uint32_t block = *g_cutSceneMotionBlock;
            const uint32_t at = ee::Address(motion);
            if (at < block || block + *g_cutSceneMotionBytes < at)
                freeMotion(motion);
        }
        const uint32_t library = *g_cutSceneLibrary;
        *g_cutSceneMotionCount = 0;
        *g_cutSceneActorCount = 0;
        if (library)
            closeAssetLibrary(library);
        *g_cutSceneLibrary = 0;
        *g_cutScenePlaying = 0;
        *g_cutSceneAtOnce = 0;
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, endCutScene);

    // startCutScene: the cut scene playing ended, then each cut scene of the name's hash played (its motions
    // from the cut scene library loaded first, with full rotation keys, into the motions' block); with atOnce its
    // at-once script in place of its script when it has one, every animation run to its end, the animators
    // updated once and the cut scene ended there and then.
    inline void startCutScene(uint32_t nameHash, uint32_t atOnce)
    {
        const uint32_t keyForm = rotationKeyForm();
        endCutScene();
        setRotationKeyForm1();
        setMotionArena(*g_cutSceneMotionBlock, *g_cutSceneMotionBytes);
        *g_cutScenePlaying = 1;
        *g_cutSceneMotionCount = 0;
        *g_cutSceneActorCount = 0;
        *g_cutSceneAtOnce = atOnce;
        for (uint32_t i = 0; i < *g_cutScenesRead; ++i)
        {
            const ee::Ptr<CutScene> scene = *g_cutScenes + i;
            if (scene->nameHash != nameHash)
                continue;
            const ee::Ptr<uint32_t> script(atOnce && scene->atOnceScript ? scene->atOnceScript : scene->script);
            gatherCutSceneMotions(script);
            runCutSceneScript(script, atOnce);
        }
        setRotationKeyForm(keyForm);
        if (atOnce)
        {
            updateAnimators(ee::Float(0.0f));
            game::tail(endCutScene);
        }
    }
    READABLE_FUNCTION_WAITS_ONLY(bmx, startCutScene);

    // cutSceneAtOnce: whether the cut scene playing is played at once (or is ending).
    inline uint32_t cutSceneAtOnce() { return *g_cutSceneAtOnce; }
    READABLE_FUNCTION_ONLY(bmx, cutSceneAtOnce);
}
