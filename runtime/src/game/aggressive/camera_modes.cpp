// The two debug camera modes: the free camera (a dev build's fly-around) and
// the motion view, which plays the skater's motions with the same camera on
// the pad. The pad's d-pad and face buttons move the eye and the look-at
// together, the shoulders orbit; a pad with analog buttons scales by the
// pressure, else half speed.

#include "aggressive/camera.h"
#include "aggressive/pads.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/skater.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget_parts.h"

#include "ee/readable.h"
#include "aggressive/level/edit_records.h"
#include "ee/plain.h"
#include "zge/input/pads.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/text/cstring.h"
#include "zge/sound/sound.h"
#include "zge/render/display.h"
#include "zge/debug/debug.h"
#include "aggressive/sound/sound_mode.h"
#include "aggressive/sound/events.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/camera_modes.cpp.h"

namespace aggressive
{
    namespace
    {
        using skater::Player;
        using zge::Vec4;
        constexpr uint32_t kRunScript = sym::runScript;      // (widget, text, 0), by address where its result is ours

        constexpr uint32_t kHideOverlay = sym::g_textSetInGameUIOverlayVisible;            // set InGameUI.Overlay.visible {0}
        constexpr uint32_t kHideMultiplayerOverlay = sym::g_textSetMultiplayerUIOverlayVisible; // set MultiplayerUI.Overlay.visible {0}
        constexpr uint32_t kShowOverlay = sym::g_textSetInGameUIOverlayVisible2;            // set InGameUI.Overlay.visible {1}
        constexpr uint32_t kMotionViewState = sym::g_textMotionViewState;        // "Motion View State"
        constexpr uint32_t kMotionFormat = sym::g_textMotion;           // "Motion : %s"
        constexpr float kFrameScale = 182.04445f;                 // degrees to a 16-bit angle

        // The debug shapes' colours.
        inline ee::Global<uint32_t, sym::lineGreen> g_lineGreen;
        inline ee::Global<uint32_t, sym::pointWhite> g_pointWhite;
        inline ee::Global<uint32_t, sym::triangleMagenta> g_triangleMagenta;
        inline ee::Global<uint32_t, sym::lineBlue> g_lineBlue;

        // The motion view's list.
        struct MotionViewEntry
        {
            char name[0x20];
            uint32_t motion;
        };
        static_assert(sizeof(MotionViewEntry) == 0x24);
        constexpr uint32_t kMotionViewCount = 4;
        inline ee::Global<MotionViewEntry[kMotionViewCount], sym::g_textKinlinemotionSkateLoop> g_motionViews;

        inline ee::Global<uint64_t, sym::g_debugDisplay> g_debugDisplay;
        inline ee::Global<uint32_t, sym::g_motionViewName> g_motionViewName;
        inline ee::Global<int32_t, sym::g_motionViewIndex> g_motionViewIndex;
        inline ee::Global<int32_t, sym::g_modeBeforeFreeCamera> g_modeBeforeFreeCamera;

