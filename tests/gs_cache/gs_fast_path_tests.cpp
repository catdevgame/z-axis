// Differential tests for the queued/tiled fast path of GSCpuBackend
// (gs_cpu_fast.cpp): a random stream of draws, CLUT loads, TEXFLUSHes,
// uploads, copies and clears goes to a backend running the reference
// rasterizer (PS2X_GS_REFERENCE=1) and to one running the fast path; local
// memory must stay identical bit for bit.
#include "gs_test_support.h"
#include "../../runtime/src/lib/gs/gs_cpu_shared.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

using namespace GSTest;

namespace
{
    // PS2X_GS_REFERENCE set to "1" or removed (Windows has _putenv_s, an
    // empty value removes the variable there).
    void setReference(bool on)
    {
#if defined(_WIN32)
        _putenv_s("PS2X_GS_REFERENCE", on ? "1" : "");
#else
        if (on)
            setenv("PS2X_GS_REFERENCE", "1", 1);
        else
            unsetenv("PS2X_GS_REFERENCE");
#endif
    }

    struct Rng
    {
        uint64_t state;
        explicit Rng(uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1u) {}
        uint32_t next()
        {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            return static_cast<uint32_t>(state >> 16);
        }
        uint32_t below(uint32_t n) { return n ? next() % n : 0u; }
        bool chance(uint32_t percent) { return below(100u) < percent; }
        int range(int lo, int hi) { return lo + static_cast<int>(below(static_cast<uint32_t>(hi - lo + 1))); }
        float unit() { return static_cast<float>(next() & 0xFFFFFFu) / static_cast<float>(0x1000000); }
        template <typename T, size_t N>
        T pick(const T (&values)[N]) { return values[below(N)]; }
    };

    struct Options
    {
        uint64_t seed = 1;
        bool deferred = true;
        uint32_t events = 2500;
        uint32_t feedbackPercent = 3;  // draws that target or sample each other's pages
        uint32_t largePercent = 10;    // large triangles
        uint32_t renderTargetPercent = 0; // draws into / sampling a small render target
        bool oneTexture = false;          // every draw samples the same texture
        uint32_t texflushPercent = 5;
    };

    constexpr uint32_t kTexBase = 8192u;  // block of the first texture (page 256)
    constexpr uint32_t kClutBase = 12800u; // page 400
    constexpr uint32_t kRtPage = 330u;     // small render target, 128x64 CT32
    constexpr uint32_t kWitnessPage = 360u; // 128x128 CT32, written only by witness sprites

