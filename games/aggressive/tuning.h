#pragma once

// The developers' tuning table (docs/DEVTOOLS.md: 405 entries in 20
// categories): named floats with a range and a step, read by the game
// whenever it uses them (camera, physics, juice, controls). The debug
// menu's Tuning page edits them live.

#include "ee/ee.h"

#include <cstddef>
#include "sym/aggressive/tuning.h.h"

namespace aggressive
{
    struct TuningEntry
    {
        char name[32];         // +0x00 "Far Clip"
        ee::Float value;       // +0x20
        ee::Float maxValue;    // +0x24
        ee::Float minValue;    // +0x28
        ee::Float step;        // +0x2C the editor's left/right step
    };
    static_assert(sizeof(TuningEntry) == 0x30);

    struct TuningCategory
    {
        ee::Ptr<TuningEntry> entries; // +0x00
        uint32_t count;               // +0x04
        char name[32];                // +0x08 "Camera"
    };
    static_assert(sizeof(TuningCategory) == 0x28);

    // The categories (20 used; a zero entry ends them) and where the
    // entries live.
    inline ee::Global<TuningCategory[32], sym::g_tuning> g_tuningCategories;
    // The categories by index (their names as the ELF's table has them).
    enum TuningCategoryIndex : uint32_t
    {
        kTuningGeneral = 0,
        kTuningFx = 1,
        kTuningCamera = 2,
        kTuningPhysics = 3,
        kTuningSkater = 4,
        kTuningControl = 5,
        kTuningScoringJuice = 6,
        kTuningScoringGrabs = 7,
        kTuningScoringAirTricks = 8,
        kTuningScoringComboTricks = 9,
        kTuningScoringGrinds = 10,
        kTuningScoringStalls = 11,
        kTuningScoringManuals = 12,
        kTuningScoringWallTricks = 13,
        kTuningGrindWalls = 14, // "Grind/Walls"
        kTuningAction = 15,
        kTuningBoobTech = 16,
        kTuningCompetitor = 17,
        kTuningVibrate = 18,
        kTuningMemCheck = 19,
    };
    // Entries of kTuningVibrate: a rumble's two motors and its length.
    enum VibrateEntry : uint32_t
    {
        kVibrateNpcBig = 9,
        kVibrateNpcSmall = 10,
        kVibrateNpcTime = 11,
    };
    // A tuning value by category and entry index.
    inline ee::Float tuningValue(uint32_t category, uint32_t entry)
    {
        return (*g_tuningCategories)[category].entries[entry].value;
    }
    constexpr uint32_t kTuningEntriesBegin = sym::tuningEntriesBegin;
    constexpr uint32_t kTuningEntriesEnd = sym::tuningEntriesEnd;
    // Camera / Far Clip (rewritten at every level start, applyLevelCameraSettings).
    inline ee::Global<TuningEntry, sym::g_tuningFarClip> g_tuningFarClip;
}
