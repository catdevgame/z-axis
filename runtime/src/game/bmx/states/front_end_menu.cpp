// BMX XXX's front end menus' handler (front_end_menus.h): what each menu does
// as it comes in and goes out, and where each of its items leads. BMX XXX's
// own code (bmx.c's menu block).

#include "front_end_menus.h"

#include "bmx/create/menu_model.h"
#include "bmx/profile/players.h"
#include "bmx/save/game_save.h"
#include "bmx/save/park_buffer.h"
#include "bmx/save/park_save.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/states/game_state.h"
#include "bmx/ui/menu.h"
#include "bmx/ui/ui_calls.h"
#include "bmx/ui/ui_nodes.h"
#include "bmx/ui/zgui_globals.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "bmx/camera/scene_camera.h"
#include "ee/readable.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/bmx/states/front_end_menu.cpp.h"

namespace game::bmx
{
    // The front end's park load's end (startParkLoad's): the park the card
    // loaded ridden or edited, else the park editor's menu again.
    void frontEndParkLoaded(int32_t result);

    namespace
    {
        using Node = ee::Ptr<zge::zgui::Node>;

        // The menus' frames by id ("UI_menu.<name>"; front_end_menus.h).
        struct FrontEndMenuNames
        {
            ee::Ptr<const char> name[kFrontEndMenuFrames];
        };
        inline ee::Global<FrontEndMenuNames, sym::g_frontEndMenuNames> g_frontEndMenuNames;
        constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame

        // The texts the handler finds and plays.
        constexpr uint32_t kFramePath = sym::g_textMenuFramePath;              // "UI_menu.%s"
        constexpr ee::Ptr<const char> kMenuUi{sym::g_textFrontEndUi};                   // "UI_menu"
        constexpr ee::Ptr<const char> kStatus{sym::g_textMenuStatus};                   // "Status"
        constexpr ee::Ptr<const char> kText{sym::g_textMenuText};                       // "Text"
        constexpr uint32_t kEmpty = sym::g_textMenuEmpty;                     // ""
        constexpr ee::Ptr<const char> kInit{sym::g_textFrontEndInit};                   // "Init"
        constexpr ee::Ptr<const char> kShutdown{sym::g_textFrontEndShutdown};           // "Shutdown"
        constexpr ee::Ptr<const char> kFadeIn{sym::g_textMenuFadeIn};                   // "FadeIn"
        constexpr ee::Ptr<const char> kFadeOut{sym::g_textMenuFadeOut};                 // "FadeOut"
        constexpr uint32_t kSelectBike = sym::g_textSelectBikeFormat;         // "Player %d: Select Bike"
        // The lists an item is read from, and the value read.
        constexpr ee::Ptr<const char> kMenuList{sym::g_textMenuMenu};                   // "Menu"
        constexpr ee::Ptr<const char> kOptionList{sym::g_textOptionMenu};               // "OPTION.Menu"
        constexpr ee::Ptr<const char> kBackgroundList{sym::g_textBackgroundMenu};       // "Background.Menu"
        constexpr ee::Ptr<const char> kLoadSaveList{sym::g_textBackBackgroundMenu};     // "Back.Background.Menu"
        constexpr ee::Ptr<const char> kExtrasList{sym::g_textBoxMenu};                  // "Box.Menu"
        constexpr ee::Ptr<const char> kSpecialModesList{sym::g_textMenuName};           // "MenuName"
        constexpr ee::Ptr<const char> kButtonAnimation{sym::g_textButtonAnimation};     // "Background.ButtonAnimation"
        constexpr ee::Ptr<const char> kSelectedIndex{sym::g_textFrontEndSelectedIndex};     // "SelectedIndex"
        constexpr ee::Ptr<const char> kListSelectedIndex{sym::g_textMenuSelectedIndexPath}; // "Menu.SelectedIndex"
        constexpr ee::Ptr<const char> kCurValue{sym::g_textFrontEndCurValue};               // "CurValue"

        // The UI's message as the rider select's option button is pressed
        // on a rider that is not disabled (its KeySniffer's OnOption in
        // UILIB.ZAL: the rider edit's home comes up).
        constexpr int32_t kMsgOption = 6;
        // The UI's message as the screen is over, and as it goes back.
        constexpr int32_t kMsgSelect = 0, kMsgBack = -1;

