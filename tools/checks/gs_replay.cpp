// zx-gsreplay: replays a GS input trace recorded with ZX_GS_DUMP and writes
// the presented picture at every frame marker as PNG.
//
//   zx-gsreplay <trace.gsr> <out-prefix> [first-frame] [options]
//
// Writes <out-prefix>-<n>.png for each frame marker n >= first-frame.
// ZX_GS_LOG_LIMIT and the other GS debug logs work as in the runner.
//
// Options (used by gs_replay_check.sh to compare renderer changes):
//   --no-png      do not write PNGs
//   --hash        print "frame <n> vram=<hash> pixels=<hash>" per frame: a
//                 64-bit FNV-1a hash of the full 4 MiB GS local memory after
//                 the frame's events, and of the presented picture
//   --picture-hash  print only "frame <n> pixels=<hash>" (no VRAM snapshot,
//                 so the picture is taken with draws still queued, as in
//                 the runner); with --gpu (also --hash) the hash of the
//                 picture at the internal resolution
//   --dump-vram   write the GS local memory per frame to <out-prefix>-<n>.vram
//   --time        print the time spent replaying each frame's events
//   --repeat <n>  replay the whole trace n times (timing); output per pass
//
// GPU renderer (builds with ZX_HAS_GS_GPU, see docs/GPU_BACKEND.md):
//   --gpu         draw with the SDL_GPU backend (headless) instead of the CPU
//                 one; the PNGs are at the internal resolution
//   --scale <n>   internal resolution multiplier for --gpu (default 1;
//                 ZX_GS_FULL_HEIGHT=1 doubles it vertically, as in the runner)
//   --compare     also replay on the CPU renderer and print, per frame, the
//                 mean and maximum per-channel difference of the GPU picture
//                 (at native resolution) to the CPU one and the share of
//                 pixels off by more than 16; with PNGs, writes
//                 <out-prefix>-<n>-cmp.png: CPU | GPU | difference x4
//   --between a,b,...  frame interpolation (gs/gs_interpolate.h): the
//                 frames made as the frame queue makes them, and after each
//                 frame from the second on the pictures between it and the
//                 frame before at those points (0 < a <= 1). At 1 the
//                 picture must be the frame's own (its replay in the
//                 renderer's own targets): "frame <n> between 1 match" or
//                 MISMATCH with both hashes; the others are written as
//                 <out-prefix>-<n>-b<a x 100>.png. Every picture's size as
//                 given to the window must be its texture's ("SIZE
//                 MISMATCH", counted as a mismatch). A summary line counts
//                 them and the renderer's reasons for not making one.
//                 ZX_INTERP_FORCE=1: made even where the window would
//                 show the frame as it is (no cut or stillness test).
#include "gs/gs_frontend.h"
#if ZX_HAS_GS_GPU
#include "gs/gs_gpu_backend.h"
#endif
#include "gs/gs_interpolate.h"
#include "gs/gs_quality.h"
#include "gs/gs_record.h"
#include "runtime/memory.h"
#include "host_backend.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    uint64_t fnv1a64(const uint8_t *data, size_t size)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (size_t i = 0; i < size; ++i)
        {
            hash ^= data[i];
            hash *= 0x100000001B3ull;
        }
        return hash;
    }

    std::string hex64(uint64_t value)
    {
        char text[17];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
        return text;
    }

    struct Options
    {
        std::string tracePath;
        std::string prefix;
        long firstFrame = 0;
        bool png = true;
        bool hash = false;
        bool pictureHash = false;
        bool dumpVram = false;
        bool time = false;
        long repeat = 1;
        bool gpu = false;
        uint32_t scale = 1;
        bool compare = false;
        std::vector<float> between; // --between
    };

    void writePng(const std::string &path, const std::vector<uint8_t> &rgba, uint32_t width, uint32_t height)
    {
        ps2_host::writePng(path, rgba.data(), width, height, width * 4u);
    }

    // The host presentation buffer is 640 pixels wide; returns the picture
    // tightly packed.
    std::vector<uint8_t> packPicture(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4u);
        const uint32_t stride = pixels.size() >= 640u * 512u * 4u ? 640u : width;
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(out.data() + static_cast<size_t>(y) * width * 4u, pixels.data() + static_cast<size_t>(y) * stride * 4u,
                        width * 4u);
        return out;
    }

    struct DiffResult
    {
        double mean = 0.0;
        int max = 0;
        double over16 = 0.0;
    };

    // Per-channel difference of two native-resolution pictures (RGB);
    // `diff` gets |a - b| x 4.
    DiffResult comparePictures(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b, uint32_t width,
                               uint32_t height, std::vector<uint8_t> &diff)
    {
        DiffResult r;
        diff.assign(static_cast<size_t>(width) * height * 4u, 255u);
        uint64_t sum = 0, over = 0;
        for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i)
        {
            int pixelMax = 0;
            for (int c = 0; c < 3; ++c)
            {
                const int d = std::abs(static_cast<int>(a[i * 4 + c]) - static_cast<int>(b[i * 4 + c]));
                sum += static_cast<uint64_t>(d);
                pixelMax = std::max(pixelMax, d);
                diff[i * 4 + c] = static_cast<uint8_t>(std::min(255, d * 4));
            }
            r.max = std::max(r.max, pixelMax);
            if (pixelMax > 16)
                ++over;
        }
        const double n = static_cast<double>(width) * height;
        r.mean = n > 0 ? static_cast<double>(sum) / (n * 3.0) : 0.0;
        r.over16 = n > 0 ? 100.0 * static_cast<double>(over) / n : 0.0;
        return r;
    }

    // Puts pictures side by side, each scaled (nearest) to `height` lines.
    std::vector<uint8_t> sideBySide(const std::vector<std::vector<uint8_t> *> &pics, const std::vector<uint32_t> &widths,
                                    const std::vector<uint32_t> &heights, uint32_t height, uint32_t &outWidth)
    {
        outWidth = 0;
        std::vector<uint32_t> scaledW;
        for (size_t i = 0; i < pics.size(); ++i)
        {
            scaledW.push_back(static_cast<uint32_t>(static_cast<uint64_t>(widths[i]) * height / heights[i]));
            outWidth += scaledW.back() + (i ? 4u : 0u);
        }
        std::vector<uint8_t> out(static_cast<size_t>(outWidth) * height * 4u, 0u);
        for (size_t i = 3; i < out.size(); i += 4)
            out[i] = 255u;
        uint32_t x0 = 0;
        for (size_t i = 0; i < pics.size(); ++i)
        {
            const std::vector<uint8_t> &p = *pics[i];
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(y) * heights[i] / height);
                for (uint32_t x = 0; x < scaledW[i]; ++x)
                {
                    const uint32_t sx = static_cast<uint32_t>(static_cast<uint64_t>(x) * widths[i] / scaledW[i]);
                    std::memcpy(out.data() + (static_cast<size_t>(y) * outWidth + x0 + x) * 4u,
                                p.data() + (static_cast<size_t>(sy) * widths[i] + sx) * 4u, 4u);
                }
            }
            x0 += scaledW[i] + 4u;
        }
        return out;
    }

    // A GS fed from its own reader of the trace.
    struct Replayer
    {
        std::vector<uint8_t> vram = std::vector<uint8_t>(PS2_GS_VRAM_SIZE, 0u);
        GSRegisters priv{};
        GS gs;
        GSRecord::Reader reader;

        bool open(const std::string &path, std::unique_ptr<GSRasterBackend> backend)
        {
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &priv);
            if (backend)
                gs.setRasterBackend(std::move(backend));
            // As in the runner (ZX_GS_DEFER=0 disables it there and here).
            const char *defer = std::getenv("ZX_GS_DEFER");
            gs.setDeferredRasterization(!(defer && defer[0] == '0'));
            if (!reader.open(path, gs, vram.data(), static_cast<uint32_t>(vram.size()), priv))
                return false;
            gs.writeRegister(0x3F, 0u); // TEXFLUSH: VRAM was replaced under the backend
            return true;
        }

        // Replays up to the next frame marker; false at the end.
        bool advance(GSRecord::Event &event, double *ms = nullptr)
        {
            using Clock = std::chrono::steady_clock;
            while (reader.next(event))
            {
                if (event.type == GSRecord::kFrameMarker)
                {
                    GSRecord::applyFrameMarker(event, priv);
                    return true;
                }
                const auto start = Clock::now();
                GSRecord::replayEvent(event, gs);
                if (ms)
                    *ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            }
            return false;
        }

        // The frame as the frame queue makes it (held by the renderer for frame interpolation), then latched.
        bool queuedPicture(std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height)
        {
            gs.capturePresentationFrame();
            GS::TakenFrame taken;
            if (!gs.takePresentationFrame(taken))
                return false;
            std::vector<uint8_t> raw;
            if (!gs.copyLatchedHostPresentationFrame(raw, width, height))
                return false;
            pixels = packPicture(raw, width, height);
            return true;
        }

        bool picture(std::vector<uint8_t> &pixels, uint32_t &width, uint32_t &height)
        {
            gs.latchHostPresentationFrame();
            std::vector<uint8_t> raw;
            if (!gs.copyLatchedHostPresentationFrame(raw, width, height))
                return false;
            pixels = packPicture(raw, width, height);
            return true;
        }
    };

