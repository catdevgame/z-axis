// Fast path of the CPU GS backend; see gs_cpu_fast.h.
//
// Every per-pixel expression below is copied from the reference rasterizer
// (GSCpuBackend::DrawTriangle, DrawSprite, WritePixel, SampleTexture) so that
// both produce identical bits; only the order of the side-effect free tests
// changes (Z and destination alpha are tested before texturing).
#include "gs_cpu_fast.h"
#include "gs_cpu_shared.h"
#include "runtime/ps2_texture_pack.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#if defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#elif defined(__SSE4_1__)
#include <smmintrin.h>
#endif
#include <cstdio>

#if defined(_MSC_VER)
#define GS_FORCEINLINE __forceinline
#else
#define GS_FORCEINLINE inline __attribute__((always_inline))
#endif

using namespace GSInternal;
using namespace GSCpuShared;

namespace
{
    constexpr uint32_t kPageShift = 13u;
    constexpr size_t kMaxQueuedDraws = 1u << 16;
    constexpr uint64_t kParallelMinArea = 8192u;

    // A small pool of raster threads shared by all backends. run() hands out
    // item indices; the caller works too and returns when all are done.
    class WorkerPool
    {
    public:
        using Fn = void (*)(void *, uint32_t);

        explicit WorkerPool(unsigned workers)
        {
            for (unsigned i = 0; i < workers; ++i)
                m_threads.emplace_back([this]
                                       { workerLoop(); });
        }

        ~WorkerPool()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_wake.notify_all();
            for (auto &thread : m_threads)
                thread.join();
        }

        size_t workerCount() const { return m_threads.size(); }

        void run(uint32_t count, Fn fn, void *ctx)
        {
            std::lock_guard<std::mutex> caller(m_callerMutex);
            uint32_t generation;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                generation = ++m_generation;
                m_fn = fn;
                m_ctx = ctx;
                m_count = count;
                m_done.store(0u, std::memory_order_relaxed);
                m_ticket.store(static_cast<uint64_t>(generation) << 32u, std::memory_order_release);
            }
            m_wake.notify_all();
            work(generation, count, fn, ctx);
            while (m_done.load(std::memory_order_acquire) != count)
                std::this_thread::yield();
        }

    private:
        // Claims items of job `generation` until none are left. The ticket
        // carries the generation so a late thread cannot claim items of a
        // newer job with the old function.
        void work(uint32_t generation, uint32_t count, Fn fn, void *ctx)
        {
            uint64_t ticket = m_ticket.load(std::memory_order_acquire);
            for (;;)
            {
                if (static_cast<uint32_t>(ticket >> 32u) != generation)
                    return;
                const uint32_t index = static_cast<uint32_t>(ticket);
                if (index >= count)
                    return;
                if (!m_ticket.compare_exchange_weak(ticket, ticket + 1u, std::memory_order_acq_rel))
                    continue;
                fn(ctx, index);
                m_done.fetch_add(1u, std::memory_order_acq_rel);
                ticket = m_ticket.load(std::memory_order_acquire);
            }
        }

        void workerLoop()
        {
            uint32_t seen = 0u;
            for (;;)
            {
                uint32_t generation, count;
                Fn fn;
                void *ctx;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_wake.wait(lock, [&]
                                { return m_stop || m_generation != seen; });
                    if (m_stop)
                        return;
                    seen = generation = m_generation;
                    count = m_count;
                    fn = m_fn;
                    ctx = m_ctx;
                }
                work(generation, count, fn, ctx);
            }
        }

        std::vector<std::thread> m_threads;
        std::mutex m_callerMutex;
        std::mutex m_mutex;
        std::condition_variable m_wake;
        bool m_stop = false;
        uint32_t m_generation = 0u;
        Fn m_fn = nullptr;
        void *m_ctx = nullptr;
        uint32_t m_count = 0u;
        std::atomic<uint64_t> m_ticket{0u};
        std::atomic<uint32_t> m_done{0u};
    };

    unsigned configuredThreads()
    {
        if (const char *value = std::getenv("PS2X_GS_THREADS"))
        {
            const long parsed = std::strtol(value, nullptr, 0);
            if (parsed >= 1)
                return static_cast<unsigned>(std::min(parsed, 64L));
        }
        const unsigned hw = std::thread::hardware_concurrency();
        return hw ? std::min(hw, 32u) : 1u;
    }

    WorkerPool &workerPool(unsigned threads)
    {
        static WorkerPool pool(threads > 1u ? threads - 1u : 0u);
        return pool;
    }

    bool isFramePsm(uint8_t psm)
    {
        return psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
    }

    bool isZPsm(uint8_t psm)
    {
        return psm == GS_PSM_Z32 || psm == GS_PSM_Z24 || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S;
    }

    bool isTexturePsm(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
        case GS_PSM_Z32:
        case GS_PSM_Z24:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return true;
        default:
            return false;
        }
    }

    uint32_t pageHeight(uint8_t psm)
    {
        return GSInternal::bitsPerPixel(psm) == 16u ? 64u : 32u;
    }

    // Pixels of the rectangle map to distinct bytes: no row wraps into the
    // next one and the pages do not wrap around local memory.
    bool injective(uint8_t psm, uint32_t fbw, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
    {
        if (x1 >= fbw * 64u)
            return false;
        const uint32_t ph = pageHeight(psm);
        const uint32_t span = (y1 / ph - y0 / ph) * fbw + (x1 / 64u - x0 / 64u) + 1u;
        return span <= GSMem::PageMask::kPages;
    }

    // Byte address of a frame/Z pixel, as GSMem's Read/Write compute it.
    GS_FORCEINLINE uint32_t pixelAddress(uint8_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        using namespace GSMem;
        constexpr uint32_t kMem = static_cast<uint32_t>(MEMORY_SIZE);
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24: return (PixelStorageTraits<C32>::Address(PageTableC32, bp, bw, x, y) * 4u) & (kMem - 4u);
        case GS_PSM_CT16: return (PixelStorageTraits<C16>::Address(PageTableC16, bp, bw, x, y) * 2u) & (kMem - 2u);
        case GS_PSM_CT16S: return (PixelStorageTraits<C16S>::Address(PageTableC16S, bp, bw, x, y) * 2u) & (kMem - 2u);
        case GS_PSM_Z32:
        case GS_PSM_Z24: return (PixelStorageTraits<Z32>::Address(PageTableZ32, bp, bw, x, y) * 4u) & (kMem - 4u);
        case GS_PSM_Z16: return (PixelStorageTraits<Z16>::Address(PageTableZ16, bp, bw, x, y) * 2u) & (kMem - 2u);
        case GS_PSM_Z16S: return (PixelStorageTraits<Z16S>::Address(PageTableZ16S, bp, bw, x, y) * 2u) & (kMem - 2u);
        default: return 0u;
        }
    }

    GS_FORCEINLINE uint32_t loadPixel(const uint8_t *vram, uint8_t psm, uint32_t address)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        {
            uint32_t v;
            std::memcpy(&v, vram + address, 4);
            return v;
        }
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            uint32_t v;
            std::memcpy(&v, vram + address, 4);
            return v & 0x00FFFFFFu;
        }
        default:
        {
            uint16_t v;
            std::memcpy(&v, vram + address, 2);
            return v;
        }
        }
    }

    GS_FORCEINLINE void storePixel(uint8_t *vram, uint8_t psm, uint32_t address, uint32_t value)
    {
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
            std::memcpy(vram + address, &value, 4);
            break;
        case GS_PSM_CT24:
        case GS_PSM_Z24:
        {
            uint32_t old;
            std::memcpy(&old, vram + address, 4);
            value = (old & 0xFF000000u) | (value & 0x00FFFFFFu);
            std::memcpy(vram + address, &value, 4);
            break;
        }
        default:
        {
            const uint16_t v = static_cast<uint16_t>(value);
            std::memcpy(vram + address, &v, 2);
            break;
        }
        }
    }

    // Addresses and the framebuffer value of the current pixel, computed
    // once and shared by the tests and the writes.
    struct PixelMem
    {
        uint32_t zAddress = UINT32_MAX;
        uint32_t fAddress = UINT32_MAX;
        bool haveRaw = false;
        uint32_t raw = 0u;
    };

    // Per draw constants of WritePixel.
    struct PixelState
    {
        uint8_t *vram;
        uint8_t fpsm;
        uint8_t zpsm;
        uint32_t fbp;
        uint32_t zbp;
        uint32_t fbw;
        uint64_t test;
        uint32_t ztestMethod;
        bool destinationAlphaTestNeedsRead;
        bool abe;
        bool pabe;
        bool fge;
        uint8_t fogR, fogG, fogB;
        uint64_t alpha;
        uint32_t fbmsk;
        uint64_t fba;
        bool zmask;
        bool colclamp;

        PixelState(const GSDrawState &state, uint8_t *vramIn)
        {
            const auto &ctx = state.context;
            vram = vramIn;
            fpsm = ctx.frame.psm;
            zpsm = ctx.zbuf.psm;
            fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
            zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
            fbw = std::max<u32>(ctx.frame.fbw, 1u);
            test = ctx.test;
            ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
            destinationAlphaTestNeedsRead = ((ctx.test >> 14) & 0x1u) != 0u && (fpsm == GS_PSM_CT32 || fpsm == GS_PSM_CT16 || fpsm == GS_PSM_CT16S);
            abe = state.prim.abe;
            pabe = state.pabe;
            fge = state.prim.fge;
            fogR = state.fogR;
            fogG = state.fogG;
            fogB = state.fogB;
            alpha = ctx.alpha;
            fbmsk = ctx.frame.fbmsk;
            fba = ctx.fba;
            zmask = ctx.zbuf.zmask;
            colclamp = (state.colclamp & 0x1u) != 0u;
        }

        // The Z and destination alpha tests of WritePixel. Nothing is written
        // for a pixel that fails them, whatever the alpha test says, so they
        // can run before texturing.
        GS_FORCEINLINE bool earlyTests(int x, int y, uint32_t z, PixelMem &m) const
        {
            switch (ztestMethod)
            {
            case 0:
                return false;
            case 1:
                break;
            case 2:
                m.zAddress = pixelAddress(zpsm, zbp, fbw, x, y);
                if (!(z >= loadPixel(vram, zpsm, m.zAddress)))
                    return false;
                break;
            case 3:
                m.zAddress = pixelAddress(zpsm, zbp, fbw, x, y);
                if (!(z > loadPixel(vram, zpsm, m.zAddress)))
                    return false;
                break;
            }
            if (destinationAlphaTestNeedsRead)
            {
                m.fAddress = pixelAddress(fpsm, fbp, fbw, x, y);
                m.raw = loadPixel(vram, fpsm, m.fAddress);
                m.haveRaw = true;
                if (!passesDestinationAlphaTest(test, fpsm, m.raw))
                    return false;
            }
            return true;
        }

        // The rest of GSCpuBackend::WritePixel, after the Z and destination
        // alpha tests passed.
        GS_FORCEINLINE void finish(int x, int y, uint32_t z, uint8_t r, uint8_t g, uint8_t b, uint8_t a, uint8_t fog, PixelMem &m) const
        {
            if (fge)
            {
                const uint32_t inverseFog = 255u - fog;
                auto applyFog = [&](uint8_t input, uint8_t fogColor) -> uint8_t
                {
                    return static_cast<uint8_t>(((static_cast<uint32_t>(fog) * input) >> 8) + ((inverseFog * fogColor) >> 8));
                };

                r = applyFog(r, fogR);
                g = applyFog(g, fogG);
                b = applyFog(b, fogB);
            }

            const PixelWriteMask writeMask = classifyAlphaTest(test, a, fpsm);
            if (!writeMask.writesAnything())
                return;

            const bool alphaBlendEnabled = abe;
            const bool preserveDestinationAlpha = writeMask.writeRgb && !writeMask.writeAlpha && fpsm == GS_PSM_CT32;
            const bool frmw = destinationAlphaTestNeedsRead || (writeMask.writesFramebuffer() && ((fbmsk != 0) || alphaBlendEnabled || preserveDestinationAlpha));

            u32 fbrgba = 0;
            if (frmw)
            {
                if (!m.haveRaw)
                {
                    m.fAddress = pixelAddress(fpsm, fbp, fbw, x, y);
                    m.raw = loadPixel(vram, fpsm, m.fAddress);
                }
                fbrgba = m.raw;
                if (bitsPerPixel(fpsm) == 16)
                    fbrgba = Rgba5551ToRgba8888(fbrgba);
                else if (fpsm == GS_PSM_CT24)
                    fbrgba |= 0x80000000u;
            }

            if (writeMask.writesFramebuffer())
            {
                const u8 srcR = r;
                const u8 srcG = g;
                const u8 srcB = b;

                if (abe)
                {
                    uint8_t dr = fbrgba & 0xFF;
                    uint8_t dg = (fbrgba >> 8) & 0xFF;
                    uint8_t db = (fbrgba >> 16) & 0xFF;
                    uint8_t da = (fbrgba >> 24) & 0xFF;

                    if (!(pabe && (a & 0x80u) == 0u))
                    {
                        uint64_t alphaReg = alpha;
                        uint8_t asel = alphaReg & 3;
                        uint8_t bsel = (alphaReg >> 2) & 3;
                        uint8_t csel = (alphaReg >> 4) & 3;
                        uint8_t dsel = (alphaReg >> 6) & 3;
                        uint8_t fix = static_cast<uint8_t>((alphaReg >> 32) & 0xFF);

                        auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
                        {
                            if (sel == 0)
                                return cs;
                            if (sel == 1)
                                return cd;
                            return 0;
                        };
                        int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                                   : fix;

                        r = blendOutput(((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr), colclamp);
                        g = blendOutput(((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg), colclamp);
                        b = blendOutput(((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db), colclamp);
                    }
                    else
                    {
                        r = srcR;
                        g = srcG;
                        b = srcB;
                    }
                }

                if (writeMask.writeAlpha && (fba & 0x1ull) != 0ull && fpsm != GS_PSM_CT24)
                    a = static_cast<uint8_t>(a | 0x80u);

                u32 pixel = pack32(r, g, b, a);
                if (fbmsk != 0)
                    pixel = (pixel & ~fbmsk) | (fbrgba & fbmsk);
                if (preserveDestinationAlpha)
                    pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
                if (bitsPerPixel(fpsm) == 16)
                    pixel = Rgba8888ToRgba5551(pixel);

                if (m.fAddress == UINT32_MAX)
                    m.fAddress = pixelAddress(fpsm, fbp, fbw, x, y);
                storePixel(vram, fpsm, m.fAddress, pixel);
            }

            if (writeMask.writeDepth && !zmask)
            {
                if (m.zAddress == UINT32_MAX)
                    m.zAddress = pixelAddress(zpsm, zbp, fbw, x, y);
                storePixel(vram, zpsm, m.zAddress, z);
            }
        }
    };

    // lerpChannel for the four channels at once. Lane by lane this is the
    // same arithmetic as the scalar code when the compiler fuses
    // a + b * c there (it does on arm64); selfTest() checks that at startup
    // and the scalar code is used otherwise. Only valid while fx and fy are
    // in [0, 1], so every intermediate stays far from the int range limits.
#if defined(__aarch64__) || defined(_M_ARM64)
    GS_FORCEINLINE float32x4_t texelChannels(uint32_t c)
    {
        const uint8x8_t bytes = vreinterpret_u8_u32(vdup_n_u32(c));
        return vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(bytes))));
    }

    GS_FORCEINLINE uint32_t lerpTexelSimd(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy)
    {
        const float32x4_t f00 = texelChannels(c00), f10 = texelChannels(c10);
        const float32x4_t f01 = texelChannels(c01), f11 = texelChannels(c11);
        const float32x4_t vfx = vdupq_n_f32(fx), vfy = vdupq_n_f32(fy);
        const float32x4_t top = vfmaq_f32(f00, vsubq_f32(f10, f00), vfx);
        const float32x4_t bottom = vfmaq_f32(f01, vsubq_f32(f11, f01), vfx);
        const float32x4_t mixed = vfmaq_f32(top, vsubq_f32(bottom, top), vfy);
        int32x4_t rounded = vcvtaq_s32_f32(mixed);
        rounded = vmaxq_s32(vminq_s32(rounded, vdupq_n_s32(255)), vdupq_n_s32(0));
        const uint16x4_t narrow16 = vmovn_u32(vreinterpretq_u32_s32(rounded));
        const uint8x8_t narrow8 = vmovn_u16(vcombine_u16(narrow16, narrow16));
        return vget_lane_u32(vreinterpret_u32_u8(narrow8), 0);
    }
    constexpr bool kHaveBilinearSimd = true;
