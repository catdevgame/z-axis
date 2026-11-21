// BMX XXX's intro state (intro.h): BMX XXX's own code (bmx.c).

#include "intro.h"
#include "front_end_menus.h"

#include "bmx/input/pads.h"
#include "bmx/rider/rider.h"
#include "bmx/save/game_save.h"
#include "bmx/save/park_save.h"
#include "bmx/sound/soundtrack.h"
#include "bmx/states/game_state.h"
#include "bmx/ui/menu_movie.h"
#include "bmx/ui/menu.h"
#include "bmx/ui/ui_nodes.h"
#include "zge/input/pads.h"
#include "zge/text/cstring.h"
#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "bmx/camera/scene_camera.h"
#include "ee/readable.h"
#include "zge/math/types.h"
#include "zge/render/display.h"
#include "zge/scene/camera.h"
#include "zge/scene/scene.h"
#include "zge/scene/scene_database.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/states/intro.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        // The UI's camera (a word of zgui's globals in BMX XXX), and zgui's
        // root node.
        inline ee::Global<ee::Ptr<SceneObject>, sym::g_uiCamera> g_uiCamera;
        inline ee::Global<ee::Ptr<zgui::Node>, sym::g_uiRoot> g_uiRoot;
        // The intro's loading step (1 at the state's enter).
        inline ee::Global<uint32_t, sym::g_introLoadStep> g_introLoadStep;
        constexpr ee::Ptr<const char> kIntroUi{sym::g_textUiIntro}; // "UI_intro"
        // findNode's kind: a frame.
        constexpr uint32_t kFrameNode = 1;
        constexpr uint32_t kScreenWidth = 640, kScreenHeight = 448;

        // The empty scene's world: its cell size, then its bounds.
        struct SceneBounds
        {
            Vec4 low, high;
        };

        // A camera node's scene word (sceneOf), which holds its camera.

        constexpr ee::Ptr<const char> kIntroScript{sym::g_textIntroScript};         // "intro.zsc"
        constexpr ee::Ptr<const char> kRiderAnimations{sym::g_textRiderAnimations}; // "assets\\motgame.zal"
        // The riders' animation archive (the game's end frees it).
        inline ee::Global<int32_t, sym::g_riderAnimations> g_riderAnimations;
        // The intro's menu: id 0, its handler (BMX XXX's own, not written yet).
        constexpr int32_t kIntroMenu = 0;
        constexpr uint32_t kIntroMenuHandler = sym::introMenu;
        // The loading's steps.
        constexpr uint32_t kLoadUi = 1, kLoadSaves = 3, kLoadSound = 5, kLoadAnimations = 6, kLoadLast = 7;

        // The intro's screens, by the menu's id.
        constexpr int32_t kIntroStart = 0, kDeveloperSplash = 4, kIntroMovie = 5, kIntroEnd = 6;
        constexpr uint32_t kIntroMenus = 7;
        // How long each screen stays, in frames it has had the screen: past
        // `skipAfter` a pressed button ends it, past `endAfter` it ends by
        // itself (0: never, for either); `loadStep` is the loading step run
        // at the screen's first frame (0: none).
        struct IntroTiming
        {
            uint16_t skipAfter; // +0x00
            uint16_t endAfter;  // +0x02
            uint8_t loadStep;   // +0x04
            uint8_t _05;
        };
        static_assert(sizeof(IntroTiming) == 6);
        inline ee::Global<IntroTiming[kIntroMenus], sym::g_introTimings> g_introTimings;
        // The screens' UI nodes ("UI_intro.FrameSplashPiracy" .. "UI_common.Movie";
        // "" for the start and the end), and the developer splash's quotes.
        constexpr uint32_t kIntroQuotes = 5;
        inline ee::Global<ee::Ptr<const char>[kIntroMenus], sym::g_introScreenNames> g_introScreenNames;
        inline ee::Global<ee::Ptr<const char>[kIntroQuotes], sym::g_introQuotes> g_introQuotes;

        // What the game was started for (the AV menu sets 4 to 6; who sets
        // 2 and 3 is not read): the intro's start goes there instead of
        // showing the screens.
        inline ee::Global<uint8_t, sym::g_startUpKind> g_startUpKind;
        constexpr uint8_t kStartLevelSession1 = 2, kStartLevelSession5 = 3, kStartAvMenuFirst = 4, kStartAvMenuLast = 6;
        // The menu the front end's card menus come back to, and its first
        // menu: after the intro, the game's load from the card (kMenuAutoLoad)
        // coming back to the press-start menu (front_end_menus.h).
        inline ee::Global<uint8_t, sym::g_frontEndReturnMenu> g_frontEndReturnMenu;
        inline ee::Global<uint8_t, sym::g_frontEndFirstMenu> g_frontEndFirstMenu;
        inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
        inline ee::Global<uint8_t, sym::g_sessionKind> g_sessionKind;
        inline ee::Global<uint32_t, sym::g_gameFrames> g_gameFrames;
        inline ee::Global<int32_t, sym::g_msgType> g_msgType;

        constexpr uint32_t kNameFormat = sym::bmx::g_textIntroNameFormat;            // "%s"
        constexpr uint32_t kNoQuote = sym::bmx::g_textIntroNoQuote;                  // "$KSTR_LOGOQUOTE_00" (the logo quote's string key)
        constexpr ee::Ptr<const char> kMovieNode{sym::g_textUiCommonMovie};          // "UI_Common.Movie"
        constexpr ee::Ptr<const char> kBackground{sym::g_textUiIntroBackground};     // "UI_intro.Background"
        constexpr ee::Ptr<const char> kFadeIn{sym::g_textIntroFadeIn};               // "FadeIn"
        constexpr ee::Ptr<const char> kFadeOut{sym::g_textIntroFadeOut};             // "FadeOut"
        constexpr ee::Ptr<const char> kInit{sym::g_textIntroInit};                   // "Init"
        constexpr ee::Ptr<const char> kShutdown{sym::g_textIntroShutdown};           // "Shutdown"
        constexpr ee::Ptr<const char> kQuoteNode{sym::g_textIntroQuote};             // "Quote"
        constexpr ee::Ptr<const char> kTextVariable{sym::g_textIntroText};           // "Text"

        // Whether a port's pad is there with triangle held.
        bool triangleHeld(uint32_t port)
        {
            const ee::Ptr<zge::Pad> pad = rider::padInput(port)->pad;
            return pad && (pad->buttons & zge::kPadTriangle) != 0;
        }
    }

    void setCameraNodeClears(ee::Ptr<SceneObject> camera, uint32_t on)
    {
        const ee::Ptr<Camera> record = objectCamera(camera);
        game::tail(setCameraClears, record, on);
    }
    READABLE_FUNCTION(setCameraNodeClears, Verified);

    void setCameraNodeClearColour(ee::Ptr<SceneObject> camera, uint32_t colour)
    {
        const ee::Ptr<Camera> record = objectCamera(camera);
        game::tail(setCameraClearColour, record, colour);
    }
    READABLE_FUNCTION(setCameraNodeClearColour, Verified);

    void setCameraNodeFarClip(ee::Ptr<SceneObject> camera, ee::Float distance)
    {
        const ee::Ptr<Camera> record = objectCamera(camera);
        game::tail(setCameraFarPlane, record, distance);
    }
    READABLE_FUNCTION(setCameraNodeFarClip, Verified);

    int32_t introEnter()
    {
        ee::Local<Vec4> eye;
        ee::Local<Vec4> target;
        ee::Local<Vec4> cellSize;
        ee::Local<SceneBounds> bounds;
        ee::Local<uint8_t[4]> black;

        resetCameraViewports(kScreenWidth, kScreenHeight);
        // Only the three coordinates of each are set, as the original's.
        cellSize->x = ee::Float(10.0f);
        cellSize->z = ee::Float(10.0f);
        cellSize->y = ee::Float(10.0f);
        bounds->low.x = ee::Float(-5.0f);
        bounds->low.z = ee::Float(-5.0f);
        bounds->low.y = ee::Float(-5.0f);
        bounds->high.x = ee::Float(5.0f);
        bounds->high.z = ee::Float(5.0f);
        bounds->high.y = ee::Float(5.0f);
        enterUnlistedScene(cellSize.address(), bounds.address());

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
        const ee::Ptr<SceneObject> camera(newSceneCamera(1));
        *g_uiCamera = camera;
        lookFromAt(camera, eye, target, 0);
        setCameraNodeClears(*g_uiCamera, 1);
        setCameraNodeClearColour(*g_uiCamera, black.address());
        *g_introLoadStep = 1;
        return 1;
    }
    READABLE_FUNCTION_WAITS(introEnter, Verified);

    int32_t introLoad()
    {
        ee::Local<Menu> menu;
        const uint32_t step = *g_introLoadStep;
        switch (step)
        {
        case kLoadUi:
            zgui::loadZguiScript(kIntroScript, *g_uiRoot);
            menu->id = kIntroMenu;
            menu->data = 0;
            menu->handler = kIntroMenuHandler;
            setMenu(menu);
            *g_introLoadStep = *g_introLoadStep + 1;
            break;
        case kLoadSaves:
            registerGameSaveClient();
            registerParkSaveClient();
            return 0;
        case kLoadSound:
            initGameSound();
            *g_introLoadStep = *g_introLoadStep + 1;
            break;
        case kLoadAnimations:
            *g_riderAnimations = loadAnimationArchive(kRiderAnimations);
            *g_introLoadStep = kLoadLast;
            break;
        case kLoadLast:
            return 0;
        case 2:
        case 4:
            // Steps with nothing to load.
            *g_introLoadStep = step + 1;
            break;
        default:
            break;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(introLoad, Verified);

    int32_t introFrame()
    {
        readPads();
        menuFrame();
        const IntroTiming &timing = (*g_introTimings)[menuId()];
        const uint32_t frames = menuFrames();
        if (timing.loadStep != 0 && frames == 1)
        {
            *g_introLoadStep = timing.loadStep;
            introLoad();
        }
        const bool skipped = timing.skipAfter != 0 && anyButtonPressed() && timing.skipAfter < frames;
        if (skipped || (timing.endAfter != 0 && timing.endAfter < frames))
        {
            *g_msgType = 0;
            bmxuiHandler();
        }
        if (menuId() == kIntroMovie)
        {
            if (!menuMovieFrame(zgui::findNode(*g_uiRoot, kMovieNode, kFrameNode)))
            {
                *g_msgType = 0;
                bmxuiHandler();
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(introFrame, Verified);

    void introMenu(int32_t event)
    {
        ee::Local<Menu> next;
        ee::Local<char[0x100]> name;
        // (The two tables are copied to locals first, as the original's.)
        ee::Local<ee::Ptr<const char>[kIntroMenus]> screenNames;
        ee::Local<ee::Ptr<const char>[kIntroQuotes]> quotes;
        for (uint32_t i = 0; i < kIntroMenus; ++i)
            screenNames[i] = (*g_introScreenNames)[i];
        for (uint32_t i = 0; i < kIntroQuotes; ++i)
            quotes[i] = (*g_introQuotes)[i];

        zge::formatString(name, kNameFormat, screenNames[menuId()]);
        const ee::Ptr<zgui::Node> screen = zgui::findNode(*g_uiRoot, name, kFrameNode);
        next->data = 0;
        next->handler = kIntroMenuHandler;

        switch (event)
        {
        case 0:
            // The screen has ended: the next one.
            menuId();
            next->id = menuId() + 1;
            setMenu(next);
            break;
        case kMsgEnter:
            switch (menuId())
            {
            case kIntroStart:
            {
                const uint8_t kind = *g_startUpKind;
                bool started = false;
                if (kind == kStartLevelSession5)
                {
                    *g_currentLevel = 0;
                    *g_sessionKind = 5;
                    setGameState(kGameStateLevel);
                    started = true;
                }
                else if (kind == kStartLevelSession1)
                {
                    *g_currentLevel = 0;
                    *g_sessionKind = 1;
                    setGameState(kGameStateLevel);
                    started = true;
                }
                else if (kind >= kStartAvMenuFirst && kind <= kStartAvMenuLast)
                {
                    setGameState(kGameStateAvMenu);
                    started = true;
                }
                if (started)
                {
                    // No screens: the rest of the loading runs here.
                    *g_introLoadStep = 4;
                    while (introLoad() != 0)
                    {
                    }
                    return;
                }
                playUiNode(zgui::findNode(*g_uiRoot, kBackground, kFrameNode), kFadeIn);
                break;
            }
            case kDeveloperSplash:
                // With triangle held on both pads the splash shows a quote.
                if (triangleHeld(0) && triangleHeld(1))
                    zge::formatString(name, kNameFormat, quotes[(*g_gameFrames + 1) % kIntroQuotes]);
                else
                    zge::formatString(name, kNoQuote);
                zgui::setVariable(zgui::findNode(screen, kQuoteNode, kFrameNode), kTextVariable, name);
                break;
            case kIntroMovie:
                // The movie's screen is passed at once (its frame shows it).
                next->id = menuId() + 1;
                setMenu(next);
                return;
            case kIntroEnd:
                playUiNode(zgui::findNode(*g_uiRoot, kBackground, kFrameNode), kFadeOut);
                break;
            default:
                break;
            }
            if (screen)
                playUiNode(screen, kInit);
            break;
        case kMsgCameIn:
        {
            const int32_t id = menuId();
            if (id == kIntroStart)
            {
                // The start goes to the legal splash (the piracy one is
                // passed over).
                next->id = 2;
                setMenu(next);
            }
            else if (id == kIntroEnd)
            {
                *g_frontEndReturnMenu = kMenuPressStart;
                *g_frontEndFirstMenu = kMenuAutoLoad;
                setGameState(kGameStateFrontEnd);
            }
            break;
        }
        case kMsgLeave:
            if (menuId() == kIntroStart)
            {
                // The start has nothing to take out: it has left.
                *g_msgType = kMsgLeft;
                bmxuiHandler();
            }
            if (screen)
                playUiNode(screen, kShutdown);
            break;
        case kMsgLeft:
            if (menuId() == kIntroMovie)
            {
                stopMenuMovie();
                resetCameraViewports(kScreenWidth, kScreenHeight);
            }
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION_WAITS(introMenu, Verified);

    void introExit()
    {
        zgui::destroyNode(zgui::findNode(*g_uiRoot, kIntroUi, kFrameNode));
        releaseSceneCamera(*g_uiCamera);
        *g_uiCamera = ee::Ptr<SceneObject>();
        game::tail(leaveScene);
    }
    READABLE_FUNCTION_WAITS(introExit, Verified);
}
