// BMX XXX's own spawnNamedEffect (ZGE4's; ZGE3's is zge/scene/particle_emitter.cpp): the named effect classes
// (zge/scene/effect_classes.cpp's registry) walked inline, with no locals in a frame, and the first class whose
// name hashes as asked spawning.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/array.h"
#include "zge/scene/effects.h"

#include <cstdint>
#include "sym/bmx/fx/named_effects.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The named effect classes (a pointer array of zge::NamedEffectClass).
        inline ee::Global<ee::Ptr<zge::PointerArray>, sym::g_namedEffectClasses> g_namedEffectClasses;
        // What the walk leaves as the result when there are no classes at all: its cursor's -1.
        constexpr uint32_t kNoEffectClasses = ~0u;
    }

    // spawnNamedEffectByWalk: BMX XXX's spawnNamedEffect (the effect spawners:
    // the user effects, the rider's trails): the named class's spawn method
    // called with (a, b); what it did, 0 when no class has that name, and -1
    // when there are no classes at all.
    uint32_t spawnNamedEffectByWalk(uint32_t nameHash, ee::Address a, ee::Address b)
    {
        zge::PointerArray &classes = **g_namedEffectClasses;
        for (uint32_t item = zge::walkFirst(classes); item != 0; zge::walkNext(classes, item))
        {
            const ee::Ptr<zge::NamedEffectClass> effectClass(item);
            if (effectClass->nameHash == nameHash)
                return effectClass->spawn(a, b);
        }
        return classes.count == 0 ? kNoEffectClasses : 0;
    }
    READABLE_FUNCTION(spawnNamedEffectByWalk, Verified);
}
