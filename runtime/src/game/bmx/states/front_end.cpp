// BMX XXX's front end state (front_end.h): the menus ("UI_menu"). BMX XXX's
// own code (bmx.c's menu block).

#include "front_end.h"
#include "front_end_menus.h"

#include "bmx/create/menu_model.h"
#include "bmx/game/player_riders.h"
#include "bmx/profile/players.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/states/game_state.h"
#include "bmx/rider/rider.h"
#include "bmx/input/pads.h"
#include "bmx/ui/menu.h"
#include "bmx/ui/menu_movie.h"
#include "bmx/ui/menu_script.h"
#include "bmx/ui/ui_calls.h"
#include "bmx/ui/ui_nodes.h"
#include "bmx/ui/zgui_globals.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/chunk_reader.h"
#include "zge/render/display.h"
#include "zge/scene/animation.h"
#include "zge/scene/animator.h"
#include "zge/math/types.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"

#include <cstdint>
#include "sym/bmx/states/front_end.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The menus' UI tree, and findNode's kind for it: a frame.
        constexpr ee::Ptr<const char> kMenuUi{sym::g_textFrontEndUi}; // "UI_menu"
        constexpr uint32_t kFrameNode = 1;
        // The menus' animation archive ("assets\\motmenu.zal", the loading's).
        inline ee::Global<uint32_t, sym::g_menuAnimations> g_menuAnimations;

        // The loading's steps; the loading frame shows for 72 frames at the
        // first and the last.
        enum FrontEndLoadStep : uint32_t
        {
            kLoadShowLoading = 0, // the loading frame up
            kLoadScene = 1,       // the empty scene and the UI's camera
            kLoadMenus = 2,       // the menus' script ("menu.zsc")
            kLoadNothing = 3,
            kLoadAnimations = 4,  // the menus' animations, with the rotation key form set to 1
            kLoadScripts = 5,     // the menus' script functions, the menus' rider model
            kLoadSound = 6,       // the sound's front end mode
            kLoadHideLoading = 7, // the loading frame shut down, then the first menu
            kLoadDone = 8,        // the loading frame put back: the loading is over
        };
        constexpr uint32_t kLoadingFrames = 72;
        // The loading frame and its parts, and what the loading plays on
        // them.
        constexpr ee::Ptr<const char> kLoadingFrame{sym::g_textFrontEndLoadingFrame}; // "UI_common.FrameLoading"
        constexpr ee::Ptr<const char> kSplashPicture{sym::g_textFrontEndSplashPicture}; // "SplashPicture"
        constexpr ee::Ptr<const char> kSplashCenter{sym::g_textFrontEndSplashCenter};   // "SplashCenter"
        constexpr ee::Ptr<const char> kPosition{sym::g_textFrontEndPosition};           // "Position"
        constexpr ee::Ptr<const char> kInit{sym::g_textFrontEndInit};                   // "Init"
        constexpr ee::Ptr<const char> kShutdown{sym::g_textFrontEndShutdown};           // "Shutdown"
        constexpr ee::Float kLoadingFrameX = 64.0f;
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_frontEndLoadingFrame> g_frontEndLoadingFrame;
        // The front end's loading bar (the chunk reader's progress hook while
        // it loads).
        constexpr zge::ChunkProgressHook kFrontEndLoadingBar{sym::frontEndLoadingBar};
        constexpr uint32_t kScreenWidth = 640, kScreenHeight = 448;
        constexpr ee::Ptr<const char> kMenuScript{sym::g_textFrontEndScript};         // "menu.zsc"
        constexpr ee::Ptr<const char> kMenuAnimations{sym::g_textMenuAnimations};    // "assets\\motmenu.zal"
        // The empty scene's world: its cell size, then its bounds.
        struct SceneBounds
        {
            zge::Vec4 low, high;
        };
        // The first menu, and its handler (front_end_menu.cpp).
        constexpr int32_t kFirstMenu = kMenuStart;
        constexpr uint32_t kFrontEndMenuHandler = sym::frontEndMenu;
        // What the front end starts as: one player, session kind 2.
        constexpr uint8_t kFrontEndSessionKind = 2;
        // The menus' movie (none at the start).
        inline ee::Global<uint32_t, sym::g_frontEndMovie> g_frontEndMovie;
    }

    // frontEndEnter: the loading starts at its first step. Always 1.
    int32_t frontEndEnter()
    {
        setLoadStep(0);
        return 1;
    }
    READABLE_FUNCTION(frontEndEnter, Verified);

    // frontEndLoad: a step a frame (FrontEndLoadStep); 0 once the loading
    // is over, else 1. The loading frame's steps run 72 frames each: at the
    // first the loading bar is hooked to the chunk reader and the frame shown
    // at its place; at the last the frame is shut down, and at the 72nd the
    // first menu comes up for one player.
    int32_t frontEndLoad()
    {
        ee::Local<zge::Vec4> eye;
        ee::Local<zge::Vec4> target;
        ee::Local<zge::Vec4> cellSize;
        ee::Local<SceneBounds> bounds;
        ee::Local<uint8_t[4]> black;
        ee::Local<Menu> menu;
        ee::Local<zge::Vec4> position;

        switch (loadStep())
        {
        case kLoadShowLoading:
            if (*g_loadStepFrames == 1)
            {
                zge::setChunkProgressHook(kFrontEndLoadingBar, 0);
                zge::resetCameraViewports(kScreenWidth, kScreenHeight);
                *g_frontEndLoadingFrame = zge::zgui::findNode(g_bmxZgui->root, kLoadingFrame, kFrameNode);
                // (Its w is left as the stack had it, as the original's.)
                position->x = kLoadingFrameX;
                position->y = 0.0f;
                position->z = 0.0f;
                zge::zgui::setVariable(*g_frontEndLoadingFrame, kPosition, position);
                playUiNode(*g_frontEndLoadingFrame, kInit);
                playUiNode(zge::zgui::findNode(*g_frontEndLoadingFrame, kSplashPicture, kFrameNode), kInit);
            }
            else if (*g_loadStepFrames == kLoadingFrames)
            {
                setLoadStep(loadStep() + 1);
            }
            break;
        case kLoadScene:
            // Only the three coordinates of each are set, as the original's.
            cellSize->x = 10.0f;
            cellSize->z = 10.0f;
            cellSize->y = 10.0f;
            bounds->low.x = -5.0f;
            bounds->low.z = -5.0f;
            bounds->low.y = -5.0f;
            bounds->high.x = 5.0f;
            bounds->high.z = 5.0f;
            bounds->high.y = 5.0f;
            zge::enterUnlistedScene(cellSize.address(), bounds.address());
            // The camera at the origin, looking down +z.
            eye->x = 0.0f;
            eye->y = 0.0f;
            eye->z = 0.0f;
            target->x = 0.0f;
            target->y = 0.0f;
            target->z = 500.0f;
            black[3] = 0xFF;
            black[0] = 0;
            black[1] = 0;
            black[2] = 0;
            g_bmxZgui->camera = ee::Ptr<zge::SceneObject>(zge::newSceneCamera(1));
            zge::lookFromAt(g_bmxZgui->camera, eye, target, 0);
            setMenuCameraNodeClears(g_bmxZgui->camera, 1);
            setMenuCameraNodeClearColour(g_bmxZgui->camera, black.address());
            setLoadStep(loadStep() + 1);
            break;
        case kLoadMenus:
            zge::zgui::loadZguiScript(kMenuScript, g_bmxZgui->root);
            setLoadStep(loadStep() + 1);
            break;
        case kLoadNothing:
            setLoadStep(loadStep() + 1);
            break;
        case kLoadAnimations:
        {
            const uint32_t form = zge::rotationKeyForm();
            zge::setRotationKeyForm1();
            *g_menuAnimations = static_cast<uint32_t>(zge::loadAnimationArchive(kMenuAnimations));
            zge::setRotationKeyForm(form);
            setLoadStep(loadStep() + 1);
            break;
        }
        case kLoadScripts:
            registerMenuScriptFunctions();
            startMenuModel();
            setLoadStep(loadStep() + 1);
            break;
        case kLoadSound:
            setGameMode(kFrontEndMode);
            setLoadStep(loadStep() + 1);
            break;
        case kLoadHideLoading:
            if (*g_loadStepFrames == 1)
            {
                zge::setChunkProgressHook(zge::ChunkProgressHook(), 0);
                playUiNode(zge::zgui::findNode(*g_frontEndLoadingFrame, kSplashPicture, kFrameNode), kShutdown);
                playUiNode(zge::zgui::findNode(*g_frontEndLoadingFrame, kSplashCenter, kFrameNode), kShutdown);
                playUiNode(*g_frontEndLoadingFrame, kShutdown);
            }
            else if (*g_loadStepFrames == kLoadingFrames)
            {
                menu->id = kFirstMenu;
                menu->handler = kFrontEndMenuHandler;
                menu->data = 0;
                setMenu(menu);
                *g_playerCount = 1;
                *g_sessionKind = kFrontEndSessionKind;
                *g_frontEndMovie = 0;
                setLoadStep(loadStep() + 1);
            }
            break;
        case kLoadDone:
            *position = zge::Vec4{0.0f, 0.0f, 0.0f, 1.0f};
            zge::zgui::setVariable(*g_frontEndLoadingFrame, kPosition, position);
            return 0;
        default:
            break;
        }
        *g_loadStepFrames = *g_loadStepFrames + 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(frontEndLoad, Verified);

    // frontEndExit: the menus' rider model put away and the menus' tree
    // destroyed, their animations freed, the UI's camera released, the
    // scene left (the frame's packets reset before and after) and the sound
    // set to no mode.
    void frontEndExit()
    {
        unusedFrontEndExitHook();
        stopMenuModel();
        zge::zgui::destroyNode(zge::zgui::findNode(g_bmxZgui->root, kMenuUi, kFrameNode));
        zge::freeAnimationLibrary(*g_menuAnimations);
        zge::releaseSceneCamera(g_bmxZgui->camera);
        g_bmxZgui->camera = ee::Ptr<zge::SceneObject>();
        zge::resetFramePackets();
        zge::leaveScene();
        zge::resetFramePackets();
        game::tail(setGameMode, kSoundModeNone);
    }
    READABLE_FUNCTION(frontEndExit, Verified);

    namespace
    {
        // (The menus by id: front_end_menus.h. Three play a movie on
        // CommonBackground.FrameExtras.FramePlayMovie.)
        constexpr ee::Ptr<const char> kDemoLoopNode{sym::g_textDemoLoopNode};   // "UI_menu.CommonBackground.DemoLoop"
        constexpr ee::Ptr<const char> kPlayMovieNode{sym::g_textPlayMovieNode}; // "UI_menu.CommonBackground.FrameExtras.FramePlayMovie"

        // The pads connected (each frame's count), the frames the front end
        // has been idle (no button changed on either pad), and the game state
        // the front end leaves for (kMenuLeaveFrontEnd's: the idle demo's is
        // the level).
        inline ee::Global<uint32_t, sym::g_connectedPads> g_connectedPads;
        inline ee::Global<uint32_t, sym::g_frontEndIdleFrames> g_frontEndIdleFrames;
        inline ee::Global<uint8_t, sym::g_frontEndNextState> g_frontEndNextState;
        // How long the screen may be idle; 0: for ever (level_frame.cpp's).
        inline ee::Global<uint32_t, sym::g_screenIdleFrames> g_screenIdleFrames;
        // What the game was started for (b7bmxc3's name): kinds 1 .. 3 go
        // back to the press-start menu when idle.
        inline ee::Global<uint8_t, sym::g_startUpKind> g_startUpKind;
        constexpr uint8_t kFirstIdleToPressStart = 1, kIdleToPressStartEnd = 4;
        // The idle demo's session: one rider (player_riders.h).
        constexpr uint8_t kIdleDemoSessionKind = 5;
        // The message the frame sends the menu (the UI's "MsgType"): 0 as
        // the screen is over (the intro's handler takes it so), -1 by the
        // second controller's back buttons or a movie's end (value-named:
        // what frontEndMenu does with it is not read yet).
        inline ee::Global<int32_t, sym::g_msgType> g_msgType;
        constexpr int32_t kMsgScreenOver = 0, kMsgMinus1 = -1;
        // The buttons: the press-start menu's (bit 14), and the two that
        // send -1 (bits 6 and 15), pressed this frame.
        constexpr uint32_t kStartButton = 0x4000, kBackButtons = 0x8040;
        constexpr uint32_t kPads = 2;

        // The buttons a pad pressed this frame.
        uint32_t pressedButtons(uint32_t pad)
        {
            const rider::PadInput *const input = rider::padInput(pad);
            return input->buttons.held & input->buttons.changed;
        }
        // The message sent to the menu for each pad of `first` .. 1 that
        // pressed one of `buttons`.
        void sendForPads(uint32_t first, uint32_t buttons, int32_t message)
        {
            for (uint32_t pad = first; pad < kPads; ++pad)
            {
                if ((pressedButtons(pad) & buttons) != 0)
                {
                    *g_msgType = message;
                    bmxuiHandler();
                }
            }
        }
        // The menu's movie on its node played on; at its end, -1 sent.
        void playMenuMovieOn(ee::Ptr<const char> node)
        {
            if (!menuMovieFrame(zge::zgui::findNode(g_bmxZgui->root, node, kFrameNode)))
            {
                *g_msgType = kMsgMinus1;
                bmxuiHandler();
            }
        }
        void askForMenu(ee::Local<Menu> &menu, int32_t id)
        {
            menu->id = id;
            menu->handler = kFrontEndMenuHandler;
            menu->data = 0;
            setMenu(menu);
        }
    }

    // frontEndFrame: the pads read and counted, the menu machine's frame;
    // a button on either pad keeps the front end from being idle. Idle too
    // long, the press-start menu leaves for the demo (session kind 5, the
    // level state), and
    // the main menu (or any menu, when the game was started for kinds 1 ..
    // 3) goes back to the press-start menu. Else the menu that has the
    // screen gets its frame: the press-start menu Start, the demo loop and
    // the movies their movie, the main menu its own, the second
    // controller's screen its back buttons and the second pad's Start.
    int32_t frontEndFrame()
    {
        ee::Local<Menu> menu;
        *g_connectedPads = 0;
        readPads();
        for (uint32_t pad = 0; pad < kPads; ++pad)
        {
            if ((rider::padInput(pad)->flags & rider::kPadMissing) == 0)
                *g_connectedPads = *g_connectedPads + 1;
        }
        menuFrame();
        for (uint32_t pad = 0; pad < kPads; ++pad)
        {
            if (rider::padInput(pad)->buttons.changed != 0)
                *g_frontEndIdleFrames = 0;
        }

        if (*g_screenIdleFrames != 0)
        {
            *g_frontEndIdleFrames = *g_frontEndIdleFrames + 1;
            if (*g_screenIdleFrames < *g_frontEndIdleFrames)
            {
                if (menuId() == kMenuPressStart)
                {
                    *g_sessionKind = kIdleDemoSessionKind;
                    *g_frontEndNextState = kGameStateLevel;
                    askForMenu(menu, kMenuLeaveFrontEnd);
                    *g_frontEndIdleFrames = 0;
                    return 1;
                }
                const uint8_t kind = *g_startUpKind;
                if ((kind < kIdleToPressStartEnd && kind >= kFirstIdleToPressStart) || menuId() == kMenuMainMenu)
                {
                    askForMenu(menu, kMenuPressStart);
                    *g_frontEndIdleFrames = 0;
                    return 1;
                }
            }
        }

        switch (menuId())
        {
        case kMenuPressStart:
            sendForPads(0, kStartButton, kMsgScreenOver);
            break;
        case kMenuDemoLoop:
            playMenuMovieOn(kDemoLoopNode);
            break;
        case kMenuMainMenu:
            mainMenuFrame();
            break;
        case kMenuPlayer2Ctrl:
            sendForPads(0, kBackButtons, kMsgMinus1);
            sendForPads(1, kStartButton, kMsgScreenOver);
            break;
        case kMenuPlayMovie:
        case kMenuPlayMovie2:
        case kMenuPlayMovie3:
            playMenuMovieOn(kPlayMovieNode);
            break;
        default:
            break;
        }
        unusedFrontEndFrameHook();
        menuModelFrame();
        return 1;
    }
    READABLE_FUNCTION(frontEndFrame, Verified);
}