#if ZX_HAS_GS_GPU
    int replayGpu(const Options &options)
    {
        GSGpuBackendOptions gpuOptions;
        gpuOptions.scale = options.scale;
        // As the runner: twice the scale vertically (GPU_BACKEND.md, Full height).
        if (const char *v = std::getenv("ZX_GS_FULL_HEIGHT"); v && v[0] == '1')
            gpuOptions.fullHeight = true;
        auto backend = std::make_unique<GSGpuBackend>(gpuOptions);
        if (!backend->valid())
        {
            std::cerr << "no SDL_GPU device" << std::endl;
            return 1;
        }
        GSGpuBackend *gpu = backend.get();
        Replayer main;
        if (!main.open(options.tracePath, std::move(backend)))
        {
            std::cerr << "cannot read trace " << options.tracePath << std::endl;
            return 1;
        }
        std::unique_ptr<Replayer> ref;
        if (options.compare)
        {
            ref = std::make_unique<Replayer>();
            if (!ref->open(options.tracePath, nullptr))
                return 1;
        }
        GSRecord::Event event, refEvent;
        long frame = 0;
        DiffResult total;
        int compared = 0;
        const bool between = !options.between.empty();
        if (between)
        {
            zx::gs_interpolate::setEnabled(true);
            main.gs.setFrameQueue(true);
        }
        uint64_t previousMade = 0;
        long matched = 0, mismatched = 0, written = 0, notMade = 0;
        while (true)
        {
            double ms = 0.0;
            if (!main.advance(event, &ms))
                break;
            if (ref)
                ref->advance(refEvent);
            // Every frame's picture is made, as the runner makes one at each game frame (the frame queue): the
            // renderer's work of a frame (its submit and the releases after it, the texture pool's trim, the mip
            // copies' and native meshes' ageing) is done at its Present, so frames replayed without one held all
            // of theirs (AI's ai2.gsr from frame 100: a 6.5 GB footprint). Those before first-frame are not shown.
            using Clock = std::chrono::steady_clock;
            const auto start = Clock::now();
            std::vector<uint8_t> native;
            uint32_t width = 0, height = 0;
            const bool has = between ? main.queuedPicture(native, width, height) : main.picture(native, width, height);
            ms += std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (frame < options.firstFrame)
            {
                if (between && has)
                    previousMade = main.gs.shownPresentationFrame().frame;
                if (ref)
                {
                    std::vector<uint8_t> cpuPic;
                    uint32_t cw = 0, ch = 0;
                    ref->picture(cpuPic, cw, ch);
                }
            }
            else
            {
                std::vector<uint8_t> scaled;
                uint32_t sw = 0, sh = 0;
                const bool hasScaled = has && gpu->LastPresentedScaled(scaled, sw, sh);
                if (options.time)
                    std::cout << "frame " << frame << " time_ms=" << ms << std::endl;
                if (!has)
                    std::cout << "frame " << frame << ": no picture" << std::endl;
                if (options.pictureHash || options.hash)
                    std::cout << "frame " << frame << " pixels="
                              << (hasScaled ? hex64(fnv1a64(scaled.data(), scaled.size())) : std::string("none")) << " " << sw
                              << "x" << sh << std::endl;
                if (hasScaled && options.png)
                {
                    const std::string out = options.prefix + "-" + std::to_string(frame) + ".png";
                    writePng(out, scaled, sw, sh);
                    std::cout << "frame " << frame << " -> " << out << " (" << sw << "x" << sh << ")" << std::endl;
                }
                if (ref && has)
                {
                    std::vector<uint8_t> cpuPic;
                    uint32_t cw = 0, ch = 0;
                    if (ref->picture(cpuPic, cw, ch) && cw == width && ch == height)
                    {
                        std::vector<uint8_t> diff;
                        const DiffResult d = comparePictures(cpuPic, native, width, height, diff);
                        std::printf("frame %ld compare %ux%u mean=%.3f max=%d over16=%.3f%%\n", frame, width, height, d.mean,
                                    d.max, d.over16);
                        total.mean += d.mean;
                        total.max = std::max(total.max, d.max);
                        total.over16 += d.over16;
                        ++compared;
                        if (options.png)
                        {
                            uint32_t outW = 0;
                            std::vector<uint8_t> *gpuPic = hasScaled ? &scaled : &native;
                            const uint32_t gw = hasScaled ? sw : width, gh = hasScaled ? sh : height;
                            const std::vector<uint8_t> sbs =
                                sideBySide({&cpuPic, gpuPic, &diff}, {width, gw, width}, {height, gh, height}, gh, outW);
                            writePng(options.prefix + "-" + std::to_string(frame) + "-cmp.png", sbs, outW, gh);
                        }
                    }
                    else
                        std::cout << "frame " << frame << " compare: CPU picture " << cw << "x" << ch << " vs GPU " << width
                                  << "x" << height << std::endl;
                }
                if (between && hasScaled)
                {
                    const uint64_t made = main.gs.shownPresentationFrame().frame;
                    for (const float a : options.between)
                    {
                        if (previousMade == 0u)
                            break;
                        GSGpuPicture pic;
                        const zx::gs_interpolate::Skip why = gpu->PresentBetween(previousMade, made, a, pic);
                        std::vector<uint8_t> b;
                        uint32_t bw = 0, bh = 0;
                        if (why != zx::gs_interpolate::Skip::kCount || !gpu->ReadBetweenPicture(b, bw, bh))
                        {
                            ++notMade;
                            std::cout << "frame " << frame << " between " << a << ": "
                                      << (why != zx::gs_interpolate::Skip::kCount ? zx::gs_interpolate::skipName(why)
                                                                                    : "no picture")
                                      << std::endl;
                            continue;
                        }
                        // The size the window is given is the picture's: it shows that much of the texture.
                        if (pic.width != bw || pic.height != bh)
                        {
                            ++mismatched;
                            std::cout << "frame " << frame << " between " << a << " SIZE MISMATCH: given " << pic.width
                                      << "x" << pic.height << ", picture " << bw << "x" << bh << std::endl;
                        }
                        if (a >= 1.0f)
                        {
                            const bool same = bw == sw && bh == sh && b == scaled;
                            (same ? matched : mismatched)++;
                            std::cout << "frame " << frame << " between 1 " << (same ? "match" : "MISMATCH");
                            if (!same)
                                std::cout << " real=" << hex64(fnv1a64(scaled.data(), scaled.size()))
                                          << " replay=" << hex64(fnv1a64(b.data(), b.size()));
                            std::cout << std::endl;
                            if (!same && options.png)
                                writePng(options.prefix + "-" + std::to_string(frame) + "-b100.png", b, bw, bh);
                        }
                        else if (options.png)
                        {
                            const std::string out = options.prefix + "-" + std::to_string(frame) + "-b" +
                                                    std::to_string(static_cast<int>(std::lround(a * 100.0f))) + ".png";
                            writePng(out, b, bw, bh);
                            ++written;
                        }
                    }
                    previousMade = made;
                }
                if (std::getenv("ZX_GS_STATS"))
                {
                    const GSGpuBackend::Stats s = gpu->TakeStats();
                    std::cout << "frame " << frame << " prims=" << s.primitives << " draws=" << s.drawCalls
                              << " passes=" << s.renderPasses << " slow=" << s.slowPrimitives << " cpu=" << s.cpuPrimitives
                              << " texuploads=" << s.textureUploads << " rt-down=" << s.targetDownloads
                              << " rt-up=" << s.targetUploads << " submits=" << s.submits << std::endl;
                }
            }
            ++frame;
        }
        if (compared)
            std::printf("compared %d frames: mean=%.3f max=%d over16=%.3f%%\n", compared, total.mean / compared, total.max,
                        total.over16 / compared);
        if (between)
            std::cout << "between: at 1 " << matched << " match, " << mismatched << " mismatch; " << written
                      << " pictures written; " << notMade << " not made | renderer " << gpu->InterpolationReport() << std::endl;
        std::cout << frame << " frames" << std::endl;
        return 0;
    }
