// The front end (front_end.h): DM2's own code (its game block, in the
// physics.c anchor's range).

#include "front_end.h"
#include "menu_mode.h"
#include "menu_variables.h"
#include "../action/action.h"
#include "../action/action_state.h"
#include "../action/ui_frame.h"
#include "../game/players.h"
#include "../game/round.h"
#include "../sound/menu_sounds.h"
#include "../sound/sound_mode.h"
#include "../input/pads.h"
#include "../level/level_load_progress.h"
#include "../rider/rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "dm2/editor/edit_gui.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/math/types.h"
#include "zge/render/display.h"
#include "zge/movie/movie.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/dm2/menu/front_end.cpp.h"

namespace game::dm2
{
    namespace rider
    {
        PadInput *padInput(uint32_t port); // input.cpp
    }

    namespace
    {
        // Whether the main menu offers multiplayer now (both pads are
        // connected): its scripts run only when that changes.
        inline ee::Global<uint32_t, sym::g_multiplayerOffered> g_multiplayerOffered;
        // "PLAY MainMenu.MainMenuFrame.UnlockMultiPlayer", "... LockMultiPlayer"
        constexpr ee::Ptr<char> kUnlockMultiplayer{sym::unlockMultiPlayerScript};
        constexpr ee::Ptr<char> kLockMultiplayer{sym::lockMultiPlayerScript};

        // The menu mode's word (menu_mode.cpp keeps it).
        inline ee::Global<uint32_t, sym::g_menuMode> g_menuModeWord;
        // The saved settings' volumes.
        inline ee::Global<uint8_t, sym::g_sfxVolumeSetting> g_sfxVolumeSetting;
        inline ee::Global<uint8_t, sym::g_musicVolumeSetting> g_musicVolumeSetting;
        // Where the front end opens (a FrontEndOpens).
        inline ee::Global<uint8_t, sym::g_frontEndOpens> g_frontEndOpens;

        constexpr uint32_t kParkMenuScriptFile = sym::parkMenuScriptFile; // "ParkMenu.zsc"
        constexpr uint32_t kMainMenuScriptFile = sym::mainMenuScriptFile; // "main.zsc"
        constexpr ee::Ptr<char> kMainMenuIntro{sym::mainMenuIntroScript}; // "PLAY MainMenu.Intro"
        // "SET MainMenu.PlayerCount {1}\nSET MainMenu.PlayerIndex {1}"
        constexpr ee::Ptr<char> kOnePlayer{sym::mainMenuOnePlayerScript};
        constexpr ee::Ptr<char> kBikeChoiceInit{sym::selectBikeInitScript};     // "PLAY MainMenu.SelectBike.Init"
        constexpr ee::Ptr<char> kRiderChoiceInit{sym::selectPlayerInitScript};  // "PLAY MainMenu.SelectPlayer.Init"
        constexpr ee::Ptr<char> kLevelChoiceInit{sym::selectLevelInitScript};   // "PLAY MainMenu.SelectLevel.Init"
        constexpr ee::Ptr<char> kMainMenuFrameInit{sym::mainMenuFrameInitScript}; // "PLAY MainMenu.MainMenuFrame.Init"
        // After a watched replay: the still background ("SET Root.MovieID
        // {19}\nSET Root.ModalPlayerNum {0}\nCALL BmxStartMovie"), the UI
        // faded in with the user locked, and the Cool Stuff frame.
        constexpr ee::Ptr<char> kStillBackground{sym::stillBackgroundScript};
        constexpr ee::Ptr<char> kLockedFadeIn{sym::lockUserFadeInScript};
        constexpr ee::Ptr<char> kCoolStuffInit{sym::rootCoolStuffFrameInitScript};

        // The screen the menus draw to.
        constexpr uint32_t kScreenWidth = 640, kScreenHeight = 448;
        constexpr uint32_t kLevelMenuEntries = 9;

        namespace tail
        {
            // Scaffolding (address-call): the engine's chunk reader told its progress hook (no header
            // declares it for game code yet); bmxStartGame and bmxmenuPlayPark end in it (the original's jump).
            uint32_t setChunkProgressHook(uint32_t hook)
            {
                return ee::tailCall<uint32_t>(sym::setChunkProgressHook, hook);
            }
        }

        // The menus' variables put back from the session just played: the
        // game picked, each player's rider, bike and outfit, the player
        // count, and the level menu's entry for the session's level (one
        // past the last entry when no entry is that level).
        void restoreMenuChoices()
        {
            const ee::Ptr<PlayerSetup> setups(sym::g_playerSetups);
            *g_currentRideType = *g_sessionSinglePlayerMode;
            (*g_currentRiders)[0] = setups[0].character;
            (*g_currentRiders)[1] = setups[1].character;
            (*g_currentBikes)[0] = setups[0].bike;
            (*g_currentBikes)[1] = setups[1].bike;
            (*g_currentRiderOutfits)[0] = setups[0].outfit;
            (*g_currentRiderOutfits)[1] = setups[1].outfit;
            *g_currentRiderCount = *g_multiplayerPlayers;
            const uint8_t level = *g_sessionLevel;
            uint32_t entry = 0;
            while (entry < kLevelMenuEntries && (*g_levelMenuGameLevels)[entry] != level)
                ++entry;
            *g_currentLevel = entry;
        }

