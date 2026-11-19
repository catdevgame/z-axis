// DM2's in-game overlay: the labels the game sends one value to: the bail's
// text (kOverlayFall), a tip (kOverlayTip), a level goal (kOverlayObjective),
// the exit warning (kOverlayExitWarning) and a demo's song
// (kOverlaySongInfo). Each keeps what it was sent (kOverlayNone: nothing),
// writes its widget's text from it and plays the widget's fade. DM2's own
// code (overlay.c).

#include "frames.h"

#include "dm2/game/players.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "dm2/editor/edit_gui.h"
#include "ee/readable.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/frame_labels.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kFallLabel{sym::g_textLabelCrashPath};         // "InGameUI.Overlay.LabelCrash"
        constexpr ee::Ptr<const char> kFallFormat{sym::g_textFallFormat};            // "#RGB{200 32 32}$KSTR_FALL_%02d"
        constexpr ee::Ptr<const char> kTipLabel{sym::g_textLabelTipPath};            // "InGameUI.Overlay.LabelTip"
        constexpr ee::Ptr<const char> kTipFormat{sym::g_textHintFormat};             // "$KSTR_HINT%d"
        constexpr ee::Ptr<const char> kObjectiveLabel{sym::g_textLabelObjPath};      // "InGameUI.Overlay.LabelObj"
        constexpr ee::Ptr<const char> kGoalFormat{sym::g_textGoalFormat};            // "$KSTRSP_GOAL_LEV%02d_%s_%d"
        constexpr ee::Ptr<const char> kRookieGoalFormat{sym::g_textRookieGoalFormat}; // "$KSTRSP_GOAL_LEV%02d_ROOKIE"
        constexpr ee::Ptr<const char> kExitWarningLabel{sym::g_textLabelExitWarnPath}; // "InGameUI.Overlay.LabelExitWarn"
        constexpr ee::Ptr<const char> kSongInfoLabel{sym::g_textLabelSongInfoPath};  // "InGameUI.OverlayReplay.Demo.LabelSongInfo"
        // "$KSTR_SONGINFO_%d_ARTIST", "_SONGNAME" and two more lines, each with the song's number.
        constexpr ee::Ptr<const char> kSongInfoFormat{sym::g_textSongInfoFormat};
        constexpr ee::Ptr<const char> kWaitAction{sym::g_textWait};                  // "Wait"

        constexpr uint8_t kLabelValueBits = 0x7F;
        // A widget's scene object is shown (the bit setSceneObjectShown sets).
        constexpr uint64_t kWidgetShown = zge::kObjectMoved;

        // The bail's kind shown, and the byte last sent.
        inline ee::Global<uint8_t, sym::g_fallKind> g_fallKind;
        inline ee::Global<LabelState, sym::g_tipState> g_tipState;
        inline ee::Global<LabelState, sym::g_objectiveState> g_objectiveState;
        inline ee::Global<uint8_t, sym::g_exitWarning> g_exitWarning;
        // The level being played (its number is in the goals' texts).
        inline ee::Global<uint8_t, sym::g_sessionLevel> g_sessionLevel;
        // What the song's label takes and keeps: the song shown (kOverlayNone: none).
        struct SongInfoMessage
        {
            uint8_t song;
            uint8_t pad01[3]; // padding: no reader (pad2: the senders never set it; only whole-message copies move it)
        };
        static_assert(sizeof(SongInfoMessage) == 4);
        inline ee::Global<SongInfoMessage, sym::g_songInfo> g_songInfo;

        // The goals' tiers after the first, as the goals' texts name them:
        // four goals a tier. The first entry ("ROOKIE") is not used: a
        // rookie goal's text has no number.
        constexpr uint32_t kGoalTiers = 5;
        constexpr int32_t kGoalsPerTier = 4;
        inline ee::Global<ee::Ptr<const char>[kGoalTiers], sym::g_goalTierNames> g_goalTierNames;
    }

    // The bail's text shown or hidden; hidden while there is none.
    void overlayFallShow(uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        visible = 0;
        if (shown != 0 && *g_fallKind != kOverlayNone)
            visible = 1;
        zge::zgui::setVariable(uiWidget(kFallLabel), kVisibleName, visible);
    }
    READABLE_FUNCTION(overlayFallShow, Verified);

    // A bail's kind sent: its text fades in (kOverlayNone: the label emptied).
    void overlayFallMessage(ee::Ptr<const uint8_t> kind)
    {
        if (*g_fallKind != *kind)
        {
            ee::Local<char[32]> text;
            const ee::Ptr<zge::zgui::Node> label = uiWidget(kFallLabel);
            *g_fallKind = *kind;
            if (*g_fallKind == kOverlayNone)
            {
                zge::stringCopy(text, kNoText);
            }
            else
            {
                zge::formatString(text, kFallFormat, *g_fallKind);
                playWidgetAction(label, kFadeInAction);
            }
            zge::zgui::setVariable(label, kTextName, text);
        }
        zge::memoryCopy(sym::g_fallMessage, kind, sizeof(uint8_t));
    }
    READABLE_FUNCTION(overlayFallMessage, Verified);

    void overlayTipShow() {}
    READABLE_FUNCTION(overlayTipShow, Verified);

    // A tip sent: its text ("$KSTR_HINT" and its number) fades in;
    // kOverlayNone empties the label.
    void overlayTipMessage(ee::Ptr<const uint8_t> tip)
    {
        ee::Local<char[256]> text;
        zge::memoryCopy(sym::g_tipState, tip, sizeof(LabelState));
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kTipLabel);
        if (*tip == kOverlayNone)
        {
            g_tipState->visible = 0;
            text[0] = 0;
        }
        else
        {
            stopWidgetAction(label, kFadeInAction);
            stopWidgetAction(label, kFadeOutAction);
            playWidgetAction(label, kFadeInAction);
            zge::formatString(text, kTipFormat, g_tipState->value & kLabelValueBits);
            g_tipState->visible = 1;
        }
        zge::zgui::setVariable(label, kTextName, text);
    }
    READABLE_FUNCTION(overlayTipMessage, Verified);

    // The tip's state copied out, with whether its label is visible now.
    void overlayTipState(ee::Address out)
    {
        ee::Local<uint32_t> visible;
        zge::zgui::copyVariable(uiWidget(kTipLabel), kVisibleName, visible);
        g_tipState->visible = static_cast<uint16_t>(*visible);
        zge::memoryCopy(out, sym::g_tipState, sizeof(LabelState));
    }
    READABLE_FUNCTION(overlayTipState, Draft);

    void overlayObjectiveSetUp()
    {
        g_objectiveState->visible = 0;
    }
    READABLE_FUNCTION(overlayObjectiveSetUp, Verified);

    // The goal's label shown or hidden; hidden while there is no goal.
    void overlayObjectiveShow(uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        visible = shown;
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kObjectiveLabel);
        if (!label)
            return;
        if (g_objectiveState->value == kOverlayNone)
            visible = 0;
        zge::zgui::setVariable(label, kVisibleName, visible);
    }
    READABLE_FUNCTION(overlayObjectiveShow, Verified);

    // A goal sent: the label takes the goal's text for this level (a rookie
    // goal's has no number; the others have their tier's name and their
    // number in the tier) and, unless the frame is hidden, fades in.
    // kOverlayNone empties the label.
    void overlayObjectiveMessage(ee::Ptr<const uint8_t> goal)
    {
        ee::Local<char[32]> text;
        ee::Local<ee::Ptr<const char>[kGoalTiers]> tierNames;
        for (uint32_t i = 0; i < kGoalTiers; ++i)
            tierNames[i] = (*g_goalTierNames)[i];
        zge::memoryCopy(sym::g_objectiveState, goal, sizeof(LabelState));
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kObjectiveLabel);
        if (!label)
            return;
        if (g_objectiveState->value == kOverlayNone)
        {
            g_objectiveState->visible = 0;
            text[0] = 0;
        }
        else
        {
            const int32_t index = g_objectiveState->value & kLabelValueBits;
            const int32_t tier = index / kGoalsPerTier;
            if (tier == 0)
                zge::formatString(text, kRookieGoalFormat, *g_sessionLevel);
            else
                zge::formatString(text, kGoalFormat, *g_sessionLevel, tierNames[tier], index - tier * kGoalsPerTier);
        }
        zge::zgui::setVariable(label, kTextName, text);
        if (!overlayFrameHidden(kOverlayObjective))
        {
            stopWidgetAction(label, kFadeOutAction);
            playWidgetAction(label, kFadeInAction);
        }
    }
    READABLE_FUNCTION(overlayObjectiveMessage, Verified);

    // The goal's state copied out, with whether its label is shown now (a
    // widget's "Visible" is its scene object's shown flag).
    void overlayObjectiveState(ee::Address out)
    {
        const ee::Ptr<zge::SceneObject> label = widgetObject(uiWidget(kObjectiveLabel));
        g_objectiveState->visible = (label->flags & kWidgetShown) != 0 ? 1 : 0;
        zge::memoryCopy(out, sym::g_objectiveState, sizeof(LabelState));
    }
    READABLE_FUNCTION(overlayObjectiveState, Verified);

    // The exit warning shown or hidden; hidden while there is none.
    void overlayExitWarningShow(uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        visible = shown;
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kExitWarningLabel);
        if (!label)
            return;
        if (*g_exitWarning == kOverlayNone)
            visible = 0;
        zge::zgui::setVariable(label, kVisibleName, visible);
    }
    READABLE_FUNCTION(overlayExitWarningShow, Verified);

    // The exit warning asked for (it fades in when it is not up yet and the
    // frame is not hidden) or, with kOverlayNone, taken away at once.
    void overlayExitWarningMessage(ee::Ptr<const uint8_t> warning)
    {
        ee::Local<uint32_t> visible;
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kExitWarningLabel);
        if (!label)
            return;
        *g_exitWarning = *warning;
        if (*g_exitWarning == kOverlayNone)
        {
            stopWidgetAction(label, kFadeInAction);
            stopWidgetAction(label, kFadeOutAction);
            visible = 0;
            zge::zgui::setVariable(label, kVisibleName, visible);
        }
        else
        {
            zge::zgui::copyVariable(label, kVisibleName, visible);
            if (*visible == 0 && !overlayFrameHidden(kOverlayExitWarning))
                playWidgetAction(label, kFadeInAction);
        }
    }
    READABLE_FUNCTION(overlayExitWarningMessage, Verified);

    // The song's label shown or hidden; hidden while there is no song.
    void overlaySongInfoShow(uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        visible = shown;
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kSongInfoLabel);
        if (!label)
            return;
        if (g_songInfo->song == kOverlayNone)
            visible = 0;
        zge::zgui::setVariable(label, kVisibleName, visible);
    }
    READABLE_FUNCTION(overlaySongInfoShow, Verified);

    // A song sent: its artist and name fade in (kOverlayNone: the label emptied).
    void overlaySongInfoMessage(ee::Ptr<const uint8_t> song)
    {
        ee::Local<char[1024]> text;
        zge::memoryCopy(sym::g_songInfo, song, sizeof(SongInfoMessage));
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kSongInfoLabel);
        if (!label)
            return;
        const uint8_t number = g_songInfo->song;
        if (number == kOverlayNone)
        {
            text[0] = 0;
        }
        else
        {
            zge::formatString(text, kSongInfoFormat, number, number, number, number);
            stopWidgetAction(label, kWaitAction);
            stopWidgetAction(label, kFadeOutAction);
            playWidgetAction(label, kFadeInAction);
        }
        zge::zgui::setVariable(label, kTextName, text);
    }
    READABLE_FUNCTION(overlaySongInfoMessage, Verified);
}
