// BMX XXX's AV menu state (av_menu.h). BMX XXX's own code (bmx.c).

#include "av_menu.h"

#include "game_state.h"
#include "intro.h"

#include "bmx/game/player_riders.h"
#include "bmx/input/pads.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/ui/menu.h"
#include "bmx/ui/menu_movie.h"
#include "bmx/ui/ui_nodes.h"
#include "bmx/ui/zgui_globals.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/math/types.h"
#include "zge/render/display.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/states/av_menu.cpp.h"

namespace game::bmx
{
    using zge::Vec4;
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameNode = 1; // findNode's kind: a frame
        // The menus' OnItemSelect and their cancel (MsgType 0 and -1).
        constexpr int32_t kMsgChosen = 0, kMsgCancelled = -1;
        constexpr uint32_t kMenuScreenWidth = 640, kScreenWidth = 512, kScreenHeight = 448;

        // The menu's pages (the menu machine's ids) and their frames
        // ("UI_AVMenu.<name>"; 8, 10 and 11 have none).
        enum AvMenuPage : int32_t
        {
            kPageMode = 0,          // FrameModeSelect
            kPageLevelSet = 1,      // FrameLevelSetSelect: where the levels' files come from
            kPageLevel = 2,         // FrameLevelSelect
            kPagePlayers = 3,       // FrameNumPlayersSelect
            kPageLevelSet2 = 4,     // FrameLevelSetSelect again (only cancelling back from 5 reaches it)
            kPageLevel2 = 5,        // FrameLevelSelect again (only cancelling back from 6 reaches it)
            kPagePark = 6,          // FrameParkSelect
            kPageScreen = 7,        // FrameScreenSelect
            kPageShowScreen = 8,    // the screen image picked, shown
            kPageMovie = 9,         // FrameMovieSelect
            kPagePlayMovie = 10,    // the movie picked, played
            kPageLeave = 11,        // the menu fades out, then the state picked
        };
        constexpr uint32_t kPages = 12;
        struct AvMenuFrames
        {
            ee::Ptr<const char> name[kPages];
        };
        inline ee::Global<AvMenuFrames, sym::g_avMenuFrames> g_avMenuFrames;

        // The mode page's lines (AVMenu.zsc's items).
        enum AvMenuMode : int32_t
        {
            kModeLevelView = 0,
            kModeMotionView = 1,
            kModeGameTest = 2,
            kModeParkEditorTest = 3,
            kModeMenuTest = 4,
            kModeMovieTest = 5,
        };
        // The state the leave page starts.
        inline ee::Global<uint8_t, sym::g_avMenuNextState> g_avMenuNextState;

