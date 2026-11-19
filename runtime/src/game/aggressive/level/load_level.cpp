// A level's load, from the front end's or a run's request to play: every
// system in order, the loading bar moved after each step by how long the
// level took last time, then the players, cameras, scripts and what the game
// type needs.

#include "aggressive/camera.h"
#include "aggressive/conversions.h"
#include "aggressive/globals.h"
#include "aggressive/level/level_object.h"
#include "aggressive/hud/hud.h"
#include "aggressive/level/dynamics.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/skater.h"
#include "zge/zgui/widget.h"
#include "aggressive/zgui.h"

#include "zge/scene/animator.h"
#include "zge/scene/carried_objects.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/memory/regions.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/collision/collision.h"
#include "zge/render/display.h"
#include "zge/render/texture_effects.h"
#include "zge/math/random.h"
#include "zge/scene/follower.h"
#include "zge/zgui/script.h"
#include "aggressive/fx/library.h"
#include "aggressive/objects.h"
#include "aggressive/pads.h"
#include "aggressive/replay.h"
#include "aggressive/script/script.h"
#include "aggressive/sound/events.h"
#include "aggressive/timers.h"
#include "aggressive/zone_memory.h"
#include "aggressive/play_modes.h"
#include "aggressive/level/double_rails.h"
#include "aggressive/level/load.h"
#include "aggressive/level/park_editor.h"
#include "aggressive/level/zones.h"
#include "aggressive/level/closers.h"
#include "aggressive/level/objectives.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/load_level.cpp.h"

namespace aggressive
{
    namespace
    {
        using parts::levelObject;
        constexpr uint32_t kShowLoadingScreen = sym::showLoadingScreen;
        constexpr uint32_t kMemset = sym::memset;            // libc

        constexpr uint32_t kFollowCamera = sym::g_followCameras, kSecondFollowCamera = sym::g_followCameraScene;
        constexpr uint32_t kFirstPark = 10u, kParkCount = 4u;
        constexpr GameType kParkEditing = static_cast<GameType>(5);
        constexpr uint32_t kParkActionNew = 0, kParkActionLoad = 1, kParkActionPlay = 2; // g_parkPlayState
        constexpr uint8_t kCheatedSave = 0x40;               // in the profile's first byte
        constexpr uint64_t kLevelNoScriptObject = 0x01, kLevelPlayersHeld = 0x04; // LevelObject::flags
        constexpr uint64_t kStopTimedStart = 4, kStopMultiplayerStart = 2; // why play stopped: the start pages
        constexpr uint32_t kGameModeLevel = 2, kGameModePaused = 3;
        constexpr uint32_t kLevelLoadedSound = 22;

        constexpr uint32_t kLoadingFolder = sym::g_textAssetsLoading;     // "ASSETS\\Loading\\"
        constexpr uint32_t kPictureFormat = sym::g_textRaw;     // "%s%s\\%s%x.RAW"
        constexpr uint32_t kBasicName = sym::g_textBasic;         // "BASIC"
        constexpr uint32_t kBasicFormat = sym::g_textRaw2;       // "%s%s\\%s%02x.RAW"
        constexpr uint32_t kTutorialLevel = 7;
        constexpr uint32_t kBasicPictures = 20;

        constexpr uint32_t kMultiplayerScript = sym::g_textMultiplayerZsc; // "Multiplayer.zsc"
        constexpr uint32_t kInGameScript = sym::g_textInGameZsc;      // "InGame.zsc"
        constexpr uint32_t kScreenWidthName = sym::g_textInGameUIScreenWidth;   // "InGameUI.ScreenWidth"
        constexpr uint32_t kScreenHeightName = sym::g_textInGameUIScreenHeight;  // "InGameUI.ScreenHeight"
        constexpr uint32_t kPlayerOneObject = sym::g_textScriptObjPlayerOne;   // "ScriptObj_PlayerOne"
        constexpr uint32_t kPiecesFile = sym::g_textAssetsEditorInlinePep;        // "assets\\editor\\Inline.Pep"
        constexpr uint32_t kReplayBytes = 0x2EE0u;

