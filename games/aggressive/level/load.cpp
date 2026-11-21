// The level's load (were the sweep's limitSmallModelDistance .. loadLevelScene): the steps
// the level load (loadLevel) runs between the loading bar's updates.
//
//   - the level's names by index (14 levels; 8 the Zoo, cut from the PS2
//     release; 10 on the park editor's parks): its scene (CIVIC, split
//     screen MPCIVIC) and folder (CIVIC, split screen MP\CIVIC);
//   - its files: "assets\levels\<folder>\<file>.bin" scripts (Free Skate
//     and Timed Run load <file>T.BIN), the scene, motions.zal; a park's
//     from "assets\editor\";
//   - the level objects' types by their script names (ScriptObj ..
//     MovableSound), the animation events' hook;
//   - small models drawn less far: a model under 20 units every way whose
//     last level of detail goes to 500 gets the size's distance;
//   - in the Career, the state the level was left in (career.h);
//   - the loading bar (x 40, y 398, up to 550 wide, 10 high).
//

#include "aggressive/level/load.h"
#include "aggressive/career.h"
#include "aggressive/globals.h"
#include "aggressive/script/script.h"
#include "aggressive/skater/skater.h"

#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "zge/assets/geo.h"
#include "zge/math/vec.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/text/cstring.h"
#include "zge/platform/platform.h"
#include "zge/render/display.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "sce/libgcc.h"
#include "aggressive/game_save.h"
#include "aggressive/script/child_states.h"
#include "aggressive/level/animated_objects.h"
#include "aggressive/level/npc_types.h"
#include "aggressive/level/objectives.h"
#include "aggressive/level/props.h"

#include <cstring>
#include "sym/aggressive/level/load.cpp.h"

namespace aggressive::level
{
    namespace
    {
        // Engine calls by address.
        constexpr uint32_t kMemset = sym::memset;          // libc (dst, byte, bytes)
        constexpr uint32_t kStrcat = sym::appendString;          // (dst, src) the engine's wrapper
        // (callback, data): the callback on each of the collision world's
        // models while it returns nonzero.
        // (x, y, width, height, rgba*): a filled rectangle on the loading screen.
        // (name, create, attach): an object type of the level's scripts:
        // the object made for it, then set up (its message handlers).
        // The animation events' hook: set when none is, else cleared (the
        // hook before back).
        constexpr uint32_t kHandleAnimationEvent = sym::handleAnimationEvent; // (sceneObject, event)
        // (scene name): the level's scene file (its FSCN chunk) read, once.
        // The PropDef props' animations ("zoo_assets" in the other builds'
        // messages: PropDef_BubbleGL .. PropDef_IconEditor).
        // The level's object list, its search tree and its locators.
        // The career's level state back, block by block.
        // ... and kept, block by block: each part's bytes into the frame's
        // word, a buffer back (saveCareerLevel).

        // The object types' callbacks (kRegisterObjectType).
        constexpr uint32_t kCreateEntity = sym::createLevelEntity; // an entity object, unless the object has one
        constexpr uint32_t kAttachScriptObject = sym::attachScriptObject;
        constexpr uint32_t kAttachAnimObject = sym::attachAnimObject;
        constexpr uint32_t kAttachCameraAnim = sym::subscribeDynamicsObjectMotion;
        constexpr uint32_t kAttachDynamicsObject = sym::initDynamicsLevelObject;
        constexpr uint32_t kCreateNpc = sym::spawnNpcModelIfReady;
        constexpr uint32_t kAttachNpc = sym::initNpcObject;
        constexpr uint32_t kAttachCar = sym::newTrafficCar;
        constexpr uint32_t kAttachGlass = sym::initGlass;
        constexpr uint32_t kAttachMovableSound = sym::initMovableSound;

