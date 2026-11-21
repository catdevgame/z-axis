// The judged competition (cut: nothing in the game starts one): eight
// entrants, the player's skater and seven rivals, over three rounds; the
// player's run is scored by six judges, the rivals' at random from their
// base and spread; after three rounds each total leaves out the worst.

#include "aggressive/skater/skater.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/competition.cpp.h"

namespace aggressive::level
{

    namespace
    {
        inline ee::Global<int32_t, sym::g_competitionSeed> g_competitionSeed;
        inline ee::Global<int32_t, sym::g_competitionScoring> g_competitionScoring; // g_competitionScoring; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_competitionRound> g_competitionRound; // g_competitionRound; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_lastScoredCompetitionRound> g_lastScoredCompetitionRound; // g_lastScoredCompetitionRound; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<ee::Float, sym::g_competitionPlayerScore> g_competitionPlayerScore;
    }

    // An entrant (g_competitionEntrants: 20, the first unused).
    struct CompetitionEntrant
    {
        uint8_t _00;               // +0x00 nothing reads it
        uint8_t picked;            // +0x01 in this competition
        uint8_t isPlayer;          // +0x02
        uint8_t droppedRound;      // +0x03 the round its total leaves out (after three)
        ee::Float roundScores[3];  // +0x04
        ee::Float baseScore[3];    // +0x10 a rival's, by score scale
        ee::Float scoreSpread[3];  // +0x1C up to this much more at random, by score scale
        ee::Float total;           // +0x28
        char name[0x20];           // +0x2C "None", "Chris Edwards" ...
    };
    static_assert(offsetof(CompetitionEntrant, total) == 0x28 && sizeof(CompetitionEntrant) == 0x4C);

    namespace
    {
        constexpr uint32_t kEntrants = 20, kRivalChoices = 13, kLineup = 8, kJudges = 6;
        // The three score scales' targets: the player's score over the target
        // (capped at 1) makes the judges' base; past 1.2 times the threshold
        // every judge gives 100.
        inline ee::Global<ee::Float[3], sym::g_judgeScoreThresholds> g_judgeScoreThresholds; // 300000, 3e6, 3e8
        inline ee::Global<ee::Float[3], sym::g_judgeScoreSteps> g_judgeScoreSteps;           // the split score's: 100000, 200000, 3e6
        inline ee::Global<CompetitionEntrant[kEntrants], sym::g_competitionEntrants> g_competitionEntrants;
        inline ee::Global<uint8_t[kLineup], sym::g_competitionLineup> g_competitionLineup;
        inline ee::Global<ee::Float[kJudges], sym::g_competitionPlayerScores> g_competitionPlayerScores; // the judges' scores of the player's last run
        constexpr int32_t kScoringSplit = 1; // g_competitionScoring: the split score (else the skater's)
        constexpr int32_t kJudgeBase = 96, kJudgeRandom = 4, kJudgeTop = 100;

        CompetitionEntrant &entrantIn(uint32_t slot) { return (*g_competitionEntrants)[(*g_competitionLineup)[slot]]; }
    }

    // competitionRandom: the competition's own random number, 0 to about 0.89: the seed turned by 4 bits and
    // by 16, the two added and mixed with 0x87654321; that, its halves swapped, is the next seed, and over
    // 2,415,919,000 (without its sign) the number.
    ee::Float competitionRandom()
    {
        const int32_t seed = *g_competitionSeed;
        const int32_t mixed = (seed << 4 | static_cast<uint32_t>(seed) >> 28) + (static_cast<uint32_t>(seed) >> 16 | seed << 16) ^ static_cast<int32_t>(0x87654321u);
        *g_competitionSeed = mixed << 16 | static_cast<uint32_t>(mixed) >> 16;
        ee::Float result = ee::toFloat(mixed) / ee::Float(2.415919e+09f);
        if (result < ee::Float(0.0f))
            result = -result;
        return result;
    }
    READABLE_FUNCTION(competitionRandom, Verified);

    void competitionNoop()
    {
    }
    READABLE_FUNCTION(competitionNoop, Verified);

