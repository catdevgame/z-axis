#include "MiniTest.h"
#include "runtime/gs/gs_gpu_texture_check.h"
#if ZX_HAS_GS_GPU
#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_gpu_backend.h"
#endif

#include <memory>
#include <vector>

namespace texcheck = zx::gpu_texture_check;

namespace
{
    std::vector<SDL_GPUTexture *> g_given; // what the check gave to "SDL"
    void fakeRelease(SDL_GPUDevice *, SDL_GPUTexture *t) { g_given.push_back(t); }
}

void register_ps2_gpu_texture_check_tests()
{
    MiniTest::Case("GpuTextureCheck", [](TestCase &tc)
    {
        tc.Run("a released handle is kept and its uses reported", [](TestCase &t)
        {
            // Fake handles: only their addresses are used.
            alignas(8) static char a[8], b[8], dev[8];
            auto *ta = reinterpret_cast<SDL_GPUTexture *>(a);
            auto *tb = reinterpret_cast<SDL_GPUTexture *>(b);
            auto *d = reinterpret_cast<SDL_GPUDevice *>(dev);
            g_given.clear();
            texcheck::configureForTest(true, 1000.0, fakeRelease);
            texcheck::releaseTexture(d, ta);
            t.IsTrue(g_given.empty(), "kept, not given to SDL");
            t.IsTrue(!texcheck::check(tb, "bind of"), "one never made here is reported");
            t.IsTrue(texcheck::check(tb, "render target", texcheck::Need::Present), "a target may be any texture");
            t.IsTrue(!texcheck::check(nullptr, "render target", texcheck::Need::Present), "but not null");
            t.IsTrue(texcheck::check(nullptr, "resolve target", texcheck::Need::Optional), "unless optional");
            t.IsTrue(!texcheck::check(ta, "bind of"), "the released one is reported");
            texcheck::releaseTexture(d, ta);
            t.Equals(texcheck::counts().misuses, uint64_t(4), "a second release is reported too");
            t.Equals(texcheck::counts().held, uint64_t(1), "one kept");
            texcheck::releaseHeld(d);
            t.Equals(g_given.size(), size_t(1), "given to SDL before the device goes");
            t.IsTrue(texcheck::check(ta, "copy to", texcheck::Need::Present), "no longer held once given back");

            // Kept for 0 s: the next release gives the older one back.
            texcheck::configureForTest(true, 0.0, fakeRelease);
            g_given.clear();
            texcheck::releaseTexture(d, ta);
            texcheck::releaseTexture(d, tb);
            t.Equals(g_given.size(), size_t(2), "both given back at once");

            // Off: straight to SDL (not called here: no device), nothing kept.
            texcheck::configureForTest(false, 2.0, fakeRelease);
            t.IsTrue(!texcheck::enabled(), "off");
            t.IsTrue(texcheck::check(ta, "bind of"), "nothing checked when off");
            t.Equals(texcheck::counts().held, uint64_t(0), "nothing kept when off");
            texcheck::configureForTest(false, 2.0, nullptr);
        });

#if ZX_HAS_GS_GPU
        tc.Run("GPU renderer: both circuits on one buffer, the second taller, use no destroyed target", [](TestCase &t)
        {
            // The intro-movie crash on Windows (BRINGUP "The intro-movie quit"):
            // both read circuits show the same buffer, and the second one's
            // DISPLAY is taller than the target. Its lookup grew (destroyed
            // and remade) the target the first had just been given, and the
            // picture was drawn from the destroyed one's texture handle. With
            // the check on a destroyed target keeps a token for its texture,
            // so such a use is counted (and not passed to SDL).
            texcheck::configureForTest(true, 1000.0, nullptr);
            bool skipped = false;
            uint32_t height = 0;
            {
                GSGpuBackendOptions options;
                auto backend = std::make_unique<GSGpuBackend>(options);
                if (!backend->valid())
                    skipped = true;
                else
                {
                    GSGpuBackend *gpu = backend.get();
                    std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
                    GS gs;
                    gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);
                    gs.setRasterBackend(std::move(backend));
                    // A 512 x 224 CT32 buffer at block 0 (a target 224 lines high).
                    gs.writeRegister(GS_REG_FRAME_1, (8ull << 16) | (static_cast<uint64_t>(GS_PSM_CT32) << 24));
                    gs.writeRegister(GS_REG_ZBUF_1, 1ull << 32);
                    gs.writeRegister(GS_REG_SCISSOR_1, 511ull << 16 | 223ull << 48);
                    gs.writeRegister(GS_REG_XYOFFSET_1, (1024ull << 4) | ((1024ull << 4) << 32));
                    gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
                    gs.writeRegister(GS_REG_PRIM, static_cast<uint64_t>(GS_PRIM_SPRITE));
                    gs.writeRegister(GS_REG_RGBAQ, 0x80204060ull);
                    gs.writeRegister(GS_REG_XYZ2, (1024ull << 4) | ((1024ull << 4) << 16));
                    gs.writeRegister(GS_REG_XYZ2, ((1024ull + 512ull) << 4) | (((1024ull + 224ull) << 4) << 16));
                    gpu->Flush();
                    GSPresentationRequest r;
                    r.pmode = 0x8003u; // both circuits, blended 50%
                    r.dispfb1 = r.dispfb2 = (8ull << 9) | (static_cast<uint64_t>(GS_PSM_CT32) << 15);
                    r.display1 = (511ull << 32) | (223ull << 44);
                    r.display2 = (511ull << 32) | (447ull << 44); // 448 lines: the target grows
                    const PresentationFrame frame = gpu->Present(r);
                    height = frame.height;
                }
            }
            const uint64_t misuses = texcheck::counts().misuses;
            texcheck::configureForTest(false, 2.0, nullptr);
            if (skipped)
            {
                t.IsTrue(true, "no SDL_GPU device: skipped");
                return;
            }
            t.Equals(height, 448u, "the picture is as tall as the taller circuit");
            t.Equals(misuses, uint64_t(0), "no texture used through a destroyed target (nor released, null or unknown)");
        });
#endif
    });
}
