// DM2's options screen (screens.h; "InGameUI.FrameOptions"), asked for from
// the pause menu: the current player's camera view, vibration and tips, and
// the volumes. DM2's own code (overlay.c).

#include "screens.h"
#include "overlay.h"

#include "dm2/camera/camera.h"
#include "dm2/game/players.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/input/pads.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_options.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<char> kOptionsInit{sym::optionsInitScript};     // "PLAY Root.InGameUI.FrameOptions.Init"
        constexpr ee::Ptr<char> kOptionsDeInit{sym::optionsDeInitScript}; // "PLAY Root.InGameUI.FrameOptions.DeInit"
        constexpr ee::Ptr<const char> kOptionsMenu{sym::optionsMenuPath}; // "InGameUI.FrameOptions.Menu"
        // The menu's lines, each a choice of values ("...Menu.CameraType",
        // "...VibrationType", "...TutorialType"), with its variables and the
        // script that shows a new value.
        constexpr ee::Ptr<const char> kCameraLine{sym::cameraTypePath};
        constexpr ee::Ptr<const char> kVibrationLine{sym::vibrationTypePath};
        constexpr ee::Ptr<const char> kTipsLine{sym::tutorialTypePath};
        constexpr ee::Ptr<const char> kMaxValueName{sym::screenMaxValueName}; // "MaxValue"
        constexpr ee::Ptr<const char> kCurValueName{sym::screenCurValueName}; // "CurValue"
        constexpr ee::Ptr<char> kUpdateScript{sym::screenUpdateScript};       // "PLAY Update"
        // The root's volume variables ("CurrentSFXVolume", "CurrentMusicVolume").
        constexpr ee::Ptr<const char> kSfxVolumeName{sym::screenSfxVolumeName};
        constexpr ee::Ptr<const char> kMusicVolumeName{sym::screenMusicVolumeName};
        inline ee::Global<uint8_t, sym::g_sfxVolumeSetting> g_sfxVolumeSetting;
        inline ee::Global<uint8_t, sym::g_musicVolumeSetting> g_musicVolumeSetting;
        // The camera line's last value (its values are the camera's views 0 .. 2).
        constexpr ee::Float kCameraViewsMax = 2.0f;

        // The menu's lines and what backing out gives (-1).
        constexpr int32_t kLineCancelled = -1, kLineCamera = 0, kLineVibration = 1, kLineTips = 2;
        // The frame's own OptionsSelect (initOverlay defines it): -1 and 0
        // close the screen.
        inline ee::Global<int32_t, sym::g_optionsSelect> g_optionsSelect;

        ee::Ptr<PlayerSetup> currentPlayerSetup()
        {
            return ee::Ptr<PlayerSetup>(sym::g_playerSetups) + static_cast<ptrdiff_t>(*g_currentPlayer);
        }

        // A line's value read back from the menu, shown anew.
        void readLine(ee::Ptr<const char> line, ee::Local<ee::Float> &value)
        {
            zge::zgui::copyVariable(uiWidget(line), kCurValueName, value);
            zge::zgui::runScriptLines(uiWidget(line), kUpdateScript);
        }
    }

    // As the screen opens: the camera line's range, the vibration line set
    // from the current player's set-up, the volumes from the saved settings,
    // then the frame's Init. As it closes, its DeInit.
    void showOptionsScreen(uint32_t shown)
    {
        ee::Local<ee::Float> value;
        ee::Local<uint32_t> volume;
        if (shown == 0)
        {
            runUiScript(kOptionsDeInit);
            return;
        }
        const ee::Ptr<PlayerSetup> setup = currentPlayerSetup();
        *value = kCameraViewsMax;
        zge::zgui::setVariable(uiWidget(kCameraLine), kMaxValueName, value);
        zge::zgui::runScriptLines(uiWidget(kCameraLine), kUpdateScript);
        *value = ee::toFloat(static_cast<int32_t>(setup->flags & kPlayerVibration));
        zge::zgui::setVariable(uiWidget(kVibrationLine), kCurValueName, value);
        zge::zgui::runScriptLines(uiWidget(kVibrationLine), kUpdateScript);
        *volume = *g_sfxVolumeSetting;
        zge::zgui::setVariable(*g_uiRoot, kSfxVolumeName, volume);
        *volume = *g_musicVolumeSetting;
        zge::zgui::setVariable(*g_uiRoot, kMusicVolumeName, volume);
        runUiScript(kOptionsInit);
    }
    READABLE_FUNCTION(showOptionsScreen, Verified);

    // The menu's script function: Start, or backing out, goes back to the
    // pause screen; a line changed takes its new value: the camera's view
    // (the camera told at once), the vibration, the tips (a tip shown is
    // cleared).
    void bmxoverlayOptionsHandler()
    {
        // The tip frame's message: only its first byte is written (the
        // frame copies the three after it as they are, as the original's).
        ee::Local<uint8_t> noTip;
        ee::Local<int32_t> line;
        ee::Local<ee::Float> value;
        if (rider::padButtons(*g_currentPlayer, zge::kPadStart, rider::kAnyPressed))
        {
            showGameScreen(kScreenPause);
            return;
        }
        if (*g_optionsSelect == -1)
            showGameScreen(kScreenPause);
        if (*g_optionsSelect == 0)
            showGameScreen(kScreenPause);
        zge::zgui::copyVariable(uiWidget(kOptionsMenu), kSelectedIndexName, line);
        const ee::Ptr<PlayerSetup> setup = currentPlayerSetup();
        switch (*line)
        {
        case kLineCancelled:
            showGameScreen(kScreenPause);
            break;
        case kLineCamera:
        {
            readLine(kCameraLine, value);
            const uint8_t view = static_cast<uint8_t>(ee::toInt(*value));
            setup->cameraView = view;
            setCameraView(*g_currentPlayer, view);
            break;
        }
        case kLineVibration:
            readLine(kVibrationLine, value);
            if (*value == 0.0f)
                setup->flags &= static_cast<uint8_t>(~kPlayerVibration);
            else
                setup->flags |= kPlayerVibration;
            break;
        case kLineTips:
            readLine(kTipsLine, value);
            if (*value == 0.0f)
                setup->flags &= static_cast<uint8_t>(~kPlayerTips);
            else
                setup->flags |= kPlayerTips;
            *noTip = kOverlayNone;
            overlayMessage(kOverlayTip, noTip);
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(bmxoverlayOptionsHandler, Verified);
}
