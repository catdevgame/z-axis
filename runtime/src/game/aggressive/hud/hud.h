#pragma once

// The in-level HUD (assets\ui's InGameUI and MultiplayerUI scripts): the
// overlay the level shares (the message line, the crash message, the XP
// rewards, the split-screen points and the timer) and each skater's own
// widgets (the score counter, the trick list, the two balance meters). The
// widgets are zgui nodes the HUD script built; the game finds them by path
// once and sets their properties and plays their animations by name.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>

namespace zge::zgui
{
    struct Node;
}

namespace aggressive
{
    struct GameObject; // (objects.h)
}

namespace aggressive::skater
{
    struct Player; // (skater/skater.h)
}

namespace aggressive::hud
{
    // A skater's own widgets (Player::hud: a heap block of 24 bytes,
    // setupSkaterHud). In split screen each player has a pane of his own
    // (MultiplayerUI.Overlay.PlayerOne / PlayerTwo); alone, InGameUI.Overlay.
    struct SkaterHud
    {
        ee::Ptr<zge::zgui::Node> score;           // +0x00 "Score": the score counter
        ee::Ptr<zge::zgui::Node> potentialScore;  // +0x04 "TrickList.PotentialScore": the line's trick score
        ee::Ptr<zge::zgui::Node> grindMeter;      // +0x08 "HorzBalanceMeter": the grind balance
        ee::Ptr<zge::zgui::Node> balanceMeter;    // +0x0C "VertBalanceMeter": the manual, stall and double grind balance
        ee::Ptr<zge::zgui::Node> trickList;       // +0x10 "TrickList": the spin and the tricks of the line
        ee::Ptr<zge::zgui::Node> completedTricks; // +0x14 "TrickList.CompletedTrickBuffer": the line's trick names
    };
    static_assert(sizeof(SkaterHud) == 0x18 && offsetof(SkaterHud, completedTricks) == 0x14);

    // A message waiting for the message line (showMessage): a block of the
    // queue's pool (0x80 bytes) and the timer that shows it.
    struct MessageSlot
    {
        uint32_t timer;    // +0x00 the timer that shows it
        ee::Float seconds; // +0x04 how long it stays
        char text[0x78];   // +0x08 (0x77 copied, then a 0 at +0x7F)
    };
    static_assert(sizeof(MessageSlot) == 0x80 && offsetof(MessageSlot, text) == 0x08);

    // showMessage: shows `text` on the HUD's message line for `seconds`
    // (at least half a second), after a random delay of up to a second.
    void showMessage(ee::Address text, ee::Float seconds);
    // setTrickText: the trick score widget (SkaterHud::potentialScore) shows `text`.
    void setTrickText(ee::Ptr<SkaterHud> hud, ee::Address text);
    // showScore: the score counter (Player::hud +0) shows `score`:
    // animated (its SetScore animation) when `animate`, else at once, with
    // its background sized to the text.
    void showScore(ee::Ptr<skater::Player> player, int32_t score, int32_t animate);

    // ---- the overlay (overlay.cpp) --------------------------------------

