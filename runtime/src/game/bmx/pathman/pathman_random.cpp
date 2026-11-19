// BMX XXX's path men (pathman_random.h): each one's own random numbers. A
// path man keeps the state of its generator in its tag record, so two of them
// started from different states stay out of step. BMX XXX's own code.

#include "pathman_random.h"

#include "bmx/level/entities.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/math/conversions.h"

#include <cstdint>
#include "sym/bmx/pathman/pathman_random.cpp.h"

namespace game::bmx
{
    using namespace zge;

    namespace
    {
        constexpr ee::Float kHalf = 0.5f;
    }

    // pathManSeed
    int32_t pathManSeed(ee::Ptr<SceneObject> pathMan)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        return tagWord(tags, kPathManTagWords, kPathManTagSeed);
    }
    READABLE_FUNCTION(pathManSeed, Verified);

    // setPathManSeed
    void setPathManSeed(ee::Ptr<SceneObject> pathMan, ee::Address seed)
    {
        const ee::Ptr<TagRecord> tags = objectComponent(pathMan);
        game::tail(setTagWord, tags, kPathManTagWords, kPathManTagSeed, seed);
    }
    READABLE_FUNCTION(setPathManSeed, Verified);

    // pathManRandom: the generator stepped from the path man's state, the
    // new state kept, and the fraction scaled to the range and rounded.
    int32_t pathManRandom(ee::Ptr<SceneObject> pathMan, uint32_t range)
    {
        ee::Local<uint32_t> state;
        state = static_cast<uint32_t>(pathManSeed(pathMan));
        const ee::Float fraction = seededRandom(state);
        setPathManSeed(pathMan, *state);
        const ee::Float scaled = fraction * unsignedToFloat(range);
        if (scaled < 0.0f)
            return ee::toInt(scaled - kHalf);
        return ee::toInt(scaled + kHalf);
    }
    READABLE_FUNCTION(pathManRandom, Verified);
}