#elif defined(__SSE4_1__)
    // x86-64 without FMA: separate multiply and add, as the scalar code is
    // compiled there; std::lround's half away from zero as truncation plus
    // one where the dropped fraction is 0.5 or more (exact for these values).
    GS_FORCEINLINE __m128 texelChannels(uint32_t c)
    {
        return _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(static_cast<int>(c))));
    }

    GS_FORCEINLINE uint32_t lerpTexelSimd(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy)
    {
        const __m128 f00 = texelChannels(c00), f10 = texelChannels(c10);
        const __m128 f01 = texelChannels(c01), f11 = texelChannels(c11);
        const __m128 vfx = _mm_set1_ps(fx), vfy = _mm_set1_ps(fy);
        const __m128 top = _mm_add_ps(f00, _mm_mul_ps(_mm_sub_ps(f10, f00), vfx));
        const __m128 bottom = _mm_add_ps(f01, _mm_mul_ps(_mm_sub_ps(f11, f01), vfx));
        const __m128 mixed = _mm_add_ps(top, _mm_mul_ps(_mm_sub_ps(bottom, top), vfy));
        const __m128 whole = _mm_round_ps(mixed, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC);
        const __m128 up = _mm_and_ps(_mm_cmpge_ps(_mm_sub_ps(mixed, whole), _mm_set1_ps(0.5f)), _mm_set1_ps(1.0f));
        __m128i rounded = _mm_cvttps_epi32(_mm_add_ps(whole, up));
        rounded = _mm_max_epi32(_mm_min_epi32(rounded, _mm_set1_epi32(255)), _mm_setzero_si128());
        const __m128i narrow16 = _mm_packus_epi32(rounded, rounded);
        return static_cast<uint32_t>(_mm_cvtsi128_si32(_mm_packus_epi16(narrow16, narrow16)));
    }
    constexpr bool kHaveBilinearSimd = true;
#else
    inline uint32_t lerpTexelSimd(uint32_t, uint32_t, uint32_t, uint32_t, float, float) { return 0u; }
    constexpr bool kHaveBilinearSimd = false;
