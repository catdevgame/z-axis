#pragma once

// Practice's restart point for a rider (Dave Mirra BMX 2 and BMX XXX: one
// body, each game's rider record layout and functions; the keys and the
// clock are zge_practice.h's).
//
// What the games can put back. Their respawn (riderReset: the rider shown,
// the input cleared, riding, then the physics reset, resetPhysics) looks for
// a ground face near the rider's position (a spiral of tries out from it,
// a face flat and wide enough, from 4 units below to 2 above) and stands the
// rider on that face's centre, up its normal, forward kept, stopped: the
// wheels' and the body's velocity cleared and the coast phase stopping. The
// game does so after a bail and when a rider leaves the level's bounds. So
// a point is the rider's place and heading, and a return puts the rider,
// stopped, on the nearest ground face to it with that heading: the game's
// own placement, as after a bail at the point. The speed is not put back:
// it is the wheels' (each wheel's velocity, the coast phase, the drag), and
// writing those is no placement the game makes. An open trick line is ended
// first (trickLineEnd, as the level exit's respawn does), and the camera cut
// to the rider (the game's).
//
// Where. Only riding on the ground (physics state ground, action riding).

#include <cstdint>

class PS2Machine;
struct R5900Context;

namespace zge_host::practice
{
    // A game's rider record and functions (each game's override fills it
    // from its symbols).
    struct RiderGame
    {
        const char *log; // the log's name ("dave-mirra")
        // Rider fields (offsets in the record).
        uint32_t rows;          // Rider::right: the rows right, up, forward (then the origin row)
        uint32_t position;      // Rider::position
        uint32_t drawnPosition; // Rider::drawnPosition
        uint32_t physicsState;  // Rider::physicsState (byte; 0 ground)
        uint32_t actionState;   // Rider::actionStates[0] (byte; 0 riding)
        uint32_t lineState;     // Rider::score.lineState (byte; 0 no line)
        // Functions.
        uint32_t riderIndexOf; // (player) -> the rider index
        uint32_t riderOf;      // (index) -> the rider record
        uint32_t riderReset;   // (index, shown)
        uint32_t trickLineEnd; // (player, landed)
        // The camera cut to the player's rider, as the game's own respawn
        // cuts it.
        void (*cutCamera)(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime, uint32_t player, uint32_t sp);
    };

    // At the game's play frame, in play, for `player`: the request taken
    // (zge_practice.h current()) and run; true when the return ran in place
    // of the frame. Otherwise the frame's time goes to the try's clock.
    bool riderPlayFrame(const RiderGame &game, uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime,
                        uint32_t player);
    // Out of play (another level, a turn started again): the point dropped.
    void riderLeft();
}