    void randomizeTexture(Rng &rng, GSDrawState &state, bool feedback, const Options &options, bool sampleTarget = false)
    {
        auto &tex = state.context.tex0;
        static const uint32_t bases[] = {kTexBase, kTexBase + 64u, kTexBase + 8u, kTexBase + 1000u, kTexBase + 2047u};
        tex.tbp0 = options.oneTexture ? kTexBase : rng.pick(bases);
        if (feedback)
            tex.tbp0 = rng.chance(50) ? 0u : 64u * 32u; // frame or Z buffer
        tex.tbw = static_cast<uint8_t>(options.oneTexture ? 2 : rng.range(1, 4));
        if (sampleTarget)
        {
            tex.tbp0 = kRtPage * 32u;
            tex.tbw = 2;
        }
        static const uint8_t psms[] = {GS_PSM_T8, GS_PSM_T8, GS_PSM_T4, GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16,
                                       GS_PSM_CT16S, GS_PSM_T8H, GS_PSM_T4HL, GS_PSM_T4HH, GS_PSM_Z16, GS_PSM_Z32};
        tex.psm = rng.pick(psms);
        tex.tw = static_cast<uint8_t>(rng.range(1, options.oneTexture || sampleTarget ? 5 : 7));
        tex.th = static_cast<uint8_t>(rng.range(1, options.oneTexture || sampleTarget ? 5 : 7));
        tex.tcc = static_cast<uint8_t>(rng.below(2));
        tex.tfx = static_cast<uint8_t>(rng.below(4));
        static const uint32_t cbps[] = {kClutBase, kClutBase + 4u, kClutBase + 32u};
        tex.cbp = rng.pick(cbps);
        if (options.renderTargetPercent && rng.chance(30))
            tex.cbp = kRtPage * 32u + rng.below(2) * 4u; // a CLUT drawn by the GS
        static const uint8_t cpsms[] = {GS_PSM_CT32, GS_PSM_CT32, GS_PSM_CT16, GS_PSM_CT16S, GS_PSM_CT24};
        tex.cpsm = rng.pick(cpsms);
        tex.csm = static_cast<uint8_t>(rng.chance(80) ? 0 : 1);
        tex.csa = static_cast<uint8_t>(rng.chance(70) ? 0 : rng.below(32));
        tex.cld = 0;
        state.textureWidth = static_cast<uint16_t>(1u << std::min<uint32_t>(tex.tw, 10u));
        state.textureHeight = static_cast<uint16_t>(1u << std::min<uint32_t>(tex.th, 10u));
        uint64_t clamp = rng.below(16);
        clamp |= static_cast<uint64_t>(rng.below(128)) << 4;
        clamp |= static_cast<uint64_t>(rng.below(128)) << 14;
        clamp |= static_cast<uint64_t>(rng.below(128)) << 24;
        clamp |= static_cast<uint64_t>(rng.below(128)) << 34;
        state.context.clamp = clamp;
        state.linearFilter = rng.chance(50);
        state.texa.ta0 = static_cast<uint8_t>(rng.below(256));
        state.texa.aem = rng.chance(30);
        state.texa.ta1 = static_cast<uint8_t>(rng.below(256));
        // Mipmapping: levels at other textures' bases, LOD from Q or fixed.
        state.mip = GSMipInfo{};
        if (rng.chance(25))
        {
            uint64_t tex1 = (rng.chance(30) ? 1ull : 0ull) | (static_cast<uint64_t>(rng.range(1, 6)) << 2) |
                            (static_cast<uint64_t>(rng.below(2)) << 5) | (static_cast<uint64_t>(rng.range(2, 5)) << 6) |
                            (static_cast<uint64_t>(rng.chance(10) ? 1u : 0u) << 9) |
                            (static_cast<uint64_t>(rng.below(4)) << 19) |
                            (static_cast<uint64_t>(static_cast<uint32_t>(rng.range(-64, 96)) & 0xFFFu) << 32);
            uint64_t miptbp[2] = {0, 0};
            for (uint32_t i = 0; i < 6u; ++i)
            {
                const uint64_t level = static_cast<uint64_t>(rng.pick(bases)) | (static_cast<uint64_t>(rng.range(1, 4)) << 14);
                miptbp[i / 3u] |= level << ((i % 3u) * 20u);
            }
            state.mip = gsDecodeMip(tex, tex1, miptbp[0], miptbp[1]);
        }
    }

    // An 8x8 sprite that copies texels into its own cell of the witness
    // area, so what a draw sampled stays visible until the cell is reused.
    GSPrimitiveBatch witnessSprite(Rng &rng, const Options &options, uint32_t cell, bool sampleTarget)
    {
        GSPrimitiveBatch batch{};
        auto &state = batch.state;
        auto &ctx = state.context;
        state.prim.type = GS_PRIM_SPRITE;
        state.prim.tme = true;
        state.prim.fst = rng.chance(50);
        ctx.frame.fbp = kWitnessPage;
        ctx.frame.fbw = 2;
        ctx.frame.psm = GS_PSM_CT32;
        ctx.zbuf.zbp = kWitnessPage + 16u;
        ctx.zbuf.psm = GS_PSM_Z32;
        ctx.zbuf.zmask = true;
        ctx.scissor = {0, 127, 0, 127};
        ctx.test = 1ull << 17; // Z always, no alpha test
        randomizeTexture(rng, state, false, options, sampleTarget);
        state.context.tex0.tfx = 1; // DECAL
        const float x = static_cast<float>((cell % 16u) * 8u);
        const float y = static_cast<float>((cell / 16u) * 8u);
        batch.vertices[0].x = x;
        batch.vertices[0].y = y;
        batch.vertices[1].x = x + 8.0f;
        batch.vertices[1].y = y + 8.0f;
        for (auto &v : batch.vertices)
        {
            v.r = v.g = v.b = v.a = 128;
            v.q = 1.0f;
        }
        batch.vertices[0].s = batch.vertices[0].t = 0.0f;
        batch.vertices[1].s = batch.vertices[1].t = 1.0f;
        batch.vertices[0].u = batch.vertices[0].v = 0;
        batch.vertices[1].u = static_cast<uint16_t>(state.textureWidth * 16u);
        batch.vertices[1].v = static_cast<uint16_t>(state.textureHeight * 16u);
        batch.vertexCount = 2;
        return batch;
    }