        // The game's single-player mode (game/round.cpp keeps it).
        inline ee::Global<uint8_t, sym::g_singlePlayerMode> g_singlePlayerModeByte;

        // The menus' choices made the players' set-up: each player's
        // character, bike and outfit.
        // Bug kept: the character's record is looked up before the player's
        // new character is stored, so the first player's bike (kept for
        // ProQuest and competition games) goes to the record of the
        // character picked for the game before this one.
        void takeMenuChoices()
        {
            const ee::Ptr<PlayerSetup> setups(sym::g_playerSetups);
            for (uint32_t player = 0; player < kMaxPlayers; ++player)
            {
                const ee::Ptr<RiderCharacter> character = playerCharacter(player);
                PlayerSetup &setup = setups[player];
                setup.character = static_cast<uint8_t>((*g_currentRiders)[player]);
                setup.bike = static_cast<uint8_t>((*g_currentBikes)[player]);
                setup.outfit = static_cast<uint8_t>((*g_currentRiderOutfits)[player]);
                if (player == 0 && *g_sessionSinglePlayerMode <= kCompetition)
                    character->bike = static_cast<uint8_t>((*g_currentBikes)[0]);
            }
        }

        // The level state asked for with the load screen's progress at
        // nothing and the level load's progress hook set (the original ends
        // in the engine's function).
        uint32_t startLevelLoad()
        {
            setGameState(kGameStateLevel);
            for (uint32_t part = 0; part < kLevelLoadParts; ++part)
                (*g_levelLoadParts)[part] = 0.0f;
            return tail::setChunkProgressHook(sym::levelLoadProgress);
        }

        bool padConnected(uint32_t port)
        {
            return (ee::Ptr<zge::Pad>(rider::padInput(port)->pad)->flags & zge::kPadConnected) != 0;
        }

        // zgui's root node (Zgui +0x0C, as AI's g_uiRoot).
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        // The main menu's zgui script is loaded (the front end's and the
        // intro's enter load it once), and the menu scene is entered
        // (enterMenuScene).
        inline ee::Global<uint32_t, sym::g_mainMenuScriptLoaded> g_mainMenuScriptLoaded;
        inline ee::Global<uint32_t, sym::g_menuSceneEntered> g_menuSceneEntered;
        constexpr ee::Ptr<const char> kParkMenuName{sym::g_textParkMenu}; // "ParkMenu"
        constexpr ee::Ptr<const char> kMainMenuName{sym::g_textMainMenu}; // "MainMenu"

        // The menu scene: a small scene of its own for the menus' models,
        // its cells this size, and its camera (the camera code lets it go
        // before the level's is made).
        inline ee::Global<zge::Vec4, sym::g_menuSceneCellSize> g_menuSceneCellSize; // (50, 50, 50)
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_menuSceneCamera> g_menuSceneCamera;
        struct SceneBounds
        {
            zge::Vec4 min;
            zge::Vec4 max;
        };
        constexpr float kMenuSceneReach = 100.0f;
        // The camera sits at the scene's origin and looks down +Z.
        constexpr float kMenuCameraTargetZ = 500.0f;
        constexpr uint32_t kMenuCameraFlags = 1;

        // Scaffolding (address-call): libc's memset (no ported declaration
        // game code can call by name yet).
        void clear(ee::Ptr<zge::Vec4> vector)
        {
            ee::game(sym::memset, vector, 0u, static_cast<uint32_t>(sizeof(zge::Vec4)));
        }

        // DM2's movieClose and findNode take no hand-on word and no kind:
        // the shared declarations' last parameters are passed as 0.
        void deleteUiNode(ee::Ptr<const char> name)
        {
            const ee::Ptr<zge::zgui::Node> node = zge::zgui::findNode(*g_uiRoot, name, 0);
            if (node)
                zge::deleteSceneObject(widgetObject(node));
        }
    }

    void enterMenuScene()
    {
        if (*g_menuSceneEntered)
            return;
        ee::Local<zge::Vec4> eye;
        ee::Local<zge::Vec4> target;
        ee::Local<zge::Vec4> cellSize;
        ee::Local<SceneBounds> bounds;
        clear(eye);
        clear(target);
        *cellSize = *g_menuSceneCellSize;
        target->z = kMenuCameraTargetZ;
        // The bounds' fourth components are left as they are (the original
        // never writes them).
        bounds->min.x = bounds->min.y = bounds->min.z = -kMenuSceneReach;
        bounds->max.x = bounds->max.y = bounds->max.z = kMenuSceneReach;
        zge::enterUnlistedScene(cellSize, bounds);
        const ee::Ptr<zge::SceneObject> camera(zge::newSceneCamera(kMenuCameraFlags));
        *g_menuSceneCamera = camera;
        zge::lookFromAt(camera, eye, target, 0);
        *g_menuSceneEntered = 1;
    }
    READABLE_FUNCTION(enterMenuScene, Verified);

