// DM2's intro menu (menu_mode.h): the callbacks of the UI frames the intro
// state shows, and the menu's script functions that move the menu mode.
// DM2's own code (its front end block).

#include "front_end.h"
#include "menu_mode.h"
#include "../action/action.h"
#include "../action/ui_frame.h"
#include "../input/pads.h"
#include "../game/players.h"
#include "../rider/rider.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "dm2/editor/edit_gui.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/render/display.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/menu/intro.cpp.h"

namespace game::dm2
{
    namespace rider
    {
        PadInput *padInput(uint32_t port); // input.cpp
    }

    namespace
    {
        // zgui's root node (Zgui +0x0C, as AI's g_uiRoot): the scripts run on it.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;

        // The scripts: the top header shown (three SET lines), the common UI's
        // fade out, and the user locked out of the UI.
        constexpr ee::Ptr<char> kTopHeaderShown{sym::topHeaderShownScript};
        constexpr ee::Ptr<char> kFadeOut{sym::fadeOutScript};
        constexpr ee::Ptr<char> kLockUser{sym::lockUserScript};
        constexpr ee::Ptr<char> kBackgroundVisible{sym::backgroundVisibleScript}; // "SET Root.CommonUI.Background.Visible {1}"

        void runUiScript(ee::Ptr<char> text)
        {
            zge::zgui::runScriptLines(*g_uiRoot, text);
        }

        // The memory card frame turned off, its callbacks cleared.
        void clearUiFrame()
        {
            setUiFrame(0, kUiFrameSaveGame, ee::Ptr<UiFrameRequest>());
        }
        constexpr uint32_t kCloseWaitFrames = 90;