#endif

    inline uint32_t lerpTexelScalar(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy)
    {
        uint32_t out = 0u;
        for (uint32_t shift = 0; shift < 32u; shift += 8u)
            out |= static_cast<uint32_t>(lerpChannel(static_cast<uint8_t>(c00 >> shift), static_cast<uint8_t>(c10 >> shift),
                                                     static_cast<uint8_t>(c01 >> shift), static_cast<uint8_t>(c11 >> shift),
                                                     fx, fy))
                   << shift;
        return out;
    }

    bool bilinearSimdMatches()
    {
        if (!kHaveBilinearSimd)
            return false;
        uint64_t state = 0x2545F4914F6CDD1Dull;
        auto next = [&]()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<uint32_t>(state >> 11);
        };
        for (int i = 0; i < 200000; ++i)
        {
            const uint32_t c00 = next(), c10 = next(), c01 = next(), c11 = next();
            float fx = static_cast<float>(next() & 0xFFFFFFu) / 16777216.0f;
            float fy = static_cast<float>(next() & 0xFFFFFFu) / 16777216.0f;
            if (i % 7 == 0)
                fx = static_cast<float>(next() % 3u) * 0.5f;
            if (i % 11 == 0)
                fy = 1.0f;
            if (lerpTexelSimd(c00, c10, c01, c11, fx, fy) != lerpTexelScalar(c00, c10, c01, c11, fx, fy))
                return false;
        }
        return true;
    }

    bool bilinearSimd()
    {
        static const bool enabled = []()
        {
            const char *env = std::getenv("PS2X_GS_SIMD");
            if (env && env[0] == '0')
                return false;
            return bilinearSimdMatches();
        }();
        return enabled;
    }

    GS_FORCEINLINE uint32_t lerpTexel(uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11, float fx, float fy)
    {
        return lerpTexelSimd(c00, c10, c01, c11, fx, fy);
    }

    // Per draw constants of SampleTexture, reading texel colors with the
    // CLUT/TEXA already applied.
    struct TexState
    {
        const uint32_t *colors = nullptr;
        uint32_t width = 0;
        int texW = 1;
        int texH = 1;
        int coordW = 1; // level 0 size: S/Q and T/Q to level 0 texels
        int coordH = 1;
        uint8_t wrapU = 0, wrapV = 0;
        uint16_t minU = 0, maxU = 0, minV = 0, maxV = 0;
        bool linear = false;
        bool fst = false;
        uint8_t level = 0; // mipmap level: texel coordinates * levelScale
        float levelScale = 1.0f;

        TexState() = default;
        TexState(const GSDrawState &state, const GSCpuBackend::FastPath::TexColors *source, uint8_t mipLevel = 0)
        {
            const auto &ctx = state.context;
            colors = source ? source->colors.data() : nullptr;
            width = source ? source->width : 0u;
            coordW = texW = state.textureWidth;
            coordH = texH = state.textureHeight;
            const uint64_t clamp = ctx.clamp;
            wrapU = static_cast<uint8_t>(clamp & 0x3u);
            wrapV = static_cast<uint8_t>((clamp >> 2) & 0x3u);
            minU = static_cast<uint16_t>((clamp >> 4) & 0x3FFu);
            maxU = static_cast<uint16_t>((clamp >> 14) & 0x3FFu);
            minV = static_cast<uint16_t>((clamp >> 24) & 0x3FFu);
            maxV = static_cast<uint16_t>((clamp >> 34) & 0x3FFu);
            linear = state.linearFilter;
            fst = state.prim.fst;
            if (mipLevel != 0u)
            {
                // As SampleTexture's levelOf.
                level = mipLevel;
                levelScale = gsMipLevelScale(mipLevel);
                texW = std::max(1, texW >> mipLevel);
                texH = std::max(1, texH >> mipLevel);
                minU = static_cast<uint16_t>(minU >> mipLevel);
                maxU = static_cast<uint16_t>(maxU >> mipLevel);
                minV = static_cast<uint16_t>(minV >> mipLevel);
                maxV = static_cast<uint16_t>(maxV >> mipLevel);
            }
        }

        GS_FORCEINLINE void texelCoords(float s, float t, float q, uint16_t u, uint16_t v, float &texUf, float &texVf) const
        {
            if (fst)
            {
                texUf = static_cast<float>(u) / 16.0f;
                texVf = static_cast<float>(v) / 16.0f;
            }
            else
            {
                const float invQ = 1.0f / fabsQ(q);
                texUf = s * invQ * static_cast<float>(coordW);
                texVf = t * invQ * static_cast<float>(coordH);
            }
            if (level != 0u)
            {
                texUf *= levelScale;
                texVf *= levelScale;
            }
        }

        GS_FORCEINLINE uint32_t samplePoint(int sampleU, int sampleV) const
        {
            sampleU = wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU);
            sampleV = wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV);
            return colors[static_cast<size_t>(sampleV) * width + static_cast<uint32_t>(sampleU)];
        }

        GS_FORCEINLINE const uint32_t *row(int sampleV) const
        {
            return colors + static_cast<size_t>(wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV)) * width;
        }

        GS_FORCEINLINE uint32_t column(int sampleU) const
        {
            return static_cast<uint32_t>(wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU));
        }

        GS_FORCEINLINE uint32_t sample(float s, float t, float q, uint16_t u, uint16_t v) const
        {
            return sampleFiltered(s, t, q, u, v, linear);
        }

        GS_FORCEINLINE uint32_t sampleFiltered(float s, float t, float q, uint16_t u, uint16_t v, bool linear) const
        {
            float texUf, texVf;
            texelCoords(s, t, q, u, v, texUf, texVf);

            if (!linear)
                return samplePoint(texelTrunc(texUf), texelTrunc(texVf));

            const float sampleU = texUf - 0.5f;
            const float sampleV = texVf - 0.5f;
            const int u0 = texelFloor(sampleU);
            const int v0 = texelFloor(sampleV);
            const int u1 = u0 + 1;
            const int v1 = v0 + 1;
            const float fx = texelFraction(sampleU);
            const float fy = texelFraction(sampleV);

            // samplePoint(u0, v0), (u1, v0), (u0, v1), (u1, v1), wrapping
            // each coordinate once.
            const uint32_t *row0 = row(v0);
            const uint32_t *row1 = row(v1);
            const uint32_t col0 = column(u0);
            const uint32_t col1 = column(u1);
            const uint32_t c00 = row0[col0];
            const uint32_t c10 = row0[col1];
            const uint32_t c01 = row1[col0];
            const uint32_t c11 = row1[col1];

            if (bilinearSimd() && fx >= 0.0f && fx <= 1.0f && fy >= 0.0f && fy <= 1.0f)
                return lerpTexel(c00, c10, c01, c11, fx, fy);

            const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                          static_cast<uint8_t>(c10 & 0xFFu),
                                          static_cast<uint8_t>(c01 & 0xFFu),
                                          static_cast<uint8_t>(c11 & 0xFFu),
                                          fx, fy);
            const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                          static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                          static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                          static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                          fx, fy);
            const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                          static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                          static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                          static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                          fx, fy);
            const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                          static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                          static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                          static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                          fx, fy);

            return static_cast<uint32_t>(r) |
                   (static_cast<uint32_t>(g) << 8) |
                   (static_cast<uint32_t>(b) << 16) |
                   (static_cast<uint32_t>(a) << 24);
        }

        // Texel coordinates of the last local memory read sample() makes.
        inline void lastRead(float s, float t, float q, uint16_t u, uint16_t v, int &outU, int &outV) const
        {
            lastReadFiltered(s, t, q, u, v, linear, outU, outV);
        }

        inline void lastReadFiltered(float s, float t, float q, uint16_t u, uint16_t v, bool linear, int &outU, int &outV) const
        {
            float texUf, texVf;
            texelCoords(s, t, q, u, v, texUf, texVf);
            int sampleU, sampleV;
            if (!linear)
            {
                sampleU = texelTrunc(texUf);
                sampleV = texelTrunc(texVf);
            }
            else
            {
                sampleU = texelFloor(texUf - 0.5f) + 1;
                sampleV = texelFloor(texVf - 0.5f) + 1;
            }
            outU = wrapTextureCoordinate(sampleU, texW, wrapU, minU, maxU);
            outV = wrapTextureCoordinate(sampleV, texH, wrapV, minV, maxV);
        }
    };

    using QueuedDraw = GSCpuBackend::FastPath::QueuedDraw;

    constexpr float kEdgeEpsilon = 1.0e-4f;

    // The levels a mipmapped draw can reach. A pixel's Q is its vertices'
    // interpolated with weights that sum to 1 and are at least
    // -kEdgeEpsilon (triangleWeights), so it lies within 2 * kEdgeEpsilon
    // of the Q range past either end, plus 1 % of margin for rounding.
    // That reach matters when the vertices' Q differ by orders of magnitude
    // (a triangle running from the camera into the distance): a reach that
    // gets to 0 can give any level, and a level outside the range has no
    // colours (MipTex). Q of mixed sign or zero: every level.
    inline void mipRange(const GSPrimitiveBatch &batch, bool sprite, uint8_t &lo, uint8_t &hi)
    {
        const GSMipInfo &m = batch.state.mip;
        lo = 0;
        hi = m.maxLevel;
        float qMin = 0.0f, qMax = 0.0f;
        if (m.lcm)
            qMin = qMax = 1.0f;
        else if (sprite)
            qMin = qMax = batch.vertices[1].q;
        else
        {
            qMin = std::min({batch.vertices[0].q, batch.vertices[1].q, batch.vertices[2].q});
            qMax = std::max({batch.vertices[0].q, batch.vertices[1].q, batch.vertices[2].q});
        }
        if (!(qMin > 0.0f || qMax < 0.0f))
            return;
        const float aLo = std::min(std::fabs(qMin), std::fabs(qMax)), aHi = std::max(std::fabs(qMin), std::fabs(qMax));
        const float reach = 2.0f * kEdgeEpsilon * (aHi - aLo);
        const GSMipSample nearest = gsMipSelect(m, (aHi + reach) * 1.01f);
        lo = nearest.level;
        if (!(aLo - reach > 0.0f))
            return; // up to the last level
        const GSMipSample farthest = gsMipSelect(m, (aLo - reach) * 0.99f);
        hi = std::max(farthest.level, farthest.level2);
    }

    // The draw state of mipmap level k as one texture (textureFor, colorsFor).
    inline GSDrawState mipLevelState(const GSDrawState &state, uint8_t k)
    {
        GSDrawState ls = state;
        if (k == 0u)
            return ls;
        ls.context.tex0.tbp0 = state.mip.tbp[k];
        ls.context.tex0.tbw = state.mip.tbw[k];
        ls.textureWidth = static_cast<uint16_t>(std::max(1, state.textureWidth >> k));
        ls.textureHeight = static_cast<uint16_t>(std::max(1, state.textureHeight >> k));
        const uint64_t clamp = state.context.clamp;
        auto field = [&](uint32_t shift) -> uint64_t
        { return (((clamp >> shift) & 0x3FFu) >> k) << shift; };
        ls.context.clamp = (clamp & ~((0xFFFFFFFFFFull) << 4)) | field(4) | field(14) | field(24) | field(34);
        return ls;
    }

    // A mipmapped draw (GSDrawState::mip): the levels it can reach, sampled
    // per pixel as SampleTexture does.
    struct MipTex
    {
        std::array<TexState, 7> levels{};
        GSMipInfo mip{};

        MipTex(const QueuedDraw &d)
        {
            const GSDrawState &state = d.batch.state;
            mip = state.mip;
            for (uint32_t k = d.mipLo; k <= d.mipHi; ++k)
                levels[k] = TexState(state, d.levelColors[k].get(), static_cast<uint8_t>(k));
        }

        GS_FORCEINLINE uint32_t sample(float s, float t, float q, uint16_t u, uint16_t v, float lodQ) const
        {
            const GSMipSample m = gsMipSelect(mip, lodQ);
            const uint32_t c0 = levels[m.level].sampleFiltered(s, t, q, u, v, m.linear);
            if (m.level2 == m.level || !(m.frac > 0.0f))
                return c0;
            const uint32_t c1 = levels[m.level2].sampleFiltered(s, t, q, u, v, m.linear);
            return gsMipBlend(c0, c1, m.frac);
        }
    };

    // Barycentric weights and coverage, as in DrawTriangle (kEdgeEpsilon above).
    GS_FORCEINLINE bool triangleWeights(const QueuedDraw &d, int x, int y, float &w0, float &w1, float &w2)
    {
        const float fx0 = d.fx0, fy0 = d.fy0, fx1 = d.fx1, fy1 = d.fy1, fx2 = d.fx2, fy2 = d.fy2;
        const float winding = d.winding;
        const float invAbsDenom = d.invAbsDenom;
        float py = static_cast<float>(y) + 0.5f;
        float px = static_cast<float>(x) + 0.5f;

        w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
        w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
        w2 = 1.0f - w0 - w1;

        return !(w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon);
    }

    GS_FORCEINLINE void triangleTexArgs(const QueuedDraw &d, float w0, float w1, float w2,
                                float &is, float &it, float &iq, uint16_t &iu, uint16_t &iv)
    {
        const GSVertex &v0 = d.batch.vertices[0];
        const GSVertex &v1 = d.batch.vertices[1];
        const GSVertex &v2 = d.batch.vertices[2];
        if (d.batch.state.prim.fst)
        {
            iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
            iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
            is = 0.0f;
            it = 0.0f;
            iq = 1.0f;
        }
        else
        {
            is = v0.s * w0 + v1.s * w1 + v2.s * w2;
            it = v0.t * w0 + v1.t * w1 + v2.t * w2;
            iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
            iu = 0;
            iv = 0;
        }
    }

    // Conservative test that no pixel center of a rectangle passes the
    // coverage test of triangleWeights. Evaluates the edge functions in
    // double from the same float constants and adds a bound on the float
    // rounding error (four roundings of at most 2^-24 relative each for
    // w0/w1, taken 4x larger), so a rejected region is rejected exactly.
    struct EdgeCull
    {
        double a0, c0, a1, c1, fx2, fy2, k, absK;

        explicit EdgeCull(const QueuedDraw &d)
        {
            const float fa0 = d.fy1 - d.fy2, fc0 = d.fx2 - d.fx1;
            const float fa1 = d.fy2 - d.fy0, fc1 = d.fx0 - d.fx2;
            a0 = fa0;
            c0 = fc0;
            a1 = fa1;
            c1 = fc1;
            fx2 = d.fx2;
            fy2 = d.fy2;
            k = static_cast<double>(d.winding) * static_cast<double>(d.invAbsDenom);
            absK = std::fabs(k);
        }

        // Largest value + error bound of each edge function at one point.
        inline void bounds(double px, double py, double &m0, double &m1, double &m2) const
        {
            constexpr double kErr = 1.0 / 1048576.0; // 2^-20
            const double dx = px - fx2, dy = py - fy2;
            const double w0 = (a0 * dx + c0 * dy) * k;
            const double w1 = (a1 * dx + c1 * dy) * k;
            const double e0 = kErr * (std::fabs(a0 * dx) + std::fabs(c0 * dy)) * absK + 1e-12;
            const double e1 = kErr * (std::fabs(a1 * dx) + std::fabs(c1 * dy)) * absK + 1e-12;
            const double w2 = 1.0 - w0 - w1;
            const double e2 = 2.0 * (e0 + e1) + kErr * (1.0 + std::fabs(w0) + std::fabs(w1));
            m0 = std::max(m0, w0 + e0);
            m1 = std::max(m1, w1 + e1);
            m2 = std::max(m2, w2 + e2);
        }

        // Value + error is convex in (px, py): its maximum over the
        // rectangle is at a corner.
        bool rejects(int x0, int y0, int x1, int y1) const
        {
            const double lo = -1e300;
            double m0 = lo, m1 = lo, m2 = lo;
            const double px0 = x0 + 0.5, px1 = x1 + 0.5, py0 = y0 + 0.5, py1 = y1 + 0.5;
            bounds(px0, py0, m0, m1, m2);
            bounds(px1, py0, m0, m1, m2);
            if (y1 != y0)
            {
                bounds(px0, py1, m0, m1, m2);
                bounds(px1, py1, m0, m1, m2);
            }
            const double eps = kEdgeEpsilon;
            return m0 < -eps || m1 < -eps || m2 < -eps;
        }
    };

    void rasterTriangle(const QueuedDraw &d, uint8_t *vram, int tx0, int ty0, int tx1, int ty1)
    {
        const GSDrawState &state = d.batch.state;
        const GSVertex &v0 = d.batch.vertices[0];
        const GSVertex &v1 = d.batch.vertices[1];
        const GSVertex &v2 = d.batch.vertices[2];
        const PixelState p(state, vram);
        const TexState tex = state.prim.tme ? TexState(state, d.colors.get()) : TexState();
        const bool mipmapped = state.prim.tme && state.mip.maxLevel != 0u;
        const std::optional<MipTex> mipTex = mipmapped ? std::optional<MipTex>(MipTex(d)) : std::nullopt;
        const auto &tex0 = state.context.tex0;

        const int minX = std::max(d.minX, tx0);
        const int maxX = std::min(d.maxX, tx1);
        const int minY = std::max(d.minY, ty0);
        const int maxY = std::min(d.maxY, ty1);
        if (minX > maxX || minY > maxY)
            return;

        const EdgeCull cull(d);
        if (cull.rejects(minX, minY, maxX, maxY))
            return;
        constexpr int kChunk = 8;

        for (int y = minY; y <= maxY; ++y)
        {
            // Trim chunks of the row that cannot be covered.
            int rowMin = minX, rowMax = maxX;
            while (rowMin <= rowMax && cull.rejects(rowMin, y, std::min(rowMin + kChunk - 1, rowMax), y))
                rowMin += kChunk;
            while (rowMax >= rowMin && cull.rejects(std::max(rowMax - kChunk + 1, rowMin), y, rowMax, y))
                rowMax -= kChunk;
            for (int x = rowMin; x <= rowMax; ++x)
            {
                float w0, w1, w2;
                if (!triangleWeights(d, x, y, w0, w1, w2))
                    continue;

                double z = v0.z * w0 + v1.z * w1 + v2.z * w2;
                const uint32_t zValue = static_cast<u32>(z + 0.5);

                PixelMem m;
                if (!p.earlyTests(x, y, zValue, m))
                    continue;

                uint8_t r, g, b, a;
                if (state.prim.iip)
                {
                    r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                    g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                    b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                    a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
                }
                else
                {
                    r = v2.r;
                    g = v2.g;
                    b = v2.b;
                    a = v2.a;
                }

                if (state.prim.tme)
                {
                    float is, it, iq;
                    uint16_t iu, iv;
                    triangleTexArgs(d, w0, w1, w2, is, it, iq, iu, iv);
                    uint32_t texel = mipmapped ? mipTex->sample(is, it, iq, iu, iv, v0.q * w0 + v1.q * w1 + v2.q * w2)
                                               : tex.sample(is, it, iq, iu, iv);

                    uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                    uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                    uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                    uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                    const TextureCombineResult color = combineTexture(tex0, r, g, b, a, tr, tg, tb, ta);
                    r = color.r;
                    g = color.g;
                    b = color.b;
                    a = color.a;
                }

                uint8_t fog = 0;
                if (p.fge)
                    fog = clampU8(static_cast<int>(v0.fog * w0 + v1.fog * w1 + v2.fog * w2));
                p.finish(x, y, zValue, r, g, b, a, fog, m);
            }
        }
    }

    struct SpriteTex
    {
        float u0f, v0f, u1f, v1f, spriteW, spriteH;
    };

    inline SpriteTex spriteTexSetup(const QueuedDraw &d)
    {
        const GSDrawState &state = d.batch.state;
        const GSVertex &v0 = d.batch.vertices[0];
        const GSVertex &v1 = d.batch.vertices[1];
        const int texW = state.textureWidth;
        const int texH = state.textureHeight;
        SpriteTex st{};
        if (state.prim.fst)
        {
            st.u0f = static_cast<float>(v0.u >> 4);
            st.v0f = static_cast<float>(v0.v >> 4);
            st.u1f = static_cast<float>(v1.u >> 4);
            st.v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            st.u0f = (v0.s / q0) * static_cast<float>(texW);
            st.v0f = (v0.t / q0) * static_cast<float>(texH);
            st.u1f = (v1.s / q1) * static_cast<float>(texW);
            st.v1f = (v1.t / q1) * static_cast<float>(texH);
        }
        st.spriteW = static_cast<float>(d.spanX);
        st.spriteH = static_cast<float>(d.spanY);
        if (st.spriteW < 1.0f)
            st.spriteW = 1.0f;
        if (st.spriteH < 1.0f)
            st.spriteH = 1.0f;
        return st;
    }

    inline float spriteTexV(const QueuedDraw &d, const SpriteTex &st, int y)
    {
        float ty = (static_cast<float>(y - d.unclippedY0) + 0.5f) / st.spriteH;
        float texVf = st.v0f + (st.v1f - st.v0f) * ty;
        return texVf;
    }

    inline float spriteTexU(const QueuedDraw &d, const SpriteTex &st, int x)
    {
        float tx = (static_cast<float>(x - d.unclippedX0) + 0.5f) / st.spriteW;
        float texUf = st.u0f + (st.u1f - st.u0f) * tx;
        return texUf;
    }

    // Arguments DrawSprite passes to SampleTexture.
    inline void spriteSampleArgs(const GSDrawState &state, float texUf, float texVf,
                                 float &s, float &t, float &q, uint16_t &u, uint16_t &v)
    {
        if (state.prim.fst)
        {
            const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
            const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
            u = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
            v = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
            s = 0.0f;
            t = 0.0f;
            q = 1.0f;
        }
        else
        {
            const int texW = state.textureWidth;
            const int texH = state.textureHeight;
            s = texUf / static_cast<float>(texW);
            t = texVf / static_cast<float>(texH);
            q = 1.0f;
            u = 0u;
            v = 0u;
        }
    }

    void rasterSprite(const QueuedDraw &d, uint8_t *vram, int tx0, int ty0, int tx1, int ty1)
    {
        const GSDrawState &state = d.batch.state;
        const GSVertex &v1 = d.batch.vertices[1];
        const PixelState p(state, vram);
        const u32 z1 = static_cast<u32>(v1.z);
        const uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

        const int minX = std::max(d.minX, tx0);
        const int maxX = std::min(d.maxX, tx1);
        const int minY = std::max(d.minY, ty0);
        const int maxY = std::min(d.maxY, ty1);

        if (state.prim.tme)
        {
            const TexState tex(state, d.colors.get());
            const bool mipmapped = state.mip.maxLevel != 0u;
            const std::optional<MipTex> mipTex = mipmapped ? std::optional<MipTex>(MipTex(d)) : std::nullopt;
            const SpriteTex st = spriteTexSetup(d);
            for (int y = minY; y <= maxY; ++y)
            {
                const float texVf = spriteTexV(d, st, y);
                for (int x = minX; x <= maxX; ++x)
                {
                    PixelMem m;
                    if (!p.earlyTests(x, y, z1, m))
                        continue;
                    const float texUf = spriteTexU(d, st, x);
                    float s, t, q;
                    uint16_t u, v;
                    spriteSampleArgs(state, texUf, texVf, s, t, q, u, v);
                    // A sprite's mipmap LOD uses the Q of its second vertex.
                    const uint32_t texel = mipmapped ? mipTex->sample(s, t, q, u, v, v1.q) : tex.sample(s, t, q, u, v);

                    uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                    uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                    uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                    uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                    const TextureCombineResult color = combineTexture(state.context.tex0, r, g, b, a, tr, tg, tb, ta);
                    p.finish(x, y, z1, color.r, color.g, color.b, color.a, v1.fog, m);
                }
            }
        }
        else
        {
            for (int y = minY; y <= maxY; ++y)
                for (int x = minX; x <= maxX; ++x)
                {
                    PixelMem m;
                    if (!p.earlyTests(x, y, z1, m))
                        continue;
                    p.finish(x, y, z1, r, g, b, a, v1.fog, m);
                }
        }
    }

    // The pixel loop of GSCpuBackend::ClearFramebuffer.
    void rasterClear(const QueuedDraw &d, uint8_t *vram, int tx0, int ty0, int tx1, int ty1)
    {
        const GSFrameReg &frame = d.batch.state.context.frame;
        const uint32_t fbp = GSInternal::framePageBaseToBlock(frame.fbp);
        const uint32_t fbw = std::max<uint32_t>(frame.fbw, 1u);
        const int minX = std::max(d.minX, tx0), maxX = std::min(d.maxX, tx1);
        const int minY = std::max(d.minY, ty0), maxY = std::min(d.maxY, ty1);
        for (int y = minY; y <= maxY; ++y)
            for (int x = minX; x <= maxX; ++x)
            {
                const uint32_t address = pixelAddress(frame.psm, fbp, fbw, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                uint32_t pixel = d.clearValue;
                if (d.clearMask != 0u)
                    pixel = (pixel & ~d.clearMask) | (loadPixel(vram, frame.psm, address) & d.clearMask);
                storePixel(vram, frame.psm, address, pixel);
            }
    }

    uint32_t maxWrappedCoordinate(uint8_t mode, int size, uint16_t regionMin, uint16_t regionMax)
    {
        switch (mode & 0x3u)
        {
        case 0:
        case 1:
            return static_cast<uint32_t>(size - 1);
        case 2:
            return regionMax;
        default:
            return static_cast<uint32_t>(regionMin | regionMax);
        }
    }
}

