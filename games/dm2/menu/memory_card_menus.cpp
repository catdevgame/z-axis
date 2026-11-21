// The front end's memory card menus (memory_card_menus.h): DM2's own code
// (its front end block, registered as script functions by the menu set-up).

#include "memory_card_menus.h"
#include "menu_mode.h"
#include "menu_variables.h"
#include "../action/ui_frame.h"
#include "../editor/editor_state.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/random.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/dm2/menu/memory_card_menus.cpp.h"

namespace game::dm2
{
    namespace
    {
        // zgui's root node (Zgui +0x0C, as AI's g_uiRoot): the scripts run on it.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        // The menu mode (menu_mode.h); the park loaded callbacks clear it
        // themselves and leave the intro's counters alone.
        inline ee::Global<uint32_t, sym::g_menuMode> g_menuMode;
        // The game's volume settings (saved with the game).
        inline ee::Global<uint8_t, sym::g_sfxVolumeSetting> g_sfxVolumeSetting;
        inline ee::Global<uint8_t, sym::g_musicVolumeSetting> g_musicVolumeSetting;

        // The scripts the callbacks run.
        constexpr ee::Ptr<char> kLockUser{sym::commonLockUserScript};               // "PLAY CommonUI.LockUser"
        constexpr ee::Ptr<char> kOptionsFrameInit{sym::optionsFrameInitScript};     // "PLAY MainMenu.OptionsFrame.Init"
        constexpr ee::Ptr<char> kParkMenuInit{sym::parkMenuInitScript};             // "PLAY ParkMenu.MainMenu.Init"
        constexpr ee::Ptr<char> kParkMenuLoadPark{sym::parkMenuLoadParkScript};     // "PLAY Root.ParkMenu.LoadPark"
        constexpr ee::Ptr<char> kSelectLevelInit{sym::selectLevelInitScript};       // "PLAY MainMenu.SelectLevel.Init"
        // "PLAY Root.MainMenu.SelectLevel.LoadParkLoading"
        constexpr ee::Ptr<char> kSelectLevelParkLoading{sym::selectLevelParkLoadingScript};
        constexpr ee::Ptr<char> kCoolStuffFrameInit{sym::coolStuffFrameInitScript}; // "PLAY MainMenu.CoolStuffFrame.Init"
        // "LOADIMAGE LoadScreen UILIB:Load%1d.bmp Root.CommonUI.StartGame": one
        // of the ten load screens, then the game started.
        constexpr uint32_t kStartGameBehindLoadScreen = sym::startGameBehindLoadScreenFormat;
        constexpr uint32_t kLoadScreens = 10;

        // The block a watched replay is loaded into.
        constexpr uint32_t kLoadedReplay = sym::g_loadedReplay;

        void runUiScript(ee::Ptr<char> text)
        {
            zge::zgui::runScriptLines(*g_uiRoot, text);
        }

        void closeUiFrame()
        {
            setUiFrame(0, 0, ee::Ptr<UiFrameRequest>());
        }
    }

    void backToOptionsFrame()
    {
        *g_currentMusicVolume = *g_musicVolumeSetting;
        *g_currentSfxVolume = *g_sfxVolumeSetting;
        setMenuMode(kMenuFrontEnd);
        closeUiFrame();
        runUiScript(kLockUser);
        game::tail(runUiScript, kOptionsFrameInit);
    }
    READABLE_FUNCTION(backToOptionsFrame, Verified);

    void bmxmenuSetModeLoadGame()
    {
        ee::Local<UiFrameRequest> request;
        request->data = 0;
        request->dataHandle = 0;
        request->onClosed = UiFrameCallback(sym::backToOptionsFrame);
        request->onDone = UiFrameCallback();
        setUiFrame(1, kUiFrameLoadGame, request);
        setMenuMode(kMenuLoadGame);
    }
    READABLE_FUNCTION(bmxmenuSetModeLoadGame, Verified);

