#pragma once
// DM2's in-game overlay (overlay.c): what the frames' files share. Each
// frame of the table (overlay.h) has its handlers in a file of its own
// (frame_score.cpp, frame_fade.cpp ...); most frames are one widget of the
// "InGameUI.Overlay" layout, shown, hidden and given its text here.

#include "overlay.h"
#include "screens.h"

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>
#include "sym/dm2/overlay/frames.h.h"

namespace game::dm2
{
    // The frame table (kOverlayFrames records).
    inline ee::Global<OverlayFrameRecord[kOverlayFrames], sym::g_overlayFrames> g_overlayFrames;
    // Whether a frame is hidden now (showOverlayFrame).
    inline bool overlayFrameHidden(OverlayFrame frame)
    {
        return ((*g_overlayFrames)[frame].flags & kOverlayFrameHidden) != 0;
    }

    // The widgets' variables the frames set, the text that clears a label,
    // and the actions a label plays as its text changes.
    constexpr ee::Ptr<const char> kVisibleName{sym::g_textOverlayVisible}; // "Visible"
    constexpr ee::Ptr<const char> kTextName{sym::g_textOverlayText};       // "Text"
    constexpr ee::Ptr<const char> kNoText{sym::g_textOverlayEmpty};        // ""
    constexpr ee::Ptr<const char> kFadeInAction{sym::g_textFadeIn};        // "FadeIn"
    constexpr ee::Ptr<const char> kFadeOutAction{sym::g_textFadeOut};      // "FadeOut"

    // A frame's handlers, by frame.
    void overlayFadeFrame();
    void overlayFadeMessage(ee::Ptr<const ScreenFade> fade);
    void overlayScoreSetUp(uint32_t starting);
    void overlayScoreFrame();
    void overlayScoreShow(uint32_t shown);
    void overlayScoreMessage(ee::Ptr<const OverlayScore> score);
    void overlayTimerShow(uint32_t shown);
    void overlayTimerMessage(ee::Ptr<const uint32_t> framesLeft);
    void bmxoverlayDisplayTimerUpdate(); // the timer logo's script function
    void overlayTrickSetUp(uint32_t starting);
    void overlayTrickShow(uint32_t shown);
    void overlayTrickMessage(ee::Ptr<const OverlayTrick> message);
    void overlayTrickShownMessage();
    void overlayRulerSetUp(uint32_t starting);
    void overlayRulerFrame();
    void overlayRulerShow(uint32_t shown);
    void overlayRulerMessage(ee::Ptr<const OverlayRuler> measure);
    void overlayGaugeSetUp(uint32_t starting);
    void overlayGaugeFrame();
    void overlayGaugeShow(uint32_t shown);
    void overlayGaugeMessage(ee::Ptr<const ee::Float> value);
    void overlayFallShow(uint32_t shown);
    void overlayFallMessage(ee::Ptr<const uint8_t> kind);
    void overlayEmptySetUp();
    void overlayEmptyFrame();
    void overlayEmptyShow();
    void overlayEmptyMessage(ee::Ptr<const uint8_t> value);
    void overlayReplayMarkSetUp();
    void overlayReplayMarkFrame();
    void overlayReplayMarkShow();
    void overlayReplayMarkMessage(ee::Ptr<const uint8_t> replayState);
    void overlayUnnamed3Frame();
    void overlayUnnamed15SetUp();
    void overlayUnnamed15Show();
    void overlayTipShow();
    void overlayTipMessage(ee::Ptr<const uint8_t> tip);
    void overlayTipState(ee::Address out);
    void overlayObjectiveSetUp();
    void overlayObjectiveShow(uint32_t shown);
    void overlayObjectiveMessage(ee::Ptr<const uint8_t> goal);
    void overlayRewardSetUp(uint32_t starting);
    void overlayRewardFrame();
    void overlayRewardShow(uint32_t shown);
    void overlayRewardMessage(ee::Ptr<const OverlayReward> reward);
    void overlayObjectiveState(ee::Address out);
    void overlayExitWarningShow(uint32_t shown);
    void overlayExitWarningMessage(ee::Ptr<const uint8_t> warning);
    void overlaySongInfoShow(uint32_t shown);
    void overlaySongInfoMessage(ee::Ptr<const uint8_t> song);
}