GSCpuBackend::FastPath::FastPath(GSCpuBackend &backend)
    : owner(backend)
{
    const char *ref = std::getenv("PS2X_GS_REFERENCE");
    reference = ref && ref[0] && ref[0] != '0';
    threads = configuredThreads();
    const char *asyncEnv = std::getenv("PS2X_GS_ASYNC");
    async = !(asyncEnv && asyncEnv[0] == '0');
    cur = std::make_unique<Batch>();
    cur->bins.resize(static_cast<size_t>(kTilesPerRow) * kTilesPerRow);
    const char *statsEnv = std::getenv("PS2X_GS_STATS");
    statsEnabled = statsEnv && statsEnv[0] && statsEnv[0] != '0';
}

GSCpuBackend::FastPath::~FastPath()
{
    if (rasterThread.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(rasterMutex);
            rasterStop = true;
        }
        rasterWake.notify_all();
        rasterThread.join();
    }
}

void GSCpuBackend::FastPath::reset()
{
    join();
    for (uint32_t bin : cur->activeBins)
        cur->bins[bin].clear();
    cur->activeBins.clear();
    cur->prepare.clear();
    cur->draws.clear();
    cur->queuedArea = 0;
    cur->writes.clear();
    cur->targets.clear();
    cacheBase = CacheBase::Invalid;
    lazyPage = 0;
    readersFrom = 0;
    pendingReadPages.clear();
    textures.clear();
    palettes.clear();
    ++clutVersion;
    lastTarget = Target{};
}