        inline ee::Global<ee::Float, sym::g_defaultFieldOfView> g_displayHorizontalScale;
        inline ee::Global<ee::Float, sym::g_defaultAspect> g_displayVerticalScale;
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelWord;
        inline ee::Global<uint32_t, sym::g_parkPlayState> g_parkAction; // what the park level is for
        inline ee::Global<ee::Ptr<level::ParkBuffer>, sym::g_parkBuffer> g_parkSlot;
        inline ee::Global<uint32_t, sym::g_gameplayCheats> g_gameplayCheats;
        inline ee::Global<uint32_t, sym::g_levelLoadTicks> g_levelLoadTicks;     // the save's, how long this level took
        inline ee::Global<uint32_t, sym::g_lastLevelLoadTicks> g_lastLevelLoadTicks; // the bar's estimate: halfway to each new load
        inline ee::Global<uint8_t, sym::g_levelCollectibles> g_levelCollectibles;
        inline ee::Global<uint8_t, sym::g_levelExtends> g_levelExtends;

        bool splitScreen()
        {
            return *g_gameType == GameType::SplitScreen;
        }

        int32_t playerCount()
        {
            return splitScreen() ? 2 : 1;
        }

        // The rectangle setCameraViewport takes.
        struct ScreenRect
        {
            ee::Float x, y, width, height;
        };

        ee::Ptr<SkaterProfile> savedProfile(uint32_t skater)
        {
            return ee::Ptr<SkaterProfile>(kSkaterRecords + skater * kSkaterRecordSize);
        }
    }

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // showLoadingScreen: the level's picture, a numbered one when the skater has won
    // its editor pieces (its bit in the record), else one of the 20 plain ones. The
    // tutorial's picture is by the pad's layout.
    void showLoadingScreen(uint32_t levelIndex, uint32_t skater)
    {
        ee::Local<char[0x400]> path;
        const int32_t played = savedProfile(skater)->editorPieceLevels & (1 << (levelIndex & 0x1Fu));
        memset(path, 0, 0x400);
        zge::resetCameraViewports(0x280, 0x1C0);
        const auto format = [&](uint32_t layout, uint32_t name, uint32_t choice) {
            zge::formatString(path, layout, kLoadingFolder, name, name, choice);
        };
        if (played)
        {
            const uint32_t scene = level::levelSceneName(levelIndex, 0);
            if (levelIndex == kTutorialLevel)
            {
                const uint32_t layout = padLayoutOption(0);
                format(kPictureFormat, scene, layout);
            }
            else
            {
                const uint32_t random = zge::randomNumber();
                format(kPictureFormat, scene, random & 3u);
            }
        }
        if (path[0] == 0)
        {
            const uint32_t random = zge::randomNumber();
            format(kBasicFormat, kBasicName, random % kBasicPictures);
        }
        zge::showRawImage(path, 0x280, 0x1C0, 24);
    }
    READABLE_FUNCTION_WAITS(showLoadingScreen, Verified);

    namespace
    {

        // The scene camera a follow camera drives.
        ee::Ptr<zge::Camera> sceneCamera(uint32_t followCamera)
        {
            return zge::sceneOf(ee::Ptr<zge::SceneObject>(ee::Ptr<FollowCamera>(followCamera)->node)).cast<zge::Camera>();
        }
    }

