#pragma once

class PS2Machine;

namespace aggressive_inline
{
    // The Xbox release's gameplay balance (ZX_TUNING=xbox): experience
    // for special tricks instead of key loot, Civic Center's 350,000 point
    // trick at exactly 350,000. Nothing is hooked otherwise.
    void installXboxTuning(PS2Machine &runtime);
}