        // The front end's script variables (menu_variables.h), each a Sint32
        // of the UI's root over the game's word, given to zgui the first
        // time the intro state is entered, with the script functions.
        inline ee::Global<uint32_t, sym::g_menuFunctionsPending> g_menuFunctionsPending;
        constexpr uint32_t kSint32TypeName = sym::frontEndSint32TypeName; // "Sint32"
        constexpr uint32_t kSecondPlayer = sizeof(uint32_t); // a two-player variable's second word
        struct MenuVariable
        {
            uint32_t name, value;
        };
        constexpr MenuVariable kMenuVariables[] = {
            {sym::g_textCurrentRider1, sym::g_currentRiders},
            {sym::g_textCurrentBike1, sym::g_currentBikes},
            {sym::g_textCurrentRiderOutfit1, sym::g_currentRiderOutfits},
            {sym::g_textCurrentRider2, sym::g_currentRiders + kSecondPlayer},
            {sym::g_textCurrentBike2, sym::g_currentBikes + kSecondPlayer},
            {sym::g_textCurrentRiderOutfit2, sym::g_currentRiderOutfits + kSecondPlayer},
            {sym::g_textCurrentLevel, sym::g_currentLevel},
            {sym::g_textCurrentGameLevel, sym::g_currentGameLevel},
            {sym::g_textCurrentRideType, sym::g_currentRideType},
            {sym::g_textCurrentRiderCount, sym::g_currentRiderCount},
            {sym::g_textCurrentSfxVolume, sym::g_currentSfxVolume},
            {sym::g_textCurrentMusicVolume, sym::g_currentMusicVolume},
            {sym::g_textCurrentTheme, sym::g_parkThemePosition},
            {sym::g_textParkEntryType, sym::g_parkEntryType},
            {sym::g_textMovieId, sym::g_movieId},
            {sym::g_textMovieStatus, sym::g_movieStatus},
        };
        // The front end's script functions, by the names the scripts call.
        struct MenuFunction
        {
            uint32_t name, function;
        };
        constexpr MenuFunction kMenuFunctions[] = {
            {sym::g_textBmxmenuSetModeLoadGame, sym::bmxmenuSetModeLoadGame},
            {sym::g_textBmxmenuSetModeSaveGame, sym::bmxmenuSetModeSaveGame},
            {sym::g_textMenuSetModeLoadPark, sym::menuSetModeLoadPark},
            {sym::g_textMenuSetModePlayPark, sym::menuSetModePlayPark},
            {sym::g_textBmxmenuGameLevelUpdate, sym::bmxmenuGameLevelUpdate},
            {sym::g_textBmxmenuGameMovieUpdate, sym::bmxmenuGameMovieUpdate},
            {sym::g_textBmxmenuBikeModelUpdate, sym::bmxmenuBikeModelUpdate},
            {sym::g_textBmxMenuMusicOff, sym::bmxMenuMusicOff},
            {sym::g_textBmxMenuMusicOn, sym::bmxMenuMusicOn},
            {sym::g_textBmxStartMovie, sym::bmxStartMovie},
            {sym::g_textBmxStopMovie, sym::bmxStopMovie},
            {sym::g_textBmxUpdateMovieStatus, sym::bmxUpdateMovieStatus},
            {sym::g_textBmxStartGame, sym::bmxStartGame},
            {sym::g_textBmxStartEditor, sym::bmxStartEditor},
            {sym::g_textBmxSetupPlayerStat, sym::bmxSetupPlayerStat},
            {sym::g_textBmxSetupBikeStat, sym::bmxSetupBikeStat},
            {sym::g_textBmxUpdateVolumeInfo, sym::bmxUpdateVolumeInfo},
            {sym::g_textBmxPlayMusicVolume, sym::bmxPlayMusicVolume},
            {sym::g_textBmxGetHiscoresData, sym::bmxGetHiscoresData},
            {sym::g_textBmxChangeCtrlConfig, sym::bmxChangeCtrlConfig},
            {sym::g_textBmxSetupLevelStats, sym::bmxSetupLevelStats},
            {sym::g_textBmxReadCtrlSetting, sym::bmxReadCtrlSetting},
            {sym::g_textBmxViewReplay, sym::bmxViewReplay},
            {sym::g_textBmxCoolStuffGetFocus, sym::bmxCoolStuffGetFocus},
            {sym::g_textBmxGetCredits, sym::bmxGetCredits},
            {sym::g_textBmxSetupUnLockPlayer, sym::bmxSetupUnLockPlayer},
            {sym::g_textBmxEndIntro, sym::bmxEndIntro},
            {sym::g_textBmxValidateName, sym::bmxValidateName},
            {sym::g_textBmxSetupTheme, sym::bmxSetupTheme},
            {sym::g_textBmxCheckCheat, sym::bmxCheckCheat},
            {sym::g_textBmxmenuPlayPark, sym::bmxmenuPlayPark},
            {sym::g_textInitializeTrickMenu, sym::initializeTrickMenu},
            {sym::g_textHarvestTrickMenu, sym::harvestTrickMenu},
        };

        // The screen the menus draw to.
        constexpr uint32_t kScreenWidth = 640, kScreenHeight = 448;
        constexpr ee::Ptr<char> kBackgroundHidden{sym::backgroundHiddenScript}; // "SET Root.CommonUI.Background.Visible {0}"
        // The main menu's zgui script, loaded once (the front end's enter
        // loads it too; its exit lets it go).
        inline ee::Global<uint32_t, sym::g_mainMenuScriptLoaded> g_mainMenuScriptLoaded;
        constexpr uint32_t kMainMenuScriptFile = sym::mainMenuScriptFile; // "main.zsc"

        // The intro's frame: how long it waits before each UI frame, the
        // scripts it runs, and whether the Acclaim movie is still to fade in
        // (once the start check has closed: the game starts with it set, the
        // intro's end sets it again).
        constexpr uint32_t kFramesBeforeUiFrame = 60;
        constexpr ee::Ptr<char> kCommonUiFadeIn{sym::commonUiFadeInScript};         // "PLAY Root.CommonUI.FadeIn"
        constexpr ee::Ptr<char> kAcclaimMovieFadeIn{sym::acclaimMovieFadeInScript}; // "PLAY Root.MainMenu.Movie_Acclaim.FadeIn"
        inline ee::Global<uint32_t, sym::g_acclaimFadeInPending> g_acclaimFadeInPending;

