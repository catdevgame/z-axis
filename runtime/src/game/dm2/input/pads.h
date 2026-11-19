#pragma once

// DM2's pads as the game keeps them (pads.cpp): the two ports' pads made and
// freed, their button records cleared, the pads handed over while something
// other than the play reads them, and the rumble. The records are the
// rider's PadInput (rider/rider.h); the rider's input code reads them
// (rider/input.cpp).

#include <cstdint>

namespace game::dm2
{
    // A pad asked of the engine for each port; a port with none connected is
    // marked missing. The pads start out taken.
    void initPads();
    // The ports' pads given back.
    void freePads();
    // Every port's buttons and sticks cleared.
    void clearPads();
    // The pads taken from the play (not 0) or given back to it (0). Taking
    // them sets the held buttons aside; giving them back restores those, and
    // either way the buttons down at that moment are ignored until let go.
    void takePads(uint32_t taken);
    // Each port's pad read into its button record, once a frame. Returns
    // bit 0 when a pad was lost, bit 1 when a pad came back.
    uint32_t readPads();
    // A player's pad rumbles: `motor` 0 the small one (any level is full),
    // 1 the big one at `level`. Only in action mode 0 (the play), for a player who has vibration
    // on, and only upwards: a weaker rumble never replaces a stronger one.
    void rumble(uint32_t player, uint32_t motor, uint32_t level);
}
