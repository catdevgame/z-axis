// DM2's leaderboard (screens.h; "InGameUI.FrameLeaderboard"), the
// competition's: the player's judges' scores of this run, the player's runs'
// averages, and the ten competitors' runs and averages by their ranking.
// DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/game/competition.h"
#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_leaderboard.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kLeaderboardFrame{sym::g_textFrameLeaderboard}; // "InGameUI.FrameLeaderboard"
        // The player's scores ("FrameScores": "Item00" .. "Item04" the
        // judges', "Item05" and on the runs' averages, "Item07" the
        // competition's average) and the board ("FrameBoard": its columns).
        constexpr ee::Ptr<const char> kScoresName{sym::g_textFrameScores};      // "FrameScores"
        constexpr ee::Ptr<const char> kItemFormat{sym::g_textItemFormat};       // "Item%02d"
        constexpr ee::Ptr<const char> kItemValueFormat{sym::g_textItemValueFormat}; // "Item%02d.Value"
        constexpr ee::Ptr<const char> kAverageItem{sym::g_textItem07};          // "Item07"
        constexpr ee::Ptr<const char> kAverageItemValue{sym::g_textItem07Value}; // "Item07.Value"
        constexpr ee::Ptr<const char> kBoardName{sym::g_textFrameBoard};        // "FrameBoard"
        constexpr ee::Ptr<const char> kDataName{sym::g_textDataName};           // "DataName"
        constexpr ee::Ptr<const char> kDataRun1{sym::g_textDataRun1};           // "DataRun1"
        constexpr ee::Ptr<const char> kDataRun2{sym::g_textDataRun2};           // "DataRun2"
        constexpr ee::Ptr<const char> kDataAverage{sym::g_textDataAverage};     // "DataAverage"
        constexpr ee::Ptr<const char> kTextName{sym::screenTextName};           // "Text"
        constexpr ee::Ptr<const char> kFadeInAction{sym::g_textFadeInAction};   // "FadeIn"
        constexpr ee::Ptr<const char> kScoreFormat{sym::g_textScoreTenthsFormat}; // "%02d.%d"
        constexpr ee::Ptr<const char> kLineFormat{sym::trickWordFormat};        // "%s%s"
        constexpr ee::Ptr<const char> kNoSeparator{sym::g_textEmpty};
        constexpr ee::Ptr<const char> kLineBreak{sym::g_textNewLine};
        // The first run's average's item (the judges' are 0 .. 4).
        constexpr uint32_t kFirstAverageItem = rider::kCompetitionJudges;

        // A judge's byte (255 the most) as a score out of 100: the game's four
        // copies of 100 / 255, one a part of the screen.
        inline ee::Global<ee::Float, sym::judgeScoreScale> judgeScoreScale;
        inline ee::Global<ee::Float, sym::runAverageScale> runAverageScale;
        inline ee::Global<ee::Float, sym::competitionAverageScale> competitionAverageScale;
        inline ee::Global<ee::Float, sym::leaderboardScoreScale> leaderboardScoreScale;
        constexpr float kTenths = 10.0f;


        // A score written "%02d.%d": its whole number and its tenth (both
        // toward zero).
        void formatScore(ee::Ptr<char> out, uint8_t value, ee::Float scale)
        {
            const ee::Float score = ee::toFloat(static_cast<int32_t>(value)) * scale;
            const ee::Float whole = ee::toFloat(ee::toInt(score));
            zge::formatString(out, kScoreFormat, ee::toInt(whole), ee::toInt((score - whole) * ee::Float(kTenths)));
        }
        ee::Ptr<char> textEnd(ee::Ptr<char> text)
        {
            return text + static_cast<int32_t>(zge::stringLength(text));
        }
        // The scores' items of the runs so far (and the competition's
        // average after the first) given an action.
        void playScoreItems(ee::Ptr<zge::zgui::Node> scores, ee::Local<char[0x40]> &item, ee::Ptr<const char> action)
        {
            for (uint32_t run = 0; run <= roundNumber(); ++run)
            {
                zge::formatString(item, kItemFormat, kFirstAverageItem + run);
                playWidgetAction(zge::zgui::findNode(scores, item, 0), action);
            }
            if (roundNumber() != 0)
            {
                zge::formatString(item, kAverageItem);
                playWidgetAction(zge::zgui::findNode(scores, item, 0), action);
            }
        }
    }

    // showLeaderboardScreen: as it opens, the player's judges' scores of the
    // run just ridden, the player's runs' averages (and after the first run
    // the competition's average), and the board: the ten competitors by
    // ranking (a created rider by the player's name) with their runs and
    // averages; the frame's Init, then the score items fade in. As it
    // closes, the items' and the frame's DeInit.
    void showLeaderboardScreen(uint32_t shown)
    {
        ee::Local<char[0x40]> item;
        ee::Local<char[0x100]> value;
        ee::Local<char[0x100]> names;
        ee::Local<char[rider::kCompetitionRuns][0x40]> runs;
        ee::Local<char[0x40]> averages;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kLeaderboardFrame);
        if (!frame)
            return;
        if (shown == 0)
        {
            const ee::Ptr<zge::zgui::Node> scores = zge::zgui::findNode(frame, kScoresName, 0);
            for (uint32_t run = 0; run < static_cast<uint32_t>(roundNumber()) + 1; ++run)
            {
                zge::formatString(item, kItemFormat, kFirstAverageItem + run);
                playWidgetAction(zge::zgui::findNode(scores, item, 0), kDeInitAction);
            }
            if (roundNumber() != 0)
            {
                zge::formatString(item, kAverageItem);
                playWidgetAction(zge::zgui::findNode(scores, item, 0), kDeInitAction);
            }
            playWidgetAction(frame, kDeInitAction);
            return;
        }

        const PlayerSetup &setup = (*g_playerSetups)[*g_currentPlayer];
        const uint8_t character = setup.character;
        const ee::Ptr<zge::zgui::Node> scores = zge::zgui::findNode(frame, kScoresName, 0);
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(*g_currentPlayer));
        const rider::CompetitionRun &thisRun = rider->competitionRuns[roundNumber()];
        for (uint32_t judge = 0; judge < rider::kCompetitionJudges; ++judge)
        {
            formatScore(value, thisRun.judges[judge], *judgeScoreScale);
            zge::formatString(item, kItemValueFormat, judge);
            zge::zgui::setVariable(zge::zgui::findNode(scores, item, 0), kTextName, value);
        }
        for (uint32_t run = 0; run <= roundNumber(); ++run)
        {
            formatScore(value, rider->competitionRuns[run].average, *runAverageScale);
            zge::formatString(item, kItemValueFormat, kFirstAverageItem + run);
            zge::zgui::setVariable(zge::zgui::findNode(scores, item, 0), kTextName, value);
        }
        if (roundNumber() != 0)
        {
            formatScore(value, (*g_competitionScores)[character].average, *competitionAverageScale);
            zge::formatString(item, kAverageItemValue);
            zge::zgui::setVariable(zge::zgui::findNode(scores, item, 0), kTextName, value);
        }

        const ee::Ptr<zge::zgui::Node> board = zge::zgui::findNode(frame, kBoardName, 0);
        const ee::Float scale = *leaderboardScoreScale;
        names[0] = 0;
        runs[0][0] = 0;
        runs[1][0] = 0;
        averages[0] = 0;
        for (uint32_t place = 0; place < kCompetitionRiders; ++place)
        {
            const uint8_t who = (*g_competitionRanking)[place];
            const ee::Ptr<const char> separator = place == 0 ? kNoSeparator : kLineBreak;
            const bool createdPlayer = who == character && setup.character >= kBuiltInCharacters &&
                                       setup.character < kCharacters;
            const ee::Ptr<const char> name = createdPlayer ? ee::Ptr<const char>(setup.name)
                                                           : kCharacterNames[who].fullName;
            zge::formatString(textEnd(names), kLineFormat, separator, name);
            for (uint32_t run = 0; run < static_cast<uint32_t>(roundNumber()) + 1; ++run)
            {
                formatScore(value, (*g_competitionScores)[who].runs[run], scale);
                const ee::Ptr<char> column(runs[run]);
                zge::formatString(textEnd(column), kLineFormat, separator, value);
            }
            formatScore(value, (*g_competitionScores)[who].average, scale);
            zge::formatString(textEnd(averages), kLineFormat, separator, value);
        }
        zge::zgui::setVariable(zge::zgui::findNode(board, kDataName, 0), kTextName, names);
        zge::zgui::setVariable(zge::zgui::findNode(board, kDataRun1, 0), kTextName, runs[0]);
        zge::zgui::setVariable(zge::zgui::findNode(board, kDataRun2, 0), kTextName, runs[1]);
        zge::zgui::setVariable(zge::zgui::findNode(board, kDataAverage, 0), kTextName, averages);
        playWidgetAction(frame, kInitAction);
        playScoreItems(zge::zgui::findNode(frame, kScoresName, 0), item, kFadeInAction);
    }
    READABLE_FUNCTION(showLeaderboardScreen, Verified);

    namespace
    {
        // What the leaderboard's script asks (g_uiMessageType): message 3
        // plays sound event 0x2F (what sends it is not read yet); any other
        // leaves the screen.
        inline ee::Global<int32_t, sym::g_uiMessageType> g_uiMessageType;
        constexpr int32_t kSoundMessage = 3;
        constexpr uint32_t kLeaderboardSound = 0x2F;
        // rider::ScoreBlock::goalFlags: the turn found the level's last gap
        // (the reward screen tells it).
        constexpr uint32_t kAllGapsFoundFlag = 0x8000;
    }

    // bmxoverlayLeaderboardHandler: message 3's sound; else the screen
    // left: the reward screen at the game's last round or when the turn
    // found the level's last gap, else the round over screen.
    void bmxoverlayLeaderboardHandler()
    {
        if (*g_uiMessageType == kSoundMessage)
        {
            game::tail(soundEvent, kLeaderboardSound, 0, 0, 0, 0);
            return;
        }
        if (roundFlags(kGameEndsFlag))
        {
            showGameScreen(kScreenReward);
            return;
        }
        const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(*g_currentPlayer));
        showGameScreen((rider->score.goalFlags & kAllGapsFoundFlag) ? kScreenReward : kScreenRoundOver);
    }
    READABLE_FUNCTION(bmxoverlayLeaderboardHandler, Verified);
}