    // judgePlayerRun: six judges' scores of the player's run into `scores`,
    // sorted best first, and the mean of the middle four (the best and the
    // worst left out). Each is 96 times the player's score over the target
    // of the scale (at most 1), plus up to 4 at random; 100 past 1.2 times
    // the threshold.
    ee::Float judgePlayerRun(ee::Ptr<skater::Player> player, uint32_t scale, ee::Ptr<ee::Float> scores)
    {
        scale &= 0xFFu;
        ee::Float score;
        ee::Float target;
        if (*g_competitionScoring == kScoringSplit)
        {
            score = ee::toFloat(aggressive::skater::splitScore(player));
            target = (*g_judgeScoreSteps)[scale];
        }
        else
        {
            score = ee::toFloat(skater::skaterScore(player));
            target = (*g_judgeScoreThresholds)[scale];
        }
        ee::Float ratio = score / target;
        if (ee::Float(1.0f) < ratio)
            ratio = ee::Float(1.0f);
        const ee::Float base = ratio * ee::Float(static_cast<float>(kJudgeBase));
        for (uint32_t judge = 0; judge < kJudges; ++judge)
        {
            scores[judge] = base + competitionRandom() * ee::Float(static_cast<float>(kJudgeRandom));
            if ((*g_judgeScoreThresholds)[scale] * ee::Float(1.2f) < ee::toFloat(skater::skaterScore(player)))
                scores[judge] = ee::Float(static_cast<float>(kJudgeTop));
        }
        for (uint32_t i = 0; i < kJudges - 1; ++i)
            for (uint32_t j = i + 1; j < kJudges; ++j)
                if (scores[i] < scores[j])
                {
                    const ee::Float higher = scores[j];
                    scores[j] = scores[i];
                    scores[i] = higher;
                }
        ee::Float sum = ee::Float(0.0f);
        for (uint32_t judge = 1; judge < kJudges - 1; ++judge)
            sum = sum + scores[judge];
        return sum / ee::Float(4.0f);
    }
    READABLE_FUNCTION(judgePlayerRun, Verified);

    // pickCompetitionRivals: a new competition: the player's skater in slot
    // 0 and seven rivals drawn at random from the first 13 entrants, the
    // round back to 0. Kept from the original: entrant 0 is never cleared
    // and can never be a rival (a slot of 0 counts as not filled).
    uint32_t pickCompetitionRivals(ee::Ptr<skater::Player> player, uint32_t scoring)
    {
        scoring &= 0xFFu;
        for (uint32_t slot = 1; slot < kLineup; ++slot)
            (*g_competitionLineup)[slot] = 0;
        for (uint32_t entrant = 1; entrant < kEntrants; ++entrant)
        {
            (*g_competitionEntrants)[entrant].picked = 0;
            (*g_competitionEntrants)[entrant].isPlayer = 0;
        }
        (*g_competitionLineup)[0] = player->skaterIndex & 0x1Fu;
        (*g_competitionEntrants)[player->skaterIndex & 0x1Fu].picked = 1;
        (*g_competitionEntrants)[player->skaterIndex & 0x1Fu].isPlayer = 1;
        for (uint32_t slot = 1; slot < kLineup; ++slot)
        {
            uint32_t entrant = static_cast<uint32_t>(ee::toInt(competitionRandom() * ee::Float(static_cast<float>(kRivalChoices))));
            // The next one not yet in, from the drawn one on.
            while ((*g_competitionLineup)[slot] == 0)
            {
                if ((*g_competitionEntrants)[entrant].picked != 1)
                {
                    (*g_competitionLineup)[slot] = entrant;
                    (*g_competitionEntrants)[entrant].picked = 1;
                }
                else
                {
                    entrant = entrant + 1 >= kRivalChoices ? 0 : entrant + 1;
                }
            }
        }
        *g_competitionScoring = scoring;
        *g_lastScoredCompetitionRound = -1;
        *g_competitionRound = 0;
        return 1;
    }
    READABLE_FUNCTION(pickCompetitionRivals, Verified);

