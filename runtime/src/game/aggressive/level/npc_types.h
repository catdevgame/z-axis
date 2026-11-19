#pragma once

// The NPC type table (npc_types.cpp; the types are npc.h's).

#include "aggressive/npc.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace aggressive
{
    // (npc_types.cpp)
    ee::Ptr<NpcType> findNpcType(uint32_t key);
    int32_t setupNpcTypeAnimations();
}