        // The game state the front end leaves for (kMenuLeaveFrontEnd), and
        // the menus that come back to a menu kept for them (the card's, and
        // the park loads').
        inline ee::Global<uint8_t, sym::g_frontEndNextState> g_frontEndNextState;
        inline ee::Global<uint8_t, sym::g_frontEndReturnMenu> g_frontEndReturnMenu;
        inline ee::Global<uint8_t, sym::g_frontEndFirstMenu> g_frontEndFirstMenu;
        // The rider select the rider edit's home goes back to (or the custom
        // riders' select).
        inline ee::Global<int32_t, sym::g_riderEditBackMenu> g_riderEditBackMenu;
        // The main menu's items, as mainMenuFrame lays them out: what each
        // leads to (MainMenuChoice), and the items' texts by choice.
        constexpr ee::Ptr<uint8_t> kMainMenuChoices{sym::g_mainMenuChoices};
        struct MainMenuTexts
        {
            ee::Ptr<const char> text[kMainMenuTexts];
        };
        inline ee::Global<MainMenuTexts, sym::g_mainMenuTexts> g_mainMenuTexts;
        constexpr ee::Ptr<const char> kMainMenuFrame{sym::g_textMainMenuFrame};   // "UI_menu.CommonBackground.FrameMainMenu"
        constexpr uint32_t kItemFormat = sym::g_textMainMenuItem;       // "%s%s"
        constexpr uint32_t kMarkedItemFormat = sym::g_textMainMenuMarkedItem; // "%s%s%s"
        constexpr uint32_t kItemBreak = sym::g_textMainMenuBreak;       // "\\n" (the script's line break)
        constexpr uint32_t kDisabledMark = sym::g_textMainMenuDisabled; // "!"
        constexpr ee::Ptr<const char> kItems{sym::g_textMainMenuItems};           // "Items"
        constexpr ee::Ptr<const char> kVisibleCount{sym::g_textMainMenuVisibleCount}; // "VisibleCount"
        // Multiplayer wants two pads.
        inline ee::Global<uint32_t, sym::g_connectedPads> g_connectedPads;
        constexpr uint32_t kMultiplayerPads = 2;
        // The level select's levels, by item (its list's script functions
        // fill it).
        constexpr uint32_t kLevelSelectItems = 12;
        inline ee::Global<uint8_t[kLevelSelectItems], sym::g_levelSelectLevels> g_levelSelectLevels;
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        inline ee::Global<int32_t, sym::g_modalPlayerNum> g_modalPlayerNum;
        inline ee::Global<int32_t, sym::g_msgType> g_msgType;
        // What the game was started for: kinds 1 and 2 skip the level select
        // and the bikes' selects.
        inline ee::Global<uint8_t, sym::g_startUpKind> g_startUpKind;
        constexpr uint8_t kStartUpKindsEnd = 3;
        // The frames the front end has been idle (front_end.cpp's).
        inline ee::Global<uint32_t, sym::g_frontEndIdleFrames> g_frontEndIdleFrames;
        // The session the riders play: a one-player tour, or two players.
        inline ee::Global<uint8_t, sym::g_sessionKind> g_sessionKind;
        constexpr uint8_t kTourSession = 0, kMultiplayerSession = 3;
        // A game load the intro asked for, still to start.
        inline ee::Global<uint32_t, sym::g_autoLoadPending> g_autoLoadPending;

        // The park editor's park to start with (park_editor.cpp's): one
        // from the card, in g_parkToLoad, to edit or to ride.
        inline ee::Global<uint8_t, sym::g_editorParkSource> g_editorParkSource;
        constexpr uint8_t kParkSourceNone = 0, kParkSourceCard = 1, kParkSourceCardToRide = 3;
        inline ee::Global<ee::Ptr<ParkBuffer>, sym::g_parkToLoad> g_parkToLoad;
        inline ee::Global<uint8_t, sym::g_parkTheme> g_parkTheme;
        constexpr uint32_t kParkBufferBytes = 0x2000;
        constexpr uint32_t kParkClient = 0;
        constexpr int32_t kParkLoaded = 2;   // startParkLoad's result: the park is in the buffer
        constexpr uint32_t kParkThemeByte = 4; // the park's theme, in its bytes

