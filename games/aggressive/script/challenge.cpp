// Natives of the career's score challenges (round 8): the challenge score
// on the HUD (the skater's points, Player +0x8D8, skater/score.cpp), the
// visible countdown and the "no trick in progress" test a challenge's end
// waits on. Recorded over games/aggressive/tools/routes.py's `challenge` (the Movie Lot's
// Kelly: 40,000 points in 60 seconds). Same conventions as library.cpp.

#include "native_parts.h"
#include "script.h"

#include "aggressive/hud/hud.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "aggressive/timers.h"
#include "sym/aggressive/script/challenge.cpp.h"

namespace aggressive::script
{
    using namespace parts;

    namespace
    {
        constexpr uint32_t kSetSkaterPoints = sym::setSkaterPoints;  // (player, points) skater/score.cpp
        constexpr uint32_t kHidePointsPanel = sym::hidePointsPanelTimerDone;  // (player) a timer's callback: the counter hidden
        // Timers (the engine's, createTimer ...): create (callback, data, tick
        // callback, tick data, seconds, interval) -> handle; release (handle).
        constexpr uint32_t kReleaseTimer = sym::releaseTimer;
        // The timer that hides the points counter 5 seconds after
        // hideChallengeScore, 0 when none runs.
        inline ee::Global<uint32_t, sym::g_hidePointsTimer> g_hidePointsTimer;
        inline constexpr ee::Float kHidePointsDelay = 5.0f;

    }

    // setChallengeScore(skater, points, unused): the skater's points.
    int32_t nativeSetChallengeScore()
    {
        const int32_t skater = popCondition();
        const int32_t points = popInt();
        popInt();
        skater::setSkaterPoints(aggressive::parts::levelPlayer(skater), points);
        return 0;
    }
    READABLE_FUNCTION(nativeSetChallengeScore, Verified);

    // showChallengeScore(skater): the points counter shown (a pending
    // hide called off).
    int32_t nativeShowChallengeScore()
    {
        const int32_t skater = popCondition();
        if (const uint32_t timer = *g_hidePointsTimer)
        {
            releaseTimer(timer);
            *g_hidePointsTimer = 0;
        }
        aggressive::hud::showPointsPanel();
        return 0;
    }
    READABLE_FUNCTION(nativeShowChallengeScore, Verified);

    // hideChallengeScore(skater): the points counter hidden 5 seconds on.
    int32_t nativeHideChallengeScore()
    {
        const int32_t skater = popCondition();
        *g_hidePointsTimer = aggressive::createTimer(kHidePointsPanel, skaterHandle(skater), 0, 0, kHidePointsDelay, ee::Float(-1.0f));
        return 0;
    }
    READABLE_FUNCTION(nativeHideChallengeScore, Verified);

    // isSkaterNotPullingAnyTricks(skater) -> 1 when no trick is in progress.
    int32_t nativeIsSkaterNotPullingAnyTricks()
    {
        const int32_t skater = popCondition();
        pushInt(aggressive::skater::noTrickPending(aggressive::parts::levelPlayer(skater)));
        return 1;
    }
    READABLE_FUNCTION(nativeIsSkaterNotPullingAnyTricks, Verified);

    // setTheVisibleTimer(timer): the HUD's countdown shows this timer.
    int32_t nativeSetTheVisibleTimer()
    {
        aggressive::setVisibleTimer(popInt());
        return 0;
    }
    READABLE_FUNCTION(nativeSetTheVisibleTimer, Verified);

    // hideTheVisibleTimer(): the HUD's countdown off.
    int32_t nativeHideTheVisibleTimer()
    {
        aggressive::hideVisibleTimer();
        return 0;
    }
    READABLE_FUNCTION(nativeHideTheVisibleTimer, Verified);

    // hidePointsPanelTimerDone, the timer hideChallengeScore started: the panel goes, and
    // the timer with it.
    void hidePointsPanelTimerDone(uint32_t /*data*/, uint32_t /*timer*/, ee::Ptr<skater::Player> playerHandle)
    {
        hud::hidePointsPanel(playerHandle);
        releaseTimer(*g_hidePointsTimer);
        *g_hidePointsTimer = 0;
    }
    READABLE_FUNCTION_WAITS(hidePointsPanelTimerDone, Verified);
}