    void bmxmenuSetModeSaveGame()
    {
        ee::Local<UiFrameRequest> request;
        request->data = 0;
        request->dataHandle = 0;
        request->onClosed = UiFrameCallback(sym::backToOptionsFrame);
        request->onDone = UiFrameCallback();
        setUiFrame(1, kUiFrameSaveGame, request);
        setMenuMode(kMenuSaveGame);
    }
    READABLE_FUNCTION(bmxmenuSetModeSaveGame, Verified);

    void backToParkMenu()
    {
        setMenuMode(kMenuFrontEnd);
        closeUiFrame();
        game::tail(runUiScript, kParkMenuInit);
    }
    READABLE_FUNCTION(backToParkMenu, Verified);

    void parkMenuParkLoaded()
    {
        *g_menuMode = kMenuFrontEnd;
        closeUiFrame();
        game::tail(runUiScript, kParkMenuLoadPark);
    }
    READABLE_FUNCTION(parkMenuParkLoaded, Verified);

    void backToLevelMenu()
    {
        setMenuMode(kMenuFrontEnd);
        closeUiFrame();
        game::tail(runUiScript, kSelectLevelInit);
    }
    READABLE_FUNCTION(backToLevelMenu, Verified);

    void levelMenuParkLoaded()
    {
        *g_menuMode = kMenuFrontEnd;
        closeUiFrame();
        game::tail(runUiScript, kSelectLevelParkLoading);
    }
    READABLE_FUNCTION(levelMenuParkLoaded, Verified);

    // Both park loads run in the load game mode: the menu mode has none of
    // their own.
    void menuSetModeLoadPark()
    {
        ee::Local<UiFrameRequest> request;
        request->onClosed = UiFrameCallback(sym::backToParkMenu);
        request->data = newParkBuffer();
        request->onDone = UiFrameCallback(sym::parkMenuParkLoaded);
        request->dataHandle = 0;
        setUiFrame(1, kUiFrameLoadPark, request);
        setMenuMode(kMenuLoadGame);
    }
    READABLE_FUNCTION(menuSetModeLoadPark, Verified);

    void menuSetModePlayPark()
    {
        ee::Local<UiFrameRequest> request;
        request->onClosed = UiFrameCallback(sym::backToLevelMenu);
        request->data = newParkBuffer();
        request->onDone = UiFrameCallback(sym::levelMenuParkLoaded);
        request->dataHandle = 0;
        setUiFrame(1, kUiFrameLoadPark, request);
        setMenuMode(kMenuLoadGame);
    }
    READABLE_FUNCTION(menuSetModePlayPark, Verified);

    void startLoadedReplay()
    {
        ee::Local<char[0x100]> script;
        closeUiFrame();
        const uint32_t loadScreen = static_cast<uint32_t>(zge::randomNumber()) % kLoadScreens + 1;
        zge::formatString(script, kStartGameBehindLoadScreen, loadScreen);
        zge::zgui::runScriptLines(*g_uiRoot, script);
    }
    READABLE_FUNCTION(startLoadedReplay, Verified);

    void backToCoolStuffFrame()
    {
        closeUiFrame();
        game::tail(runUiScript, kCoolStuffFrameInit);
    }
    READABLE_FUNCTION(backToCoolStuffFrame, Verified);

    void bmxViewReplay()
    {
        ee::Local<UiFrameRequest> request;
        setMenuMode(kMenuViewReplay);
        request->onDone = UiFrameCallback(sym::startLoadedReplay);
        request->onClosed = UiFrameCallback(sym::backToCoolStuffFrame);
        request->data = kLoadedReplay;
        request->dataHandle = 0;
        setUiFrame(1, kUiFrameLoadReplay, request);
    }
    READABLE_FUNCTION(bmxViewReplay, Verified);

    uint32_t loadedReplay()
    {
        return kLoadedReplay;
    }
    READABLE_FUNCTION(loadedReplay, Verified);
}