        void releasePlayerPad(uint32_t port)
        {
            zge::releasePad(ee::Ptr<zge::Pad>(rider::padInput(port)->pad));
        }
    }

    void closeIntroMenu()
    {
        runUiScript(kTopHeaderShown);
        runUiScript(kFadeOut);
        runUiScript(kLockUser);
        clearUiFrame();
        setMenuMode(kMenuIntroMenuClosed);
        game::tail(setMenuWait, kCloseWaitFrames);
    }
    READABLE_FUNCTION(closeIntroMenu, Draft);

    void closeIntroMenuAtOnce()
    {
        runUiScript(kTopHeaderShown);
        runUiScript(kLockUser);
        clearUiFrame();
        game::tail(setMenuMode, kMenuIntroMenuClosed);
    }
    READABLE_FUNCTION(closeIntroMenuAtOnce, Verified);

    void closeIntroEnd()
    {
        runUiScript(kFadeOut);
        runUiScript(kLockUser);
        clearUiFrame();
        setMenuMode(kMenuIntroEndClosed);
        game::tail(setMenuWait, kCloseWaitFrames);
    }
    READABLE_FUNCTION(closeIntroEnd, Verified);

    void bmxEndIntro()
    {
        game::tail(setMenuMode, kMenuIntroEnded);
    }
    READABLE_FUNCTION(bmxEndIntro, Verified);

    void bmxCoolStuffGetFocus()
    {
        game::tail(setMenuMode, kMenuFrontEnd);
    }
    READABLE_FUNCTION(bmxCoolStuffGetFocus, Verified);

    // introStateExit: the intro game state left: the menu mode back to the
    // front end's, the UI frame cleared, the common background shown.
    void introStateExit()
    {
        setMenuMode(kMenuFrontEnd);
        clearUiFrame();
        game::tail(zge::zgui::runScriptLines, *g_uiRoot, kBackgroundVisible);
    }
    READABLE_FUNCTION(introStateExit, Verified);

    uint32_t introStateEnter()
    {
        if (*g_menuFunctionsPending != 0)
        {
            const ee::Ptr<zge::SceneObject> root = widgetObject(*g_uiRoot);
            for (const MenuVariable &variable : kMenuVariables)
                zge::zgui::defineWidgetProperty(root, ee::Ptr<const char>(variable.name),
                                                ee::Ptr<const char>(kSint32TypeName), variable.value);
            for (const MenuFunction &function : kMenuFunctions)
                zge::zgui::registerScriptFunction(function.name, function.function);
            *g_menuFunctionsPending = 0;
        }
        zge::resetCameraViewports(kScreenWidth, kScreenHeight);
        enterMenuScene();
        runUiScript(kBackgroundHidden);
        if (*g_mainMenuScriptLoaded == 0)
        {
            zge::zgui::loadZguiScript(kMainMenuScriptFile, *g_uiRoot);
            *g_mainMenuScriptLoaded = 1;
        }
        *g_introFrames = 0;
        releasePlayerPad(0);
        releasePlayerPad(1);
        return 1;
    }
    READABLE_FUNCTION(introStateEnter, Verified);

    uint32_t introStateFrame()
    {
        ee::Local<UiFrameRequest> request;
        if (menuWaiting())
            return 1;
        if (*g_introMenuSetUp == 0)
            ++*g_introFrames;
        if (*g_introFrames == kFramesBeforeUiFrame)
        {
            if (menuMode() == kMenuFrontEnd)
            {
                request->onClosed = UiFrameCallback(sym::closeIntroMenu);
                request->data = 0;
                request->dataHandle = 0;
                request->onDone = UiFrameCallback(sym::closeIntroMenuAtOnce);
                setMenuMode(kMenuIntroMenu);
                setUiFrame(1, kUiFrameStartCheck, request);
                *g_introMenuSetUp = 1;
            }
            else if (menuMode() == kMenuIntroEnded)
            {
                runUiScript(kCommonUiFadeIn);
                request->onClosed = UiFrameCallback(sym::closeIntroEnd);
                request->data = 0;
                request->dataHandle = 0;
                request->onDone = UiFrameCallback(sym::closeIntroEnd);
                setMenuMode(kMenuIntroEnded);
                setUiFrame(1, kUiFrameAutoLoad, request);
                *g_introMenuSetUp = 1;
            }
        }
        readPads();
        switch (menuMode())
        {
        case kMenuIntroMenu:
        case kMenuIntroEnded:
            uiFrameUpdate();
            break;
        case kMenuIntroMenuClosed:
            if (*g_acclaimFadeInPending != 0)
            {
                *g_acclaimFadeInPending = 0;
                runUiScript(kAcclaimMovieFadeIn);
            }
            break;
        case kMenuIntroEndClosed:
            *g_acclaimFadeInPending = 1;
            setGameState(kGameStateFrontEnd);
            break;
        default:
            break;
        }
        return 1;
    }
    READABLE_FUNCTION(introStateFrame, Verified);