        // The custom riders' records follow the thirteen riders'.
        constexpr uint8_t kFirstCustomCharacter = 13;

        // A park's load from the card started, into a new buffer.
        void startCardParkLoad()
        {
            const ee::Ptr<ParkBuffer> buffer = newParkBuffer(kParkBufferBytes);
            *g_parkToLoad = buffer;
            setParkBufferClient(buffer, kParkClient);
            startParkLoad(*g_parkToLoad, sym::frontEndParkLoaded, 0);
        }
    }

    // frontEndMenu: the front end's menus' handler.
    //   kMsgEnter    the player and the status as the menu needs them, what
    //                the menu starts (the card's saves and loads, a park
    //                load, the menus' fades), then the frame's "Init";
    //   kMsgCameIn   the first menu after the start, the state left for
    //                after the leaving, the rider object shown for the rider
    //                selects and the rider edit;
    //   kMsgLeave    the rider object hidden, the frame's "Shutdown" (the
    //                start, the park and card menus answer at once);
    //   kMsgLeft     the status cleared;
    //   kMsgSelect   the menu the item chosen leads to;
    //   kMsgBack     the menu before (the main menu for most);
    //   kMsgOption   from a rider select, the rider edit's home.
    void frontEndMenu(int32_t message, uint32_t data)
    {
        ee::Local<char[0x400]> text;
        ee::Local<Menu> menu;
        ee::Local<FrontEndMenuNames> names;
        ee::Local<uint32_t> selected;
        ee::Local<uint32_t> listChoice;
        ee::Local<ee::Float> curValue;

        *names = *g_frontEndMenuNames;
        zge::formatString(text, kFramePath, names->name[menuId()]);
        const Node frame = zge::zgui::findNode(g_bmxZgui->root, text, kFrameNode);
        const Node status = zge::zgui::findNode(frame, kStatus, kFrameNode);
        menu->data = 0;
        menu->handler = sym::frontEndMenu;
        // The item chosen in the frame's list (its "SelectedIndex").
        const auto selectedIn = [&](ee::Ptr<const char> list) {
            return zge::zgui::copyVariable(zge::zgui::findNode(frame, list, kFrameNode), kSelectedIndex, selected);
        };
        const auto goTo = [&](int32_t id) {
            menu->id = id;
            setMenu(menu);
        };

        switch (message)
        {
        case kMsgEnter:
            switch (menuId())
            {
            case kMenuMainMenu:
                *g_playerCount = 1;
                *g_modalPlayerNum = -1;
                mainMenuFrame();
                *g_currentPlayer = 0;
                break;
            case kMenuRiderSelect:
            case kMenuRiderSelect2P1:
                *g_currentPlayer = 0;
                *g_modalPlayerNum = 0;
                break;
            case kMenuRiderSelect2P2:
            case kMenuPlayer2Ctrl:
                *g_currentPlayer = 1;
                *g_modalPlayerNum = 1;
                break;
            case kMenuLevelSelect:
            case kMenuLevelSelect2P:
            {
                const uint8_t kind = *g_startUpKind;
                *g_modalPlayerNum = -1;
                if (kind < kStartUpKindsEnd && kind != 0)
                {
                    *g_frontEndNextState = kGameStateLevel;
                    menu->id = kMenuLeaveFrontEnd;
                    *g_currentLevel = 0;
                    setMenu(menu);
                    return;
                }
                break;
            }
            case kMenuSelectBike1:
            case kMenuSelectBike2:
            {
                const uint8_t kind = *g_startUpKind;
                if (kind < kStartUpKindsEnd && kind != 0)
                {
                    goTo(menuId() + 1);
                    return;
                }
                const int32_t player = menuId() - kMenuSelectBike1;
                *g_currentPlayer = static_cast<uint8_t>(player);
                *g_modalPlayerNum = player & 0xFF;
                zge::formatString(text, kSelectBike, *g_currentPlayer + 1);
                zge::zgui::setVariable(status, kText, text);
                break;
            }
            case kMenuRiderName:
                showCharacterNames(frame);
                break;
            case kMenuGender:
                showGenderChoice(frame);
                break;
            case kMenuSaveGame:
                startGameSave();
                break;
            case kMenuLoadGame:
                startGameLoad();
                break;
            case kMenuLoadParkToEdit:
                startCardParkLoad();
                *g_editorParkSource = kParkSourceCard;
                break;
            case kMenuLoadParkToRide:
                startCardParkLoad();
                menu->id = kMenuLoadParkToRide; // (set, and not asked for)
                *g_editorParkSource = kParkSourceCardToRide;
                break;
            case kMenuAutoLoad:
                if (*g_autoLoadPending != 0)
                    startGameAutoLoad();
                *g_autoLoadPending = 0;
                break;
            case kMenuStart:
                showMenuModel(0, 0);
                playUiNode(zge::zgui::findNode(g_bmxZgui->root, kMenuUi, kFrameNode), kFadeIn);
                break;
            case kMenuLeaveFrontEnd:
                playUiNode(zge::zgui::findNode(g_bmxZgui->root, kMenuUi, kFrameNode), kFadeOut);
                setGameMode(kSoundModeNone);
                break;
            default:
                break;
            }
            if (frame)
                playUiNode(frame, kInit);
            return;

        case kMsgCameIn:
            switch (menuId())
            {
            case kMenuStart:
                goTo(*g_frontEndFirstMenu);
                *g_frontEndFirstMenu = kMenuMainMenu;
                return;
            case kMenuLeaveFrontEnd:
                setGameState(*g_frontEndNextState);
                return;
            case kMenuPlayer2Ctrl:
                showMenuModel(0, 0);
                return;
            case kMenuStyleHead:
            case kMenuHeadFace:
            case kMenuHeadFacialHair:
            case kMenuHeadHairStyle:
            case kMenuHeadHat:
            case kMenuHeadAccessory:
            case kMenuHeadTattoos:
            case kMenuStyleUpperBody:
            case kMenuUpperShirts:
            case kMenuUpperPads:
            case kMenuUpperGloves:
            case kMenuUpperAccessory:
            case kMenuUpperChestTattoo:
            case kMenuUpperBackTattoo:
            case kMenuUpperLeftArmTattoo:
            case kMenuUpperRightArmTattoo:
                showMenuModel(1, 1);
                return;
            case kMenuRiderSelect:
            case kMenuRiderSelect2P1:
            case kMenuRiderSelect2P2:
            case kMenuRiderEditHome:
            case kMenuRiderName:
            case kMenuStyle:
            case kMenuStyleLowerBody:
            case kMenuLowerPads:
            case kMenuLowerPants:
            case kMenuLowerShoes:
            case kMenuLowerSocks:
            case kMenuLowerUnderwear:
            case kMenuLowerAccessory:
            case kMenuLowerLeftLegTattoo:
            case kMenuLowerRightLegTattoo:
            case kMenuSkills:
            case kMenuSkillsConfirm:
            case kMenuGender:
            case kMenuGenderConfirm:
            case kMenuBodySize:
                showMenuModel(1, 0);
                return;
            default:
                return;
            }

        case kMsgLeave:
        {
            const int32_t id = menuId();
            const bool answersAtOnce = id == kMenuStart || id == kMenuLoadParkToEdit || id == kMenuLoadParkToRide ||
                                       id == kMenuSaveGame || id == kMenuLoadGame || id == kMenuAutoLoad;
            if (answersAtOnce)
            {
                *g_msgType = kMsgLeft;
                bmxuiHandler();
            }
            showMenuModel(0, 0);
            if (frame)
                playUiNode(frame, kShutdown);
            return;
        }

        case kMsgLeft:
            zge::formatString(text, kEmpty);
            zge::zgui::setVariable(status, kText, text);
            return;

        case kMsgSelect:
            switch (menuId())
            {
            case kMenuMainMenu:
                selectedIn(kMenuList);
                switch (kMainMenuChoices[*selected])
                {
                case kChoiceMultiplayer:
                    *g_playerCount = 2;
                    *g_sessionKind = kMultiplayerSession;
                    menu->id = kMenuRiderSelect2P1;
                    break;
                case kChoiceParkEditor:
                    menu->id = kMenuParkEditor;
                    break;
                case kChoiceRiderEditor:
                    menu->id = kMenuSelectCustom;
                    break;
                case kChoiceExtras:
                    menu->id = kMenuExtras;
                    break;
                case kChoiceOptions:
                    menu->id = kMenuOptions;
                    break;
                case kChoiceHardcoreTour:
                default:
                    *g_playerCount = 1;
                    menu->id = kMenuRiderSelect;
                    *g_sessionKind = kTourSession;
                    break;
                }
                setMenu(menu);
                return;
            case kMenuRiderSelect:
                goTo(kMenuLevelSelect);
                return;
            case kMenuRiderSelect2P1:
            case kMenuPlayer2Ctrl:
                goTo(kMenuRiderSelect2P2);
                return;
            case kMenuRiderSelect2P2:
                goTo(kMenuLevelSelect2P);
                return;
            case kMenuLevelSelect:
            case kMenuLevelSelect2P:
            {
                const int32_t copied = selectedIn(kOptionList);
                if (*selected >= kLevelSelectItems || copied == 0)
                    return;
                *g_frontEndNextState = kGameStateLevel;
                *g_currentLevel = (*g_levelSelectLevels)[*selected];
                *g_sessionKind = menuId() == kMenuLevelSelect2P ? kMultiplayerSession : kTourSession;
                goTo(kMenuLeaveFrontEnd);
                return;
            }
            case kMenuSelectCustom:
                zge::zgui::copyVariable(frame, kListSelectedIndex, listChoice);
                (*g_playerSetups)[0].character = static_cast<uint8_t>(static_cast<uint8_t>(*listChoice) + kFirstCustomCharacter);
                setMenuModel(playerCharacter(0)->model);
                *g_riderEditBackMenu = kMenuSelectCustom;
                goTo(kMenuRiderEditHome);
                return;
            case kMenuRiderEditHome:
            {
                // The home's buttons are an animation's value.
                zge::zgui::copyVariable(zge::zgui::findNode(frame, kButtonAnimation, kFrameNode), kCurValue, curValue);
                const uint32_t button = static_cast<uint32_t>(ee::toInt(*curValue));
                *selected = button;
                switch (button)
                {
                case 0: menu->id = kMenuRiderName; break;
                case 1: menu->id = kMenuStyle; break;
                case 2: menu->id = kMenuBodySize; break;
                case 3: menu->id = kMenuGender; break;
                case 4: menu->id = *g_riderEditBackMenu; break;
                default: break; // (the menu's id as it was)
                }
                setMenu(menu);
                return;
            }
            case kMenuStyle:
                selectedIn(kMenuList);
                if (static_cast<int32_t>(*selected) == 1)
                    menu->id = kMenuStyleUpperBody;
                else if (static_cast<int32_t>(*selected) == 0)
                    menu->id = kMenuStyleHead;
                else if (static_cast<int32_t>(*selected) == 2)
                    menu->id = kMenuStyleLowerBody;
                else
                    menu->id = kMenuRiderEditHome;
                setMenu(menu);
                return;
            case kMenuStyleHead:
            case kMenuStyleUpperBody:
            case kMenuStyleLowerBody:
                // The body part's own part menus follow it.
                selectedIn(kMenuList);
                goTo(menuId() + static_cast<int32_t>(*selected + 1));
                return;
            case kMenuHeadFace:
            case kMenuHeadFacialHair:
            case kMenuHeadHairStyle:
            case kMenuHeadHat:
            case kMenuHeadAccessory:
            case kMenuHeadTattoos:
                goTo(kMenuStyleHead);
                return;
            case kMenuUpperShirts:
            case kMenuUpperPads:
            case kMenuUpperGloves:
            case kMenuUpperAccessory:
            case kMenuUpperChestTattoo:
            case kMenuUpperBackTattoo:
            case kMenuUpperLeftArmTattoo:
            case kMenuUpperRightArmTattoo:
                goTo(kMenuStyleUpperBody);
                return;
            case kMenuLowerPads:
            case kMenuLowerPants:
            case kMenuLowerShoes:
            case kMenuLowerSocks:
            case kMenuLowerUnderwear:
            case kMenuLowerAccessory:
            case kMenuLowerLeftLegTattoo:
            case kMenuLowerRightLegTattoo:
                goTo(kMenuStyleLowerBody);
                return;
            case kMenuSkills:
            case kMenuBodySize:
                goTo(kMenuRiderEditHome);
                return;
            case kMenuRiderName:
                keepCharacterNames(frame);
                goTo(kMenuRiderEditHome);
                return;
            case kMenuGender:
                goTo(kMenuGenderConfirm);
                return;
            case kMenuGenderConfirm:
                selectedIn(kMenuList);
                if (*selected == 0)
                {
                    changeCharacterGender();
                    menu->id = kMenuRiderEditHome;
                }
                else if (*selected == 1)
                {
                    menu->id = kMenuGender;
                }
                setMenu(menu);
                return;
            case kMenuParkEditor:
                selectedIn(kOptionList);
                if (static_cast<int32_t>(*selected) == 1)
                {
                    *g_frontEndReturnMenu = static_cast<uint8_t>(menuId());
                    menu->id = kMenuLoadParkToEdit;
                }
                else if (static_cast<int32_t>(*selected) == 0)
                {
                    menu->id = kMenuNewPark;
                }
                else if (static_cast<int32_t>(*selected) == 2)
                {
                    *g_frontEndReturnMenu = static_cast<uint8_t>(menuId());
                    menu->id = kMenuLoadParkToRide;
                }
                setMenu(menu);
                return;
            case kMenuNewPark:
                *g_editorParkSource = kParkSourceNone;
                zge::zgui::copyVariable(zge::zgui::findNode(frame, kBackgroundList, kFrameNode), kSelectedIndex, selected);
                menu->id = kMenuLeaveFrontEnd;
                *g_parkTheme = static_cast<uint8_t>(*selected);
                *g_frontEndNextState = kGameStateParkEditor;
                setMenu(menu);
                return;
            case kMenuLoadParkToEdit:
            case kMenuLoadParkToRide:
            case kMenuSaveGame:
            case kMenuLoadGame:
            case kMenuAutoLoad:
                goTo(*g_frontEndReturnMenu);
                return;
            case kMenuOptions:
                selectedIn(kOptionList);
                switch (*selected)
                {
                case 0: menu->id = kMenuLoadSave; break;
                case 1: menu->id = kMenuControls; break;
                case 2: menu->id = kMenuAudio; break;
                case 3: menu->id = kMenuPlaylist; break;
                case 4: menu->id = kMenuRaunchy; break;
                case 5: menu->id = kMenuLanguage; break;
                default: break; // (the menu's id as it was)
                }
                setMenu(menu);
                return;
            case kMenuLoadSave:
                selectedIn(kLoadSaveList);
                if (*selected == 0)
                {
                    *g_frontEndReturnMenu = static_cast<uint8_t>(menuId());
                    menu->id = kMenuLoadGame;
                }
                else if (*selected == 1)
                {
                    *g_frontEndReturnMenu = static_cast<uint8_t>(menuId());
                    menu->id = kMenuSaveGame;
                }
                setMenu(menu);
                return;
            case kMenuControls:
            case kMenuAudio:
            case kMenuPlaylist:
            case kMenuLanguage:
            case kMenuRaunchy:
                goTo(kMenuOptions);
                return;
            case kMenuExtras:
                selectedIn(kExtrasList);
                switch (*selected)
                {
                case 0: menu->id = kMenuScores; break;
                case 1: menu->id = kMenuMovies; break;
                case 2: menu->id = kMenuCredits; break;
                case 3: menu->id = kMenuCheats; break;
                case 4: menu->id = kMenuSpecialModes; break;
                case 5: menu->id = kMenuPlayMovie2; break;
                case 6: menu->id = kMenuAkaDemos; break;
                default: break; // (the menu's id as it was)
                }
                setMenu(menu);
                return;
            case kMenuMovies:
                goTo(kMenuPlayMovie);
                return;
            case kMenuAkaDemos:
                goTo(kMenuPlayMovie3);
                return;
            case kMenuScores:
            case kMenuCredits:
            case kMenuCheats:
            case kMenuPlayMovie2:
                goTo(kMenuExtras);
                return;
            case kMenuSpecialModes:
            {
                // An opened feature's mode turned on or off (the shifts take
                // the item's low five bits, as the EE's).
                selectedIn(kSpecialModesList);
                const uint32_t feature = *selected & 31;
                if (((static_cast<int32_t>(g_savedGame->features) >> feature) & 1) != 0)
                    g_savedGame->featuresOn = static_cast<uint16_t>(g_savedGame->featuresOn ^ (1u << feature));
                return;
            }
            case kMenuDemoLoop:
                goTo(kMenuPressStart);
                *g_frontEndIdleFrames = 0;
                return;
            case kMenuPressStart:
                goTo(kMenuMainMenu);
                return;
            default:
                return;
            }

        case kMsgBack:
            switch (menuId())
            {
            case kMenuMainMenu:
                return;
            case kMenuDemoLoop:
                goTo(kMenuPressStart);
                *g_frontEndIdleFrames = 0;
                return;
            case kMenuLevelSelect:
                goTo(kMenuRiderSelect);
                return;
            case kMenuRiderSelect2P2:
            case kMenuPlayer2Ctrl:
                goTo(kMenuRiderSelect2P1);
                return;
            case kMenuLevelSelect2P:
                goTo(kMenuRiderSelect2P2);
                return;
            case kMenuRiderEditHome:
                goTo(*g_riderEditBackMenu);
                return;
            case kMenuRiderName:
            case kMenuStyle:
            case kMenuSkills:
            case kMenuGender:
            case kMenuGenderConfirm:
            case kMenuBodySize:
                goTo(kMenuRiderEditHome);
                return;
            case kMenuStyleHead:
            case kMenuStyleUpperBody:
            case kMenuStyleLowerBody:
                goTo(kMenuStyle);
                return;
            case kMenuHeadFace:
            case kMenuHeadFacialHair:
            case kMenuHeadHairStyle:
            case kMenuHeadHat:
            case kMenuHeadAccessory:
            case kMenuHeadTattoos:
                goTo(kMenuStyleHead);
                return;
            case kMenuUpperShirts:
            case kMenuUpperPads:
            case kMenuUpperGloves:
            case kMenuUpperAccessory:
            case kMenuUpperChestTattoo:
            case kMenuUpperBackTattoo:
            case kMenuUpperLeftArmTattoo:
            case kMenuUpperRightArmTattoo:
                goTo(kMenuStyleUpperBody);
                return;
            case kMenuLowerPads:
            case kMenuLowerPants:
            case kMenuLowerShoes:
            case kMenuLowerSocks:
            case kMenuLowerUnderwear:
            case kMenuLowerAccessory:
            case kMenuLowerLeftLegTattoo:
            case kMenuLowerRightLegTattoo:
                goTo(kMenuStyleLowerBody);
                return;
            case kMenuNewPark:
            case kMenuLoadParkToEdit:
            case kMenuLoadParkToRide:
                goTo(kMenuParkEditor);
                return;
            case kMenuLoadSave:
            case kMenuControls:
            case kMenuAudio:
            case kMenuPlaylist:
            case kMenuLanguage:
            case kMenuRaunchy:
                goTo(kMenuOptions);
                return;
            case kMenuScores:
            case kMenuMovies:
            case kMenuCredits:
            case kMenuCheats:
            case kMenuSpecialModes:
            case kMenuPlayMovie2:
            case kMenuAkaDemos:
                goTo(kMenuExtras);
                return;
            case kMenuPlayMovie3:
                goTo(kMenuAkaDemos);
                return;
            case kMenuPlayMovie:
                goTo(kMenuMovies);
                return;
            default:
                goTo(kMenuMainMenu);
                return;
            }

        case kMsgOption:
        {
            const int32_t id = menuId();
            if (id < kMenuRiderSelect || id > kMenuRiderSelect2P2)
                return;
            *g_riderEditBackMenu = menuId();
            goTo(kMenuRiderEditHome);
            if (g_bmxZgui->selectSound)
                g_bmxZgui->selectSound();
            return;
        }

        default:
            return;
        }
    }
    READABLE_FUNCTION_WAITS(frontEndMenu, Verified);