        uint32_t angleOf(ee::Float degrees)
        {
            return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(ee::toInt(degrees * ee::Float(kFrameScale)))));
        }

        namespace // the game functions below call: ee/plain.h
        {
            uint32_t padHasPressure(uint32_t level, uint32_t port) { return padDeviceBit(level, port); }
            void orbitCamera(FreeCameraMove how, uint32_t angle) { orbitFreeCamera(static_cast<uint32_t>(how), angle); }
        }

        // The moves of the eye and the look-at point together.
        enum class Move
        {
            Forward,
            Back,
            Left,
            Right,
            Up,
        };

        // The vectors the moves share, in the caller's frame.
        struct CameraPad
        {
            Vec4 &eye;
            Vec4 &lookAt;
            Vec4 &along;
            const Vec4 &up;
            ee::Float &speed;
            ee::Float seconds;

            static bool held(uint32_t button)
            {
                return skater::padHeld(button, 0) != 0;
            }

            // A button's pressure on `scale`, half without one.
            static ee::Float pressure(uint32_t level, ee::Float scale)
            {
                const uint32_t analog = padHasPressure(level, 0);
                if (analog)
                {
                    const ee::Float amount = padAxis(level, 0);
                    return scale * amount;
                }
                return scale * ee::Float(0.5f);
            }

            // While the button is held the eye and the look-at move together.
            void move(uint32_t button, uint32_t level, Move kind, ee::Float scale)
            {
                if (!held(button))
                    return;
                FollowCamera &camera = *g_followCamera;
                eye = camera.eye;
                lookAt = camera.lookAt;
                if (kind != Move::Up)
                {
                    // Level, along the view or across it.
                    if (kind == Move::Forward)
                        along = lookAt - eye;
                    else
                        along = eye - lookAt;
                    along.y = 0.0f;
                    along = zge::unit(along);
                    if (kind == Move::Left || kind == Move::Right)
                        along = zge::cross(along, up);
                    if (kind == Move::Right)
                        zge::vec3Negate(&along.xyz(), &along.xyz());
                }
                const ee::Float amount = pressure(level, scale);
                along = (kind == Move::Up ? up : along) * amount;
                eye = eye + along;
                lookAt = lookAt + along;
                setEye(&camera, &eye);
                setLookAt(&camera, &lookAt);
            }

            // A turn of the camera at `perSecond` while the button is held.
            void orbit(uint32_t button, FreeCameraMove how, ee::Float perSecond)
            {
                if (held(button))
                {
                    speed = seconds * perSecond;
                    orbitCamera(how, angleOf(speed));
                }
            }

            // The same, by the button's pressure.
            void orbitBy(uint32_t button, uint32_t level, FreeCameraMove how, ee::Float perSecond)
            {
                if (held(button))
                {
                    speed = seconds * perSecond;
                    speed = pressure(level, speed);
                    orbitCamera(how, angleOf(speed));
                }
            }

            // Everything both modes do with the pad, in the original's order.
            void update()
            {
                using namespace zge;
                const ee::Float step = speed;
                move(kPadUp, kLevelUp, Move::Forward, step);
                move(kPadDown, kLevelDown, Move::Back, step);
                move(kPadLeft, kLevelLeft, Move::Left, step);
                move(kPadRight, kLevelRight, Move::Right, step);
                move(kPadTriangle, kLevelTriangle, Move::Up, step);
                move(kPadCross, kLevelCross, Move::Up, -step);
                orbit(kPadSquare, FreeCameraMove::Turn, ee::Float(180.0f));
                orbit(kPadCircle, FreeCameraMove::Turn, ee::Float(-180.0f));
                orbitBy(kPadR1, kLevelR1, FreeCameraMove::Tilt, ee::Float(-100.0f));
                orbitBy(kPadL1, kLevelL1, FreeCameraMove::Tilt, ee::Float(100.0f));
                orbitBy(kPadR2, kLevelR2, FreeCameraMove::Orbit, ee::Float(-200.0f));
                orbitBy(kPadL2, kLevelL2, FreeCameraMove::Orbit, ee::Float(200.0f));
            }
        };

        // Straight up in the game's axes (VU0's constant vector with y less one).
        constexpr Vec4 kUp{0.0f, -1.0f, 0.0f, 1.0f};
    }

    // freeCameraModeNoop: a debug hook left empty.
    void freeCameraModeNoop()
    {
    }
    READABLE_FUNCTION(freeCameraModeNoop, Verified);

    // enterFreeCameraMode.
    uint32_t enterFreeCameraMode()
    {
        setFollowCameraMode(&*g_followCamera, static_cast<uint32_t>(CameraMode::Free));
        if (*g_debugDisplay & 3u)
            freeCameraModeNoop();
        *g_modeBeforeFreeCamera = gameModeOf();
        setGameMode(kModePaused);
        if (*g_gameType != GameType::SplitScreen)
            zgui::runScript(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<char>(kHideOverlay), 0);
        else
            zgui::runScript(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<char>(kHideMultiplayerOverlay), 0);
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterFreeCameraMode, Verified);

    namespace
    {
        void cameraFrameDone() { freeCameraFrameDone(); }
    }

    // updateFreeCameraMode: the camera on pad 0; START ends the level, SELECT takes a
    // screenshot; a debug line along the rail the followed skater was last
    // on, and while the first skater is in a wall tap the wall's triangle,
    // the point hit and its normal.
    uint32_t updateFreeCameraMode(ee::Float seconds)
    {
        ee::Local<Vec4> eye;
        ee::Local<Vec4> lookAt;
        ee::Local<Vec4> along;
        ee::Local<Vec4> up;
        ee::Local<uint32_t> lineColour;
        ee::Local<uint32_t> pointColour;
        ee::Local<uint32_t> triangleColour;
        ee::Local<uint32_t> normalColour;
        ee::Local<Vec4> normalEnd;
        ee::Float speed = seconds * ee::Float(100.0f);
        *up = kUp;
        CameraPad pad{*eye, *lookAt, *along, *up, speed, seconds};
        pad.update();

        if (pad.held(zge::kPadStart) && skater::padPressed(zge::kPadStart, 0))
            enterGameMode(kModePlaying);
        if (pad.held(zge::kPadSelect) && skater::padPressed(zge::kPadSelect, 0))
            zge::captureScreenToHost(0);
        const ee::Ptr<Player> followed = g_gameState->cameras[0].player;
        if (followed)
        {
            *lineColour = *g_lineGreen;
            zge::addDebugLine(&followed->railStart, &followed->railEnd, lineColour, 0);
        }
        if (currentLevel()->players[0].state == skater::State::WallTap)
        {
            *pointColour = *g_pointWhite;
            *triangleColour = *g_triangleMagenta;
            *normalColour = *g_lineBlue;
            const skater::WallContact &wall = currentLevel()->players[0].wallContact;
            zge::addDebugPoint(&wall.point, pointColour, 0, 0, ee::Float(0.25f));
            zge::drawTriangle(&wall.corners[0], &wall.corners[1], &wall.corners[2], triangleColour, 0);
            *normalEnd = wall.point + wall.normal;
            zge::addDebugLine(&wall.point, normalEnd, normalColour, 0);
        }
        cameraFrameDone();
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateFreeCameraMode, Verified);

    namespace
    {
        namespace tail
        {
        int32_t runScript(ee::Address widget, uint32_t text, uint32_t argument) { return ee::tailCall<int32_t>(kRunScript, widget, text, argument); }
        }
    }

    // exitFreeCameraMode: the skater goes where the camera looked.
    int32_t exitFreeCameraMode()
    {
        setFollowCameraMode(&*g_followCamera, static_cast<uint32_t>(CameraMode::Follow));
        Player &skater = currentLevel()->players[0];
        skater.position = g_followCamera->lookAt;
        skater::nodeToPosition(&skater);
        if (*g_debugDisplay & 3u)
            freeCameraModeNoop();
        setGameMode(*g_modeBeforeFreeCamera);
        return tail::runScript(*g_uiRoot, kShowOverlay, 0);
    }
    READABLE_FUNCTION_WAITS(exitFreeCameraMode, Verified);

    // enterMotionViewMode: the first motion of the list. playSkaterAnimation
    // reads only the skater, the motion and the blend; the original hands it
    // the running top state as its third word and, as its fourth and fifth,
    // the words this mode's caller (the play mode table's enter) left in
    // those places. The replay holds the call to all of them, so they are
    // handed on as they came.
    uint32_t enterMotionViewMode(uint32_t, uint32_t, uint32_t, uint32_t handedOnFourth, uint32_t handedOnFifth)
    {
        const ee::Ptr<TopState> topState = g_gameState->topState;
        const uint32_t motion = *g_motionViewName;
        *g_motionViewIndex = 0;
        skater::playSkaterAnimation(&topState->level->players[0], motion, ee::Float(0.0f));
        return 1;
    }
    READABLE_FUNCTION_WAITS(enterMotionViewMode, Verified);

    // updateMotionViewMode: the camera on pad 0; SELECT the next motion, START out.
    uint32_t updateMotionViewMode(ee::Float seconds)
    {
        ee::Local<Vec4> eye;
        ee::Local<Vec4> lookAt;
        ee::Local<Vec4> along;
        ee::Local<Vec4> up;
        ee::Local<char[0x40]> name; // "Motion : " and the motion's name
        const ee::Ptr<Level> level = currentLevel();
        ee::Float speed = seconds * ee::Float(100.0f);
        *up = kUp;
        zge::debugPrint(0, 0, kMotionViewState);
        zge::formatString(name, kMotionFormat, &(*g_motionViews)[*g_motionViewIndex]);
        zge::debugPrint(0, 15, name);

        CameraPad pad{*eye, *lookAt, *along, *up, speed, seconds};
        pad.update();

        if (pad.held(zge::kPadSelect) && skater::padPressed(zge::kPadSelect, 0))
        {
            int32_t index = *g_motionViewIndex + 1;
            *g_motionViewIndex = index;
            if (index >= static_cast<int32_t>(kMotionViewCount))
            {
                *g_motionViewIndex = 0;
                index = 0;
            }
            skater::playSkaterAnimation(&level->players[0], (*g_motionViews)[index].motion, ee::Float(0.0f));
        }
        if (pad.held(zge::kPadStart) && skater::padPressed(zge::kPadStart, 0))
        {
            Level &now = *currentLevel();
            now.flags = static_cast<uint16_t>(now.flags & ~kLevelStopReasonBits);
            enterGameMode(kModePaused);
        }
        for ([[maybe_unused]] Player &player : level->players)
            skater::stepAnimators(seconds);
        return 1;
    }
    READABLE_FUNCTION_WAITS(updateMotionViewMode, Verified);

    // exitMotionViewMode.
    void exitMotionViewMode()
    {
    }
    READABLE_FUNCTION(exitMotionViewMode, Verified);

    ee::Ptr<zge::editor::EditCamera> toolSetCamera(ee::Ptr<zge::editor::EditToolSet> toolSet)
    {
        return toolSet->editCamera;
    }
    READABLE_FUNCTION(toolSetCamera, Draft);
}