    void frontEndExit()
    {
        zge::movieClose(0);
        deleteUiNode(kParkMenuName);
        deleteUiNode(kMainMenuName);
        *g_mainMenuScriptLoaded = 0;
        zge::leaveScene();
        *g_menuSceneEntered = 0;
    }
    READABLE_FUNCTION_WAITS(frontEndExit, Verified);

    uint32_t frontEndEnter()
    {
        *g_currentSfxVolume = *g_sfxVolumeSetting;
        *g_currentMusicVolume = *g_musicVolumeSetting;
        zge::releasePad(ee::Ptr<zge::Pad>(rider::padInput(0)->pad));
        zge::releasePad(ee::Ptr<zge::Pad>(rider::padInput(1)->pad));
        zge::resetCameraViewports(kScreenWidth, kScreenHeight);
        *g_multiplayerOffered = 1;
        enterMenuScene();
        zge::zgui::loadZguiScript(kParkMenuScriptFile, *g_uiRoot);
        if (*g_mainMenuScriptLoaded == 0)
        {
            zge::zgui::loadZguiScript(kMainMenuScriptFile, *g_uiRoot);
            *g_mainMenuScriptLoaded = 1;
        }
        *g_multiplayerPlayers = 1;
        zge::zgui::g_zgui->input.idleFrames = 0;
        bmxUpdateVolumeInfo();
        setGameMode(kSoundModeFrontEnd);

        if (*g_menuModeWord == kMenuFrontEnd)
        {
            zge::zgui::runScriptLines(*g_uiRoot, kMainMenuIntro);
            switch (*g_frontEndOpens)
            {
            case kOpensBikeChoice:
                restoreMenuChoices();
                zge::zgui::runScriptLines(*g_uiRoot, kOnePlayer);
                zge::zgui::runScriptLines(*g_uiRoot, kBikeChoiceInit);
                break;
            case kOpensRiderChoice:
                restoreMenuChoices();
                zge::zgui::runScriptLines(*g_uiRoot, kOnePlayer);
                zge::zgui::runScriptLines(*g_uiRoot, kRiderChoiceInit);
                break;
            case kOpensLevelChoice:
                restoreMenuChoices();
                zge::zgui::runScriptLines(*g_uiRoot, kOnePlayer);
                zge::zgui::runScriptLines(*g_uiRoot, kLevelChoiceInit);
                break;
            case kOpensMainMenu:
                *g_currentLevel = 0;
                zge::zgui::runScriptLines(*g_uiRoot, kMainMenuFrameInit);
                break;
            default:
                break;
            }
        }
        else if (*g_menuModeWord == kMenuViewReplay)
        {
            zge::zgui::runScriptLines(*g_uiRoot, kStillBackground);
            zge::zgui::runScriptLines(*g_uiRoot, kLockedFadeIn);
            zge::zgui::runScriptLines(*g_uiRoot, kCoolStuffInit);
        }
        return 1;
    }
    READABLE_FUNCTION(frontEndEnter, Verified);

    uint32_t bmxStartGame()
    {
        // A replay to watch brings its own players and level.
        if (menuMode() == kMenuViewReplay)
        {
            *g_multiplayerPlayers = 1;
        }
        else
        {
            *g_sessionSinglePlayerMode = static_cast<uint8_t>(*g_currentRideType);
            takeMenuChoices();
            *g_multiplayerPlayers = static_cast<uint8_t>(*g_currentRiderCount);
            *g_sessionLevel = (*g_levelMenuGameLevels)[*g_currentLevel];
            setMenuMode(kMenuFrontEnd);
        }
        return startLevelLoad();
    }
    READABLE_FUNCTION(bmxStartGame, Verified);

    uint32_t bmxmenuPlayPark()
    {
        *g_singlePlayerModeByte = static_cast<uint8_t>(*g_currentRideType);
        *g_sessionSinglePlayerMode = static_cast<uint8_t>(*g_currentRideType);
        takeMenuChoices();
        *g_sessionLevel = kParkLevel;
        *g_multiplayerPlayers = static_cast<uint8_t>(*g_currentRiderCount);
        return startLevelLoad();
    }
    READABLE_FUNCTION(bmxmenuPlayPark, Verified);

    uint32_t frontEndFrame()
    {
        readPads();
        // Multiplayer needs both pads.
        if (padConnected(0) && padConnected(1))
        {
            if (*g_multiplayerOffered == 0)
            {
                zge::zgui::runScriptLines(*g_uiRoot, kUnlockMultiplayer);
                *g_multiplayerOffered = 1;
            }
        }
        else if (*g_multiplayerOffered != 0)
        {
            zge::zgui::runScriptLines(*g_uiRoot, kLockMultiplayer);
            *g_multiplayerOffered = 0;
        }

        // The memory card frame runs under the load and save menus, the
        // intro's menu and a replay being loaded to watch.
        switch (menuMode())
        {
        case kMenuLoadGame:
        case kMenuSaveGame:
        case kMenuIntroMenu:
        case kMenuViewReplay:
            uiFrameUpdate();
            break;
        default:
            break;
        }
        return 1;
    }
    READABLE_FUNCTION(frontEndFrame, Verified);
}