void GSCpuBackend::FastPath::textureFlush()
{
    cacheBase = CacheBase::Invalid;
    pendingReadPages.clear();
    readersFrom = cur->draws.size();
}

void GSCpuBackend::FastPath::noteWrite(uint32_t page)
{
    if (reference)
        return;
    page %= GSMem::PageMask::kPages;
    ++pageGen[page];
    if (cacheBase == CacheBase::Lazy && page == lazyPage)
    {
        owner.m_texturePageCache.Load(owner.m_vram, page << kPageShift);
        cacheBase = CacheBase::Eager;
    }
}

void GSCpuBackend::FastPath::prepareSerial()
{
    if (reference)
        return;
    flush();
    switch (cacheBase)
    {
    case CacheBase::Lazy:
        owner.m_texturePageCache.Load(owner.m_vram, lazyPage << kPageShift);
        cacheBase = CacheBase::Eager;
        break;
    case CacheBase::Invalid:
        owner.m_texturePageCache.Invalidate();
        break;
    case CacheBase::Eager:
        break;
    }
}

void GSCpuBackend::FastPath::finishSerial()
{
    if (reference)
        return;
    const auto &cache = owner.m_texturePageCache;
    if (!cache.HasPage())
    {
        cacheBase = CacheBase::Invalid;
        return;
    }
    // A copy that still matches local memory behaves like no copy at all.
    if (std::memcmp(cache.Data(), owner.m_vram + cache.PageBase(), GSMem::TexturePageCache::kPageSize) == 0)
    {
        cacheBase = CacheBase::Lazy;
        lazyPage = cache.PageBase() >> kPageShift;
    }
    else
    {
        cacheBase = CacheBase::Eager;
    }
}

GSCpuBackend::FastPath::Target GSCpuBackend::FastPath::makeTarget(uint32_t fbp, uint32_t fbw, uint8_t fpsm, uint32_t zbp, uint8_t zpsm,
                                                                  bool usesFrame, bool usesZ,
                                                                  uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) const
{
    Target t;
    t.fbp = fbp;
    t.fbw = std::max<uint32_t>(fbw, 1u);
    t.fpsm = fpsm;
    t.zbp = zbp;
    t.zpsm = zpsm;
    t.usesFrame = usesFrame;
    t.usesZ = usesZ;
    t.x0 = static_cast<uint16_t>(x0);
    t.y0 = static_cast<uint16_t>(y0);
    t.x1 = static_cast<uint16_t>(x1);
    t.y1 = static_cast<uint16_t>(y1);
    t.valid = t.x0 <= t.x1 && t.y0 <= t.y1;
    if (t.valid && t.usesFrame)
    {
        t.valid = isFramePsm(t.fpsm) && injective(t.fpsm, t.fbw, t.x0, t.y0, t.x1, t.y1);
        if (t.valid && t.usesZ)
            t.valid = isZPsm(t.zpsm) && injective(t.zpsm, t.fbw, t.x0, t.y0, t.x1, t.y1);
        if (t.valid)
        {
            GSMem::AddRectPages(t.frame, t.fpsm, framePageBaseToBlock(t.fbp), t.fbw, t.x0, t.y0, t.x1, t.y1);
            if (t.usesZ)
                GSMem::AddRectPages(t.z, t.zpsm, framePageBaseToBlock(t.zbp), t.fbw, t.x0, t.y0, t.x1, t.y1);
            t.valid = !t.frame.intersects(t.z);
            t.pages = t.frame;
            t.pages.merge(t.z);
        }
    }
    return t;
}

const GSCpuBackend::FastPath::Target &GSCpuBackend::FastPath::targetFor(const GSDrawState &state)
{
    const auto &ctx = state.context;
    const uint32_t ztestMethod = static_cast<uint32_t>((ctx.test >> 17) & 3u);
    const bool usesFrame = ztestMethod != 0u;
    const bool usesZ = ztestMethod >= 2u || (ztestMethod != 0u && !ctx.zbuf.zmask);
    if (lastTarget.fbp == ctx.frame.fbp && lastTarget.fbw == std::max<uint32_t>(ctx.frame.fbw, 1u) &&
        lastTarget.fpsm == ctx.frame.psm && lastTarget.zbp == ctx.zbuf.zbp && lastTarget.zpsm == ctx.zbuf.psm &&
        lastTarget.usesFrame == usesFrame && lastTarget.usesZ == usesZ && lastTarget.x0 == ctx.scissor.x0 &&
        lastTarget.y0 == ctx.scissor.y0 && lastTarget.x1 == ctx.scissor.x1 && lastTarget.y1 == ctx.scissor.y1)
        return lastTarget;
    lastTarget = makeTarget(ctx.frame.fbp, ctx.frame.fbw, ctx.frame.psm, ctx.zbuf.zbp, ctx.zbuf.psm, usesFrame, usesZ,
                            ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
    return lastTarget;
}

bool GSCpuBackend::FastPath::targetCompatible(const Target &t) const
{
    for (const Target &o : cur->targets)
    {
        if (!o.pages.intersects(t.pages))
            continue;
        // Overlapping targets must be the same buffers, so a pixel lands in
        // the same tile whichever draw writes it.
        if (o.fbp != t.fbp || o.fbw != t.fbw || o.fpsm != t.fpsm)
            return false;
        if (o.usesZ && t.usesZ && (o.zbp != t.zbp || o.zpsm != t.zpsm))
            return false;
        if (o.frame.intersects(t.z) || t.frame.intersects(o.z))
            return false;
        const uint32_t x0 = std::min(o.x0, t.x0), y0 = std::min(o.y0, t.y0);
        const uint32_t x1 = std::max(o.x1, t.x1), y1 = std::max(o.y1, t.y1);
        if (!injective(t.fpsm, t.fbw, x0, y0, x1, y1))
            return false;
        if (o.usesZ && t.usesZ && !injective(t.zpsm, t.fbw, x0, y0, x1, y1))
            return false;
    }
    return true;
}

std::shared_ptr<GSCpuBackend::FastPath::TexSource> *
GSCpuBackend::FastPath::textureFor(const GSDrawState &state, GSMem::PageMask &pages)
{
    const auto &ctx = state.context;
    const auto &tex = ctx.tex0;
    const uint64_t clamp = ctx.clamp;
    const uint32_t width = maxWrappedCoordinate(static_cast<uint8_t>(clamp & 0x3u), state.textureWidth,
                                                static_cast<uint16_t>((clamp >> 4) & 0x3FFu),
                                                static_cast<uint16_t>((clamp >> 14) & 0x3FFu)) + 1u;
    const uint32_t height = maxWrappedCoordinate(static_cast<uint8_t>((clamp >> 2) & 0x3u), state.textureHeight,
                                                 static_cast<uint16_t>((clamp >> 24) & 0x3FFu),
                                                 static_cast<uint16_t>((clamp >> 34) & 0x3FFu)) + 1u;
    const uint64_t key = static_cast<uint64_t>(tex.psm & 0x3Fu) |
                         (static_cast<uint64_t>(tex.tbp0 & 0x3FFFu) << 6u) |
                         (static_cast<uint64_t>(tex.tbw & 0x3Fu) << 20u) |
                         (static_cast<uint64_t>(width - 1u) << 26u) |
                         (static_cast<uint64_t>(height - 1u) << 36u);
    if (textures.size() > 1024u && textures.find(key) == textures.end())
        textures.clear();
    std::shared_ptr<TexSource> &slot = textures[key];
    if (!slot)
    {
        slot = std::make_shared<TexSource>();
        slot->psm = tex.psm;
        slot->tbp0 = tex.tbp0;
        slot->tbw = tex.tbw;
        slot->width = width;
        slot->height = height;
        GSMem::AddRectPages(slot->mask, tex.psm, tex.tbp0, tex.tbw, 0u, 0u, width - 1u, height - 1u);
        for (uint32_t page = 0; page < GSMem::PageMask::kPages; ++page)
            if (slot->mask.test(page))
                slot->pages.push_back(page);
    }
    pages = slot->mask;
    return &slot;
}

std::shared_ptr<const GSCpuBackend::FastPath::Palette> GSCpuBackend::FastPath::paletteFor(const GSDrawState &state)
{
    const auto &tex = state.context.tex0;
    const uint64_t key = static_cast<uint64_t>(tex.psm & 0x3Fu) |
                         (static_cast<uint64_t>(tex.cpsm & 0x3Fu) << 6u) |
                         (static_cast<uint64_t>(tex.csm & 1u) << 12u) |
                         (static_cast<uint64_t>(tex.csa & 0x1Fu) << 13u) |
                         (static_cast<uint64_t>(state.texa.ta0) << 18u) |
                         (static_cast<uint64_t>(state.texa.aem ? 1u : 0u) << 26u) |
                         (static_cast<uint64_t>(state.texa.ta1) << 27u);
    std::shared_ptr<const Palette> &slot = palettes[key];
    if (slot)
        return slot;

    auto palette = std::make_shared<Palette>();
    for (uint32_t index = 0; index < 256u; ++index)
        palette->colors[index] = owner.LookupCLUT(state, static_cast<uint8_t>(index), tex.cpsm, tex.csm, tex.csa, tex.psm);
    ++stats.paletteBuilds;

    // Same colors, same object: texture colors built for it stay usable
    // when the game switches between a few CLUTs.
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t color : palette->colors)
    {
        hash ^= color;
        hash *= 0x100000001B3ull;
    }
    if (internedPalettes.size() > 4096u)
        internedPalettes.clear();
    std::shared_ptr<const Palette> &interned = internedPalettes[hash];
    if (!interned || interned->colors != palette->colors)
        interned = palette;
    slot = interned;
    return slot;
}