#endif

    int replayOnce(const Options &options, int pass)
    {
        std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
        GSRegisters priv{};
        GS gs;
        gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &priv);
        // As in the runner (ZX_GS_DEFER=0 disables it there and here).
        const char *defer = std::getenv("ZX_GS_DEFER");
        gs.setDeferredRasterization(!(defer && defer[0] == '0'));

        GSRecord::Reader reader;
        if (!reader.open(options.tracePath, gs, vram.data(), static_cast<uint32_t>(vram.size()), priv))
        {
            std::cerr << "cannot read trace " << options.tracePath << std::endl;
            return 1;
        }
        gs.writeRegister(0x3F, 0u); // TEXFLUSH: VRAM was replaced under the backend

        using Clock = std::chrono::steady_clock;
        GSRecord::Event event;
        long frame = 0;
        uint64_t events = 0;
        double totalMs = 0.0;
        Clock::duration frameTime{};
        while (true)
        {
            const bool more = reader.next(event);
            if (more && event.type != GSRecord::kFrameMarker)
            {
                const auto start = Clock::now();
                GSRecord::replayEvent(event, gs);
                frameTime += Clock::now() - start;
                ++events;
                continue;
            }
            if (!more)
                break;
            ++events;
            GSRecord::applyFrameMarker(event, priv);
            if (frame >= options.firstFrame)
            {
                const std::string label = (pass > 0 ? "pass " + std::to_string(pass) + " " : std::string()) +
                                          "frame " + std::to_string(frame);
                std::string vramHash;
                if (options.hash || options.dumpVram)
                {
                    // Goes through the backend so queued work is completed.
                    gs.refreshDisplaySnapshot();
                    uint32_t size = 0u;
                    const uint8_t *snapshot = gs.lockDisplaySnapshot(size);
                    if (options.hash)
                        vramHash = hex64(fnv1a64(snapshot, size));
                    if (options.dumpVram)
                    {
                        const std::string out = options.prefix + "-" + std::to_string(frame) + ".vram";
                        if (FILE *file = std::fopen(out.c_str(), "wb"))
                        {
                            std::fwrite(snapshot, 1, size, file);
                            std::fclose(file);
                        }
                    }
                    gs.unlockDisplaySnapshot();
                }
                const auto presentStart = Clock::now();
                gs.latchHostPresentationFrame();
                std::vector<uint8_t> pixels;
                uint32_t width = 0u, height = 0u, displayFbp = 0u, sourceFbp = 0u;
                const bool hasPicture = gs.copyLatchedHostPresentationFrame(pixels, width, height, &displayFbp, &sourceFbp);
                frameTime += Clock::now() - presentStart;
                const double ms = std::chrono::duration<double, std::milli>(frameTime).count();
                totalMs += ms;
                if (options.hash)
                    std::cout << label << " vram=" << vramHash << " pixels="
                              << (hasPicture ? hex64(fnv1a64(pixels.data(), pixels.size())) : std::string("none"))
                              << " " << width << "x" << height << std::endl;
                if (options.pictureHash)
                    std::cout << label << " pixels="
                              << (hasPicture ? hex64(fnv1a64(pixels.data(), pixels.size())) : std::string("none"))
                              << " " << width << "x" << height << std::endl;
                if (options.time)
                    std::cout << label << " time_ms=" << ms << std::endl;
                if (hasPicture && options.png)
                {
                    const std::string out = options.prefix + "-" + std::to_string(frame) + ".png";
                    ps2_host::writePng(out, pixels.data(), width, height, width * 4u);
                    std::cout << "frame " << frame << " -> " << out << " (" << width << "x" << height
                              << " dispfbp=" << displayFbp << " srcfbp=" << sourceFbp << ")" << std::endl;
                }
                else if (!hasPicture && !options.hash && !options.pictureHash)
                {
                    std::cout << "frame " << frame << ": no picture" << std::endl;
                }
            }
            else
                gs.latchHostPresentationFrame(); // (every frame's picture is made, as in replayGpu; not shown)
            frameTime = {};
            ++frame;
        }
        std::cout << events << " events, " << frame << " frames";
        if (options.time)
            std::cout << ", " << totalMs << " ms";
        std::cout << std::endl;
        return 0;
    }
}