    GSPrimitiveBatch randomBatch(Rng &rng, const Options &options, uint32_t &witness)
    {
        GSPrimitiveBatch batch{};
        auto &state = batch.state;
        auto &ctx = state.context;
        const uint32_t kind = rng.below(100);
        state.prim.type = kind < 55 ? GS_PRIM_TRIANGLE : kind < 70 ? GS_PRIM_TRISTRIP : kind < 88 ? GS_PRIM_SPRITE
                                                     : kind < 95 ? GS_PRIM_LINE
                                                                 : GS_PRIM_POINT;
        state.prim.iip = rng.chance(60);
        state.prim.tme = rng.chance(75);
        state.prim.fge = rng.chance(15);
        state.prim.abe = rng.chance(40);
        state.prim.fst = rng.chance(30);
        const bool feedback = rng.chance(options.feedbackPercent);
        const uint32_t rtRoll = rng.below(100);
        const bool intoTarget = rtRoll < options.renderTargetPercent / 2u;
        const bool sampleTarget = !intoTarget && rtRoll < options.renderTargetPercent;
        if ((sampleTarget || options.oneTexture) && rng.chance(50))
            return witnessSprite(rng, options, witness++ % 256u, sampleTarget);

        static const uint32_t fbps[] = {0u, 0u, 0u, 0u, 100u};
        ctx.frame.fbp = rng.pick(fbps);
        if (feedback && rng.chance(50))
            ctx.frame.fbp = kTexBase / 32u; // render into a texture
        static const uint32_t fbws[] = {8u, 8u, 8u, 4u, 10u};
        ctx.frame.fbw = rng.pick(fbws);
        static const uint8_t fpsms[] = {GS_PSM_CT32, GS_PSM_CT32, GS_PSM_CT32, GS_PSM_CT24, GS_PSM_CT16, GS_PSM_CT16S};
        ctx.frame.psm = rng.pick(fpsms);
        ctx.frame.fbmsk = rng.chance(10) ? rng.next() : 0u;
        ctx.zbuf.zbp = rng.chance(3) ? 0u : 64u;
        static const uint8_t zpsms[] = {GS_PSM_Z32, GS_PSM_Z24, GS_PSM_Z16, GS_PSM_Z16S, GS_PSM_Z16S, GS_PSM_CT32};
        ctx.zbuf.psm = rng.pick(zpsms);
        ctx.zbuf.zmask = rng.chance(30);
        if (rng.chance(80))
            ctx.scissor = {0, 511, 0, 223};
        else
        {
            ctx.scissor.x0 = static_cast<uint16_t>(rng.below(300));
            ctx.scissor.x1 = static_cast<uint16_t>(ctx.scissor.x0 + rng.below(rng.chance(10) ? 400u : 200u));
            ctx.scissor.y0 = static_cast<uint16_t>(rng.below(200));
            ctx.scissor.y1 = static_cast<uint16_t>(ctx.scissor.y0 + rng.below(100));
        }
        if (intoTarget)
        {
            ctx.frame.fbp = kRtPage;
            ctx.frame.fbw = 2;
            ctx.zbuf.zbp = kRtPage + 8u;
            ctx.scissor = {0, 127, 0, 63};
            if (rng.chance(50))
                state.prim.tme = false;
        }
        const int ofx = rng.range(0, 64);
        const int ofy = rng.range(0, 64);
        ctx.xyoffset.ofx = static_cast<uint16_t>(ofx << 4);
        ctx.xyoffset.ofy = static_cast<uint16_t>(ofy << 4);

        uint64_t test = rng.below(2);                               // ATE
        test |= static_cast<uint64_t>(rng.below(8)) << 1;           // ATST
        test |= static_cast<uint64_t>(rng.below(256)) << 4;         // AREF
        test |= static_cast<uint64_t>(rng.below(4)) << 12;          // AFAIL
        test |= static_cast<uint64_t>(rng.chance(15)) << 14;        // DATE
        test |= static_cast<uint64_t>(rng.below(2)) << 15;          // DATM
        test |= 1ull << 16;                                         // ZTE
        test |= static_cast<uint64_t>(rng.chance(5) ? 0u : rng.range(1, 3)) << 17; // ZTST
        ctx.test = test;
        ctx.alpha = static_cast<uint64_t>(rng.below(256)) | (static_cast<uint64_t>(rng.below(256)) << 32);
        ctx.fba = rng.chance(20) ? 1u : 0u;
        state.pabe = rng.chance(20);
        state.colclamp = rng.chance(70) ? 1u : 0u;
        state.fogR = static_cast<uint8_t>(rng.below(256));
        state.fogG = static_cast<uint8_t>(rng.below(256));
        state.fogB = static_cast<uint8_t>(rng.below(256));
        randomizeTexture(rng, state, feedback && !(ctx.frame.fbp == kTexBase / 32u), options, sampleTarget);

        const bool large = rng.chance(options.largePercent);
        // Coplanar draws with the same Z, so the Z test sees ties.
        const uint32_t flatZ = rng.chance(40) ? rng.range(1, 5) : 0u;
        const float cx = static_cast<float>(ofx + (intoTarget ? rng.range(-10, 140) : rng.range(-40, 560)));
        const float cy = static_cast<float>(ofy + (intoTarget ? rng.range(-10, 70) : rng.range(-40, 260)));
        for (auto &v : batch.vertices)
        {
            const float spread = large ? 700.0f : 40.0f;
            v.x = cx + (rng.unit() - 0.5f) * spread;
            v.y = cy + (rng.unit() - 0.5f) * spread;
            v.x = static_cast<float>(static_cast<int>(v.x * 16.0f)) / 16.0f;
            v.y = static_cast<float>(static_cast<int>(v.y * 16.0f)) / 16.0f;
            if (v.x < 0.0f)
                v.x = 0.0f;
            if (v.y < 0.0f)
                v.y = 0.0f;
            static const uint32_t zscales[] = {0xFFu, 0xFFFFu, 0xFFFFFFu, 0xFFFFFFFFu};
            static const uint32_t zfixed[] = {0u, 1000u, 0xFFFFu, 0x10000u, 0xFFFFFFu};
            v.z = static_cast<double>(rng.next() % (rng.pick(zscales) + 0ull + 1ull));
            if (flatZ)
                v.z = static_cast<double>(zfixed[flatZ - 1u]);
            v.r = static_cast<uint8_t>(rng.below(256));
            v.g = static_cast<uint8_t>(rng.below(256));
            v.b = static_cast<uint8_t>(rng.below(256));
            v.a = static_cast<uint8_t>(rng.below(256));
            v.q = rng.chance(5) ? -rng.unit() : 0.05f + rng.unit() * 2.0f;
            v.s = (rng.unit() * 3.0f - 1.0f) * v.q;
            v.t = (rng.unit() * 3.0f - 1.0f) * v.q;
            v.u = static_cast<uint16_t>(rng.below(state.textureWidth * 32u));
            v.v = static_cast<uint16_t>(rng.below(state.textureHeight * 32u));
            v.fog = static_cast<uint8_t>(rng.below(256));
        }
        batch.vertexCount = state.prim.type == GS_PRIM_SPRITE || state.prim.type == GS_PRIM_LINE ? 2
                            : state.prim.type == GS_PRIM_POINT                                    ? 1
                                                                                                  : 3;
        return batch;
    }

