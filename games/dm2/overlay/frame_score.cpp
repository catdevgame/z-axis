// DM2's in-game overlay: the score label (kOverlayScore,
// "InGameUI.Overlay.LabelScore"). The game sends the score to show; the label
// counts up to it, or shows it as the game being played measures it: a
// distance, a number of tricks, degrees of spin. DM2's own code (overlay.c).

#include "frames.h"

#include "dm2/game/round.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/overlay/frame_score.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kScoreLabel{sym::g_textLabelScorePath};    // "InGameUI.Overlay.LabelScore"
        constexpr ee::Ptr<const char> kCountFormat{sym::g_textCountFormat};      // "%ld"
        constexpr ee::Ptr<const char> kTricksFormat{sym::g_textTricksFormat};    // "%ld tricks"
        constexpr ee::Ptr<const char> kDegreesFormat{sym::g_textDegreesFormat};  // "%ld degrees"

        // The score the label shows, and the one sent (with the flags sent
        // since the label last took them).
        inline ee::Global<ee::Float, sym::g_shownScore> g_shownScore;
        inline ee::Global<OverlayScore, sym::g_wantedScore> g_wantedScore;
        // No score: the label is hidden.
        constexpr ee::Float kNoScore = 4294967296.0f;

        // Counting up, a frame takes a tenth of what is left (in whole
        // points) and ten points more.
        inline ee::Global<ee::Float, sym::g_scoreCountRate> g_scoreCountRate;
        constexpr ee::Float kScoreCountStep = 10.0f;
        constexpr uint32_t kDegreesPerHalfTurn = 180;

        // What the score of a multiplayer game is: points, the distance of
        // a jump, a number of tricks, or half turns of spin. (The modes are
        // by number: the menus name them, not this file.)
        enum class ScoreKind { Points, Distance, Tricks, HalfTurns, None };
        ScoreKind scoreKindOf(uint32_t mode)
        {
            switch (mode)
            {
            case kHorseMode:
                return ScoreKind::None;
            case 6: case 7: case 8: case 11: case 12: case 13:
                return ScoreKind::Distance;
            case 9:
                return ScoreKind::Tricks;
            case 10:
                return ScoreKind::HalfTurns;
            default:
                return ScoreKind::Points;
            }
        }

        // A free ride and a park test keep no score.
        bool unscoredSinglePlayerMode(uint32_t mode)
        {
            return mode >= kFreeRide && mode <= kParkTest;
        }

        // The score sent becomes the one shown; false when it already was
        // and nothing asked for it at once.
        bool takeWantedScore()
        {
            const ee::Float wanted = g_wantedScore->score;
            if ((g_wantedScore->flags & kScoreAtOnce) == 0 && wanted == *g_shownScore)
                return false;
            *g_shownScore = wanted;
            return true;
        }
    }

    // As the overlay starts (and ends): the label emptied and shown (hidden).
    void overlayScoreSetUp(uint32_t starting)
    {
        ee::Local<uint32_t> visible;
        visible = starting;
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kScoreLabel);
        if (!label)
            return;
        zge::zgui::setVariable(label, kTextName, kNoText);
        zge::zgui::setVariable(label, kVisibleName, visible);
    }
    READABLE_FUNCTION(overlayScoreSetUp, Verified);

    // The label's frame: its text made from the score. Points count up to
    // the score sent (and while they have nothing to count the text is left
    // alone); every other kind of score is written only when it changed or
    // was sent to show at once, and the label is emptied otherwise.
    void overlayScoreFrame()
    {
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kScoreLabel);
        if (!label)
            return;
        ee::Local<char[32]> text;
        text[0] = 0;

        const uint32_t mode = multiplayerMode();
        ScoreKind kind = scoreKindOf(mode);
        if (mode == 0)
        {
            if (singlePlayerMode() == kFreeRide || singlePlayerMode() == kParkTest)
                kind = ScoreKind::None;
        }
        switch (kind)
        {
        case ScoreKind::Points:
        {
            ee::Float shown = g_wantedScore->score;
            if ((g_wantedScore->flags & kScoreAtOnce) != 0)
            {
                g_wantedScore->flags &= static_cast<uint8_t>(~kScoreAtOnce);
            }
            else
            {
                const ee::Float from = *g_shownScore;
                if (!(from < shown))
                    return;
                const ee::Float tenth = (shown - from) * *g_scoreCountRate;
                const ee::Float next = from + ee::toFloat(ee::toInt(tenth)) + kScoreCountStep;
                if (next < shown)
                    shown = next;
            }
            *g_shownScore = shown;
            zge::formatString(text, kCountFormat, sce::libgcc::fptoui(*g_shownScore));
            break;
        }
        case ScoreKind::Distance:
            if (takeWantedScore())
                formatFeetAndInches(text, *g_shownScore);
            break;
        case ScoreKind::Tricks:
            if (takeWantedScore())
                zge::formatString(text, kTricksFormat, sce::libgcc::fptoui(*g_shownScore));
            break;
        case ScoreKind::HalfTurns:
            if (takeWantedScore())
                zge::formatString(text, kDegreesFormat, sce::libgcc::fptoui(*g_shownScore) * kDegreesPerHalfTurn);
            break;
        case ScoreKind::None:
            break;
        }
        zge::zgui::setVariable(label, kTextName, text);
    }
    READABLE_FUNCTION(overlayScoreFrame, Verified);

    // The label shown or hidden: it stays hidden in a game that keeps no
    // score (a free ride, a park test, HORSE) and while there is no score.
    void overlayScoreShow(uint32_t shown)
    {
        ee::Local<uint32_t> visible;
        visible = shown;
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kScoreLabel);
        if (!label)
            return;
        const uint32_t mode = multiplayerMode();
        if (mode == 0)
        {
            if (unscoredSinglePlayerMode(singlePlayerMode()))
                visible = 0;
        }
        else if (mode == kHorseMode)
        {
            visible = 0;
        }
        if (*g_shownScore == kNoScore)
            visible = 0;
        zge::zgui::setVariable(label, kVisibleName, visible);
    }
    READABLE_FUNCTION(overlayScoreShow, Verified);

    // A score sent: kept for the frame to show (kNoScore at once), and the
    // label made visible when there is a score and the frame is not hidden.
    // HORSE leaves the label as it is.
    void overlayScoreMessage(ee::Ptr<const OverlayScore> sent)
    {
        const ee::Ptr<zge::zgui::Node> label = uiWidget(kScoreLabel);
        if (!label)
            return;
        g_wantedScore->score = sent->score;
        g_wantedScore->flags |= sent->flags;
        if (sent->score == kNoScore)
            *g_shownScore = sent->score;

        const uint32_t mode = multiplayerMode();
        if (mode == 0)
            singlePlayerMode(); // (asked and not used: a free ride's label is hidden by its show handler)
        else if (mode == kHorseMode)
            return;
        ee::Local<uint32_t> visible;
        visible = 0;
        if (*g_shownScore != kNoScore && !overlayFrameHidden(kOverlayScore))
            visible = 1;
        zge::zgui::setVariable(label, kVisibleName, visible);
    }
    READABLE_FUNCTION(overlayScoreMessage, Verified);
}