        constexpr uint32_t kFrameFormat = sym::g_textAvMenuFrame;               // "UI_AVMenu.%s"
        constexpr uint32_t kAvMenuUi = sym::g_textAvMenu;                       // "UI_AVMenu"
        constexpr uint32_t kStatusName = sym::g_textAvMenuStatus;               // "UI_AVMenu.Status"
        constexpr uint32_t kMenuName = sym::g_textAvMenuMenu;                   // "Menu"
        constexpr uint32_t kSelectedIndexName = sym::g_textAvMenuSelectedIndex; // "SelectedIndex"
        constexpr uint32_t kItemsName = sym::g_textAvMenuItems;                 // "Items"
        constexpr uint32_t kTextName = sym::g_textIntroText;                    // "Text"
        constexpr uint32_t kVisibleName = sym::g_textStateMovieVisible;         // "Visible"
        constexpr uint32_t kInitAction = sym::g_textIntroInit;                  // "Init"
        constexpr uint32_t kDeInitAction = sym::g_textAvMenuDeInit;             // "DeInit"
        constexpr uint32_t kFadeInAction = sym::g_textIntroFadeIn;              // "FadeIn"
        constexpr uint32_t kFadeOutAction = sym::g_textIntroFadeOut;            // "FadeOut"
        constexpr uint32_t kMovieNode = sym::g_textUiCommonMovie;               // "UI_Common.Movie"
        // The status line, by page.
        constexpr uint32_t kSelectMode = sym::g_textAvMenuSelectMode;           // "Select Application Mode:"
        constexpr uint32_t kSelectLevelSet = sym::g_textAvMenuSelectLevelSet;   // "Select Level Export Location:"
        constexpr uint32_t kSelectPlayers = sym::g_textAvMenuSelectPlayers;     // "Select Number of Players:"
        constexpr uint32_t kSelectLevel = sym::g_textAvMenuSelectLevel;         // "Select Level:"
        constexpr uint32_t kSelectPark = sym::g_textAvMenuSelectPark;           // "Select Park:"
        constexpr uint32_t kSelectScreen = sym::g_textAvMenuSelectScreen;       // "Select Screen file:"
        constexpr uint32_t kSelectMovie = sym::g_textAvMenuSelectMovie;         // "Select Movie file:"
        constexpr uint32_t kNoText = sym::g_textNoLoadingFile;                  // ""
        // A list's lines, one text: a line break before each but the first.
        constexpr uint32_t kLineBreak = sym::g_textAvMenuLineBreak;             // "\n"
        constexpr uint32_t kLevelLine = sym::g_textAvMenuLevelLine;             // "%s%02d: %-8s    %s"
        constexpr uint32_t kMovieLine = sym::g_textAvMenuMovieLine;             // "%s%02d: %s"
        // The lists: the levels and the parks (a row's files' name and
        // title), the movies.
        struct NamedRow
        {
            uint8_t _00[4];
            ee::Ptr<const char> name;  // +0x04
            ee::Ptr<const char> title; // +0x08
            uint8_t _0c[4];
        };
        static_assert(sizeof(NamedRow) == 0x10);
        constexpr uint32_t kListedLevels = 12, kParks = 4, kMovies = 20;
        inline ee::Global<NamedRow[kListedLevels], sym::g_levels> g_levelRows;
        inline ee::Global<NamedRow[kParks], sym::g_parkThemes> g_parkRows;
        struct MovieRow
        {
            ee::Ptr<const char> name;
            uint32_t flags;
        };
        inline ee::Global<MovieRow[kMovies], sym::g_movieRows> g_movieRows;
        // The screen image and the movie picked.
        constexpr uint32_t kScreenFile = sym::g_textAvMenuScreenFile;           // "assets\\screens\\screen%d.raw"
        inline ee::Global<char[0x40], sym::g_avMenuScreenPath> g_avMenuScreenPath;
        constexpr uint32_t kMovieFile = sym::g_textMovieFile;                   // "assets\\movies\\%s.%s"
        constexpr uint32_t kMovieExtension = sym::g_textMovieExtension;         // "pss"
        constexpr uint32_t kScreenBitsPerPixel = 32;
        // The buttons that end the screen image: the face buttons and start.
        constexpr uint32_t kScreenSkipButtons = zge::kPadTriangle | zge::kPadSquare | zge::kPadCircle | zge::kPadCross | zge::kPadStart;

        // What each test sets up.
        inline ee::Global<uint8_t, sym::g_startUpKind> g_startUpKind;
        constexpr uint8_t kStartGameTest = 4, kStartLevelView = 5, kStartMotionView = 6;
        inline ee::Global<uint8_t, sym::g_levelArtSource> g_levelArtSource;
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        inline ee::Global<uint8_t, sym::g_parkTheme> g_parkTheme;
        inline ee::Global<uint8_t, sym::g_editorParkSource> g_editorParkSource;
        inline ee::Global<uint8_t, sym::g_frontEndFirstMenu> g_frontEndFirstMenu;
        constexpr uint8_t kFrontEndAfterRound = 4; // the front end's own start (screens.h)
        constexpr uint8_t kTourSession = 0, kAvMenuSession = 2, kMultiplayerSession = 3, kDemoSession = 5;
        constexpr uint8_t kTestCharacter2 = 1; // the second player's character in a two-player test
        // RiderCharacter::flags bit 1 (DM2's kCharacterMovieOpen), set on the
        // test's character.
        constexpr uint16_t kCharacterMovieOpen = 0x0002;

        // The empty scene's world: its cell size, then its bounds.
        struct SceneBounds
        {
            Vec4 low, high;
        };
        constexpr uint32_t kVersionFormat = sym::g_textAvMenuBuilt;             // "Built: %s %s"
        constexpr uint32_t kBuildDate = sym::g_textAvMenuBuildDate;             // "Oct 16 2002"
        constexpr uint32_t kBuildTime = sym::g_textAvMenuBuildTime;             // "11:36:20"
        constexpr uint32_t kVersionName = sym::g_textAvMenuVersion;             // "UI_AVMenu.Version"
        constexpr ee::Ptr<const char> kAvMenuScript{sym::g_textAvMenuScript};   // "AVMenu.zsc"

