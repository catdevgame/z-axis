// The level's breakables (level/breakables.h) as the skater's effects see
// them: a walk over the pool, newest first, and each one's state and what
// it shows.

#include "aggressive/level/breakables.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/memory/pool.h"
#include "aggressive/level/park_cells.h"

#include <cstdint>
#include "sym/aggressive/level/breakables.cpp.h"

namespace aggressive::level
{
    namespace
    {
        // setBreakableRecordState, and what it leaves in the result register (setBreakable returns it).
        int32_t setBreakableRecordStateKeepingResult(ee::Ptr<Breakable> breakable, uint32_t state) { return ee::tailCall<int32_t>(sym::setBreakableRecordState, breakable, state); }
    }

    // firstBreakable: the newest breakable, 0 when there are none or no pool.
    ee::Ptr<Breakable> firstBreakable()
    {
        const ee::Ptr<zge::Pool> pool = zge::editor::breakablePool();
        if (!pool)
            return ee::Ptr<Breakable>();
        const ee::Ptr<zge::ListNode> link = zge::firstPoolLink(pool);
        if (!link)
            return ee::Ptr<Breakable>();
        return zge::poolRecordAs<Breakable>(link);
    }
    READABLE_FUNCTION(firstBreakable, Verified);

    // nextBreakable: the one after it in the pool, 0 after the last.
    ee::Ptr<Breakable> nextBreakable(ee::Ptr<Breakable> breakable)
    {
        const ee::Ptr<zge::ListNode> link = zge::nextPoolLink(zge::poolLinkOfRecord(breakable));
        if (!link)
            return ee::Ptr<Breakable>();
        return zge::poolRecordAs<Breakable>(link);
    }
    READABLE_FUNCTION(nextBreakable, Verified);

    // setBreakable: its state (the skater's effects set 0 when it breaks).
    int32_t setBreakable(ee::Ptr<Breakable> breakable, uint32_t state)
    {
        return setBreakableRecordStateKeepingResult(breakable, state);
    }
    READABLE_FUNCTION(setBreakable, Verified);

    // breakableWhole: non-zero while it stands.
    int32_t breakableWhole(ee::Ptr<Breakable> breakable)
    {
        return breakableRecordState(breakable);
    }
    READABLE_FUNCTION(breakableWhole, Verified);

    // breakableObject: what it shows.
    int32_t breakableObject(ee::Ptr<Breakable> breakable)
    {
        return breakableRecordObject(breakable);
    }
    READABLE_FUNCTION(breakableObject, Verified);
}
