// DM2's in-game overlay: the trick display (kOverlayTrick,
// "InGameUI.OverlayToggle.DisplayTrick"): four name labels the line's tricks
// go through in turn, and the line's score. DM2's own code (overlay.c).

#include "frames.h"
#include "overlay.h"

#include "dm2/game/round.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "dm2/editor/edit_gui.h"
#include "ee/readable.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/frame_trick.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kTrickDisplay{sym::g_textDisplayTrickPath};       // "InGameUI.OverlayToggle.DisplayTrick"
        constexpr ee::Ptr<const char> kTrickNameLabel{sym::g_textTrickLabelNamePath};   // "...DisplayTrick.LabelName"
        constexpr ee::Ptr<const char> kTrickScoreLabel{sym::g_textTrickLabelScorePath}; // "...DisplayTrick.LabelScore"
        constexpr ee::Ptr<const char> kPathFormat{sym::g_textPathFormat};               // "%s"
        constexpr ee::Ptr<const char> kNumberedPathFormat{sym::g_textNumberedPathFormat}; // "%s%02d"
        constexpr ee::Ptr<const char> kDisplayInitAction{sym::g_textOverlayInit};       // "Init"
        constexpr ee::Ptr<const char> kDisplayDeInitAction{sym::g_textOverlayDeInit};   // "DeInit"

        // The display's widgets, found as the overlay starts.
        constexpr uint32_t kTrickNameLabels = 4;
        struct TrickDisplayWidgets
        {
            ee::Ptr<zge::zgui::Node> names[kTrickNameLabels]; // LabelName00 .. LabelName03
            ee::Ptr<zge::zgui::Node> score;                   // LabelScore
            ee::Ptr<zge::zgui::Node> display;                 // the display itself (0: the layout has none)
        };
        inline ee::Global<TrickDisplayWidgets, sym::g_trickDisplayWidgets> g_trickDisplayWidgets;

        // The name label the current trick is in, and the one the next
        // trick takes: the four labels are used in turn.
        inline ee::Global<uint32_t, sym::g_trickNameLabel> g_trickNameLabel;
        inline ee::Global<uint32_t, sym::g_nextTrickNameLabel> g_nextTrickNameLabel;

        constexpr ee::Ptr<const char> kFlashOnAction{sym::g_textFlashOn};   // "FlashOn"
        constexpr ee::Ptr<const char> kCrashAction{sym::g_textCrash};       // "Crash"
        constexpr ee::Ptr<const char> kPointsFormat{sym::g_textPointsFormat}; // "%ld points"
        // What a line's text starts with from its second trick on: a grey "TO" and a line break.
        constexpr ee::Ptr<const char> kToPrefix{sym::g_textTrickToPrefix};  // "#RGB{192 192 192}TO#RGB{238 181 44}\n"
        // The script that lets a name label fade out with no delay.
        constexpr ee::Ptr<char> kFadeOutAtOnceScript{sym::g_textFadeOutAtOnceScript}; // "SET FadeOut.StartDelay {0.0}"
        constexpr uint32_t kFadeOutAtOnceRuns = 4;
        // The characters of a line of a trick's text before it is broken.
        constexpr uint32_t kTrickLineChars = 18;

        // Whether the game shows a line's points: not HORSE nor the
        // best-turn modes, and not a free ride or a park's test.
        bool gameShowsLinePoints()
        {
            const uint8_t mode = multiplayerMode();
            if (mode == kHorseMode)
                return false;
            if (mode >= kFirstBestTurnMode && mode <= kLastBestTurnMode)
                return false;
            if (mode == 0)
            {
                const uint8_t single = singlePlayerMode();
                if (single == kFreeRide || single == kParkTest)
                    return false;
            }
            return true;
        }
    }

    // As the overlay starts, the display's widgets are found and it plays
    // its "Init"; as the overlay ends it plays its "DeInit".
    void overlayTrickSetUp(uint32_t starting)
    {
        ee::Local<char[128]> path;
        TrickDisplayWidgets &widgets = *g_trickDisplayWidgets;
        zge::formatString(path, kPathFormat, kTrickDisplay);
        widgets.display = uiWidget(path);
        if (!widgets.display)
            return;
        if (starting == 0)
        {
            playWidgetAction(widgets.display, kDisplayDeInitAction);
            return;
        }
        for (uint32_t i = 0; i < kTrickNameLabels; ++i)
        {
            zge::formatString(path, kNumberedPathFormat, kTrickNameLabel, i);
            widgets.names[i] = uiWidget(path);
        }
        zge::formatString(path, kPathFormat, kTrickScoreLabel);
        widgets.score = uiWidget(path);
        playWidgetAction(widgets.display, kDisplayInitAction);
    }
    READABLE_FUNCTION(overlayTrickSetUp, Verified);

    void overlayTrickShow(uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        visible = shown;
        const ee::Ptr<zge::zgui::Node> display = g_trickDisplayWidgets->display;
        if (display)
            zge::zgui::setVariable(display, kVisibleName, visible);
    }
    READABLE_FUNCTION(overlayTrickShow, Verified);

    // What the game tells the display as the turn's current trick line goes
    // on (a TrickDisplayAction and the rider's score block). Nothing while
    // the layout has no trick display, and, but for the clear, the score's
    // fade-out and the crash, nothing while the turn has no line.
    void overlayTrickMessage(ee::Ptr<const OverlayTrick> message)
    {
        ee::Local<char[512]> text;
        ee::Local<char[512]> lines;
        TrickDisplayWidgets &widgets = *g_trickDisplayWidgets;
        if (!widgets.display)
            return;
        const ee::Ptr<rider::ScoreBlock> turn{message->turn};
        const ee::Ptr<zge::zgui::Node> score = widgets.score;
        switch (message->action)
        {
        case kTrickDisplayClear:
            playWidgetAction(widgets.display, kDisplayInitAction);
            *g_nextTrickNameLabel = 1;
            *g_trickNameLabel = 0;
            for (uint32_t i = 0; i < kTrickNameLabels; ++i)
                zge::setSceneObjectShown(widgetObject(widgets.names[i]), 0);
            break;

        case kTrickDisplayTrickBegun:
        {
            if (turn->lineCount == 0)
                break;
            const ee::Ptr<zge::zgui::Node> next = widgets.names[*g_nextTrickNameLabel];
            if (!next)
                break;
            for (uint32_t run = 0; run < kFadeOutAtOnceRuns; ++run)
                zge::zgui::runScriptLines(widgets.names[*g_trickNameLabel], kFadeOutAtOnceScript);
            playWidgetAction(next, kFadeInAction);
            *g_trickNameLabel = *g_nextTrickNameLabel;
            *g_nextTrickNameLabel = (*g_trickNameLabel + 1) % kTrickNameLabels;
            if (turn->lineCount == 1)
            {
                stopWidgetAction(score, kFadeOutAction);
                playWidgetAction(score, kFadeInAction);
            }
            else
            {
                playWidgetAction(score, kFlashOnAction);
            }
            break;
        }

        case kTrickDisplayTrickText:
        {
            if (turn->lineCount == 0)
                break;
            const ee::Ptr<zge::zgui::Node> name = widgets.names[*g_trickNameLabel];
            if (!name)
                break;
            text[0] = 0;
            dm2TrickText(text, &turn->lines[turn->lineCount - 1], 1);
            breakTextLines(lines, text, kTrickLineChars);
            if (turn->lineCount >= 2)
            {
                // (The label's number goes along; the prefix takes nothing.)
                zge::formatString(text, kToPrefix, *g_trickNameLabel);
                zge::appendString(text, lines);
                zge::zgui::setVariable(name, kTextName, text);
            }
            else
            {
                zge::zgui::setVariable(name, kTextName, lines);
            }
            text[0] = 0;
            if (gameShowsLinePoints())
                zge::formatString(text, kPointsFormat, turn->linePoints);
            zge::zgui::setVariable(score, kTextName, text);
            break;
        }

        case kTrickDisplayNameFadeOut:
        {
            if (turn->lineCount == 0)
                break;
            const ee::Ptr<zge::zgui::Node> name = widgets.names[*g_trickNameLabel];
            if (name)
                playWidgetAction(name, kFadeOutAction);
            break;
        }

        case kTrickDisplayScoreFadeOut:
            playWidgetAction(score, kFadeOutAction);
            break;

        case kTrickDisplayCrash:
        {
            const ee::Ptr<zge::zgui::Node> name = widgets.names[*g_trickNameLabel];
            if (name)
                playWidgetAction(name, kCrashAction);
            playWidgetAction(score, kCrashAction);
            break;
        }

        default:
            break;
        }
    }
    READABLE_FUNCTION(overlayTrickMessage, Verified);

    // The display's handler for a value sent while it is shown: nothing.
    void overlayTrickShownMessage() {}
    READABLE_FUNCTION(overlayTrickShownMessage, Verified);
}
