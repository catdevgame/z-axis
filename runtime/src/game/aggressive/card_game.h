#pragma once

// (declared for its callers in other files)

#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive::skater { struct CardHand; }

namespace aggressive
{
    // (card_game.cpp)
    void collectCard(ee::Ptr<aggressive::skater::CardHand> hand, uint32_t card);
    uint32_t drawRandomCard();
    void endCardGame();
    void resetCards(ee::Ptr<aggressive::skater::CardHand> hand);
    void startCardGame(uint32_t seed);
}
