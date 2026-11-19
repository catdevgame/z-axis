#pragma once

// Trigger zones (inlineFx.c): boxes in the level, made from the level's
// locators of kind kZoneLocator (level/zones.cpp), that tell their scene
// object when a skater comes in and goes out (skater/zones.cpp). A zone is
// found through a coarse grid over the level.

#include "ee/ee.h"
#include "zge/math/types.h"

#include "zge/math/box.h"
#include "aggressive/skater/skater.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/zones.h.h"

namespace aggressive::level
{
    // A zone: its id, the box, and what it tells.
    struct Zone
    {
        uint32_t id;           // +0x00 the locator's name (sent with the events)
        uint8_t _04[0x0C];
        zge::Box box;          // +0x10 boxContainsPoint's box: low, high (inclusive)
        zge::Matrix turn;      // +0x30 the locator's orientation
        uint32_t target;       // +0x70 the object told (a name for findObjects), or the prop's id
        // +0x74 set: entering goes to zoneEnterHandler(target, player)
        // instead of an event (leaving is an event either way).
        uint32_t enterHandler;
        uint32_t next;         // +0x78
        uint32_t _7c;
    };
    static_assert(sizeof(Zone) == 0x80 && offsetof(Zone, box) == 0x10 && offsetof(Zone, target) == 0x70 && offsetof(Zone, next) == 0x78);

    // A grid cell's list of the zones over it.
    struct ZoneListNode
    {
        uint32_t zone;
        uint32_t next;
    };

    // The grid: cells of a ZoneListNode list, over x and z from the level's
    // minimum (g_zoneGridMinX, g_zoneGridMinZ), cell sizes in g_zoneCellWidth and
    // g_zoneCellDepth. Five cells a side as initZones clears it and freeLevelZones frees it; a
    // skater or a box at the level's far edge indexes a sixth (rows 5 words
    // apart, as the original indexes them), past its end.
    inline ee::Global<ee::Float, sym::g_zoneGridMinX> g_zoneGridMinX;
    inline ee::Global<ee::Float, sym::g_zoneGridMinZ> g_zoneGridMinZ;
    inline ee::Global<ee::Float, sym::g_zoneCellWidth> g_zoneCellWidth;
    inline ee::Global<ee::Float, sym::g_zoneCellHeight> g_zoneCellHeight;
    inline ee::Global<ee::Float, sym::g_zoneCellDepth> g_zoneCellDepth;
    constexpr uint32_t kZoneGrid = sym::zoneGrid;
    constexpr uint32_t kZoneGridRowWords = 5;
    constexpr uint32_t kZoneGridCells = 6;

    // The events' name hashes ("boxEnter", "boxExit").
    inline ee::Global<uint32_t, sym::g_enterEvent> g_enterEvent;
    inline ee::Global<uint32_t, sym::g_exitEvent> g_exitEvent;
}

namespace aggressive::level
{
    // (zones.cpp)
    void freeLevelZones();
    void initZones();
    void zoneEnterHandler(uint32_t id, ee::Ptr<skater::Player> player);
}