    // mainMenuFrame: the main menu's items, a line each (the multiplayer's
    // marked disabled with fewer than two pads), and how many; the park
    // editor's is not among them.
    void mainMenuFrame()
    {
        ee::Local<char[0x100]> items;
        ee::Local<MainMenuTexts> texts;
        ee::Local<uint32_t> count;

        *texts = *g_mainMenuTexts;
        const Node frame = zge::zgui::findNode(g_bmxZgui->root, kMainMenuFrame, kFrameNode);
        const Node list = zge::zgui::findNode(frame, kMenuList, kFrameNode);
        items[0] = '\0';
        *count = 0;
        // The next item's line on the end of the items, after a line break
        // unless it is the first.
        const auto itemText = [&]() { return texts->text[kMainMenuChoices[*count]]; };
        const auto lineBreak = [&]() { return *count != 0 ? kItemBreak : kEmpty; };
        const auto addItem = [&](uint8_t next) {
            *count = *count + 1;
            kMainMenuChoices[*count] = next;
        };

        kMainMenuChoices[0] = kChoiceHardcoreTour;
        uint32_t end = static_cast<uint32_t>(zge::stringLength(items));
        zge::formatString(items.address() + end, kItemFormat, lineBreak(), itemText());
        addItem(kChoiceMultiplayer);
        end = static_cast<uint32_t>(zge::stringLength(items));
        zge::formatString(items.address() + end, kMarkedItemFormat, lineBreak(), *g_connectedPads < kMultiplayerPads ? kDisabledMark : kEmpty,
                          itemText());
        addItem(kChoiceRiderEditor);
        end = static_cast<uint32_t>(zge::stringLength(items));
        zge::formatString(items.address() + end, kItemFormat, lineBreak(), itemText());
        addItem(kChoiceExtras);
        end = static_cast<uint32_t>(zge::stringLength(items));
        zge::formatString(items.address() + end, kItemFormat, lineBreak(), itemText());
        addItem(kChoiceOptions);
        end = static_cast<uint32_t>(zge::stringLength(items));
        zge::formatString(items.address() + end, kItemFormat, lineBreak(), itemText());
        *count = *count + 1;
        zge::zgui::setVariable(list, kItems, items);
        zge::zgui::setVariable(list, kVisibleCount, count);
    }
    READABLE_FUNCTION(mainMenuFrame, Verified);

