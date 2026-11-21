#pragma once

// Aggressive Inline (SLUS-20327): the bottom screen's snapshot from AI's RAM
// (aggressive_inline_panel.cpp; the snapshot is runtime/panel_snapshot.h).

#include "runtime/panel_snapshot.h"

#include <cstddef>
#include <cstdint>

namespace aggressive_inline::panel_state
{
    // One snapshot from AI's main RAM (ramBytes, 32 MB). `servedLevel`: the
    // level the host serves in place of the game's folder ("ESTATE"), or
    // nullptr: the snapshot's levelFile is that, else the game's folder.
    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out, const char *servedLevel);
    // The same with the served level asked of what install was given (none:
    // the game's folder): the capture the panel calls.
    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out);
    // Installs the capture as the game's (panel::setGameCapture);
    // `servedLevel` answers which level the host serves, or nullptr for none.
    void install(const char *(*servedLevel)());
}