int main(int argc, char **argv)
{
    // Picture quality options from the environment (ZX_GS_AA, ...).
    zx::gs_quality::applyEnvironment();
    Options options;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--no-png")
            options.png = false;
        else if (arg == "--hash")
            options.hash = true;
        else if (arg == "--picture-hash")
            options.pictureHash = true;
        else if (arg == "--dump-vram")
            options.dumpVram = true;
        else if (arg == "--time")
            options.time = true;
        else if (arg == "--repeat" && i + 1 < argc)
            options.repeat = std::max(1L, std::strtol(argv[++i], nullptr, 0));
        else if (arg == "--gpu")
            options.gpu = true;
        else if (arg == "--scale" && i + 1 < argc)
            options.scale = static_cast<uint32_t>(std::max(1L, std::strtol(argv[++i], nullptr, 0)));
        else if (arg == "--compare")
            options.compare = true;
        else if (arg == "--between" && i + 1 < argc)
        {
            const std::string list = argv[++i];
            for (size_t at = 0; at < list.size();)
            {
                const size_t comma = list.find(',', at);
                const std::string item = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                const float a = std::strtof(item.c_str(), nullptr);
                if (a > 0.0f && a <= 1.0f)
                    options.between.push_back(a);
                at = comma == std::string::npos ? list.size() : comma + 1;
            }
            options.gpu = true;
        }
        else if (arg.rfind("--", 0) == 0)
        {
            std::cerr << "unknown option " << arg << std::endl;
            return 2;
        }
        else
            positional.push_back(arg);
    }
    if (positional.size() < 2)
    {
        std::cerr << "usage: " << argv[0]
                  << " <trace.gsr> <out-prefix> [first-frame] [--no-png] [--hash] [--picture-hash] [--dump-vram] [--time] [--repeat n]"
                  << " [--gpu [--scale n] [--compare] [--between a,b,...]]"
                  << std::endl;
        return 2;
    }
    options.tracePath = positional[0];
    options.prefix = positional[1];
    options.firstFrame = positional.size() > 2 ? std::strtol(positional[2].c_str(), nullptr, 0) : 0;

    if (options.gpu)
    {
#if ZX_HAS_GS_GPU
        for (long pass = 0; pass < options.repeat; ++pass)
            if (const int result = replayGpu(options))
                return result;
        return 0;
#else
        std::cerr << "built without the GPU backend (ZX_GS_GPU)" << std::endl;
        return 2;
#endif
    }
    for (long pass = 0; pass < options.repeat; ++pass)
    {
        const int result = replayOnce(options, options.repeat > 1 ? static_cast<int>(pass) + 1 : 0);
        if (result != 0)
            return result;
    }
    return 0;
}