    namespace
    {
        // The replay the menus loaded to be viewed (DM2's loadedReplay
        // hands its buffer on; nothing in BMX XXX sets this word).
        inline ee::Global<uint32_t, sym::g_loadedReplay> g_loadedReplay;
    }

    // loadedReplay (DM2's name): where the replay to view is.
    ee::Address loadedReplay()
    {
        return *g_loadedReplay;
    }
    READABLE_FUNCTION(loadedReplay, Draft);

    // frontEndParkLoaded: a park the card loaded leaves the front end with
    // the park's theme, to ride (loaded so) or for the editor; else the park
    // editor's menu comes back.
    void frontEndParkLoaded(int32_t result)
    {
        ee::Local<Menu> menu;
        menu->id = kMenuStart;
        menu->handler = sym::frontEndMenu;
        menu->data = 0;
        if (result == kParkLoaded)
        {
            const uint8_t theme = ee::Ptr<uint8_t>(parkBufferData(*g_parkToLoad))[kParkThemeByte];
            menu->id = kMenuLeaveFrontEnd;
            *g_parkTheme = theme;
            *g_frontEndNextState = *g_editorParkSource == kParkSourceCardToRide ? kGameStateRidePark : kGameStateParkEditor;
            setMenu(menu);
        }
        else
        {
            menu->id = kMenuParkEditor;
            setMenu(menu);
        }
    }
    READABLE_FUNCTION(frontEndParkLoaded, Verified);

    // The menus' camera: whether its frame is cleared, and the colour (R,
    // G, B, A at `colour`). The menu block's own copies of the intro's
    // setCameraNodeClears and setCameraNodeClearColour.
    void setMenuCameraNodeClears(ee::Ptr<zge::SceneObject> camera, uint32_t on)
    {
        const ee::Ptr<zge::Camera> record = objectCamera(camera);
        game::tail(zge::setCameraClears, record, on);
    }
    READABLE_FUNCTION(setMenuCameraNodeClears, Verified);

    void setMenuCameraNodeClearColour(ee::Ptr<zge::SceneObject> camera, uint32_t colour)
    {
        const ee::Ptr<zge::Camera> record = objectCamera(camera);
        game::tail(zge::setCameraClearColour, record, colour);
    }
    READABLE_FUNCTION(setMenuCameraNodeClearColour, Verified);
}
