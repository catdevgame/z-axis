#pragma once

#include <cstdint>

// ZX_PLAY_LINK=<dir>: a player program outside the game (tools/playbot in
// the main repo) plays in lock step. At every port-0 pad read the game stops
// and hands over: the player reads what it wants of guest memory, answers
// with the pad for this read, and the game goes on. The game cannot run ahead
// of the player, so a run only depends on the answers, not on host speed;
// the answers written as a ZX_PAD_SCRIPT replay the run tick for tick.
//
// <dir> holds two FIFOs the player made: `up` (game to player), `down`
// (player to game). All numbers little endian.
//   game:    'T', u64 vsync tick, i64 script time in vsyncs, u32 reads so far
//   player:  'R', u32 address, u32 bytes     -> the bytes, on up
//            'G', u16 buttons (PAD_ bits, added to the rest), u8 axes mask
//                 (1 RX, 2 RY, 4 LX, 8 LY), u8 RX, RY, LX, LY,
//                 u32 further reads to run with this same answer
// When the player goes away (its end of `down` closes) the game exits: a
// run driven by a player cannot outlive it.
// Not built on Windows (no FIFOs): enabled() is false there.
namespace ps2_play_link
{
    bool enabled();
    // The pad read, port 0, after the pad script and ZX_CONTROL.
    // `sticks`: RX, RY, LX, LY. Adds to `pressed`, and to `pressure` (the
    // 16 buttons' pressures by button bit) at full pressure as a script does.
    void onPadRead(const uint8_t *rdram, uint64_t tick, int64_t scriptTime, uint16_t &pressed, uint8_t *sticks,
                   uint8_t *pressure);
    // A port-1 read with ZX_PAD_PORT1=mirror: the last answer on it too, as the
    // session's pad script (which reaches every port then) gives it in a replay.
    void onMirroredRead(uint16_t &pressed, uint8_t *sticks, uint8_t *pressure);
    // A child forked at a spot (runtime/spot_fork.h): the parent's link
    // closed, and the child's own ZX_PLAY_LINK opened at its next pad read.
    void relink();
}