    // loadLevel: every system in the order it needs, the loading bar moved
    // after each step by how long this level took last time.
    uint32_t loadLevel()
    {
        ee::Local<ScreenRect> viewport;
        ee::Local<int32_t> screenWidth;
        ee::Local<int32_t> screenHeight;
        ee::Local<uint32_t> playerOneHash;
        const uint32_t start = zge::collisionClock();
        setGameMode(2);
        setPlayPhase(4);
        showLoadingScreen(*g_levelWord, (*g_playerSkaters)[0]);
        level::showLoadingProgress(ee::Float(0.0f));

        // The bar: this load's time over the last one's.
        const auto moveBar = [&] {
            const ee::Float elapsed = unsignedToFloat(zge::collisionClock() - start);
            const ee::Float expected = unsignedToFloat(*g_lastLevelLoadTicks);
            level::showLoadingProgress(elapsed / expected);
        };

        const ee::Ptr<parts::TopState> top = parts::topState();
        if (top->level == 0)
            top->level = zge::heapAllocate(sizeof(LevelObject));
        const ee::Ptr<LevelObject> level = parts::currentLevel();
        level->flags = ((level->flags & ~kLevelNoScriptObject) | kLevelLoading) & ~kLevelPlayersHeld;
        initTimers();
        moveBar();
        zge::initCarriedAnimals();
        moveBar();
        level->runTimer = 0;
        setupFollowCameras(static_cast<uint32_t>(*g_gameType));
        moveBar();
        newZoneMemory();
        moveBar();
        level::setupLevelContacts();
        moveBar();
        aggressive::level::initCloserQueries();
        moveBar();
        setupKeyedValues();
        moveBar();
        zge::initAnimators();
        moveBar();
        script::registerScriptNatives();
        moveBar();
        aggressive::level::initObjectives();
        moveBar();
        level::registerParkEditorNatives();
        moveBar();
        level::loadLevelScene(*g_levelWord);
        moveBar();
        level::limitSmallModelDistances();
        moveBar();
        moveBar();
        level->mode = 0;
        if (splitScreen())
            zge::zgui::loadZguiScript(kMultiplayerScript, *g_uiRoot);
        else
        {
            const ee::Ptr<zgui::Node> widget = *g_uiRoot;
            *screenWidth = 0x200;
            *screenHeight = 0x1C0;
            zge::zgui::loadZguiScript(kInGameScript, widget);
            zge::zgui::setVariable(*g_uiRoot, kScreenWidthName, screenWidth);
            zge::zgui::setVariable(*g_uiRoot, kScreenHeightName, screenHeight);
        }
        moveBar();
        hud::setupOverlay();
        moveBar();
        skater::loadSkaterMotions();
        moveBar();
        moveBar();

        // Each player's profile from the save, and set-up.
        const ee::Ptr<skater::Player> firstPlayer = parts::levelPlayer(0), secondPlayer = parts::levelPlayer(1);
        for (int32_t index = 0; index < playerCount(); ++index)
        {
            const ee::Ptr<skater::Player> player = parts::levelPlayer(index);
            zge::memoryCopy(skater::skaterProfile(player), savedProfile((*g_playerSkaters)[index]), sizeof(SkaterProfile));
            player->number = static_cast<uint8_t>(index);
            skater::setupSkaterForRun(player);
            moveBar();
        }
        aggressive::skater::setupSkater(firstPlayer);
        moveBar();
        if (splitScreen())
            aggressive::skater::setupSkater(secondPlayer);
        moveBar();

        // Cheats on: every player's save says so.
        if (*g_gameplayCheats)
        {
            for (int32_t index = 0; index < playerCount(); ++index)
                skater::skaterProfile(parts::levelPlayer(index))->index |= kCheatedSave;
        }
        level::setupLevelRails();
        moveBar();
        applyLevelCameraSettings(ee::Ptr<FollowCamera>(kFollowCamera));
        if (splitScreen())
            applyLevelCameraSettings(ee::Ptr<FollowCamera>(kSecondFollowCamera));
        moveBar();
        setFollowCameraMode(ee::Ptr<FollowCamera>(kFollowCamera), 0);
        setFollowCameraTarget(ee::Ptr<FollowCamera>(kFollowCamera), firstPlayer);
        if (splitScreen())
        {
            setFollowCameraMode(ee::Ptr<FollowCamera>(kSecondFollowCamera), 0);
            setFollowCameraTarget(ee::Ptr<FollowCamera>(kSecondFollowCamera), secondPlayer);
        }
        moveBar();
        const uint32_t folder = level::levelFolderName(*g_levelWord);
        level::loadLevelArchive(folder);
        moveBar();
        zge::loadLevelPaths();
        moveBar();
        level::registerLevelObjectTypes();
        moveBar();

        // The players' script object; a level without one is flagged.
        *playerOneHash = zge::nameHash(ee::Ptr<const char>(kPlayerOneObject));
        const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(playerOneHash, 1);
        for (int32_t index = 0; index < playerCount(); ++index)
            parts::levelPlayer(index)->script = found ? ee::Address(*found) : ee::Address(0u);
        if (!found)
            level->flags |= kLevelNoScriptObject;
        level::initZones();
        for (int32_t index = 0; index < playerCount(); ++index)
            skater::skaterProfile(parts::levelPlayer(index))->levels[*g_levelWord].collectibles = *g_levelCollectibles;
        for (int32_t index = 0; index < playerCount(); ++index)
            skater::skaterProfile(parts::levelPlayer(index))->levels[*g_levelWord].juiceExtendsTotal = *g_levelExtends;
        moveBar();

        // The run starts by the game type: free skate and the career play at
        // once; a timed run and split screen stop first on their start page.
        if (*g_gameType == GameType::TimedRun)
        {
            enterGameMode(kGameModeLevel);
            parts::setLevelState(kStopTimedStart);
            enterGameMode(kGameModePaused);
        }
        else if (splitScreen())
        {
            parts::setLevelState(kStopMultiplayerStart);
            enterGameMode(kGameModePaused);
        }
        else
            enterGameMode(kGameModeLevel);
        moveBar();
        fx::initInlineFx();
        moveBar();
        setupSoundEmitters();
        moveBar();
        const uint32_t scene = level::levelSceneName(*g_levelWord, 0);
        level::loadLevelScripts(folder, scene);
        moveBar();
        level::restoreCareerLevel();
        moveBar();
        level::hookAnimationEvents();
        moveBar();
        initReplay(kReplayBytes);
        moveBar();
        level::showLoadingProgress(ee::Float(1.0f));

        // The screen: outside the parks, the viewports; split screen shares it
        // left and right.
        const uint32_t park = static_cast<uint32_t>(*g_levelWord) - kFirstPark;
        if (park >= kParkCount)
        {
            viewport->x = viewport->y = ee::Float(0.0f);
            viewport->width = ee::Float(512.0f);
            viewport->height = ee::Float(448.0f);
            zge::resetCameraViewports(0x200, 0x1C0);
            if (splitScreen())
            {
                zge::setCameraViewport(sceneCamera(kFollowCamera), viewport);
                zge::setCameraViewport(sceneCamera(kSecondFollowCamera), viewport);
                viewport->width = viewport->width * ee::Float(0.5f);
                zge::swapCameraViewport(sceneCamera(kFollowCamera), viewport);
                viewport->x = viewport->x + viewport->width;
                zge::swapCameraViewport(sceneCamera(kSecondFollowCamera), viewport);
            }
            else
            {
                zge::setCameraViewport(sceneCamera(kFollowCamera), viewport);
                zge::setCameraScales(sceneCamera(kFollowCamera), *g_displayHorizontalScale, *g_displayVerticalScale);
            }
        }

        // The parks: the editor on a park, new or loaded, or one played.
        const uint32_t parkAction = *g_parkAction;
        if (*g_gameType == kParkEditing)
        {
            if (parkAction == kParkActionNew || parkAction == kParkActionLoad)
            {
                level::enterParkEditor(park, kPiecesFile, 0);
                zge::initTextureFx();
            }
            if (parkAction == kParkActionLoad)
                level::loadParkIntoEditor(park, kPiecesFile, *g_parkSlot);
        }
        else
        {
            if (parkAction == kParkActionPlay)
                level::playSavedPark(park, kPiecesFile, *g_parkSlot);
            zge::initTextureFx();
        }
        const uint32_t ticks = (zge::collisionClock() - start + *g_lastLevelLoadTicks) >> 1;
        *g_levelLoadTicks = ticks;
        *g_lastLevelLoadTicks = ticks;
        level->flags &= ~kLevelLoading;
        setPlayPhase(0);
        soundEvent(kLevelLoadedSound, 0, 0, 0, 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(loadLevel, Verified);
}