    struct Pair
    {
        std::vector<uint8_t> refVram = std::vector<uint8_t>(kVramSize);
        std::vector<uint8_t> fastVram = std::vector<uint8_t>(kVramSize);
        std::unique_ptr<GSCpuBackend> ref;
        std::unique_ptr<GSCpuBackend> fast;

        explicit Pair(Rng &rng, bool deferred)
        {
            for (size_t i = 0; i < refVram.size(); i += 4)
            {
                const uint32_t value = rng.next();
                std::memcpy(refVram.data() + i, &value, 4);
            }
            fastVram = refVram;
            setReference(true);
            ref = std::make_unique<GSCpuBackend>();
            setReference(false);
            fast = std::make_unique<GSCpuBackend>();
            ref->Initialize(refVram.data(), kVramSize);
            fast->Initialize(fastVram.data(), kVramSize);
            fast->SetDeferred(deferred);
        }

        template <typename Fn>
        void both(Fn fn)
        {
            fn(*ref);
            fn(*fast);
        }

        void compare(const std::string &where)
        {
            std::vector<uint8_t> a, b;
            ref->SnapshotVram(a);
            fast->SnapshotVram(b);
            if (a != b)
            {
                size_t first = 0;
                while (a[first] == b[first])
                    ++first;
                size_t count = 0;
                for (size_t i = 0; i < a.size(); ++i)
                    count += a[i] != b[i];
                std::ostringstream error;
                error << where << ": local memory differs, " << count << " bytes, first at 0x" << std::hex << first
                      << " (page " << std::dec << first / 8192u << ")";
                throw std::runtime_error(error.str());
            }
            std::vector<uint8_t> clutA, clutB;
            ref->SaveClutState(clutA);
            fast->SaveClutState(clutB);
            require(clutA == clutB, where + ": CLUT differs");
        }
    };