std::shared_ptr<const GSCpuBackend::FastPath::TexColors>
GSCpuBackend::FastPath::colorsFor(const std::shared_ptr<TexSource> &sourcePtr, const GSDrawState &state,
                                  const GSDrawState *packState, uint8_t level)
{
    TexSource &source = *sourcePtr;
    const uint8_t psm = source.psm;
    const bool indexed = isFourBitIndexedPsm(psm) || isEightBitIndexedPsm(psm);
    std::shared_ptr<const Palette> palette;
    uint64_t key;
    if (indexed)
    {
        palette = paletteFor(state);
        key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(palette.get()));
    }
    else
    {
        key = (1ull << 63) | static_cast<uint64_t>(state.texa.ta0) |
              (static_cast<uint64_t>(state.texa.aem ? 1u : 0u) << 8u) |
              (static_cast<uint64_t>(state.texa.ta1) << 9u);
    }
    // A texture pack: the colours are also the region's (from CLAMP) and
    // the level's, and are made again once a picture they wait for is read.
    namespace tp = ps2x::texture_pack;
    std::shared_ptr<PackJob> pack;
    uint64_t packGeneration = 0;
    if (packState && tp::active() && (indexed ? true : (tp::hasPictures() || tp::dumpingDirect())))
    {
        const GSTex0Reg &tex = packState->context.tex0;
        const tp::Region r = tp::regionOf(tex.tw, tex.th, packState->context.clamp);
        uint64_t packKey = 0xCBF29CE484222325ull;
        for (uint64_t v : {static_cast<uint64_t>(r.hasX) | (static_cast<uint64_t>(r.hasY) << 1) | (static_cast<uint64_t>(level) << 2),
                           static_cast<uint64_t>(r.minX) | (static_cast<uint64_t>(r.maxX) << 16) |
                               (static_cast<uint64_t>(r.minY) << 32) | (static_cast<uint64_t>(r.maxY) << 48),
                           static_cast<uint64_t>(tp::nameBits(tex.psm, tex.tw, tex.th, packState->texa))})
            packKey = (packKey ^ v) * 0x100000001B3ull;
        key ^= packKey;
        pack = std::make_shared<PackJob>();
        pack->state = *packState;
        pack->level = level;
        if (indexed)
        {
            pack->clutCount = tp::readClut(owner.m_clut.data(), tex.psm, tex.cpsm, tex.csa, packState->texa, pack->clut.data());
            pack->clutHash = tp::clutHash(pack->clut.data(), pack->clutCount);
            key ^= pack->clutHash * 0x9E3779B97F4A7C15ull;
        }
        packGeneration = tp::generation();
    }
    for (size_t i = 0; i < source.colorCache.size(); ++i)
    {
        const ColorCacheEntry &entry = source.colorCache[i];
        if (entry.key != key)
            continue;
        if (!(pack && entry.packGeneration != packGeneration && entry.colors->packWaiting.load(std::memory_order_acquire)))
            return entry.colors;
        source.colorCache.erase(source.colorCache.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }

    // Filled on the raster thread (prepareBatch) before any tile reads it.
    auto colors = std::make_shared<TexColors>();
    colors->width = source.width;
    PrepareJob job;
    job.colors = colors;
    job.source = sourcePtr;
    job.palette = palette;
    job.texa = state.texa;
    job.pack = std::move(pack);
    cur->prepare.push_back(std::move(job));
    ++stats.colorBuilds;
    stats.colorTexels += static_cast<uint64_t>(source.width) * source.height;

    if (source.colorCache.size() >= 8u)
        source.colorCache.erase(source.colorCache.begin());
    source.colorCache.push_back({key, packGeneration, palette, colors});
    return colors;
}

void GSCpuBackend::FastPath::reportStats()
{
    std::fprintf(stderr,
                 "[gs:fast] queued=%llu serial=%llu batches=%llu async=%llu area=%llu decodes=%llu texels=%llu "
                 "colorBuilds=%llu colorTexels=%llu palettes=%llu clutLoads=%llu clutChanges=%llu\n",
                 static_cast<unsigned long long>(stats.queued), static_cast<unsigned long long>(stats.serial),
                 static_cast<unsigned long long>(stats.flushes), static_cast<unsigned long long>(stats.parallelFlushes),
                 static_cast<unsigned long long>(stats.queuedArea), static_cast<unsigned long long>(stats.decodes),
                 static_cast<unsigned long long>(stats.decodedTexels), static_cast<unsigned long long>(stats.colorBuilds),
                 static_cast<unsigned long long>(stats.colorTexels), static_cast<unsigned long long>(stats.paletteBuilds),
                 static_cast<unsigned long long>(stats.clutLoads), static_cast<unsigned long long>(stats.clutChanges));
    stats = Stats{};
}

bool GSCpuBackend::FastPath::setupTriangle(QueuedDraw &d) const
{
    const GSDrawState &state = d.batch.state;
    const GSVertex &v0 = d.batch.vertices[0];
    const GSVertex &v1 = d.batch.vertices[1];
    const GSVertex &v2 = d.batch.vertices[2];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return false;

    d.fx0 = fx0;
    d.fy0 = fy0;
    d.fx1 = fx1;
    d.fy1 = fy1;
    d.fx2 = fx2;
    d.fy2 = fy2;
    d.winding = (denom < 0.0f) ? -1.0f : 1.0f;
    d.invAbsDenom = 1.0f / std::fabs(denom);
    d.minX = minX;
    d.maxX = maxX;
    d.minY = minY;
    d.maxY = maxY;
    return true;
}

bool GSCpuBackend::FastPath::setupSprite(QueuedDraw &d) const
{
    const GSDrawState &state = d.batch.state;
    const GSVertex &v0 = d.batch.vertices[0];
    const GSVertex &v1 = d.batch.vertices[1];
    const auto &ctx = state.context;

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
        return false;

    d.unclippedX0 = unclippedX0;
    d.unclippedY0 = unclippedY0;
    d.spanX = spanX;
    d.spanY = spanY;
    d.minX = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    d.minY = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    d.maxX = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    d.maxY = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);
    return true;
}

namespace
{
    // PS2X_TEXTURE_PACK_LOD_STATS=1: which of PCSX2's names (with its
    // default, hardware mipmapping on) each textured primitive's texture
    // gets, and whether texture_pack::candidateNames has it, printed at
    // exit. PCSX2 takes the LOD range over a whole draw (GSVertexTrace:
    // log2(1/|Q|) * 2^L + K at the extreme Q), here per primitive.
    struct Pcsx2NameStats
    {
        std::atomic<uint64_t> flat{0};       // no mip levels: its level 0
        std::atomic<uint64_t> magnified{0};  // mipmapped, LOD <= 0 everywhere: level 0 alone
        std::atomic<uint64_t> fromLevel0{0}; // levels 0..MXL
        std::atomic<uint64_t> fromLower{0};  // levels k..MXL, k > 0
        std::atomic<uint64_t> unmatched{0};  // a name over level 0 at block 0 (the frame buffer)
        ~Pcsx2NameStats()
        {
            const uint64_t total = flat + magnified + fromLevel0 + fromLower + unmatched;
            if (total == 0u)
                return;
            std::fprintf(stderr,
                         "[texture-pack] PCSX2 names of %llu textured primitives: %llu without mip levels, mipmapped: "
                         "%llu level 0 alone, %llu levels 0..MXL, %llu levels k..MXL (k > 0), %llu over a level 0 in the "
                         "frame buffer (no stable name); ours have %.2f%%\n",
                         static_cast<unsigned long long>(total), static_cast<unsigned long long>(flat.load()),
                         static_cast<unsigned long long>(magnified.load()), static_cast<unsigned long long>(fromLevel0.load()),
                         static_cast<unsigned long long>(fromLower.load()), static_cast<unsigned long long>(unmatched.load()),
                         100.0 * static_cast<double>(total - unmatched) / static_cast<double>(total));
        }
    };

    void countPcsx2Name(const GSPrimitiveBatch &batch)
    {
        static const bool on = []
        {
            const char *v = std::getenv("PS2X_TEXTURE_PACK_LOD_STATS");
            return v && *v && *v != '0';
        }();
        if (!on || !batch.state.prim.tme)
            return;
        static Pcsx2NameStats stats;
        const GSDrawState &st = batch.state;
        const GSMipInfo &m = st.mip;
        if (m.maxLevel == 0u)
        {
            ++stats.flat;
            return;
        }
        const float scale = static_cast<float>(1u << m.l);
        float lo = m.k, hi = m.k;
        if (!m.lcm && !st.prim.fst)
        {
            float qmin = 1.0e30f, qmax = 0.0f;
            const uint32_t n = std::min<uint32_t>(batch.vertexCount, 3u);
            for (uint32_t i = 0; i < n; ++i)
            {
                const float q = std::fabs(batch.vertices[i].q);
                qmin = std::min(qmin, q);
                qmax = std::max(qmax, q);
            }
            lo = -std::log2(std::max(qmax, 1.0e-30f)) * scale + m.k;
            hi = -std::log2(std::max(qmin, 1.0e-30f)) * scale + m.k;
        }
        const bool frameLevel0 = st.context.tex0.tbp0 == 0u;
        if (hi <= 0.0f)
        {
            ++(frameLevel0 ? stats.unmatched : stats.magnified);
            return;
        }
        int base;
        if (m.lcm || st.prim.fst)
            base = std::max(static_cast<int>(std::floor(m.k + 0.5f)), 0);
        else if (m.mipLinear)
            base = std::max(static_cast<int>(std::floor(lo)), 0);
        else
            base = std::max(static_cast<int>(std::round(lo + 0.0625f)), 0);
        base = std::min<int>(base, m.maxLevel);
        if (base == 0)
            ++(frameLevel0 ? stats.unmatched : stats.fromLevel0);
        else
            ++stats.fromLower;
    }
}

