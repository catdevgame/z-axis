// The card levels' game (were the sweep's startCardGame .. drawRandomCard):
// a level's props deal cards (drawRandomCard: 52 cards, each dealt once
// until all have been), the skater who collects one adds it to a hand of
// five scored as blackjack (collectCard: aces 1 or 11, faces 10; over 21
// the hand is lost), and the hand shows on the multiplayer overlay's card
// widgets (Card1P1 .. Card5P1 for the first player's, Card1P2 .. Card5P2
// for the second's).

#include "aggressive/globals.h"
#include "aggressive/skater/skater.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "sym/aggressive/card_game.cpp.h"

namespace aggressive
{
    namespace
    {
        using zgui::Node;

        // The overlay's card widgets ("MultiplayerUI.OverlayToggle.Cards").
        constexpr uint32_t kCardsOverlay = sym::g_textMultiplayerUIOverlayToggle;
        constexpr uint32_t kHandSize = 5;
        constexpr uint32_t kFirstPlayerCards[kHandSize] = {sym::g_textCard1P1, sym::g_textCard2P1, sym::g_textCard3P1,
                                                           sym::g_textCard4P1, sym::g_textCard5P1};
        constexpr uint32_t kSecondPlayerCards[kHandSize] = {sym::g_textCard1P2, sym::g_textCard2P2, sym::g_textCard3P2,
                                                            sym::g_textCard4P2, sym::g_textCard5P2};
        constexpr ee::Ptr<const char> kVisible{sym::g_textVisible}; // "Visible"
        constexpr uint32_t kValue = sym::g_textValue;     // "Value": a card widget's text child
        constexpr ee::Ptr<const char> kText{sym::g_textText3};      // "Text"
        // The card's colour: hearts and diamonds red, the others black.
        constexpr uint32_t kRedValue = sym::g_textSetValueColor;    // "SET Value.Color {128 0 0 255}"
        constexpr uint32_t kBlackValue = sym::g_textSetValueColor2; // "SET Value.Color {0 0 0 255}"
        constexpr uint64_t kWidgetKind = 1; // findNode's kind for a widget, as the UI's other callers ask

        // A card is its rank (0 the ace .. 12 the king) times 4 plus its suit.
        constexpr uint32_t kDeckSize = 52;
        constexpr uint32_t kSuitMask = 0x3;
        constexpr uint32_t kRankShift = 2;
        constexpr uint32_t kBlackjack = 21;

        inline ee::Global<int32_t, sym::g_cardRandomSeed> g_cardRandomSeed;
        inline ee::Global<uint8_t, sym::g_cardsDealt> g_cardsDealt;     // cards dealt (a byte, never reset)
        inline ee::Global<uint64_t, sym::g_cardsDrawn> g_cardsDrawn;    // a bit per card dealt
        inline ee::Global<uint8_t, sym::g_cardGameActive> g_cardGameActive;

        ee::Ptr<Node> findWidget(ee::Ptr<Node> parent, uint32_t name)
        {
            return zgui::findNode(parent, ee::Ptr<const char>(name), kWidgetKind);
        }

        ee::Ptr<Node> cardsOverlay()
        {
            return findWidget(*zgui::g_root, kCardsOverlay);
        }

        // The hand's card widgets: the first player's or the second's.
        const uint32_t (&cardWidgets(ee::Ptr<aggressive::skater::CardHand> hand))[kHandSize]
        {
            return hand->widgets == 0 ? kFirstPlayerCards : kSecondPlayerCards;
        }
    }

