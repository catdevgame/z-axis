// The game over screen's standings (standings.h). BMX XXX's own code (game.c).

#include "standings.h"
#include "player_riders.h"
#include "round.h"

#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/bmx/game/standings.cpp.h"

namespace game::bmx
{
    namespace
    {
        // A player's line, after a line break from the second on.
        constexpr uint32_t kPlayerLine = sym::g_textStandingPlayer;       // "%s$KSTR_PLAYER $KSTR_NUMBER_%d: "
        constexpr ee::Ptr<const char> kLineBreak{sym::g_textLineBreak};   // "\\n" (the script's line break)
        constexpr ee::Ptr<const char> kNoBreak{sym::g_textNoSeparator};   // ""
        // A letter of the word the player has, and the one just taken (red,
        // then the text's grey again).
        constexpr uint32_t kLetter = sym::g_textHorseLetter;              // " %c"
        constexpr uint32_t kNewLetter = sym::g_textHorseLetterNew;        // "#RGB{170 20 10} %c#RGB{192 192 192}"
    }

    // wordGameStandingText: a line a player; a player's letters are its
    // score block's horseLetters, from the HORSE word.
    void wordGameStandingText(ee::Ptr<char> out)
    {
        out[0] = 0;
        for (uint32_t player = 0; player < *g_playerCount; ++player)
        {
            const rider::ScoreBlock &score = playerRider(player)->score;
            zge::formatString(out + zge::stringLength(out), kPlayerLine, player != 0 ? kLineBreak : kNoBreak, player);
            for (uint32_t letter = 0; letter < score.horseLetters; ++letter)
            {
                if (score.turnScore == 0.0f && letter == score.horseLetters - 1)
                {
                    const ee::Ptr<char> end = out + zge::stringLength(out);
                    zge::formatString(end, kNewLetter, static_cast<int32_t>(static_cast<int8_t>(horseWord()[letter])));
                }
                else
                {
                    const ee::Ptr<char> end = out + zge::stringLength(out);
                    zge::formatString(end, kLetter, static_cast<int32_t>(static_cast<int8_t>(horseWord()[letter])));
                }
            }
        }
    }
    READABLE_FUNCTION(wordGameStandingText, Draft);

    namespace
    {
        // The leader's line: the number's key, "PLAYER" and it, the result.
        constexpr uint32_t kNumberKey = sym::g_textNumberKey;               // "$KSTR_NUMBER_%d"
        constexpr uint32_t kLeaderLine = sym::g_textStandingLeader;         // "%s %s"
        constexpr ee::Ptr<const char> kPlayerKey{sym::g_textPlayerKey};     // "$KSTR_PLAYER"
        constexpr uint32_t kSeriesWin = sym::g_textSeriesWin;               // "$KSTR_SERIES_WIN_%d"
        constexpr uint32_t kSeriesLeads = sym::g_textSeriesLeads;           // "$KSTR_SERIES_WIN_2"
        constexpr uint32_t kResult = sym::g_textStandingResult;             // " %s "
        constexpr uint32_t kTieLine = sym::g_textStandingTie;               // "%s "
        constexpr ee::Ptr<const char> kSeriesTie{sym::g_textSeriesTie};     // "$KSTR_SERIES_TIE"
        // The rounds won: two players', or each one's after a dash.
        constexpr uint32_t kTwoScores = sym::g_textRoundsWonPair;           // "%ld-%ld"
        constexpr uint32_t kScoreInList = sym::g_textRoundsWonListed;       // "%s%ld"
        constexpr ee::Ptr<const char> kDash{sym::g_textRoundsWonDash};      // "-"
        // roundWinner's count below two asks for the game's leader.
        constexpr uint32_t kLeaderRounds = 1;
    }

    // seriesStandingText: the leader's line unless the game is tied (the
    // result's number: 0 when the leader won every round, 1 when not, 2
    // while the game goes on), then the rounds each rider won (its score
    // block's horseLetters in a game of rounds).
    void seriesStandingText(ee::Ptr<char> out)
    {
        ee::Local<char[0x40]> text;
        out[0] = 0;
        if (!roundFlags(kTieFlag))
        {
            const uint8_t leader = roundWinner(roundNumber(), kLeaderRounds);
            zge::formatString(text, kNumberKey, leader);
            zge::formatString(out + zge::stringLength(out), kLeaderLine, kPlayerKey, text);
            if (roundFlags(kGameEndsFlag))
            {
                const ee::Ptr<rider::Rider> rider = playerRider(leader);
                zge::formatString(text, kSeriesWin, rider->score.horseLetters != roundCount() ? 1 : 0);
            }
            else
            {
                zge::formatString(text, kSeriesLeads);
            }
            zge::formatString(out + zge::stringLength(out), kResult, text);
        }
        else
        {
            zge::formatString(out, kTieLine, kSeriesTie);
        }

        if (*g_playerCount == 2 && !roundFlags(kTieFlag))
        {
            const uint8_t leader = roundWinner(roundNumber(), kLeaderRounds);
            const ee::Ptr<char> end = out + zge::stringLength(out);
            const ee::Ptr<rider::Rider> first = playerRider(leader);
            const ee::Ptr<rider::Rider> second = playerRider(leader == 0 ? 1 : 0);
            zge::formatString(end, kTwoScores, first->score.horseLetters, second->score.horseLetters);
        }
        else
        {
            for (uint32_t player = 0; player < *g_playerCount; ++player)
            {
                const ee::Ptr<char> end = out + zge::stringLength(out);
                const ee::Ptr<const char> dash = player != 0 ? kDash : kNoBreak;
                zge::formatString(end, kScoreInList, dash, playerRider(player)->score.horseLetters);
            }
        }
    }
    READABLE_FUNCTION(seriesStandingText, Verified);
}