bool GSCpuBackend::FastPath::tryQueue(const GSPrimitiveBatch &batch)
{
    countPcsx2Name(batch);
    if (reference)
        return false;

    const GSDrawState &state = batch.state;
    const auto &ctx = state.context;
    const bool sprite = state.prim.type == GS_PRIM_SPRITE;
    const bool triangle = state.prim.type == GS_PRIM_TRIANGLE || state.prim.type == GS_PRIM_TRISTRIP ||
                          state.prim.type == GS_PRIM_TRIFAN;
    if (!sprite && !triangle)
        return false;
    if (ctx.scissor.x0 > ctx.scissor.x1 || ctx.scissor.y0 > ctx.scissor.y1)
        return false;
    if (state.prim.tme && !isTexturePsm(ctx.tex0.psm))
        return false;

    const Target &target = targetFor(state);
    if (!target.valid)
        return false;

    std::shared_ptr<TexSource> *texture = nullptr;
    GSMem::PageMask readPages;
    const bool mipmapped = state.prim.tme && state.mip.maxLevel != 0u;
    uint8_t mipLo = 0, mipHi = 0;
    std::array<std::shared_ptr<TexSource> *, 7> levelSlots{};
    if (mipmapped)
    {
        mipRange(batch, sprite, mipLo, mipHi);
        // textureFor may clear the map: not between the levels.
        if (textures.size() > 1024u - 7u)
            textures.clear();
        for (uint32_t k = mipLo; k <= mipHi; ++k)
        {
            GSMem::PageMask levelPages;
            levelSlots[k] = textureFor(mipLevelState(state, static_cast<uint8_t>(k)), levelPages);
            readPages.merge(levelPages);
        }
        if (readPages.intersects(target.pages))
            return false; // reads what it writes
    }
    else if (state.prim.tme)
    {
        texture = textureFor(state, readPages);
        if (readPages.intersects(target.pages))
            return false; // reads what it writes
    }

    QueuedDraw d;
    d.batch = batch;
    d.sprite = sprite;
    if (!(sprite ? setupSprite(d) : setupTriangle(d)))
        return true; // draws nothing and reads nothing

    if (state.prim.tme)
    {
        if (readPages.intersects(cur->writes))
            handOff();
        // The cached copy may be stale: only the reference rasterizer reads it.
        if (cacheBase == CacheBase::Eager && readPages.test(owner.m_texturePageCache.PageBase() >> kPageShift))
        {
            if (!pendingReadPages.empty())
                handOff();
            if (cacheBase == CacheBase::Eager && readPages.test(owner.m_texturePageCache.PageBase() >> kPageShift))
                return false;
        }
    }
    admitTarget(target);

    // The texels are read on the raster thread when this batch starts:
    // no queued draw before it writes them, earlier batches are done.
    auto prepareTexture = [&](std::shared_ptr<TexSource> &slot)
    {
        bool valid = slot->gens.size() == slot->pages.size();
        for (size_t i = 0; valid && i < slot->pages.size(); ++i)
            valid = slot->gens[i] == pageGen[slot->pages[i]];
        if (!valid)
        {
            auto fresh = std::make_shared<TexSource>();
            fresh->psm = slot->psm;
            fresh->tbp0 = slot->tbp0;
            fresh->tbw = slot->tbw;
            fresh->width = slot->width;
            fresh->height = slot->height;
            fresh->mask = slot->mask;
            fresh->pages = slot->pages;
            fresh->gens.resize(fresh->pages.size());
            for (size_t i = 0; i < fresh->pages.size(); ++i)
                fresh->gens[i] = pageGen[fresh->pages[i]];
            slot = fresh;
            PrepareJob job;
            job.decode = fresh;
            cur->prepare.push_back(std::move(job));
            ++stats.decodes;
            stats.decodedTexels += static_cast<uint64_t>(fresh->width) * fresh->height;
        }
    };
    if (mipmapped)
    {
        d.mipLo = mipLo;
        d.mipHi = mipHi;
        for (uint32_t k = mipLo; k <= mipHi; ++k)
        {
            prepareTexture(*levelSlots[k]);
            d.levelColors[k] = colorsFor(*levelSlots[k], mipLevelState(state, static_cast<uint8_t>(k)), &state,
                                         static_cast<uint8_t>(k));
        }
    }
    else if (state.prim.tme)
    {
        prepareTexture(*texture);
        d.colors = colorsFor(*texture, state, &state, 0);
    }

    enqueue(std::move(d), target);
    if (state.prim.tme)
        pendingReadPages.merge(readPages);
    if (!deferred)
        flush();
    else if (cur->draws.size() >= kMaxQueuedDraws || cur->targets.size() > 16u)
        handOff();
    return true;
}

void GSCpuBackend::FastPath::admitTarget(const Target &target)
{
    if (!targetCompatible(target))
        handOff();
    // A cached page that this draw writes must be copied first.
    if (!target.pages.empty())
    {
        bool hit = target.pages.intersects(pendingReadPages);
        if (cacheBase == CacheBase::Lazy && target.pages.test(lazyPage))
            hit = true;
        if (hit)
        {
            handOff();
            if (cacheBase == CacheBase::Lazy && target.pages.test(lazyPage))
            {
                GSMem::PageMask page;
                page.set(lazyPage);
                waitFor(page);
                owner.m_texturePageCache.Load(owner.m_vram, lazyPage << kPageShift);
                cacheBase = CacheBase::Eager;
            }
        }
    }
}

void GSCpuBackend::FastPath::enqueue(QueuedDraw &&d, const Target &target)
{
    ++stats.queued;
    Batch &b = *cur;
    const uint32_t index = static_cast<uint32_t>(b.draws.size());
    if (target.usesFrame)
    {
        for (uint32_t ty = static_cast<uint32_t>(d.minY) >> kTileShift; ty <= static_cast<uint32_t>(d.maxY) >> kTileShift; ++ty)
            for (uint32_t tx = static_cast<uint32_t>(d.minX) >> kTileShift; tx <= static_cast<uint32_t>(d.maxX) >> kTileShift; ++tx)
            {
                const uint32_t bin = ty * kTilesPerRow + tx;
                if (b.bins[bin].empty())
                    b.activeBins.push_back(bin);
                b.bins[bin].push_back(index);
            }
        const uint64_t area = static_cast<uint64_t>(d.maxX - d.minX + 1) * static_cast<uint64_t>(d.maxY - d.minY + 1);
        b.queuedArea += area;
        stats.queuedArea += area;
        b.writes.merge(target.pages);
        bool known = false;
        for (const Target &o : b.targets)
            if (o.fbp == target.fbp && o.fbw == target.fbw && o.fpsm == target.fpsm && o.zbp == target.zbp &&
                o.zpsm == target.zpsm && o.usesZ == target.usesZ && o.x0 == target.x0 && o.y0 == target.y0 &&
                o.x1 == target.x1 && o.y1 == target.y1)
                known = true;
        if (!known)
            b.targets.push_back(target);
    }
    b.draws.push_back(std::move(d));
}

bool GSCpuBackend::FastPath::queueClear(const GSContext &context, uint32_t value, uint32_t mask,
                                        uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (reference || std::getenv("NOCLEAR"))
        return false;
    const Target target = makeTarget(context.frame.fbp, context.frame.fbw, context.frame.psm, 0u, 0u, true, false, x0, y0, x1, y1);
    if (!target.valid)
        return false;
    admitTarget(target);
    QueuedDraw d;
    d.batch.state.context = context;
    d.clear = true;
    d.clearValue = value;
    d.clearMask = mask;
    d.minX = static_cast<int>(x0);
    d.minY = static_cast<int>(y0);
    d.maxX = static_cast<int>(x1);
    d.maxY = static_cast<int>(y1);
    enqueue(std::move(d), target);
    if (!deferred)
        flush();
    else if (cur->draws.size() >= kMaxQueuedDraws || cur->targets.size() > 16u)
        handOff();
    return true;
}

void GSCpuBackend::FastPath::rasterTile(Batch &batch, uint32_t bin)
{
    const int tx0 = static_cast<int>((bin % kTilesPerRow) << kTileShift);
    const int ty0 = static_cast<int>((bin / kTilesPerRow) << kTileShift);
    const int tx1 = tx0 + (1 << kTileShift) - 1;
    const int ty1 = ty0 + (1 << kTileShift) - 1;
    for (uint32_t index : batch.bins[bin])
    {
        const QueuedDraw &d = batch.draws[index];
        if (d.clear)
            rasterClear(d, owner.m_vram, tx0, ty0, tx1, ty1);
        else if (d.sprite)
            rasterSprite(d, owner.m_vram, tx0, ty0, tx1, ty1);
        else
            rasterTriangle(d, owner.m_vram, tx0, ty0, tx1, ty1);
    }
}

bool GSCpuBackend::FastPath::lastReadPage(const QueuedDraw &d, uint32_t &page) const
{
    const GSDrawState &state = d.batch.state;
    const TexState tex(state, nullptr);
    float s, t, q, lodQ;
    uint16_t u, v;
    if (d.sprite)
    {
        const SpriteTex st = spriteTexSetup(d);
        spriteSampleArgs(state, spriteTexU(d, st, d.maxX), spriteTexV(d, st, d.maxY), s, t, q, u, v);
        lodQ = d.batch.vertices[1].q;
    }
    else
    {
        bool found = false;
        float w0 = 0, w1 = 0, w2 = 0;
        for (int y = d.maxY; y >= d.minY && !found; --y)
            for (int x = d.maxX; x >= d.minX; --x)
                if (triangleWeights(d, x, y, w0, w1, w2))
                {
                    found = true;
                    break;
                }
        if (!found)
            return false;
        triangleTexArgs(d, w0, w1, w2, s, t, q, u, v);
        const GSVertex *vx = d.batch.vertices.data();
        lodQ = vx[0].q * w0 + vx[1].q * w1 + vx[2].q * w2;
    }
    int su, sv;
    uint32_t tbp = state.context.tex0.tbp0, tbw = state.context.tex0.tbw;
    if (state.mip.maxLevel != 0u)
    {
        // The last level SampleTexture reads: the second one when it blends.
        const GSMipSample m = gsMipSelect(state.mip, lodQ);
        const uint8_t level = (m.level2 != m.level && m.frac > 0.0f) ? m.level2 : m.level;
        const TexState levelTex(state, nullptr, level);
        levelTex.lastReadFiltered(s, t, q, u, v, m.linear, su, sv);
        tbp = state.mip.tbp[level];
        tbw = state.mip.tbw[level];
    }
    else
        tex.lastRead(s, t, q, u, v, su, sv);
    const uint32_t address = GSMem::ByteAddress(state.context.tex0.psm, tbp, tbw,
                                                static_cast<uint32_t>(su), static_cast<uint32_t>(sv));
    page = address >> kPageShift;
    return true;
}

void GSCpuBackend::FastPath::resolvePending()
{
    if (pendingReadPages.empty())
        return;
    for (size_t i = cur->draws.size(); i-- > readersFrom;)
    {
        const QueuedDraw &d = cur->draws[i];
        uint32_t page;
        if (d.batch.state.prim.tme && lastReadPage(d, page))
        {
            cacheBase = CacheBase::Lazy;
            lazyPage = page;
            break;
        }
    }
    pendingReadPages.clear();
    readersFrom = cur->draws.size();
}

void GSCpuBackend::FastPath::prepareBatch(Batch &batch)
{
    for (PrepareJob &job : batch.prepare)
    {
        if (job.decode)
        {
            TexSource &src = *job.decode;
            src.texels.resize(static_cast<size_t>(src.width) * src.height);
            const auto read = owner.m_readVramFuncs[src.psm & 0x3Fu];
            for (uint32_t v = 0; v < src.height; ++v)
                for (uint32_t u = 0; u < src.width; ++u)
                    src.texels[static_cast<size_t>(v) * src.width + u] = read(owner.m_vram, src.tbp0, src.tbw, u, v);
            continue;
        }
        // What SampleTexture's samplePoint returns for each texel.
        const TexSource &source = *job.source;
        const uint8_t psm = source.psm;
        const GSTexaReg &texa = job.texa;
        std::vector<uint32_t> &colors = job.colors->colors;
        colors.resize(source.texels.size());
        for (size_t i = 0; i < source.texels.size(); ++i)
        {
            const u32 out = source.texels[i];
            uint32_t color = 0xFFFF00FFu;
            switch (psm)
            {
            case GS_PSM_CT32:
            case GS_PSM_Z32:
            case GS_PSM_CT24:
            case GS_PSM_Z24:
                color = applyTexa(texa, psm, out);
                break;
            case GS_PSM_CT16:
            case GS_PSM_CT16S:
            case GS_PSM_Z16:
            case GS_PSM_Z16S:
                color = applyTexa(texa, psm, Rgba5551ToRgba8888(out));
                break;
            case GS_PSM_T8:
            case GS_PSM_T8H:
            case GS_PSM_T4:
            case GS_PSM_T4HL:
            case GS_PSM_T4HH:
                color = job.palette->colors[static_cast<u8>(out)];
                break;
            }
            colors[i] = color;
        }
        if (job.pack)
            applyPack(job);
    }
}

