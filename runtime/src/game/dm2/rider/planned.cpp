// Dave Mirra BMX 2's rider functions to be ported: registered in DM2 (ee/game_id.h) for recording only,
// so ZX_FNREC records their calls in DM2 runs and zx-fncheck replays them
// against their traced originals (built from DM2's namespaced generated
// code, game.cmake). Names from the rider map (tools/ridermap.py; the work
// database's func_map).
//
// AI's registrations never install in DM2, nor DM2's in AI.

#include "ee/readable.h"

namespace game::dm2::rider
{
    // Round 1's 33 leaves (MIXING 14.4) are readable: input.cpp, tricks.cpp,
    // physics_state.cpp, contact.cpp, rider_state.cpp. Round 2's 77 (the
    // collision and surface layer, MIXING 14.7) are in collision/.
    // Round 0's proof (MIXING 14.4) beyond the leaves is readable too: the
    // rider's frame (actions/frame.cpp; record it with ZX_FNREC_ATOMIC=1)
    // and the wheels' ground probe (physics/ground.cpp).
}