    void runRandom(const Options &options)
    {
        Rng rng(options.seed);
        Pair pair(rng, options.deferred);
        uint32_t witness = 0;
        for (uint32_t event = 0; event < options.events; ++event)
        {
            const uint32_t kind = rng.below(100);
            const std::string where = "seed " + std::to_string(options.seed) + " event " + std::to_string(event);
            if (kind < 72)
            {
                const GSPrimitiveBatch batch = randomBatch(rng, options, witness);
                pair.both([&](GSCpuBackend &backend)
                          { backend.Submit(batch); });
            }
            else if (kind < 80)
            {
                GSDrawState state{};
                randomizeTexture(rng, state, false, options);
                GSTex0Reg tex0 = state.context.tex0;
                static const uint8_t indexed[] = {GS_PSM_T8, GS_PSM_T4, GS_PSM_T8H, GS_PSM_T4HL};
                tex0.psm = rng.pick(indexed);
                tex0.cld = static_cast<uint8_t>(rng.range(1, 5));
                GSTexClutReg texclut{};
                texclut.cbw = static_cast<uint8_t>(rng.below(4));
                texclut.cou = static_cast<uint8_t>(rng.below(4));
                texclut.cov = static_cast<uint16_t>(rng.below(16));
                pair.both([&](GSCpuBackend &backend)
                          { backend.LoadClut(tex0, texclut); });
                std::vector<uint8_t> clutA, clutB;
                pair.ref->SaveClutState(clutA);
                pair.fast->SaveClutState(clutB);
                require(clutA == clutB, where + ": CLUT differs after load");
            }
            else if (kind < 85)
            {
                if (rng.below(5) < options.texflushPercent)
                    pair.both([](GSCpuBackend &backend)
                              { backend.TextureFlush(); });
            }
            else if (kind < 90)
            {
                GSTransferCommand transfer{};
                transfer.direction = 0;
                static const uint32_t targets[] = {kTexBase, kTexBase + 8u, kClutBase, 0u, kRtPage * 32u};
                transfer.bitbltbuf.dbp = options.oneTexture ? kTexBase : rng.pick(targets);
                transfer.bitbltbuf.dbw = static_cast<uint8_t>(options.oneTexture ? 2 : rng.range(1, 4));
                static const uint8_t psms[] = {GS_PSM_CT32, GS_PSM_CT16, GS_PSM_T8, GS_PSM_T4};
                transfer.bitbltbuf.dpsm = rng.pick(psms);
                transfer.trxpos.dsax = static_cast<uint16_t>(rng.below(options.oneTexture ? 16 : 64));
                transfer.trxpos.dsay = static_cast<uint16_t>(rng.below(options.oneTexture ? 16 : 64));
                transfer.trxreg.rrw = static_cast<uint16_t>(rng.range(1, 32));
                transfer.trxreg.rrh = static_cast<uint16_t>(rng.range(1, 32));
                std::vector<uint8_t> data(static_cast<size_t>(transfer.trxreg.rrw) * transfer.trxreg.rrh * 4u);
                for (auto &byte : data)
                    byte = static_cast<uint8_t>(rng.next());
                pair.both([&](GSCpuBackend &backend)
                          {
                              backend.BeginTransfer(transfer);
                              backend.UploadImage(data.data(), static_cast<uint32_t>(data.size())); });
            }
            else if (kind < 93)
            {
                GSTransferCommand transfer{};
                transfer.direction = 2;
                transfer.bitbltbuf.sbp = rng.chance(50) ? 0u : kTexBase;
                transfer.bitbltbuf.sbw = 8;
                transfer.bitbltbuf.spsm = GS_PSM_CT32;
                transfer.bitbltbuf.dbp = rng.chance(50) ? kTexBase : kClutBase;
                transfer.bitbltbuf.dbw = static_cast<uint8_t>(rng.range(1, 4));
                transfer.bitbltbuf.dpsm = GS_PSM_CT32;
                transfer.trxpos.ssax = static_cast<uint16_t>(rng.below(64));
                transfer.trxpos.ssay = static_cast<uint16_t>(rng.below(64));
                transfer.trxreg.rrw = static_cast<uint16_t>(rng.range(1, 32));
                transfer.trxreg.rrh = static_cast<uint16_t>(rng.range(1, 32));
                pair.both([&](GSCpuBackend &backend)
                          { backend.BeginTransfer(transfer); });
            }
            else if (kind < 96)
            {
                const uint32_t x = rng.below(options.oneTexture ? 16 : 64), y = rng.below(options.oneTexture ? 16 : 64), value = rng.next();
                static const uint32_t bases[] = {kTexBase, kClutBase, 0u, kRtPage * 32u};
                const uint32_t base = options.oneTexture ? kTexBase : rng.pick(bases);
                pair.both([&](GSCpuBackend &backend)
                          { backend.WriteVram(GS_PSM_CT32, base, 2, x, y, value); });
            }
            else if (kind < 99 && rng.chance(50))
            {
                // Present while draws are queued: only the displayed pages
                // must be complete.
                GSPresentationRequest request{};
                request.pmode = 1u | (rng.chance(30) ? 2u : 0u) | (static_cast<uint64_t>(rng.below(256)) << 8);
                static const uint32_t displayPages[] = {0u, 0u, 100u, kWitnessPage, kRtPage, kTexBase / 32u};
                const uint64_t fbp = rng.pick(displayPages);
                const uint64_t fbw = rng.chance(80) ? 8u : 2u;
                const uint64_t psm = rng.chance(80) ? GS_PSM_CT32 : GS_PSM_CT16;
                request.dispfb1 = fbp | (fbw << 9) | (psm << 15) | (static_cast<uint64_t>(rng.below(16)) << 32);
                request.dispfb2 = rng.pick(displayPages) | (8ull << 9);
                request.display1 = (511ull << 32) | (223ull << 44);
                request.display2 = (511ull << 32) | (223ull << 44);
                request.contextFrames[0] = {0u, 8u, GS_PSM_CT32, 0u};
                request.contextFrames[1] = {100u, 8u, GS_PSM_CT32, 0u};
                if (rng.chance(10))
                {
                    request.hasPreferredSource = true;
                    request.preferredSource = {kTexBase, 8u, GS_PSM_CT32, 0u};
                    request.preferredDestFbp = static_cast<uint32_t>(fbp);
                }
                request.vsyncTick = rng.below(4);
                const PresentationFrame a = pair.ref->Present(request);
                const PresentationFrame b = pair.fast->Present(request);
                require(a.pixels == b.pixels && a.width == b.width && a.height == b.height &&
                            a.displayFbp == b.displayFbp && a.sourceFbp == b.sourceFbp,
                        where + ": presented picture differs");
            }
            else if (kind < 97)
            {
                GSContext context{};
                context.frame.fbp = rng.chance(50) ? 0u : kTexBase / 32u;
                context.frame.fbw = 8;
                context.frame.psm = rng.chance(50) ? GS_PSM_CT32 : GS_PSM_CT16;
                context.scissor = {0, static_cast<uint16_t>(rng.below(512)), 0, static_cast<uint16_t>(rng.below(224))};
                const uint32_t color = rng.next();
                pair.both([&](GSCpuBackend &backend)
                          { backend.ClearFramebuffer(context, color); });
            }
            else
            {
                pair.compare(where);
            }
        }
        pair.compare("seed " + std::to_string(options.seed) + " end");
    }

