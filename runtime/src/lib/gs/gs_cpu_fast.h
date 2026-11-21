#pragma once

// Fast path of the CPU GS backend (gs_cpu_fast.cpp).
//
// Triangles and sprites are queued instead of drawn immediately and drawn in
// screen tiles, in parallel, when something needs the result (any other
// backend call, or a queued draw that reads what an earlier one writes).
// Within a tile the draws run in submission order, and a batch only holds
// draws whose framebuffer/Z pixels map to distinct memory, so the result is
// the same as drawing them one after another. Everything else (lines,
// points, render-to-own-texture, unusual formats) is drawn by the reference
// rasterizer in gs_cpu_backend.cpp after the queue is flushed.
//
// The reference rasterizer reads texels through a one-page texture cache
// (GSMem::TexturePageCache) whose copy can be stale until TEXFLUSH. The fast
// path keeps that behaviour exactly: it tracks which page the cache holds and
// only copies the page when something is about to write to it (a copy taken
// then is identical to one taken at the miss). Draws that could read a stale
// copy go through the reference rasterizer.
//
// ZX_GS_REFERENCE=1 disables the fast path. ZX_GS_THREADS=<n> sets the
// number of raster threads (default: all cores, 1 = draw on the caller).

#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_memory.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct GSCpuBackend::FastPath
{
    explicit FastPath(GSCpuBackend &owner);
    ~FastPath();

    struct Palette
    {
        std::array<uint32_t, 256> colors{};
    };

    // A texture with its CLUT or TEXA applied: what SampleTexture's
    // samplePoint returns for each texel.
    struct TexColors
    {
        std::vector<uint32_t> colors;
        uint32_t width = 0;
        // A texture pack has a picture of this texture that was not read
        // yet (set on the raster thread): made again once one is.
        mutable std::atomic<bool> packWaiting{false};
    };

    struct ColorCacheEntry
    {
        uint64_t key = 0;
        uint64_t packGeneration = 0; // texture_pack::generation() when made
        std::shared_ptr<const Palette> palette; // keeps the key's palette alive
        std::shared_ptr<const TexColors> colors;
    };

    // Texels of one texture, read once from local memory (no CLUT applied).
    struct TexSource
    {
        uint8_t psm = 0;
        uint32_t tbp0 = 0;
        uint32_t tbw = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint32_t> texels;
        std::vector<uint32_t> pages;
        std::vector<uint32_t> gens;
        GSMem::PageMask mask;
        std::vector<ColorCacheEntry> colorCache;
    };

    // Framebuffer and Z buffer of a draw.
    struct Target
    {
        uint32_t fbp = 0;
        uint32_t fbw = 0;
        uint32_t zbp = 0;
        uint8_t fpsm = 0;
        uint8_t zpsm = 0;
        bool usesFrame = false;
        bool usesZ = false;
        uint16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        bool valid = false;
        GSMem::PageMask frame;
        GSMem::PageMask z;
        GSMem::PageMask pages;
    };

    struct QueuedDraw
    {
        GSPrimitiveBatch batch;
        std::shared_ptr<const TexColors> colors;
        // Mipmapped (GSDrawState::mip): the colors of the levels it can
        // reach, mipLo..mipHi.
        std::array<std::shared_ptr<const TexColors>, 7> levelColors;
        uint8_t mipLo = 0, mipHi = 0;
        bool sprite = false;
        // ClearFramebuffer: fill the rectangle (minX..maxY) with value,
        // keeping the bits of mask.
        bool clear = false;
        uint32_t clearValue = 0, clearMask = 0;
        int minX = 0, maxX = 0, minY = 0, maxY = 0;
        // Triangle setup, as in GSCpuBackend::DrawTriangle.
        float fx0 = 0, fy0 = 0, fx1 = 0, fy1 = 0, fx2 = 0, fy2 = 0;
        float winding = 0, invAbsDenom = 0;
        // Sprite setup, as in GSCpuBackend::DrawSprite.
        int unclippedX0 = 0, unclippedY0 = 0, spanX = 0, spanY = 0;
    };

    // Texture reads and color conversions a batch needs, done on the
    // raster thread before its tiles (local memory is current then).
    // A texture pack's part of a colour build (colorsFor with a pack or a
    // dump folder): the draw's level 0 state, the level, the palette as
    // PCSX2 reads it.
    struct PackJob
    {
        GSDrawState state;
        uint8_t level = 0;
        uint32_t clutCount = 0;
        uint64_t clutHash = 0;
        std::array<uint32_t, 256> clut{};
    };

    struct PrepareJob
    {
        std::shared_ptr<const PackJob> pack;
        std::shared_ptr<TexSource> decode;
        std::shared_ptr<TexColors> colors;
        std::shared_ptr<const TexSource> source;
        std::shared_ptr<const Palette> palette;
        GSTexaReg texa{};
    };

    // Draws queued together and drawn tile by tile.
    struct Batch
    {
        std::vector<PrepareJob> prepare;
        std::vector<QueuedDraw> draws;
        std::vector<std::vector<uint32_t>> bins;
        std::vector<uint32_t> activeBins;
        uint64_t queuedArea = 0;
        GSMem::PageMask writes;
        std::vector<Target> targets;
    };

    enum class CacheBase : uint8_t
    {
        Invalid, // no page cached
        Eager,   // owner.m_texturePageCache holds a copy that may be stale
        Lazy,    // page lazyPage is cached and equals local memory
    };

    // Returns false if the draw must go through the reference rasterizer
    // (the queue is then already flushed and the cache prepared).
    bool tryQueue(const GSPrimitiveBatch &batch);
    // Draws everything queued and waits until local memory is current.
    void flush();
    // Starts drawing the current batch (on the raster thread when async).
    void handOff();
    // Waits until every handed off batch is drawn.
    void join();
    // Waits for the raster thread if a handed off batch writes these pages.
    void waitFor(const GSMem::PageMask &pages);
    bool inflightTouches(const GSMem::PageMask &pages);
    void runBatch(Batch &batch);
    void rasterLoop();
    void recycle();
    void prepareSerial();
    void finishSerial();
    void noteWrite(uint32_t page);
    void textureFlush();
    void reset();
    void loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut);
    void clutChanged() { ++clutVersion; palettes.clear(); }

    bool queueClear(const GSContext &context, uint32_t value, uint32_t mask,
                    uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1);
    Target makeTarget(uint32_t fbp, uint32_t fbw, uint8_t fpsm, uint32_t zbp, uint8_t zpsm, bool usesFrame, bool usesZ,
                      uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) const;
    void admitTarget(const Target &target);
    void enqueue(QueuedDraw &&draw, const Target &target);
    void prepareBatch(Batch &batch);
    bool setupTriangle(QueuedDraw &draw) const;
    bool setupSprite(QueuedDraw &draw) const;
    const Target &targetFor(const GSDrawState &state);
    bool targetCompatible(const Target &target) const;
    std::shared_ptr<TexSource> *textureFor(const GSDrawState &state, GSMem::PageMask &pages);
    std::shared_ptr<const Palette> paletteFor(const GSDrawState &state);
    // packState: the draw's own state (level 0's) for a texture pack's
    // lookup, the colours being level `level`'s.
    std::shared_ptr<const TexColors> colorsFor(const std::shared_ptr<TexSource> &sourcePtr, const GSDrawState &state,
                                               const GSDrawState *packState = nullptr, uint8_t level = 0);
    // A pack's picture (or a dump) for a colour build, on the raster thread.
    void applyPack(PrepareJob &job);
    void reportStats();
    void resolvePending();
    void rasterTile(Batch &batch, uint32_t bin);
    bool lastReadPage(const QueuedDraw &draw, uint32_t &page) const;

    GSCpuBackend &owner;
    bool reference = false;
    bool deferred = false; // see GSRasterBackend::SetDeferred
    unsigned threads = 1;

    static constexpr uint32_t kTileShift = 5u;
    static constexpr uint32_t kTilesPerRow = 2048u >> kTileShift;
    static constexpr uint64_t kMaxInflight = 4u;
    std::unique_ptr<Batch> cur;
    std::vector<std::unique_ptr<Batch>> freeBatches;

    // Asynchronous drawing (ZX_GS_ASYNC=0 draws on the caller instead).
    bool async = true;
    std::thread rasterThread;
    std::mutex rasterMutex;
    std::condition_variable rasterWake;
    std::condition_variable rasterDone;
    std::deque<std::unique_ptr<Batch>> rasterQueue;   // handed off, not started
    std::vector<std::unique_ptr<Batch>> retired;      // drawn, to be recycled
    bool rasterStop = false;
    uint64_t handedSeq = 0;                           // producer side
    std::atomic<uint64_t> completedSeq{0};
    GSMem::PageMask inflightWrites;                   // producer side

    CacheBase cacheBase = CacheBase::Invalid;
    uint32_t lazyPage = 0;
    size_t readersFrom = 0; // queued draws from here on may have read texels
    GSMem::PageMask pendingReadPages;

    std::array<uint32_t, GSMem::PageMask::kPages> pageGen{};
    std::unordered_map<uint64_t, std::shared_ptr<TexSource>> textures;
    std::unordered_map<uint64_t, std::shared_ptr<const Palette>> palettes;
    std::unordered_map<uint64_t, std::shared_ptr<const Palette>> internedPalettes;
    uint64_t clutVersion = 1;
    Target lastTarget;
    // Names dump mode has written: not read out again (raster thread).
    std::mutex packDumpedMutex;
    std::unordered_set<uint64_t> packDumped; // NameHash values

    struct Stats
    {
        uint64_t queued = 0, serial = 0, flushes = 0, parallelFlushes = 0;
        uint64_t decodes = 0, decodedTexels = 0, colorBuilds = 0, colorTexels = 0;
        uint64_t paletteBuilds = 0, clutLoads = 0, clutChanges = 0, queuedArea = 0;
    } stats;
    bool statsEnabled = false;
};