    namespace
    {
        // The movie menu's items (MainMenu.MovieFrame.Menu): the intro
        // movie, then each of the first 14 characters' movie, its name
        // after a "!" (the menu's mark of an item not open) until the
        // character's movie is open (kCharacterMovieOpen).
        constexpr ee::Ptr<const char> kMovieItemsScript{sym::movieMenuItemsScript}; // "SET Root.MainMenu.MovieFrame.Menu.Items {"%s\n .. %s"}" (15)
        constexpr ee::Ptr<const char> kIntroMovieItem{sym::introMovieItem};          // "Intro Movie"
        struct MovieItem
        {
            uint32_t locked, open;
        };
        constexpr MovieItem kMovieItems[] = {
            {sym::lockedMovieItemDaveMirra, sym::movieItemDaveMirra},
            {sym::lockedMovieItemRyanNyquist, sym::movieItemRyanNyquist},
            {sym::lockedMovieItemTroyMcMurray, sym::movieItemTroyMcMurray},
            {sym::lockedMovieItemMikeLaird, sym::movieItemMikeLaird},
            {sym::lockedMovieItemTimMirra, sym::movieItemTimMirra},
            {sym::lockedMovieItemKenanHarkin, sym::movieItemKenanHarkin},
            {sym::lockedMovieItemLeighRamsdell, sym::movieItemLeighRamsdell},
            {sym::lockedMovieItemJoeyGarcia, sym::movieItemJoeyGarcia},
            {sym::lockedMovieItemRickMoliterno, sym::movieItemRickMoliterno},
            {sym::lockedMovieItemToddLyons, sym::movieItemToddLyons},
            {sym::lockedMovieItemLucE, sym::movieItemLucE},
            {sym::lockedMovieItemScottWirch, sym::movieItemScottWirch},
            {sym::lockedMovieItemColinMackay, sym::movieItemColinMackay},
            {sym::lockedMovieItemZachShaw, sym::movieItemZachShaw},
        };
        constexpr uint32_t kMovieCharacters = sizeof(kMovieItems) / sizeof(kMovieItems[0]);
        constexpr uint32_t kMovieScriptBytes = 0x200;
    }

    // bmxmenuGameMovieUpdate: the script's "bmxmenuGameMovieUpdate": the movie
    // menu's items set, each character's movie marked not open until its
    // flag is.
    void bmxmenuGameMovieUpdate()
    {
        ee::Local<char[kMovieScriptBytes]> script;
        ee::Ptr<const char> items[kMovieCharacters];
        for (uint32_t character = 0; character < kMovieCharacters; ++character)
        {
            const bool open = (ee::Ptr<RiderCharacter>(sym::g_riderCharacters) + character)->flags & kCharacterMovieOpen;
            items[character] = ee::Ptr<const char>(open ? kMovieItems[character].open : kMovieItems[character].locked);
        }
        zge::formatStringWithStackWords(script, kMovieItemsScript, kIntroMovieItem, items[0], items[1], items[2], items[3], items[4], items[5],
                          items[6], items[7], items[8], items[9], items[10], items[11], items[12], items[13]);
        zge::zgui::runScriptLines(*g_uiRoot, ee::Ptr<char>(script.address()));
    }
    READABLE_FUNCTION(bmxmenuGameMovieUpdate, Verified);
}