    template <uint64_t Seed, bool Deferred, uint32_t Feedback>
    void randomTest()
    {
        Options options;
        options.seed = Seed;
        options.deferred = Deferred;
        options.feedbackPercent = Feedback;
        runRandom(options);
    }

    // Render to texture and sample it again without TEXFLUSH.
    template <uint64_t Seed, bool Deferred>
    void renderTarget()
    {
        Options options;
        options.seed = Seed;
        options.deferred = Deferred;
        options.renderTargetPercent = 60;
        options.texflushPercent = 1;
        runRandom(options);
    }

    // One texture, poked by uploads and CPU writes, rarely flushed: the
    // texture cache hands out stale copies.
    template <uint64_t Seed>
    void staleCache()
    {
        Options options;
        options.seed = Seed;
        options.oneTexture = true;
        options.texflushPercent = 1;
        runRandom(options);
    }

    // The level traces: big triangles over one T8 texture, Z16S, CT32.
    void bigTriangles()
    {
        Options options;
        options.seed = 77;
        options.largePercent = 60;
        options.events = 600;
        runRandom(options);
    }
}

namespace
{
    // The texel every wrap mode gives for the exact whole value of a
    // coordinate (a float past 2^24 is a whole number): its low 10 bits (the
    // most the masks reach) from fmod, exact for any double, as two's
    // complement; its place against the texture and the region compared as
    // it is.
    // `plus` is added exactly (a double past 2^53 has no room for a +1).
    int exactWrap(double whole, int size, uint8_t mode, uint16_t regionMin, uint16_t regionMax, int plus = 0)
    {
        double rest = std::fmod(whole, 1024.0);
        if (rest < 0)
            rest += 1024.0;
        const int low = (static_cast<int>(rest) + plus) & 0x3FF;
        whole += plus; // (for the comparisons: a whole number past the texture stays past it)
        switch (mode)
        {
        case 0: return low & (size - 1);
        case 1: return whole < 0 ? 0 : whole > size - 1 ? size - 1 : static_cast<int>(whole);
        case 2: // (as the renderer reads a region: up to its minimum, then down to its maximum)
            return static_cast<int>(std::min(std::max(whole, static_cast<double>(regionMin)), static_cast<double>(regionMax)));
        default: return (low & regionMin) | regionMax;
        }
    }

