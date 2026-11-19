#pragma once

// BMX XXX's pads as the game keeps them (pads.cpp): the two ports' pads made
// and freed, the pads handed over while something other than the play reads
// them, and each port's pad read into its record once a frame. BMX XXX's own
// code, DM2's line (game/dm2/input/pads.h). The records are the rider's
// PadInput (rider/rider.h); the rider's input code reads them
// (rider/input.cpp: padInput, padButtons, rumble).

#include <cstdint>

namespace game::bmx
{
    // A pad asked of the engine for each port; a port with none connected is
    // marked missing. The pads start out taken.
    void initPads();
    // The ports' pads given back.
    void freePads();
    // The pads taken from the play (not 0) or given back to it (0). Taking
    // them sets the held buttons aside; giving them back restores those, and
    // either way the buttons down at that moment are ignored until let go.
    void takePads(uint32_t taken);
    // Each port's pad read into its record, once a frame. Returns bit 0 when
    // a pad was lost, bit 1 when a pad came back.
    uint32_t readPads();
    // 1 when either port's pad has a button other than a direction newly
    // down this frame.
    int32_t anyButtonPressed();
}