        // Strings (.sdata, .rodata).
        constexpr uint32_t kLevelsFolder = sym::levelsFolder; // "assets\levels\"
        constexpr uint32_t kEditorFolder = sym::editorFolder; // "assets\editor\"
        constexpr uint32_t kBackslash = sym::backslash;    // "\"
        constexpr uint32_t kTimedSuffix = sym::timedSuffix;  // "T" (Free Skate's and Timed Run's scripts)
        constexpr uint32_t kBinExtension = sym::binExtension; // ".bin"
        constexpr uint32_t kMotionsFile = sym::motionsFile;  // "motions.zal"
        // The loading bar's colour (RGBA bytes B0 8E 15 C0).
        constexpr uint32_t kLoadingBarColour = sym::loadingBarColour;
        constexpr uint32_t kScriptObj = sym::scriptObj;      // "ScriptObj"
        constexpr uint32_t kScriptPropDef = sym::scriptPropDef;  // "ScriptPropDef"
        constexpr uint32_t kAnimObj = sym::animObj;        // "AnimObj"
        constexpr uint32_t kAnimPropDef = sym::animPropDef;    // "AnimPropDef"
        constexpr uint32_t kCameraAnim = sym::cameraAnim;     // "CameraAnim"
        constexpr uint32_t kDynamicsObject = sym::dynamicsObject; // "DynamicsObject"
        constexpr uint32_t kNpc = sym::npc;            // "NPC"
        constexpr uint32_t kCar = sym::car;            // "Car"
        constexpr uint32_t kGlass = sym::glass;          // "Glass"
        constexpr uint32_t kMovableSound = sym::movableSound;   // "MovableSound"

        // The level tables (32-byte names, by level index): scenes, split
        // screen scenes, folders, split screen folders.
        constexpr uint32_t kLevelScenes = sym::levelScenes;
        constexpr uint32_t kSplitScreenScenes = sym::splitScreenScenes;
        constexpr uint32_t kLevelFolders = sym::levelFolders;
        constexpr uint32_t kSplitScreenFolders = sym::splitScreenFolders;
        constexpr uint32_t kLevelNameSize = 32;
        constexpr uint32_t kLevelCount = 14;
        constexpr uint32_t kFirstPark = 10; // levels from here on are the park editor's

        // The small models' draw distances by size (.data g_smallModelDistances): a
        // model smaller than `size` every way is drawn to `distance`.
        struct SmallModelDistance
        {
            ee::Float size;
            ee::Float distance;
        };
        inline ee::Global<SmallModelDistance[4], sym::g_smallModelDistances> g_smallModelDistances;
        inline constexpr ee::Float kSmallModelSize = 20.0f;
        inline constexpr ee::Float kDefaultLastDistance = 500.0f; // the exporter's: such models' last level

        // The loading bar: where, how big, and the load's progress (0 .. 1)
        // as its width.
        constexpr int32_t kLoadingBarX = 40;
        constexpr int32_t kLoadingBarY = 398;
        constexpr int32_t kLoadingBarHeight = 10;
        constexpr uint32_t kLoadingBarMaxWidth = 550;
        constexpr uint32_t kLoadingBarMinWidth = 5;
        // .sdata: the bar's whole width as a float (550).
        inline ee::Global<ee::Float, sym::g_loadingBarWidth> g_loadingBarWidth;

        // The game state the original reads from one base (g_debugDisplay).
        constexpr uint32_t kGameStateBase = sym::g_debugDisplay;
    }

