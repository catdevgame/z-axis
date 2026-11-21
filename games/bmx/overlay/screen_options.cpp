// BMX XXX's options screen (screens.h; "FrameOptions"): the player's
// camera type and vibration, the two volumes, and the music's track. BMX
// XXX's own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/camera/action_camera.h"
#include "bmx/overlay/screen_calls.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_options.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kFrameOptions = sym::g_textFrameOptions;            // "FrameOptions"
        constexpr uint32_t kCameraTypeWidget = sym::g_textMenuCameraType;      // "Menu.CameraType"
        constexpr uint32_t kCameraTypeCaption = sym::g_textMenuCameraTypeCaption; // "Menu.CameraType.Caption"
        constexpr uint32_t kVibrationWidget = sym::g_textMenuVibrationType;    // "Menu.VibrationType"
        constexpr uint32_t kVibrationCaption = sym::g_textMenuVibrationTypeCaption; // "Menu.VibrationType.Caption"
        constexpr uint32_t kSoundVolumeWidget = sym::g_textMenuVolumeSFX;      // "Menu.VolumeSFX"
        constexpr uint32_t kMusicVolumeWidget = sym::g_textMenuVolumeMusic;    // "Menu.VolumeMusic"
        constexpr uint32_t kMusicTrackWidget = sym::g_textMenuMusicTrack;      // "Menu.MusicTrack"
        constexpr uint32_t kCaptionWidget = sym::g_textCaption;                // "Caption"
        constexpr uint32_t kBarWidget = sym::g_textBar;                        // "Bar"
        constexpr uint32_t kCurValueName = sym::g_textCurValue;                // "CurValue"
        constexpr uint32_t kMaxValueName = sym::g_textMaxValue;                // "MaxValue"
        constexpr uint32_t kWidthName = sym::g_textWidth;                      // "Width"
        constexpr uint32_t kCameraTypeText = sym::bmx::g_textCameraTypeKey;    // "$KSTR_CAMERA_TYPE_%d"
        constexpr uint32_t kOnOffFormat = sym::bmx::g_textStringFormat;        // "%s"
        constexpr uint32_t kOnText = sym::g_textOn;                            // "$KSTR_ON"
        constexpr uint32_t kOffText = sym::g_textOff;                          // "$KSTR_OFF"
        // "$KSTR_SONGINFO_%02d_SONGNAME\n$KSTR_SONGINFO_%02d_ARTIST"
        constexpr uint32_t kSongInfoText = sym::bmx::g_textSongInfoKeys;

        // The menu's lines.
        enum OptionsLine : uint32_t
        {
            kOptionCameraType = 0,
            kOptionVibration = 1,
            kOptionSoundVolume = 2,
            kOptionMusicVolume = 3,
            kOptionMusicTrack = 4,
        };
        // The frame's own messages: a line's value was changed, and the
        // lines are to be written again (the track's line asks for it too).
        constexpr int32_t kOptionChanged = 2;
        constexpr int32_t kOptionsRefresh = 30;

        constexpr ee::Float kLastCameraType = 3.0f;
        // A volume's bar is one and a half units wide a step.
        constexpr ee::Float kBarWidthAStep = 1.5f;
        // The sound events the options send (named from this use): a
        // volume set (which, 0 .. 127), the music's track asked for and
        // set, and the number of tracks asked for.
        constexpr uint32_t kSetVolumeEvent = 0, kTrackEvent = 2, kSetTrackEvent = 3, kTrackCountEvent = 10;
        constexpr uint32_t kSoundVolume = 0, kMusicVolume = 1;
        constexpr uint32_t kFullVolume = 0x7F;

        // An options volume (0 .. 100 and over) as the sound takes it.
        uint32_t soundLevel(uint8_t volume)
        {
            const uint32_t level = (volume * kFullVolume) >> 7;
            return level < 0x80 ? level : kFullVolume;
        }
    }

    void optionsScreenMessage(int32_t message, uint32_t data)
    {
        ee::Local<char[0x100]> text;
        ee::Local<uint32_t> selected;
        ee::Local<ee::Float> value;
        ee::Local<ee::Float> track;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameOptions, 1);
        if (!frame)
            return;

        switch (message)
        {
        case kScreenEnter:
            optionsScreenMessage(kOptionsRefresh, data);
            playUiNode(frame, kInitAction);
            return;

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            return;

        case kScreenCancelled:
            showGameScreen(kScreenPause);
            return;

        case kOptionChanged:
        {
            const ee::Ptr<calls::PlayerSetup> setup = calls::playerSetup(static_cast<uint32_t>(*calls::g_modalPlayerNum));
            zge::zgui::copyVariable(zge::zgui::findNode(frame, kMenuName, 1), kSelectedIndexName, selected);
            switch (selected)
            {
            case kOptionCameraType:
                zge::zgui::copyVariable(zge::zgui::findNode(frame, kCameraTypeWidget, 1), kCurValueName, value);
                setup->cameraType = static_cast<uint8_t>(ee::toInt(value));
                setCameraType(playerActionCamera(static_cast<uint32_t>(*calls::g_modalPlayerNum)), setup->cameraType);
                break;
            case kOptionVibration:
                zge::zgui::copyVariable(zge::zgui::findNode(frame, kVibrationWidget, 1), kCurValueName, value);
                setup->flags = static_cast<uint8_t>((setup->flags & ~calls::kSetupVibration) |
                                                    (*value == ee::Float(0.0f) ? 0 : calls::kSetupVibration));
                break;
            case kOptionSoundVolume:
                zge::zgui::copyVariable(zge::zgui::findNode(frame, kSoundVolumeWidget, 1), kCurValueName, value);
                calls::g_savedGame->soundVolume = static_cast<uint8_t>(ee::toInt(value));
                soundEvent(kSetVolumeEvent, kSoundVolume, soundLevel(calls::g_savedGame->soundVolume), 0, 0);
                break;
            case kOptionMusicVolume:
                zge::zgui::copyVariable(zge::zgui::findNode(frame, kMusicVolumeWidget, 1), kCurValueName, value);
                calls::g_savedGame->musicVolume = static_cast<uint8_t>(ee::toInt(value));
                soundEvent(kSetVolumeEvent, kMusicVolume, soundLevel(calls::g_savedGame->musicVolume), 0, 0);
                break;
            case kOptionMusicTrack:
                zge::zgui::copyVariable(zge::zgui::findNode(frame, kMusicTrackWidget, 1), kCurValueName, track);
                soundEvent(kSetTrackEvent, sce::libgcc::fptoui(track), 0, 0, 0);
                break;
            default:
                break;
            }
            optionsScreenMessage(kOptionsRefresh, data);
            return;
        }

        case kOptionsRefresh:
        {
            value = kLastCameraType;
            zge::zgui::setVariable(zge::zgui::findNode(frame, kCameraTypeWidget, 1), kMaxValueName, value);
            const ee::Ptr<calls::PlayerSetup> setup = calls::playerSetup(static_cast<uint32_t>(*calls::g_modalPlayerNum));
            value = ee::toFloat(setup->cameraType);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kCameraTypeWidget, 1), kCurValueName, value);
            zge::formatString(text, kCameraTypeText, ee::toInt(value));
            zge::zgui::setVariable(zge::zgui::findNode(frame, kCameraTypeCaption, 1), kTextName, text);

            value = ee::toFloat(setup->flags & calls::kSetupVibration);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kVibrationWidget, 1), kCurValueName, value);
            zge::formatString(text, kOnOffFormat, *value == ee::Float(0.0f) ? kOffText : kOnText);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kVibrationCaption, 1), kTextName, text);

            const ee::Ptr<Node> soundVolume = zge::zgui::findNode(frame, kSoundVolumeWidget, 1);
            value = ee::toFloat(calls::g_savedGame->soundVolume);
            zge::zgui::setVariable(soundVolume, kCurValueName, value);
            value = *value * kBarWidthAStep;
            zge::zgui::setVariable(zge::zgui::findNode(soundVolume, kBarWidget, 1), kWidthName, value);

            const ee::Ptr<Node> musicVolume = zge::zgui::findNode(frame, kMusicVolumeWidget, 1);
            value = ee::toFloat(calls::g_savedGame->musicVolume);
            zge::zgui::setVariable(musicVolume, kCurValueName, value);
            value = *value * kBarWidthAStep;
            zge::zgui::setVariable(zge::zgui::findNode(musicVolume, kBarWidget, 1), kWidthName, value);

            track = ee::toFloat(static_cast<int32_t>(soundEvent(kTrackEvent, 0, 0, 0, 0)));
            const ee::Ptr<Node> musicTrack = zge::zgui::findNode(frame, kMusicTrackWidget, 1);
            zge::zgui::setVariable(musicTrack, kCurValueName, track);
            const uint32_t tracks =
                sce::libgcc::fptoui(ee::toFloat(static_cast<int32_t>(soundEvent(kTrackCountEvent, 0, 0, 0, 0))));
            value = ee::toFloat(static_cast<int32_t>(tracks - 1));
            zge::zgui::setVariable(musicTrack, kMaxValueName, value);
            zge::formatString(text, kSongInfoText, ee::toInt(track), ee::toInt(track));
            zge::zgui::setVariable(zge::zgui::findNode(musicTrack, kCaptionWidget, 1), kTextName, text);
            return;
        }

        default:
            return;
        }
    }
    READABLE_FUNCTION(optionsScreenMessage, Verified);
}