    // texelFloor and texelTrunc: for any coordinate, every wrap mode reads
    // the texel it reads for the coordinate's exact value, the next texel
    // (+1) too; within 2^30 the int is the value itself; NaN reads as 0.
    void hugeCoordinates()
    {
        std::vector<float> values;
        for (int e = 20; e <= 127; ++e)
            for (float m : {1.0f, 1.0000001f, 1.3f, 1.7777777f, 1.9999999f})
            {
                values.push_back(std::ldexp(m, e));
                values.push_back(-std::ldexp(m, e));
            }
        for (float v : {1.8236509e15f, 1.4589196e16f, 3.4e38f, 1073741824.0f, 1073741823.0f, 16777216.5f})
        {
            values.push_back(v);
            values.push_back(-v);
        }
        int checked = 0;
        for (float v : values)
            for (int size : {1, 8, 64, 1024})
                for (uint8_t mode = 0; mode < 4; ++mode)
                    for (auto [lo, hi] : {std::pair<uint16_t, uint16_t>{0, 0}, {3, 5}, {0x3F0, 0x00F}, {0x3FF, 0x3FF}})
                    {
                        const double whole = std::floor(static_cast<double>(v));
                        const int f = GSCpuShared::texelFloor(v);
                        require(GSCpuShared::wrapTextureCoordinate(f, size, mode, lo, hi) == exactWrap(whole, size, mode, lo, hi),
                                "texelFloor: the texel of the exact value");
                        require(GSCpuShared::wrapTextureCoordinate(f + 1, size, mode, lo, hi) ==
                                    exactWrap(whole, size, mode, lo, hi, 1),
                                "texelFloor + 1: the next texel of the exact value");
                        const int t = GSCpuShared::texelTrunc(v);
                        require(GSCpuShared::wrapTextureCoordinate(t, size, mode, lo, hi) ==
                                    exactWrap(std::trunc(static_cast<double>(v)), size, mode, lo, hi),
                                "texelTrunc: the texel of the exact value");
                        ++checked;
                    }
        for (float v : {0.0f, 0.5f, -0.5f, 7.25f, -7.25f, 1023.9f, 16777215.0f, -16777215.0f, 1073741760.0f})
        {
            require(GSCpuShared::texelFloor(v) == static_cast<int>(std::floor(v)), "within 2^30: the value itself (floor)");
            require(GSCpuShared::texelTrunc(v) == static_cast<int>(v), "within 2^30: the value itself (trunc)");
        }
        require(GSCpuShared::texelFloor(std::numeric_limits<float>::quiet_NaN()) == 0, "NaN reads as 0");
        require(GSCpuShared::texelFloor(std::numeric_limits<float>::infinity()) > 1024 &&
                    GSCpuShared::texelFloor(-std::numeric_limits<float>::infinity()) < 0,
                "infinities: past either end");
        require(GSCpuShared::texelFraction(1.4589196e16f) == 0.0f && GSCpuShared::texelFraction(2.25f) == 0.25f,
                "the bilinear weight: 0 for a whole coordinate");
        std::cout << "huge_coordinates: " << checked << " coordinate, size and mode cases" << std::endl;
    }
}

int main(int argc, char **argv)
{
    return run(argc, argv, {
        {"random_1", randomTest<1, true, 3>},
        {"random_2", randomTest<2, true, 3>},
        {"random_3", randomTest<3, true, 3>},
        {"random_4", randomTest<4, true, 3>},
        {"random_immediate", randomTest<5, false, 3>},
        {"random_feedback_1", randomTest<6, true, 30>},
        {"random_feedback_2", randomTest<7, true, 30>},
        {"random_feedback_immediate", randomTest<8, false, 30>},
        {"big_triangles", bigTriangles},
        {"render_target_1", renderTarget<11, true>},
        {"render_target_2", renderTarget<12, true>},
        {"render_target_immediate", renderTarget<13, false>},
        {"stale_cache_1", staleCache<21>},
        {"stale_cache_2", staleCache<22>},
        {"render_target_3", renderTarget<14, true>},
        {"render_target_4", renderTarget<15, true>},
        {"stale_cache_3", staleCache<23>},
        {"huge_coordinates", hugeCoordinates},
    });
}
