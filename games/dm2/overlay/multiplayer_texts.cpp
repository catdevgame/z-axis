// DM2's standings texts of a series (multiplayer mode 2) and of HORSE (mode
// 3), which the game over and multiplayer ready screens show (screens.h).
// DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "dm2/rider/physics/physics.h"
#include "dm2/rider/rider.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/overlay/multiplayer_texts.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kNumberFormat{sym::g_textNumberFormat};          // "$KSTR_NUMBER_%d"
        constexpr ee::Ptr<const char> kTwoWordsFormat{sym::g_textTwoWordsFormat};      // "%s %s"
        constexpr ee::Ptr<const char> kPlayerWord{sym::g_textPlayerWord};              // "$KSTR_PLAYER"
        constexpr ee::Ptr<const char> kSeriesResultFormat{sym::g_textSeriesResultFormat}; // "$KSTR_SERIES_WIN_%d"
        constexpr ee::Ptr<const char> kSeriesLeads{sym::g_textSeriesWin2};             // "$KSTR_SERIES_WIN_2"
        constexpr ee::Ptr<const char> kSpacedWordFormat{sym::g_textSpacedWordFormat};  // " %s "
        constexpr ee::Ptr<const char> kWordSpaceFormat{sym::g_textWordSpaceFormat};    // "%s "
        constexpr ee::Ptr<const char> kSeriesTie{sym::g_textSeriesTie};                // "$KSTR_SERIES_TIE"
        constexpr ee::Ptr<const char> kRoundsFormat{sym::g_textRoundsFormat};          // "%ld-%ld"
        constexpr ee::Ptr<const char> kSeparatedNumberFormat{sym::g_textSeparatedNumberFormat}; // "%s%ld"
        constexpr ee::Ptr<const char> kDash{sym::g_textDash};                          // "-"
        constexpr ee::Ptr<const char> kHorsePlayerFormat{sym::g_textHorsePlayerFormat}; // "%s$KSTR_PLAYER $KSTR_NUMBER_%d: "
        // A letter, and the turn's new one in red ("#RGB{170 20 10} %c#RGB{192 192 192}").
        constexpr ee::Ptr<const char> kHorseLetterFormat{sym::g_textHorseLetterFormat}; // " %c"
        constexpr ee::Ptr<const char> kHorseNewLetterFormat{sym::g_textHorseNewLetterFormat};
        constexpr ee::Ptr<const char> kNoSeparator{sym::g_textEmpty};
        constexpr ee::Ptr<const char> kLineBreak{sym::g_textNewLine};

        ee::Ptr<char> textEnd(ee::Ptr<char> text)
        {
            return text + static_cast<int32_t>(zge::stringLength(text));
        }
        // ScoreBlock::horseLetters: in a series the rounds a rider has won,
        // in HORSE the letters taken.
        uint32_t roundsWon(uint32_t player)
        {
            return rider::riderOf(riderIndexOf(player))->score.horseLetters;
        }
    }

    // seriesStandingText: the leader ("$KSTR_PLAYER $KSTR_NUMBER_n") and,
    // once the series is over, its result by whether the winner's rounds are
    // all the rounds played (else "leads"); or a tie. Then the rounds won:
    // the leader's against the other's with two players, else each player's
    // in turn.
    void seriesStandingText(ee::Ptr<char> out)
    {
        ee::Local<char[0x40]> word;
        out[0] = 0;
        if (!roundFlags(kTieFlag))
        {
            const uint8_t leader = roundWinner(roundNumber(), 1);
            zge::formatString(word, kNumberFormat, leader);
            zge::formatString(textEnd(out), kTwoWordsFormat, kPlayerWord, word);
            if (roundFlags(kGameEndsFlag))
            {
                const ee::Ptr<rider::Rider> winner = rider::riderOf(riderIndexOf(leader));
                const uint32_t rounds = roundCount();
                zge::formatString(word, kSeriesResultFormat, winner->score.horseLetters != rounds ? 1 : 0);
            }
            else
                zge::formatString(word, kSeriesLeads);
            zge::formatString(textEnd(out), kSpacedWordFormat, word);
        }
        else
            zge::formatString(out, kWordSpaceFormat, kSeriesTie);

        if (*g_multiplayerPlayers == 2 && !roundFlags(kTieFlag))
        {
            const uint8_t leader = roundWinner(roundNumber(), 1);
            const ee::Ptr<char> end = textEnd(out);
            const uint32_t leaderRounds = roundsWon(leader);
            const uint32_t otherRounds = roundsWon(leader == 0 ? 1 : 0);
            zge::formatString(end, kRoundsFormat, leaderRounds, otherRounds);
            return;
        }
        for (uint32_t player = 0; player < *g_multiplayerPlayers;)
        {
            const ee::Ptr<char> end = textEnd(out);
            const ee::Ptr<const char> separator = player == 0 ? kNoSeparator : kDash;
            const uint32_t rounds = roundsWon(player);
            ++player;
            zge::formatString(end, kSeparatedNumberFormat, separator, rounds);
        }
    }
    READABLE_FUNCTION(seriesStandingText, Verified);

    // horseLettersText: each player's line ("$KSTR_PLAYER $KSTR_NUMBER_n:")
    // with the HORSE word's letters taken; the letter taken this turn (the
    // turn scored nothing) blinks red with the screen's frames.
    void horseLettersText(ee::Ptr<char> out)
    {
        out[0] = 0;
        for (uint32_t player = 0; player < *g_multiplayerPlayers; ++player)
        {
            const ee::Ptr<rider::Rider> rider = rider::riderOf(riderIndexOf(player));
            zge::formatString(textEnd(out), kHorsePlayerFormat, player == 0 ? kNoSeparator : kLineBreak, player);
            for (uint32_t letter = 0; letter < rider->score.horseLetters; ++letter)
            {
                const bool newLetter = rider->score.turnScore == 0.0f && letter == rider->score.horseLetters - 1 &&
                                       (*g_screenFrames & 1) != 0;
                const ee::Ptr<char> end = textEnd(out);
                const int8_t character = static_cast<int8_t>(horseWord()[letter]);
                zge::formatString(end, newLetter ? kHorseNewLetterFormat : kHorseLetterFormat, character);
            }
        }
    }
    READABLE_FUNCTION(horseLettersText, Verified);
}
