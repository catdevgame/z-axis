#pragma once

// GS texture memory audit (developers; ZX_GSMEM_AUDIT=1, off by default).
//
// Aggressive Inline's, Dave Mirra BMX 2's and BMX XXX's GS texture cache
// (the engine's placeTextureLevel, the same code in all three; its state
// g_textureCache) gives each mip level it uploads GS words at a low
// cursor that grows up from above the frame and Z buffers, and each CLUT
// 0x100 words below a high cursor that grows down from the top of GS memory
// (word 0x100000, block 16384). Neither allocator checks the other cursor or
// the end: the frame end flushes the whole cache only when less than 384 KB
// is left, and a frame that needs more than what is left runs on, first over
// the CLUTs and then, the 14-bit block address wrapping, onto the frame
// buffers, Z and whatever lies low (BRINGUP "Venice and Greenville in AI's
// free skate": garbage, stipple, a sky panel in front of the skater).
//
// The audit wraps the level allocator, lets the game's own code run
// unchanged, and reads where the level went. It counts, per level played:
//   allocations   mip levels given GS memory
//   peak          the highest block reached (an allocation's end: first
//                 block past it), the number to keep under 16384
//   clut floor    the lowest block the CLUTs reached (the high cursor)
//   over CLUTs    allocations ending past the CLUTs' lowest block at the
//                 time (texels written over palettes: the game's own
//                 "overflow" condition, cache +0x24)
//   past the end  allocations ending past block 16384 (wrapping onto the
//                 frame buffers); a subset of the above
//   frames        vsyncs in which at least one allocation overran
// and logs a summary per level when the level changes, every 600 vsyncs
// while it is played (if anything changed) and at each first overrun of a
// level. ZX_GSMEM_AUDIT=2 also logs every overrunning allocation (the
// texture's name). Nothing in the game's RAM, its GS memory or its timing
// changes: the wrapper charges no cycles and writes nothing.

#include <cstdint>
#include <functional>
#include <map>
#include <string>

class PS2Machine;

namespace zx::gs_texture_audit
{
    constexpr uint32_t kGsWords = 0x100000u; // 4 MB of GS memory, in 32-bit words
    constexpr uint32_t kWordsPerBlock = 64u;
    constexpr uint32_t kGsBlocks = kGsWords / kWordsPerBlock; // 16384

    struct LevelStats
    {
        uint64_t allocations = 0;
        uint32_t lowestStart = UINT32_MAX; // block
        uint32_t peakEnd = 0;              // block (exclusive end)
        uint32_t clutFloor = kGsBlocks;    // lowest block of a CLUT seen (the high cursor)
        uint64_t overCluts = 0;
        uint64_t pastEnd = 0;
        uint64_t frames = 0;        // vsyncs counted while the level was current
        uint64_t overrunFrames = 0; // ... with an overrun in them
        std::string peakTexture;    // the texture whose level reached the peak
    };

    // The accounting, separate from the hooks so it can be tested.
    class Audit
    {
    public:
        // The level the next allocations belong to ("AI Movie", "DM2 WOODWARD").
        // Returns true if it changed (the previous level's stats are final
        // until it is played again).
        bool setLevel(const std::string &label);
        const std::string &level() const { return m_level; }

        // One mip level placed at GS word `address`, `words` long, with the
        // CLUTs' cursor at word `clutCursor` once it was placed. Returns
        // 0 (fits), 1 (over the CLUTs) or 2 (past the end of GS memory).
        int noteAllocation(uint32_t address, uint32_t words, uint32_t clutCursor, const char *texture = nullptr);
        // A vsync: closes the frame's count.
        void noteFrame();

        const LevelStats &stats(const std::string &label) const;
        const std::map<std::string, LevelStats> &all() const { return m_levels; }
        // "[gsmem] AI Movie: 1234 allocations, blocks 5632..14558 (peak), CLUTs from 16100, 0 over the CLUTs, 0 past
        // the end (16384), overruns in 0 of 600 frames"
        static std::string describe(const std::string &label, const LevelStats &s);

    private:
        std::map<std::string, LevelStats> m_levels;
        std::string m_level = "?";
        LevelStats *m_current = nullptr;
        bool m_frameOverran = false;
    };

    // Where a game keeps its cache (both games' layout: low cursor +0x14,
    // high cursor +0x18; a level record: GS word address +0x08, words
    // +0x14; the texture object's name 0x30 before the cache's handle).
    struct GameCache
    {
        const char *game = "";     // the label prefix: "AI", "DM2"
        uint32_t levelAllocator = 0; // a0 = texture object + 0x30, a1 = its level record
        uint32_t cacheState = 0;
    };

    // A function that names the level being played (empty: not this one's);
    // the first source that answers wins, the game's own name last.
    using LabelSource = std::function<std::string(const uint8_t *rdram)>;
    void addLabelSource(LabelSource source);

    // ZX_GSMEM_AUDIT set and not "0": wraps the game's level allocator
    // (after whatever wrapped it before, so both run) and adds a vsync hook;
    // `name` names the level from the game's own state. Otherwise nothing.
    bool install(PS2Machine &runtime, const GameCache &cache, LabelSource name);
    bool enabled();
}