    // scoreCompetitionRound: the round's scores, once a round: the player's
    // judged, the rivals' their base plus their spread at random; each
    // total the round's score (round 0), the mean of two (round 1), or the
    // mean of the best two of three (round 2); then the lineup sorted by
    // total, best first. Always 0.
    int32_t scoreCompetitionRound(ee::Ptr<skater::Player> player, uint32_t scale)
    {
        const int32_t round = *g_competitionRound;
        scale &= 0xFFu;
        if (!(*g_lastScoredCompetitionRound < round))
            return 0;
        for (uint32_t slot = 0; slot < kLineup; ++slot)
        {
            CompetitionEntrant &entrant = entrantIn(slot);
            ee::Float *roundScore = &entrant.roundScores[0] + round;
            if ((*g_competitionLineup)[slot] == (player->skaterIndex & 0x1Fu))
            {
                const ee::Float judged = judgePlayerRun(player, scale, ee::Ptr<ee::Float>(*g_competitionPlayerScores));
                *roundScore = judged;
                *g_competitionPlayerScore = judged;
            }
            else
            {
                const ee::Float spread = entrant.scoreSpread[scale];
                *roundScore = entrant.baseScore[scale] + spread * competitionRandom();
            }
            const ee::Float *scores = entrant.roundScores;
            if (round == 0)
            {
                entrant.total = *roundScore;
            }
            else if (round == 1)
            {
                entrant.total = (scores[0] + scores[1]) / ee::Float(2.0f);
            }
            else if (round == 2)
            {
                ee::Float kept;
                if (scores[0] < scores[1] && scores[0] < scores[2])
                {
                    kept = scores[1] + scores[2];
                    entrant.droppedRound = 0;
                }
                else if (scores[1] < scores[0] && scores[1] < scores[2])
                {
                    kept = scores[0] + scores[2];
                    entrant.droppedRound = 1;
                }
                else
                {
                    kept = scores[0] + scores[1];
                    entrant.droppedRound = 2;
                }
                entrant.total = kept / ee::Float(2.0f);
            }
        }
        for (uint32_t i = 0; i < kLineup - 1; ++i)
            for (uint32_t j = i + 1; j < kLineup; ++j)
                if (entrantIn(i).total < entrantIn(j).total)
                {
                    const uint8_t better = (*g_competitionLineup)[j];
                    (*g_competitionLineup)[j] = (*g_competitionLineup)[i];
                    (*g_competitionLineup)[i] = better;
                }
        *g_lastScoredCompetitionRound = *g_competitionRound;
        return 0;
    }
    READABLE_FUNCTION(scoreCompetitionRound, Verified);

    // competitionPlayerScores: the judges' scores of the player's last run
    // (also into *out).
    ee::Ptr<ee::Float> competitionPlayerScores(ee::Ptr<ee::Ptr<ee::Float>> out)
    {
        *out = ee::Ptr<ee::Float>(*g_competitionPlayerScores);
        return ee::Ptr<ee::Float>(*g_competitionPlayerScores);
    }
    READABLE_FUNCTION(competitionPlayerScores, Verified);

    // competitionPlayerPlace: the player's place in the lineup (the first
    // slot whose entrant is the player), 8 when none.
    uint32_t competitionPlayerPlace()
    {
        for (uint8_t slot = 0; slot < kLineup; ++slot)
            if (entrantIn(slot).isPlayer != 0)
                return slot;
        return kLineup;
    }
    READABLE_FUNCTION(competitionPlayerPlace, Verified);

    // competitionEntrantName: the name of the entrant in a slot.
    ee::Ptr<char> competitionEntrantName(uint32_t slot)
    {
        return ee::Ptr<char>(entrantIn(slot).name);
    }
    READABLE_FUNCTION(competitionEntrantName, Verified);

    // competitionEntrantScore: where the entrant in a slot keeps its score of
    // a round, counted from 1 (round 0 is the entrant's first word).
    ee::Ptr<ee::Float> competitionEntrantScore(uint32_t slot, uint32_t round)
    {
        return ee::Ptr<ee::Float>(&entrantIn(slot).roundScores[0]) + (static_cast<int32_t>(round) - 1);
    }
    READABLE_FUNCTION(competitionEntrantScore, Verified);

    // competitionEntrantOffset: the byte offset of the entrant in a slot in
    // g_competitionEntrants.
    uint32_t competitionEntrantOffset(uint32_t slot)
    {
        return (*g_competitionLineup)[slot] * sizeof(CompetitionEntrant);
    }
    READABLE_FUNCTION(competitionEntrantOffset, Verified);

    // competitionRound: the round the competition is in (0 to 2).
    uint32_t competitionRound()
    {
        return *g_competitionRound;
    }
    READABLE_FUNCTION(competitionRound, Verified);

    // nextCompetitionRound: the competition on to its next round; returns that round.
    int32_t nextCompetitionRound()
    {
        const int32_t next = *g_competitionRound + 1;
        *g_competitionRound = next;
        return next;
    }
    READABLE_FUNCTION(nextCompetitionRound, Verified);
}
