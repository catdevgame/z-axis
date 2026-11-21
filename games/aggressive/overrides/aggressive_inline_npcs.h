#pragma once

// Aggressive Inline (SLUS-20327): the NPC constructor's first hide, known for
// the overrides that keep it from happening (aggressive_inline_estate.cpp:
// Estate's lesson givers; aggressive_inline_zoo.cpp: the Zoo's kelp, sharks
// and willows, as the Xbox keeps them).
//
// The constructor (func_1E3500, the readable initNpcObject in
// games/aggressive/pickups.cpp) hides a new NPC's object outside the
// career unless its type is scripted (setSceneObjectShown(object, 0)), and
// again, later, an NPC of the invisible type. The constructor is wrapped (its
// object, while it runs), so the hide is known by running inside it, for its
// object, with shown 0, before the NPC record has its invisible flag; never
// by the call's return address (a readable caller gives none for a callee it
// calls from two places).

#include <cstdint>

class PS2Machine;
struct R5900Context;

namespace aggressive_inline::npcs
{
    // The constructor wrapped, once (every override that asks shares it).
    bool installNpcStart(PS2Machine &runtime);
    // This setSceneObjectShown call (its registers) is the constructor's
    // first hide of its NPC.
    bool isNpcStartHide(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
