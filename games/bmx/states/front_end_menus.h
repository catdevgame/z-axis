#pragma once
// BMX XXX's front end menus (bmx.c's menu block): the menus' ids, each
// named by its frame in the front end's table of frames ("UI_menu.<name>",
// front_end_menu.cpp's g_frontEndMenuNames) or, for the table's empty rows,
// by what frontEndMenu does with it ("(inferred)").

#include "ee/ee.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace game::bmx
{
    enum FrontEndMenu : int32_t
    {
        kMenuStart = 0,             // (no frame) the menus fade in, then the first menu
        kMenuPressStart = 1,        // ComFrameTitle.FramePressStart
        kMenuTitleLanguage = 2,     // ComFrameTitle.FrameLanguage
        kMenuDemoLoop = 3,          // CommonBackground.DemoLoop
        kMenuMainMenu = 4,          // CommonBackground.FrameMainMenu
        kMenuSaveWaiver = 5,        // .FrameMainMenu.FrameSaveWaiver
        kMenuTeamStatus = 6,        // CommonBackground.FrameTeamStatus
        kMenuRiderSelect = 7,       // CommonBackground.FrameRiderSelect: one player's
        kMenuRiderSelect2P1 = 8,    // the same frame: the first of two players'
        kMenuRiderSelect2P2 = 9,    // the same frame: the second of two players'
        kMenuPlayer2Ctrl = 10,      // .FrameRiderSelect.Player2Ctrl
        kMenuLevelSelect = 11,      // CommonBackground.FrameLevelSelect: one player's
        kMenuLevelSelect2P = 12,    // the same frame: two players'
        kMenuLevelLoad = 13,        // CommonBackground.FrameLevelLoad
        kMenuSelectBike1 = 15,      // (no frame; inferred) "Player 1: Select Bike"
        kMenuSelectBike2 = 16,      // (no frame; inferred) "Player 2: Select Bike"
        kMenuOptions = 18,          // CommonBackground.FrameOptions
        kMenuLoadSave = 19,         // .FrameOptions.FrameLoadSave
        kMenuControls = 20,         // .FrameOptions.FrameControls
        kMenuAudio = 21,            // .FrameOptions.FrameAudio
        kMenuPlaylist = 22,         // .FrameOptions.FramePlaylist
        kMenuLanguage = 23,         // .FrameOptions.FrameLanguage
        kMenuRaunchy = 24,          // .FrameOptions.FrameRaunchy
        kMenuParkEditor = 25,       // CommonBackground.FrameParkEditor
        kMenuNewPark = 27,          // .FrameParkEditor.FrameNew
        kMenuLoadParkToEdit = 28,   // CommonBackground.DummyIdleFrame: a park loaded from the card for the editor
        kMenuLoadParkToRide = 29,   // CommonBackground.DummyIdleFrame: a park loaded from the card to ride
        kMenuExtras = 30,           // CommonBackground.FrameExtras
        kMenuScores = 31,           // .FrameExtras.FrameScores
        kMenuMovies = 32,           // .FrameExtras.FrameMovies
        kMenuPlayMovie = 33,        // .FrameExtras.FramePlayMovie (the movies')
        kMenuCredits = 34,          // .FrameExtras.FrameCredits
        kMenuCheats = 35,           // .FrameExtras.FrameCheats
        kMenuSpecialModes = 36,     // .FrameExtras.FrameSpecialModes
        kMenuPlayMovie2 = 37,       // .FrameExtras.FramePlayMovie (the extras' sixth item's)
        kMenuAkaDemos = 38,         // .FrameExtras.FrameAKAdemos
        kMenuPlayMovie3 = 39,       // .FrameExtras.FramePlayMovie (the demos')
        kMenuSelectCustom = 40,     // CommonBackground.ComFrameRiderEdit.FrameSelectCustom
        kMenuRiderEditHome = 41,    // .ComFrameRiderEdit.FrameHome
        kMenuRiderName = 42,        // .ComFrameRiderEdit.ComFrameBio.FrameName
        kMenuStyle = 43,            // .ComFrameRiderEdit.ComFrameStyle.FrameMain
        // The style's three body parts, each followed by its own part menus
        // (.ComFrameStyle.ComStyleSub.Frame_<H|U|L>_<part>).
        kMenuStyleHead = 44,        // .ComFrameStyle.FrameHead
        kMenuHeadFace = 45,
        kMenuHeadFacialHair = 46,
        kMenuHeadHairStyle = 47,
        kMenuHeadHat = 48,
        kMenuHeadAccessory = 49,
        kMenuHeadTattoos = 50,
        kMenuStyleUpperBody = 51,   // .ComFrameStyle.FrameUBod
        kMenuUpperShirts = 52,
        kMenuUpperPads = 53,
        kMenuUpperGloves = 54,
        kMenuUpperAccessory = 55,
        kMenuUpperChestTattoo = 56,
        kMenuUpperBackTattoo = 57,
        kMenuUpperLeftArmTattoo = 58,
        kMenuUpperRightArmTattoo = 59,
        kMenuStyleLowerBody = 60,   // .ComFrameStyle.FrameLBod
        kMenuLowerPads = 61,
        kMenuLowerPants = 62,
        kMenuLowerShoes = 63,
        kMenuLowerSocks = 64,
        kMenuLowerUnderwear = 65,
        kMenuLowerAccessory = 66,
        kMenuLowerLeftLegTattoo = 67,
        kMenuLowerRightLegTattoo = 68,
        kMenuSkills = 69,           // .ComFrameRiderEdit.FrameSkills
        kMenuSkillsConfirm = 70,    // .ComFrameRiderEdit.FrameSkillsConf
        kMenuGender = 71,           // .ComFrameRiderEdit.FrameGender
        kMenuGenderConfirm = 72,    // .ComFrameRiderEdit.FrameGenderConf: "yes" changes the gender, "no" goes back
        kMenuBodySize = 73,         // .ComFrameRiderEdit.FrameBodySize
        kMenuSaveGame = 74,         // (no frame; inferred) the game saved to the card (startGameSave)
        kMenuLoadGame = 75,         // (no frame; inferred) the game loaded from the card (startGameLoad)
        kMenuAutoLoad = 76,         // (no frame; inferred) the game loaded at the start (startGameAutoLoad), the intro's first menu
        kMenuLeaveFrontEnd = 77,    // (no frame; inferred) the menus fade out, then the game state the front end leaves for
    };
    constexpr uint32_t kFrontEndMenuFrames = 78; // the table's rows: ids 0 .. 77

    // The main menu's items, by their texts ("$KSTR_COM_MENU_MAIN_OPT<n>").
    enum MainMenuChoice : uint8_t
    {
        kChoiceHardcoreTour = 0, // HARDCORE TOUR: one player
        kChoiceMultiplayer = 1,  // MULTIPLAYER: two players
        kChoiceParkEditor = 2,   // PARK EDITOR (not among the main menu's items)
        kChoiceRiderEditor = 3,  // RIDER EDITOR
        kChoiceExtras = 4,       // ACCLAIM EXTRAS
        kChoiceOptions = 5,      // OPTIONS
    };
    constexpr uint32_t kMainMenuTexts = 6;

    // The front end's menus' handler (front_end_menu.cpp; Menu::handler):
    // the messages of the menu machine (menu.h) and of the UI's scripts.
    void frontEndMenu(int32_t message, uint32_t data);
    // The main menu's items laid out as it comes in (each item's choice in
    // the menu's table of choices).
    void mainMenuFrame();
    // Where the replay the menus loaded to be viewed is (level_state.cpp's
    // enter copies it into the viewed replay).
    ee::Address loadedReplay();
    // The menus' camera (front_end.cpp makes it): its frame cleared or not,
    // and its clear colour (R, G, B, A at `colour`).
    void setMenuCameraNodeClears(ee::Ptr<zge::SceneObject> camera, uint32_t on);
    void setMenuCameraNodeClearColour(ee::Ptr<zge::SceneObject> camera, uint32_t colour);
}
