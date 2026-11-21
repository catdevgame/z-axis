// The level's view regions (the Region record: zge/render/view_regions.h; the PVS
// block of the engine, DM2 and BMX XXX have it too): a region made for each
// PVSO chunk and freed with the level, its list of regions out of view
// resolved once the level is loaded, the region a point is in, and the
// marks the camera's region puts on what it cannot see.

#include "zge/render/view_regions.h"

#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/scene/visibility_regions.h"
#include "zge/scene/node.h"
#include "zge/debug/debug.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/scene/visibility_regions.cpp.h"

namespace zge
{
    namespace
    {
        inline ee::Global<ee::Ptr<Pool>, sym::g_regions> g_regions;
        inline ee::Global<int32_t, sym::g_regionsReady> g_regionsReady;
        // Bumped each time a region marks its list: a region whose
        // outOfViewMark equals it is out of view.
        inline ee::Global<uint32_t, sym::g_outOfViewMark> g_outOfViewMark;

        // reportRegions' colours: a region in view, one out of view.
        constexpr uint32_t kInViewColour = sym::g_regionNameColour;
        constexpr uint32_t kOutOfViewColour = sym::g_regionBoxColour;

        namespace tail
        {
        int32_t poolFree(ee::Ptr<Pool> pool, ee::Ptr<Region> region) { return ee::tailCall<int32_t>(sym::poolFree, pool, region); }
        }
    }

    // newRegion (readPvsoChunk): a cleared region from the pool, with room for
    // hiddenCount entries in its list. 0 when the pool has none.
    ee::Ptr<Region> newRegion(uint32_t hiddenCount)
    {
        const ee::Ptr<Region> region(poolAlloc(*g_regions));
        if (!region)
            return region;
        memoryFill(region, 0, sizeof(Region));
        if (hiddenCount == 0)
            return region;
        region->hiddenCount = static_cast<decltype(region->hiddenCount)>(hiddenCount); // a halfword in ZGE3, a word in ZGE2
        region->hidden = ee::Ptr<uint32_t>(heapAllocate(hiddenCount << 2));
        return region;
    }
    READABLE_FUNCTION_WAITS(newRegion, Verified);

    // freeRegion: its list freed and the region given back to the pool.
    int32_t freeRegion(ee::Ptr<Region> region)
    {
        if (region->hidden)
            heapFree(region->hidden);
        return tail::poolFree(*g_regions, region); // the original's tail call
    }
    READABLE_FUNCTION_WAITS(freeRegion, Verified);

    // shutdownRegions: the region pool destroyed, once there is one.
    uint32_t shutdownRegions()
    {
        if (*g_regionsReady == 0)
            return 1;
        destroyPool(*g_regions);
        *g_regions = ee::Ptr<Pool>();
        *g_regionsReady = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownRegions, Verified);

    // freeAllRegions: every region freed, the first each time, until the
    // pool has none. Returns 1.
    uint32_t freeAllRegions()
    {
        while (const ee::Ptr<ListNode> link = firstPoolLink(*g_regions))
            freeRegion(poolRecordAs<Region>(link));
        return 1;
    }
    READABLE_FUNCTION(freeAllRegions, Verified);

    // finishRegions (finishCollisionWorld): each region's list of mesh hashes
    // made a list of those meshes' regions (0 for a mesh with none), last
    // first. Returns 1.
    uint32_t finishRegions()
    {
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_regions); link; link = nextPoolLink(link))
        {
            const ee::Ptr<Region> region = poolRecordAs<Region>(link);
            for (int32_t i = region->hiddenCount - 1; i != -1; --i)
                region->hidden[i] = ee::Address(findRegionForMesh(region->hidden[i])); // the hash's word holds the region from now on
        }
        return 1;
    }
    READABLE_FUNCTION(finishRegions, Verified);

    // findRegionAt (markRegionsOutOfView): the first region whose box holds the
    // point, or 0. ZGE2 is given a box, and asks for the first
    // region whose box holds it (boxContainsBox).
    ee::Ptr<Region> findRegionAt(ee::Ptr<const Vec3> position)
    {
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_regions); link; link = nextPoolLink(link))
        {
            const ee::Ptr<Region> region = poolRecordAs<Region>(link);
            if constexpr (game::readable::kIsDm2)
            {
                if (boxContainsBox(ee::Ptr<Box>(&region->box), boxGivenAsPoint(position)))
                    return region;
            }
            else if (boxContainsPoint(&region->box, position))
                return region;
        }
        return ee::Ptr<Region>();
    }
    READABLE_FUNCTION(findRegionAt, Verified);

    // findRegionForMesh: the region that is the mesh with this hash (a
    // node's or a region list's, as the level's files give them), or 0.
    ee::Ptr<Region> findRegionForMesh(uint32_t meshHash)
    {
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_regions); link; link = nextPoolLink(link))
        {
            const ee::Ptr<Region> region = poolRecordAs<Region>(link);
            if (region->mesh == meshHash)
                return region;
        }
        return ee::Ptr<Region>();
    }
    READABLE_FUNCTION(findRegionForMesh, Verified);

    namespace
    {
        // The region a point is in: ZGE2 asks its findRegion, the one that tests a point (ZGE3's findRegionAt;
        // ZGE2's findRegionAt tests a box).
        ee::Ptr<Region> regionHoldingPoint(ee::Ptr<const Vec3> position)
        {
            if constexpr (game::readable::kIsDm2)
                return ee::Ptr<Region>(findRegion(boxGivenAsPoint(position)));
            else
                return findRegionAt(position);
        }
    }

    // markRegionsOutOfView (drawScene): a new mark, put on each region out of
    // view from the one the point is in. That region, or 0 for a point in
    // none (then nothing is out of view).
    ee::Ptr<Region> markRegionsOutOfView(ee::Ptr<const Vec3> position)
    {
        *g_outOfViewMark = *g_outOfViewMark + 1;
        const ee::Ptr<Region> here = regionHoldingPoint(position);
        if (!here)
            return here;
        const uint32_t mark = *g_outOfViewMark;
        for (int32_t i = here->hiddenCount - 1; i != -1; --i)
        {
            const ee::Ptr<Region> hidden(here->hidden[i]);
            if (hidden)
                hidden->outOfViewMark = mark;
        }
        return here;
    }
    READABLE_FUNCTION(markRegionsOutOfView, Verified);

    // reportRegions (no caller in this build): a debug marker on the regions'
    // boxes: with no region given, on every one in view's colour; else, seen
    // from the given region's centre (marked again for each region), on those
    // in view (inView non-zero) or on those out of view.
    void reportRegions(ee::Ptr<Region> from, uint32_t inView)
    {
        if (!from)
        {
            for (ee::Ptr<ListNode> link = firstPoolLink(*g_regions); link; link = nextPoolLink(link))
                addDebugText(&poolRecordAs<Region>(link)->box, 0, kInViewColour, 0);
            return;
        }
        ee::Local<Vec3> centre;
        for (ee::Ptr<ListNode> link = firstPoolLink(*g_regions); link; link = nextPoolLink(link))
        {
            const ee::Ptr<Region> here = markRegionsOutOfView(boxCentre(ee::Ptr<Vec3>(centre.address()), &from->box));
            const ee::Ptr<Region> region = poolRecordAs<Region>(link);
            const bool shown = !here || !region || region->outOfViewMark != *g_outOfViewMark;
            if (inView != 0 ? shown : !shown)
                addDebugText(&region->box, 0, inView != 0 ? kInViewColour : kOutOfViewColour, 0);
        }
    }
    READABLE_FUNCTION_WAITS(reportRegions, Draft);
}