// The texture's PCSX2 name (runtime/ps2_texture_pack.h) from local memory
// as the batch reads it, and a pack's picture of it at the level's size in
// place of the colours: the CPU renderer draws at the PS2's resolution, so
// a larger picture is box-filtered down to the texture's own texels (its
// region's, for a region's picture). The rest of the texture keeps its
// colours. Dump mode writes the name if no pack has it.
void GSCpuBackend::FastPath::applyPack(PrepareJob &job)
{
    namespace tp = ps2x::texture_pack;
    const PackJob &pj = *job.pack;
    const GSDrawState &st = pj.state;
    const GSTex0Reg &tex = st.context.tex0;
    const bool palette = tp::hasPalette(tex.psm);
    const bool mipmapped = st.mip.maxLevel != 0u;
    tp::Level levels[7];
    const uint32_t count = mipmapped ? std::min<uint32_t>(st.mip.maxLevel + 1u, 7u) : 1u;
    for (uint32_t k = 0; k < count; ++k)
    {
        levels[k].tbp = k == 0u ? tex.tbp0 : st.mip.tbp[k];
        levels[k].tbw = k == 0u ? tex.tbw : st.mip.tbw[k];
        levels[k].twLog2 = tex.tw > k ? tex.tw - k : 0u;
        levels[k].thLog2 = tex.th > k ? tex.th - k : 0u;
    }
    tp::Candidate names[8];
    const uint32_t found = tp::candidateShapes(tex.psm, levels, count, st.context.clamp, st.texa, palette ? pj.clutHash : 0u,
                                               names);
    if (found == 0u)
        return;
    // A name is hashed when a pack may have it (or dump mode wants the first).
    bool known = false;
    ps2x::texture_replace::PicturePtr picture;
    uint32_t chosen = 0;
    for (uint32_t i = 0; i < found && !known; ++i)
    {
        if (!tp::mayHave(names[i].name))
            continue;
        tp::hashCandidate(owner.m_vram, tex.psm, levels, st.texa, names[i]);
        picture = tp::find(names[i].name, &known);
        chosen = i;
    }
    if (!known && pj.level == 0u && tp::dumping() && (palette || tp::dumpingDirect()))
        tp::hashCandidate(owner.m_vram, tex.psm, levels, st.texa, names[0]);
    const tp::Name &first = names[0].name;
    if (!known && pj.level == 0u && tp::dumping() && (palette || tp::dumpingDirect()))
    {
        bool fresh;
        {
            std::lock_guard<std::mutex> lock(packDumpedMutex);
            fresh = packDumped.insert(tp::NameHash{}(first)).second;
        }
        if (fresh)
        {
            const uint32_t b = names[0].base;
            tp::Region r = names[0].region0;
            r.minX >>= b;
            r.maxX >>= b;
            r.minY >>= b;
            r.maxY >>= b;
            const uint32_t w = first.regionWidth ? first.regionWidth : (1u << levels[b].twLog2);
            const uint32_t h = first.regionHeight ? first.regionHeight : (1u << levels[b].thLog2);
            tp::dump(first, w, h, tp::readColors(owner.m_vram, tex.psm, levels[b], r, st.texa, palette ? pj.clut.data() : nullptr));
        }
    }
    const tp::Region region = names[chosen].region0;
    if (!picture)
    {
        job.colors->packWaiting.store(known, std::memory_order_release);
        return;
    }
    // The level's part the picture covers.
    const tp::Region rk = pj.level == 0u ? region : region.atLevel(pj.level);
    const uint32_t lw = std::max<uint32_t>((1u << tex.tw) >> pj.level, 1u), lh = std::max<uint32_t>((1u << tex.th) >> pj.level, 1u);
    const uint32_t rw = static_cast<uint32_t>(rk.width(lw)), rh = static_cast<uint32_t>(rk.height(lh));
    const uint32_t ox = rk.hasX ? static_cast<uint32_t>(rk.minX) : 0u, oy = rk.hasY ? static_cast<uint32_t>(rk.minY) : 0u;
    if (rw == 0u || rh == 0u)
        return;
    const std::vector<uint32_t> pixels = tp::colorsAt(*picture, rw, rh);
    std::vector<uint32_t> &colors = job.colors->colors;
    const uint32_t width = job.colors->width;
    const uint32_t height = width ? static_cast<uint32_t>(colors.size() / width) : 0u;
    for (uint32_t y = 0; y < rh && oy + y < height; ++y)
        for (uint32_t x = 0; x < rw && ox + x < width; ++x)
            colors[static_cast<size_t>(oy + y) * width + ox + x] = pixels[static_cast<size_t>(y) * rw + x];
}

void GSCpuBackend::FastPath::runBatch(Batch &batch)
{
    prepareBatch(batch);
    if (threads > 1u && batch.activeBins.size() > 1u && batch.queuedArea >= kParallelMinArea)
    {
        struct Job
        {
            FastPath *self;
            Batch *batch;
        } job{this, &batch};
        workerPool(threads).run(static_cast<uint32_t>(batch.activeBins.size()), [](void *ctx, uint32_t item)
                                {
                                    auto *j = static_cast<Job *>(ctx);
                                    j->self->rasterTile(*j->batch, j->batch->activeBins[item]); }, &job);
    }
    else
    {
        for (uint32_t bin : batch.activeBins)
            rasterTile(batch, bin);
    }
}

void GSCpuBackend::FastPath::rasterLoop()
{
    for (;;)
    {
        std::unique_ptr<Batch> batch;
        {
            std::unique_lock<std::mutex> lock(rasterMutex);
            rasterWake.wait(lock, [&]
                            { return rasterStop || !rasterQueue.empty(); });
            // On destruction queued batches are dropped: nobody can observe
            // them, and the local memory buffer may already be gone.
            if (rasterStop)
                return;
            batch = std::move(rasterQueue.front());
            rasterQueue.pop_front();
        }
        runBatch(*batch);
        {
            std::lock_guard<std::mutex> lock(rasterMutex);
            retired.push_back(std::move(batch));
            completedSeq.fetch_add(1u, std::memory_order_release);
        }
        rasterDone.notify_all();
    }
}

void GSCpuBackend::FastPath::recycle()
{
    std::vector<std::unique_ptr<Batch>> done;
    {
        std::lock_guard<std::mutex> lock(rasterMutex);
        done.swap(retired);
    }
    for (auto &batch : done)
    {
        for (uint32_t bin : batch->activeBins)
            batch->bins[bin].clear();
        batch->activeBins.clear();
        batch->prepare.clear();
        batch->draws.clear();
        batch->queuedArea = 0;
        batch->writes.clear();
        batch->targets.clear();
        freeBatches.push_back(std::move(batch));
    }
}

void GSCpuBackend::FastPath::handOff()
{
    if (cur->draws.empty())
        return;

    ++stats.flushes;
    // Everything the producer needs from these draws (texture cache state,
    // write generations) is settled now; the pixels follow.
    resolvePending();
    for (uint32_t page = 0; page < GSMem::PageMask::kPages; ++page)
        if (cur->writes.test(page))
            ++pageGen[page];
    readersFrom = 0;

    if (!async || threads <= 1u || !deferred)
    {
        join();
        runBatch(*cur);
        ++handedSeq;
        {
            std::lock_guard<std::mutex> lock(rasterMutex);
            retired.push_back(std::move(cur));
            completedSeq.fetch_add(1u, std::memory_order_release);
        }
    }
    else
    {
        if (!rasterThread.joinable())
            rasterThread = std::thread([this]
                                       { rasterLoop(); });
        if (handedSeq - completedSeq.load(std::memory_order_acquire) >= kMaxInflight)
        {
            std::unique_lock<std::mutex> lock(rasterMutex);
            rasterDone.wait(lock, [&]
                            { return handedSeq - completedSeq.load(std::memory_order_acquire) < kMaxInflight; });
        }
        if (completedSeq.load(std::memory_order_acquire) == handedSeq)
            inflightWrites.clear();
        inflightWrites.merge(cur->writes);
        ++handedSeq;
        {
            std::lock_guard<std::mutex> lock(rasterMutex);
            rasterQueue.push_back(std::move(cur));
        }
        rasterWake.notify_one();
        ++stats.parallelFlushes;
    }

    recycle();
    if (!freeBatches.empty())
    {
        cur = std::move(freeBatches.back());
        freeBatches.pop_back();
    }
    else
    {
        cur = std::make_unique<Batch>();
        cur->bins.resize(static_cast<size_t>(kTilesPerRow) * kTilesPerRow);
    }
}

void GSCpuBackend::FastPath::join()
{
    if (completedSeq.load(std::memory_order_acquire) != handedSeq)
    {
        std::unique_lock<std::mutex> lock(rasterMutex);
        rasterDone.wait(lock, [&]
                        { return completedSeq.load(std::memory_order_acquire) == handedSeq; });
    }
    inflightWrites.clear();
    recycle();
}

bool GSCpuBackend::FastPath::inflightTouches(const GSMem::PageMask &pages)
{
    if (completedSeq.load(std::memory_order_acquire) == handedSeq)
    {
        inflightWrites.clear();
        return false;
    }
    return inflightWrites.intersects(pages);
}

void GSCpuBackend::FastPath::waitFor(const GSMem::PageMask &pages)
{
    if (inflightTouches(pages))
        join();
}

void GSCpuBackend::FastPath::flush()
{
    handOff();
    join();
}

void GSCpuBackend::FastPath::loadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    const bool fourBit = isFourBitIndexedPsm(tex0.psm);
    const bool sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
    const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
    if (!sixteenBit && !thirtyTwoBit)
        return; // LoadClutUnlocked reads nothing

    // Local memory LoadClutUnlocked reads (see there).
    const uint32_t entryCount = fourBit ? 16u : 256u;
    const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
    const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;
    const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
    const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;
    GSMem::PageMask pages;
    if (tex0.csm == 0u)
    {
        GSMem::AddRectPages(pages, tex0.cpsm, tex0.cbp, 1u, 0u, 0u, 15u, fourBit ? 1u : 15u);
    }
    else
    {
        const uint32_t width = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
        const uint32_t x0 = (static_cast<uint32_t>(texclut.cou) << 4u) + firstEntry;
        const uint32_t x1 = (static_cast<uint32_t>(texclut.cou) << 4u) + entryCount - 1u;
        GSMem::AddRectPages(pages, tex0.cpsm, tex0.cbp, width, x0, texclut.cov, x1, texclut.cov);
    }

    ++stats.clutLoads;
    if (pages.intersects(cur->writes))
        handOff();
    waitFor(pages);
    const std::array<uint16_t, 512> before = owner.m_clut;
    bool serial = false;
    if (cacheBase == CacheBase::Eager && pages.test(owner.m_texturePageCache.PageBase() >> kPageShift))
    {
        if (!pendingReadPages.empty())
            handOff();
        serial = cacheBase == CacheBase::Eager && pages.test(owner.m_texturePageCache.PageBase() >> kPageShift);
    }
    if (serial)
    {
        // The stale copy is read: use the cache as the reference path does.
        prepareSerial();
        owner.LoadClutUnlocked(tex0, texclut, false);
        finishSerial();
    }
    else
    {
        // Every read returns local memory; the cache ends up holding the
        // page of the last one.
        const uint32_t address = owner.LoadClutUnlocked(tex0, texclut, true);
        if (address != UINT32_MAX)
        {
            cacheBase = CacheBase::Lazy;
            lazyPage = address >> kPageShift;
            pendingReadPages.clear();
            readersFrom = cur->draws.size();
        }
    }
    if (before != owner.m_clut)
    {
        ++stats.clutChanges;
        clutChanged();
    }
}