        Menu page(int32_t id)
        {
            Menu menu;
            menu.id = id;
            menu.handler = sym::avMenu;
            menu.data = 0;
            return menu;
        }
        ee::Ptr<char> endOf(ee::Ptr<char> text) { return text + zge::stringLength(text); }
        uint32_t lineSeparator(uint32_t line) { return line != 0 ? kLineBreak : kNoText; }
    }

    int32_t avMenuEnter()
    {
        ee::Local<Vec4> eye;
        ee::Local<Vec4> target;
        ee::Local<Vec4> cellSize;
        ee::Local<SceneBounds> bounds;
        ee::Local<uint8_t[4]> black;
        ee::Local<Menu> menu;
        ee::Local<char[0x40]> version;

        zge::resetCameraViewports(kMenuScreenWidth, kScreenHeight);
        // Only the three coordinates of each are set, as the original's.
        cellSize->x = ee::Float(50.0f);
        cellSize->z = ee::Float(50.0f);
        cellSize->y = ee::Float(50.0f);
        bounds->low.x = ee::Float(-100.0f);
        bounds->low.z = ee::Float(-100.0f);
        bounds->low.y = ee::Float(-100.0f);
        bounds->high.x = ee::Float(100.0f);
        bounds->high.z = ee::Float(100.0f);
        bounds->high.y = ee::Float(100.0f);
        zge::enterUnlistedScene(cellSize.address(), bounds.address());

        // The camera at the origin, looking down +z.
        eye->x = ee::Float(0.0f);
        eye->y = ee::Float(0.0f);
        eye->z = ee::Float(0.0f);
        target->x = ee::Float(0.0f);
        target->y = ee::Float(0.0f);
        target->z = ee::Float(500.0f);
        black[3] = 0xFF;
        black[0] = 0;
        black[1] = 0;
        black[2] = 0;
        g_bmxZgui->camera = ee::Ptr<zge::SceneObject>(zge::newSceneCamera(1));
        zge::lookFromAt(g_bmxZgui->camera, eye, target, 0);
        setCameraNodeClears(g_bmxZgui->camera, 1);
        setCameraNodeClearColour(g_bmxZgui->camera, black.address());
        zge::zgui::loadZguiScript(kAvMenuScript, g_bmxZgui->root);
        const ee::Ptr<Node> versionNode = zge::zgui::findNode(g_bmxZgui->root, kVersionName, kFrameNode);
        zge::formatString(version, kVersionFormat, kBuildDate, kBuildTime);
        zge::zgui::setVariable(versionNode, kTextName, version);
        *menu = page(kPageMode);
        setMenu(menu);
        *g_sessionKind = kAvMenuSession;
        *g_playerCount = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(avMenuEnter, Verified);

    void avMenuExit()
    {
        zge::zgui::destroyNode(zge::zgui::findNode(g_bmxZgui->root, kAvMenuUi, kFrameNode));
        zge::releaseSceneCamera(g_bmxZgui->camera);
        g_bmxZgui->camera = {};
        game::tail(zge::leaveScene);
    }
    READABLE_FUNCTION_WAITS(avMenuExit, Verified);

    int32_t avMenuFrame()
    {
        ee::Local<Menu> menu;
        readPads();
        menuFrame();
        switch (menuId())
        {
        case kPageShowScreen:
        {
            const ee::Ptr<zge::Pad> pad = rider::padInput(0)->pad;
            if ((pad->buttons & pad->changed) != 0)
            {
                *menu = page(kPageScreen);
                setMenu(menu);
            }
            break;
        }
        case kPagePlayMovie:
            if (!menuMovieFrame(zge::zgui::findNode(g_bmxZgui->root, kMovieNode, kFrameNode)))
            {
                *menu = page(kPageMovie);
                setMenu(menu);
            }
            break;
        default:
            break;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(avMenuFrame, Verified);

    void avMenu(int32_t message, uint32_t data)
    {
        ee::Local<char[0x400]> text;
        ee::Local<Menu> menu;
        ee::Local<AvMenuFrames> frames;
        ee::Local<int32_t> selected;
        ee::Local<uint32_t> visible;

        *frames = *g_avMenuFrames;
        zge::formatString(text, kFrameFormat, frames->name[menuId()]);
        const ee::Ptr<Node> frame = zge::zgui::findNode(g_bmxZgui->root, text, kFrameNode);
        const ee::Ptr<Node> status = zge::zgui::findNode(g_bmxZgui->root, kStatusName, kFrameNode);
        menu->data = 0;
        menu->handler = sym::avMenu;
        // The status line gets the text (formatted just before).
        const auto showStatus = [&]() { zge::zgui::setVariable(status, kTextName, text); };
        const auto menuNode = [&]() { return zge::zgui::findNode(frame, kMenuName, kFrameNode); };
        const auto selectedLine = [&]() { zge::zgui::copyVariable(menuNode(), kSelectedIndexName, selected); };
        const auto goTo = [&](int32_t id) {
            menu->id = id;
            setMenu(menu);
        };
        const auto showAvMenu = [&](uint32_t shown) {
            *visible = shown;
            zge::zgui::setVariable(zge::zgui::findNode(g_bmxZgui->root, kAvMenuUi, kFrameNode), kVisibleName, visible);
        };

        switch (message)
        {
        case kMsgEnter:
            switch (menuId())
            {
            case kPageMode:
                zge::formatString(text, kSelectMode);
                showStatus();
                break;
            case kPageLevelSet:
            case kPageLevelSet2:
                zge::formatString(text, kSelectLevelSet);
                showStatus();
                break;
            case kPagePlayers:
                zge::formatString(text, kSelectPlayers);
                showStatus();
                *selected = *g_playerCount;
                zge::zgui::setVariable(menuNode(), kSelectedIndexName, selected);
                break;
            case kPageLevel:
            case kPageLevel2:
                zge::formatString(text, kSelectLevel);
                showStatus();
                text[0] = 0;
                for (uint32_t level = 0; level < kListedLevels; ++level)
                {
                    const ee::Ptr<char> end = endOf(text);
                    zge::formatString(end, kLevelLine, lineSeparator(level), level, (*g_levelRows)[level].name,
                                      (*g_levelRows)[level].title);
                }
                zge::zgui::setVariable(menuNode(), kItemsName, text);
                break;
            case kPagePark:
                zge::formatString(text, kSelectPark);
                showStatus();
                text[0] = 0;
                for (uint32_t park = 0; park < kParks; ++park)
                {
                    const ee::Ptr<char> end = endOf(text);
                    zge::formatString(end, kLevelLine, lineSeparator(park), park, (*g_parkRows)[park].name,
                                      (*g_parkRows)[park].title);
                }
                zge::zgui::setVariable(menuNode(), kItemsName, text);
                break;
            case kPageScreen:
                zge::formatString(text, kSelectScreen);
                showStatus();
                break;
            case kPageMovie:
                zge::formatString(text, kSelectMovie);
                showStatus();
                text[0] = 0;
                for (uint32_t movie = 0; movie < kMovies; ++movie)
                {
                    // Numbered from 1.
                    const ee::Ptr<char> end = endOf(text);
                    zge::formatString(end, kMovieLine, lineSeparator(movie), movie + 1, (*g_movieRows)[movie].name);
                }
                zge::zgui::setVariable(menuNode(), kItemsName, text);
                break;
            case kPageShowScreen:
            case kPagePlayMovie:
            case kPageLeave:
                playUiNode(zge::zgui::findNode(g_bmxZgui->root, kAvMenuUi, kFrameNode), kFadeOutAction);
                break;
            default:
                break;
            }
            if (frame)
                playUiNode(frame, kInitAction);
            return;

        case kMsgCameIn:
            switch (menuId())
            {
            case kPageShowScreen:
                // The menu hidden, the image over the whole screen until a
                // face button or start is pressed on the first pad.
                showAvMenu(0);
                zge::resetCameraViewports(kScreenWidth, kScreenHeight);
                if (zge::showRawImage(sym::g_avMenuScreenPath, kScreenWidth, kScreenHeight, kScreenBitsPerPixel))
                {
                    const ee::Ptr<zge::Pad> pad = rider::padInput(0)->pad;
                    if ((pad->buttons & pad->changed & kScreenSkipButtons) == 0)
                    {
                        do
                            readPads();
                        while ((pad->buttons & pad->changed & kScreenSkipButtons) == 0);
                    }
                }
                goTo(kPageScreen);
                return;
            case kPagePlayMovie:
                showAvMenu(0);
                zge::resetCameraViewports(kScreenWidth, kScreenHeight);
                *visible = static_cast<uint32_t>(playMenuMovie(sym::g_moviePath, 0));
                if (*visible == 0)
                    goTo(kPageMovie);
                return;
            case kPageLeave:
                setGameState(*g_avMenuNextState);
                return;
            default:
                return;
            }

        case kMsgLeave:
            switch (menuId())
            {
            case kPagePlayMovie:
                *visible = 0;
                zge::zgui::setVariable(zge::zgui::findNode(g_bmxZgui->root, kMovieNode, kFrameNode), kVisibleName, visible);
                stopMenuMovie();
                [[fallthrough]];
            case kPageShowScreen:
                zge::resetCameraViewports(kMenuScreenWidth, kScreenHeight);
                showAvMenu(1);
                playUiNode(zge::zgui::findNode(g_bmxZgui->root, kAvMenuUi, kFrameNode), kFadeInAction);
                break;
            default:
                break;
            }
            zge::formatString(text, kNoText);
            showStatus();
            if (frame)
                playUiNode(frame, kDeInitAction);
            return;

        case kMsgChosen:
            switch (menuId())
            {
            case kPageMode:
            {
                selectedLine();
                const auto testCharacter = [&](uint8_t startUp) {
                    // The first player's character is the team, opened.
                    const uint8_t character = (*g_playerSetups)[0].character;
                    g_savedGame->teamRewards = 1;
                    g_savedGame->teamMembers[0] = (*g_playerSetups)[0].character;
                    *g_startUpKind = startUp;
                    g_savedGame->characters[character].flags |= kCharacterMovieOpen;
                    *g_sessionKind = kTourSession;
                };
                switch (*selected)
                {
                case kModeMotionView:
                    *g_startUpKind = kStartMotionView;
                    *g_avMenuNextState = kGameStateMotionViewer;
                    goTo(kPageLeave);
                    return;
                case kModeGameTest:
                    testCharacter(kStartGameTest);
                    goTo(kPagePlayers);
                    return;
                case kModeParkEditorTest:
                    goTo(kPagePark);
                    return;
                case kModeMenuTest:
                    *g_frontEndFirstMenu = kFrontEndAfterRound;
                    *g_avMenuNextState = kGameStateFrontEnd;
                    goTo(kPageLeave);
                    return;
                case kModeMovieTest:
                    goTo(kPageMovie);
                    return;
                case kModeLevelView:
                default:
                    testCharacter(kStartLevelView);
                    goTo(kPageLevelSet);
                    return;
                }
            }
            case kPageLevelSet:
            case kPageLevelSet2:
                selectedLine();
                *g_levelArtSource = static_cast<uint8_t>(*selected);
                goTo(kPageLevel);
                return;
            case kPageLevel:
            case kPageLevel2:
                selectedLine();
                *g_currentLevel = static_cast<uint8_t>(*selected);
                *g_avMenuNextState = kGameStateLevel;
                goTo(kPageLeave);
                return;
            case kPagePlayers:
            {
                selectedLine();
                const uint8_t players = static_cast<uint8_t>(*selected);
                *g_playerCount = players;
                if (players >= 2)
                {
                    *g_sessionKind = kMultiplayerSession;
                    (*g_playerSetups)[1].character = kTestCharacter2;
                }
                else if (players == 0)
                {
                    *g_sessionKind = kDemoSession;
                }
                goTo(kPageLevelSet);
                return;
            }
            case kPagePark:
                selectedLine();
                *g_parkTheme = static_cast<uint8_t>(*selected);
                *g_avMenuNextState = kGameStateParkEditor;
                *g_editorParkSource = 0;
                goTo(kPageLeave);
                return;
            case kPageScreen:
                selectedLine();
                zge::formatString(sym::g_avMenuScreenPath, kScreenFile, *selected + 1);
                goTo(kPageShowScreen);
                return;
            case kPageMovie:
                selectedLine();
                zge::formatString(sym::g_moviePath, kMovieFile, (*g_movieRows)[*selected].name, kMovieExtension);
                goTo(kPagePlayMovie);
                return;
            default:
                return;
            }

        case kMsgCancelled:
            switch (menuId())
            {
            case kPageMode:
                return;
            case kPagePlayers:
            case kPagePark:
            case kPageScreen:
            case kPageMovie:
                goTo(kPageMode);
                return;
            default:
                // The page before.
                goTo(menuId() - 1);
                return;
            }

        default:
            return;
        }
    }
    READABLE_FUNCTION_WAITS(avMenu, Verified);
}
