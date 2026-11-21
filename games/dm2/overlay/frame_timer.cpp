// DM2's in-game overlay: the turn's clock (kOverlayTimer,
// "InGameUI.Overlay.FrameTimer"). The game sends the frames left; the frame
// writes them as minutes and seconds with a blinking colon, red for the
// last five seconds. DM2's own code (overlay.c).

#include "frames.h"

#include "dm2/game/round.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/frame_timer.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kTimerFrame{sym::g_textFrameTimerPath};      // "InGameUI.Overlay.FrameTimer"
        constexpr ee::Ptr<const char> kBlinkerLabel{sym::g_textLabelBlinker};      // "LabelBlinker"
        constexpr ee::Ptr<const char> kMinutesLabel{sym::g_textLabelMin};          // "LabelMin"
        constexpr ee::Ptr<const char> kSecondsLabel{sym::g_textLabelSec};          // "LabelSec"
        constexpr ee::Ptr<const char> kColorName{sym::g_textColor};                // "Color"
        constexpr ee::Ptr<const char> kColon{sym::g_textColon};                    // ":"
        constexpr ee::Ptr<const char> kNoColon{sym::g_textSpace};                  // " "
        constexpr ee::Ptr<const char> kClockFormat{sym::g_textClockFormat};        // "%1ld%c%02ld"
        constexpr ee::Ptr<const char> kMinutesFormat{sym::g_textMinutesFormat};    // "%1ld"
        constexpr ee::Ptr<const char> kSecondsFormat{sym::g_textSecondsFormat};    // "%02ld"
        constexpr ee::Ptr<const char> kLogoImage{sym::g_textImageLogo};            // "ImageLogo"
        constexpr ee::Ptr<const char> kMaterialName{sym::g_textMaterial};          // "Material"

        // The clock's colours (red, green, blue, alpha bytes): red for the
        // last seconds, white before.
        inline ee::Global<uint32_t, sym::g_timerLastSecondsColour> g_timerLastSecondsColour;
        inline ee::Global<uint32_t, sym::g_timerColour> g_timerColour;
        // The clock shown: the frames last written (kNoClock: none yet, the
        // frame hidden) and their text.
        inline ee::Global<uint32_t, sym::g_shownClock> g_shownClock;
        constexpr uint32_t kNoClock = 0xFFFFFFFF;

        constexpr uint32_t kFramesPerSecond = 60;
        constexpr uint32_t kFramesPerMinute = 60 * kFramesPerSecond;
        constexpr uint32_t kLastSecondsFrames = 5 * kFramesPerSecond + 1;

        // The materials of the timer's logo, and the one its script
        // function sets next (always the first: nothing sets another).
        constexpr uint32_t kLogoMaterials = 1;
        inline ee::Global<uint8_t, sym::g_timerLogoMaterial> g_timerLogoMaterial;

        // A free ride and a park test have no clock.
        bool untimedSinglePlayerMode(uint32_t mode)
        {
            return mode >= kFreeRide && mode <= kParkTest;
        }
    }

    // The clock shown or hidden: it stays hidden in an untimed game and
    // until a time has been written.
    void overlayTimerShow(uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        visible = shown;
        if (multiplayerMode() == 0 && untimedSinglePlayerMode(singlePlayerMode()))
            visible = 0;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kTimerFrame);
        if (*g_shownClock == kNoClock)
            visible = 0;
        zge::zgui::setVariable(frame, kVisibleName, visible);
    }
    READABLE_FUNCTION(overlayTimerShow, Verified);

    // The frames left sent: the colon shows for the second half of each
    // second, and when the time's text changed its labels are written again
    // in the colour of the time left. An untimed game hides the clock
    // instead. A started second counts whole (59 frames are added), and the
    // seconds run from 1 to 60 within a minute.
    void overlayTimerMessage(ee::Ptr<const uint32_t> framesLeft)
    {
        ee::Local<char[16]> text;
        ee::Local<char[16]> minutesText;
        ee::Local<char[16]> secondsText;
        ee::Local<uint32_t> lastSecondsColour;
        ee::Local<uint32_t> colour;
        ee::Local<uint32_t> visible;
        lastSecondsColour = *g_timerLastSecondsColour;
        colour = *g_timerColour;

        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kTimerFrame);
        if (multiplayerMode() == 0 && untimedSinglePlayerMode(singlePlayerMode()))
        {
            visible = 0;
            zge::zgui::setVariable(frame, kVisibleName, visible);
            return;
        }

        uint32_t frames = *framesLeft + (kFramesPerSecond - 1);
        uint32_t minutes = 0;
        uint32_t seconds = 0;
        while (frames > kFramesPerMinute)
        {
            frames -= kFramesPerMinute;
            ++minutes;
        }
        while (frames > kFramesPerSecond)
        {
            frames -= kFramesPerSecond;
            ++seconds;
        }
        const ee::Ptr<const char> blinker = frames < kFramesPerSecond / 2 ? kNoColon : kColon;
        zge::zgui::setVariable(zge::zgui::findNode(frame, kBlinkerLabel, 0), kTextName, blinker);

        zge::formatString(text, kClockFormat, minutes, ' ', seconds);
        zge::formatString(minutesText, kMinutesFormat, minutes);
        zge::formatString(secondsText, kSecondsFormat, seconds);
        if (zge::compareStrings(text, sym::g_shownClockText) == 0)
            return;

        const bool lastSeconds = *framesLeft <= kLastSecondsFrames;
        const ee::Address shownColour = lastSeconds ? ee::Address(lastSecondsColour) : ee::Address(colour);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMinutesLabel, 0), kColorName, shownColour);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kSecondsLabel, 0), kColorName, shownColour);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kBlinkerLabel, 0), kColorName, shownColour);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kMinutesLabel, 0), kTextName, minutesText);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kSecondsLabel, 0), kTextName, secondsText);
        zge::stringCopy(sym::g_shownClockText, text);
        *g_shownClock = *framesLeft;
    }
    READABLE_FUNCTION(overlayTimerMessage, Verified);

    // The script function "bmxoverlayDisplayTimerUpdate": the timer's logo
    // takes its material and fades in.
    void bmxoverlayDisplayTimerUpdate()
    {
        ee::Local<ee::Ptr<const char>[kLogoMaterials]> materials;
        materials[0] = ee::Ptr<const char>(sym::g_textCommonLogo); // "Common.Logo"
        const ee::Ptr<zge::zgui::Node> logo = zge::zgui::findNode(uiWidget(kTimerFrame), kLogoImage, 0);
        zge::zgui::setVariable(logo, kMaterialName, materials[*g_timerLogoMaterial]);
        playWidgetAction(logo, kFadeInAction);
        *g_timerLogoMaterial = 0;
    }
    READABLE_FUNCTION(bmxoverlayDisplayTimerUpdate, Draft);
}
