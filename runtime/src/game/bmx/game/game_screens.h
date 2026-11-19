#pragma once
// game.c's own game screens (overlay/screens.h's kScreenAfterGame ..
// kScreenAfterTurn; game_screens.cpp): the multiplayer screens between games
// and turns. BMX XXX's own code.

#include "bmx/overlay/screens.h"
#include "ee/ee.h"

#include <cstdint>

namespace game::bmx
{
    // kScreenAfterGame ("FrameMPGameSelect"): after a multiplayer game, the
    // next game's mode picked (SKILLZ, PAINTBALL, STRIP CHALLENGE) or the
    // level left; the menu's line shows its mode's description.
    void mpGameSelectScreenMessage(int32_t message);
    // kScreenBeforeGame (the keyboard frame): B-M-X's word typed before its
    // first turn (up to 8 letters; an empty one is refused with the menu's
    // blocked sound); cancelled, the game select comes back.
    void horseWordScreenMessage(int32_t message);
    // kScreenNextPlayer ("FrameMPReady"): before a multiplayer turn, the
    // next player's number, the score to beat and the all-time best (B-M-X:
    // the instructions or the line to match), then continue or exit.
    void mpReadyScreenMessage(int32_t message);
    // kScreenAfterTurn ("FrameMPTimeUp"): after a multiplayer turn, its
    // score and the records (HIGH FIVE: the round's scores and the series;
    // B-M-X: the letters), then continue or exit.
    void mpTimeUpScreenMessage(int32_t message);
    // The menu's lines: the three modes' names and the exit (inferred name).
    void listMpGameModes(ee::Ptr<zge::zgui::Node> menu);
}