    // setupOverlay: finds the overlay's widgets, registers its two script
    // functions, names the game mode and makes the message line's queue.
    int32_t setupOverlay();
    // showOverlayText: the message line (the "Objective" widget) shows `text`:
    // faded in and out after `seconds`; with `holdAndStop` it stays and
    // play stops (the level's stop reason 0xC, when it is still running).
    int32_t showOverlayText(ee::Address text, uint32_t holdAndStop, ee::Float seconds);
    // showMessageIfFree: shows `text` for `seconds` when the message line is
    // free; whether it did.
    int32_t showMessageIfFree(ee::Address text, ee::Float seconds);
    // showQueuedMessage: the message timer's callback (timer, expired?, slot).
    void showQueuedMessage(uint32_t timer, uint32_t interrupted, uint32_t slot);
    // releaseMessageSlot: a message slot's timer released and the slot back to
    // the queue's pool.
    int32_t releaseMessageSlot(uint32_t slot);
    // showCrashMessage: the crash message (not in split screen).
    int32_t showCrashMessage(ee::Address text, ee::Float seconds);
    // showXpReward: an XP reward ("Jump XP +25") queued on the XP indicator.
    int32_t showXpReward(ee::Ptr<skater::Player> player, ee::Address text, int32_t points);
    // nextXpLine, the script function inlineOverlayGetNextXPLine: the XP
    // indicator's next waiting line to its label.
    int32_t nextXpLine();
    // formatIntegerWithCommas, the script function
    // inlineOverlayFormatIntegerAsStringWithCommas: (integer variable,
    // text widget).
    int32_t formatIntegerWithCommas(ee::Ptr<ee::Ptr<zge::zgui::Node>> arguments);
    // showPoints: the split-screen points counter (SecondaryScore), as showScore.
    int32_t showPoints(ee::Ptr<skater::Player> player, int32_t points, int32_t animate);
    // showPointsPanel / hidePointsPanel: the points counter shown; set to 0 and hidden.
    int32_t showPointsPanel();
    int32_t hidePointsPanel(ee::Ptr<skater::Player> player);
    // timerFreakOut: the timer's FreakOut animation (the last seconds)
    // played, or stopped when `stop` (a timer's tick hook: its data, then stop).
    int32_t timerFreakOut(uint32_t data, uint32_t stop);

    // ---- a skater's widgets (hud.cpp) -----------------------------------

    // setupSkaterHud / freeSkaterHud: the skater's SkaterHud found and freed.
    void setupSkaterHud(ee::Ptr<skater::Player> player);
    void freeSkaterHud(ee::Ptr<skater::Player> player);
    // setCornerColors: a widget's four corner colours set to *color (stopping
    // at the first it refuses); whether all were.
    int32_t setCornerColors(ee::Ptr<zge::zgui::Node> widget, ee::Address color);
    // objectName: the game object's name into out (size bytes; "" for
    // none), asked of the object by message.
    void objectName(ee::Ptr<GameObject> object, ee::Address out, uint32_t size);
    // setGrindBalance / setBalance: the grind meter and the balance meter
    // show `balance` (-1 .. 1). setGrindSafeDirection / setSafeDirection: their safe
    // direction (the way leaning is safe: green). showGrindMeter ..
    // hideBalanceMeter: shown and hidden.
    int32_t setGrindBalance(ee::Ptr<skater::Player> player, ee::Float balance);
    int32_t setBalance(ee::Ptr<skater::Player> player, ee::Float balance);
    int32_t setGrindSafeDirection(ee::Ptr<skater::Player> player, int32_t direction);
    int32_t setSafeDirection(ee::Ptr<skater::Player> player, int32_t direction);
    int32_t showGrindMeter(ee::Ptr<skater::Player> player);
    int32_t hideGrindMeter(ee::Ptr<skater::Player> player);
    int32_t showBalanceMeter(ee::Ptr<skater::Player> player);
    int32_t hideBalanceMeter(ee::Ptr<skater::Player> player);
    // showSpinText: the trick list shows the spin ("180 CCW  360 CW") and
    // the current trick's name after the line's.
    int32_t showSpinText(ee::Ptr<skater::Player> player);
    // addCompletedTrick: a landed trick's name added to the line's.
    int32_t addCompletedTrick(ee::Ptr<skater::Player> player, ee::Ptr<GameObject> trick);
    // clearTrickList: the line's trick names cleared, the trick list faded out.
    int32_t clearTrickList(ee::Ptr<skater::Player> player);
    // resetTrickList: the same at once (its fade stopped).
    void resetTrickList(ee::Ptr<skater::Player> player);
    // formatScore: `score` as text with digit groups ("12,345") into out.
    int32_t formatScore(int32_t score, ee::Address out);
}

namespace aggressive::hud
{
    // (overlay.cpp)
    int32_t freeHudMessages();
}
