#pragma once

// Two game pointers compared by the EE address they hold, as the game's own
// compare does: an address and its uncached mirror differ here. `==` on two
// ee::Ptrs compares where they land in host memory instead (the conversion to
// a host pointer), which is not the game's question.

#include "ee/ee.h"

namespace ee
{
    // Whether two pointers hold the same address.
    template <typename T>
    inline bool same(Ptr<T> a, Ptr<T> b) { return a.address == b.address; }
    // Whether a comes before b in memory, as the game's unsigned compare has it.
    template <typename T>
    inline bool before(Ptr<T> a, Ptr<T> b) { return a.address < b.address; }
}