    namespace
    {
        // The rank's names, the line after it and the suits: the font draws
        // "<", ">", "\\" and "|" as the four suits' symbols.
        constexpr uint32_t kRedCardColour = sym::g_textRgba2;   // "#RGBA{128 0 0 255}"
        constexpr uint32_t kBlackCardColour = sym::g_textRgba3; // "#RGBA{0 0 0 255}"
        constexpr uint32_t kAce = sym::g_textAce, kJack = sym::g_textJack, kQueen = sym::g_textQueen, kKing = sym::g_textKing;
        constexpr uint32_t kRankNumberFormat = sym::g_cardNumberFormat; // "%d"
        constexpr uint32_t kLineEnd = sym::g_textCardLineEnd;           // "\n"
        constexpr uint32_t kSuitGlyphs[4] = {sym::g_textSuit0Glyph, sym::g_textSuit1Glyph, sym::g_textSuit2Glyph, sym::g_textSuit3Glyph};
        constexpr uint32_t kCardSeparator = sym::g_textCardSeparator;   // " "
        constexpr uint32_t kAceRank = 0, kJackRank = 10, kQueenRank = 11, kKingRank = 12;
    }

    // cardText: the card's name appended to text: its rank ("A", "2" ..
    // "10", "J", "Q", "K"), a line end and its suit's symbol. The colour code
    // for its suit (red for 1 and 2) is appended first and cut off at once
    // (text emptied): the name has no colour, collectCard colours the widget
    // by script instead. That first append is onto whatever text holds (the
    // callers' buffers are not cleared).
    void cardText(uint32_t card, ee::Ptr<char> text)
    {
        ee::Local<char[16]> number;
        const uint32_t suit = card & kSuitMask;
        const uint32_t rank = card >> kRankShift;
        zge::appendString(text, suit - 1 < 2 ? kRedCardColour : kBlackCardColour);
        *text = 0;
        if (rank == kAceRank)
            zge::appendString(text, kAce);
        else if (rank == kJackRank)
            zge::appendString(text, kJack);
        else if (rank == kQueenRank)
            zge::appendString(text, kQueen);
        else if (rank == kKingRank)
            zge::appendString(text, kKing);
        else
        {
            zge::formatString(number, kRankNumberFormat, rank + 1);
            zge::appendString(text, number);
        }
        zge::appendString(text, kLineEnd);
        zge::appendString(text, kSuitGlyphs[suit]);
    }
    READABLE_FUNCTION(cardText, Verified);

    // cardTextList: the hand's cards' names into text, each followed by a
    // space. No caller in AI.
    void cardTextList(ee::Ptr<aggressive::skater::CardHand> hand, ee::Ptr<char> text)
    {
        ee::Local<char[0x20]> name;
        *text = 0;
        for (uint8_t i = 0; i < hand->count; ++i)
        {
            cardText(hand->cards[i], name);
            zge::appendString(text, name);
            zge::appendString(text, kCardSeparator);
        }
    }
    READABLE_FUNCTION(cardTextList, Verified);

    // startCardGame: the deck shuffled from `seed` (the collision clock), no
    // card dealt yet, the card widgets shown.
    void startCardGame(uint32_t seed)
    {
        ee::Local<uint32_t> visible;
        *g_cardRandomSeed = seed;
        *g_cardGameActive = 1;
        *visible = 1;
        *g_cardsDrawn = 0;
        zgui::setVariable(cardsOverlay(), kVisible, visible);
    }
    READABLE_FUNCTION_WAITS(startCardGame, Verified);

    // endCardGame: the card widgets hidden.
    void endCardGame()
    {
        ee::Local<uint32_t> visible;
        *visible = 0;
        *g_cardGameActive = 0;
        zgui::setVariable(cardsOverlay(), kVisible, visible);
    }
    READABLE_FUNCTION_WAITS(endCardGame, Verified);

    // resetCards: the hand thrown in: its five widgets hidden, its cards,
    // its count and its points 0.
    void resetCards(ee::Ptr<aggressive::skater::CardHand> hand)
    {
        ee::Local<uint32_t> visible;
        *visible = 0;
        const ee::Ptr<Node> overlay = cardsOverlay();
        for (const uint32_t name : cardWidgets(hand))
            zgui::setVariable(findWidget(overlay, name), kVisible, visible);
        for (uint32_t i = 0; i < kHandSize; ++i)
            hand->cards[i] = 0;
        hand->points = 0;
        hand->count = 0;
    }
    READABLE_FUNCTION_WAITS(resetCards, Verified);