    // limitSmallModelDistance (kForEachWorldModel's callback): a small model's last
    // level of detail drawn only as far as its size's distance. Always 1
    // (the walk goes on).
    uint32_t limitSmallModelDistance(ee::Ptr<zge::GeoObject> model)
    {
        ee::Local<zge::Vec4> extent;
        // The box's extent, every way; the largest.
        *extent = zge::quadwordOf(ee::Ptr<zge::Float4>(&model->boundsMax)) - zge::quadwordOf(ee::Ptr<zge::Float4>(&model->boundsMin));
        const ee::Float largest = ee::max(ee::max(extent->y, extent->x), extent->z);
        if (!(largest < kSmallModelSize))
            return 1;
        // Each part's last level of detail, the last part first; only one still at the
        // exporter's distance is limited.
        for (int32_t index = zge::modelPartCount(model) - 1; index != -1; --index)
        {
            const ee::Ptr<zge::CollisionFrame> part = zge::classModel(model, index);
            const uint32_t levels = zge::frameLevels(part);
            const ee::Ptr<zge::FramePart> last = zge::frameLevelPart(part, levels - 1);
            const ee::Float distance = last->distance;
            if (!(distance == kDefaultLastDistance))
                continue;
            // The first size class the model is smaller than.
            for (const SmallModelDistance &sizeClass : *g_smallModelDistances)
            {
                if (largest < sizeClass.size)
                {
                    last->distance = ee::min(sizeClass.distance, distance);
                    break;
                }
            }
        }
        return 1;
    }
    READABLE_FUNCTION(limitSmallModelDistance, Verified);

    // limitSmallModelDistances: limitSmallModelDistance over the world's models.
    uint32_t limitSmallModelDistances()
    {
        return game::tail(zge::forEachWorldObject, sym::limitSmallModelDistance, 0);
    }
    READABLE_FUNCTION_WAITS(limitSmallModelDistances, Verified);

