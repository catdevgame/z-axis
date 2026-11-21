// BMX XXX's game screens (screens.h): the overlay's opening and closing,
// the screens' input and task, the change of screen, and the two handlers
// with no frame of their own (play, and leaving the level). BMX XXX's own
// code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/camera/action_camera.h"
#include "bmx/game/round.h"
#include "bmx/overlay/displays.h"
#include "bmx/overlay/screen_calls.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/platform/tasks.h"
#include "zge/render/primitives.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/overlay/screens.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kLevelStringsName = sym::bmx::g_textLevelStringsName; // "string.level.%s.%s"
        constexpr uint32_t kOverlayScript = sym::g_textOverlayScript;            // "overlay.zsc"
        constexpr uint32_t kOverlayUiName = sym::g_textOverlayUi;                // "UI_overlay"
        // The trick list's script functions (screen_trick_list.cpp).
        constexpr uint32_t kUpdateTrickName = sym::g_textTrickListUpdateTrick;       // "bmxoverlayTrickListUIUpdateTrick"
        constexpr uint32_t kUpdateCategoryName = sym::g_textTrickListUpdateCategory; // "bmxoverlayTrickListUIUpdateCategory"

        // The screens' task.
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_gameScreensTask> g_gameScreensTask;
        constexpr uint32_t kTaskStage = 1;
        constexpr uint32_t kTaskAtEnd = 1;
        constexpr uint32_t kOverlaySourceLine = 0x134;

        // ---- a pad that is out
        // The message over the screen while a player's pad is out, and
        // whether it is up.
        inline ee::Global<uint32_t, sym::g_padOutShown> g_padOutShown;
        constexpr uint32_t kPadOutText = sym::g_textControllerYank;         // "$KSTR_CONTROLER_YANK"
        constexpr uint32_t kPadOutMessage = sym::g_textCtrlYankMessage;     // "UI_overlay.CtrlYank.Message"
        constexpr uint32_t kStopPadOutHide = sym::g_textStopCtrlYankHide;   // "Stop UI_overlay.CtrlYank.Hide"
        constexpr uint32_t kPlayPadOutShow = sym::g_textPlayCtrlYankShow;   // "Play UI_overlay.CtrlYank.Show"
        constexpr uint32_t kStopPadOutShow = sym::g_textStopCtrlYankShow;   // "Stop UI_overlay.CtrlYank.Show"
        constexpr uint32_t kPlayPadOutHide = sym::g_textPlayCtrlYankHide;   // "Play UI_overlay.CtrlYank.Hide"
        // "LOCKINPUT 0" .. "LOCKINPUT 3" and "UNLOCKINPUT 0" .. "UNLOCKINPUT 3".
        constexpr uint32_t kPads = 4;
        constexpr uint32_t kLockInput[kPads] = {sym::g_textPauseLockInput0, sym::g_textPauseLockInput1, sym::g_textPauseLockInput2,
                                                sym::g_textPauseLockInput3};
        constexpr uint32_t kUnlockInput[kPads] = {sym::g_textPauseUnlockInput0, sym::g_textPauseUnlockInput1,
                                                  sym::g_textPauseUnlockInput2, sym::g_textPauseUnlockInput3};

        // A port's pad as the check reads it (the rider's PadInput).
        struct PadState
        {
            uint8_t _00[0x0C];
            uint16_t flags; // +0x0C kPadOut
        };
        // The engine pad's state words, as the screens read them.
        const PadState *padStateOf(const rider::PadInput *pad) { return reinterpret_cast<const PadState *>(pad); }
        // The pad is out of its port (read from this use: the message asks
        // for it back).
        constexpr uint16_t kPadOut = 0x1;
        // Scaffolding (address-call): the pad's own port number, from 0 (the
        // rider's input code; named from the message it fills).
        uint32_t playerPadPort(uint32_t player) { return ee::game<uint32_t>(sym::playerPadPort, player); }

        // Whether a screen puts the message up while a pad is out.
        bool screenAsksForPads(int32_t screen)
        {
            switch (screen)
            {
            case kScreenPause:
            case kScreenPauseNoChoice:
            case kScreenOptions:
            case kScreenBikeParts:
            case kScreenGameOverBikeParts:
            case kScreenTrickList:
            case kScreenRoundOverTrickList:
            case kScreenGameOverTrickList:
            case kScreenNextPlayerTrickList:
            case kScreenInstructions:
            case kScreenPauseObjectives:
            case kScreenPauseObjectiveDetail:
            case kScreenHighScore:
            case kScreenRecords:
            case kScreenNextPlayer:
                return true;
            default:
                return false;
            }
        }

        // ---- the sound's voices, drawn
        // Set by nothing in the game: with it the task draws a bar for each
        // of the sound's voices, coloured by its state.
        inline ee::Global<uint32_t, sym::g_showVoiceStates> g_showVoiceStates;
        constexpr uint32_t kVoices = 40;
        struct Rgba
        {
            uint8_t channel[4];
        };
        // A voice state's colour, by the state's code.
        inline ee::Ptr<Rgba> voiceStateColour(uint32_t code) { return ee::Ptr<Rgba>(sym::g_voiceStateColours) + code; }
        // One quad, its corners after it.
        struct BarQuad
        {
            uint32_t texture;                          // +0x00 none
            ee::Ptr<zge::PrimitiveVertex> vertices[4]; // +0x04
            uint8_t _14[0x20 - 0x14];
            zge::PrimitiveVertex corners[4];           // +0x20
        };
        static_assert(offsetof(BarQuad, corners) == 0x20 && sizeof(BarQuad) == 0xA0);
        constexpr uint32_t kScreenQuads = 0x02000002;
        // The row of bars: 426 units wide from 37, a bar 10 wide, from 18 to 26 down.
        constexpr uint32_t kBarsWidth = 0x1AA, kBarsAcross = 0x27;
        constexpr int32_t kBarsLeft = 0x25, kBarTop = 0x12, kBarBottom = 0x1A;
        constexpr ee::Float kBarWidth = 10.0f;

        // The player whose pad has the menus.
        uint32_t menusPlayer() { return static_cast<uint32_t>(*calls::g_modalPlayerNum); }
        // Scaffolding (address-call): the sound's voice states have no declaration in a header.
        void copyVoiceStateCodes(ee::Address out) { ee::game(sym::copyVoiceStateCodes, out); }

    }

    void openOverlay()
    {
        ee::Local<char[0x40]> name;
        zge::formatString(name, kLevelStringsName, calls::levelInfo(*calls::g_currentLevel)->code,
                          (*calls::g_languageCodes)[calls::g_savedGame->language]);
        *calls::g_levelStrings = loadStringLibrary(name);

        zge::zgui::loadZguiScript(kOverlayScript, *zge::zgui::g_root);
        *g_overlayUi = zge::zgui::findNode(*zge::zgui::g_root, kOverlayUiName, 1);
        openOverlayDisplays();

        *g_gameScreen = 0xFFFFFFFFu;
        showGameScreen(kScreenNone);
        *g_resumeAction = kResumePlay;
        menuFrame();

        zge::zgui::registerScriptFunction(kUpdateTrickName, sym::bmxoverlayTrickListUIUpdateTrick);
        zge::zgui::registerScriptFunction(kUpdateCategoryName, sym::bmxoverlayTrickListUIUpdateCategory);
        *g_gameScreensTask = zge::newTask(kTaskStage, sym::gameScreensFrame, 0u, kTaskAtEnd, sym::g_overlaySourceFile,
                                          kOverlaySourceLine); // "./src/com/action/overlay.c"
    }
    READABLE_FUNCTION(openOverlay, Verified);

    // closeOverlay: a screen of the pause menu's kind and up is told to
    // leave. (With no menu at all the test passes too, and the record before
    // the table's first is called: kept.)
    void closeOverlay()
    {
        if (static_cast<uint32_t>(menuId()) >= kScreenPause)
            gameScreenRecord(menuId())->handler(kScreenLeaveNow, 0);
        zge::deleteTask(*g_gameScreensTask);
        closeOverlayDisplays();
        zge::zgui::destroyNode(*g_overlayUi);
        game::tail(freeGameStrings, *calls::g_levelStrings);
    }
    READABLE_FUNCTION(closeOverlay, Verified);

    // gameScreensInput: the first player found whose rider is shown and whose
    // pad is out is asked
    // for it: every pad's input is locked and the message comes up. Once no
    // pad is out the inputs are unlocked and the message goes.
    uint32_t gameScreensInput()
    {
        ee::Local<char[0x100]> text;
        ee::Local<char[0x100]> message;

        if (screenAsksForPads(menuId()))
        {
            bool padOut = false;
            for (uint32_t player = 0; player < *calls::g_playerCount; ++player)
            {
                const ee::Ptr<rider::Rider> playerRider = rider::riderOf(riderIndexOf(player));
                const PadState *pad = padStateOf(rider::padInput(player));
                if (!rider::hasFlag(playerRider, rider::kFlagShown) || (pad->flags & kPadOut) == 0)
                    continue;

                const uint32_t port = (playerPadPort(player & 0xFF) + 1) & 0xFF;
                zge::expandStringKeys(text, ee::Ptr<const char>(kPadOutText));
                zge::formatString(message, text, port);
                zge::zgui::setVariable(zge::zgui::findNode(*zge::zgui::g_root, kPadOutMessage, 1), kTextName, message);
                for (uint32_t i = 0; i < kPads; ++i)
                    zge::zgui::runScript(*zge::zgui::g_root, ee::Ptr<char>(kLockInput[i]), 0);
                if (*g_padOutShown == 0)
                {
                    *g_padOutShown = 1;
                    zge::zgui::runScript(*zge::zgui::g_root, ee::Ptr<char>(kStopPadOutHide), 0);
                    zge::zgui::runScript(*zge::zgui::g_root, ee::Ptr<char>(kPlayPadOutShow), 0);
                }
                padOut = true;
                break;
            }

            if (*g_padOutShown != 0 && !padOut)
            {
                *g_padOutShown = 0;
                for (uint32_t i = 0; i < kPads; ++i)
                    zge::zgui::runScript(*zge::zgui::g_root, ee::Ptr<char>(kUnlockInput[i]), 0);
                zge::zgui::runScript(*zge::zgui::g_root, ee::Ptr<char>(kStopPadOutShow), 0);
                zge::zgui::runScript(*zge::zgui::g_root, ee::Ptr<char>(kPlayPadOutHide), 0);
            }
        }

        const int32_t screen = menuId();
        if (screen != kNoMenu)
        {
            const ee::Ptr<GameScreenRecord> record = gameScreenRecord(screen);
            if (record->input)
                record->input();
        }
        return 0;
    }
    READABLE_FUNCTION(gameScreensInput, Verified);

    uint32_t gameScreensFrame()
    {
        ee::Local<uint8_t[0x30]> codes; // a code a voice; the last eight bytes are not used
        ee::Local<BarQuad> quad;
        ee::Local<Rgba> colour;

        const int32_t screen = menuId();
        if (screen >= static_cast<int32_t>(kScreenPlay) && screen <= static_cast<int32_t>(kScreenReplayDemo))
        {
            updateOverlayDisplays();
        }
        else if (screen != kNoMenu)
        {
            const ee::Ptr<GameScreenRecord> record = gameScreenRecord(screen);
            if (record->frame)
                record->frame();
        }

        if (*g_showVoiceStates != 0)
        {
            quad->texture = 0;
            for (uint32_t i = 0; i < 4; ++i)
                quad->vertices[i] = &quad->corners[3 - i];
            copyVoiceStateCodes(codes);

            for (uint32_t voice = 0; voice < kVoices; ++voice)
            {
                *colour = *voiceStateColour(codes[voice]);
                // Corners in turn: left top, right top, right bottom, left bottom.
                for (uint32_t i = 0; i < 4; ++i)
                {
                    zge::PrimitiveVertex &corner = quad->corners[i];
                    ee::Float x = ee::toFloat(static_cast<int32_t>(voice * kBarsWidth / kBarsAcross) + kBarsLeft);
                    if (((i + 1) & 2) != 0)
                        x = x + kBarWidth;
                    corner.x = x;
                    corner.y = ee::toFloat((i & 2) != 0 ? kBarBottom : kBarTop);
                    corner.z = ee::Float::fromBits(0);
                    for (uint32_t c = 0; c < 4; ++c)
                        corner.rgba[c] = colour->channel[c];
                }
                zge::drawPrimitiveQuads(kScreenQuads, ee::Ptr<uint32_t>(&quad->texture), 1, 0);
            }
        }
        return 1;
    }
    READABLE_FUNCTION(gameScreensFrame, Verified);

    uint32_t gameScreen()
    {
        const uint32_t screen = *g_gameScreen;
        if (screen == kScreenExitGame || screen == kScreenExitLevel)
            return *g_screenBehindExitConfirm;
        return screen;
    }
    READABLE_FUNCTION(gameScreen, Verified);

    void showGameScreen(uint32_t screen)
    {
        ee::Local<Menu> menu;
        if (screen == kScreenLast)
            screen = *g_lastGameScreen;
        else if (screen >= kScreenPause)
            *g_lastGameScreen = screen;

        if (screen == *g_gameScreen)
            return;
        menu->id = static_cast<int32_t>(screen);
        menu->data = 0;
        menu->handler = gameScreenRecord(static_cast<int32_t>(screen))->handler;
        setMenu(menu);
        *g_gameScreen = screen;
        *g_gameScreenChanged = 1;
    }
    READABLE_FUNCTION(showGameScreen, Verified);

    // noneScreenMessage: the screens with no frame (play, the replays).
    // With no screen at all the coming in is answered at once; otherwise
    // "FrameNone" plays its actions. Once in, play's own start (what the
    // screen before asked for), then what the level shows and the sound's
    // mode.
    void noneScreenMessage(int32_t message)
    {
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, sym::g_textFrameNone, 1); // "FrameNone"
        if (!frame)
            return;

        switch (message)
        {
        case kScreenEnter:
            if (gameScreen() == kScreenNone)
            {
                *calls::g_msgType = kScreenCameIn;
                game::tail(bmxuiHandler);
                return;
            }
            game::tail(playUiNode, frame, kInitAction);
            return;

        case kScreenCameIn:
        {
            const uint32_t screen = gameScreen();
            if (screen == kScreenPlay)
            {
                switch (*g_resumeAction)
                {
                case kResumeRespawn:
                    rider::resetPhysics(rider::riderOf(riderIndexOf(menusPlayer())), 1);
                    rider::riderReset(riderIndexOf(menusPlayer()), 1);
                    // The camera's second mode level: 3, then none.
                    setCameraMode(playerActionCamera(menusPlayer()), 1, 3);
                    setCameraMode(playerActionCamera(menusPlayer()), 1, 0);
                    break;
                case kResumeTurnRunning:
                    setRoundPhase(calls::kTurnRunningPhase);
                    break;
                case kResumeNewGame:
                    startGame(gameMode());
                    setRoundPhase(calls::kTurnStartPhase);
                    break;
                case kResumeTurnStart:
                    setRoundPhase(calls::kTurnStartPhase);
                    break;
                default:
                    break;
                }
                setCameraState(calls::kCameraStatePlay);
            }
            else if (screen == kScreenReplay)
            {
                setCameraState(calls::kCameraStateReplay);
            }
            else if (screen == kScreenReplayDemo)
            {
                setCameraState(calls::kCameraStateReplayDemo);
            }
            game::tail(setGameMode, calls::kSoundModePlay);
            return;
        }

        case kScreenLeaveNow:
            setCameraState(calls::kCameraStateScreen);
            game::tail(playUiNode, frame, kShutdownAction);
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(noneScreenMessage, Verified);

    // leaveScreenMessage: the three screens that leave the level. The
    // whole screen fades out; once it has, the game's state changes.
    void leaveScreenMessage(int32_t message)
    {
        if (message == kScreenEnter)
        {
            game::tail(playOverlayFade, kOverlayWholeScreen, kOverlayFadeOutCB);
            return;
        }
        if (message != kScreenCameIn)
            return;

        const int32_t screen = menuId();
        if (screen == static_cast<int32_t>(kScreenLeaveToMovie))
            game::tail(setGameState, calls::kGameStateMovie);
        else if (screen == static_cast<int32_t>(kScreenLeaveToEditor))
            game::tail(setGameState, calls::kGameStateParkEditor);
        else if (*calls::g_sessionKind == calls::kSessionParkTest)
            game::tail(setGameState, calls::kGameStateParkEditor);
        else
            game::tail(setGameState, calls::kGameStateFrontEnd);
    }
    READABLE_FUNCTION(leaveScreenMessage, Verified);
}