    // collectCard: a card for the hand unless it holds five already. The
    // hand is scored as blackjack: an ace 11, or 1 where 11 would take the
    // hand over 21; a face card 10, the others their number. Over 21 even
    // so, the hand is lost (resetCards). Else the points are kept and the
    // card shows on its widget (by the hand's count), red for hearts and
    // diamonds (suits 1 and 2).
    void collectCard(ee::Ptr<aggressive::skater::CardHand> hand, uint32_t card)
    {
        ee::Local<char[0x20]> text;
        ee::Local<uint32_t> visible;
        const uint32_t suit = card & kSuitMask;
        *visible = 1;
        const ee::Ptr<Node> overlay = cardsOverlay();
        if (hand->count == kHandSize)
            return;

        hand->cards[hand->count] = card;
        hand->count += 1;
        uint32_t points = 0;
        uint32_t softAces = 0; // aces counted 11
        for (uint32_t i = 0; i < hand->count; ++i)
        {
            const uint32_t rank = hand->cards[i] >> kRankShift;
            points += rank >= 10 ? 10 : rank + 1;
            if (rank == 0)
            {
                points += 10;
                ++softAces;
            }
        }
        while (points > kBlackjack)
        {
            if (softAces == 0)
            {
                resetCards(hand);
                return;
            }
            points -= 10;
            --softAces;
        }
        hand->points = static_cast<uint16_t>(points);

        cardText(card, text);
        // Past the five widgets (a count the hand never reaches) the widget is 0.
        const uint32_t slot = hand->count - 1;
        const ee::Ptr<Node> widget = slot < kHandSize ? findWidget(overlay, cardWidgets(hand)[slot]) : ee::Ptr<Node>();
        zgui::setVariable(widget, kVisible, visible);
        zgui::setVariable(findWidget(widget, kValue), kText, text);
        const bool red = suit - 1 < 2;
        zgui::runScript(widget, ee::Ptr<char>(red ? kRedValue : kBlackValue), 0);
    }
    READABLE_FUNCTION_WAITS(collectCard, Verified);

    // cardRandom: the card game's random number, 0 .. 1, from its seed
    // (which moves on at each call).
    ee::Float cardRandom()
    {
        const int32_t cardRandomSeed = *g_cardRandomSeed;
        const int32_t mixed = (cardRandomSeed << 4 | static_cast<uint32_t>(cardRandomSeed) >> 28) + (static_cast<uint32_t>(cardRandomSeed) >> 0x10 | cardRandomSeed << 16) ^ static_cast<int32_t>(0x87654321u);
        *g_cardRandomSeed = mixed << 16 | static_cast<uint32_t>(mixed) >> 0x10;
        return ee::toFloat(static_cast<int32_t>(mixed & 0xFFFFu)) / ee::Float(65535.0f);
    }
    READABLE_FUNCTION(cardRandom, Verified);

    // drawRandomCard: a card no one has been dealt since the deck was last
    // full: a random one, or the next free after it (wrapping at 52). Once
    // 52 have been dealt the deck is whole again; the count of cards dealt
    // keeps rising, so from then on every deal starts it again.
    uint32_t drawRandomCard()
    {
        uint32_t card = static_cast<uint32_t>(ee::toInt(aggressive::cardRandom() * ee::Float(52.0f)));
        const uint64_t drawn = *g_cardsDrawn;
        const uint8_t dealt = *g_cardsDealt;
        while ((drawn >> (card & 0x3F)) & 1)
            card = card + 1 >= kDeckSize ? 0 : card + 1;
        *g_cardsDrawn = drawn | uint64_t{1} << (card & 0x3F);
        *g_cardsDealt = static_cast<uint8_t>(dealt + 1);
        if (static_cast<uint8_t>(dealt + 1) >= kDeckSize)
            *g_cardsDrawn = 0;
        return card;
    }
    READABLE_FUNCTION(drawRandomCard, Verified);
}