    namespace
    {
        void memset(ee::Ptr<uint8_t> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // loadLevelScripts: the level's script objects from "<folder>\<file>.bin"
    // (Free Skate and Timed Run: <file>T.bin) under assets\levels\, a park's
    // from assets\editor\ (its file name without the first two letters).
    void loadLevelScripts(uint32_t folder, uint32_t file)
    {
        ee::Local<uint8_t[0xC0]> path;
        memset(path, 0, 0x80);
        if (*skater::g_levelWord < kFirstPark)
        {
            zge::stringCopy(path, kLevelsFolder);
            zge::appendString(path, folder);
            zge::appendString(path, kBackslash);
            zge::appendString(path, file);
            const GameType type = *g_gameType;
            if (type == GameType::FreeSkate || type == GameType::TimedRun)
                zge::appendString(path, kTimedSuffix);
            zge::appendString(path, kBinExtension);
        }
        else
        {
            zge::stringCopy(path, kEditorFolder);
            zge::appendString(path, folder);
            zge::appendString(path, kBackslash);
            zge::appendString(path, file + 2);
            zge::appendString(path, kBinExtension);
        }
        aggressive::script::loadScriptObjects(path);
    }
    READABLE_FUNCTION_WAITS(loadLevelScripts, Verified);

    // showLoadingProgress: the loading bar at `progress` (0 .. 1) of its width,
    // 5 to 550 pixels.
    void showLoadingProgress(ee::Float progress)
    {
        ee::Local<uint8_t[4]> colour;
        const ee::Float wholeWidth = *g_loadingBarWidth;
        std::memcpy(&*colour, ee::Ptr<uint8_t>(kLoadingBarColour), 4);
        const uint32_t width = sce::libgcc::fptoui(progress * wholeWidth);
        const uint32_t capped = width < kLoadingBarMaxWidth + 1 ? width : kLoadingBarMaxWidth;
        zge::clearDisplayRectangle(kLoadingBarX, kLoadingBarY, kLoadingBarMinWidth - 1 < capped ? capped : kLoadingBarMinWidth, kLoadingBarHeight, colour);
    }
    READABLE_FUNCTION(showLoadingProgress, Verified);

    // loadLevelArchive: the level's motions archive, "<folder>\motions.zal"
    // under assets\levels\ (a park's under assets\editor\).
    void loadLevelArchive(uint32_t folder)
    {
        ee::Local<uint8_t[0xA0]> path;
        memset(path, 0, 0x80);
        if (*skater::g_levelWord < kFirstPark)
            zge::stringCopy(path, kLevelsFolder);
        else
            zge::stringCopy(path, kEditorFolder);
        zge::appendString(path, folder);
        zge::appendString(path, kBackslash);
        zge::appendString(path, kMotionsFile);
        zge::loadAnimationArchive(path);
    }
    READABLE_FUNCTION_WAITS(loadLevelArchive, Verified);

    // hookAnimationEvents: the animation events go to handleAnimationEvent.
    void hookAnimationEvents()
    {
        game::tail(zge::toggleAnimationEventHook, kHandleAnimationEvent);
    }
    READABLE_FUNCTION(hookAnimationEvents, Verified);

    // unhookAnimationEvents: the animation events' hook taken off again.
    void unhookAnimationEvents()
    {
        game::tail(zge::toggleAnimationEventHook, 0);
    }
    READABLE_FUNCTION(unhookAnimationEvents, Verified);

    // registerLevelObjectTypes: the level scripts' object types, by the names their
    // objects carry; then the props' animations and the level's objects
    // and locators. 1.
    uint32_t registerLevelObjectTypes()
    {
        struct ObjectType
        {
            uint32_t name, create, attach;
        };
        static constexpr ObjectType kTypes[] = {
            {kScriptObj, kCreateEntity, kAttachScriptObject},
            {kScriptPropDef, kCreateEntity, kAttachScriptObject},
            {kAnimObj, kCreateEntity, kAttachAnimObject},
            {kAnimPropDef, kCreateEntity, kAttachAnimObject},
            {kCameraAnim, kCreateEntity, kAttachCameraAnim},
            {kDynamicsObject, kCreateEntity, kAttachDynamicsObject},
            {kNpc, kCreateNpc, kAttachNpc},
            {kCar, kCreateEntity, kAttachCar},
            {kGlass, kCreateEntity, kAttachGlass},
            {kMovableSound, kCreateEntity, kAttachMovableSound},
        };
        for (const ObjectType &type : kTypes)
            zge::registerLevelObjectType(type.name, type.create, type.attach);
        aggressive::setupNpcTypeAnimations();
        return game::tail(zge::spawnLevelObjects);
    }
    READABLE_FUNCTION_WAITS(registerLevelObjectTypes, Verified);

    // restoreCareerLevel: in the Career, the level as the skater left it: its
    // goals, the script's variables, the props, what was shown or hidden
    // and the script objects' states (career.h).
    void restoreCareerLevel()
    {
        const ee::Ptr<CareerLevelState> state = careerLevelState((*g_playerSkaters)[0], *skater::g_levelWord);
        if (*g_gameType != GameType::Career)
            return;
        if (state->goals)
            loadObjectiveState(state->goals);
        if (state->scriptVariables)
            script::scriptRestoreVariables(state->scriptVariables);
        if (state->props)
            loadLevelPropState(state->props);
        if (state->visibility)
            aggressive::script::restoreChildStates(state->visibility, state->visibilityCount);
        if (!state->objectStates)
            return;
        game::tail(restoreAnimObjectStates, state->objectStates, state->objectStatesSize);
    }
    READABLE_FUNCTION_WAITS(restoreCareerLevel, Verified);

    // saveCareerLevel (saveCareerProgress' tail call, on leaving a level): in
    // the Career, the level as the skater leaves it kept in its career
    // level state (career.h), part by part in the save's region (each old
    // block freed first): the script's variables, the goals, the props,
    // what is shown or hidden and the script objects' states.
    void saveCareerLevel()
    {
        ee::Local<uint32_t> bytes;
        const ee::Ptr<CareerLevelState> state = careerLevelState((*g_playerSkaters)[0], *skater::g_levelWord);
        if (*g_gameType != GameType::Career)
            return;
        // A part kept: its old block freed, the part fetched (its size in `bytes`)
        // and copied into a new block of the save's region.
        const auto keep = [&](auto &block, uint32_t &size, auto fetch, bool freeFetched) {
            if (block)
            {
                aggressive::saveRegionFree(block);
                block = {};
            }
            const ee::Address fetched = fetch();
            size = *bytes;
            if (size)
            {
                block = std::remove_reference_t<decltype(block)>(aggressive::saveRegionAllocate(size));
                zge::memoryCopy(block, fetched, *bytes);
                if (freeFetched)
                    zge::freeMemory(fetched);
            }
        };
        keep(state->scriptVariables, state->scriptVariablesSize, [&] { return script::scriptVariables(bytes); }, false);
        keep(state->goals, state->goalsSize, [&] { return saveObjectiveState(bytes); }, false);
        keep(state->props, state->propsSize, [&] { return saveLevelPropState(bytes); }, false);
        keep(state->visibility, state->visibilityCount, [&] { return aggressive::script::saveChildStates(bytes); }, true);
        // The objects' states written straight into their block.
        if (state->objectStates)
        {
            aggressive::saveRegionFree(state->objectStates);
            state->objectStates = {};
        }
        const uint32_t stateBytes = animObjectStateBytes();
        *bytes = stateBytes;
        state->objectStatesSize = stateBytes;
        if (stateBytes)
        {
            state->objectStates = ee::Ptr<AnimObjectState>(aggressive::saveRegionAllocate(stateBytes));
            saveAnimObjectStates(state->objectStates);
        }
    }
    READABLE_FUNCTION_WAITS(saveCareerLevel, Verified);

    // levelSceneName: the level's scene name (32 bytes in the level table);
    // in split screen with `splitScreenScene` the split screen one (MP...).
    // 0 past the table.
    uint32_t levelSceneName(uint32_t level, uint8_t splitScreenScene)
    {
        if (level >= kLevelCount)
            return 0;
        if (*g_gameType == GameType::SplitScreen && splitScreenScene)
            return kSplitScreenScenes + (level << 5);
        return kLevelScenes + (level << 5);
    }
    READABLE_FUNCTION(levelSceneName, Verified);

    // levelFromName: a level's index by its scene name, 14 when the name is
    // none of them (the table is copied to the stack first, as the original
    // does; the DM2 and Estate overrides hook it for their levels).
    int32_t levelFromName(ee::Address name)
    {
        constexpr uint32_t kLevels = 14;
        ee::Local<uint32_t[kLevels]> names;
        for (uint32_t i = 0; i < kLevels; ++i)
            names[i] = *(ee::Ptr<uint32_t>(sym::levelNames) + i);
        if (name == 0)
            return kLevels;
        for (uint32_t i = 0; i < kLevels; ++i)
            if (zge::compareStrings(names[i], name) == 0)
                return static_cast<int32_t>(i);
        return kLevels;
    }
    READABLE_FUNCTION(levelFromName, Verified);

    // levelFolderName: the level's folder name (split screen: MP\...); 0 past
    // the table.
    uint32_t levelFolderName(uint32_t level)
    {
        if (level >= kLevelCount)
            return 0;
        if (*g_gameType == GameType::SplitScreen)
            return kSplitScreenFolders + (level << 5);
        return kLevelFolders + (level << 5);
    }
    READABLE_FUNCTION(levelFolderName, Verified);

    // loadLevelScene: the level's scene file (its split screen scene in
    // split screen).
    uint32_t loadLevelScene(uint32_t level)
    {
        const uint32_t scene = levelSceneName(level, 1);
        return game::tail(zge::enterScene, scene);
    }
    READABLE_FUNCTION_WAITS(loadLevelScene, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelNumber;
    }

    // setLevelWord: the level's number set. What it answers is what the original's result register holds:
    // the high half of the level number's address, the base of its store.
    int32_t setLevelWord(uint32_t levelWord)
    {
        *g_levelNumber = levelWord;
        return ee::high(sym::g_levelNumber);
    }
    READABLE_FUNCTION(setLevelWord, Verified);
}
