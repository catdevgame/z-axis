// Hardware GS renderer on SDL_GPU (prototype). See GPU_BACKEND.md next to
// this file for the design, what is supported and what is not.

#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_gpu_texture_check.h"
#include "runtime/gs/gs_interpolate.h"
#include "runtime/gs/gs_quality.h"
#include "runtime/gs/gs_texture_mips.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_frame_trace.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/gs/ps2_gs_common.h"
#include "runtime/gs/ps2_gs_memory.h"
#include "gs_cpu_shared.h"
#include "gs_gpu_shaders_msl.h"
#if defined(PS2X_GS_GPU_DXIL)
#include "gs_gpu_shaders_dxil.h" // generated from gs_gpu_shaders.hlsl
#endif
#if defined(PS2X_GS_GPU_SPIRV)
#include "gs_gpu_shaders_spirv.h" // generated from gs_gpu_shaders.hlsl
#endif
#include "ps2_host_sdl.h"
#include "runtime/ps2_texture_pack.h"
#include "runtime/ps2_texture_replace.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>

using namespace GSCpuShared;
namespace texcheck = ps2x::gpu_texture_check;

namespace
{
    constexpr uint32_t kPageBytes = GSMem::TexturePageCache::kPageSize;
    constexpr uint32_t kPageCount = GSMem::PageMask::kPages;
    constexpr uint32_t kHostFrameWidth = 640u;
    constexpr uint32_t kHostFrameHeight = 512u;
    constexpr uint32_t kMaxTargetHeight = 2048u;

    struct GpuVertex
    {
        float pos[4];
        float color[4];
        float tex[4];
        float uvBounds[4];
    };

    // Matches FSUniforms in gs_gpu_shaders_msl.h.
    struct GpuFragmentUniforms
    {
        uint32_t cfg0[4];
        uint32_t cfg1[4];
        uint32_t cfg2[4];
        uint32_t cfg3[4];
        float f0[4];
        float f1[4];
        float f2[4]; // a texture pack picture's area (texture_pack::Region): origin, size; size 0 = the whole texture
    };
    static_assert(sizeof(GpuFragmentUniforms) == 112, "uniform layout");

    // Matches NVSUniforms in the shaders: a native draw's vertex stage.
    struct GpuNativeUniforms
    {
        float m[4][4];  // object -> GS window (row vectors), GSNativeDraw::transform
        float view[4];  // XYOFFSET (pixels), 2 / target width, 2 / target height
        float misc[4];  // 1 / Z scale, texture width, texture height, fog
        float opts[4];  // x: 1 = colour interpolated in perspective, y: alpha (< 0: the vertices'),
                        // z: normal offset / 32768, w: S/T transformed by uvm from (s, t, 1) (1) or the normal (2)
        float uvm[2][4]; // S, T = i.x * uvm[0].xy + i.y * uvm[0].zw + i.z * uvm[1].xy + uvm[1].zw
        float clip[4][4]; // GSNativeDraw::clipTransform
        float ext[4];     // x: GSNativeDraw::dropFlags, y: 1 = X, Y clamped to the GS window
        // The lights (a lit rigid draw's GSNativeLights, a skinned one's GSNativeSkinning).
        float lightDir[3][4];    // row k: component k of lights 0, 1, 2
        float lightColour[4][4]; // lights 0, 1, 2, the ambient
        float light[4];          // x: 1 = a rigid mesh lit from its normals, y: its colour's scale, z: its alpha
    };
    static_assert(sizeof(GpuNativeUniforms) == 224 + 48 + 64 + 16, "uniform layout");

    // Matches SVSUniforms in the shaders: a skinned native draw's vertex
    // stage (GSNativeSkinning; its lights in n).
    struct GpuSkinUniforms
    {
        GpuNativeUniforms n;
        float skin[4]; // x: 1 = lit, y: the colour's cap (GSNativeSkinning::maxColour), z: 1 = alpha lit too
        float palette[GSNativeSkinning::kPaletteRows][4];
    };
    static_assert(sizeof(GpuSkinUniforms) == 352 + 16 + 2048, "uniform layout");

    struct PresentUniforms
    {
        int32_t src1[4];
        int32_t src2[4];
        uint32_t cfg[4];
        int32_t out[4];
    };

    struct Rect
    {
        int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // inclusive
        bool empty() const { return x1 < x0 || y1 < y0; }
        void merge(const Rect &o)
        {
            if (o.empty())
                return;
            if (empty())
            {
                *this = o;
                return;
            }
            x0 = std::min(x0, o.x0);
            y0 = std::min(y0, o.y0);
            x1 = std::max(x1, o.x1);
            y1 = std::max(y1, o.y1);
        }
        bool overlaps(const Rect &o) const
        {
            return !empty() && !o.empty() && x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1;
        }
    };

    bool isColorPsm(uint8_t psm)
    {
        return psm == GS_PSM_CT32 || psm == GS_PSM_CT24 || psm == GS_PSM_CT16 || psm == GS_PSM_CT16S;
    }

    bool isDepthPsm(uint8_t psm)
    {
        return psm == GS_PSM_Z32 || psm == GS_PSM_Z24 || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S;
    }

    bool is16BitPsm(uint8_t psm)
    {
        return psm == GS_PSM_CT16 || psm == GS_PSM_CT16S || psm == GS_PSM_Z16 || psm == GS_PSM_Z16S;
    }

    // Formats that share a memory layout (and so a render target).
    uint8_t storageClass(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_CT24:
            return GS_PSM_CT32;
        case GS_PSM_Z24:
            return GS_PSM_Z32;
        default:
            return psm;
        }
    }

    uint32_t depthBits(uint8_t psm)
    {
        switch (psm)
        {
        case GS_PSM_Z32:
            return 32u;
        case GS_PSM_Z24:
            return 24u;
        default:
            return 16u;
        }
    }

    using ReadFunc = uint32_t (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t);
    using WriteFunc = void (*)(uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

    ReadFunc readFunc(uint8_t psm)
    {
        using namespace GSMem;
        switch (psm)
        {
        case GS_PSM_CT32: return ReadCT32;
        case GS_PSM_CT24: return ReadCT24;
        case GS_PSM_CT16: return ReadCT16;
        case GS_PSM_CT16S: return ReadCT16S;
        case GS_PSM_T8: return ReadP8;
        case GS_PSM_T8H: return ReadP8H;
        case GS_PSM_T4: return ReadP4;
        case GS_PSM_T4HL: return ReadP4HL;
        case GS_PSM_T4HH: return ReadP4HH;
        case GS_PSM_Z32: return ReadZ32;
        case GS_PSM_Z24: return ReadZ24;
        case GS_PSM_Z16: return ReadZ16;
        case GS_PSM_Z16S: return ReadZ16S;
        default: return ReadNull;
        }
    }

    WriteFunc writeFunc(uint8_t psm)
    {
        using namespace GSMem;
        switch (psm)
        {
        case GS_PSM_CT32: return WriteCT32;
        case GS_PSM_CT24: return WriteCT24;
        case GS_PSM_CT16: return WriteCT16;
        case GS_PSM_CT16S: return WriteCT16S;
        case GS_PSM_Z32: return WriteZ32;
        case GS_PSM_Z24: return WriteZ24;
        case GS_PSM_Z16: return WriteZ16;
        case GS_PSM_Z16S: return WriteZ16S;
        default: return WriteNull;
        }
    }

    // A palette's key: 64 bits at a time (FNV-1a, used before, went a byte at a time, a
    // microsecond for 256 colours, at every CLUT load). Only compared in this
    // process; a palette found by it is compared colour by colour.
    uint64_t paletteHash(const uint32_t *colors, size_t count)
    {
        uint64_t h = 0x9E3779B97F4A7C15ull ^ count;
        for (size_t i = 0; i + 1 < count; i += 2)
        {
            uint64_t w;
            std::memcpy(&w, colors + i, sizeof(w));
            h = (h ^ w) * 0xFF51AFD7ED558CCDull;
            h ^= h >> 32;
        }
        return h;
    }

    // Page size in pixels per storage format.
    void pageExtent(uint8_t psm, uint32_t &w, uint32_t &h)
    {
        switch (bitsPerPixel(psm))
        {
        case 32: w = 64u; h = 32u; break;
        case 16: w = 64u; h = 64u; break;
        case 8: w = 128u; h = 64u; break;
        default: w = 128u; h = 128u; break;
        }
    }

    static bool envFlag(const char *name)
    {
        const char *value = ps2x::boot::text(name); // the environment's, or the boot's (runtime/ps2_boot_config.h)
        return value && value[0] && value[0] != '0';
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        // As GSCpuBackend.
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = 640u;
            outHeight = 448u;
        }
        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    // 2D rectangles drawn as two triangles (the front end, HUD panels,
    // text) often end on a pixel's sample point (x = 639.5, y = 223.75 with
    // samples at x + 0.5). The CPU renderer covers such a pixel (its edges
    // are inclusive), the GPU does not, and upscaled it covers part of it:
    // a thin dark line at the right and bottom of the screen. For a flat
    // (one Z, one Q) triangle whose legs are horizontal and vertical, move
    // its corners to the edges of the native pixels the CPU renderer covers
    // and extend every attribute along its plane, so each sample still sees
    // the values of the original triangle. Both halves of a quad share the
    // same corners, so their diagonal still meets. Returns false (nothing
    // changed) for anything else.
    bool snapRectangleHalf(GpuVertex *v, bool fst)
    {
        constexpr float kTol = 0.07f; // one 1/16 step of a GS coordinate
        float minX = v[0].pos[0], maxX = minX, minY = v[0].pos[1], maxY = minY;
        for (int i = 1; i < 3; ++i)
        {
            if (v[i].pos[2] != v[0].pos[2] || (!fst && v[i].tex[2] != v[0].tex[2]))
                return false;
            minX = std::min(minX, v[i].pos[0]);
            maxX = std::max(maxX, v[i].pos[0]);
            minY = std::min(minY, v[i].pos[1]);
            maxY = std::max(maxY, v[i].pos[1]);
        }
        if (maxX - minX <= 2.0f * kTol || maxY - minY <= 2.0f * kTol)
            return false;
        for (int i = 0; i < 3; ++i)
            if ((v[i].pos[0] - minX > kTol && maxX - v[i].pos[0] > kTol) ||
                (v[i].pos[1] - minY > kTol && maxY - v[i].pos[1] > kTol))
                return false;
        const float x0 = std::ceil(minX - 0.5f), x1 = std::floor(maxX - 0.5f) + 1.0f;
        const float y0 = std::ceil(minY - 0.5f), y1 = std::floor(maxY - 0.5f) + 1.0f;
        if (x1 <= x0 || y1 <= y0)
            return false;
        const float ax = v[0].pos[0], ay = v[0].pos[1];
        const float e1x = v[1].pos[0] - ax, e1y = v[1].pos[1] - ay;
        const float e2x = v[2].pos[0] - ax, e2y = v[2].pos[1] - ay;
        const float det = e1x * e2y - e2x * e1y;
        if (std::fabs(det) < 1e-6f)
            return false;
        GpuVertex out[3];
        for (int i = 0; i < 3; ++i)
        {
            const float nx = v[i].pos[0] - minX <= kTol ? x0 : x1;
            const float ny = v[i].pos[1] - minY <= kTol ? y0 : y1;
            // Barycentric weights of the new corner in the original triangle.
            const float dx = nx - ax, dy = ny - ay;
            const float w1 = (dx * e2y - e2x * dy) / det;
            const float w2 = (e1x * dy - dx * e1y) / det;
            const float w0 = 1.0f - w1 - w2;
            out[i] = v[i];
            out[i].pos[0] = nx;
            out[i].pos[1] = ny;
            for (int c = 0; c < 4; ++c)
            {
                out[i].color[c] = std::clamp(w0 * v[0].color[c] + w1 * v[1].color[c] + w2 * v[2].color[c], 0.0f, 255.0f);
                out[i].tex[c] = w0 * v[0].tex[c] + w1 * v[1].tex[c] + w2 * v[2].tex[c];
            }
        }
        for (int i = 0; i < 3; ++i)
            v[i] = out[i];
        return true;
    }
}

struct GSGpuBackend::Impl
{
    // A GPU texture standing in for a GS frame or Z buffer.
    struct Target
    {
        uint32_t bp = 0;    // block address
        uint32_t bw = 1;    // buffer width / 64
        uint8_t psm = 0;
        bool depth = false;
        uint32_t width = 0; // GS pixels
        uint32_t height = 0;
        SDL_GPUTexture *tex = nullptr; // width * scaleX x height * scaleY
        GSMem::PageMask pages;      // everything width x height covers
        GSMem::PageMask dirtyPages; // drawn since the last write back
        Rect dirty;
        bool stale = false; // local memory is newer: reload before use
        uint64_t contentSerial = 0; // changes whenever its pixels may change (contentClock)
        uint64_t lastUse = 0;
        uint64_t passSerial = 0;
    };

    // A texture decoded from local memory.
    struct TexEntry
    {
        SDL_GPUTexture *tex = nullptr;
        uint32_t width = 0, height = 0;
        bool indexed = false;
        // Read from a target on the GPU (gatherTexture): which, and its
        // content then; tex can take another gather.
        bool gathered = false;
        const void *gatherTarget = nullptr;
        uint64_t gatherSerial = 0;
        GSMem::PageMask pages;
        uint64_t decodeGen = 0;
        uint64_t checkedEpoch = UINT64_MAX;
        uint64_t lastUse = 0;
        uint64_t passSerial = 0;
        // Replacement (runtime/ps2_texture_replace.h), indexed textures only.
        uint64_t indexHash = 0;       // of the decoded indices
        uint64_t replGen = 0;         // registry generation replPic was looked up at
        ps2x::texture_replace::PicturePtr replPic;
        uint64_t replPalette = 0;     // the palette first drawn with it
        bool replOff = false;         // drawn with another palette since: not replaced
        bool replRegionLogged = false;
        bool rgbHashed = false;       // RGBA texture with indexHash = rgbHash (see decodeTexture)
        // Changes at every decode (decodeSerialClock): names what tex holds
        // for the native draws' mipmapped copies (mipCopyFor).
        uint64_t decodeSerial = 0;
        // A mipmapped texture's first level decoded from local memory (the
        // levels above it lie in a render target).
        uint32_t mipSynced = 0;
        // Texture pack (runtime/ps2_texture_pack.h): the PCSX2 names of
        // what tex holds for the CLAMP and TEXA it was last drawn with
        // (texture_pack::candidateNames, their palette hash left 0), and
        // the picture found for them with the palette it was drawn with.
        uint64_t packSerial = 0; // decodeSerial the names are of
        uint64_t packShape = 0;  // the CLAMP and TEXA they are for
        struct PackNames
        {
            std::array<ps2x::texture_pack::Candidate, 8> names;
            std::array<ps2x::texture_pack::Level, 7> levels;
        };
        std::shared_ptr<PackNames> packNames;
        uint32_t packCount = 0, packChosen = 0;
        uint64_t packClut = 0, packGen = 0;
        ps2x::texture_replace::PicturePtr packPic;
    };

    // A replacement picture on the GPU.
    struct ReplTex
    {
        std::weak_ptr<const ps2x::texture_replace::Picture> pic;
        SDL_GPUTexture *tex = nullptr;
        size_t bytes = 0;
        uint64_t passSerial = 0;
    };

    // A fence shared by everything that waits for one submit.
    struct FenceRef
    {
        SDL_GPUDevice *dev = nullptr;
        SDL_GPUFence *fence = nullptr;
        ~FenceRef()
        {
            if (fence)
                SDL_ReleaseGPUFence(dev, fence);
        }
    };

    struct PendingUpload
    {
        SDL_GPUTexture *tex = nullptr;
        uint32_t width = 0, height = 0, bytesPerPixel = 4;
        uint32_t level = 0; // mipmap level
        std::vector<uint8_t> data;
        // Or a range of a replacement picture (not copied).
        ps2x::texture_replace::PicturePtr src;
        size_t srcOffset = 0, srcSize = 0;
        size_t bytes() const { return src ? srcSize : data.size(); }
        const uint8_t *bytesPtr() const { return src ? src->data.data() + srcOffset : data.data(); }
    };

    // A GS-path draw's place for frame interpolation (holdMotion, Interp): reproject, its vertices are 3D (Z
    // tested, not a sprite or the fog) and move with the camera; their GS window offset and Z scale, the GS
    // scissor (x0, y0, x1, y1: which view), stq: S, T, Q in perspective (scaled with w).
    struct GsTag
    {
        uint8_t reproject = 0;
        uint8_t stq = 0;
        float ofx = 0.0f, ofy = 0.0f, zScale = 0.0f;
        SDL_Rect scissor{};
        bool operator==(const GsTag &o) const
        {
            return reproject == o.reproject && stq == o.stq && ofx == o.ofx && ofy == o.ofy && zScale == o.zScale &&
                   scissor.x == o.scissor.x && scissor.y == o.scissor.y && scissor.w == o.scissor.w &&
                   scissor.h == o.scissor.h;
        }
    };

    struct DrawCall
    {
        uint64_t pipe = 0;
        // A native draw (SubmitNative): its mesh's vertex buffer and vertex
        // uniforms; first/count are in that buffer.
        SDL_GPUBuffer *nativeBuf = nullptr;
        uint32_t nativeOffset = 0;        // the mesh's place in nativeBuf (a mesh chunk)
        uint32_t skinOffset = 0;          // skinned: the mesh's GSNativeSkinWeights, from nativeOffset
        uint32_t skin = UINT32_MAX;       // skinned: its uniforms in skinUniforms
        GpuNativeUniforms nu{};
        GpuFragmentUniforms fu{};
        SDL_GPUTexture *tex[4]{};
        SDL_GPUSampler *sampler0 = nullptr; // texture 0's (null: the nearest one)
        SDL_Rect scissor{};
        float blend[4]{};
        uint32_t first = 0;
        uint32_t count = 0;
        // Frame interpolation: a native draw's motion (Interp's held frame), a GS-path draw's tag.
        uint32_t motion = UINT32_MAX;
        GsTag gs;
    };

    struct Pass
    {
        Target *color = nullptr;
        Target *depth = nullptr;
        std::vector<GpuVertex> verts;
        std::vector<DrawCall> draws;
        Rect copyRegion;               // of the colour target, copied to copyTex before the pass
        std::vector<Rect> slowBoxes;   // primitives reading that copy
        bool onlySlow = true;          // no other draw yet (more copy readers may join)
        bool feedback = false;         // a draw samples the copy as its texture

        // Back to an empty pass, keeping the vectors' storage: a pass is
        // thousands of vertices, and regrowing them from nothing every pass
        // (copies, and on Windows fresh pages from the kernel each time)
        // cost the GS worker more than the drawing.
        void reset()
        {
            color = nullptr;
            depth = nullptr;
            verts.clear();
            draws.clear();
            copyRegion = Rect{};
            slowBoxes.clear();
            onlySlow = true;
            feedback = false;
        }
    };

    struct PipeKey
    {
        uint8_t slow = 0;
        uint8_t depthOut = 0;
        uint8_t lines = 0;
        uint8_t hasDepth = 0;
        uint8_t depthOp = 0; // SDL_GPUCompareOp
        uint8_t depthWrite = 0;
        uint8_t writeMask = 0xF;
        uint8_t blend = 0;
        uint8_t srcColor = 0, dstColor = 0, colorOp = 0, srcAlpha = 0, dstAlpha = 0, alphaOp = 0;
        uint8_t native = 0; // native draw: its vertex shader and layout, depth clipping
        uint8_t depth16 = 0; // the Z target is D16 (hwDepth16); first, so keys without it read as 0

        uint64_t pack() const
        {
            uint64_t v = 0;
            const uint8_t f[] = {depth16, slow, depthOut, lines, hasDepth, depthOp, depthWrite, writeMask, blend,
                                 srcColor, dstColor, colorOp, srcAlpha, dstAlpha, alphaOp, native};
            for (uint8_t b : f)
                v = (v << 4) | (b & 0xFu);
            return v;
        }

        // pack()'s inverse (a key from the pipeline file).
        static PipeKey unpack(uint64_t v)
        {
            PipeKey k;
            uint8_t *f[] = {&k.depth16, &k.slow, &k.depthOut, &k.lines, &k.hasDepth, &k.depthOp, &k.depthWrite, &k.writeMask,
                            &k.blend, &k.srcColor, &k.dstColor, &k.colorOp, &k.srcAlpha, &k.dstAlpha, &k.alphaOp, &k.native};
            for (int i = 15; i >= 0; --i, v >>= 4)
                *f[i] = static_cast<uint8_t>(v & 0xFu);
            return k;
        }
    };

    struct BlendSetup
    {
        bool slow = false;
        bool keepColor = false; // RGB unchanged (only alpha is written)
        uint32_t premul = 0;
        uint32_t alphaOut = 0;
        bool enable = false;
        SDL_GPUBlendFactor srcColor = SDL_GPU_BLENDFACTOR_ONE;
        SDL_GPUBlendFactor dstColor = SDL_GPU_BLENDFACTOR_ZERO;
        SDL_GPUBlendOp op = SDL_GPU_BLENDOP_ADD;
        float constant = 0.0f;
        bool wrapTrick = false;
        bool readsVertices = false; // decided by the vertices' colours, not by the draw state alone
    };

    GSGpuBackendOptions options;
    // Internal resolution per GS pixel. With fullHeight the vertical one is
    // twice the horizontal: games that draw 224-line field frames (shown
    // line-doubled) get as much detail vertically as horizontally.
    uint32_t scaleX = 1, scaleY = 1;
    SDL_GPUDevice *dev = nullptr;
    SDL_GPUShaderFormat shaderFormat = SDL_GPU_SHADERFORMAT_INVALID;
    bool ownDevice = false;
    bool initTried = false;
    int hostDeviceHook = 0; // ps2_host::addGpuDeviceReleaseHook id
    bool ok = false;
    bool statsEnabled = false;
    bool trace = false; // PS2X_GS_GPU_TRACE=1: log target loads and write-backs
    bool deferWriteBacks = true; // PS2X_GS_GPU_DEFER=0: every write-back waits (see OwedWriteBack)

    GSCpuBackend cpu;
    uint8_t *vram = nullptr;
    uint32_t vramSize = 0;
    mutable std::recursive_mutex mutex;

    SDL_GPUShader *vs = nullptr;
    SDL_GPUShader *fs[2][2]{}; // [slow][depthOut]
    SDL_GPUShader *blitVs = nullptr, *depthLoadFs = nullptr, *depthStoreFs = nullptr, *presentFs = nullptr;
    SDL_GPUShader *gatherRgbaFs = nullptr, *gatherIndexFs = nullptr;
    std::unordered_map<uint64_t, SDL_GPUGraphicsPipeline *> pipelines; // by PipeKey::pack(), under pipelineMutex
    SDL_GPUGraphicsPipeline *depthLoadPipe = nullptr, *depthStorePipe = nullptr, *presentPipe = nullptr;
    SDL_GPUGraphicsPipeline *depthLoadPipe16 = nullptr; // for D16 Z targets (hwDepth16)
    // The Depth Test setting's Fast (gs_quality.h, PS2X_GS_DEPTH16), in a game
    // whose override allows it (PS2X_GS_D16_TARGETS, bmx_xxx.cpp): 16-bit Z
    // buffers are D16 targets. Their Z is stored as Z / 65535, the format's
    // own steps, so the GPU rounds depth to the GS's 16-bit Z itself and the
    // fragment shader writes none: a shader that writes depth turns the
    // GPU's early depth test off and shades every layer of overdraw (BMX
    // XXX at 2x full height on the Thor: 20 game fps against 46). Not exact:
    // the GPU compares and rounds the interpolated depth itself, and at
    // near-equal surfaces a few edge pixels come out otherwise (GPU_BACKEND.md).
    // Not for a game with native draws (their high-precision depth needs a float Z).
    bool fastDepth = false;       // the setting, read at init
    mutable int depth16 = -1;     // -1: not decided yet (at the first Z target: after the game's override ran)
    bool hwDepth16(uint8_t zpsm) const
    {
        if (depth16 < 0)
        {
            depth16 = fastDepth && depthLoadPipe16 && envFlag("PS2X_GS_D16_TARGETS") ? 1 : 0;
            if (depth16)
                std::cerr << "[gs-gpu] depth test: fast (16-bit Z buffers as D16 targets)" << std::endl;
        }
        return depth16 == 1 && depthBits(zpsm) == 16u;
    }
    // Stored depth to Z: d * depthScale.
    double depthScale(uint8_t zpsm) const
    {
        return hwDepth16(zpsm) ? 65535.0 : std::ldexp(1.0, static_cast<int>(depthBits(zpsm)));
    }
    SDL_GPUTextureFormat depthFormat(uint8_t zpsm) const
    {
        return hwDepth16(zpsm) ? SDL_GPU_TEXTUREFORMAT_D16_UNORM : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    }
    SDL_GPUGraphicsPipeline *gatherRgbaPipe = nullptr, *gatherIndexPipe = nullptr;
    SDL_GPUSampler *sampler = nullptr;
    SDL_GPUTexture *dummyRgba = nullptr, *dummyIndex = nullptr;
    // Replacement pictures: trilinear, anisotropic; [u clamp][v clamp].
    SDL_GPUSampler *replSampler[2][2]{};
    // Native draws' pictures (replacements and mipmapped copies of PS2
    // textures), by the texture filtering option (gs_quality.h Anisotropy):
    // [0 trilinear, 1..4 trilinear with 2x..16x anisotropic][u clamp][v clamp].
    SDL_GPUSampler *filterSampler[5][2][2]{};
    bool replBc2 = false; // the device samples BC2
    std::unordered_map<const ps2x::texture_replace::Picture *, ReplTex> replTextures;
    // replacementTexture's last entry (pointers into an unordered_map stay
    // valid until that element is erased).
    const ps2x::texture_replace::Picture *lastReplPic = nullptr;
    ReplTex *lastRepl = nullptr;
    uint64_t replSweepGen = 0;
    size_t replUploadedThisFrame = 0;
    // cfg0.x of a mipmapped copy magnified NEAREST (F_MAG_NEAREST in the shaders).
    static constexpr uint32_t kMagNearestFlag = 0x10000u;
    // cfg0.x of a native draw's picture: clamps by mip level (F_MIP_INSET).
    static constexpr uint32_t kMipInsetFlag = 0x20000u;
    bool mipInset = true; // PS2X_GS_MIP_INSET=0: half a level 0 texel at every level (diagnostics)
    size_t replGpuBytes = 0;
    static constexpr size_t kReplUploadPerFrame = size_t(24) << 20;
    // Texture pack pictures uploaded per presented frame (of the above):
    // a level's first frames take its pictures a few at a time, the PS2
    // textures drawn until theirs are up, instead of all in one long frame.
    // PS2X_TEXTURE_PACK_UPLOAD_MB.
    size_t packUploadPerFrame = size_t(6) << 20;
    size_t packUploadedThisFrame = 0;

    // Native meshes on the GPU (SubmitNative), by GSNativeMesh::id;
    // uploaded on first use, given back once the mesh is gone.
    struct NativeMeshGpu
    {
        std::weak_ptr<const GSNativeMesh> mesh;
        SDL_GPUBuffer *buf = nullptr; // its chunk's buffer
        uint32_t chunk = 0, offset = 0, span = 0;
        size_t bytes = 0;
    };
    // The meshes' vertex buffers: chunks of kMeshChunkBytes (or one mesh's
    // size, if larger), each mesh a range of one. One buffer per mesh cost
    // 0.2-7 ms each to create on Direct3D 12, and a new area of a level asks
    // for up to 150 in a frame. A mesh's range is free again after the
    // submit that follows its sweep (the copies into it come after every
    // draw recorded from it, on the same queue).
    struct MeshChunk
    {
        SDL_GPUBuffer *buf = nullptr;
        uint32_t size = 0;
        std::vector<std::pair<uint32_t, uint32_t>> free; // offset, size; sorted, coalesced
    };
    std::vector<MeshChunk> meshChunks;
    struct MeshRange
    {
        uint32_t chunk, offset, size;
    };
    std::vector<MeshRange> freeMeshRangesAfterSubmit;
    static constexpr uint32_t kMeshChunkBytes = 16u << 20;
    static constexpr uint32_t kMeshAlign = 256u;
    // Skinned native draws' vertex uniforms (DrawCall::skin), until their
    // pass is drawn.
    std::vector<GpuSkinUniforms> skinUniforms;
    struct PendingBufferUpload
    {
        SDL_GPUBuffer *buf = nullptr;
        uint32_t offset = 0;
        std::shared_ptr<const GSNativeMesh> mesh;
    };
    SDL_GPUShader *nativeVs = nullptr;
    SDL_GPUShader *nativeSkinVs = nullptr;
    std::unordered_map<uint64_t, NativeMeshGpu> nativeMeshes;
    std::vector<PendingBufferUpload> bufferUploads;
    std::vector<SDL_GPUBuffer *> releaseBuffersAfterSubmit;
    size_t nativeGpuBytes = 0;
    size_t nativeUploadedThisFrame = 0;
    static constexpr size_t kNativeUploadPerFrame = size_t(48) << 20;
    bool nativeTint = false; // PS2X_NATIVE_TINT=1: native draws tinted green
    bool nativeOff = false;  // PS2X_NATIVE_GPU=0: native draws as primitives (GS::drawNative's own)

    // A game's fog planes (GSFogPlanes, NoteFogPlanes): the notes in stream
    // order, and the plane triangles held back while a set comes in (drawn
    // as they are if it turns out not to be one).
    std::deque<GSFogPlanes> fogNotes;
    std::vector<GSPrimitiveBatch> fogHeld;
    SDL_GPUShader *fogFs = nullptr, *fogAlphaFs = nullptr, *fxaaFs = nullptr;
    SDL_GPUGraphicsPipeline *fogPipe = nullptr, *fogAlphaPipe = nullptr, *fxaaPipe = nullptr;
    SDL_GPUSampler *linearSampler = nullptr; // bilinear, clamped (FXAA)
    bool fogMismatchLogged = false;

    SDL_GPUCommandBuffer *cmd = nullptr;
    SDL_GPUBuffer *vbuf = nullptr;
    uint32_t vbufSize = 0, vbufUsed = 0;
    bool vbufCycle = true;
    SDL_GPUTransferBuffer *upTb = nullptr;
    uint32_t upTbSize = 0;
    SDL_GPUTransferBuffer *downTb = nullptr;
    uint32_t downTbSize = 0;

    Pass pass;
    bool passOpen = false;
    GsTag gsTag; // frame interpolation: the GS-path primitive being added (addDraw)
    uint64_t passSerial = 1;
    std::vector<PendingUpload> uploads;
    std::vector<SDL_GPUTexture *> releaseAfterSubmit;
    // Sampler textures given up (re-decoded or moved textures, palettes,
    // mip copies, replacement pictures) are kept for the next texture of
    // the same size, format and levels: creating one costs 0.2-1 ms on
    // Direct3D 12 (a committed resource), and a new area or a texture cache
    // refill asks for tens in a frame. A reused texture's first upload
    // cycles it (SDL_GPU: draws recorded before keep its old contents).
    struct PooledTexture
    {
        SDL_GPUTexture *tex = nullptr;
        uint64_t key = 0;
        size_t bytes = 0;
        uint64_t frame = 0; // poolFrame when given back
    };
    std::unordered_map<SDL_GPUTexture *, std::pair<uint64_t, size_t>> pooledKind; // textures that may be pooled
    std::unordered_map<uint64_t, std::vector<PooledTexture>> texturePool;
    std::unordered_set<SDL_GPUTexture *> cycleOnUpload;
    size_t texturePoolBytes = 0;
    uint64_t poolFrame = 0;
    static constexpr size_t kTexturePoolBytesMax = size_t(64) << 20;
    static constexpr uint64_t kTexturePoolIdleFrames = 1800; // given back for good after 30 s unused
    uint64_t poolHits = 0, poolMisses = 0;
    bool noTexturePool = false; // PS2X_GS_NO_TEXTURE_POOL=1: textures given back are released (as before)
    bool meshBufferEach = false; // PS2X_GS_MESH_BUFFER_EACH=1: a buffer per native mesh (as before)
    // Textures made ahead, per shape (poolKey), on a thread of their own:
    // the first texture of a shape is made where it is asked for and the
    // shape's stock then kept topped up (more after each miss), so a new
    // area's textures, palettes, mip copies and replacement pictures come
    // from the stock instead of a creation on the GS worker. Common shapes
    // are seeded when the renderer starts. A shape not asked for in 30 s
    // gives its stock back.
    struct StockShape
    {
        uint32_t w = 0, h = 0, levels = 1;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
        SDL_GPUTextureUsageFlags usage = 0;
        size_t bytes = 0;
        uint32_t want = 0;
        std::vector<SDL_GPUTexture *> ready;
        uint64_t lastAsk = 0; // poolFrame
    };
    std::mutex stockMutex;
    std::condition_variable stockCv;
    std::unordered_map<uint64_t, StockShape> stock;
    size_t stockBytes = 0; // ready textures
    bool stockStop = false;
    std::thread stockThread;
    uint64_t stockHits = 0;
    static constexpr size_t kStockBytesMax = size_t(96) << 20;
    static constexpr size_t kStockShapeBytesMax = size_t(16) << 20;
    static constexpr uint32_t kStockWantMax = 64;
    static constexpr uint64_t kStockIdleFrames = 1800;
    std::unordered_map<uint64_t, SDL_GPUTexture *> copyTextures; // by scaled size
    std::unordered_map<uint64_t, SDL_GPUTexture *> stagingTextures;

    std::vector<std::unique_ptr<Target>> targets;
    std::unordered_map<uint64_t, TexEntry> textures;
    // The entry of the last key asked: a mesh's primitives ask for the same
    // texture one after another (the map's nodes stay put until clear()).
    uint64_t lastTextureKey = 0;
    TexEntry *lastTextureEntry = nullptr;
    TexEntry &textureEntry(uint64_t key)
    {
        if (lastTextureEntry && lastTextureKey == key)
            return *lastTextureEntry;
        TexEntry &e = textures[key];
        lastTextureKey = key;
        lastTextureEntry = &e;
        return e;
    }
    void clearTextures()
    {
        textures.clear();
        lastTextureEntry = nullptr;
    }
    // Whether no page of `pages` was written after generation `gen` (the
    // set pages only, a word at a time).
    bool pagesUnchangedSince(const GSMem::PageMask &pages, uint64_t gen) const
    {
        for (size_t w = 0; w < pages.bits.size(); ++w)
        {
            uint64_t bits = pages.bits[w];
            while (bits != 0u)
            {
                const uint32_t page = static_cast<uint32_t>(w * 64u) + static_cast<uint32_t>(__builtin_ctzll(bits));
                if (pageGen[page] > gen)
                    return false;
                bits &= bits - 1u;
            }
        }
        return true;
    }
    struct Palette
    {
        SDL_GPUTexture *tex = nullptr;
        std::array<uint32_t, 256> colors{};
    };
    std::unordered_map<uint64_t, Palette> palettes;
    uint64_t clutEpoch = 1;   // CLUT loads and restores (the CPU backend's CLUT may have changed)
    uint64_t paletteEpoch = 0; // clutEpoch when `clut` was last taken from the CPU backend
    // Changes of `clut`'s contents: a load of the colours already there
    // (every material of a model reloading its CLUT) keeps the palettes.
    uint64_t clutContentEpoch = 1;
    std::array<uint16_t, 512> clut{};
    std::vector<uint8_t> clutStateScratch;
    // The CLUT entries a palette of the last load's selection reads come
    // from that load alone (clutLoadValid): its source (CBP, formats, CSA,
    // TEXCLUT) and the newest generation of its local memory pages. Such a
    // load's palettes are kept by both (a model's materials take turns
    // loading their own CLUTs: each gets its palette back without a rebuild).
    struct ClutLoad
    {
        uint64_t source = 0;
        uint64_t gen = 0;
        uint32_t select = 0; // palette()'s selection it serves
        bool operator==(const ClutLoad &o) const { return source == o.source && gen == o.gen && select == o.select; }
    };
    struct ClutLoadHash
    {
        size_t operator()(const ClutLoad &k) const { return std::hash<uint64_t>()(k.source * 0x9E3779B97F4A7C15ull ^ k.gen ^ (uint64_t(k.select) << 40)); }
    };
    struct SourcePalette
    {
        SDL_GPUTexture *tex = nullptr; // owned by `palettes`
        uint32_t texaClass = 0, alpha = 0;
        uint64_t content = 0;
    };
    bool clutLoadValid = false;
    ClutLoad clutLoad;
    std::unordered_map<ClutLoad, SourcePalette, ClutLoadHash> sourcePalettes;

    static uint32_t paletteSelect(const GSTex0Reg &tex)
    {
        return tex.cpsm | (static_cast<uint32_t>(tex.csa) << 8) | (static_cast<uint32_t>(tex.csm) << 16) |
               (isFourBitIndexedPsm(tex.psm) ? 1u << 20 : 0u);
    }

    std::array<uint64_t, kPageCount> pageGen{};
    uint64_t gen = 1;
    uint64_t epoch = 1; // bumped by every local memory write
    uint64_t useClock = 0;
    uint64_t contentClock = 0; // Target::contentSerial

    const char *traceReason = "";
    GSTransferCommand transfer{};
    GSMem::PageMask transferDest;

    std::vector<uint8_t> lastScaled;
    uint32_t lastScaledW = 0, lastScaledH = 0;
    Stats stats;

    explicit Impl(const GSGpuBackendOptions &o) : options(o)
    {
        scaleX = std::clamp<uint32_t>(o.scale, 1u, 8u);
        scaleY = o.fullHeight ? scaleX * 2u : scaleX;
#if defined(__ANDROID__)
        // PS2X_GS_SCALE=<x>x<y>: each on its own.
        if (const char *v = ps2x::boot::text("PS2X_GS_SCALE"); v && std::strchr(v, 'x'))
            scaleY = std::clamp<uint32_t>(static_cast<uint32_t>(std::atoi(std::strchr(v, 'x') + 1)), 1u, 16u);
#endif
        statsEnabled = envFlag("PS2X_GS_STATS");
        replTint = envFlag("PS2X_GS_REPL_TINT");
        if (const char *v = std::getenv("PS2X_GS_REPL_TINT"))
            replTint2 = std::atoi(v) >= 2;
        if (const char *v = std::getenv("PS2X_TRACE_TEXTURE_REPLACE"))
            traceReplace = std::atoi(v);
        contentKeys = envFlag("PS2X_TEXTURE_REPLACE_CONTENT");
        if (const char *mb = std::getenv("PS2X_TEXTURE_PACK_UPLOAD_MB"); mb && *mb)
            packUploadPerFrame = static_cast<size_t>(std::max(1l, std::strtol(mb, nullptr, 10))) << 20;
        trace = envFlag("PS2X_GS_GPU_TRACE");
        if (const char *v = ps2x::boot::text("PS2X_GS_DRAW_MEMO"); v && v[0] == '0')
            drawMemoOn = false;
        drawMemoCheck = envFlag("PS2X_GS_DRAW_MEMO_CHECK");
        if (const char *v = ps2x::boot::text("PS2X_GS_SHADER_DEPTH"); v && v[0] == '0')
            shaderDepth = false;
        // The Depth Test setting (gs_quality; PS2X_GS_DEPTH16 over it), where the game allows D16 targets
        // (PS2X_GS_D16_TARGETS: a game with no native draws, set by its override), read once: targets keep
        // their format, so a change applies from the next game start.
        fastDepth = ps2x::gs_quality::fastDepth();
        int depthTest = 0;
        if (const char *v = ps2x::boot::text("PS2X_GS_DEPTH16");
            v && v[0] && ps2x::gs_quality::parseValue(ps2x::gs_quality::Option::DepthTest, v, depthTest))
            fastDepth = depthTest == 1;
        // (D16 targets where the game allows them: decided at the first Z target, hwDepth16)
        nativeTint = envFlag("PS2X_NATIVE_TINT");
        interp.timing = envFlag("PS2X_INTERP_TIMING");
        interp.force = envFlag("PS2X_INTERP_FORCE");
        interp.mathAtOne = envFlag("PS2X_INTERP_MATH_AT_ONE");
        // PS2X_INTERP_LEAVE=letters (measuring what a picture between costs): c the start copies, p the passes,
        // f the fog and FXAA passes, r the presentation left out (the picture is then wrong).
        if (const char *leave = std::getenv("PS2X_INTERP_LEAVE"))
            interp.leave = leave;
        if (const char *v = std::getenv("PS2X_NATIVE_GPU"); v && v[0] == '0')
            nativeOff = true;
        if (const char *v = std::getenv("PS2X_GS_GPU_DEFER"); v && v[0] == '0')
            deferWriteBacks = false;
        if (const char *v = std::getenv("PS2X_GS_MIP_INSET"); v && v[0] == '0')
            mipInset = false;
        if (const char *v = std::getenv("PS2X_GS_MIP_BUDGET"))
            mipTexelsPerFrame = static_cast<size_t>(std::strtoull(v, nullptr, 10));
        cpu.SetDeferred(false);
        if (!o.hostDevice)
            init();
    }

    ~Impl()
    {
        if (hostDeviceHook)
            ps2_host::removeGpuDeviceReleaseHook(hostDeviceHook);
        shutdown();
    }

    // ---- device and pipeline setup ---------------------------------------

    // The shader formats this build has: MSL source (Metal, compiled by SDL
    // at run time) and, on Windows, DXIL compiled from the HLSL at build time
    // (Direct3D 12).
    static SDL_GPUShaderFormat builtShaderFormats()
    {
        SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_MSL;
#if defined(PS2X_GS_GPU_DXIL)
        formats |= SDL_GPU_SHADERFORMAT_DXIL;
#endif
#if defined(PS2X_GS_GPU_SPIRV)
        formats |= SDL_GPU_SHADERFORMAT_SPIRV;
#endif
        return formats;
    }

    // slow / depthOut: the GS_SLOW / GS_DEPTH_OUT variant (gs_fs only).
    SDL_GPUShader *makeShader(const char *entry, SDL_GPUShaderStage stage, uint32_t samplers, uint32_t uniforms,
                              int slow, int depthOut)
    {
        SDL_GPUShaderCreateInfo info{};
        info.entrypoint = entry;
        info.stage = stage;
        info.num_samplers = samplers;
        info.num_uniform_buffers = uniforms;
        std::string source;
        if (shaderFormat == SDL_GPU_SHADERFORMAT_MSL)
        {
            source = std::string("#define GS_SLOW ") + (slow ? "1" : "0") + "\n#define GS_DEPTH_OUT " +
                     (depthOut ? "1" : "0") + "\n" + kGsGpuShaderMsl;
            info.code = reinterpret_cast<const Uint8 *>(source.c_str());
            info.code_size = source.size();
            info.format = SDL_GPU_SHADERFORMAT_MSL;
        }
#if defined(PS2X_GS_GPU_DXIL)
        else if (shaderFormat == SDL_GPU_SHADERFORMAT_DXIL)
        {
            for (const gs_gpu_dxil::Shader &s : gs_gpu_dxil::kShaders)
                if (std::strcmp(s.entry, entry) == 0 && s.slow == slow && s.depthOut == depthOut)
                {
                    info.code = s.code;
                    info.code_size = s.size;
                }
            info.format = SDL_GPU_SHADERFORMAT_DXIL;
        }
#endif
#if defined(PS2X_GS_GPU_SPIRV)
        else if (shaderFormat == SDL_GPU_SHADERFORMAT_SPIRV)
        {
            for (const gs_gpu_spirv::Shader &s : gs_gpu_spirv::kShaders)
                if (std::strcmp(s.entry, entry) == 0 && s.slow == slow && s.depthOut == depthOut)
                {
                    info.code = s.code;
                    info.code_size = s.size;
                }
            info.format = SDL_GPU_SHADERFORMAT_SPIRV;
        }
#endif
        if (!info.code)
        {
            std::cerr << "[gs-gpu] no shader " << entry << std::endl;
            return nullptr;
        }
        SDL_GPUShader *shader = SDL_CreateGPUShader(dev, &info);
        if (!shader)
            std::cerr << "[gs-gpu] shader " << entry << " slow=" << slow << " depth=" << depthOut << ": " << SDL_GetError()
                      << std::endl;
        return shader;
    }

    void init()
    {
        initTried = true;
        dev = options.device;
        if (!dev && options.hostDevice)
        {
            dev = ps2_host::gpuDevice();
            options.readback = dev == nullptr;
        }
        if (!dev)
        {
            // SDL_GPU wants the video subsystem even without a window.
            if (!SDL_WasInit(SDL_INIT_VIDEO) && !SDL_InitSubSystem(SDL_INIT_VIDEO))
            {
                std::cerr << "[gs-gpu] SDL video: " << SDL_GetError() << std::endl;
                return;
            }
            dev = SDL_CreateGPUDevice(builtShaderFormats(), options.debug, nullptr);
            ownDevice = dev != nullptr;
        }
        if (!dev)
        {
            std::cerr << "[gs-gpu] no SDL_GPU device: " << SDL_GetError() << std::endl;
            return;
        }
        if (!ownDevice && dev == ps2_host::gpuDevice())
        {
            // The window's device: give everything back before the host
            // layer destroys it (the backend then draws nothing).
            hostDeviceHook = ps2_host::addGpuDeviceReleaseHook([this]()
                                                               {
                                                                   std::lock_guard<std::recursive_mutex> lock(mutex);
                                                                   hostDeviceHook = 0;
                                                                   shutdown();
                                                                   ok = false; });
        }
        const SDL_GPUShaderFormat formats = SDL_GetGPUShaderFormats(dev) & builtShaderFormats();
        if (formats & SDL_GPU_SHADERFORMAT_MSL)
            shaderFormat = SDL_GPU_SHADERFORMAT_MSL;
        else if (formats & SDL_GPU_SHADERFORMAT_DXIL)
            shaderFormat = SDL_GPU_SHADERFORMAT_DXIL;
#if defined(PS2X_GS_GPU_SPIRV)
        else if (formats & SDL_GPU_SHADERFORMAT_SPIRV)
            shaderFormat = SDL_GPU_SHADERFORMAT_SPIRV;
#endif
        else
        {
            std::cerr << "[gs-gpu] the device takes none of this build's shader formats (driver "
                      << SDL_GetGPUDeviceDriver(dev) << "; MSL for Metal, DXIL for Direct3D 12)" << std::endl;
            return;
        }
        // SDL_GPU wants a command buffer used only on the thread that
        // acquired it; Metal does not mind (see the command buffer section).
        threadOwnedCommands = std::strcmp(SDL_GetGPUDeviceDriver(dev), "metal") != 0;
        if (const char *v = std::getenv("PS2X_GS_GPU_THREAD_OWNED"); v && v[0])
            threadOwnedCommands = v[0] == '1';
        vs = makeShader("gs_vs", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 0, 0);
        nativeVs = makeShader("gs_native_vs", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 0, 0);
        nativeSkinVs = makeShader("gs_native_skin_vs", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1, 0, 0);
        for (int slow = 0; slow < 2; ++slow)
            for (int depth = 0; depth < 2; ++depth)
                fs[slow][depth] = makeShader("gs_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1, slow, depth);
        blitVs = makeShader("blit_vs", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 0, 0);
        depthLoadFs = makeShader("depth_load_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, 0, 0);
        depthStoreFs = makeShader("depth_store_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 0, 0);
        presentFs = makeShader("present_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, 0, 0);
        gatherRgbaFs = makeShader("gather_rgba_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, 0, 0);
        gatherIndexFs = makeShader("gather_index_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1, 0, 0);
        // Picture quality passes (fog, anti-aliasing): optional, the game
        // draws as before without them.
        fogFs = makeShader("fog_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, 0, 0);
        fogAlphaFs = makeShader("fog_alpha_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, 0, 0);
        fxaaFs = makeShader("fxaa_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, 0, 0);
        if (!vs || !nativeVs || !nativeSkinVs || !blitVs || !depthLoadFs || !depthStoreFs || !presentFs || !gatherRgbaFs || !gatherIndexFs)
            return;
        for (auto &row : fs)
            for (SDL_GPUShader *s : row)
                if (!s)
                    return;

        SDL_GPUSamplerCreateInfo si{};
        si.min_filter = SDL_GPU_FILTER_NEAREST;
        si.mag_filter = SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler = SDL_CreateGPUSampler(dev, &si);
        for (int cu = 0; cu < 2; ++cu)
            for (int cv = 0; cv < 2; ++cv)
            {
                SDL_GPUSamplerCreateInfo ri{};
                ri.min_filter = SDL_GPU_FILTER_LINEAR;
                ri.mag_filter = SDL_GPU_FILTER_LINEAR;
                ri.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
                ri.address_mode_u = cu ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
                ri.address_mode_v = cv ? SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE : SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
                ri.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
                ri.max_lod = envFlag("PS2X_GS_REPL_NO_MIPS") ? 0.0f : 1000.0f;
                if (const char *bias = std::getenv("PS2X_GS_REPL_LOD_BIAS"))
                    ri.mip_lod_bias = static_cast<float>(std::atof(bias));
                ri.enable_anisotropy = !envFlag("PS2X_GS_REPL_NO_ANISO");
                ri.max_anisotropy = 8.0f;
                replSampler[cu][cv] = SDL_CreateGPUSampler(dev, &ri);
                for (int a = 0; a < 5; ++a)
                {
                    ri.enable_anisotropy = a > 0;
                    ri.max_anisotropy = a > 0 ? static_cast<float>(1 << a) : 1.0f;
                    filterSampler[a][cu][cv] = SDL_CreateGPUSampler(dev, &ri);
                }
            }
        replBc2 = SDL_GPUTextureSupportsFormat(dev, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM, SDL_GPU_TEXTURETYPE_2D,
                                               SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (!replBc2)
            std::cerr << "[gs-gpu] the device has no BC2 textures: no replacement textures" << std::endl;

        dummyRgba = createTexture(1, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        dummyIndex = createTexture(1, 1, SDL_GPU_TEXTUREFORMAT_R8_UINT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        noTexturePool = envFlag("PS2X_GS_NO_TEXTURE_POOL");
        meshBufferEach = envFlag("PS2X_GS_MESH_BUFFER_EACH");
        if (!envFlag("PS2X_GS_NO_TEXTURE_STOCK"))
            startStock();

        // Fixed pipelines.
        {
            SDL_GPUGraphicsPipelineCreateInfo pi{};
            pi.vertex_shader = blitVs;
            pi.fragment_shader = depthLoadFs;
            pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pi.depth_stencil_state.enable_depth_test = true;
            pi.depth_stencil_state.enable_depth_write = true;
            pi.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
            pi.target_info.has_depth_stencil_target = true;
            pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
            depthLoadPipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
            if (fastDepth)
            {
                // (without it, D32F targets as with Exact)
                pi.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D16_UNORM;
                depthLoadPipe16 = SDL_CreateGPUGraphicsPipeline(dev, &pi);
            }
        }
        {
            SDL_GPUColorTargetDescription ct{};
            ct.format = SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
            SDL_GPUGraphicsPipelineCreateInfo pi{};
            pi.vertex_shader = blitVs;
            pi.fragment_shader = depthStoreFs;
            pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pi.target_info.color_target_descriptions = &ct;
            pi.target_info.num_color_targets = 1;
            depthStorePipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        }
        {
            SDL_GPUColorTargetDescription ct{};
            ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            SDL_GPUGraphicsPipelineCreateInfo pi{};
            pi.vertex_shader = blitVs;
            pi.fragment_shader = presentFs;
            pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pi.target_info.color_target_descriptions = &ct;
            pi.target_info.num_color_targets = 1;
            presentPipe = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        }
        for (int index = 0; index < 2; ++index)
        {
            SDL_GPUColorTargetDescription ct{};
            ct.format = index ? SDL_GPU_TEXTUREFORMAT_R8_UINT : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            SDL_GPUGraphicsPipelineCreateInfo pi{};
            pi.vertex_shader = blitVs;
            pi.fragment_shader = index ? gatherIndexFs : gatherRgbaFs;
            pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            pi.target_info.color_target_descriptions = &ct;
            pi.target_info.num_color_targets = 1;
            (index ? gatherIndexPipe : gatherRgbaPipe) = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        }
        {
            SDL_GPUSamplerCreateInfo li{};
            li.min_filter = SDL_GPU_FILTER_LINEAR;
            li.mag_filter = SDL_GPU_FILTER_LINEAR;
            li.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
            li.address_mode_u = li.address_mode_v = li.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
            linearSampler = SDL_CreateGPUSampler(dev, &li);
            // Fog: the fog colour over the target with alpha 1 - k^n (RGB
            // only); its alpha pass writes A only. FXAA: RGB only (the
            // game keeps counters in the frame buffer's alpha).
            auto quad = [&](SDL_GPUShader *fsh, bool blend, SDL_GPUColorComponentFlags mask) -> SDL_GPUGraphicsPipeline *
            {
                if (!fsh)
                    return nullptr;
                SDL_GPUColorTargetDescription ct{};
                ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
                ct.blend_state.enable_color_write_mask = true;
                ct.blend_state.color_write_mask = mask;
                if (blend)
                {
                    ct.blend_state.enable_blend = true;
                    ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
                    ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
                    ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
                    ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
                    ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
                    ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
                }
                SDL_GPUGraphicsPipelineCreateInfo pi{};
                pi.vertex_shader = blitVs;
                pi.fragment_shader = fsh;
                pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
                pi.target_info.color_target_descriptions = &ct;
                pi.target_info.num_color_targets = 1;
                return SDL_CreateGPUGraphicsPipeline(dev, &pi);
            };
            const SDL_GPUColorComponentFlags rgb = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G | SDL_GPU_COLORCOMPONENT_B;
            fogPipe = quad(fogFs, true, rgb);
            fogAlphaPipe = quad(fogAlphaFs, false, SDL_GPU_COLORCOMPONENT_A);
            fxaaPipe = quad(fxaaFs, false, rgb);
            if (!fogPipe || !fogAlphaPipe || !fxaaPipe || !linearSampler)
                std::cerr << "[gs-gpu] no fog / anti-aliasing passes: " << SDL_GetError() << std::endl;
        }
        if (!sampler || !dummyRgba || !dummyIndex || !depthLoadPipe || !depthStorePipe || !presentPipe || !gatherRgbaPipe ||
            !gatherIndexPipe)
        {
            std::cerr << "[gs-gpu] setup failed: " << SDL_GetError() << std::endl;
            return;
        }
        // Clear the dummies (never read for real, but keep them defined).
        {
            PendingUpload a;
            a.tex = dummyRgba;
            a.width = a.height = 1;
            a.data.assign(4, 0);
            uploads.push_back(std::move(a));
            PendingUpload b;
            b.tex = dummyIndex;
            b.width = b.height = 1;
            b.bytesPerPixel = 1;
            b.data.assign(1, 0);
            uploads.push_back(std::move(b));
        }
        ok = true;
        startPipelinesAhead();
    }

    void shutdown()
    {
        if (drawMemoCheck)
            std::cerr << "[gs-gpu] draw memo check: " << memoChecked << " primitives checked, " << memoDiffer << " differ" << std::endl;
        if (!dev)
            return;
        if (ok)
        {
            submit(false);
            SDL_WaitForGPUIdle(dev);
        }
        // Local memory still owed by deferred write-backs gets its pixels
        // (the device is idle now).
        while (ok && !owed.empty())
            materializeFront();
        dropClutSnapshots();
        for (auto &kv : gatherCoordTextures)
            texcheck::releaseTexture(dev, kv.second);
        gatherCoordTextures.clear();
        for (auto &r : retiringBuffers)
            SDL_ReleaseGPUTransferBuffer(dev, r.tb);
        retiringBuffers.clear();
        for (auto &r : retiringClutTextures)
            texcheck::releaseTexture(dev, r.tex);
        retiringClutTextures.clear();
        for (SDL_GPUTexture *t : spareClutTextures)
            texcheck::releaseTexture(dev, t);
        spareClutTextures.clear();
        for (auto &b : spareDownloadBuffers)
            SDL_ReleaseGPUTransferBuffer(dev, b.first);
        spareDownloadBuffers.clear();
        for (PresentPicture &p : pictures)
        {
            if (p.tex)
                texcheck::releaseTexture(dev, p.tex);
            p = PresentPicture{};
        }
        for (auto &t : targets)
            if (t->tex)
                texcheck::releaseTexture(dev, t->tex);
        targets.clear();
        targetsChanged();
        for (auto &kv : textures)
            if (kv.second.tex)
                texcheck::releaseTexture(dev, kv.second.tex);
        clearTextures();
        for (auto &kv : palettes)
            texcheck::releaseTexture(dev, kv.second.tex);
        palettes.clear();
        sourcePalettes.clear();
        for (auto &kv : replTextures)
            if (kv.second.tex)
                texcheck::releaseTexture(dev, kv.second.tex);
        replTextures.clear();
        lastRepl = nullptr;
        replGpuBytes = 0;
        for (auto &row : replSampler)
            for (SDL_GPUSampler *&s : row)
            {
                if (s)
                    SDL_ReleaseGPUSampler(dev, s);
                s = nullptr;
            }
        for (auto &level : filterSampler)
            for (auto &row : level)
                for (SDL_GPUSampler *&s : row)
                {
                    if (s)
                        SDL_ReleaseGPUSampler(dev, s);
                    s = nullptr;
                }
        for (auto &kv : mipCopies)
            if (kv.second.tex)
                texcheck::releaseTexture(dev, kv.second.tex);
        mipCopies.clear();
        mipCopyKeys.clear();
        mipCopyGpuBytes = 0;
        for (auto &kv : copyTextures)
            texcheck::releaseTexture(dev, kv.second);
        for (auto &kv : stagingTextures)
            texcheck::releaseTexture(dev, kv.second);
        stopStock();
        releasePool();
        for (SDL_GPUTexture *t : releaseAfterSubmit)
            texcheck::releaseTexture(dev, t);
        stopPipelinesAhead();
        for (auto &kv : pipelines)
            if (kv.second)
                SDL_ReleaseGPUGraphicsPipeline(dev, kv.second);
        pipelines.clear();
        if (depthLoadPipe) SDL_ReleaseGPUGraphicsPipeline(dev, depthLoadPipe);
        if (depthLoadPipe16) SDL_ReleaseGPUGraphicsPipeline(dev, depthLoadPipe16);
        if (depthStorePipe) SDL_ReleaseGPUGraphicsPipeline(dev, depthStorePipe);
        if (presentPipe) SDL_ReleaseGPUGraphicsPipeline(dev, presentPipe);
        if (gatherRgbaPipe) SDL_ReleaseGPUGraphicsPipeline(dev, gatherRgbaPipe);
        if (gatherIndexPipe) SDL_ReleaseGPUGraphicsPipeline(dev, gatherIndexPipe);
        for (SDL_GPUGraphicsPipeline **gp : {&fogPipe, &fogAlphaPipe, &fxaaPipe})
        {
            if (*gp)
                SDL_ReleaseGPUGraphicsPipeline(dev, *gp);
            *gp = nullptr;
        }
        for (SDL_GPUShader **sh : {&fogFs, &fogAlphaFs, &fxaaFs, &nativeSkinVs})
        {
            if (*sh)
                SDL_ReleaseGPUShader(dev, *sh);
            *sh = nullptr;
        }
        if (linearSampler)
            SDL_ReleaseGPUSampler(dev, linearSampler);
        linearSampler = nullptr;
        fogNotes.clear();
        fogHeld.clear();
        for (auto &row : fs)
            for (SDL_GPUShader *s : row)
                if (s) SDL_ReleaseGPUShader(dev, s);
        for (SDL_GPUShader *s : {vs, nativeVs, blitVs, depthLoadFs, depthStoreFs, presentFs, gatherRgbaFs, gatherIndexFs})
            if (s) SDL_ReleaseGPUShader(dev, s);
        for (MeshChunk &c : meshChunks)
            if (c.buf)
                SDL_ReleaseGPUBuffer(dev, c.buf);
        meshChunks.clear();
        freeMeshRangesAfterSubmit.clear();
        nativeMeshes.clear();
        bufferUploads.clear();
        nativeGpuBytes = 0;
        for (SDL_GPUBuffer *b : releaseBuffersAfterSubmit)
            SDL_ReleaseGPUBuffer(dev, b);
        releaseBuffersAfterSubmit.clear();
        if (sampler) SDL_ReleaseGPUSampler(dev, sampler);
        if (dummyRgba) texcheck::releaseTexture(dev, dummyRgba);
        if (dummyIndex) texcheck::releaseTexture(dev, dummyIndex);
        if (vbuf) SDL_ReleaseGPUBuffer(dev, vbuf);
        if (upTb) SDL_ReleaseGPUTransferBuffer(dev, upTb);
        if (downTb) SDL_ReleaseGPUTransferBuffer(dev, downTb);
        if (ownDevice)
        {
            texcheck::releaseHeld(dev);
            SDL_DestroyGPUDevice(dev);
        }
        dev = nullptr;
    }

    // Who creates a texture (the frame trace's argument).
    enum CreateSite : uint16_t
    {
        kSiteOther = 0,
        kSiteDecode,
        kSiteMipDecode,
        kSiteReplacement,
        kSiteMipCopy,
        kSitePalette,
        kSiteClut,
        kSiteTarget,
        kSiteGather,
        kSiteStock, // made ahead (the texture stock's thread)
    };

    SDL_GPUTexture *createTexture(uint32_t w, uint32_t h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                                  uint32_t levels = 1, uint16_t site = kSiteOther)
    {
        ps2x::ftrace::Scope traceCreate(ps2x::ftrace::kUser5, ps2x::ftrace::kUser5 + 100, site);
        SDL_GPUTextureCreateInfo ti{};
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = format;
        ti.usage = usage;
        ti.width = std::max<uint32_t>(w, 1u);
        ti.height = std::max<uint32_t>(h, 1u);
        ti.layer_count_or_depth = 1;
        ti.num_levels = std::max<uint32_t>(levels, 1u);
        SDL_GPUTexture *tex = texcheck::createTexture(dev, &ti);
        if (!tex)
            std::cerr << "[gs-gpu] texture " << w << "x" << h << ": " << SDL_GetError() << std::endl;
        return tex;
    }

    static uint64_t poolKey(uint32_t w, uint32_t h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, uint32_t levels)
    {
        return (static_cast<uint64_t>(w) << 48) | (static_cast<uint64_t>(h) << 32) | (static_cast<uint64_t>(format) << 16) |
               (static_cast<uint64_t>(usage & 0xFFu) << 8) | (levels & 0xFFu);
    }

    static size_t textureBytes(uint32_t w, uint32_t h, SDL_GPUTextureFormat format, uint32_t levels)
    {
        const size_t texel = format == SDL_GPU_TEXTUREFORMAT_R8_UINT ? 1u : format == SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM ? 1u : 4u;
        size_t bytes = 0;
        for (uint32_t k = 0; k < std::max<uint32_t>(levels, 1u); ++k)
            bytes += static_cast<size_t>(std::max<uint32_t>(w >> k, 1u)) * std::max<uint32_t>(h >> k, 1u) * texel;
        return bytes;
    }

    // createTexture for a sampler texture that is given back with
    // giveBackTexture: one from the pool when there is one.
    SDL_GPUTexture *pooledTexture(uint32_t w, uint32_t h, SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage,
                                  uint32_t levels, uint16_t site)
    {
        w = std::max<uint32_t>(w, 1u);
        h = std::max<uint32_t>(h, 1u);
        levels = std::max<uint32_t>(levels, 1u);
        const uint64_t key = poolKey(w, h, format, usage, levels);
        auto it = texturePool.find(key);
        if (!noTexturePool && it != texturePool.end() && !it->second.empty())
        {
            const PooledTexture p = it->second.back();
            it->second.pop_back();
            texturePoolBytes -= std::min(texturePoolBytes, p.bytes);
            cycleOnUpload.insert(p.tex);
            ++poolHits;
            return p.tex;
        }
        SDL_GPUTexture *t = takeFromStock(key, w, h, format, usage, levels);
        if (t)
            ++stockHits;
        else
        {
            t = createTexture(w, h, format, usage, levels, site);
            if (t)
                ++poolMisses;
        }
        if (t)
            pooledKind[t] = {key, textureBytes(w, h, format, levels)};
        return t;
    }

    // A made-ahead texture of this shape, or null (then the stock of the
    // shape is asked to grow).
    SDL_GPUTexture *takeFromStock(uint64_t key, uint32_t w, uint32_t h, SDL_GPUTextureFormat format,
                                  SDL_GPUTextureUsageFlags usage, uint32_t levels)
    {
        if (!stockThread.joinable())
            return nullptr;
        SDL_GPUTexture *t = nullptr;
        {
            std::lock_guard<std::mutex> lock(stockMutex);
            StockShape &sh = stock[key];
            if (sh.bytes == 0u)
            {
                sh.w = w;
                sh.h = h;
                sh.format = format;
                sh.usage = usage;
                sh.levels = levels;
                sh.bytes = textureBytes(w, h, format, levels);
            }
            sh.lastAsk = poolFrame;
            if (!sh.ready.empty())
            {
                t = sh.ready.back();
                sh.ready.pop_back();
                stockBytes -= std::min(stockBytes, sh.bytes);
            }
            else
            {
                const uint32_t most = static_cast<uint32_t>(std::max<size_t>(2u, kStockShapeBytesMax / std::max<size_t>(sh.bytes, 1u)));
                sh.want = std::min(std::min(kStockWantMax, most), std::max(8u, sh.want * 2u));
            }
        }
        stockCv.notify_one();
        return t;
    }

    void seedStock(uint32_t w, uint32_t h, SDL_GPUTextureFormat format, uint32_t levels, uint32_t want)
    {
        const uint64_t key = poolKey(w, h, format, SDL_GPU_TEXTUREUSAGE_SAMPLER, levels);
        StockShape &sh = stock[key];
        sh.w = w;
        sh.h = h;
        sh.format = format;
        sh.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        sh.levels = levels;
        sh.bytes = textureBytes(w, h, format, levels);
        sh.want = want;
    }

    void startStock()
    {
        {
            std::lock_guard<std::mutex> lock(stockMutex);
            // What a level asks for most (palettes, power-of-two textures,
            // Xbox pictures with their mip chains).
            seedStock(256, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 32);
            for (uint32_t size : {32u, 64u, 128u, 256u})
            {
                seedStock(size, size, SDL_GPU_TEXTUREFORMAT_R8_UINT, 1, 8);
                seedStock(size, size, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, 1, 4);
            }
            if (replBc2)
                for (uint32_t size : {64u, 128u, 256u, 512u})
                {
                    uint32_t levels = 1;
                    while ((size >> levels) >= 1u)
                        ++levels;
                    seedStock(size, size, SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM, levels, size >= 512u ? 4u : 8u);
                }
            stockStop = false;
        }
        stockThread = std::thread([this]()
                                  { stockWorker(); });
    }

    void stockWorker()
    {
        std::unique_lock<std::mutex> lock(stockMutex);
        for (;;)
        {
            StockShape *need = nullptr;
            uint64_t needKey = 0;
            if (!stockStop && stockBytes < kStockBytesMax)
                for (auto &kv : stock)
                    if (kv.second.ready.size() < kv.second.want && kv.second.bytes != 0u)
                    {
                        need = &kv.second;
                        needKey = kv.first;
                        break;
                    }
            if (stockStop)
                return;
            if (!need)
            {
                stockCv.wait(lock);
                continue;
            }
            const StockShape shape = *need; // its size, format, levels
            lock.unlock();
            SDL_GPUTexture *t = createTexture(shape.w, shape.h, shape.format, shape.usage, shape.levels, kSiteStock);
            lock.lock();
            if (!t)
            {
                stock[needKey].want = 0; // cannot: stop asking
                continue;
            }
            StockShape &sh = stock[needKey];
            sh.ready.push_back(t);
            stockBytes += sh.bytes;
        }
    }

    // At presented frames (every 64): shapes not asked for in a while give
    // their stock back.
    void trimStock()
    {
        std::vector<SDL_GPUTexture *> drop;
        {
            std::lock_guard<std::mutex> lock(stockMutex);
            for (auto &kv : stock)
            {
                StockShape &sh = kv.second;
                if (sh.lastAsk + kStockIdleFrames < poolFrame && sh.lastAsk != 0u)
                {
                    for (SDL_GPUTexture *t : sh.ready)
                        drop.push_back(t);
                    stockBytes -= std::min(stockBytes, sh.bytes * sh.ready.size());
                    sh.ready.clear();
                    sh.want = 0;
                }
            }
        }
        for (SDL_GPUTexture *t : drop)
            releaseAfterSubmit.push_back(t);
    }

    void stopStock()
    {
        if (!stockThread.joinable())
            return;
        {
            std::lock_guard<std::mutex> lock(stockMutex);
            stockStop = true;
        }
        stockCv.notify_all();
        stockThread.join();
        for (auto &kv : stock)
            for (SDL_GPUTexture *t : kv.second.ready)
                releaseAfterSubmit.push_back(t);
        stock.clear();
        stockBytes = 0;
    }

    // A texture no longer drawn from (after any pass that drew with it was
    // closed): kept for reuse if pooledTexture made it, else released after
    // the next submit.
    void giveBackTexture(SDL_GPUTexture *t)
    {
        if (!t)
            return;
        auto it = pooledKind.find(t);
        if (it == pooledKind.end() || noTexturePool)
        {
            if (it != pooledKind.end())
                pooledKind.erase(it);
            cycleOnUpload.erase(t);
            releaseAfterSubmit.push_back(t);
            return;
        }
        cycleOnUpload.erase(t);
        texturePool[it->second.first].push_back({t, it->second.first, it->second.second, poolFrame});
        texturePoolBytes += it->second.second;
        trimTexturePool(false);
    }

    // Over the cap (or `idle`: also those unused for kTexturePoolIdleFrames),
    // the oldest pooled textures are released.
    void trimTexturePool(bool idle)
    {
        while (texturePoolBytes > kTexturePoolBytesMax || idle)
        {
            std::vector<PooledTexture> *oldestList = nullptr;
            for (auto &kv : texturePool)
                if (!kv.second.empty() && (!oldestList || kv.second.front().frame < oldestList->front().frame))
                    oldestList = &kv.second;
            if (!oldestList)
                return;
            const PooledTexture p = oldestList->front();
            if (texturePoolBytes <= kTexturePoolBytesMax && p.frame + kTexturePoolIdleFrames >= poolFrame)
                return;
            oldestList->erase(oldestList->begin());
            texturePoolBytes -= std::min(texturePoolBytes, p.bytes);
            pooledKind.erase(p.tex);
            releaseAfterSubmit.push_back(p.tex);
        }
    }

    void releasePool()
    {
        for (auto &kv : texturePool)
            for (const PooledTexture &p : kv.second)
            {
                pooledKind.erase(p.tex);
                releaseAfterSubmit.push_back(p.tex);
            }
        texturePool.clear();
        texturePoolBytes = 0;
    }

    // ---- pipelines made ahead ---------------------------------------------

    // SDL_GPU passes the driver no pipeline cache, so a pipeline's first use
    // compiles it on the thread that draws with it (the GS worker, or the
    // window thread when a presentation closes the pass): 50-130 ms, a
    // stalled frame. With a pipeline file (GSGpuBackendOptions::pipelineFile,
    // one per game) the keys of the pipelines a game makes are kept there in
    // the order it first used them, and the next start makes them again on
    // a thread of its own (SDL_GPU creation is safe on any thread) while the
    // game boots. The pictures are the same: only when a pipeline is made
    // changes.
    static constexpr const char *kPipelineFileHeader = "# Z-Axis GS pipelines 1 (PipeKey::pack in hex, in first-use order)";
    std::mutex pipelineMutex;
    std::condition_variable pipelineCv;    // the made-ahead thread finished a key
    uint64_t pipelineMaking = UINT64_MAX;  // the key it makes now (under pipelineMutex)
    std::vector<uint64_t> pipelinesAhead;  // the file's keys
    std::unordered_set<uint64_t> pipelinesFiled;
    std::thread pipelineThread;
    std::atomic<bool> pipelineStop{false};

    // Reads the file's keys (a file of another layout, or none: a new file).
    void loadPipelineFile()
    {
        pipelinesAhead.clear();
        pipelinesFiled.clear();
        std::ifstream in(options.pipelineFile);
        std::string line;
        if (in && std::getline(in, line) && line == kPipelineFileHeader)
        {
            while (std::getline(in, line))
            {
                char *end = nullptr;
                const uint64_t key = std::strtoull(line.c_str(), &end, 16);
                if (end != line.c_str() && PipeKey::unpack(key).pack() == key && pipelinesFiled.insert(key).second)
                    pipelinesAhead.push_back(key);
            }
            return;
        }
        in.close();
        std::ofstream out(options.pipelineFile, std::ios::trunc);
        out << kPipelineFileHeader << '\n';
    }

    // A pipeline made for the first time: its key goes to the file.
    void filePipeline(uint64_t key)
    {
        if (options.pipelineFile.empty() || !pipelinesFiled.insert(key).second)
            return;
        std::ofstream out(options.pipelineFile, std::ios::app);
        char text[24];
        std::snprintf(text, sizeof(text), "%016llx\n", static_cast<unsigned long long>(key));
        out << text;
    }

    void startPipelinesAhead()
    {
        if (options.pipelineFile.empty())
            return;
        loadPipelineFile();
        if (pipelinesAhead.empty())
            return;
        pipelineStop = false;
        pipelineThread = std::thread([this]()
                                     { pipelinesAheadWorker(); });
    }

    void pipelinesAheadWorker()
    {
        const auto start = std::chrono::steady_clock::now();
        size_t made = 0;
        for (const uint64_t key : pipelinesAhead)
        {
            if (pipelineStop.load(std::memory_order_relaxed))
                break;
            {
                std::lock_guard<std::mutex> lock(pipelineMutex);
                if (pipelines.count(key) != 0u)
                    continue; // drawn with before its turn here
                pipelineMaking = key;
            }
            SDL_GPUGraphicsPipeline *p = createPipeline(PipeKey::unpack(key));
            {
                std::lock_guard<std::mutex> lock(pipelineMutex);
                pipelineMaking = UINT64_MAX;
                pipelines.emplace(key, p);
            }
            pipelineCv.notify_all();
            ++made;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cerr << "[gs-gpu] " << made << " of " << pipelinesAhead.size() << " pipelines made ahead (" << static_cast<int>(ms)
                  << " ms)" << std::endl;
    }

    void stopPipelinesAhead()
    {
        pipelineStop = true;
        if (pipelineThread.joinable())
            pipelineThread.join();
    }

    SDL_GPUGraphicsPipeline *pipeline(const PipeKey &k)
    {
        const uint64_t key = k.pack();
        {
            // (one being made ahead: waited for, not made twice)
            std::unique_lock<std::mutex> lock(pipelineMutex);
            pipelineCv.wait(lock, [&]()
                            { return pipelineMaking != key; });
            auto it = pipelines.find(key);
            if (it != pipelines.end())
                return it->second;
        }
        SDL_GPUGraphicsPipeline *p = createPipeline(k);
        {
            std::lock_guard<std::mutex> lock(pipelineMutex);
            const auto [it, added] = pipelines.emplace(key, p);
            if (!added)
            {
                // The made-ahead thread got to it meanwhile.
                if (p)
                    SDL_ReleaseGPUGraphicsPipeline(dev, p);
                p = it->second;
            }
        }
        filePipeline(key);
        return p;
    }

    SDL_GPUGraphicsPipeline *createPipeline(const PipeKey &k)
    {
        SDL_GPUVertexBufferDescription vbs[2]{};
        SDL_GPUVertexBufferDescription &vb = vbs[0];
        vb.slot = 0;
        vb.pitch = sizeof(GpuVertex);
        vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
        SDL_GPUVertexAttribute attrs[7]{};
        for (uint32_t i = 0; i < 4; ++i)
        {
            attrs[i].location = i;
            attrs[i].buffer_slot = 0;
            attrs[i].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
            attrs[i].offset = i * 16u;
        }
        SDL_GPUColorTargetDescription ct{};
        ct.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ct.blend_state.enable_blend = k.blend != 0;
        ct.blend_state.src_color_blendfactor = static_cast<SDL_GPUBlendFactor>(k.srcColor);
        ct.blend_state.dst_color_blendfactor = static_cast<SDL_GPUBlendFactor>(k.dstColor);
        ct.blend_state.color_blend_op = static_cast<SDL_GPUBlendOp>(k.colorOp);
        ct.blend_state.src_alpha_blendfactor = static_cast<SDL_GPUBlendFactor>(k.srcAlpha);
        ct.blend_state.dst_alpha_blendfactor = static_cast<SDL_GPUBlendFactor>(k.dstAlpha);
        ct.blend_state.alpha_blend_op = static_cast<SDL_GPUBlendOp>(k.alphaOp);
        ct.blend_state.enable_color_write_mask = true;
        ct.blend_state.color_write_mask = k.writeMask;

        if (k.native)
        {
            // GSNativeVertex: position, S/T, RGBA8 (integers), S/T range.
            vb.pitch = sizeof(GSNativeVertex);
            attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
            attrs[0].offset = offsetof(GSNativeVertex, pos);
            attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
            attrs[1].offset = offsetof(GSNativeVertex, st);
            attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4;
            attrs[2].offset = offsetof(GSNativeVertex, rgba);
            attrs[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
            attrs[3].offset = offsetof(GSNativeVertex, stBounds);
            attrs[4].location = 4;
            attrs[4].buffer_slot = 0;
            attrs[4].format = SDL_GPU_VERTEXELEMENTFORMAT_SHORT4;
            attrs[4].offset = offsetof(GSNativeVertex, normal);
            if (k.native == 2)
            {
                // Skinned: GSNativeSkinWeights in a second stream.
                vbs[1].slot = 1;
                vbs[1].pitch = sizeof(GSNativeSkinWeights);
                vbs[1].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
                attrs[5] = {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, offsetof(GSNativeSkinWeights, bone)};
                attrs[6] = {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, offsetof(GSNativeSkinWeights, weight)};
            }
        }
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = k.native == 2 ? nativeSkinVs : k.native ? nativeVs : vs;
        pi.fragment_shader = fs[k.slow][k.depthOut];
        pi.vertex_input_state.vertex_buffer_descriptions = vbs;
        pi.vertex_input_state.num_vertex_buffers = k.native == 2 ? 2 : 1;
        pi.vertex_input_state.vertex_attributes = attrs;
        pi.vertex_input_state.num_vertex_attributes = k.native == 2 ? 7 : k.native ? 5 : 4;
        pi.primitive_type = k.lines ? SDL_GPU_PRIMITIVETYPE_LINELIST : SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        // Native geometry is clipped at the GS's depth range (Z 0..2^bits:
        // the near plane where Z reaches the top, as the VU1 clipper cuts).
        pi.rasterizer_state.enable_depth_clip = k.native != 0;
        if (k.hasDepth)
        {
            pi.depth_stencil_state.enable_depth_test = true;
            pi.depth_stencil_state.enable_depth_write = k.depthWrite != 0;
            pi.depth_stencil_state.compare_op = static_cast<SDL_GPUCompareOp>(k.depthOp);
            pi.target_info.has_depth_stencil_target = true;
            pi.target_info.depth_stencil_format = k.depth16 ? SDL_GPU_TEXTUREFORMAT_D16_UNORM : SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        }
        pi.target_info.color_target_descriptions = &ct;
        pi.target_info.num_color_targets = 1;
        SDL_GPUGraphicsPipeline *p = nullptr;
        {
            ps2x::ftrace::Scope traceCreate(ps2x::ftrace::kPipelineCreate, ps2x::ftrace::kPipelineCreate + 100, k.native);
            p = SDL_CreateGPUGraphicsPipeline(dev, &pi);
        }
        if (!p)
            std::cerr << "[gs-gpu] pipeline: " << SDL_GetError() << std::endl;
        return p;
    }

    // ---- command buffer --------------------------------------------------

    SDL_GPUCommandBuffer *commandBuffer()
    {
        if (!cmd)
        {
            ps2x::ftrace::Scope traceAcquire(ps2x::ftrace::kUser3, ps2x::ftrace::kUser3 + 100);
            cmd = SDL_AcquireGPUCommandBuffer(dev);
            cmdThread = SDL_GetCurrentThreadID();
        }
        return cmd;
    }

    // SDL_GPU uses a command buffer only on the thread that acquired it
    // (Metal does not mind, Direct3D 12 and Vulkan do). The GS worker draws
    // and records most commands; the window thread presents and others read
    // back. With threadOwnedCommands every public call runs in a CallScope:
    // on entry it waits until no other thread holds an open command buffer
    // (that thread submits it when its current call ends, or when the GS
    // worker runs out of work), and on exit a thread other than the drawing
    // one submits what it recorded, so no command buffer outlives a call on
    // a thread that may not come back.
    bool threadOwnedCommands = false;
    SDL_ThreadID cmdThread = 0;  // the thread that acquired cmd
    SDL_ThreadID drawThread = 0; // the thread that submits primitives
    std::atomic<int> cmdWaiters{0}; // changed under the mutex; read without it by letWaitersIn
    int callDepth = 0;
    bool warnedTakeover = false;
    std::condition_variable_any cmdCv;

    void claimCommands(std::unique_lock<std::recursive_mutex> &lock)
    {
        if (!threadOwnedCommands || !cmd)
            return;
        const SDL_ThreadID me = SDL_GetCurrentThreadID();
        if (cmdThread == me)
            return;
        ++cmdWaiters;
        ps2x::ftrace::Scope traceClaim(ps2x::ftrace::kGpuClaimBegin, ps2x::ftrace::kGpuClaimEnd);
        cmdCv.wait_for(lock, std::chrono::milliseconds(20), [&]()
                       { return !cmd || cmdThread == me; });
        --cmdWaiters;
        if (cmd && cmdThread != me)
        {
            // The owner is busy elsewhere for long (drawing on the game
            // thread without the GS worker): go on with its commands here.
            if (!warnedTakeover)
                std::cerr << "[gs-gpu] took over a command buffer from another thread" << std::endl;
            warnedTakeover = true;
            cmdThread = me;
        }
    }

    void releaseCommands()
    {
        if (!threadOwnedCommands)
            return;
        const SDL_ThreadID me = SDL_GetCurrentThreadID();
        if (cmd && cmdThread == me && (cmdWaiters > 0 || me != drawThread))
            submit(false);
        if (cmdWaiters > 0)
            cmdCv.notify_all();
    }

    // Threads other than the drawing one blocked on `mutex`. The drawing
    // thread takes it for every primitive; a mutex that does not hand over
    // (Windows' SRW locks) would wake such a waiter after each one for
    // nothing, at a kernel call each, until it happens to win. Waiters say
    // so, and the drawing thread lets them in before its next call.
    std::atomic<int> otherWaiters{0};
    std::atomic<SDL_ThreadID> drawThreadId{0};
    static inline thread_local int t_scopeDepth = 0; // CallScopes open on this thread (any backend)

    struct CallScope
    {
        Impl &im;
        std::unique_lock<std::recursive_mutex> lock;
        // quiet: the drawing thread's own scope for primitives or a packet,
        // which leaves the draw memo standing; any other call ends it.
        explicit CallScope(Impl &impl, bool quiet = false) : im(impl), lock(impl.mutex, std::defer_lock)
        {
            const bool drawer = SDL_GetCurrentThreadID() == im.drawThreadId.load(std::memory_order_relaxed);
            if (drawer && t_scopeDepth == 0 && im.otherWaiters.load(std::memory_order_acquire) != 0)
            {
                // At most a millisecond (the waiter may wait for something else).
                const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
                while (im.otherWaiters.load(std::memory_order_acquire) != 0 && std::chrono::steady_clock::now() < until)
                    std::this_thread::yield();
            }
            if (!lock.try_lock())
            {
                ps2x::ftrace::Scope traceLock(ps2x::ftrace::kUser4, ps2x::ftrace::kUser4 + 100);
                if (!drawer)
                    im.otherWaiters.fetch_add(1, std::memory_order_acq_rel);
                lock.lock();
                if (!drawer)
                    im.otherWaiters.fetch_sub(1, std::memory_order_acq_rel);
            }
            ++t_scopeDepth;
            if (!quiet || !drawer)
                ++im.drawMemoSerial;
            if (im.callDepth == 0)
                im.claimCommands(lock);
            ++im.callDepth;
        }
        ~CallScope()
        {
            if (--im.callDepth == 0)
                im.releaseCommands();
            --t_scopeDepth;
        }
        CallScope(const CallScope &) = delete;
        CallScope &operator=(const CallScope &) = delete;
    };

    // A GIF packet (BeginPacket .. EndPacket) runs in one CallScope held
    // over the whole packet by its thread, so Submit, once per primitive,
    // takes no lock of its own. Between primitives it lets in the threads
    // that wait: for the mutex (otherWaiters) or for this thread's command
    // buffer (cmdWaiters), as the end of a per-primitive scope did.
    std::optional<CallScope> packetScope;
    int packetDepth = 0;
    static inline thread_local const Impl *t_packetOwner = nullptr; // the Impl whose packetScope this thread holds

    void beginPacket()
    {
        if (packetDepth++ == 0)
        {
            packetScope.emplace(*this, true);
            t_packetOwner = this;
        }
    }

    void endPacket()
    {
        if (--packetDepth == 0)
        {
            t_packetOwner = nullptr;
            packetScope.reset();
        }
    }

    // In a packet, before a primitive: when another thread waits, the
    // packet's scope ends (submitting the command buffer for a waiter on
    // it), the waiter goes first (at most a millisecond), and a new scope
    // goes on with the packet.
    void letWaitersIn()
    {
        if (t_scopeDepth != 1) // a call inside another one: the packet goes on
            return;
        if (otherWaiters.load(std::memory_order_acquire) == 0 && cmdWaiters.load(std::memory_order_acquire) == 0)
            return;
        packetScope.reset();
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
        while ((otherWaiters.load(std::memory_order_acquire) != 0 || cmdWaiters.load(std::memory_order_acquire) != 0) &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::yield();
        packetScope.emplace(*this, true); // (the waiters' own scopes ended the draw memo)
    }

    // A public call the GS worker makes inside packets: in the packet's
    // scope, after letting waiters in, else in a CallScope of its own.
    // primitive: Submit's, which leaves the draw memo standing.
    struct CallEntry
    {
        std::optional<CallScope> scope;
        explicit CallEntry(Impl &im, bool primitive = false)
        {
            if (t_packetOwner == &im)
            {
                im.letWaitersIn();
                if (!primitive)
                    ++im.drawMemoSerial;
            }
            else
                scope.emplace(im, primitive);
        }
    };

    void ensureUploadBuffer(uint32_t size)
    {
        if (upTb && upTbSize >= size)
            return;
        if (upTb)
            SDL_ReleaseGPUTransferBuffer(dev, upTb);
        upTbSize = std::max<uint32_t>(size, 8u << 20);
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        ti.size = upTbSize;
        ps2x::ftrace::Scope traceCreate(ps2x::ftrace::kBufferCreate, ps2x::ftrace::kBufferCreate + 100, 1);
        upTb = SDL_CreateGPUTransferBuffer(dev, &ti);
    }

    void ensureDownloadBuffer(uint32_t size)
    {
        if (downTb && downTbSize >= size)
            return;
        if (downTb)
            SDL_ReleaseGPUTransferBuffer(dev, downTb);
        downTbSize = std::max<uint32_t>(size, 8u << 20);
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        ti.size = downTbSize;
        downTb = SDL_CreateGPUTransferBuffer(dev, &ti);
    }

    // Records the pending texture uploads and `extra` bytes of vertices
    // into one copy pass; returns the vertex buffer offset used.
    uint32_t recordUploads(const void *vertexData, uint32_t vertexBytes, SDL_GPUCopyPass **outCopy)
    {
        // Texture data at 512-byte offsets: block aligned for compressed
        // formats (Metal) and placement aligned for Direct3D 12.
        const auto alignUp = [](uint32_t v)
        { return (v + 511u) & ~511u; };
        uint32_t total = vertexBytes;
        for (const PendingUpload &u : uploads)
            total = alignUp(total) + static_cast<uint32_t>(u.bytes());
        for (const PendingBufferUpload &u : bufferUploads)
            total = alignUp(total) + static_cast<uint32_t>(nativeMeshBytes(*u.mesh));
        uint32_t vertexOffset = 0;
        if (total == 0u)
        {
            *outCopy = nullptr;
            return 0;
        }
        ensureUploadBuffer(total);
        ps2x::ftrace::Scope traceUpload(ps2x::ftrace::kUser6, ps2x::ftrace::kUser6 + 100);
        uint8_t *map = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(dev, upTb, true));
        uint32_t at = 0;
        if (vertexBytes)
        {
            std::memcpy(map, vertexData, vertexBytes);
            at = vertexBytes;
        }
        for (const PendingUpload &u : uploads)
        {
            at = alignUp(at);
            std::memcpy(map + at, u.bytesPtr(), u.bytes());
            at += static_cast<uint32_t>(u.bytes());
        }
        for (const PendingBufferUpload &u : bufferUploads)
        {
            at = alignUp(at);
            const size_t bytes = u.mesh->vertices.size() * sizeof(GSNativeVertex);
            std::memcpy(map + at, u.mesh->vertices.data(), bytes);
            at += static_cast<uint32_t>(bytes);
            const size_t skinBytes = u.mesh->skin.size() * sizeof(GSNativeSkinWeights);
            if (skinBytes)
                std::memcpy(map + at, u.mesh->skin.data(), skinBytes);
            at += static_cast<uint32_t>(skinBytes);
        }
        SDL_UnmapGPUTransferBuffer(dev, upTb);

        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer());
        at = 0;
        if (vertexBytes)
        {
            if (!vbuf || vbufUsed + vertexBytes > vbufSize)
            {
                if (vbuf && vertexBytes <= vbufSize)
                {
                    // Full: start over in a fresh backing buffer.
                    vbufUsed = 0;
                    vbufCycle = true;
                }
                else
                {
                    if (vbuf)
                        SDL_ReleaseGPUBuffer(dev, vbuf);
                    vbufSize = std::max<uint32_t>(vertexBytes, 16u << 20);
                    SDL_GPUBufferCreateInfo bi{};
                    bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
                    bi.size = vbufSize;
                    vbuf = SDL_CreateGPUBuffer(dev, &bi);
                    vbufUsed = 0;
                    vbufCycle = false;
                }
            }
            SDL_GPUTransferBufferLocation src{upTb, 0};
            SDL_GPUBufferRegion dst{vbuf, vbufUsed, vertexBytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, vbufCycle);
            vbufCycle = false;
            vertexOffset = vbufUsed;
            vbufUsed += vertexBytes;
            at = vertexBytes;
        }
        for (const PendingUpload &u : uploads)
        {
            at = alignUp(at);
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = upTb;
            src.offset = at;
            src.pixels_per_row = u.width;
            src.rows_per_layer = u.height;
            SDL_GPUTextureRegion dst{};
            dst.texture = u.tex;
            dst.mip_level = u.level;
            dst.w = u.width;
            dst.h = u.height;
            dst.d = 1;
            // A pooled texture's first upload cycles it (its earlier draws keep the old texels).
            heldTextureWritten(u.tex);
            texcheck::uploadToTexture(copy, &src, &dst, cycleOnUpload.erase(u.tex) != 0u);
            at += static_cast<uint32_t>(u.bytes());
            ++stats.textureUploads;
        }
        uploads.clear();
        for (const PendingBufferUpload &u : bufferUploads)
        {
            at = alignUp(at);
            const uint32_t bytes = static_cast<uint32_t>(nativeMeshBytes(*u.mesh));
            SDL_GPUTransferBufferLocation src{upTb, at};
            SDL_GPUBufferRegion dst{u.buf, u.offset, bytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, false);
            at += bytes;
            stats.nativeMeshBytes += bytes;
        }
        bufferUploads.clear();
        *outCopy = copy;
        return vertexOffset;
    }

    // A native mesh's GPU buffer: its vertices, then (skinned) their bones.
    static size_t nativeMeshBytes(const GSNativeMesh &mesh)
    {
        return mesh.vertices.size() * sizeof(GSNativeVertex) + mesh.skin.size() * sizeof(GSNativeSkinWeights);
    }

    // A range of kMeshAlign-aligned bytes in a mesh chunk (first fit; a
    // new chunk when none has room).
    bool allocMeshRange(uint32_t bytes, uint32_t &chunk, uint32_t &offset, uint32_t &span)
    {
        span = (bytes + kMeshAlign - 1u) & ~(kMeshAlign - 1u);
        for (uint32_t c = 0; c < meshChunks.size() && !meshBufferEach; ++c)
        {
            auto &fl = meshChunks[c].free;
            for (size_t k = 0; k < fl.size(); ++k)
                if (fl[k].second >= span)
                {
                    chunk = c;
                    offset = fl[k].first;
                    fl[k].first += span;
                    fl[k].second -= span;
                    if (fl[k].second == 0u)
                        fl.erase(fl.begin() + static_cast<std::ptrdiff_t>(k));
                    return true;
                }
        }
        MeshChunk mc;
        mc.size = meshBufferEach ? span : std::max(kMeshChunkBytes, span);
        SDL_GPUBufferCreateInfo bi{};
        bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bi.size = mc.size;
        {
            ps2x::ftrace::Scope traceCreate(ps2x::ftrace::kBufferCreate, ps2x::ftrace::kBufferCreate + 100, 2);
            mc.buf = SDL_CreateGPUBuffer(dev, &bi);
        }
        if (!mc.buf)
            return false;
        if (mc.size > span)
            mc.free.push_back({span, mc.size - span});
        chunk = static_cast<uint32_t>(meshChunks.size());
        offset = 0;
        meshChunks.push_back(std::move(mc));
        return true;
    }

    void freeMeshRange(const MeshRange &r)
    {
        if (r.chunk >= meshChunks.size())
            return;
        if (meshBufferEach)
        {
            // Its own buffer: released (the submit that made the range free has gone).
            if (meshChunks[r.chunk].buf)
                SDL_ReleaseGPUBuffer(dev, meshChunks[r.chunk].buf);
            meshChunks[r.chunk].buf = nullptr;
            return;
        }
        auto &fl = meshChunks[r.chunk].free;
        auto it = std::lower_bound(fl.begin(), fl.end(), std::make_pair(r.offset, 0u));
        it = fl.insert(it, {r.offset, r.size});
        // Coalesce with the next and the previous.
        if (it + 1 != fl.end() && it->first + it->second == (it + 1)->first)
        {
            it->second += (it + 1)->second;
            fl.erase(it + 1);
        }
        if (it != fl.begin() && (it - 1)->first + (it - 1)->second == it->first)
        {
            (it - 1)->second += it->second;
            fl.erase(it);
        }
    }

    // The GPU copy of a native mesh (its chunk's buffer and its offset),
    // uploaded with the next copy pass (null while this frame's upload
    // budget is spent: drawn as primitives).
    SDL_GPUBuffer *nativeMeshBuffer(const std::shared_ptr<const GSNativeMesh> &mesh, uint32_t &offset)
    {
        auto it = nativeMeshes.find(mesh->id);
        if (it != nativeMeshes.end() && it->second.buf)
        {
            offset = it->second.offset;
            return it->second.buf;
        }
        const size_t bytes = nativeMeshBytes(*mesh);
        if (bytes == 0u || bytes > 0xFFFFFFFFu - kMeshAlign ||
            (nativeUploadedThisFrame != 0u && nativeUploadedThisFrame + bytes > kNativeUploadPerFrame))
            return nullptr;
        uint32_t chunk = 0, span = 0;
        if (!allocMeshRange(static_cast<uint32_t>(bytes), chunk, offset, span))
            return nullptr;
        SDL_GPUBuffer *buf = meshChunks[chunk].buf;
        NativeMeshGpu &g = nativeMeshes[mesh->id];
        g.mesh = mesh;
        g.buf = buf;
        g.chunk = chunk;
        g.offset = offset;
        g.span = span;
        g.bytes = bytes;
        nativeGpuBytes += bytes;
        nativeUploadedThisFrame += bytes;
        bufferUploads.push_back({buf, offset, mesh});
        return buf;
    }

    void sweepNativeMeshes()
    {
        for (auto it = nativeMeshes.begin(); it != nativeMeshes.end();)
        {
            if (!it->second.mesh.expired())
            {
                ++it;
                continue;
            }
            freeMeshRangesAfterSubmit.push_back({it->second.chunk, it->second.offset, it->second.span});
            nativeGpuBytes -= std::min(nativeGpuBytes, it->second.bytes);
            it = nativeMeshes.erase(it);
        }
    }

    // A native draw (GS::drawNative): the mesh's box on screen, the GS state
    // as for a primitive there, the mesh's vertex buffer and transform.
    bool submitNative(const GSNativeDraw &d, const GSDrawState &state)
    {
        const auto &ctx = state.context;
        traceReason = "native";
        if (!isColorPsm(ctx.frame.psm) || (state.prim.tme && state.prim.fst))
            return false;
        if (((ctx.test >> 17) & 3u) == 0u)
            return true; // Z test NEVER
        if (hwDepth16(ctx.zbuf.psm))
            return false; // (its depth is the float one: GS-path primitives instead)
        uint32_t meshOffset = 0;
        SDL_GPUBuffer *buf = nativeMeshBuffer(d.mesh, meshOffset);
        if (!buf)
            return false;
        // Whole pixels, as for GS-path primitives (the field offset of half
        // a line some frames carry is left out there too).
        const float ofx = static_cast<float>(ctx.xyoffset.ofx >> 4);
        const float ofy = static_cast<float>(ctx.xyoffset.ofy >> 4);
        // Where it can draw: its box's corners on screen, if all in front.
        Rect box{ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1};
        const bool skinned = d.skin && d.mesh->skin.size() == d.mesh->vertices.size();
        if (!skinned) // (a skinned mesh's bones move it: the scissor)
        {
            const GSNativeMesh &m = *d.mesh;
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            bool front = true;
            for (int c = 0; c < 8 && front; ++c)
            {
                const float p[3] = {(c & 1) ? m.boundsMax[0] : m.boundsMin[0], (c & 2) ? m.boundsMax[1] : m.boundsMin[1],
                                    (c & 4) ? m.boundsMax[2] : m.boundsMin[2]};
                float h[4];
                for (int k = 0; k < 4; ++k)
                    h[k] = p[0] * d.transform[0][k] + p[1] * d.transform[1][k] + p[2] * d.transform[2][k] + d.transform[3][k];
                if (!(h[3] > 1.0e-3f))
                {
                    front = false;
                    break;
                }
                const float x = h[0] / h[3] - ofx, y = h[1] / h[3] - ofy;
                lo[0] = std::min(lo[0], x);
                lo[1] = std::min(lo[1], y);
                hi[0] = std::max(hi[0], x);
                hi[1] = std::max(hi[1], y);
            }
            if (front)
            {
                box.x0 = std::max(box.x0, static_cast<int>(std::floor(std::max(lo[0], -65536.0f))));
                box.y0 = std::max(box.y0, static_cast<int>(std::floor(std::max(lo[1], -65536.0f))));
                box.x1 = std::min(box.x1, static_cast<int>(std::ceil(std::min(hi[0], 65536.0f))));
                box.y1 = std::min(box.y1, static_cast<int>(std::ceil(std::min(hi[1], 65536.0f))));
                if (box.empty())
                    return true; // off screen
            }
        }
        GSPrimitiveBatch batch{};
        batch.state = state;
        batch.vertexCount = 0;
        const double zScale = std::ldexp(1.0, static_cast<int>(depthBits(ctx.zbuf.psm)));
        // Picture quality (gs_quality.h): colours in perspective on every
        // native mesh, and skinned textures too (the PS2 maps them affinely).
        const bool perspective = ps2x::gs_quality::perspectiveCorrect();
        const bool perspectiveColour = d.perspectiveColour || perspective;
        const bool affineUV = d.affineUV && !perspective;
        const uint32_t motion = holdMotion(d, state);
        const bool drawn = drawPrepared(batch, box, 0u, false, [&](const DrawCall &dc)
                                        {
                                            DrawCall n = dc;
                                            n.motion = motion;
                                            n.nativeBuf = buf;
                                            n.nativeOffset = meshOffset;
                                            n.first = d.first;
                                            n.count = d.count;
                                            std::memcpy(n.nu.m, d.transform, sizeof(n.nu.m));
                                            n.nu.view[0] = ofx;
                                            n.nu.view[1] = ofy;
                                            n.nu.view[2] = 2.0f / static_cast<float>(pass.color->width);
                                            n.nu.view[3] = 2.0f / static_cast<float>(pass.color->height);
                                            n.nu.misc[0] = static_cast<float>(1.0 / zScale);
                                            n.nu.misc[1] = static_cast<float>(state.textureWidth);
                                            n.nu.misc[2] = static_cast<float>(state.textureHeight);
                                            n.nu.misc[3] = static_cast<float>(d.fog);
                                            n.nu.opts[0] = perspectiveColour ? 1.0f : 0.0f;
                                            n.nu.opts[1] = static_cast<float>(d.alpha);
                                            n.nu.opts[2] = d.normalOffset / 32768.0f;
                                            n.nu.opts[3] = !d.uvTransform ? 0.0f : d.uvFromNormal ? 2.0f : 1.0f;
                                            for (int r = 0; r < 4; ++r)
                                            {
                                                n.nu.uvm[r >> 1][(r & 1) * 2] = d.uv[r][0];
                                                n.nu.uvm[r >> 1][(r & 1) * 2 + 1] = d.uv[r][1];
                                            }
                                            std::memcpy(n.nu.clip, d.clipTransform, sizeof(n.nu.clip));
                                            n.nu.ext[0] = static_cast<float>(d.dropFlags);
                                            n.nu.ext[1] = d.clampXY ? 1.0f : 0.0f;
                                            n.nu.ext[2] = affineUV ? 1.0f : 0.0f;
                                            if (d.lights && !skinned)
                                            {
                                                std::memcpy(n.nu.lightDir, d.lights->lightDir, sizeof(n.nu.lightDir));
                                                std::memcpy(n.nu.lightColour, d.lights->lightColour, sizeof(n.nu.lightColour));
                                                n.nu.light[0] = 1.0f;
                                                n.nu.light[1] = d.lights->scale;
                                                n.nu.light[2] = static_cast<float>(d.lights->alpha);
                                            }
                                            if (skinned)
                                            {
                                                std::memcpy(n.nu.lightDir, d.skin->lightDir, sizeof(n.nu.lightDir));
                                                std::memcpy(n.nu.lightColour, d.skin->lightColour, sizeof(n.nu.lightColour));
                                                n.skinOffset = static_cast<uint32_t>(d.mesh->vertices.size() * sizeof(GSNativeVertex));
                                                n.skin = static_cast<uint32_t>(skinUniforms.size());
                                                GpuSkinUniforms &su = skinUniforms.emplace_back();
                                                su.skin[0] = d.skin->lit ? 1.0f : 0.0f;
                                                su.skin[1] = d.skin->maxColour;
                                                su.skin[2] = d.skin->alphaFromLights ? 1.0f : 0.0f;
                                                su.skin[3] = 0.0f;
                                                std::memcpy(su.palette, d.skin->palette, sizeof(su.palette));
                                            }
                                            if (perspectiveColour)
                                                n.fu.cfg0[0] |= 32u;
                                            if (nativeTint)
                                                n.fu.cfg0[0] |= 16u;
                                            pass.draws.push_back(n); },
                                        skinned ? 2 : 1);
        if (drawn)
        {
            ++stats.nativeDraws;
            stats.nativeVertices += d.count;
        }
        else
            ++stats.nativeDeclined;
        return drawn;
    }

    // ---- picture quality: fog planes, anti-aliasing ----------------------

    // A draw changed `box` (GS pixels) of target t: newer than local memory
    // there (as drawPrepared's bookkeeping).
    // Bookkeeping after a draw into t over r: these pages are now newer on
    // the GPU. True when the dirty rectangle grew (other targets over its
    // pages may have been marked stale).
    bool noteDrawn(Target *t, Rect r)
    {
        t->contentSerial = ++contentClock;
        r.x1 = std::min<int>(r.x1, static_cast<int>(t->width) - 1);
        r.y1 = std::min<int>(r.y1, static_cast<int>(t->height) - 1);
        if (r.empty())
            return false;
        // dirtyPages are the pages of the whole dirty rectangle, so a draw
        // inside it changes nothing.
        const Rect &d = t->dirty;
        if (!d.empty() && r.x0 >= d.x0 && r.y0 >= d.y0 && r.x1 <= d.x1 && r.y1 <= d.y1)
            return false;
        t->dirty.merge(r);
        GSMem::PageMask p;
        GSMem::AddRectPages(p, t->psm, t->bp, t->bw, static_cast<uint32_t>(t->dirty.x0), static_cast<uint32_t>(t->dirty.y0),
                            static_cast<uint32_t>(t->dirty.x1), static_cast<uint32_t>(t->dirty.y1));
        t->dirtyPages = p;
        // Other targets over these pages reload before their next use
        // (textures decoded from them are checked against the dirty
        // pages, see textureTarget() and syncToCpu()).
        markTargetsStale(p, t);
        return true;
    }

    // Held plane triangles are drawn before anything else looks at the
    // targets (a set is contiguous in the stream; this is only a guard).
    void settleFog()
    {
        if (!fogHeld.empty())
            releaseFogPlanes();
    }

    void noteFogPlanes(const GSFogPlanes &planes)
    {
        if (planes.count < 1 || planes.count > 8 || !(planes.step > 0.0f))
            return;
        if (fogNotes.size() >= 8u) // never matched (another renderer drew them)
            fogNotes.pop_front();
        fogNotes.push_back(planes);
    }

    // A triangle of a game's fog planes: flat (one colour, one Z),
    // untextured, blended, over (nearly) the whole scissor.
    static bool fogPlaneTriangle(const GSPrimitiveBatch &b)
    {
        const GSDrawState &st = b.state;
        const GSPrimType type = st.prim.type;
        if (b.vertexCount != 3u || st.prim.tme || !st.prim.abe ||
            (type != GS_PRIM_TRIANGLE && type != GS_PRIM_TRISTRIP && type != GS_PRIM_TRIFAN))
            return false;
        const GSVertex &v0 = b.vertices[0];
        float lo[2] = {v0.x, v0.y}, hi[2] = {v0.x, v0.y};
        for (int i = 1; i < 3; ++i)
        {
            const GSVertex &v = b.vertices[i];
            if (v.r != v0.r || v.g != v0.g || v.b != v0.b || v.a != v0.a || v.z != v0.z)
                return false;
            lo[0] = std::min(lo[0], v.x);
            lo[1] = std::min(lo[1], v.y);
            hi[0] = std::max(hi[0], v.x);
            hi[1] = std::max(hi[1], v.y);
        }
        const auto &sc = st.context.scissor;
        const float sw = static_cast<float>(static_cast<int>(sc.x1) - static_cast<int>(sc.x0) + 1);
        const float sh = static_cast<float>(static_cast<int>(sc.y1) - static_cast<int>(sc.y0) + 1);
        return sw > 0.0f && sh > 0.0f && hi[0] - lo[0] >= 0.75f * sw && hi[1] - lo[1] >= 0.75f * sh;
    }

    // Holds b if it continues the set of fog plane triangles coming in.
    bool holdFogPlane(const GSPrimitiveBatch &b)
    {
        if (!fogPlaneTriangle(b))
            return false;
        if (!fogHeld.empty())
        {
            const GSPrimitiveBatch &f = fogHeld.front();
            const auto &a = f.state.context, &c = b.state.context;
            const GSVertex &v = f.vertices[0], &w = b.vertices[0];
            if (v.r != w.r || v.g != w.g || v.b != w.b || v.a != w.a || a.frame.fbp != c.frame.fbp ||
                a.zbuf.zbp != c.zbuf.zbp || a.alpha != c.alpha || a.test != c.test || a.scissor.x0 != c.scissor.x0 ||
                a.scissor.y0 != c.scissor.y0 || a.scissor.x1 != c.scissor.x1 || a.scissor.y1 != c.scissor.y1)
                return false;
        }
        fogHeld.push_back(b);
        if (fogHeld.size() >= 2u * static_cast<size_t>(fogNotes.front().count))
            finishFogPlanes();
        return true;
    }

    // Draws the held triangles as the game sent them.
    void releaseFogPlanes()
    {
        std::vector<GSPrimitiveBatch> held;
        held.swap(fogHeld);
        for (const GSPrimitiveBatch &b : held)
            submitPrimitiveNow(b);
    }

    // A whole set: the note it belongs to, then smooth fog in place of the
    // planes (gs_quality Fog) and the view's anti-aliasing (AntiAliasing),
    // at the planes' place in the stream: after the view's 3D, before the
    // HUD and anything else of the 2D packet.
    void finishFogPlanes()
    {
        std::vector<GSPrimitiveBatch> held;
        held.swap(fogHeld);
        std::vector<double> zs;
        for (const GSPrimitiveBatch &b : held)
            zs.push_back(b.vertices[0].z);
        std::sort(zs.begin(), zs.end(), std::greater<double>());
        zs.erase(std::unique(zs.begin(), zs.end()), zs.end());
        // Its note (the oldest; notes of planes some other renderer drew are
        // dropped on the way), and the planes' own depth mapping: Z = A / w
        // + B fitted to their Z values at their distances (the draw
        // distance setting changes the Z mapping of a view after the
        // projection the note read), so the fog follows exactly what the
        // planes test against.
        bool found = false;
        GSFogPlanes note;
        while (!fogNotes.empty())
        {
            note = fogNotes.front();
            fogNotes.pop_front();
            if (static_cast<int>(zs.size()) != note.count || zs.size() < 2u)
                continue;
            double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
            const double n = static_cast<double>(zs.size());
            for (size_t k = 0; k < zs.size(); ++k)
            {
                const double x = 1.0 / (static_cast<double>(note.nearest) + static_cast<double>(note.step) * k);
                sx += x;
                sy += zs[k];
                sxx += x * x;
                sxy += x * zs[k];
            }
            const double det = n * sxx - sx * sx;
            if (!(std::abs(det) > 0.0))
                continue;
            const double a = (n * sxy - sx * sy) / det;
            const double b = (sy - a * sx) / n;
            double worst = 0.0;
            for (size_t k = 0; k < zs.size(); ++k)
                worst = std::max(worst, std::abs(a / (note.nearest + note.step * k) + b - zs[k]));
            // Integer Z values of evenly spaced planes: within a step or two.
            if (a > 0.0 && worst <= 2.5)
            {
                note.zA = static_cast<float>(a);
                note.zB = static_cast<float>(b);
                found = true;
                break;
            }
            if (!fogMismatchLogged)
            {
                fogMismatchLogged = true;
                std::cerr << "[gs-gpu] fog planes: Z " << zs.front() << " .. " << zs.back() << " do not fit planes from "
                          << note.nearest << " every " << note.step << " (left as planes)" << std::endl;
            }
        }
        if (!found || !(ps2x::gs_quality::smoothFog() && fogPass(held, note, zs)))
            for (const GSPrimitiveBatch &b : held)
                submitPrimitiveNow(b);
        if (ps2x::gs_quality::fxaa())
            fxaaPass(held.front());
    }

    // The planes' rectangle on their target (GS pixels), inside the scissor.
    static Rect fogPlaneBox(const std::vector<GSPrimitiveBatch> &held)
    {
        const auto &ctx = held.front().state.context;
        const float ofx = static_cast<float>(ctx.xyoffset.ofx >> 4);
        const float ofy = static_cast<float>(ctx.xyoffset.ofy >> 4);
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (const GSPrimitiveBatch &b : held)
            for (int i = 0; i < 3; ++i)
            {
                lo[0] = std::min(lo[0], b.vertices[i].x - ofx);
                lo[1] = std::min(lo[1], b.vertices[i].y - ofy);
                hi[0] = std::max(hi[0], b.vertices[i].x - ofx);
                hi[1] = std::max(hi[1], b.vertices[i].y - ofy);
            }
        Rect r{static_cast<int>(std::floor(lo[0])), static_cast<int>(std::floor(lo[1])), static_cast<int>(std::ceil(hi[0])),
               static_cast<int>(std::ceil(hi[1]))};
        r.x0 = std::max<int>(r.x0, ctx.scissor.x0);
        r.y0 = std::max<int>(r.y0, ctx.scissor.y0);
        r.x1 = std::min<int>(r.x1, ctx.scissor.x1);
        r.y1 = std::min<int>(r.y1, ctx.scissor.y1);
        return r;
    }

    // Must match FogUniforms in the shaders.
    struct GpuFogUniforms
    {
        float colour[4];
        float z[4];
        float n[4];
        float planes[8];
        uint32_t cfg[4];
    };
    static_assert(sizeof(GpuFogUniforms) == 96, "uniform layout");

    // Smooth fog over the planes' rectangle from the Z buffer, as the planes
    // would blend it (only the form the game uses: (Cs - Cd) As + Cd, Z
    // tested, no Z write). False: draw the planes.
    bool fogPass(const std::vector<GSPrimitiveBatch> &held, const GSFogPlanes &note, const std::vector<double> &zs)
    {
        if (!fogPipe || !fogAlphaPipe || held.empty() || zs.empty())
            return false;
        const GSPrimitiveBatch &b = held.front();
        const auto &ctx = b.state.context;
        const uint8_t fpsm = ctx.frame.psm, zpsm = ctx.zbuf.psm;
        const uint64_t test = ctx.test;
        const bool ate = (test & 1u) != 0u;
        const uint32_t atst = static_cast<uint32_t>((test >> 1) & 7u);
        const uint32_t afail = static_cast<uint32_t>((test >> 12) & 3u);
        const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3u);
        const bool zte = ((test >> 16) & 1u) != 0u;
        const bool failsAll = ate && atst == 0u; // drawn by AFAIL alone
        const bool zWrite = !ctx.zbuf.zmask && !(failsAll && (afail == 1u || afail == 3u));
        const uint32_t fbmsk = ctx.frame.fbmsk;
        const GSVertex &v = b.vertices[0];
        if ((fpsm != GS_PSM_CT32 && fpsm != GS_PSM_CT24) || !isDepthPsm(zpsm) || (ctx.alpha & 0xFFu) != 0x44u || !zte ||
            ztst < 2u || zWrite || (ate && atst != 0u && atst != 1u) || (failsAll && afail == 2u) || ((test >> 14) & 1u) ||
            (fbmsk & 0x00FFFFFFu) != 0u || ((fbmsk >> 24) != 0u && (fbmsk >> 24) != 0xFFu) || v.a > 128u ||
            !b.state.prim.abe)
            return false;
        const bool writeAlpha = fpsm == GS_PSM_CT32 && (fbmsk >> 24) == 0u && !(failsAll && afail == 3u);

        const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        Rect box = fogPlaneBox(held);
        if (static_cast<uint32_t>(std::max(box.x1, 0)) >= fbw * 64u)
            box.x1 = static_cast<int>(fbw * 64u) - 1;
        if (box.empty())
            return true;
        const uint32_t needH = static_cast<uint32_t>(ctx.scissor.y1 < 1024 ? std::max<int>(box.y1, ctx.scissor.y1) : box.y1) + 1u;
        Target *color = getTarget(ctx.frame.fbp << 5, fbw, fpsm, needH);
        Target *depth = getTarget(ctx.zbuf.zbp << 5, fbw, zpsm, color->height);
        if (depth->height > color->height)
            shrinkTarget(*depth, color->height);
        else if (depth->height < color->height)
            color = getTarget(ctx.frame.fbp << 5, fbw, fpsm, depth->height);
        if (!color || !depth || !color->tex || !depth->tex || color->height != depth->height)
            return false;
        box.x1 = std::min<int>(box.x1, static_cast<int>(color->width) - 1);
        box.y1 = std::min<int>(box.y1, static_cast<int>(color->height) - 1);
        if (box.empty())
            return true;

        GpuFogUniforms u{};
        u.colour[0] = static_cast<float>(v.r) / 255.0f;
        u.colour[1] = static_cast<float>(v.g) / 255.0f;
        u.colour[2] = static_cast<float>(v.b) / 255.0f;
        u.colour[3] = 1.0f - static_cast<float>(v.a) / 128.0f;
        u.z[0] = static_cast<float>(std::ldexp(1.0, static_cast<int>(depthBits(zpsm))));
        u.z[1] = note.zA;
        u.z[2] = note.zB;
        u.z[3] = note.nearest;
        u.n[0] = note.step;
        u.n[1] = static_cast<float>(std::min<size_t>(zs.size(), 8u));
        u.n[2] = 1.0f;
        u.n[3] = static_cast<float>(v.a | ((ctx.fba & 1u) ? 0x80u : 0u)) / 255.0f;
        for (size_t k = 0; k < 8u && k < zs.size(); ++k)
            u.planes[k] = static_cast<float>(zs[k]);
        u.cfg[0] = ztst == 2u ? 1u : 0u;

        closePass();
        const SDL_Rect sr{box.x0 * static_cast<int>(scaleX), box.y0 * static_cast<int>(scaleY),
                          (box.x1 - box.x0 + 1) * static_cast<int>(scaleX), (box.y1 - box.y0 + 1) * static_cast<int>(scaleY)};
        HeldFrame *frame = holding();
        if (frame)
        {
            Step st;
            st.kind = Step::Kind::Fog;
            st.color = holdTarget(*frame, color);
            st.depth = holdTarget(*frame, depth);
            st.fog = u;
            st.scissor = sr;
            st.writeAlpha = writeAlpha;
            copyHeldSnapshots(*frame);
            frame->steps.push_back(st);
        }
        if (!recordFogDraw(color->tex, depth->tex, color->width, color->height, sr, u, writeAlpha))
        {
            notReplayable("fog pass");
            return false;
        }
        ++stats.fogPasses;
        noteDrawn(color, box);
        return true;
    }

    // The smooth fog's pass into colour from depth (w x h GS pixels), within the scissor sr (target pixels).
    bool recordFogDraw(SDL_GPUTexture *colour, SDL_GPUTexture *depth, uint32_t w, uint32_t h, const SDL_Rect &sr,
                       const GpuFogUniforms &u, bool writeAlpha)
    {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = colour;
        ct.load_op = SDL_GPU_LOADOP_LOAD;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *rp = texcheck::beginRenderPass(commandBuffer(), &ct, 1, nullptr);
        if (!rp)
            return false;
        ++stats.renderPasses;
        SDL_GPUViewport vp{0, 0, static_cast<float>(w * scaleX), static_cast<float>(h * scaleY), 0.0f, 1.0f};
        SDL_SetGPUViewport(rp, &vp);
        SDL_SetGPUScissor(rp, &sr);
        SDL_GPUTextureSamplerBinding tb{depth, sampler};
        SDL_BindGPUGraphicsPipeline(rp, fogPipe);
        SDL_PushGPUFragmentUniformData(commandBuffer(), 0, &u, sizeof(u));
        texcheck::bindFragmentSamplers(rp, 0, &tb, 1);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        if (writeAlpha)
        {
            SDL_BindGPUGraphicsPipeline(rp, fogAlphaPipe);
            SDL_PushGPUFragmentUniformData(commandBuffer(), 0, &u, sizeof(u));
            texcheck::bindFragmentSamplers(rp, 0, &tb, 1);
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        }
        SDL_EndGPURenderPass(rp);
        stats.drawCalls += writeAlpha ? 2u : 1u;
        return true;
    }

    // FXAA over a view's 3D picture (the fog planes' rectangle), on a copy
    // of it, RGB only.
    void fxaaPass(const GSPrimitiveBatch &b)
    {
        if (!fxaaPipe || !linearSampler)
            return;
        const auto &ctx = b.state.context;
        const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        Target *color = isColorPsm(ctx.frame.psm) ? findTarget(ctx.frame.fbp << 5, fbw, ctx.frame.psm) : nullptr;
        if (!color || !color->tex || color->stale)
            return;
        std::vector<GSPrimitiveBatch> one{b};
        Rect box = fogPlaneBox(one);
        // Both triangles of a plane: the box of the whole rectangle is the
        // scissor's here (the first triangle only spans half).
        box = Rect{std::max<int>(ctx.scissor.x0, 0), std::max<int>(ctx.scissor.y0, 0), ctx.scissor.x1, ctx.scissor.y1};
        box.x1 = std::min<int>(box.x1, static_cast<int>(color->width) - 1);
        box.y1 = std::min<int>(box.y1, static_cast<int>(color->height) - 1);
        if (box.empty())
            return;
        closePass();
        if (HeldFrame *held = holding())
        {
            Step st;
            st.kind = Step::Kind::Fxaa;
            st.color = holdTarget(*held, color);
            st.box = box;
            copyHeldSnapshots(*held);
            held->steps.push_back(st);
        }
        if (!recordFxaa(color->tex, copyTexture(color->width * scaleX, color->height * scaleY), color->width, color->height, box))
            return;
        ++stats.fxaaPasses;
        noteDrawn(color, box);
    }

    // FXAA over box (GS pixels) of colour (w x h GS pixels), from its copy in copyTex.
    bool recordFxaa(SDL_GPUTexture *colour, SDL_GPUTexture *copyTex, uint32_t width, uint32_t height, const Rect &box)
    {
        const uint32_t tw = width * scaleX, th = height * scaleY;
        {
            // The box and a margin (FXAA looks up to 16 pixels along an edge).
            const int m = 4;
            const int x0 = std::max(box.x0 - m, 0), y0 = std::max(box.y0 - m, 0);
            const int x1 = std::min<int>(box.x1 + m, static_cast<int>(width) - 1);
            const int y1 = std::min<int>(box.y1 + m, static_cast<int>(height) - 1);
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer());
            SDL_GPUTextureLocation src{};
            src.texture = colour;
            src.x = static_cast<uint32_t>(x0) * scaleX;
            src.y = static_cast<uint32_t>(y0) * scaleY;
            SDL_GPUTextureLocation dst = src;
            dst.texture = copyTex;
            texcheck::copyTextureToTexture(copy, &src, &dst, static_cast<uint32_t>(x1 - x0 + 1) * scaleX,
                                        static_cast<uint32_t>(y1 - y0 + 1) * scaleY, 1, false);
            SDL_EndGPUCopyPass(copy);
        }
        SDL_GPUColorTargetInfo ct{};
        ct.texture = colour;
        ct.load_op = SDL_GPU_LOADOP_LOAD;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *rp = texcheck::beginRenderPass(commandBuffer(), &ct, 1, nullptr);
        if (!rp)
            return false;
        ++stats.renderPasses;
        SDL_GPUViewport vp{0, 0, static_cast<float>(tw), static_cast<float>(th), 0.0f, 1.0f};
        SDL_SetGPUViewport(rp, &vp);
        const SDL_Rect sr{box.x0 * static_cast<int>(scaleX), box.y0 * static_cast<int>(scaleY),
                          (box.x1 - box.x0 + 1) * static_cast<int>(scaleX), (box.y1 - box.y0 + 1) * static_cast<int>(scaleY)};
        SDL_SetGPUScissor(rp, &sr);
        // x, y: 1 / size; z: subpixel smoothing (FXAA's default 0.75); w:
        // the relative contrast an edge needs (0.125: FXAA's high quality).
        const float u[4] = {1.0f / static_cast<float>(tw), 1.0f / static_cast<float>(th), 0.75f, 0.125f};
        SDL_GPUTextureSamplerBinding tb{copyTex, linearSampler};
        SDL_BindGPUGraphicsPipeline(rp, fxaaPipe);
        SDL_PushGPUFragmentUniformData(commandBuffer(), 0, u, sizeof(u));
        texcheck::bindFragmentSamplers(rp, 0, &tb, 1);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
        ++stats.drawCalls;
        return true;
    }

    void flushUploads()
    {
        SDL_GPUCopyPass *copy = nullptr;
        recordUploads(nullptr, 0, &copy);
        if (copy)
            SDL_EndGPUCopyPass(copy);
    }

    SDL_GPUTexture *copyTexture(uint32_t w, uint32_t h)
    {
        const uint64_t key = (static_cast<uint64_t>(w) << 32) | h;
        auto it = copyTextures.find(key);
        if (it != copyTextures.end())
            return it->second;
        SDL_GPUTexture *t = createTexture(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        copyTextures.emplace(key, t);
        return t;
    }

    SDL_GPUTexture *stagingTexture(uint32_t w, uint32_t h, SDL_GPUTextureFormat format)
    {
        const uint64_t key = (static_cast<uint64_t>(format) << 48) | (static_cast<uint64_t>(w) << 24) | h;
        auto it = stagingTextures.find(key);
        if (it != stagingTextures.end())
            return it->second;
        SDL_GPUTexture *t = createTexture(w, h, format, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        stagingTextures.emplace(key, t);
        return t;
    }

    void closePass()
    {
        if (!passOpen)
            return;
        passOpen = false;
        Pass &p = pass;
        if (p.draws.empty())
        {
            p.reset();
            skinUniforms.clear();
            ++passSerial;
            return;
        }
        ps2x::ftrace::Scope tracePass(ps2x::ftrace::kGpuPassBegin, ps2x::ftrace::kGpuPassEnd, static_cast<uint16_t>(std::min<size_t>(p.draws.size(), 65535u)));
        holdPassBegin(p);
        SDL_GPUCopyPass *copy = nullptr;
        const uint32_t vertexOffset = recordUploads(p.verts.data(), static_cast<uint32_t>(p.verts.size() * sizeof(GpuVertex)), &copy);
        if (!p.copyRegion.empty())
        {
            if (!copy)
                copy = SDL_BeginGPUCopyPass(commandBuffer());
            Rect r = p.copyRegion;
            r.x0 = std::max(r.x0, 0);
            r.y0 = std::max(r.y0, 0);
            r.x1 = std::min<int>(r.x1, static_cast<int>(p.color->width) - 1);
            r.y1 = std::min<int>(r.y1, static_cast<int>(p.color->height) - 1);
            if (!r.empty())
            {
                SDL_GPUTextureLocation src{};
                src.texture = p.color->tex;
                src.x = static_cast<uint32_t>(r.x0) * scaleX;
                src.y = static_cast<uint32_t>(r.y0) * scaleY;
                SDL_GPUTextureLocation dst = src;
                dst.texture = copyTexture(p.color->width * scaleX, p.color->height * scaleY);
                texcheck::copyTextureToTexture(copy, &src, &dst, static_cast<uint32_t>(r.x1 - r.x0 + 1) * scaleX,
                                            static_cast<uint32_t>(r.y1 - r.y0 + 1) * scaleY, 1, false);
            }
        }
        if (copy)
            SDL_EndGPUCopyPass(copy);

        recordPassDraws(p.color->tex, p.depth ? p.depth->tex : nullptr, p.color->width, p.color->height, p.draws,
                        skinUniforms, vertexOffset);
        holdPass(p);
        p.reset();
        skinUniforms.clear();
        ++passSerial;
    }

    // A pass's draws recorded into colour (and depth), w x h GS pixels: the pass's own vertices at vertexOffset
    // of the vertex buffer, or a native draw's mesh (closePass, and frame interpolation's replay of a pass).
    void recordPassDraws(SDL_GPUTexture *colour, SDL_GPUTexture *depth, uint32_t width, uint32_t height,
                         const std::vector<DrawCall> &draws, std::vector<GpuSkinUniforms> &skins, uint32_t vertexOffset)
    {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = colour;
        ct.load_op = SDL_GPU_LOADOP_LOAD;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo dt{};
        if (depth)
        {
            dt.texture = depth;
            dt.load_op = SDL_GPU_LOADOP_LOAD;
            dt.store_op = SDL_GPU_STOREOP_STORE;
            dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        }
        SDL_GPURenderPass *rp = texcheck::beginRenderPass(commandBuffer(), &ct, 1, depth ? &dt : nullptr);
        ++stats.renderPasses;
        SDL_GPUViewport vp{0, 0, static_cast<float>(width * scaleX), static_cast<float>(height * scaleY), 0.0f, 1.0f};
        SDL_SetGPUViewport(rp, &vp);
        const float ndc[4] = {2.0f / static_cast<float>(width), 2.0f / static_cast<float>(height), 0.0f, 0.0f};
        // The pass's own vertices, or a native draw's mesh.
        SDL_GPUBuffer *boundBuf = nullptr;
        uint32_t boundOffset = 0;
        uint64_t boundPipe = UINT64_MAX;
        // State already set for the previous draw is not set again: the
        // pass keeps its scissor, blend constants and samplers, and the
        // command buffer its uniforms (each sampler bind is a new descriptor
        // set, each uniform push a new offset, and a tiling GPU replays them
        // in every bin).
        const DrawCall *prev = nullptr;
        for (const DrawCall &d : draws)
        {
            if (d.pipe != boundPipe)
            {
                SDL_BindGPUGraphicsPipeline(rp, pipelineByPacked(d.pipe));
                boundPipe = d.pipe;
            }
            if (d.nativeBuf && d.skin != UINT32_MAX && d.skin < skins.size())
            {
                // Skinned: the mesh's vertices and bones.
                const SDL_GPUBufferBinding nb[2] = {{d.nativeBuf, d.nativeOffset}, {d.nativeBuf, d.nativeOffset + d.skinOffset}};
                SDL_BindGPUVertexBuffers(rp, 0, nb, 2);
                boundBuf = nullptr;
                GpuSkinUniforms &su = skins[d.skin];
                su.n = d.nu;
                SDL_PushGPUVertexUniformData(commandBuffer(), 0, &su, sizeof(su));
            }
            else if (d.nativeBuf)
            {
                if (boundBuf != d.nativeBuf || boundOffset != d.nativeOffset)
                {
                    SDL_GPUBufferBinding nb{d.nativeBuf, d.nativeOffset};
                    SDL_BindGPUVertexBuffers(rp, 0, &nb, 1);
                    boundBuf = d.nativeBuf;
                    boundOffset = d.nativeOffset;
                }
                SDL_PushGPUVertexUniformData(commandBuffer(), 0, &d.nu, sizeof(d.nu));
            }
            else if (boundBuf != vbuf)
            {
                SDL_GPUBufferBinding vbb{vbuf, vertexOffset};
                SDL_BindGPUVertexBuffers(rp, 0, &vbb, 1);
                SDL_PushGPUVertexUniformData(commandBuffer(), 0, ndc, sizeof(ndc));
                boundBuf = vbuf;
            }
            if (!prev || prev->scissor.x != d.scissor.x || prev->scissor.y != d.scissor.y || prev->scissor.w != d.scissor.w ||
                prev->scissor.h != d.scissor.h)
                SDL_SetGPUScissor(rp, &d.scissor);
            if (!prev || std::memcmp(prev->blend, d.blend, sizeof(d.blend)) != 0)
            {
                SDL_FColor bc{d.blend[0], d.blend[1], d.blend[2], d.blend[3]};
                SDL_SetGPUBlendConstants(rp, bc);
            }
            if (!prev || std::memcmp(prev->tex, d.tex, sizeof(d.tex)) != 0 || prev->sampler0 != d.sampler0)
            {
                SDL_GPUTextureSamplerBinding tb[4];
                for (int i = 0; i < 4; ++i)
                    tb[i] = {d.tex[i], sampler};
                if (d.sampler0)
                    tb[0].sampler = d.sampler0;
                texcheck::bindFragmentSamplers(rp, 0, tb, 4);
            }
            if (!prev || std::memcmp(&prev->fu, &d.fu, sizeof(d.fu)) != 0)
                SDL_PushGPUFragmentUniformData(commandBuffer(), 0, &d.fu, sizeof(d.fu));
            SDL_DrawGPUPrimitives(rp, d.count, 1, d.first, 0);
            ++stats.drawCalls;
            prev = &d;
        }
        SDL_EndGPURenderPass(rp);
    }

    std::unordered_map<uint64_t, PipeKey> pipeKeys;
    uint64_t lastPipeKey = UINT64_MAX; // in pipeKeys (keys are never removed)
    SDL_GPUGraphicsPipeline *pipelineByPacked(uint64_t packed)
    {
        return pipeline(pipeKeys[packed]);
    }

    std::shared_ptr<FenceRef> submit(bool wantFence)
    {
        closePass();
        if (!uploads.empty())
            flushUploads();
        if (!cmd)
            return nullptr;
        // Owed write-backs and CLUT snapshots in this command buffer wait
        // for it with a fence.
        bool unfenced = false;
        for (const OwedWriteBack &o : owed)
            unfenced |= !o.fence;
        for (const ClutSnapshot &c : clutSnapshots)
            unfenced |= !c.fence;
        for (const RetiringBuffer &r : retiringBuffers)
            unfenced |= !r.fence;
        for (const RetiringTexture &r : retiringClutTextures)
            unfenced |= !r.fence;
        std::shared_ptr<FenceRef> fence;
        ps2x::ftrace::Scope traceSubmit(ps2x::ftrace::kGpuSubmitBegin, ps2x::ftrace::kGpuSubmitEnd);
        if (wantFence || unfenced)
        {
            fence = std::make_shared<FenceRef>();
            fence->dev = dev;
            fence->fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            for (OwedWriteBack &o : owed)
                if (!o.fence)
                    o.fence = fence;
            for (ClutSnapshot &c : clutSnapshots)
                if (!c.fence)
                    c.fence = fence;
            for (RetiringBuffer &r : retiringBuffers)
                if (!r.fence)
                    r.fence = fence;
            for (RetiringTexture &r : retiringClutTextures)
                if (!r.fence)
                    r.fence = fence;
        }
        else
            SDL_SubmitGPUCommandBuffer(cmd);
        cmd = nullptr;
        cmdThread = 0;
        ++stats.submits;
        vbufUsed = 0;
        vbufCycle = true;
        for (SDL_GPUTexture *t : releaseAfterSubmit)
        {
            if (interp.on && heldUses(t))
                interp.deferredRelease.push_back(t); // (a held frame draws with it)
            else
                texcheck::releaseTexture(dev, t);
        }
        releaseAfterSubmit.clear();
        for (SDL_GPUBuffer *b : releaseBuffersAfterSubmit)
            SDL_ReleaseGPUBuffer(dev, b);
        releaseBuffersAfterSubmit.clear();
        for (const MeshRange &r : freeMeshRangesAfterSubmit)
            freeMeshRange(r);
        freeMeshRangesAfterSubmit.clear();
        return fence;
    }

    void submitAndWait()
    {
        const std::shared_ptr<FenceRef> fence = submit(true);
        if (fence && fence->fence)
            waitFence(fence->fence);
    }

    // ---- local memory bookkeeping ----------------------------------------

    void markWritten(const GSMem::PageMask &pages)
    {
        ++gen;
        for (uint32_t i = 0; i < kPageCount; ++i)
            if (pages.test(i))
                pageGen[i] = gen;
        ++epoch;
    }

    // Local memory in these pages changed under other targets. A target
    // whose lower part was overwritten (often one left taller than the
    // buffer it now stands for, overlapping the next buffer) gives those
    // lines up; one overwritten from its first page row reloads before its
    // next use. Callers write unsaved drawing back first.
    void markTargetsStale(const GSMem::PageMask &pages, const Target *except = nullptr)
    {
        for (auto &t : targets)
            if (t.get() != except && t->pages.intersects(pages))
            {
                const uint32_t row = firstOverlapRow(*t, pages);
                if (row > 0u && row < t->height)
                    shrinkTarget(*t, row);
                else
                    t->stale = true;
            }
    }

    uint32_t firstOverlapRow(const Target &t, const GSMem::PageMask &pages) const
    {
        uint32_t pw, ph;
        pageExtent(t.psm, pw, ph);
        for (uint32_t y = 0; y < t.height; y += ph)
        {
            GSMem::PageMask row;
            GSMem::AddRectPages(row, t.psm, t.bp, t.bw, 0, y, t.width - 1u, std::min(y + ph, t.height) - 1u);
            if (row.intersects(pages))
                return y;
        }
        return t.height;
    }

    // Keeps the first `height` lines of a target.
    void shrinkTarget(Target &t, uint32_t height)
    {
        if (height == 0u || height >= t.height)
            return;
        if (passOpen && (pass.color == &t || pass.depth == &t))
            closePass();
        notReplayable("target resized");
        flushUploads();
        SDL_GPUTexture *nt = createTexture(t.width * scaleX, height * scaleY,
                                           t.depth ? depthFormat(t.psm) : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                           (t.depth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) |
                                               SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (!nt)
        {
            t.stale = true;
            return;
        }
        if (trace)
            std::cout << "[gs-gpu] shrink bp=" << t.bp << " bw=" << t.bw << " psm=" << int(t.psm) << " " << t.height << " -> "
                      << height << " lines (" << traceReason << ")" << std::endl;
        if (!t.stale)
        {
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer());
            SDL_GPUTextureLocation src{};
            src.texture = t.tex;
            SDL_GPUTextureLocation dst{};
            dst.texture = nt;
            texcheck::copyTextureToTexture(copy, &src, &dst, t.width * scaleX, height * scaleY, 1, false);
            SDL_EndGPUCopyPass(copy);
        }
        releaseAfterSubmit.push_back(t.tex);
        t.tex = nt;
        t.height = height;
        t.contentSerial = ++contentClock;
        t.pages.clear();
        GSMem::AddRectPages(t.pages, t.psm, t.bp, t.bw, 0, 0, t.width - 1u, height - 1u);
        t.dirty.y1 = std::min<int>(t.dirty.y1, static_cast<int>(height) - 1);
        t.dirtyPages.clear();
        if (!t.dirty.empty())
            GSMem::AddRectPages(t.dirtyPages, t.psm, t.bp, t.bw, static_cast<uint32_t>(t.dirty.x0), static_cast<uint32_t>(t.dirty.y0),
                                static_cast<uint32_t>(t.dirty.x1), static_cast<uint32_t>(t.dirty.y1));
    }

    // Writes back every render target drawn in these pages, so local memory
    // is current there. Deferred: the write-backs are recorded and their
    // bookkeeping done, but the pixels reach local memory later (see
    // OwedWriteBack); only for callers that read those pages on the GPU.
    void syncToCpu(const GSMem::PageMask &pages, bool deferred = false)
    {
        deferred = deferred && deferWriteBacks;
        std::vector<Target *> list;
        for (auto &t : targets)
            if (!t->dirty.empty() && t->dirtyPages.intersects(pages))
                list.push_back(t.get());
        if (!deferred)
            materialize(pages);
        if (list.empty())
            return;
        for (Target *t : list)
            download(*t, deferred);
    }

    void syncAll()
    {
        GSMem::PageMask all;
        for (auto &w : all.bits)
            w = ~0ull;
        syncToCpu(all);
    }

    // ---- deferred write-backs ----------------------------------------------
    //
    // A write-back waits for the GPU to finish everything recorded so far.
    // On a lightly loaded GPU at low clocks (Direct3D 12 with vsync) that is
    // most of a frame, and some games read a frame buffer back several times
    // a frame (Aggressive Inline's split screen at draw distance far: CLUT
    // loads and 8-bit textures over the frame being drawn). A deferred
    // write-back records the download and does the bookkeeping at once
    // (dirty cleared, pages marked written, other targets stale, texture
    // flush) exactly as an immediate one, but local memory gets the pixels
    // only when the GPU is done (materialize), so those pages are owed until
    // then: everything that reads or writes local memory there materializes
    // first, and sees what an immediate write-back would have left. The
    // readers that want no wait (CLUT loads, indexed textures) take the
    // pixels from the target itself on the GPU instead (gather passes).
    struct OwedWriteBack
    {
        uint32_t bp = 0, bw = 1;
        uint8_t psm = 0;
        bool depth = false;
        Rect r;         // GS pixels
        uint32_t w = 0; // download row length (texels)
        GSMem::PageMask pages;
        SDL_GPUTransferBuffer *tb = nullptr;
        uint32_t tbSize = 0;
        std::shared_ptr<FenceRef> fence; // set by the submit that carries it
    };
    std::deque<OwedWriteBack> owed;
    GSMem::PageMask owedPages;
    std::vector<std::pair<SDL_GPUTransferBuffer *, uint32_t>> spareDownloadBuffers;
    // Download buffers given up while the GPU may still write them: spare
    // again once their submit is done.
    struct RetiringBuffer
    {
        SDL_GPUTransferBuffer *tb = nullptr;
        uint32_t size = 0;
        std::shared_ptr<FenceRef> fence; // set by the submit that carries it
    };
    std::vector<RetiringBuffer> retiringBuffers;
    bool materializing = false;

    SDL_GPUTransferBuffer *takeDownloadBuffer(uint32_t size, uint32_t &outSize)
    {
        for (size_t i = 0; i < retiringBuffers.size();)
        {
            const RetiringBuffer &r = retiringBuffers[i];
            if (r.fence && (!r.fence->fence || SDL_QueryGPUFence(dev, r.fence->fence)))
            {
                returnDownloadBuffer(r.tb, r.size);
                retiringBuffers.erase(retiringBuffers.begin() + static_cast<std::ptrdiff_t>(i));
            }
            else
                ++i;
        }
        for (size_t i = 0; i < spareDownloadBuffers.size(); ++i)
            if (spareDownloadBuffers[i].second >= size)
            {
                SDL_GPUTransferBuffer *tb = spareDownloadBuffers[i].first;
                outSize = spareDownloadBuffers[i].second;
                spareDownloadBuffers.erase(spareDownloadBuffers.begin() + static_cast<std::ptrdiff_t>(i));
                return tb;
            }
        SDL_GPUTransferBufferCreateInfo ti{};
        ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        ti.size = std::max<uint32_t>(size, 1u << 20);
        outSize = ti.size;
        return SDL_CreateGPUTransferBuffer(dev, &ti);
    }

    void returnDownloadBuffer(SDL_GPUTransferBuffer *tb, uint32_t size)
    {
        if (!tb)
            return;
        if (spareDownloadBuffers.size() >= 8u)
        {
            SDL_ReleaseGPUTransferBuffer(dev, tb);
            return;
        }
        spareDownloadBuffers.emplace_back(tb, size);
    }

    void waitFence(SDL_GPUFence *fence)
    {
        ps2x::ftrace::Scope traceFence(ps2x::ftrace::kUser2, ps2x::ftrace::kUser2 + 100);
        SDL_WaitForGPUFences(dev, true, &fence, 1);
    }

    // Stores downloaded pixels (map: rows of w texels from r's corner, at
    // the internal resolution) into local memory: the centre sample of each
    // GS pixel, in the buffer's format.
    void storeDownloaded(const uint8_t *map, uint32_t w, const Rect &r, uint32_t bp, uint32_t bw, uint8_t psm, bool depth)
    {
        const WriteFunc write = writeFunc(psm);
        const uint32_t halfX = scaleX / 2u, halfY = scaleY / 2u;
        const uint32_t bits = depth ? depthBits(psm) : 0u;
        const double zScale = depth ? depthScale(psm) : 1.0;
        const double zMax = std::ldexp(1.0, static_cast<int>(bits)) - 1.0;
        for (int y = r.y0; y <= r.y1; ++y)
        {
            const uint32_t sy = static_cast<uint32_t>(y - r.y0) * scaleY + halfY;
            for (int x = r.x0; x <= r.x1; ++x)
            {
                const uint32_t sx = static_cast<uint32_t>(x - r.x0) * scaleX + halfX;
                const uint8_t *p = map + (static_cast<size_t>(sy) * w + sx) * 4u;
                uint32_t value;
                if (depth)
                {
                    float d;
                    std::memcpy(&d, p, 4);
                    const double z = std::floor(static_cast<double>(d) * zScale + 0.5);
                    value = static_cast<uint32_t>(std::clamp(z, 0.0, zMax));
                }
                else
                {
                    value = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                            (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
                    if (is16BitPsm(psm))
                        value = Rgba8888ToRgba5551(value);
                }
                write(vram, bp, bw, static_cast<uint32_t>(x), static_cast<uint32_t>(y), value);
            }
        }
    }

    // The oldest owed write-back: wait for it and store its pixels.
    void materializeFront()
    {
        ps2x::ftrace::Scope traceMaterialize(ps2x::ftrace::kUser7, ps2x::ftrace::kUser7 + 100);
        OwedWriteBack &o = owed.front();
        materializing = true;
        if (!o.fence)
            submit(false); // gives it a fence
        if (o.fence && o.fence->fence && !SDL_QueryGPUFence(dev, o.fence->fence))
        {
            ++stats.owedWaits;
            waitFence(o.fence->fence);
        }
        if (const uint8_t *map = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(dev, o.tb, false)))
        {
            storeDownloaded(map, o.w, o.r, o.bp, o.bw, o.psm, o.depth);
            SDL_UnmapGPUTransferBuffer(dev, o.tb);
        }
        returnDownloadBuffer(o.tb, o.tbSize);
        owed.pop_front();
        owedPages.clear();
        for (const OwedWriteBack &w : owed)
            owedPages.merge(w.pages);
        materializing = false;
    }

    // Local memory in these pages gets every owed pixel (in order).
    void materialize(const GSMem::PageMask &pages)
    {
        if (owed.empty() || !owedPages.intersects(pages))
            return;
        size_t last = 0;
        for (size_t i = 0; i < owed.size(); ++i)
            if (owed[i].pages.intersects(pages))
                last = i + 1;
        for (size_t i = 0; i < last; ++i)
            materializeFront();
    }

    void materializeAll()
    {
        while (!owed.empty())
            materializeFront();
    }

    // After a submit: owed write-backs the GPU has finished cost nothing to
    // store now.
    void retireOwed()
    {
        if (materializing)
            return;
        while (!owed.empty() && owed.front().fence && owed.front().fence->fence &&
               SDL_QueryGPUFence(dev, owed.front().fence->fence))
            materializeFront();
    }

    // Reads the drawn part of a target back into local memory (deferred:
    // see above).
    void download(Target &t, bool deferred = false)
    {
        if (t.dirty.empty())
            return;
        ps2x::ftrace::Scope traceDownload(ps2x::ftrace::kTargetDownload, ps2x::ftrace::kTargetDownload + 100);
        Rect r = t.dirty;
        r.x0 = std::max(r.x0, 0);
        r.y0 = std::max(r.y0, 0);
        r.x1 = std::min<int>(r.x1, static_cast<int>(t.width) - 1);
        r.y1 = std::min<int>(r.y1, static_cast<int>(t.height) - 1);
        if (r.empty())
        {
            t.dirty = Rect{};
            t.dirtyPages.clear();
            return;
        }
        if (trace)
            std::cout << "[gs-gpu] write back" << (deferred ? " (deferred)" : "") << " bp=" << t.bp << " bw=" << t.bw
                      << " psm=" << int(t.psm) << " rect=" << r.x0 << "," << r.y0 << "-" << r.x1 << "," << r.y1 << " ("
                      << traceReason << ")" << std::endl;
        GSMem::PageMask written;
        GSMem::AddRectPages(written, t.psm, t.bp, t.bw, static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0),
                            static_cast<uint32_t>(r.x1), static_cast<uint32_t>(r.y1));
        // Older owed pixels go to memory before these newer ones.
        if (!deferred)
            materialize(written);
        closePass();
        const uint32_t w = static_cast<uint32_t>(r.x1 - r.x0 + 1) * scaleX;
        const uint32_t h = static_cast<uint32_t>(r.y1 - r.y0 + 1) * scaleY;
        const uint32_t bytes = w * h * 4u;
        SDL_GPUTransferBuffer *tb = nullptr;
        uint32_t tbSize = 0;
        if (deferred)
            tb = takeDownloadBuffer(bytes, tbSize);
        else
        {
            ensureDownloadBuffer(bytes);
            tb = downTb;
        }

        SDL_GPUTexture *src = t.tex;
        SDL_GPUTexture *temp = nullptr;
        if (t.depth)
        {
            // Depth to R32F first.
            temp = createTexture(t.width * scaleX, t.height * scaleY, SDL_GPU_TEXTUREFORMAT_R32_FLOAT,
                                 SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
            flushUploads();
            SDL_GPUColorTargetInfo ct{};
            ct.texture = temp;
            ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
            ct.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPURenderPass *rp = texcheck::beginRenderPass(commandBuffer(), &ct, 1, nullptr);
            SDL_BindGPUGraphicsPipeline(rp, depthStorePipe);
            SDL_GPUTextureSamplerBinding tb2{t.tex, sampler};
            texcheck::bindFragmentSamplers(rp, 0, &tb2, 1);
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
            SDL_EndGPURenderPass(rp);
            src = temp;
        }
        flushUploads();
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer());
        SDL_GPUTextureRegion region{};
        region.texture = src;
        region.x = static_cast<uint32_t>(r.x0) * scaleX;
        region.y = static_cast<uint32_t>(r.y0) * scaleY;
        region.w = w;
        region.h = h;
        region.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = tb;
        dst.pixels_per_row = w;
        dst.rows_per_layer = h;
        texcheck::downloadFromTexture(copy, &region, &dst);
        SDL_EndGPUCopyPass(copy);
        if (deferred)
        {
            if (temp)
                releaseAfterSubmit.push_back(temp);
            // Older owed write-backs of this target inside this one's
            // rectangle never need storing: these pixels replace all of
            // theirs.
            for (auto it = owed.begin(); it != owed.end();)
            {
                if (it->bp == t.bp && it->bw == t.bw && it->psm == t.psm && it->depth == t.depth && it->r.x0 >= r.x0 &&
                    it->r.y0 >= r.y0 && it->r.x1 <= r.x1 && it->r.y1 <= r.y1)
                {
                    retiringBuffers.push_back({it->tb, it->tbSize, it->fence});
                    it = owed.erase(it);
                    ++stats.supersededDownloads;
                }
                else
                    ++it;
            }
            OwedWriteBack o;
            o.bp = t.bp;
            o.bw = t.bw;
            o.psm = t.psm;
            o.depth = t.depth;
            o.r = r;
            o.w = w;
            o.pages = written;
            o.tb = tb;
            o.tbSize = tbSize;
            owed.push_back(o);
            owedPages.clear();
            for (const OwedWriteBack &w : owed)
                owedPages.merge(w.pages);
            ++stats.deferredDownloads;
            retireOwed();
            // Nothing read these pages for long: store the oldest.
            while (owed.size() > 32u)
                materializeFront();
        }
        else
        {
            submitAndWait();
            if (temp)
                texcheck::releaseTexture(dev, temp);
            const uint8_t *map = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(dev, downTb, false));
            storeDownloaded(map, w, r, t.bp, t.bw, t.psm, t.depth);
            SDL_UnmapGPUTransferBuffer(dev, downTb);
        }
        ++stats.targetDownloads;

        t.dirty = Rect{};
        t.dirtyPages.clear();
        markWritten(written);
        // Other targets over these pages now have to reload.
        markTargetsStale(written, &t);
        cpu.TextureFlush();
    }

    // ---- local memory read from a target on the GPU -------------------------
    //
    // Right after a (deferred) write-back of the pages it covers, local memory
    // there holds exactly what a non-stale colour target that is not dirty
    // holds (a write-back stores its centre samples). So a CLUT load or an
    // 8-bit texture over such a target can take its bytes from the target
    // on the GPU (gather passes) instead of waiting for the owed pixels.
    // Only PSMCT32 targets: a CT24 write-back keeps the byte above the colour
    // in memory.

    // Word offset inside a PSMCT32 page -> pixel of the page.
    std::array<uint8_t, 2048> ct32PageX{}, ct32PageY{};
    bool ct32PageTableReady = false;

    // The pixel of colour target t that holds byte address a, if t's area
    // has it.
    bool targetPixelOf(const Target &t, uint32_t a, uint32_t &x, uint32_t &y)
    {
        if (!ct32PageTableReady)
        {
            for (uint32_t py = 0; py < 32u; ++py)
                for (uint32_t px = 0; px < 64u; ++px)
                {
                    const uint32_t off = (GSMem::ByteAddress(GS_PSM_CT32, 0u, 1u, px, py) / 4u) & 2047u;
                    ct32PageX[off] = static_cast<uint8_t>(px);
                    ct32PageY[off] = static_cast<uint8_t>(py);
                }
            ct32PageTableReady = true;
        }
        if (a == UINT32_MAX || t.bp % 32u != 0u)
            return false;
        const uint32_t page = a / kPageBytes, first = t.bp / 32u;
        if (page < first)
            return false;
        const uint32_t rel = page - first;
        const uint32_t off = (a % kPageBytes) / 4u;
        x = (rel % t.bw) * 64u + ct32PageX[off];
        y = (rel / t.bw) * 32u + ct32PageY[off];
        if (x >= t.width || y >= t.height)
            return false;
        // The inverse of the address the GS would use (also catches wraps).
        return GSMem::ByteAddress(GS_PSM_CT32, t.bp, t.bw, x, y) == (a & ~3u);
    }

    // The target owed pages are to be read from: the one non-stale PSMCT32
    // target every owed write-back over these pages came from, not dirty in
    // them.
    Target *owedSource(const GSMem::PageMask &pages)
    {
        Target *found = nullptr;
        for (const OwedWriteBack &o : owed)
        {
            if (!o.pages.intersects(pages))
                continue;
            if (o.psm != GS_PSM_CT32 || o.depth)
                return nullptr;
            Target *t = findTarget(o.bp, o.bw, o.psm);
            if (!t || (found && found != t))
                return nullptr;
            found = t;
        }
        if (!found || found->stale || found->depth || found->psm != GS_PSM_CT32 || !found->tex)
            return nullptr;
        if (!found->dirty.empty() && found->dirtyPages.intersects(pages))
            return nullptr;
        return found;
    }

    // Coordinate textures (RGBA16_UINT: x, y, byte, valid) by layout.
    std::map<std::array<uint32_t, 10>, SDL_GPUTexture *> gatherCoordTextures;

    SDL_GPUTexture *coordTexture(const std::array<uint32_t, 10> &key, uint32_t w, uint32_t h,
                                 const std::vector<uint16_t> &coords)
    {
        SDL_GPUTexture *tex = createTexture(w, h, SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UINT, SDL_GPU_TEXTUREUSAGE_SAMPLER);
        if (!tex)
            return nullptr;
        PendingUpload up;
        up.tex = tex;
        up.width = w;
        up.height = h;
        up.bytesPerPixel = 8;
        up.data.resize(coords.size() * sizeof(uint16_t));
        std::memcpy(up.data.data(), coords.data(), up.data.size());
        uploads.push_back(std::move(up));
        gatherCoordTextures.emplace(key, tex);
        return tex;
    }

    // Fills `out` (w x h) from target t through a coordinate texture.
    void recordGather(Target &t, SDL_GPUTexture *coords, SDL_GPUTexture *out, bool index)
    {
        ps2x::ftrace::Scope traceGather(ps2x::ftrace::kUser8, ps2x::ftrace::kUser8 + 100);
        closePass();
        flushUploads();
        heldTextureWritten(out);
        SDL_GPUColorTargetInfo ct{};
        ct.texture = out;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *rp = texcheck::beginRenderPass(commandBuffer(), &ct, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(rp, index ? gatherIndexPipe : gatherRgbaPipe);
        const float u[4] = {static_cast<float>(scaleX), static_cast<float>(scaleY), static_cast<float>(scaleX / 2u),
                            static_cast<float>(scaleY / 2u)};
        SDL_PushGPUFragmentUniformData(commandBuffer(), 0, u, sizeof(u));
        SDL_GPUTextureSamplerBinding tb[2] = {{t.tex, sampler}, {coords, sampler}};
        texcheck::bindFragmentSamplers(rp, 0, tb, 2);
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
    }

    // A local memory image whose every byte is 0xF0: a 4-bit read of it
    // tells which nibble of its byte a texel is.
    std::vector<uint8_t> nibbleProbe;

    // An indexed texture (T8, T8H, T4, T4HL, T4HH) whose pages are owed,
    // from the target.
    // previous: e's last gathered texture of this size (reused), with the
    // target and content it was gathered from: if that is still the source
    // and has not changed, the texels are the same and nothing is redone.
    bool gatherTexture(const GSTex0Reg &tex, uint32_t w, uint32_t h, TexEntry &e, SDL_GPUTexture *previous,
                       const void *previousTarget, uint64_t previousSerial)
    {
        const bool fourBit = tex.psm == GS_PSM_T4 || tex.psm == GS_PSM_T4HL || tex.psm == GS_PSM_T4HH;
        if (tex.psm != GS_PSM_T8 && tex.psm != GS_PSM_T8H && !fourBit)
            return false;
        Target *t = owedSource(e.pages);
        if (!t)
            return false;
        const uint32_t tbw = std::max<uint32_t>(tex.tbw, 1u);
        const std::array<uint32_t, 10> key = {1u, tex.tbp0, tbw, tex.psm, w, h, t->bp, t->bw, t->height, 0u};
        SDL_GPUTexture *coords = nullptr;
        if (auto it = gatherCoordTextures.find(key); it != gatherCoordTextures.end())
            coords = it->second;
        else
        {
            std::vector<uint16_t> c(static_cast<size_t>(w) * h * 4u);
            const ReadFunc read = readFunc(tex.psm);
            if (fourBit && nibbleProbe.size() != vramSize)
                nibbleProbe.assign(vramSize, 0xF0u);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const uint32_t a = GSMem::ByteAddress(tex.psm, tex.tbp0, tbw, x, y);
                    uint32_t px = 0, py = 0;
                    if (!targetPixelOf(*t, a, px, py))
                        return false;
                    uint16_t *o = &c[(static_cast<size_t>(y) * w + x) * 4u];
                    o[0] = static_cast<uint16_t>(px);
                    o[1] = static_cast<uint16_t>(py);
                    o[2] = static_cast<uint16_t>(a & 3u);
                    if (fourBit)
                        o[2] |= read(nibbleProbe.data(), tex.tbp0, tbw, x, y) != 0u ? 8u : 4u;
                    o[3] = 1;
                }
            coords = coordTexture(key, w, h, c);
            if (!coords)
                return false;
        }
        e.gathered = true;
        e.gatherTarget = t;
        e.tex = previous;
        if (previous && previousTarget == t && previousSerial == t->contentSerial)
        {
            e.gatherSerial = previousSerial;
            return true;
        }
        if (!e.tex)
            e.tex = createTexture(w, h, SDL_GPU_TEXTUREFORMAT_R8_UINT, SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1,
                                  kSiteGather);
        if (!e.tex)
        {
            e.gathered = false;
            return false;
        }
        e.gatherSerial = t->contentSerial;
        recordGather(*t, coords, e.tex, true);
        ++stats.gpuReads;
        if (trace)
            std::cout << "[gs-gpu] texture tbp0=" << tex.tbp0 << " tbw=" << unsigned(tex.tbw) << " psm=" << int(tex.psm) << " " << w << "x" << h
                      << " read from target bp=" << t->bp << " on the GPU" << std::endl;
        return true;
    }

    // ---- CLUT entries loaded on the GPU ----------------------------------
    //
    // A CLUT load from owed pages: the 32-bit value of each loaded entry is
    // gathered into a 256 x 1 texture (by destination entry), which is the
    // palette itself in the common case (CSM1, CSA 0, 8-bit, PSMCT32), and
    // is also downloaded so the CPU backend's CLUT buffer can get those
    // entries when something reads them there (materializeClut).
    struct ClutSnapshot
    {
        uint64_t id = 0;
        std::array<uint32_t, 10> key{}; // CLUT layout (gatherCoordTextures)
        const void *target = nullptr;   // and the target content it came from
        uint64_t serial = 0;
        SDL_GPUTexture *tex = nullptr;
        SDL_GPUTransferBuffer *tb = nullptr;
        uint32_t tbSize = 0;
        std::shared_ptr<FenceRef> fence;
    };
    std::deque<ClutSnapshot> clutSnapshots;
    std::array<uint64_t, 512> clutOwner{}; // snapshot holding CLUT buffer entry i (0: the CPU backend's value)
    uint64_t nextClutSnapshot = 1;
    bool clutOwned = false;

    struct ClutSource
    {
        uint32_t dest;    // CLUT buffer entry (32-bit: and dest + 256)
        uint32_t address; // byte address in local memory
    };

    // What GSCpuBackend::LoadClutUnlocked reads and where it stores it.
    static bool clutSources(const GSTex0Reg &tex0, const GSTexClutReg &texclut, std::vector<ClutSource> &out, bool &sixteenBit)
    {
        out.clear();
        const bool fourBit = isFourBitIndexedPsm(tex0.psm);
        sixteenBit = tex0.cpsm == GS_PSM_CT16 || tex0.cpsm == GS_PSM_CT16S;
        const bool thirtyTwoBit = tex0.cpsm == GS_PSM_CT32 || tex0.cpsm == GS_PSM_CT24;
        if (!sixteenBit && !thirtyTwoBit)
            return false;
        const uint32_t entryCount = fourBit ? 16u : 256u;
        const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
        const uint32_t destinationBase = (static_cast<uint32_t>(tex0.csa) & csaMask) << 4u;
        const bool loadCsm1Suffix = tex0.csm == 0u && thirtyTwoBit && !fourBit;
        const uint32_t firstEntry = loadCsm1Suffix ? destinationBase : 0u;
        for (uint32_t entry = firstEntry; entry < entryCount; ++entry)
        {
            uint32_t x, y, width = 1u;
            if (tex0.csm == 0u)
            {
                const uint32_t sourceIndex = swizzleClutIndexCSM1(entry);
                x = sourceIndex & 0x0Fu;
                y = sourceIndex >> 4u;
            }
            else
            {
                width = texclut.cbw != 0u ? static_cast<uint32_t>(texclut.cbw) : 1u;
                x = (static_cast<uint32_t>(texclut.cou) << 4u) + entry;
                y = static_cast<uint32_t>(texclut.cov);
            }
            const uint32_t dest = (loadCsm1Suffix ? entry : destinationBase + entry) & (sixteenBit ? 0x1FFu : 0x0FFu);
            out.push_back({dest, GSMem::ByteAddress(tex0.cpsm, tex0.cbp, width, x, y)});
        }
        return true;
    }

    std::vector<ClutSource> clutSourceScratch;

    // A due CLUT load from the CPU backend: its entries hold the CPU's values.
    void noteCpuClutLoad(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
    {
        if (!clutOwned)
            return;
        bool sixteenBit = false;
        if (!clutSources(tex0, texclut, clutSourceScratch, sixteenBit))
            return;
        for (const ClutSource &s : clutSourceScratch)
        {
            clutOwner[s.dest] = 0;
            if (!sixteenBit)
                clutOwner[s.dest + 256u] = 0;
        }
        pruneClutSnapshots();
    }

    // A due CLUT load (PSMCT32) from owed pages, on the GPU. False: not
    // possible here (nothing done).
    bool gatherClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut, const GSMem::PageMask &pages)
    {
        if (tex0.cpsm != GS_PSM_CT32)
            return false;
        bool sixteenBit = false;
        if (!clutSources(tex0, texclut, clutSourceScratch, sixteenBit) || clutSourceScratch.empty())
            return false;
        Target *t = owedSource(pages);
        if (!t)
            return false;
        std::array<uint32_t, 10> key = {2u, tex0.cbp, tex0.csm, tex0.csa, isFourBitIndexedPsm(tex0.psm) ? 1u : 0u,
                                        tex0.csm ? (static_cast<uint32_t>(texclut.cbw) | (static_cast<uint32_t>(texclut.cou) << 8) |
                                                    (static_cast<uint32_t>(texclut.cov) << 16))
                                                 : 0u,
                                        t->bp, t->bw, t->height, 0u};
        // The same CLUT from an unchanged target: the newest snapshot again.
        if (!clutSnapshots.empty())
        {
            const ClutSnapshot &last = clutSnapshots.back();
            if (last.key == key && last.target == t && last.serial == t->contentSerial)
            {
                cpu.LoadClut(tex0, texclut);
                for (const ClutSource &s : clutSourceScratch)
                    clutOwner[s.dest] = clutOwner[s.dest + 256u] = last.id;
                clutOwned = true;
                pruneClutSnapshots();
                return true;
            }
        }
        SDL_GPUTexture *coords = nullptr;
        if (auto it = gatherCoordTextures.find(key); it != gatherCoordTextures.end())
            coords = it->second;
        else
        {
            std::vector<uint16_t> c(256u * 4u, 0);
            for (const ClutSource &s : clutSourceScratch)
            {
                uint32_t px = 0, py = 0;
                if (!targetPixelOf(*t, s.address, px, py))
                    return false;
                uint16_t *o = &c[s.dest * 4u];
                o[0] = static_cast<uint16_t>(px);
                o[1] = static_cast<uint16_t>(py);
                o[2] = 0;
                o[3] = 1;
            }
            coords = coordTexture(key, 256u, 1u, c);
            if (!coords)
                return false;
        }
        ClutSnapshot snap;
        snap.id = nextClutSnapshot++;
        snap.key = key;
        snap.target = t;
        snap.serial = t->contentSerial;
        snap.tex = takeClutTexture();
        if (!snap.tex)
            return false;
        recordGather(*t, coords, snap.tex, false);
        snap.tb = takeDownloadBuffer(256u * 4u, snap.tbSize);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer());
        SDL_GPUTextureRegion region{};
        region.texture = snap.tex;
        region.w = 256u;
        region.h = 1u;
        region.d = 1u;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = snap.tb;
        dst.pixels_per_row = 256u;
        dst.rows_per_layer = 1u;
        texcheck::downloadFromTexture(copy, &region, &dst);
        SDL_EndGPUCopyPass(copy);
        // The CPU backend keeps its CLUT bookkeeping (CBP0/CBP1, texture
        // cache); the entries it reads from the owed pages are replaced.
        cpu.LoadClut(tex0, texclut);
        for (const ClutSource &s : clutSourceScratch)
            clutOwner[s.dest] = clutOwner[s.dest + 256u] = snap.id;
        clutOwned = true;
        clutSnapshots.push_back(snap);
        pruneClutSnapshots();
        ++stats.gpuReads;
        if (trace)
            std::cout << "[gs-gpu] clut cbp=" << tex0.cbp << " csm=" << unsigned(tex0.csm) << " csa=" << unsigned(tex0.csa) << " read from target bp="
                      << t->bp << " on the GPU" << std::endl;
        return true;
    }

    // The palette straight from the newest snapshot: 8-bit indices, PSMCT32,
    // CSM1, CSA 0, every entry from that snapshot.
    SDL_GPUTexture *snapshotPalette(const GSTex0Reg &tex)
    {
        if (!clutOwned || clutSnapshots.empty() || tex.cpsm != GS_PSM_CT32 || tex.csm != 0u || tex.csa != 0u ||
            isFourBitIndexedPsm(tex.psm))
            return nullptr;
        const ClutSnapshot &s = clutSnapshots.back();
        for (uint32_t i = 0; i < 512u; ++i)
            if (clutOwner[i] != s.id)
                return nullptr;
        return s.tex;
    }

    // Whether buildPalette(tex) reads entries that a snapshot holds.
    bool paletteReadsSnapshot(const GSTex0Reg &tex) const
    {
        if (!clutOwned)
            return false;
        const bool sixteenBit = tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S;
        const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
        const uint32_t clutBase = (static_cast<uint32_t>(tex.csa) & csaMask) << 4u;
        const bool fourBit = isFourBitIndexedPsm(tex.psm);
        const uint32_t count = fourBit ? 16u : 256u;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t clutIndex = (clutBase + i) & (sixteenBit ? 0x1FFu : 0x0FFu);
            if (!sixteenBit && tex.csm == 0u && !fourBit)
                clutIndex = std::min((i & 0xF0u) + clutBase, 240u) + (i & 0x0Fu);
            if (clutOwner[clutIndex] != 0u || (!sixteenBit && clutOwner[clutIndex + 256u] != 0u))
                return true;
        }
        return false;
    }

    // Snapshot textures (256 x 1) the GPU may still read or write: spare
    // again once their submit is done.
    struct RetiringTexture
    {
        SDL_GPUTexture *tex = nullptr;
        std::shared_ptr<FenceRef> fence; // set by the submit that carries it
    };
    std::vector<RetiringTexture> retiringClutTextures;
    std::vector<SDL_GPUTexture *> spareClutTextures;

    SDL_GPUTexture *takeClutTexture()
    {
        for (size_t i = 0; i < retiringClutTextures.size();)
        {
            const RetiringTexture &r = retiringClutTextures[i];
            if (r.fence && (!r.fence->fence || SDL_QueryGPUFence(dev, r.fence->fence)))
            {
                spareClutTextures.push_back(r.tex);
                retiringClutTextures.erase(retiringClutTextures.begin() + static_cast<std::ptrdiff_t>(i));
            }
            else
                ++i;
        }
        if (!spareClutTextures.empty())
        {
            SDL_GPUTexture *tex = spareClutTextures.back();
            spareClutTextures.pop_back();
            return tex;
        }
        return createTexture(256u, 1u, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                             SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, 1, kSiteClut);
    }

    // Snapshots no entry comes from any more go.
    void pruneClutSnapshots()
    {
        for (auto it = clutSnapshots.begin(); it != clutSnapshots.end();)
        {
            bool used = false;
            for (uint64_t o : clutOwner)
                if (o == it->id)
                {
                    used = true;
                    break;
                }
            if (used)
            {
                ++it;
                continue;
            }
            if (lastPalette == it->tex)
                lastPalette = nullptr;
            retiringClutTextures.push_back({it->tex, it->fence});
            retiringBuffers.push_back({it->tb, it->tbSize, it->fence});
            it = clutSnapshots.erase(it);
        }
        clutOwned = !clutSnapshots.empty();
    }

    // The CPU backend's CLUT buffer gets the entries snapshots hold.
    void materializeClut()
    {
        if (!clutOwned)
            return;
        std::vector<uint8_t> state;
        cpu.SaveClutState(state);
        if (state.size() < 512u * 2u)
            return;
        for (ClutSnapshot &s : clutSnapshots)
        {
            if (!s.fence)
                submit(false);
            if (s.fence && s.fence->fence && !SDL_QueryGPUFence(dev, s.fence->fence))
            {
                ++stats.owedWaits;
                waitFence(s.fence->fence);
            }
            const uint32_t *map = static_cast<const uint32_t *>(SDL_MapGPUTransferBuffer(dev, s.tb, false));
            if (!map)
                continue;
            for (uint32_t i = 0; i < 512u; ++i)
                if (clutOwner[i] == s.id)
                {
                    const uint32_t raw = map[i & 0xFFu];
                    const uint16_t v = static_cast<uint16_t>(i < 256u ? raw & 0xFFFFu : raw >> 16u);
                    std::memcpy(state.data() + i * 2u, &v, 2);
                    clutOwner[i] = 0;
                }
            SDL_UnmapGPUTransferBuffer(dev, s.tb);
        }
        cpu.RestoreClutState(state);
        pruneClutSnapshots();
    }

    // The CLUT buffer was replaced (or reset): snapshots hold nothing.
    void dropClutSnapshots()
    {
        clutOwner.fill(0);
        pruneClutSnapshots();
    }

    // Loads a target's whole area from local memory.
    void load(Target &t)
    {
        ps2x::ftrace::Scope traceLoad(ps2x::ftrace::kTargetLoad, ps2x::ftrace::kTargetLoad + 100);
        // Anything else drawn over these pages goes to memory first.
        for (auto &o : targets)
            if (o.get() != &t && !o->dirty.empty() && o->dirtyPages.intersects(t.pages))
                download(*o);
        materialize(t.pages);
        closePass();
        notReplayable("target load");
        if (trace)
            std::cout << "[gs-gpu] load bp=" << t.bp << " bw=" << t.bw << " psm=" << int(t.psm) << " " << t.width << "x"
                      << t.height << " (" << traceReason << ")" << std::endl;
        const ReadFunc read = readFunc(t.psm);
        if (t.depth)
        {
            const double zScale = depthScale(t.psm);
            PendingUpload up;
            up.tex = stagingTexture(t.width, t.height, SDL_GPU_TEXTUREFORMAT_R32_FLOAT);
            up.width = t.width;
            up.height = t.height;
            up.data.resize(static_cast<size_t>(t.width) * t.height * 4u);
            float *out = reinterpret_cast<float *>(up.data.data());
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                    out[y * t.width + x] = static_cast<float>(static_cast<double>(read(vram, t.bp, t.bw, x, y)) / zScale);
            SDL_GPUTexture *staging = up.tex;
            uploads.push_back(std::move(up));
            flushUploads();
            SDL_GPUDepthStencilTargetInfo dt{};
            dt.texture = t.tex;
            dt.load_op = SDL_GPU_LOADOP_DONT_CARE;
            dt.store_op = SDL_GPU_STOREOP_STORE;
            dt.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
            dt.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
            SDL_GPURenderPass *rp = texcheck::beginRenderPass(commandBuffer(), nullptr, 0, &dt);
            SDL_BindGPUGraphicsPipeline(rp, hwDepth16(t.psm) ? depthLoadPipe16 : depthLoadPipe);
            const float u[4] = {static_cast<float>(scaleX), static_cast<float>(scaleY), 0, 0};
            SDL_PushGPUFragmentUniformData(commandBuffer(), 0, u, sizeof(u));
            SDL_GPUTextureSamplerBinding tb{staging, sampler};
            texcheck::bindFragmentSamplers(rp, 0, &tb, 1);
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
            SDL_EndGPURenderPass(rp);
        }
        else
        {
            PendingUpload up;
            up.tex = stagingTexture(t.width, t.height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM);
            up.width = t.width;
            up.height = t.height;
            up.data.resize(static_cast<size_t>(t.width) * t.height * 4u);
            uint32_t *out = reinterpret_cast<uint32_t *>(up.data.data());
            const bool sixteen = is16BitPsm(t.psm);
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                {
                    uint32_t v = read(vram, t.bp, t.bw, x, y);
                    if (sixteen)
                        v = Rgba5551ToRgba8888(static_cast<u16>(v));
                    out[y * t.width + x] = v;
                }
            SDL_GPUTexture *staging = up.tex;
            uploads.push_back(std::move(up));
            flushUploads();
            SDL_GPUBlitInfo bi{};
            bi.source.texture = staging;
            bi.source.w = t.width;
            bi.source.h = t.height;
            bi.destination.texture = t.tex;
            bi.destination.w = t.width * scaleX;
            bi.destination.h = t.height * scaleY;
            bi.load_op = SDL_GPU_LOADOP_DONT_CARE;
            bi.filter = SDL_GPU_FILTER_NEAREST;
            texcheck::blitTexture(commandBuffer(), &bi);
        }
        // The staging texture is reused (cycled) by later loads.
        t.stale = false;
        t.dirty = Rect{};
        t.dirtyPages.clear();
        t.contentSerial = ++contentClock;
        ++stats.targetUploads;
    }

    // findTarget's last two answers (a draw looks up its colour and its Z
    // buffer), until `targets` gains or loses one.
    struct FoundTarget
    {
        uint32_t bp = UINT32_MAX, bw = 0;
        uint8_t cls = 0xFF;
        Target *t = nullptr;
    } foundTargets[2];
    uint32_t foundNext = 0;
    void targetsChanged()
    {
        foundTargets[0] = foundTargets[1] = FoundTarget{};
    }

    Target *findTarget(uint32_t bp, uint32_t bw, uint8_t psm)
    {
        const uint8_t cls = storageClass(psm);
        for (const FoundTarget &f : foundTargets)
            if (f.bp == bp && f.bw == bw && f.cls == cls)
                return f.t;
        Target *found = nullptr;
        for (auto &t : targets)
            if (t->bp == bp && t->bw == bw && storageClass(t->psm) == cls)
            {
                found = t.get();
                break;
            }
        foundTargets[foundNext] = {bp, bw, cls, found};
        foundNext ^= 1u;
        return found;
    }

    void destroyTarget(Target *t)
    {
        if (passOpen && (pass.color == t || pass.depth == t))
            closePass();
        if (t->tex)
            releaseAfterSubmit.push_back(t->tex);
        auto it = std::find_if(targets.begin(), targets.end(), [&](const std::unique_ptr<Target> &p) { return p.get() == t; });
        if (it != targets.end())
        {
            // PS2X_GPU_TEXTURE_CHECK=1: the object is kept 2 s with a token
            // for its texture, so a stale Target pointer is reported when its
            // texture is used (instead of reading freed memory).
            if (SDL_GPUTexture *token = texcheck::ownerGone("GS target"))
            {
                (*it)->tex = token;
                const auto now = std::chrono::steady_clock::now();
                while (!deadTargets.empty() && now - deadTargets.front().first > std::chrono::seconds(2))
                    deadTargets.pop_front();
                deadTargets.emplace_back(now, std::move(*it));
            }
            targets.erase(it);
            ++targetsDestroyed;
        }
        targetsChanged();
    }
    uint64_t targetsDestroyed = 0; // a Target pointer kept across a getTarget() is stale when this moved
    std::deque<std::pair<std::chrono::steady_clock::time_point, std::unique_ptr<Target>>> deadTargets; // the check's

    // The target for a frame or Z buffer, at least `height` lines high.
    Target *getTarget(uint32_t bp, uint32_t bw, uint8_t psm, uint32_t height)
    {
        bw = std::max<uint32_t>(bw, 1u);
        height = std::min<uint32_t>((height + 31u) & ~31u, kMaxTargetHeight);
        Target *t = findTarget(bp, bw, psm);
        if (t && t->height < height)
        {
            // Grow: through local memory.
            if (!t->dirty.empty())
                download(*t);
            destroyTarget(t);
            t = nullptr;
        }
        if (!t)
        {
            auto nt = std::make_unique<Target>();
            nt->bp = bp;
            nt->bw = bw;
            nt->psm = psm;
            nt->depth = isDepthPsm(psm);
            nt->width = bw * 64u;
            nt->height = height;
            nt->contentSerial = ++contentClock;
            nt->tex = createTexture(nt->width * scaleX, nt->height * scaleY,
                                    nt->depth ? depthFormat(psm) : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    (nt->depth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) |
                                        SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, kSiteTarget);
            GSMem::AddRectPages(nt->pages, psm, bp, bw, 0, 0, nt->width - 1u, nt->height - 1u);
            eraseObjTagsInPages(nt->pages); // drawn into: no texture of the game's there now
            nt->stale = true;
            t = nt.get();
            targets.push_back(std::move(nt));
            targetsChanged();
        }
        else if (t->psm != psm)
            t->psm = psm; // CT24 <-> CT32 share the layout
        if (t->stale)
            load(*t);
        t->lastUse = ++useClock;
        return t;
    }

    // ---- textures --------------------------------------------------------

    void refreshClut()
    {
        if (paletteEpoch == clutEpoch)
            return;
        cpu.SaveClutState(clutStateScratch);
        if (clutStateScratch.size() >= sizeof(clut) && std::memcmp(clut.data(), clutStateScratch.data(), sizeof(clut)) != 0)
        {
            std::memcpy(clut.data(), clutStateScratch.data(), sizeof(clut));
            ++clutContentEpoch;
        }
        paletteEpoch = clutEpoch;
    }

    // Colour of each index as GSCpuBackend::LookupCLUT gives it (TEXA is
    // applied in the shader).
    // The last palette() result, valid while the CLUT and its selection
    // stay the same.
    struct PaletteCacheKey
    {
        uint64_t clutEpoch = 0;
        uint32_t select = UINT32_MAX;
    } lastPaletteKey;
    SDL_GPUTexture *lastPalette = nullptr;
    uint32_t lastPaletteTexa = 0;
    uint64_t lastPaletteContent = 0; // FNV-1a of its colours
    uint32_t lastPaletteAlpha = 0;   // its largest alpha (the PS2's "opaque": 0x80, 0x7F ...)

    SDL_GPUTexture *palette(const GSTex0Reg &tex, uint32_t &texaClass)
    {
        const uint32_t select = paletteSelect(tex);
        // The last load's palette, kept by its source.
        if (clutLoadValid && !clutOwned && clutLoad.select == select)
        {
            auto it = sourcePalettes.find(clutLoad);
            if (it != sourcePalettes.end())
            {
                texaClass = it->second.texaClass;
                lastPalette = it->second.tex;
                lastPaletteTexa = texaClass;
                lastPaletteContent = it->second.content;
                lastPaletteAlpha = it->second.alpha;
                lastPaletteKey = {UINT64_MAX, UINT32_MAX};
                return lastPalette;
            }
        }
        // Without GPU-held CLUT entries (snapshots) the colours alone
        // decide: the same contents give the same palette.
        if (!clutOwned)
            refreshClut();
        const uint64_t epoch = clutOwned ? clutEpoch : (clutContentEpoch | (1ull << 63));
        if (lastPalette && lastPaletteKey.clutEpoch == epoch && lastPaletteKey.select == select)
        {
            texaClass = lastPaletteTexa;
            return lastPalette;
        }
        lastPaletteKey = {epoch, select};
        if (SDL_GPUTexture *snap = snapshotPalette(tex))
        {
            // The colours are on the GPU only: a content key of their own
            // (a replacement keeps to one palette) and the PS2's opaque alpha.
            texaClass = 0;
            lastPalette = snap;
            lastPaletteTexa = texaClass;
            lastPaletteContent = 0x8000000000000000ull | clutSnapshots.back().id;
            lastPaletteAlpha = 0x80u;
            return snap;
        }
        if (paletteReadsSnapshot(tex))
            materializeClut();
        lastPalette = buildPalette(tex, texaClass, lastPaletteContent);
        lastPaletteTexa = texaClass;
        if (clutLoadValid && !clutOwned && clutLoad.select == select && lastPalette)
            sourcePalettes[clutLoad] = {lastPalette, texaClass, lastPaletteAlpha, lastPaletteContent};
        return lastPalette;
    }

    SDL_GPUTexture *buildPalette(const GSTex0Reg &tex, uint32_t &texaClass, uint64_t &content)
    {
        refreshClut();
        Palette pal;
        const bool sixteenBit = tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S;
        const uint32_t csaMask = sixteenBit ? 0x1Fu : 0x0Fu;
        const uint32_t clutBase = (static_cast<uint32_t>(tex.csa) & csaMask) << 4u;
        const bool fourBit = isFourBitIndexedPsm(tex.psm);
        const uint32_t count = fourBit ? 16u : 256u;
        texaClass = tex.cpsm == GS_PSM_CT24 ? 1u : (sixteenBit ? 2u : 0u);
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t clutIndex = (clutBase + i) & (sixteenBit ? 0x1FFu : 0x0FFu);
            if (!sixteenBit && tex.csm == 0u && !fourBit)
                clutIndex = std::min((i & 0xF0u) + clutBase, 240u) + (i & 0x0Fu);
            uint32_t c;
            if (sixteenBit)
                c = Rgba5551ToRgba8888(clut[clutIndex]);
            else if (tex.cpsm == GS_PSM_CT32 || tex.cpsm == GS_PSM_CT24)
                c = static_cast<uint32_t>(clut[clutIndex]) | (static_cast<uint32_t>(clut[clutIndex + 256u]) << 16u);
            else
                c = 0xFFFF00FFu;
            if (tex.cpsm == GS_PSM_CT24)
                c &= 0x00FFFFFFu;
            pal.colors[i] = c;
        }
        const uint64_t key = paletteHash(pal.colors.data(), pal.colors.size());
        content = key;
        lastPaletteAlpha = 0;
        for (uint32_t i = 0; i < count; ++i)
            lastPaletteAlpha = std::max(lastPaletteAlpha, pal.colors[i] >> 24);
        if (traceReplace > 2)
        {
            std::fprintf(stderr, "[gs-gpu] palette %llx cpsm %u csa %u csm %u:", static_cast<unsigned long long>(key), tex.cpsm,
                         tex.csa, tex.csm);
            for (uint32_t i = 0; i < 16u; ++i)
                std::fprintf(stderr, " %08x", pal.colors[i]);
            std::fprintf(stderr, "\n");
        }
        auto it = palettes.find(key);
        if (it != palettes.end() && it->second.colors == pal.colors)
            return it->second.tex;
        if (palettes.size() > 4096u)
        {
            for (auto &kv : palettes)
                giveBackTexture(kv.second.tex);
            palettes.clear();
            sourcePalettes.clear();
            closePass(); // the released ones may be bound by pending draws
            lastPalette = nullptr;
        }
        pal.tex = pooledTexture(256, 1, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, kSitePalette);
        PendingUpload up;
        up.tex = pal.tex;
        up.width = 256;
        up.height = 1;
        up.data.resize(sizeof(pal.colors));
        std::memcpy(up.data.data(), pal.colors.data(), sizeof(pal.colors));
        uploads.push_back(std::move(up));
        SDL_GPUTexture *result = pal.tex;
        palettes[key] = pal;
        return result;
    }

    TexEntry &decodeTexture(const GSTex0Reg &tex, uint32_t w, uint32_t h)
    {
        const uint64_t key = (static_cast<uint64_t>(tex.tbp0) << 32) | (static_cast<uint64_t>(tex.tbw) << 24) |
                             (static_cast<uint64_t>(tex.psm) << 16) | (static_cast<uint64_t>(tex.tw) << 8) | tex.th;
        TexEntry &e = textureEntry(key);
        bool fresh = e.tex != nullptr;
        if (fresh && e.checkedEpoch != epoch)
        {
            fresh = pagesUnchangedSince(e.pages, e.decodeGen);
            if (fresh)
                e.checkedEpoch = epoch;
        }
        e.lastUse = ++useClock;
        if (fresh)
            return e;

        e.decodeSerial = ++decodeSerialClock;
        ps2x::ftrace::Scope traceDecode(ps2x::ftrace::kTextureDecode, ps2x::ftrace::kTextureDecode + 100,
                                        static_cast<uint16_t>(w / 8u));
        const bool indexed = isFourBitIndexedPsm(tex.psm) || isEightBitIndexedPsm(tex.psm);
        // A gathered texture of the same size takes the next gather in place
        // (the gather pass comes after every draw recorded so far).
        SDL_GPUTexture *previous = nullptr;
        if (e.tex && e.gathered && e.width == w && e.height == h)
        {
            previous = e.tex;
            e.tex = nullptr;
        }
        else if (e.tex)
        {
            if (e.passSerial == passSerial && passOpen)
                closePass();
            giveBackTexture(e.tex);
            e.tex = nullptr;
        }
        const bool wasGathered = e.gathered;
        const void *gatherTarget = e.gatherTarget;
        const uint64_t gatherSerial = e.gatherSerial;
        e.gathered = false;
        e.width = w;
        e.height = h;
        e.indexed = indexed;
        e.indexHash = 0;
        e.rgbHashed = false;
        e.replGen = 0;
        e.replPic.reset();
        e.replPalette = 0;
        e.replOff = false;
        e.pages.clear();
        const uint32_t tbw = std::max<uint32_t>(tex.tbw, 1u);
        GSMem::AddRectPages(e.pages, tex.psm, tex.tbp0, tbw, 0, 0, w - 1u, h - 1u);
        if (trace)
        {
            static char reason[96];
            std::snprintf(reason, sizeof(reason), "texture tbp0=%u tbw=%u psm=%u %ux%u", tex.tbp0, tex.tbw, tex.psm, w, h);
            traceReason = reason;
        }
        // Deferred: an 8-bit texture over pages still owed by a write-back
        // is read from the target on the GPU (see gatherTexture); otherwise
        // they are materialized first.
        syncToCpu(e.pages, true);
        if (owedPages.intersects(e.pages))
        {
            if (indexed && gatherTexture(tex, w, h, e, previous, wasGathered ? gatherTarget : nullptr, gatherSerial))
            {
                e.decodeGen = gen;
                e.checkedEpoch = epoch;
                return e;
            }
            materialize(e.pages);
        }
        if (previous)
        {
            if (e.passSerial == passSerial && passOpen)
                closePass();
            giveBackTexture(previous);
        }
        e.tex = pooledTexture(w, h, indexed ? SDL_GPU_TEXTUREFORMAT_R8_UINT : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                              SDL_GPU_TEXTUREUSAGE_SAMPLER, 1, kSiteDecode);
        PendingUpload up;
        up.tex = e.tex;
        up.width = w;
        up.height = h;
        up.bytesPerPixel = indexed ? 1u : 4u;
        up.data.resize(static_cast<size_t>(w) * h * up.bytesPerPixel);
        const ReadFunc read = readFunc(tex.psm);
        if (indexed)
        {
            uint8_t *out = up.data.data();
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                    out[y * w + x] = static_cast<uint8_t>(read(vram, tex.tbp0, tbw, x, y));
            if (contentKeys || traceReplace > 1)
                e.indexHash = ps2x::texture_replace::indexHash(out, up.data.size());
            if (traceReplace > 1)
                std::cerr << "[gs-gpu] texture " << w << "x" << h << " psm " << unsigned(tex.psm) << " hash " << std::hex
                          << e.indexHash << std::dec << std::endl;
        }
        else
        {
            uint32_t *out = reinterpret_cast<uint32_t *>(up.data.data());
            const bool sixteen = is16BitPsm(tex.psm);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    uint32_t v = read(vram, tex.tbp0, tbw, x, y);
                    if (sixteen)
                        v = Rgba5551ToRgba8888(static_cast<u16>(v));
                    out[y * w + x] = v;
                }
            // Movie frames shown at a higher resolution are keyed by RGB.
            if (ps2x::texture_replace::hasRgbEntries())
            {
                e.indexHash = ps2x::texture_replace::rgbHash(out, w, h);
                e.rgbHashed = true;
                if (traceReplace > 1)
                    std::cerr << "[gs-gpu] rgb texture " << w << "x" << h << " psm " << unsigned(tex.psm) << " hash "
                              << std::hex << e.indexHash << std::dec << std::endl;
            }
        }
        uploads.push_back(std::move(up));
        e.decodeGen = gen;
        e.checkedEpoch = epoch;
        return e;
    }

    // A mipmapped texture (GSMipInfo): one GPU texture with a level per GS
    // level, each decoded from its own base pointer and buffer width.
    // Levels whose texels lie in a render target are not decoded (that
    // would read the target back every draw): `lowest` is the first level
    // that is, and the shader samples no level below it. The game's distant
    // models have TBP0 = 0 (the frame buffer) and only levels 1 and up.
    TexEntry &decodeMipTexture(const GSTex0Reg &tex, const GSMipInfo &mip, uint32_t w, uint32_t h, uint32_t &lowest)
    {
        const uint32_t levels = mip.maxLevel + 1u;
        GSMem::PageMask levelPages[7];
        lowest = 0;
        for (uint32_t k = 0; k < levels; ++k)
        {
            const uint32_t lw = std::max<uint32_t>(w >> k, 1u), lh = std::max<uint32_t>(h >> k, 1u);
            GSMem::AddRectPages(levelPages[k], tex.psm, mip.tbp[k], std::max<uint32_t>(mip.tbw[k], 1u), 0, 0, lw - 1u, lh - 1u);
        }
        for (; lowest < mip.maxLevel; ++lowest)
        {
            bool inTarget = false;
            for (auto &tp : targets)
                inTarget = inTarget || (!tp->stale && tp->pages.intersects(levelPages[lowest]));
            if (!inTarget)
                break;
        }
        // PS2X_GS_MIP_TARGET_LEVELS=memory: levels in a render target are
        // read as the GS reads them where the LOD reaches them, decoded from
        // local memory as it is (the target is not brought back; only levels
        // from `synced` on follow GS memory). Dave Mirra BMX 2 keeps level 0
        // of its level textures at TBP 0, in the frame buffer, and starting
        // at level 1 there showed noise (its override sets this for its boot).
        // Unset: the draw starts at the first level outside the targets.
        static ps2x::boot::PerBoot<bool> targetLevelsFromMemory([]
        {
            const char *v = ps2x::boot::text("PS2X_GS_MIP_TARGET_LEVELS");
            return v && std::string_view(v) == "memory";
        });
        const uint32_t synced = lowest;
        if (targetLevelsFromMemory.get())
            lowest = 0;
        // FNV-1a over everything that defines the decoded levels.
        uint64_t key = 1469598103934665603ull;
        auto mix = [&key](uint64_t v)
        {
            for (int i = 0; i < 8; ++i)
            {
                key ^= (v >> (i * 8)) & 0xFFu;
                key *= 1099511628211ull;
            }
        };
        mix(tex.psm | (static_cast<uint64_t>(tex.tw) << 8) | (static_cast<uint64_t>(tex.th) << 16) |
            (static_cast<uint64_t>(levels) << 24) | (static_cast<uint64_t>(synced) << 32));
        for (uint32_t k = lowest; k < levels; ++k)
            mix(mip.tbp[k] | (static_cast<uint64_t>(mip.tbw[k]) << 32));
        key |= 1ull << 63; // apart from decodeTexture's keys (bit 63 clear)
        TexEntry &e = textureEntry(key);
        bool fresh = e.tex != nullptr;
        if (fresh && e.checkedEpoch != epoch)
        {
            fresh = pagesUnchangedSince(e.pages, e.decodeGen);
            if (fresh)
                e.checkedEpoch = epoch;
        }
        e.lastUse = ++useClock;
        if (fresh)
            return e;

        e.decodeSerial = ++decodeSerialClock;
        const bool indexed = isFourBitIndexedPsm(tex.psm) || isEightBitIndexedPsm(tex.psm);
        if (e.tex)
        {
            if (e.passSerial == passSerial && passOpen)
                closePass();
            giveBackTexture(e.tex);
            e.tex = nullptr;
        }
        e.width = w;
        e.height = h;
        e.indexed = indexed;
        e.mipSynced = synced;
        e.pages.clear();
        for (uint32_t k = synced; k < levels; ++k)
            e.pages.merge(levelPages[k]);
        syncToCpu(e.pages);
        e.tex = pooledTexture(w, h, indexed ? SDL_GPU_TEXTUREFORMAT_R8_UINT : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                              SDL_GPU_TEXTUREUSAGE_SAMPLER, levels, kSiteMipDecode);
        const ReadFunc read = readFunc(tex.psm);
        const bool sixteen = is16BitPsm(tex.psm);
        for (uint32_t k = lowest; k < levels; ++k)
        {
            const uint32_t lw = std::max<uint32_t>(w >> k, 1u), lh = std::max<uint32_t>(h >> k, 1u);
            const uint32_t tbp = mip.tbp[k], tbw = std::max<uint32_t>(mip.tbw[k], 1u);
            PendingUpload up;
            up.tex = e.tex;
            up.level = k;
            up.width = lw;
            up.height = lh;
            up.bytesPerPixel = indexed ? 1u : 4u;
            up.data.resize(static_cast<size_t>(lw) * lh * up.bytesPerPixel);
            if (indexed)
            {
                uint8_t *out = up.data.data();
                for (uint32_t y = 0; y < lh; ++y)
                    for (uint32_t x = 0; x < lw; ++x)
                        out[y * lw + x] = static_cast<uint8_t>(read(vram, tbp, tbw, x, y));
            }
            else
            {
                uint32_t *out = reinterpret_cast<uint32_t *>(up.data.data());
                for (uint32_t y = 0; y < lh; ++y)
                    for (uint32_t x = 0; x < lw; ++x)
                    {
                        uint32_t v = read(vram, tbp, tbw, x, y);
                        if (sixteen)
                            v = Rgba5551ToRgba8888(static_cast<u16>(v));
                        out[y * lw + x] = v;
                    }
            }
            uploads.push_back(std::move(up));
        }
        e.decodeGen = gen;
        e.checkedEpoch = epoch;
        return e;
    }

    // ---- replacement pictures (runtime/ps2_texture_replace.h) ------------

    bool replTint = false;  // PS2X_GS_REPL_TINT=1: replacements drawn tinted red
    bool replTint2 = false; // PS2X_GS_REPL_TINT=2: also indexed textures without one, blue
    int traceReplace = 0; // PS2X_TRACE_TEXTURE_REPLACE=1: replacements found / turned off, 2: every indexed decode
    // PS2X_TEXTURE_REPLACE_CONTENT=1: also find replacements by what GS
    // memory holds (the indices' hash at each decode), for textures no
    // override names. Off: only by identity (GSTextureObjectNote).
    bool contentKeys = false;

    // ---- textures by identity (GSTextureObjectNote) ----------------------
    //
    // The game's texture cache says which of its texture objects (and which
    // mip level) it put where; a draw whose TEX0 (or, with only mip levels 1
    // and up resident, MIPTBP1) points at one is drawn with that object's
    // host picture, whatever GS memory holds. Notes are kept by block address
    // until another note or a write other than their own upload covers them.

    struct ObjTag
    {
        GSTextureObjectNote note{};
        uint32_t last = 0; // last block
        ps2x::texture_replace::PicturePtr pic;
        uint64_t picGen = 0;    // registry generation pic was looked up at
        uint64_t palette = 0;   // the palette first drawn with it
        bool paletteOff = false; // drawn with another palette since: not replaced
        bool regionLogged = false;
    };
    std::map<uint32_t, ObjTag> objTags; // by first block
    // objectFor's last lookup (objTagsChanged: objTags had an insert or erase since).
    uint32_t lastObjTbp = UINT32_MAX;
    ObjTag *lastObj = nullptr;
    bool objTagsChanged = true;
    uint32_t objTagSpan = 0;            // largest blocks of any note (overlap search)
    uint64_t objNotes = 0, objTagsErased = 0;

    // Notes overlapping blocks [first, last] go, but for one whose own upload
    // this is (same start, format and size).
    template <typename Keep>
    void eraseObjTags(uint32_t first, uint32_t last, Keep keep)
    {
        if (objTags.empty())
            return;
        auto it = objTags.upper_bound(last);
        while (it != objTags.begin())
        {
            --it;
            if (it->first + objTagSpan <= first)
                break;
            if (it->second.last < first || keep(it->second))
                continue;
            ++objTagsErased;
            objTagsChanged = true;
            it = objTags.erase(it);
        }
    }
    void eraseObjTags(uint32_t first, uint32_t last)
    {
        eraseObjTags(first, last, [](const ObjTag &) { return false; });
    }

    void eraseObjTagsInPages(const GSMem::PageMask &pages)
    {
        if (objTags.empty())
            return;
        for (uint32_t i = 0; i < kPageCount; ++i)
            if (pages.test(i))
                eraseObjTags(i * 32u, i * 32u + 31u);
    }

    // The first and last block a rectangle of a transfer touches.
    static bool rectBlocks(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h,
                           uint32_t &first, uint32_t &last)
    {
        uint32_t bwPx = 8, bhPx = 8; // block size in pixels
        switch (psm)
        {
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            bwPx = 16u;
            break;
        case GS_PSM_T8:
            bwPx = bhPx = 16u;
            break;
        case GS_PSM_T4:
            bwPx = 32u;
            bhPx = 16u;
            break;
        default:
            break;
        }
        if (w == 0u || h == 0u)
            return false;
        first = UINT32_MAX;
        last = 0;
        bw = std::max<uint32_t>(bw, 1u);
        const uint32_t x1 = x0 + w - 1u, y1 = y0 + h - 1u;
        for (uint32_t by = y0 / bhPx; by <= y1 / bhPx; ++by)
            for (uint32_t bx = x0 / bwPx; bx <= x1 / bwPx; ++bx)
            {
                const uint32_t block = GSMem::ByteAddress(psm, bp, bw, bx * bwPx, by * bhPx) / 256u;
                first = std::min(first, block);
                last = std::max(last, block);
            }
        return first <= last;
    }

    void noteTextureObjects(const GSTextureObjectNote *notes, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const GSTextureObjectNote &n = notes[i];
            if (n.blocks == 0u || n.tbp >= 0x4000u)
                continue;
            ObjTag t;
            t.note = n;
            t.last = std::min<uint32_t>(n.tbp + n.blocks - 1u, 0x3FFFu);
            eraseObjTags(n.tbp, t.last);
            objTagSpan = std::max(objTagSpan, n.blocks);
            objTags[n.tbp] = std::move(t);
            objTagsChanged = true;
            ++objNotes;
            if (traceReplace > 2)
                std::cerr << "[gs-gpu] object 0x" << std::hex << n.object << std::dec << " " << n.name << " level "
                          << unsigned(n.level) << " at " << n.tbp << "+" << n.blocks << " " << n.width << "x" << n.height
                          << " psm " << unsigned(n.psm) << std::endl;
        }
    }

    // A note's name (at most 23 characters of the game's) against a
    // picture's (the PS2 texture's name), ASCII case aside; a picture
    // without a name goes with any.
    static bool sameTextureName(const char *note, const std::string &picture)
    {
        if (picture.empty() || !note[0])
            return true;
        size_t i = 0;
        for (; note[i] && i < picture.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(note[i])) != std::tolower(static_cast<unsigned char>(picture[i])))
                return false;
        return !note[i] ? (i == picture.size() || i == sizeof(GSTextureObjectNote::name) - 1u) : false;
    }

    // The texture object a draw samples, if a note names it: level 0 at
    // TBP0, or level 1 at MIPTBP1's TBP1 (the game's distant models: only
    // levels 1 and up uploaded, TBP0 = 0).
    ObjTag *objectFor(const GSTex0Reg &tex, uint32_t texW, uint32_t texH, const GSMipInfo &mip, bool mipmapped)
    {
        if (objTags.empty())
            return nullptr;
        const auto at = [&](uint32_t tbp, uint32_t level) -> ObjTag *
        {
            // Draws of one texture come in runs: the last lookup is kept
            // until objTags changes.
            if (tbp != lastObjTbp || objTagsChanged)
            {
                const auto it = objTags.find(tbp);
                lastObjTbp = tbp;
                lastObj = it == objTags.end() ? nullptr : &it->second;
                objTagsChanged = false;
            }
            if (!lastObj)
                return nullptr;
            ObjTag &t = *lastObj;
            if (t.note.level != level || t.note.psm != tex.psm || t.note.width != std::max<uint32_t>(texW >> level, 1u) ||
                t.note.height != std::max<uint32_t>(texH >> level, 1u))
                return nullptr;
            return &t;
        };
        if (ObjTag *t = at(tex.tbp0, 0))
            return t;
        if (mipmapped && mip.maxLevel >= 1u)
            return at(mip.tbp[1], 1);
        return nullptr;
    }

    // The replacement picture for a draw of a noted object (null: draw the
    // PS2 texture). Not when it is drawn with a palette other than the one
    // it was first drawn with (CLUT animation), nor with REGION_REPEAT over a
    // sub-rectangle.
    ReplTex *replacementForObject(ObjTag &t, uint64_t paletteContent, uint64_t clamp, uint32_t texW, uint32_t texH)
    {
        if (!replBc2 || t.note.keyHash == 0u)
            return nullptr;
        const uint64_t gen = ps2x::texture_replace::generation();
        if (t.picGen != gen)
        {
            if (gen != replSweepGen)
            {
                replSweepGen = gen;
                sweepReplacements();
            }
            t.picGen = gen;
            t.pic = ps2x::texture_replace::find(t.note.keyWidth, t.note.keyHeight, t.note.keyHash);
            t.palette = 0;
            t.paletteOff = false;
            // The same indices in another texture (a flat one, the game's
            // own run-time textures): a picture is for the texture of its
            // name only.
            if (t.pic && !sameTextureName(t.note.name, t.pic->name))
            {
                if (traceReplace)
                    std::cerr << "[gs-gpu] object 0x" << std::hex << t.note.object << std::dec << " " << t.note.name
                              << " has the key of " << t.pic->name << ": not replaced" << std::endl;
                t.pic.reset();
            }
            if (t.pic && traceReplace)
                std::cerr << "[gs-gpu] object 0x" << std::hex << t.note.object << std::dec << " " << t.note.name << " "
                          << t.note.keyWidth << "x" << t.note.keyHeight << " drawn as " << t.pic->name << " "
                          << t.pic->width << "x" << t.pic->height << std::endl;
        }
        if (!t.pic || t.paletteOff)
            return nullptr;
        if (t.palette == 0u)
            t.palette = paletteContent;
        else if (t.palette != paletteContent)
        {
            t.paletteOff = true;
            if (traceReplace)
                std::cerr << "[gs-gpu] " << t.note.name << " drawn with another palette: not replaced" << std::endl;
            return nullptr;
        }
        if (!regionRepeatReplaceable(clamp, texW, texH))
        {
            if (traceReplace && !t.regionLogged)
            {
                t.regionLogged = true;
                std::cerr << "[gs-gpu] " << t.note.name << " REGION_REPEAT clamp=" << std::hex << clamp << std::dec
                          << ": not replaced" << std::endl;
            }
            return nullptr;
        }
        return replacementTexture(t.pic);
    }

    // Gives back the GPU copies of pictures the registry dropped.
    void sweepReplacements()
    {
        for (auto it = replTextures.begin(); it != replTextures.end();)
        {
            if (!it->second.pic.expired())
            {
                ++it;
                continue;
            }
            if (passOpen && it->second.passSerial == passSerial)
                closePass();
            giveBackTexture(it->second.tex);
            replGpuBytes -= std::min(replGpuBytes, it->second.bytes);
            lastRepl = nullptr;
            it = replTextures.erase(it);
        }
    }

    // The GPU copy of a picture, uploaded on first use (null while this
    // frame's upload budget is spent: the PS2 texture is drawn meanwhile).
    ReplTex *replacementTexture(const ps2x::texture_replace::PicturePtr &pic, bool fromPack = false)
    {
        if (lastRepl && lastReplPic == pic.get() && lastRepl->pic.lock() == pic)
            return lastRepl;
        lastRepl = nullptr;
        auto it = replTextures.find(pic.get());
        if (it != replTextures.end())
        {
            if (it->second.pic.lock() == pic)
            {
                lastReplPic = pic.get();
                lastRepl = &it->second;
                return &it->second;
            }
            // An old picture at the same address.
            if (passOpen && it->second.passSerial == passSerial)
                closePass();
            giveBackTexture(it->second.tex);
            replGpuBytes -= std::min(replGpuBytes, it->second.bytes);
            replTextures.erase(it);
        }
        const size_t size = pic->levelOffset(pic->mips);
        if (replUploadedThisFrame > 0u && replUploadedThisFrame + size > kReplUploadPerFrame)
            return nullptr;
        if (fromPack && packUploadedThisFrame > 0u && packUploadedThisFrame + size > packUploadPerFrame)
            return nullptr;
        if (fromPack)
            packUploadedThisFrame += size;
        ps2x::ftrace::Scope traceRepl(ps2x::ftrace::kReplacementUpload, ps2x::ftrace::kReplacementUpload + 100,
                                      static_cast<uint16_t>(std::min<size_t>(size >> 16, 65535u)));
        const SDL_GPUTextureFormat format = pic->format == ps2x::texture_replace::Format::BC2
                                                ? SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM
                                                : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        SDL_GPUTexture *t = pooledTexture(pic->width, pic->height, format, SDL_GPU_TEXTUREUSAGE_SAMPLER, pic->mips, kSiteReplacement);
        if (!t)
            return nullptr;
        for (uint32_t level = 0; level < pic->mips; ++level)
        {
            PendingUpload up;
            up.tex = t;
            up.width = pic->levelWidth(level);
            up.height = pic->levelHeight(level);
            up.level = level;
            up.src = pic;
            up.srcOffset = pic->levelOffset(level);
            up.srcSize = pic->levelSize(level);
            uploads.push_back(std::move(up));
        }
        replUploadedThisFrame += size;
        replGpuBytes += size;
        stats.replacementBytes += size;
        ReplTex &r = replTextures[pic.get()];
        r.pic = pic;
        r.tex = t;
        r.bytes = size;
        lastReplPic = pic.get();
        lastRepl = &r;
        return &r;
    }

    // REGION_REPEAT with MSK = size - 1 and FIX = 0 (how this game asks for
    // plain repeat) is REPEAT; other region repeats (a sub-rectangle) have no
    // equivalent on a picture of another size and are not replaced.
    static bool regionRepeatIsRepeat(uint64_t clamp, uint32_t w, uint32_t h)
    {
        const auto ok = [](uint32_t mode, uint32_t msk, uint32_t fix, uint32_t size)
        { return mode != 3u || ((msk & (size - 1u)) == size - 1u && (fix & (size - 1u)) == 0u); };
        return ok(static_cast<uint32_t>(clamp & 3u), static_cast<uint32_t>((clamp >> 4) & 0x3FFu),
                  static_cast<uint32_t>((clamp >> 14) & 0x3FFu), w) &&
               ok(static_cast<uint32_t>((clamp >> 2) & 3u), static_cast<uint32_t>((clamp >> 24) & 0x3FFu),
                  static_cast<uint32_t>((clamp >> 34) & 0x3FFu), h);
    }

    // REGION_REPEAT a replacement can follow: a power-of-two cell the
    // shader wraps in (u & MSK) | FIX with FIX on a cell boundary, which
    // is how this game repeats one picture of an atlas (palms, buildings,
    // tires). Others (MSK not 2^k - 1) are not replaced.
    static bool regionRepeatReplaceable(uint64_t clamp, uint32_t w, uint32_t h)
    {
        const auto ok = [](uint32_t mode, uint32_t msk, uint32_t fix, uint32_t size)
        {
            msk &= size - 1u;
            return mode != 3u || (((msk + 1u) & msk) == 0u && (fix & msk) == 0u);
        };
        return ok(static_cast<uint32_t>(clamp & 3u), static_cast<uint32_t>((clamp >> 4) & 0x3FFu),
                  static_cast<uint32_t>((clamp >> 14) & 0x3FFu), w) &&
               ok(static_cast<uint32_t>((clamp >> 2) & 3u), static_cast<uint32_t>((clamp >> 24) & 0x3FFu),
                  static_cast<uint32_t>((clamp >> 34) & 0x3FFu), h);
    }

    // The sampler for a replacement: clamped on an axis the GS clamps or
    // wraps in a cell (the shader does that), repeating on a plain repeat.
    SDL_GPUSampler *replacementSampler(uint64_t clamp, uint32_t w, uint32_t h)
    {
        const auto clamped = [](uint32_t mode, uint32_t msk, uint32_t size)
        { return mode == 1u || mode == 2u || (mode == 3u && (msk & (size - 1u)) != size - 1u); };
        return replSampler[clamped(static_cast<uint32_t>(clamp & 3u), static_cast<uint32_t>((clamp >> 4) & 0x3FFu), w)]
                          [clamped(static_cast<uint32_t>((clamp >> 2) & 3u), static_cast<uint32_t>((clamp >> 24) & 0x3FFu), h)];
    }

    // The replacement to draw for an indexed texture, if any. Not when the
    // texture is drawn with a palette other than the one it was first drawn
    // with (CLUT animation, or the same indices meaning another picture),
    // nor with REGION_REPEAT over a sub-rectangle.
    ReplTex *replacementFor(TexEntry &e, uint64_t paletteContent, uint64_t clamp)
    {
        if (!replBc2)
            return nullptr;
        const uint64_t gen = ps2x::texture_replace::generation();
        if (!e.rgbHashed && !contentKeys)
            return nullptr;
        // Indices read from a target on the GPU (gatherTexture) have no key:
        // a frame buffer is no packed picture.
        if (e.gathered)
            return nullptr;
        if (e.replGen != gen)
        {
            if (gen != replSweepGen)
            {
                replSweepGen = gen;
                sweepReplacements();
            }
            e.replGen = gen;
            e.replPic = ps2x::texture_replace::find(e.width, e.height, e.indexHash, e.rgbHashed);
            e.replPalette = 0;
            e.replOff = false;
            if (e.replPic && traceReplace)
                std::cerr << "[gs-gpu] replacing " << e.width << "x" << e.height << " " << std::hex << e.indexHash << std::dec
                          << " with " << e.replPic->name << " " << e.replPic->width << "x" << e.replPic->height << std::endl;
        }
        if (!e.replPic || e.replOff)
            return nullptr;
        if (e.replPalette == 0u)
            e.replPalette = paletteContent;
        else if (e.replPalette != paletteContent)
        {
            e.replOff = true;
            if (traceReplace)
                std::cerr << "[gs-gpu] " << e.replPic->name << " drawn with another palette (" << std::hex << e.replPalette
                          << " then " << paletteContent << std::dec << "): not replaced" << std::endl;
            return nullptr;
        }
        if (!regionRepeatIsRepeat(clamp, e.width, e.height))
        {
            if (traceReplace && !e.replRegionLogged)
            {
                e.replRegionLogged = true;
                std::cerr << "[gs-gpu] " << e.replPic->name << " REGION_REPEAT clamp=" << std::hex << clamp << std::dec
                          << ": not replaced" << std::endl;
            }
            return nullptr;
        }
        return replacementTexture(e.replPic);
    }

    // ---- texture packs (runtime/ps2_texture_pack.h) ------------------------
    //
    // A draw's texture is named as PCSX2 names it (its GS memory, its
    // palette, its region from CLAMP), and a pack's picture of that name is
    // drawn in its place: texture kind 3, as the Xbox pictures. Dump mode
    // writes each name it has not seen. Nothing here runs without a pack or
    // a dump folder (texture_pack::active).

    // The palette of the draw as PCSX2 reads it, kept while the CLUT, its
    // selection and TEXA stay the same.
    struct PackClut
    {
        uint64_t contentEpoch = 0;
        uint32_t select = UINT32_MAX;
        uint32_t texa = UINT32_MAX;
        uint32_t count = 0;
        uint64_t hash = 0;
        std::array<uint32_t, 256> entries{};
    } packClutCache;
    // Names dump mode has written (or a pack has): not read out again.
    std::unordered_set<ps2x::texture_pack::Name, ps2x::texture_pack::NameHash> packDumped;
    uint64_t packSweepGen = 0; // texture_pack::generation() at the last sweep

    const PackClut *packClutFor(const GSTex0Reg &tex, const GSTexaReg &texa)
    {
        refreshClut();
        const uint32_t select = (static_cast<uint32_t>(tex.psm) & 0x3Fu) | (static_cast<uint32_t>(tex.cpsm & 0x3Fu) << 6) |
                                (static_cast<uint32_t>(tex.csa & 0x1Fu) << 12);
        const bool sixteen = tex.cpsm == GS_PSM_CT16 || tex.cpsm == GS_PSM_CT16S;
        const uint32_t texaKey = sixteen ? (texa.ta0 | (static_cast<uint32_t>(texa.ta1) << 8) | (texa.aem ? 1u << 16 : 0u)) : 0u;
        PackClut &c = packClutCache;
        if (c.contentEpoch != clutContentEpoch || c.select != select || c.texa != texaKey)
        {
            c.contentEpoch = clutContentEpoch;
            c.select = select;
            c.texa = texaKey;
            c.count = ps2x::texture_pack::readClut(clut.data(), tex.psm, tex.cpsm, tex.csa, texa, c.entries.data());
            c.hash = ps2x::texture_pack::clutHash(c.entries.data(), c.count);
        }
        return &c;
    }

    // The pack's picture for a draw of the decoded texture e (null: none,
    // not read yet, or a form a picture cannot follow). region: the part of
    // the texture the picture covers, in level 0's texels.
    ReplTex *packReplacement(TexEntry &e, const GSTex0Reg &tex, const GSDrawState &st, bool mipmapped,
                             ps2x::texture_pack::Region &region)
    {
        namespace tp = ps2x::texture_pack;
        // Texels read from a target on the GPU are not what local memory holds.
        if (e.gathered)
            return nullptr;
        const auto started = std::chrono::steady_clock::now();
        struct Timed
        {
            uint64_t &total;
            std::chrono::steady_clock::time_point start;
            ~Timed()
            {
                total += static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
            }
        } timed{stats.packNanoseconds, started};
        const bool palette = tp::hasPalette(tex.psm);
        if (!palette && !tp::hasPictures() && !tp::dumpingDirect())
            return nullptr;
        // GPU-held CLUT entries are not in the CPU's CLUT buffer.
        if (palette && clutOwned)
            return nullptr;
        const auto &ctx = st.context;
        const GSMipInfo &mip = st.mip;
        const uint32_t count = mipmapped ? std::min<uint32_t>(mip.maxLevel + 1u, 7u) : 1u;
        // A level 0 in a render target past the frame buffer's block 0 (its
        // names would follow what is drawn there).
        if (mipmapped && e.mipSynced > ((count > 1u && tex.tbp0 == 0u) ? 1u : 0u))
            return nullptr;
        const uint64_t shape = (ctx.clamp & 0xFFFFFFFFFFFull) ^
                               (static_cast<uint64_t>(tp::nameBits(tex.psm, tex.tw, tex.th, st.texa)) * 0x9E3779B97F4A7C15ull);
        if (e.packSerial != e.decodeSerial || e.packShape != shape)
        {
            if (!e.packNames)
                e.packNames = std::make_shared<TexEntry::PackNames>();
            auto &levels = e.packNames->levels;
            for (uint32_t k = 0; k < count; ++k)
            {
                levels[k].tbp = k == 0u ? tex.tbp0 : mip.tbp[k];
                levels[k].tbw = k == 0u ? tex.tbw : mip.tbw[k];
                levels[k].twLog2 = tex.tw > k ? tex.tw - k : 0u;
                levels[k].thLog2 = tex.th > k ? tex.th - k : 0u;
            }
            // Their shapes only: a name is hashed when a pack may have it
            // (or dump mode wants the first one).
            e.packCount = tp::candidateShapes(tex.psm, levels.data(), count, ctx.clamp, st.texa, 0u, e.packNames->names.data());
            e.packSerial = e.decodeSerial;
            e.packShape = shape;
            e.packGen = 0;
        }
        auto &names = e.packNames->names;
        const auto hashed = [&](uint32_t i) -> tp::Candidate &
        {
            if (!names[i].hashed)
            {
                ++stats.packHashes;
                syncToCpu(e.pages);
                tp::hashCandidate(vram, tex.psm, e.packNames->levels.data(), st.texa, names[i]);
            }
            return names[i];
        };
        if (e.packCount == 0u)
            return nullptr;
        const PackClut *pc = palette ? packClutFor(tex, st.texa) : nullptr;
        const uint64_t clutHash = pc ? pc->hash : 0u;
        const uint64_t gen = tp::generation();
        // Pictures the packs dropped (over their budget) leave the GPU too.
        if (gen != packSweepGen)
        {
            packSweepGen = gen;
            sweepReplacements();
        }
        if (e.packGen != gen || e.packClut != clutHash)
        {
            ++stats.packLookups;
            e.packGen = gen;
            e.packClut = clutHash;
            const ps2x::texture_replace::PicturePtr before = e.packPic;
            e.packPic.reset();
            bool anyKnown = false;
            for (uint32_t i = 0; i < e.packCount && !anyKnown; ++i)
            {
                tp::Name name = names[i].name;
                name.clutHash = palette ? clutHash : 0u;
                if (!tp::mayHave(name))
                    continue;
                name.textureHash = hashed(i).name.textureHash;
                e.packPic = tp::find(name, &anyKnown);
                e.packChosen = i;
            }
            const bool dumpIt = !anyKnown && tp::dumping() && (palette || tp::dumpingDirect());
            tp::Name first = dumpIt ? hashed(0).name : names[0].name;
            first.clutHash = palette ? clutHash : 0u;
            if (dumpIt && packDumped.insert(first).second)
            {
                syncToCpu(e.pages);
                const tp::Candidate &c = names[0];
                const uint32_t b = c.base;
                const tp::Level level{b == 0u ? tex.tbp0 : mip.tbp[b], b == 0u ? tex.tbw : mip.tbw[b],
                                      tex.tw > b ? tex.tw - b : 0u, tex.th > b ? tex.th - b : 0u};
                tp::Region r;
                r.hasX = c.region0.hasX;
                r.hasY = c.region0.hasY;
                r.minX = c.region0.minX >> b;
                r.maxX = c.region0.maxX >> b;
                r.minY = c.region0.minY >> b;
                r.maxY = c.region0.maxY >> b;
                const uint32_t w = first.regionWidth ? first.regionWidth : (1u << level.twLog2);
                const uint32_t h = first.regionHeight ? first.regionHeight : (1u << level.thLog2);
                tp::dump(first, w, h, tp::readColors(vram, tex.psm, level, r, st.texa, pc ? pc->entries.data() : nullptr));
            }
            if (e.packPic && traceReplace && e.packPic != before)
                std::cerr << "[gs-gpu] pack picture " << e.packPic->name << " " << e.packPic->width << "x" << e.packPic->height
                          << " for " << [&]
                             {
                                 tp::Name n = names[e.packChosen].name;
                                 n.clutHash = palette ? clutHash : 0u;
                                 return tp::fileName(n);
                             }() << std::endl;
        }
        if (!e.packPic)
            return nullptr;
        if (!regionRepeatReplaceable(ctx.clamp, e.width, e.height))
            return nullptr;
        region = names[e.packChosen].region0;
        return replacementTexture(e.packPic, true);
    }

    // ---- native draws' texture filtering (gs_quality.h Anisotropy) -------
    //
    // A native draw's replacement picture is sampled trilinear, with the
    // option's anisotropy (Off: without). With the option on, a native draw
    // of a PS2 texture (no replacement) is drawn from a mipmapped RGBA copy
    // (runtime/gs/gs_texture_mips.h) the same way: texture kind 3, the
    // replacement's coordinates and clamps, the copy's alpha as it is (TEXA
    // applied per texel when the copy is made). GS-path draws (2D, HUD,
    // particles) and the option Off sample as the GS does.

    struct MipCopy
    {
        SDL_GPUTexture *tex = nullptr;
        size_t bytes = 0;
        uint64_t lastFrame = 0; // mipFrame of its last draw
        uint64_t passSerial = 0;
    };
    // By content: the texels as read (indices or colours, every level the
    // GS has), the palette's colours, TEXA, size and format. A texture the
    // game's cache flushes and uploads again (fast turns) finds its copy.
    std::unordered_map<uint64_t, MipCopy> mipCopies;
    // A decode (TexEntry::decodeSerial) with a palette and TEXA -> its
    // copy's content key, so a texture drawn again is not read again.
    std::unordered_map<uint64_t, uint64_t> mipCopyKeys;
    uint64_t decodeSerialClock = 0;
    uint64_t mipFrame = 0;          // presented frames
    size_t mipCopyGpuBytes = 0;
    size_t mipTexelsThisFrame = 0;  // texels read or built this frame
    // Budget per presented frame, in texels read again from GS memory and
    // texels built (about 3.5 ns each on an M5 Pro: half a million, under
    // 2 ms; a level's first frames make a hundred copies, 1.3 M texels in
    // the Movie Lot): over it, a texture is drawn as the GS does until a
    // later frame. PS2X_GS_MIP_BUDGET=<texels>, 0 no limit.
    size_t mipTexelsPerFrame = size_t(512) << 10;
    static constexpr size_t kMipCopyBytesMax = size_t(256) << 20;
    static constexpr uint64_t kMipCopyIdleFrames = 1800; // dropped unused this long (30 s)

    // The sampler for a native draw's picture: clamped or repeating per axis
    // as replacementSampler, filtered as the option says.
    SDL_GPUSampler *filterSamplerFor(uint64_t clamp, uint32_t w, uint32_t h, int level)
    {
        const auto clamped = [](uint32_t mode, uint32_t msk, uint32_t size)
        { return mode == 1u || mode == 2u || (mode == 3u && (msk & (size - 1u)) != size - 1u); };
        return filterSampler[std::clamp(level, 0, 4)]
                            [clamped(static_cast<uint32_t>(clamp & 3u), static_cast<uint32_t>((clamp >> 4) & 0x3FFu), w)]
                            [clamped(static_cast<uint32_t>((clamp >> 2) & 3u), static_cast<uint32_t>((clamp >> 24) & 0x3FFu), h)];
    }

    // drawPrepared's hook for native draws, after the texture is chosen:
    // a replacement's sampler by the option, or (option on) a PS2 texture's
    // mipmapped copy in place of the texture. Returns the copy drawn, if any.
    MipCopy *nativeFiltering(const GSDrawState &st, TexEntry *entry, uint32_t lowest, uint32_t &texKind,
                             uint32_t &texaClass, SDL_GPUTexture *texBind[4], SDL_GPUSampler *&sampler0)
    {
        const auto &ctx = st.context;
        const uint32_t texW = st.textureWidth, texH = st.textureHeight;
        const int level = std::clamp(ps2x::gs_quality::get(ps2x::gs_quality::Option::Anisotropy), 0, 4);
        if (texKind == 3u)
        {
            sampler0 = filterSamplerFor(ctx.clamp, texW, texH, level);
            return nullptr;
        }
        if (level == 0 || !entry || entry->gathered || (texKind != 0u && texKind != 1u) || !entry->tex)
            return nullptr;
        // REGION_REPEAT the shader cannot follow on a filtered picture.
        if (!regionRepeatReplaceable(ctx.clamp, texW, texH))
            return nullptr;
        MipCopy *m = mipCopyFor(*entry, st, lowest, texaClass);
        if (!m)
            return nullptr;
        texKind = 3u;
        texaClass = 0u; // in the copy's texels
        texBind[0] = m->tex;
        texBind[1] = dummyIndex;
        texBind[2] = dummyRgba;
        sampler0 = filterSamplerFor(ctx.clamp, texW, texH, level);
        ++stats.mipCopyPrimitives;
        return m;
    }

    // The mipmapped copy of a decoded texture as this draw sees it (its
    // palette, TEXA), made on first use; null: draw the texture as the GS
    // does (a palette only on the GPU, texels owed by a write-back, this
    // frame's budget spent).
    MipCopy *mipCopyFor(TexEntry &e, const GSDrawState &st, uint32_t lowest, uint32_t texaClass)
    {
        namespace gm = ps2x::gs_mips;
        const GSTex0Reg &tex = st.context.tex0;
        const bool indexed = e.indexed;
        const uint32_t *palColors = nullptr;
        if (indexed)
        {
            // The draw's palette (palette() just set lastPaletteContent);
            // colours held on the GPU only (a snapshot) have no CPU copy.
            auto it = palettes.find(lastPaletteContent);
            if (it == palettes.end())
                return nullptr;
            palColors = it->second.colors.data();
        }
        const uint32_t texaBits = texaClass == 0u ? 0u
                                                  : (st.texa.ta0 | (static_cast<uint32_t>(st.texa.ta1) << 8) |
                                                     (st.texa.aem ? 1u << 16 : 0u) | (texaClass << 20));
        // Colours weighted by alpha in the lower levels only where the
        // texture's alpha is used (TCC); else it may be anything.
        const bool weighted = tex.tcc != 0u;
        const uint64_t variant = texaBits | (weighted ? 1ull << 24 : 0ull);
        const uint64_t front =
            gm::fnvWord(gm::fnvWord(gm::fnvWord(gm::kFnvBasis, e.decodeSerial), indexed ? lastPaletteContent : 0u), variant);
        if (auto fit = mipCopyKeys.find(front); fit != mipCopyKeys.end())
        {
            auto cit = mipCopies.find(fit->second);
            if (cit != mipCopies.end())
            {
                cit->second.lastFrame = mipFrame;
                return &cit->second;
            }
            mipCopyKeys.erase(fit);
        }

        ps2x::ftrace::Scope traceMip(ps2x::ftrace::kMipCopy, ps2x::ftrace::kMipCopy + 100);
        // The GS's levels: lowest .. MXL of a mipmapped texture, else 0.
        const bool mipmapped = st.mip.maxLevel != 0u;
        const uint32_t first = mipmapped ? lowest : 0u;
        const uint32_t last = mipmapped ? st.mip.maxLevel : 0u;
        const uint32_t texW = std::max<uint32_t>(e.width, 1u), texH = std::max<uint32_t>(e.height, 1u);
        // The texels (indices or colours) as decodeTexture read them: still
        // in its pending upload when the texture was decoded for this draw
        // (the usual case, also when the game's cache flushes and uploads
        // it again every frame of a fast turn), else read again.
        const ReadFunc read = readFunc(tex.psm);
        const auto pending = [&](uint32_t level, size_t bytes) -> const uint8_t *
        {
            for (size_t i = uploads.size(), n = 0; i-- > 0 && n < 16u; ++n)
            {
                const PendingUpload &up = uploads[i];
                if (up.tex == e.tex && up.level == level && !up.src && up.data.size() == bytes)
                    return up.data.data();
            }
            return nullptr;
        };
        const auto overBudget = [&](size_t n)
        { return mipTexelsPerFrame != 0u && mipTexelsThisFrame > 0u && mipTexelsThisFrame + n > mipTexelsPerFrame; };
        size_t texels = 0;
        bool allPending = true;
        for (uint32_t k = first; k <= last; ++k)
        {
            const size_t n = static_cast<size_t>(std::max<uint32_t>(texW >> k, 1u)) * std::max<uint32_t>(texH >> k, 1u);
            texels += n;
            allPending = allPending && pending(k, indexed ? n : n * 4u);
        }
        if (!allPending)
        {
            // Reading GS memory again costs as much as a decode.
            if (overBudget(texels))
            {
                ++stats.mipCopyDeferred;
                return nullptr;
            }
            if (owedPages.intersects(e.pages))
                return nullptr;
            mipTexelsThisFrame += texels;
        }
        std::vector<gm::Level> levels;
        levels.reserve(gm::chainLength(texW >> first, texH >> first));
        uint64_t key = gm::fnvWord(gm::kFnvBasis, tex.psm | (static_cast<uint64_t>(texW) << 8) |
                                                      (static_cast<uint64_t>(texH) << 24) |
                                                      (static_cast<uint64_t>(first) << 40) | (static_cast<uint64_t>(last) << 48));
        key = gm::fnvWord(gm::fnvWord(key, variant), indexed ? lastPaletteContent : 0u);
        std::vector<uint8_t> indices;
        for (uint32_t k = first; k <= last; ++k)
        {
            const uint32_t lw = std::max<uint32_t>(texW >> k, 1u), lh = std::max<uint32_t>(texH >> k, 1u);
            const uint32_t tbp = mipmapped ? st.mip.tbp[k] : tex.tbp0;
            const uint32_t tbw = std::max<uint32_t>(mipmapped ? st.mip.tbw[k] : tex.tbw, 1u);
            gm::Level &l = levels.emplace_back();
            l.width = lw;
            l.height = lh;
            l.texels.resize(static_cast<size_t>(lw) * lh);
            if (indexed)
            {
                const uint8_t *src = pending(k, l.texels.size());
                if (!src)
                {
                    indices.resize(l.texels.size());
                    for (uint32_t y = 0; y < lh; ++y)
                        for (uint32_t x = 0; x < lw; ++x)
                            indices[static_cast<size_t>(y) * lw + x] = static_cast<uint8_t>(read(vram, tbp, tbw, x, y));
                    src = indices.data();
                }
                key = gm::fnvBytes64(key, src, l.texels.size());
                // Expanded below, once the content is known to be new.
                for (size_t i = 0; i < l.texels.size(); ++i)
                    l.texels[i] = src[i];
            }
            else if (const uint8_t *src = pending(k, l.texels.size() * 4u))
            {
                std::memcpy(l.texels.data(), src, l.texels.size() * 4u);
                key = gm::fnvBytes64(key, src, l.texels.size() * 4u);
            }
            else
            {
                const bool sixteen = is16BitPsm(tex.psm);
                for (uint32_t y = 0; y < lh; ++y)
                    for (uint32_t x = 0; x < lw; ++x)
                    {
                        uint32_t v = read(vram, tbp, tbw, x, y);
                        if (sixteen)
                            v = Rgba5551ToRgba8888(static_cast<u16>(v));
                        l.texels[static_cast<size_t>(y) * lw + x] = v;
                    }
                key = gm::fnvBytes64(key, l.texels.data(), l.texels.size() * 4u);
            }
        }
        if (mipCopyKeys.size() > 65536u)
            mipCopyKeys.clear();
        mipCopyKeys[front] = key;
        if (auto cit = mipCopies.find(key); cit != mipCopies.end())
        {
            cit->second.lastFrame = mipFrame;
            return &cit->second;
        }

        // A new picture: colours with TEXA per texel, then the chain (about
        // 4/3 of the texels).
        if (overBudget(texels * 4u / 3u))
        {
            ++stats.mipCopyDeferred;
            return nullptr;
        }
        if (indexed)
        {
            uint32_t pal[256];
            for (uint32_t i = 0; i < 256u; ++i)
                pal[i] = gm::applyTexa(palColors[i], texaClass, st.texa.ta0, st.texa.ta1, st.texa.aem);
            for (gm::Level &l : levels)
                for (uint32_t &t : l.texels)
                    t = pal[t & 0xFFu];
        }
        else if (texaClass != 0u)
        {
            for (gm::Level &l : levels)
                for (uint32_t &t : l.texels)
                    t = gm::applyTexa(t, texaClass, st.texa.ta0, st.texa.ta1, st.texa.aem);
        }
        gm::completeChain(levels, weighted);
        mipTexelsThisFrame += texels * 4u / 3u;
        SDL_GPUTexture *t = pooledTexture(levels[0].width, levels[0].height, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                          SDL_GPU_TEXTUREUSAGE_SAMPLER, static_cast<uint32_t>(levels.size()), kSiteMipCopy);
        if (!t)
            return nullptr;
        size_t bytes = 0;
        for (uint32_t k = 0; k < levels.size(); ++k)
        {
            PendingUpload up;
            up.tex = t;
            up.level = k;
            up.width = levels[k].width;
            up.height = levels[k].height;
            up.bytesPerPixel = 4u;
            up.data.resize(levels[k].texels.size() * 4u);
            std::memcpy(up.data.data(), levels[k].texels.data(), up.data.size());
            bytes += up.data.size();
            uploads.push_back(std::move(up));
        }
        if (traceReplace)
            std::cerr << "[gs-gpu] mip copy " << texW << "x" << texH << " psm " << unsigned(tex.psm) << " levels " << first
                      << ".." << last << " -> " << levels.size() << " filter " << (st.linearFilter ? "linear" : "nearest")
                      << (st.mip.maxLevel ? (st.mip.magLinear ? " mag linear" : " mag nearest") : "") << " clamp " << std::hex
                      << st.context.clamp << std::dec << " texa " << texaClass << " tcc " << unsigned(tex.tcc) << std::endl;
        MipCopy &m = mipCopies[key];
        m.tex = t;
        m.bytes = bytes;
        m.lastFrame = mipFrame;
        mipCopyGpuBytes += bytes;
        stats.mipCopyBytes += bytes;
        return &m;
    }

    // At each presented frame: the budget starts again; copies not drawn
    // for a while (another level's) go, the least recently drawn first
    // while over the memory cap.
    void mipCopiesFrame()
    {
        ++mipFrame;
        mipTexelsThisFrame = 0;
        if ((mipFrame & 63u) != 0u && mipCopyGpuBytes <= kMipCopyBytesMax)
            return;
        const auto drop = [&](std::unordered_map<uint64_t, MipCopy>::iterator it)
        {
            if (passOpen && it->second.passSerial == passSerial)
                closePass();
            giveBackTexture(it->second.tex);
            mipCopyGpuBytes -= std::min(mipCopyGpuBytes, it->second.bytes);
            return mipCopies.erase(it);
        };
        for (auto it = mipCopies.begin(); it != mipCopies.end();)
            it = it->second.lastFrame + kMipCopyIdleFrames < mipFrame ? drop(it) : std::next(it);
        while (mipCopyGpuBytes > kMipCopyBytesMax && !mipCopies.empty())
        {
            auto oldest = mipCopies.begin();
            for (auto it = mipCopies.begin(); it != mipCopies.end(); ++it)
                if (it->second.lastFrame < oldest->second.lastFrame)
                    oldest = it;
            drop(oldest);
        }
    }

    // Pages of a texture's TW x TH area (the last one is kept).
    GSTex0Reg lastPagesTex{};
    uint32_t lastPagesW = 0, lastPagesH = 0;
    GSMem::PageMask lastPages;
    const GSMem::PageMask &texturePages(const GSTex0Reg &tex, uint32_t w, uint32_t h)
    {
        if (lastPagesW != w || lastPagesH != h || lastPagesTex.tbp0 != tex.tbp0 || lastPagesTex.tbw != tex.tbw ||
            lastPagesTex.psm != tex.psm)
        {
            lastPages.clear();
            GSMem::AddRectPages(lastPages, tex.psm, tex.tbp0, std::max<uint32_t>(tex.tbw, 1u), 0, 0, w - 1u, h - 1u);
            lastPagesTex = tex;
            lastPagesW = w;
            lastPagesH = h;
        }
        return lastPages;
    }

    // A render target that holds this texture's texels, if any.
    Target *textureTarget(const GSTex0Reg &tex, const GSMem::PageMask &pages, float &dx, float &dy)
    {
        for (auto &tp : targets)
        {
            Target &t = *tp;
            if (t.depth || t.stale || !t.pages.intersects(pages))
                continue;
            if (storageClass(t.psm) != storageClass(tex.psm) || std::max<uint32_t>(tex.tbw, 1u) != t.bw)
                continue;
            if (tex.tbp0 < t.bp || ((tex.tbp0 - t.bp) % 32u) != 0u)
                continue;
            const uint32_t pageOffset = (tex.tbp0 - t.bp) / 32u;
            if (pageOffset % t.bw != 0u)
                continue;
            uint32_t pw, ph;
            pageExtent(t.psm, pw, ph);
            const uint32_t rowY = (pageOffset / t.bw) * ph;
            if (rowY >= t.height)
                continue;
            dx = 0.0f;
            dy = static_cast<float>(rowY);
            return &t;
        }
        return nullptr;
    }

    // ---- drawing ---------------------------------------------------------

    static SDL_GPUBlendFactor factorOf(bool fix, bool oneMinus)
    {
        if (fix)
            return oneMinus ? SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR : SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
        return oneMinus ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA : SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    }

    // Shadows sharp (gs_quality.h): a GS-path primitive of one colour that
    // adds into a wrapping sum (ABE with COLCLAMP=0) gets that colour exactly
    // (F_EXACT_COLOR). The GS draws a flat primitive in its provoking
    // vertex's colour; here the colour is interpolated like Gouraud and
    // truncated, and on long thin triangles the interpolation's float error
    // makes 248 into 247 and alpha 0x80 into 0x7F at some pixels. The skater
    // shadow (FUN_0010F1E8, VU1 0x1C) counts its volume in a CT16S buffer
    // with such triangles, a fan from the light through the silhouette:
    // front faces add 248 * As / 128 (minus 8, wrapping), back faces 8 * As /
    // 128, so a pixel one off in colour or alpha added 7 instead of 8 or took
    // 9 to 11, and the 5-bit count lost its place: the shadow came out in
    // fragments with the fan's edges showing (on the GPU renderer only, at
    // every scale). Rounding gives the constant back
    // exactly (the error is far below a half). Other primitives keep the
    // GS's truncation.
    static bool exactFlatColour(const GSPrimitiveBatch &batch)
    {
        const GSDrawState &st = batch.state;
        if (!st.prim.abe || (st.colclamp & 1u) != 0u || !ps2x::gs_quality::nativeShadows())
            return false;
        const uint8_t type = st.prim.type;
        if (type == GS_PRIM_SPRITE || type == GS_PRIM_POINT || !st.prim.iip)
            return true; // one colour by construction (submitPrimitive)
        const uint32_t n = (type == GS_PRIM_LINE || type == GS_PRIM_LINESTRIP) ? 2u : 3u;
        if (batch.vertexCount < n)
            return false;
        const GSVertex &a = batch.vertices[0];
        for (uint32_t i = 1; i < n; ++i)
        {
            const GSVertex &v = batch.vertices[i];
            if (v.r != a.r || v.g != a.g || v.b != a.b || v.a != a.a)
                return false;
        }
        return true;
    }

    // Maps ALPHA (A - B) * C + D onto the blend unit where it can: the
    // shader makes the source term, the hardware adds the destination term.
    BlendSetup blendSetup(const GSPrimitiveBatch &batch)
    {
        const GSDrawState &st = batch.state;
        const auto &ctx = st.context;
        BlendSetup b;
        if (!st.prim.abe)
            return b;
        const uint32_t A = ctx.alpha & 3u, B = (ctx.alpha >> 2) & 3u, C = (ctx.alpha >> 4) & 3u, D = (ctx.alpha >> 6) & 3u;
        const uint32_t fix = static_cast<uint32_t>((ctx.alpha >> 32) & 0xFFu);
        const bool colclamp = (st.colclamp & 1u) != 0u;
        if (st.pabe || C == 1u || C == 3u)
        {
            b.slow = true;
            return b;
        }
        const bool isFix = C == 2u;
        if (isFix && fix > 128u)
        {
            b.slow = true;
            return b;
        }
        b.constant = static_cast<float>(fix) / 128.0f;
        // 0 = Cs, 1 = Cd, 2 = 0 (3 behaves as 0 too)
        const uint32_t a = std::min(A, 2u), bb = std::min(B, 2u), d = std::min(D, 2u);
        auto set = [&](uint32_t premul, bool enable, SDL_GPUBlendFactor dst, SDL_GPUBlendOp op)
        {
            b.premul = premul;
            b.enable = enable;
            b.srcColor = SDL_GPU_BLENDFACTOR_ONE;
            b.dstColor = dst;
            b.op = op;
        };
        const SDL_GPUBlendFactor X = factorOf(isFix, false);
        const SDL_GPUBlendFactor oneMinusX = factorOf(isFix, true);
        bool dstTerm = true;
        if (a == bb)
        {
            if (d == 0u)
                set(0, false, SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDOP_ADD), dstTerm = false;
            else if (d == 1u)
                b.keepColor = true, dstTerm = false;
            else
                set(4, false, SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDOP_ADD), dstTerm = false;
        }
        else if (a == 0u && bb == 1u)
        {
            if (d == 1u) set(1, true, oneMinusX, SDL_GPU_BLENDOP_ADD);
            else if (d == 0u) set(2, true, X, SDL_GPU_BLENDOP_SUBTRACT);
            else set(1, true, X, SDL_GPU_BLENDOP_SUBTRACT);
        }
        else if (a == 1u && bb == 0u)
        {
            if (d == 1u) { b.slow = true; return b; }
            else if (d == 0u) set(3, true, X, SDL_GPU_BLENDOP_ADD);
            else set(1, true, X, SDL_GPU_BLENDOP_REVERSE_SUBTRACT);
        }
        else if (a == 0u && bb == 2u)
        {
            if (d == 1u) set(1, true, SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDOP_ADD);
            else if (d == 0u) set(2, false, SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDOP_ADD), dstTerm = false;
            else set(1, false, SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDOP_ADD), dstTerm = false;
        }
        else if (a == 2u && bb == 0u)
        {
            if (d == 1u) set(1, true, SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDOP_REVERSE_SUBTRACT);
            else if (d == 0u) set(3, false, SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDOP_ADD), dstTerm = false;
            else set(6, false, SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDOP_ADD), dstTerm = false;
        }
        else if (a == 1u && bb == 2u)
        {
            if (d == 1u) { b.slow = true; return b; }
            else if (d == 0u) set(0, true, X, SDL_GPU_BLENDOP_ADD);
            else set(4, true, X, SDL_GPU_BLENDOP_ADD);
        }
        else // a == 2, bb == 1
        {
            if (d == 1u) set(4, true, oneMinusX, SDL_GPU_BLENDOP_ADD);
            else if (d == 0u) set(0, true, X, SDL_GPU_BLENDOP_SUBTRACT);
            else set(4, true, X, SDL_GPU_BLENDOP_REVERSE_SUBTRACT);
        }

        const bool usesSrcAlpha = b.enable && (b.dstColor == SDL_GPU_BLENDFACTOR_SRC_ALPHA ||
                                               b.dstColor == SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA);
        if (usesSrcAlpha)
        {
            b.alphaOut = 1;
            if ((ctx.fba & 1u) != 0u)
            {
                b.slow = true;
                return b;
            }
        }

        if (!colclamp && dstTerm)
        {
            b.readsVertices = true;
            // Wrapping sums need the destination in the shader, except the
            // common "add a constant" (Cs * C + Cd) of a flat primitive:
            // a term of 128 or more is then subtracted as its negative,
            // exact as long as the sum itself does not wrap.
            const bool addConst = a == 0u && bb == 2u && d == 1u && !st.prim.tme;
            bool flat = addConst;
            const GSVertex &pv = batch.vertices[st.prim.type == GS_PRIM_SPRITE || st.prim.type == GS_PRIM_LINE || st.prim.type == GS_PRIM_LINESTRIP
                                                    ? 1
                                                    : (st.prim.type == GS_PRIM_POINT ? 0 : 2)];
            if (flat && st.prim.iip)
                for (uint32_t i = 0; i < batch.vertexCount; ++i)
                {
                    const GSVertex &v = batch.vertices[i];
                    if (v.r != pv.r || v.g != pv.g || v.b != pv.b || v.a != pv.a)
                        flat = false;
                }
            if (!flat || st.prim.fge)
            {
                b.slow = true;
                return b;
            }
            const uint32_t x = isFix ? fix : pv.a;
            const uint32_t vr = ((pv.r * x) >> 7) & 255u, vg = ((pv.g * x) >> 7) & 255u, vb = ((pv.b * x) >> 7) & 255u;
            const bool negR = vr >= 128u, negG = vg >= 128u, negB = vb >= 128u;
            if (negR != negG || negR != negB)
            {
                b.slow = true;
                return b;
            }
            b.premul = 5;
            b.op = negR ? SDL_GPU_BLENDOP_REVERSE_SUBTRACT : SDL_GPU_BLENDOP_ADD;
            b.wrapTrick = true;
        }
        return b;
    }

    // FBMSK as a per-channel write mask; false if it masks part of a channel.
    static bool channelMask(uint32_t fbmsk, uint8_t psm, uint8_t &enableMask)
    {
        enableMask = 0xF;
        if (fbmsk == 0u)
            return true;
        const bool sixteen = is16BitPsm(psm);
        const uint32_t rel[4] = {sixteen ? 0xF8u : 0xFFu, sixteen ? 0xF800u : 0xFF00u, sixteen ? 0xF80000u : 0xFF0000u,
                                 sixteen ? 0x80000000u : 0xFF000000u};
        for (int i = 0; i < 4; ++i)
        {
            const uint32_t m = fbmsk & rel[i];
            if (m == rel[i])
                enableMask &= static_cast<uint8_t>(~(1u << i));
            else if (m != 0u)
            {
                if (i == 3 && psm == GS_PSM_CT24)
                    continue;
                return false;
            }
        }
        return true;
    }

    struct Masks
    {
        bool rgb = true, alpha = true, depth = true;
    };

    // Lets a slow primitive join the open pass (one copy of the target
    // serves every primitive that does not overlap another one).
    bool openPassFor(Target *color, Target *depth, bool slow, bool feedback, const Rect &box)
    {
        if (passOpen && (pass.color != color || (depth && pass.depth != depth)))
            closePass();
        if (passOpen && feedback)
            closePass();
        if (passOpen && slow)
        {
            bool joins = pass.onlySlow && !pass.feedback;
            for (const Rect &r : pass.slowBoxes)
                if (r.overlaps(box))
                    joins = false;
            if (!joins)
                closePass();
        }
        if (!passOpen)
        {
            pass.reset();
            pass.color = color;
            pass.depth = depth;
            passOpen = true;
            ++passSerial;
        }
        if (slow)
        {
            pass.copyRegion.merge(box);
            pass.slowBoxes.push_back(box);
        }
        else
            pass.onlySlow = false;
        if (feedback)
        {
            pass.feedback = true;
            pass.onlySlow = false;
            Rect all{0, 0, static_cast<int>(color->width) - 1, static_cast<int>(color->height) - 1};
            pass.copyRegion.merge(all);
        }
        return true;
    }

    void addDraw(const DrawCall &dc, const GpuVertex *verts, uint32_t count)
    {
        const uint32_t first = static_cast<uint32_t>(pass.verts.size());
        pass.verts.insert(pass.verts.end(), verts, verts + count);
        if (!pass.draws.empty())
        {
            // The same draw state right after the last draw's vertices: they
            // become one draw.
            DrawCall &last = pass.draws.back();
            if (last.pipe == dc.pipe && last.first + last.count == first &&
                std::memcmp(&last.fu, &dc.fu, sizeof(dc.fu)) == 0 && std::memcmp(last.tex, dc.tex, sizeof(dc.tex)) == 0 &&
                last.sampler0 == dc.sampler0 && std::memcmp(&last.scissor, &dc.scissor, sizeof(dc.scissor)) == 0 &&
                std::memcmp(last.blend, dc.blend, sizeof(dc.blend)) == 0 && last.gs == gsTag)
            {
                last.count += count;
                return;
            }
        }
        DrawCall &d = pass.draws.emplace_back(dc);
        d.first = first;
        d.count = count;
        d.gs = gsTag;
    }

    // Draws on the CPU rasterizer, through local memory.
    void drawOnCpu(const GSPrimitiveBatch &batch)
    {
        ++drawMemoSerial;
        ps2x::ftrace::Scope traceCpu(ps2x::ftrace::kCpuDraw, ps2x::ftrace::kCpuDraw + 100);
        ++stats.cpuPrimitives;
        notReplayable("cpu raster");
        syncAll();
        materializeClut();
        cpu.Submit(batch);
        cpu.Sync(GSSyncReason::DebugReadback);
        const auto &ctx = batch.state.context;
        GSMem::PageMask pages;
        const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        GSMem::AddRectPages(pages, ctx.frame.psm, ctx.frame.fbp << 5, fbw, ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
        GSMem::AddRectPages(pages, ctx.zbuf.psm, ctx.zbuf.zbp << 5, fbw, ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1);
        markWritten(pages);
        markTargetsStale(pages);
    }

    // ---- the draw memo ---------------------------------------------------
    //
    // A mesh comes as primitives of one draw state (in BMX XXX's Bronx about
    // 40,000 primitives a frame in 600 draws), and drawPrepared takes the
    // same decisions for each: the same texture (fresh), palette, targets,
    // pass, blending, uniforms and pipelines. The memo keeps what the last
    // one emitted and gives it to the next primitives of the same state
    // while nothing else happened in the backend, with their own bookkeeping
    // (texture and target use clocks, content serials, dirty rectangles) as
    // drawPrepared does it. Taken from the second primitive of a run (the
    // first may fill caches: a decode, a palette, a target), and only where
    // drawPrepared's decisions are the draw state's alone: no shader
    // blending, no texture from a target, no replacement pictures, no
    // colours read by the blend setup, no dirty rectangle growing.
    // Anything else (any other call into the backend, a waiter let in, a
    // pass closed, a target destroyed or grown, a texture decoded) ends it.
    // PS2X_GS_DRAW_MEMO=0 turns it off; PS2X_GS_DRAW_MEMO_CHECK=1 draws every
    // primitive through drawPrepared and counts those whose draws differ
    // from the memo's (the proof run must say 0).
    struct DrawMemo
    {
        bool valid = false;
        uint64_t serial = 0;      // drawMemoSerial when taken
        uint64_t passSerial = 0;
        uint64_t targetsDestroyed = 0;
        GSDrawState state{};
        uint32_t coordMode = 0;
        bool line = false;
        Rect scissor;             // the context's scissor
        uint32_t fbw = 0;
        Target *color = nullptr;
        Target *depth = nullptr;
        bool depthLookedUp = false; // needDepth: getTarget moved its use clock
        Target *passDepth = nullptr;
        TexEntry *entry = nullptr;
        SDL_GPUTexture *entryTex = nullptr;
        bool wroteColor = false, wroteDepth = false;
        bool eligible = false;    // what recordDrawMemo found
        uint32_t drawCount = 0;
        DrawCall draws[2];
    };
    DrawMemo drawMemo;
    // The last primitive that went through drawPrepared: a second one of the
    // same state in a row takes the memo.
    struct
    {
        bool valid = false;
        uint64_t serial = 0;
        uint32_t coordMode = 0;
        bool line = false;
        GSDrawState state{};
    } memoCandidate;
    uint64_t drawMemoSerial = 1; // moved by every call into the backend but a primitive in a packet
    bool drawMemoOn = true;
    bool shaderDepth = true; // PS2X_GS_SHADER_DEPTH=0: see drawPrepared
    bool drawMemoCheck = false;
    uint64_t memoHits = 0;
    uint64_t memoChecked = 0, memoDiffer = 0;

    static bool sameDrawState(const GSDrawState &a, const GSDrawState &b)
    {
        // Byte for byte: padding that differs only costs a memo miss.
        return std::memcmp(&a, &b, sizeof(GSDrawState)) == 0;
    }

    static bool sameDraw(const DrawCall &a, const DrawCall &b)
    {
        return a.pipe == b.pipe && a.nativeBuf == b.nativeBuf && a.nativeOffset == b.nativeOffset && a.skinOffset == b.skinOffset &&
               a.skin == b.skin && std::memcmp(&a.nu, &b.nu, sizeof(a.nu)) == 0 && std::memcmp(&a.fu, &b.fu, sizeof(a.fu)) == 0 &&
               std::memcmp(a.tex, b.tex, sizeof(a.tex)) == 0 && a.sampler0 == b.sampler0 && a.scissor.x == b.scissor.x &&
               a.scissor.y == b.scissor.y && a.scissor.w == b.scissor.w && a.scissor.h == b.scissor.h &&
               std::memcmp(a.blend, b.blend, sizeof(a.blend)) == 0;
    }

    // drawPrepared's decisions for a primitive, for the memo (see above).
    void recordDrawMemo(DrawMemo &m, const GSPrimitiveBatch &batch, uint32_t coordMode, bool line, const Rect &sc, uint32_t fbw,
                        Target *color, Target *depth, bool needDepth, TexEntry *entry, bool grew, bool wroteColor,
                        bool wroteDepth, const BlendSetup &blend, bool otherTexture)
    {
        m.eligible = false;
        if (grew || blend.slow || blend.readsVertices || blend.wrapTrick || otherTexture || !objTags.empty() || !passOpen ||
            pass.color != color || (needDepth && pass.depth != depth) || color->stale || (depth && depth->stale))
            return;
        if (entry && (entry->gathered || entry->tex == nullptr || entry->checkedEpoch != epoch ||
                      (replBc2 && (contentKeys || entry->rgbHashed))))
            return;
        std::memcpy(&m.state, &batch.state, sizeof(GSDrawState)); // (padding too: sameDrawState compares bytes)
        m.coordMode = coordMode;
        m.line = line;
        m.scissor = sc;
        m.fbw = fbw;
        m.color = color;
        m.depth = depth;
        m.depthLookedUp = needDepth;
        m.passDepth = pass.depth;
        m.entry = entry;
        m.entryTex = entry ? entry->tex : nullptr;
        m.wroteColor = wroteColor;
        m.wroteDepth = wroteDepth;
        m.passSerial = passSerial;
        m.targetsDestroyed = targetsDestroyed;
        m.eligible = true;
    }

    // Whether the memo holds this primitive's draw state, with nothing else
    // done in the backend since it was taken.
    bool memoHolds(const GSPrimitiveBatch &batch, uint32_t coordMode, bool line) const
    {
        const DrawMemo &m = drawMemo;
        return m.valid && m.serial == drawMemoSerial && m.passSerial == passSerial && passOpen && pass.color == m.color &&
               pass.depth == m.passDepth && m.targetsDestroyed == targetsDestroyed && m.coordMode == coordMode && m.line == line &&
               (!m.entry || (m.entry->tex == m.entryTex && m.entry->checkedEpoch == epoch)) && !m.color->stale &&
               (!m.depth || !m.depth->stale) && sameDrawState(m.state, batch.state);
    }

    // drawPrepared's box steps for the memo's draw: false when the primitive
    // needs more (its colour target grows); else box is clipped and `empty`
    // says whether drawPrepared would have drawn nothing.
    bool memoBox(Rect &box, bool &empty) const
    {
        const DrawMemo &m = drawMemo;
        const Rect &sc = m.scissor;
        box.x0 = std::max(box.x0, sc.x0);
        box.y0 = std::max(box.y0, sc.y0);
        box.x1 = std::min(box.x1, sc.x1);
        box.y1 = std::min(box.y1, sc.y1);
        empty = box.empty();
        if (empty)
            return true;
        const uint32_t needH = static_cast<uint32_t>(sc.y1 < 1024 ? std::max(box.y1, sc.y1) : box.y1) + 1u;
        if (static_cast<uint32_t>(box.x1) >= m.fbw * 64u)
            box.x1 = static_cast<int>(m.fbw * 64u) - 1;
        empty = box.empty();
        if (empty)
            return true;
        return std::min<uint32_t>((needH + 31u) & ~31u, kMaxTargetHeight) <= m.color->height;
    }

    // The memo's draws for this primitive (its own exact-colour flag).
    uint32_t memoDraws(const GSPrimitiveBatch &batch, DrawCall *out) const
    {
        const bool exact = exactFlatColour(batch);
        for (uint32_t i = 0; i < drawMemo.drawCount; ++i)
        {
            out[i] = drawMemo.draws[i];
            out[i].fu.cfg0[0] = (out[i].fu.cfg0[0] & ~0x40000u) | (exact ? 0x40000u : 0u); // F_EXACT_COLOR
        }
        return drawMemo.drawCount;
    }

    void noteMemoCheck(bool same)
    {
        ++memoChecked;
        if (!same && ++memoDiffer <= 5u)
            std::cerr << "[gs-gpu] draw memo check: primitive " << stats.primitives << " differs from the memo" << std::endl;
        if (memoChecked % 1000000u == 0u)
            std::cerr << "[gs-gpu] draw memo check: " << memoChecked << " primitives checked, " << memoDiffer << " differ" << std::endl;
    }

    // The memo's draw, with drawPrepared's bookkeeping in its order.
    void drawFromMemo(const GSPrimitiveBatch &batch, const Rect &box, const GpuVertex *v, uint32_t vcount)
    {
        DrawMemo &m = drawMemo;
        ++memoHits;
        if (m.entry)
            m.entry->lastUse = ++useClock;
        m.color->lastUse = ++useClock;
        if (m.depthLookedUp)
            m.depth->lastUse = ++useClock;
        DrawCall dcs[2];
        const uint32_t n = memoDraws(batch, dcs);
        for (uint32_t i = 0; i < n; ++i)
            addDraw(dcs[i], v, vcount);
        bool grew = false;
        if (m.wroteColor)
            grew |= noteDrawn(m.color, box);
        if (m.wroteDepth && m.depth)
            grew |= noteDrawn(m.depth, box);
        if (grew)
            m.valid = false; // other targets may be stale now
    }

    // A GS primitive (Submit). A game's fog planes are held back until the
    // set is complete (finishFogPlanes); anything else draws now.
    void submitPrimitive(const GSPrimitiveBatch &batch)
    {
        if (!fogNotes.empty() || !fogHeld.empty())
            ++drawMemoSerial;
        if (!fogNotes.empty() && holdFogPlane(batch))
            return;
        if (!fogHeld.empty())
        {
            releaseFogPlanes();
            ++drawMemoSerial;
        }
        submitPrimitiveNow(batch);
    }

    void submitPrimitiveNow(const GSPrimitiveBatch &batch)
    {
        const GSDrawState &st = batch.state;
        const auto &ctx = st.context;
        ++stats.primitives;
        traceReason = "draw";

        const uint8_t fpsm = ctx.frame.psm;
        if (!isColorPsm(fpsm))
        {
            drawOnCpu(batch);
            return;
        }
        if (((ctx.test >> 17) & 3u) == 0u)
            return; // Z test NEVER

        // Geometry in target pixels.
        const float ofx = static_cast<float>(ctx.xyoffset.ofx >> 4);
        const float ofy = static_cast<float>(ctx.xyoffset.ofy >> 4);
        const bool sprite = st.prim.type == GS_PRIM_SPRITE;
        const bool line = st.prim.type == GS_PRIM_LINE || st.prim.type == GS_PRIM_LINESTRIP;
        const bool point = st.prim.type == GS_PRIM_POINT;
        const bool tri = !sprite && !line && !point;
        if (tri && batch.vertexCount < 3u)
            return;
        if ((sprite || line) && batch.vertexCount < 2u)
            return;

        const uint8_t zpsm = ctx.zbuf.psm;
        const uint32_t zbitsN = depthBits(zpsm);
        // 2^bits, and its exact reciprocal: z * zInv is z / zScale.
        const double zScale = static_cast<double>(1ull << zbitsN);
        const double zInv = hwDepth16(zpsm) ? 1.0 / 65535.0 : 1.0 / zScale;
        // Frame interpolation: a 3D primitive moves with the camera in a picture between frames: Z tested GEQUAL
        // or GREATER, not a sprite, a point or a fog plane, and in perspective (its Z varies, or its texture's Q
        // is not 1). 2D drawn as triangles at one Z (the HUD, text) stays where it is.
        struct TagScope
        {
            GsTag &tag;
            ~TagScope() { tag = GsTag{}; }
        } tagScope{gsTag};
        auto inPerspective = [&]()
        {
            const uint32_t n = line ? 2u : 3u;
            const GSVertex &v0 = batch.vertices[0];
            for (uint32_t i = 1; i < n && i < batch.vertexCount; ++i)
                if (batch.vertices[i].z != v0.z)
                    return true;
            if (st.prim.tme && !st.prim.fst)
                for (uint32_t i = 0; i < n && i < batch.vertexCount; ++i)
                    if (std::fabs(batch.vertices[i].q - 1.0f) > 1e-6f)
                        return true;
            return false;
        };
        if (interp.on && !sprite && !point && ((ctx.test >> 16) & 1u) != 0u && ((ctx.test >> 17) & 3u) >= 2u &&
            isDepthPsm(zpsm) && !hwDepth16(zpsm) && !fogPlaneTriangle(batch) && inPerspective())
        {
            gsTag.reproject = 1;
            gsTag.stq = st.prim.tme && !st.prim.fst ? 1 : 0;
            gsTag.ofx = ofx;
            gsTag.ofy = ofy;
            gsTag.zScale = static_cast<float>(zScale);
            gsTag.scissor = SDL_Rect{static_cast<int>(ctx.scissor.x0), static_cast<int>(ctx.scissor.y0),
                                     static_cast<int>(ctx.scissor.x1), static_cast<int>(ctx.scissor.y1)};
        }
        const uint32_t texW = st.textureWidth, texH = st.textureHeight;

        GpuVertex v[6];
        uint32_t vcount = 0;
        Rect box;
        auto colorOf = [&](const GSVertex &s, float *out)
        {
            out[0] = s.r;
            out[1] = s.g;
            out[2] = s.b;
            out[3] = s.a;
        };
        uint32_t coordMode = 0;
        if (sprite || point)
        {
            const GSVertex &a = batch.vertices[0];
            const GSVertex &b = point ? batch.vertices[0] : batch.vertices[1];
            int x0 = static_cast<int>(a.x) - static_cast<int>(ofx);
            int y0 = static_cast<int>(a.y) - static_cast<int>(ofy);
            int x1 = static_cast<int>(b.x) - static_cast<int>(ofx);
            int y1 = static_cast<int>(b.y) - static_cast<int>(ofy);
            if (point)
            {
                x1 = x0 + 1;
                y1 = y0 + 1;
            }
            if (x0 > x1)
                std::swap(x0, x1);
            if (y0 > y1)
                std::swap(y0, y1);
            const int spanX = std::max(1, x1 - x0), spanY = std::max(1, y1 - y0);
            x1 = x0 + spanX;
            y1 = y0 + spanY;
            box = {x0, y0, x1 - 1, y1 - 1};
            float t0[2] = {0, 0}, t1[2] = {0, 0};
            if (st.prim.tme)
            {
                if (st.prim.fst)
                {
                    t0[0] = static_cast<float>(a.u >> 4);
                    t0[1] = static_cast<float>(a.v >> 4);
                    t1[0] = static_cast<float>(b.u >> 4);
                    t1[1] = static_cast<float>(b.v >> 4);
                    coordMode = 2;
                }
                else
                {
                    const float q0 = fabsQ(a.q), q1 = fabsQ(b.q);
                    t0[0] = (a.s / q0) * static_cast<float>(texW);
                    t0[1] = (a.t / q0) * static_cast<float>(texH);
                    t1[0] = (b.s / q1) * static_cast<float>(texW);
                    t1[1] = (b.t / q1) * static_cast<float>(texH);
                    coordMode = 3;
                }
            }
            const float z = static_cast<float>(static_cast<double>(static_cast<uint32_t>(b.z)) * zInv);
            const float corners[4][4] = {{static_cast<float>(x0), static_cast<float>(y0), t0[0], t0[1]},
                                         {static_cast<float>(x1), static_cast<float>(y0), t1[0], t0[1]},
                                         {static_cast<float>(x0), static_cast<float>(y1), t0[0], t1[1]},
                                         {static_cast<float>(x1), static_cast<float>(y1), t1[0], t1[1]}};
            const int order[6] = {0, 1, 2, 2, 1, 3};
            for (int i = 0; i < 6; ++i)
            {
                const float *c = corners[order[i]];
                GpuVertex &o = v[vcount++];
                o.pos[0] = c[0];
                o.pos[1] = c[1];
                o.pos[2] = z;
                o.pos[3] = 0;
                colorOf(b, o.color);
                o.tex[0] = c[2];
                o.tex[1] = c[3];
                // The texture coordinates are final; a mipmapped sprite's
                // LOD uses the Q of its second vertex.
                o.tex[2] = st.mip.maxLevel != 0u ? b.q : 1.0f;
                o.tex[3] = b.fog;
            }
        }
        else
        {
            const uint32_t n = line ? 2u : 3u;
            const GSVertex &pv = batch.vertices[n - 1u];
            float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
            for (uint32_t i = 0; i < n; ++i)
            {
                const GSVertex &s = batch.vertices[i];
                GpuVertex &o = v[vcount++];
                float x = s.x - ofx, y = s.y - ofy;
                if (line)
                {
                    x = std::floor(s.x) - ofx + 0.5f;
                    y = std::floor(s.y) - ofy + 0.5f;
                }
                o.pos[0] = x;
                o.pos[1] = y;
                o.pos[2] = static_cast<float>(s.z * zInv);
                o.pos[3] = 0;
                colorOf(st.prim.iip ? s : pv, o.color);
                if (st.prim.fst)
                {
                    o.tex[0] = s.u;
                    o.tex[1] = s.v;
                    o.tex[2] = st.mip.maxLevel != 0u ? s.q : 1.0f; // mipmap LOD only
                    coordMode = 1;
                }
                else
                {
                    o.tex[0] = s.s;
                    o.tex[1] = s.t;
                    o.tex[2] = s.q;
                    coordMode = 0;
                }
                o.tex[3] = s.fog;
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
            box = {static_cast<int>(std::floor(minX)), static_cast<int>(std::floor(minY)),
                   static_cast<int>(std::ceil(maxX)), static_cast<int>(std::ceil(maxY))};
        }
        // Texel range of the primitive (for filtering at higher resolution).
        if (st.prim.tme)
        {
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
            for (uint32_t i = 0; i < vcount; ++i)
            {
                float uvt[2];
                if (coordMode == 0u)
                {
                    const float q = fabsQ(v[i].tex[2]);
                    uvt[0] = v[i].tex[0] / q * static_cast<float>(texW);
                    uvt[1] = v[i].tex[1] / q * static_cast<float>(texH);
                }
                else if (coordMode == 1u)
                {
                    uvt[0] = std::floor(v[i].tex[0]) / 16.0f;
                    uvt[1] = std::floor(v[i].tex[1]) / 16.0f;
                }
                else
                {
                    uvt[0] = v[i].tex[0];
                    uvt[1] = v[i].tex[1];
                }
                for (int c = 0; c < 2; ++c)
                {
                    lo[c] = std::min(lo[c], uvt[c]);
                    hi[c] = std::max(hi[c], uvt[c]);
                }
            }
            for (uint32_t i = 0; i < vcount; ++i)
            {
                v[i].uvBounds[0] = lo[0];
                v[i].uvBounds[1] = lo[1];
                v[i].uvBounds[2] = hi[0];
                v[i].uvBounds[3] = hi[1];
            }
        }
        else
            for (uint32_t i = 0; i < vcount; ++i)
                v[i].uvBounds[0] = v[i].uvBounds[1] = v[i].uvBounds[2] = v[i].uvBounds[3] = 0.0f;
        // Snapped corners are whole pixels: the box is the pixels they enclose.
        if (tri && snapRectangleHalf(v, st.prim.fst != 0u))
            box = {static_cast<int>(std::min({v[0].pos[0], v[1].pos[0], v[2].pos[0]})),
                   static_cast<int>(std::min({v[0].pos[1], v[1].pos[1], v[2].pos[1]})),
                   static_cast<int>(std::max({v[0].pos[0], v[1].pos[0], v[2].pos[0]})) - 1,
                   static_cast<int>(std::max({v[0].pos[1], v[1].pos[1], v[2].pos[1]})) - 1};

        if (!drawMemoOn || trace || !fogNotes.empty() || !fogHeld.empty())
        {
            drawPrepared(batch, box, coordMode, line, [&](const DrawCall &dc)
                         { addDraw(dc, v, vcount); });
            return;
        }
        Rect memoRect = box;
        bool memoEmpty = false;
        const bool usable = memoHolds(batch, coordMode, line) && memoBox(memoRect, memoEmpty);
        if (usable && !drawMemoCheck)
        {
            if (!memoEmpty)
                drawFromMemo(batch, memoRect, v, vcount);
            return; // (empty: drawPrepared draws nothing either)
        }
        if (usable && memoEmpty)
        {
            // The check: drawPrepared draws nothing and changes nothing; the
            // memo stands, as without the check.
            const DrawMemo kept = drawMemo;
            const uint64_t useBefore = useClock, contentBefore = contentClock;
            const size_t vertsBefore = pass.verts.size();
            drawPrepared(batch, box, coordMode, line, [&](const DrawCall &dc)
                         { addDraw(dc, v, vcount); });
            noteMemoCheck(useClock == useBefore && contentClock == contentBefore && passOpen && pass.verts.size() == vertsBefore);
            drawMemo = kept;
            return;
        }
        // Through drawPrepared; a second primitive of the same state in a
        // row takes the memo when its call changed nothing but the clocks.
        const bool checking = usable && drawMemoCheck && !memoEmpty;
        DrawCall predicted[2];
        const uint32_t predictedCount = checking ? memoDraws(batch, predicted) : 0u;
        const uint64_t predictedUses = (drawMemo.entry ? 1u : 0u) + 1u + (drawMemo.depthLookedUp ? 1u : 0u);
        const uint64_t predictedContents = (drawMemo.wroteColor ? 1u : 0u) + (drawMemo.wroteDepth && drawMemo.depth ? 1u : 0u);
        const bool second = memoCandidate.valid && memoCandidate.serial == drawMemoSerial &&
                            memoCandidate.coordMode == coordMode && memoCandidate.line == line &&
                            sameDrawState(memoCandidate.state, batch.state);
        drawMemo.valid = false;
        DrawMemo &rec = drawMemo;
        rec.eligible = false;
        rec.drawCount = 0;
        const uint64_t useBefore = useClock, contentBefore = contentClock, passBefore = passSerial;
        const uint64_t destroyedBefore = targetsDestroyed, serialBefore = drawMemoSerial;
        const size_t uploadsBefore = uploads.size();
        bool overflow = false;
        drawPrepared(batch, box, coordMode, line, [&](const DrawCall &dc)
                     {
                         if (rec.drawCount < 2u)
                             rec.draws[rec.drawCount++] = dc;
                         else
                             overflow = true;
                         addDraw(dc, v, vcount); },
                     0, &rec);
        if (checking)
        {
            bool same = rec.drawCount == predictedCount && !overflow && useClock - useBefore == predictedUses &&
                        contentClock - contentBefore == predictedContents;
            for (uint32_t i = 0; same && i < predictedCount; ++i)
                same = sameDraw(predicted[i], rec.draws[i]);
            noteMemoCheck(same);
        }
        const uint64_t uses = (rec.entry ? 1u : 0u) + 1u + (rec.depthLookedUp ? 1u : 0u);
        const uint64_t contents = (rec.wroteColor ? 1u : 0u) + (rec.wroteDepth && rec.depth ? 1u : 0u);
        rec.valid = second && rec.eligible && !overflow && drawMemoSerial == serialBefore && passSerial == passBefore &&
                    targetsDestroyed == destroyedBefore && uploads.size() == uploadsBefore && useClock - useBefore == uses &&
                    contentClock - contentBefore == contents;
        rec.serial = drawMemoSerial;
        memoCandidate.valid = true;
        memoCandidate.serial = drawMemoSerial;
        memoCandidate.coordMode = coordMode;
        memoCandidate.line = line;
        std::memcpy(&memoCandidate.state, &batch.state, sizeof(GSDrawState));
    }

    // The GS state of a draw (blending, tests, texture, targets, uniforms,
    // pipelines) for geometry in `box` (target pixels), emitted by `emit`
    // once per pipeline pass. native: a native draw (SubmitNative), which
    // declines (false) what needs the destination in the shader or the
    // CPU rasterizer (its triangles then come as primitives).
    template <class Emit>
    bool drawPrepared(const GSPrimitiveBatch &batch, Rect box, uint32_t coordMode, bool line, Emit &&emit,
                      uint8_t native = 0, DrawMemo *record = nullptr)
    {
        const GSDrawState &st = batch.state;
        const auto &ctx = st.context;
        const uint8_t fpsm = ctx.frame.psm;
        const uint8_t zpsm = ctx.zbuf.psm;
        const uint32_t zbitsN = depthBits(zpsm);
        const double zScale = std::ldexp(1.0, static_cast<int>(zbitsN));
        const uint32_t texW = st.textureWidth, texH = st.textureHeight;

        // Alpha test classes (TEST.AFAIL).
        const uint64_t test = ctx.test;
        const bool ate = (test & 1u) != 0u;
        const uint32_t atst = static_cast<uint32_t>((test >> 1) & 7u);
        const uint32_t afail = static_cast<uint32_t>((test >> 12) & 3u);
        const uint32_t ztst = static_cast<uint32_t>((test >> 17) & 3u);
        if (ztst == 0u)
            return true; // NEVER
        auto failMasks = [&]() -> Masks
        {
            switch (afail)
            {
            case 1: return {true, true, false};
            case 2: return {false, false, true};
            case 3: return fpsm == GS_PSM_CT32 ? Masks{true, false, false} : Masks{true, true, false};
            default: return {false, false, false};
            }
        };
        struct PassSpec
        {
            uint32_t mode;
            Masks masks;
        };
        PassSpec passes[2];
        int passCount = 0;
        if (!ate || atst == 1u)
            passes[passCount++] = {0u, Masks{}};
        else if (atst == 0u)
            passes[passCount++] = {0u, failMasks()};
        else
        {
            passes[passCount++] = {1u, Masks{}};
            const Masks f = failMasks();
            if (f.rgb || f.alpha || f.depth)
                passes[passCount++] = {2u, f};
        }


        // Clip to the scissor.
        Rect sc{ctx.scissor.x0, ctx.scissor.y0, ctx.scissor.x1, ctx.scissor.y1};
        box.x0 = std::max(box.x0, sc.x0);
        box.y0 = std::max(box.y0, sc.y0);
        box.x1 = std::min(box.x1, sc.x1);
        box.y1 = std::min(box.y1, sc.y1);
        if (box.empty())
            return true;

        // Blending, masks.
        BlendSetup blend = blendSetup(batch);
        uint8_t fbEnable = 0xF;
        if (!channelMask(ctx.frame.fbmsk, fpsm, fbEnable))
            blend.slow = true;
        const bool date = ((test >> 14) & 1u) != 0u;
        if (date && fpsm != GS_PSM_CT24)
            blend.slow = true;
        // A native draw is many primitives that may overlap one another:
        // shader blending reads one copy of the target taken before them.
        if (native && (blend.slow || blend.wrapTrick))
            return false;

        // Targets.
        const uint32_t fbw = std::max<uint32_t>(ctx.frame.fbw, 1u);
        // Height: the scissor's (usually the buffer's), else what is drawn.
        const uint32_t needH = static_cast<uint32_t>(sc.y1 < 1024 ? std::max(box.y1, sc.y1) : box.y1) + 1u;
        if (static_cast<uint32_t>(box.x1) >= fbw * 64u)
            box.x1 = static_cast<int>(fbw * 64u) - 1;
        if (box.empty())
            return true;
        bool zWriteAny = false;
        for (int i = 0; i < passCount; ++i)
            zWriteAny |= passes[i].masks.depth && !ctx.zbuf.zmask;
        const bool needDepth = ztst != 1u || zWriteAny;
        if (needDepth && !isDepthPsm(zpsm))
        {
            if (native)
                return false;
            drawOnCpu(batch);
            return true;
        }

        // Texture.
        SDL_GPUTexture *texBind[4] = {dummyRgba, dummyIndex, dummyRgba, dummyRgba};
        uint32_t texKind = 0, texaClass = 0, src16 = 0;
        float rttOff[2] = {0, 0};
        Target *rtt = nullptr;
        TexEntry *entry = nullptr;
        ReplTex *replTex = nullptr;
        SDL_GPUSampler *sampler0 = nullptr;
        uint32_t mipLowest = 0;
        // A texture pack's picture (texture_pack::Region: the part it covers).
        bool packPicture = false;
        ps2x::texture_pack::Region packRegion;
        const bool packs = st.prim.tme && ps2x::texture_pack::active();
        const auto usePack = [&](ReplTex *r)
        {
            replTex = r;
            packPicture = true;
            texKind = 3;
            texBind[0] = r->tex;
            sampler0 = replacementSampler(ctx.clamp, texW, texH);
            ++stats.replacedPrimitives;
        };
        const bool namedIndexed = st.prim.tme && !objTags.empty() &&
                                  (isFourBitIndexedPsm(ctx.tex0.psm) || isEightBitIndexedPsm(ctx.tex0.psm));
        // A texture the game named (GSTextureObjectNote): its host picture,
        // without decoding what GS memory holds. Under a texture pack's
        // picture of the same texture (tried after the pack's, then).
        const auto useObject = [&]
        {
            if (ObjTag *t = objectFor(ctx.tex0, texW, texH, st.mip, st.mip.maxLevel != 0u))
            {
                SDL_GPUTexture *pal = palette(ctx.tex0, texaClass);
                replTex = replacementForObject(*t, lastPaletteContent, ctx.clamp, texW, texH);
                if (replTex)
                {
                    texKind = 3;
                    texBind[0] = replTex->tex;
                    texBind[2] = pal;
                    sampler0 = replacementSampler(ctx.clamp, texW, texH);
                    ++stats.replacedPrimitives;
                    ++stats.replacedByObject;
                }
            }
        };
        if (namedIndexed && !packs)
            useObject();
        if (!replTex && st.prim.tme && st.mip.maxLevel != 0u)
        {
            // Mipmapped: every level decoded from local memory.
            const GSTex0Reg &tex = ctx.tex0;
            TexEntry &e = decodeMipTexture(tex, st.mip, texW, texH, mipLowest);
            entry = &e;
            texaClass = tex.psm == GS_PSM_CT24 ? 1u : (is16BitPsm(tex.psm) ? 2u : 0u);
            if (e.indexed)
            {
                texKind = 1;
                texBind[1] = e.tex;
                texBind[2] = palette(tex, texaClass);
            }
            else
            {
                texKind = 0;
                texBind[0] = e.tex;
            }
            if (packs)
                if (ReplTex *r = packReplacement(e, tex, st, true, packRegion))
                    usePack(r);
        }
        else if (!replTex && st.prim.tme)
        {
            const GSTex0Reg &tex = ctx.tex0;
            const GSMem::PageMask &tpages = texturePages(tex, texW, texH);
            rtt = textureTarget(tex, tpages, rttOff[0], rttOff[1]);
            if (rtt)
            {
                texKind = 2;
                src16 = is16BitPsm(rtt->psm) ? 1u : 0u;
                texaClass = tex.psm == GS_PSM_CT24 ? 1u : (is16BitPsm(tex.psm) ? 2u : 0u);
            }
            else
            {
                const bool indexed = isFourBitIndexedPsm(tex.psm) || isEightBitIndexedPsm(tex.psm);
                TexEntry &e = decodeTexture(tex, texW, texH);
                entry = &e;
                if (indexed)
                {
                    texKind = 1;
                    texBind[1] = e.tex;
                    texBind[2] = palette(tex, texaClass);
                    if (packs)
                        if (ReplTex *r = packReplacement(e, tex, st, false, packRegion))
                            usePack(r);
                    if (!replTex)
                        replTex = replacementFor(e, lastPaletteContent, ctx.clamp);
                    if (replTex && !packPicture)
                    {
                        texKind = 3;
                        texBind[0] = replTex->tex;
                        const uint64_t wms = ctx.clamp & 3u, wmt = (ctx.clamp >> 2) & 3u;
                        sampler0 = replSampler[wms == 1u || wms == 2u][wmt == 1u || wmt == 2u];
                        ++stats.replacedPrimitives;
                    }
                }
                else
                {
                    texKind = 0;
                    texBind[0] = e.tex;
                    texaClass = tex.psm == GS_PSM_CT24 ? 1u : (is16BitPsm(tex.psm) ? 2u : 0u);
                    if (packs)
                        if (ReplTex *r = packReplacement(e, tex, st, false, packRegion))
                            usePack(r);
                    if (!replTex)
                        replTex = e.rgbHashed ? replacementFor(e, 0u, ctx.clamp) : nullptr;
                    if (replTex && !packPicture)
                    {
                        texKind = 3;
                        texBind[0] = replTex->tex;
                        const uint64_t wms = ctx.clamp & 3u, wmt = (ctx.clamp >> 2) & 3u;
                        sampler0 = replSampler[wms == 1u || wms == 2u][wmt == 1u || wmt == 2u];
                        ++stats.replacedPrimitives;
                    }
                }
            }
        }
        if (namedIndexed && packs && !replTex && !rtt)
            useObject();

        // Native draws: texture filtering as the picture quality option says.
        MipCopy *mipCopy = native ? nativeFiltering(st, entry, mipLowest, texKind, texaClass, texBind, sampler0) : nullptr;

        // The frame and Z buffer lookups below can grow (destroy and remake)
        // the target rtt points to, when the draw reads the buffer it draws
        // to: rtt is looked up again then.
        const uint64_t destroyedBefore = targetsDestroyed;
        Target *color = getTarget(ctx.frame.fbp << 5, fbw, fpsm, needH);
        Target *depth = nullptr;
        if (needDepth)
        {
            depth = getTarget(ctx.zbuf.zbp << 5, fbw, zpsm, color->height);
            // Same size for both attachments.
            if (depth->height > color->height)
                shrinkTarget(*depth, color->height);
            else if (depth->height < color->height)
                color = getTarget(ctx.frame.fbp << 5, fbw, fpsm, depth->height);
        }
        else
        {
            // Keep the pass going if it already has this Z buffer.
            Target *z = isDepthPsm(zpsm) ? findTarget(ctx.zbuf.zbp << 5, fbw, zpsm) : nullptr;
            if (z && passOpen && pass.color == color && pass.depth == z)
                depth = z;
        }
        if (rtt && targetsDestroyed != destroyedBefore)
        {
            rtt = textureTarget(ctx.tex0, texturePages(ctx.tex0, texW, texH), rttOff[0], rttOff[1]);
            if (!rtt)
                return true; // not seen: the grown target covers the same pages
            src16 = is16BitPsm(rtt->psm) ? 1u : 0u;
        }
        if (!color || !color->tex || (depth && !depth->tex))
            return true;
        const bool feedback = rtt == color;
        if (native && feedback)
            return false;
        if (rtt)
            texBind[0] = feedback ? copyTexture(color->width * scaleX, color->height * scaleY) : rtt->tex;

        if (blend.slow)
            ++stats.slowPrimitives;
        openPassFor(color, depth, blend.slow, feedback, box);
        if (depth && !pass.depth)
            pass.depth = depth;
        if (blend.slow)
            texBind[3] = copyTexture(color->width * scaleX, color->height * scaleY);
        if (entry)
            entry->passSerial = passSerial;
        if (replTex)
            replTex->passSerial = passSerial;
        if (mipCopy)
            mipCopy->passSerial = passSerial;

        // Uniforms.
        DrawCall dc{};
        GpuFragmentUniforms &u = dc.fu;
        u.cfg0[0] = (st.prim.tme ? 1u : 0u) | (st.prim.fge ? 2u : 0u) | (replTint ? 4u : 0u) | (replTint2 ? 8u : 0u);
        if (!native && exactFlatColour(batch))
            u.cfg0[0] |= 0x40000u; // F_EXACT_COLOR
        u.cfg0[1] = texKind;
        u.cfg0[2] = (ctx.tex0.tfx & 3u) | ((ctx.tex0.tcc & 1u) << 2) | (coordMode << 4) | ((st.linearFilter ? 1u : 0u) << 8) |
                    (texaClass << 12) | (src16 << 14);
        if (st.prim.tme && st.mip.maxLevel != 0u)
        {
            const GSMipInfo &m = st.mip;
            u.cfg0[2] |= (static_cast<uint32_t>(m.maxLevel) << 16) | (mipLowest << 19) | ((m.lcm ? 1u : 0u) << 22) |
                         ((m.magLinear ? 1u : 0u) << 23) | ((m.minLinear ? 1u : 0u) << 24) | ((m.mipLinear ? 1u : 0u) << 25) |
                         (static_cast<uint32_t>(m.l) << 26);
            u.f1[3] = m.k;
        }
        u.cfg1[0] = static_cast<uint32_t>(ctx.clamp & 0xFu);
        if (replTex || mipCopy)
        {
            // REGION_REPEAT over the whole texture is a plain repeat; over
            // a cell the shader wraps in it (regionRepeatReplaceable).
            if ((u.cfg1[0] & 3u) == 3u && ((ctx.clamp >> 4) & (texW - 1u)) == texW - 1u)
                u.cfg1[0] &= ~3u;
            if (((u.cfg1[0] >> 2) & 3u) == 3u && ((ctx.clamp >> 24) & (texH - 1u)) == texH - 1u)
                u.cfg1[0] &= ~0xCu;
        }
        u.cfg1[1] = static_cast<uint32_t>((ctx.clamp >> 4) & 0x3FFu) | (static_cast<uint32_t>((ctx.clamp >> 14) & 0x3FFu) << 16);
        u.cfg1[2] = static_cast<uint32_t>((ctx.clamp >> 24) & 0x3FFu) | (static_cast<uint32_t>((ctx.clamp >> 34) & 0x3FFu) << 16);
        u.cfg1[3] = texW | (texH << 16);
        u.cfg2[0] = st.texa.ta0 | (static_cast<uint32_t>(st.texa.ta1) << 8) | (st.texa.aem ? 1u << 16 : 0u);
        u.cfg2[1] = st.fogR | (static_cast<uint32_t>(st.fogG) << 8) | (static_cast<uint32_t>(st.fogB) << 16);
        u.cfg2[2] = blend.slow ? ctx.frame.fbmsk : 0u;
        u.cfg2[3] = static_cast<uint32_t>(ctx.alpha & 0xFFu) | (static_cast<uint32_t>((ctx.alpha >> 32) & 0xFFu) << 8);
        u.cfg3[0] = blend.premul;
        u.cfg3[1] = blend.alphaOut;
        u.cfg3[2] = (is16BitPsm(fpsm) ? 1u : 0u) | (((st.colclamp & 1u) || blend.wrapTrick) ? 2u : 0u) | (st.pabe ? 4u : 0u) |
                    ((ctx.fba & 1u) && fpsm != GS_PSM_CT24 ? 8u : 0u) | (date ? 16u : 0u) | (((test >> 15) & 1u) ? 32u : 0u) |
                    (fpsm == GS_PSM_CT24 ? 128u : 0u) | (st.prim.abe ? 256u : 0u);
        u.cfg3[3] = static_cast<uint32_t>(std::min(zScale - 1.0, 4294967295.0));
        u.f0[0] = static_cast<float>(scaleX);
        u.f1[2] = static_cast<float>(scaleY);
        u.f0[1] = rttOff[0];
        u.f0[2] = rttOff[1];
        u.f0[3] = static_cast<float>(zScale);
        if (rtt)
        {
            u.f1[0] = static_cast<float>(rtt->width);
            u.f1[1] = static_cast<float>(rtt->height);
        }
        // A replacement's alpha 1.0 is the PS2 palette's own opaque alpha:
        // the game keeps counters in the frame buffer's alpha (the skater
        // shadow), so 0x7F must stay 0x7F.
        if (replTex)
            u.f1[3] = entry && entry->rgbHashed ? 128.0f : static_cast<float>(lastPaletteAlpha);
        if (packPicture)
        {
            // A pack's alpha is the PS2's (0x80 opaque), as PCSX2's dumps
            // have it; a region's picture covers that rectangle only.
            u.f1[3] = 255.0f;
            if (packRegion.hasX || packRegion.hasY)
            {
                u.f2[0] = packRegion.hasX ? static_cast<float>(packRegion.minX) : 0.0f;
                u.f2[1] = packRegion.hasY ? static_cast<float>(packRegion.minY) : 0.0f;
                u.f2[2] = static_cast<float>(packRegion.width(texW));
                u.f2[3] = static_cast<float>(packRegion.height(texH));
            }
        }
        if (mipCopy)
        {
            // The copy holds the GS's alpha bytes. A texture the game
            // magnifies NEAREST keeps its texels sharp up close (pixel art,
            // signs, lookups): the shader takes the nearest texel wherever
            // a pixel covers at most one, filtering only where it shrinks.
            u.f1[3] = 255.0f;
            if (!(st.mip.maxLevel != 0u ? st.mip.magLinear : st.linearFilter))
                u.cfg0[0] |= kMagNearestFlag;
        }
        // A native draw's picture (replacement or copy) keeps every mip
        // level inside its texel range and regions (the shader needs the
        // sampler's anisotropy to know the level).
        if (native && texKind == 3u && mipInset)
        {
            u.cfg0[0] |= kMipInsetFlag;
            u.f1[0] = static_cast<float>(ps2x::gs_quality::anisotropy());
        }
        std::memcpy(dc.tex, texBind, sizeof(texBind));
        dc.sampler0 = sampler0;
        const int sx0 = std::clamp<int>(sc.x0, 0, static_cast<int>(color->width));
        const int sy0 = std::clamp<int>(sc.y0, 0, static_cast<int>(color->height));
        const int sx1 = std::clamp<int>(sc.x1 + 1, 0, static_cast<int>(color->width));
        const int sy1 = std::clamp<int>(sc.y1 + 1, 0, static_cast<int>(color->height));
        if (sx1 <= sx0 || sy1 <= sy0)
            return true;
        dc.scissor = {sx0 * static_cast<int>(scaleX), sy0 * static_cast<int>(scaleY), (sx1 - sx0) * static_cast<int>(scaleX),
                      (sy1 - sy0) * static_cast<int>(scaleY)};
        dc.blend[0] = dc.blend[1] = dc.blend[2] = std::min(blend.constant, 1.0f);
        dc.blend[3] = 128.0f / 255.0f;

        static const SDL_GPUCompareOp zops[4] = {SDL_GPU_COMPAREOP_NEVER, SDL_GPU_COMPAREOP_ALWAYS,
                                                 SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_GREATER};
        bool wroteColor = false, wroteDepth = false;
        for (int i = 0; i < passCount; ++i)
        {
            const PassSpec &ps = passes[i];
            PipeKey k{};
            k.slow = blend.slow ? 1 : 0;
            k.lines = line ? 1 : 0;
            k.native = native;
            k.hasDepth = pass.depth ? 1 : 0;
            k.depth16 = pass.depth && hwDepth16(pass.depth->psm) ? 1 : 0;
            const bool zWrite = ps.masks.depth && !ctx.zbuf.zmask && depth != nullptr;
            const bool zTest = depth != nullptr && ztst != 1u;
            k.depthOp = static_cast<uint8_t>(zTest ? zops[ztst] : SDL_GPU_COMPAREOP_ALWAYS);
            k.depthWrite = zWrite ? 1 : 0;
            k.depthOut = (zWrite || zTest) && pass.depth ? 1 : 0;
            // Native meshes with high depth precision (gs_quality.h): the
            // GPU's own depth, z'/w' / 2^bits in float, not rounded to the
            // GS's integer steps (one step is 49 units at 2000 with a 16-bit
            // Z buffer), so far surfaces sort among themselves.
            if (native && ps2x::gs_quality::preciseDepth())
                k.depthOut = 0;
            // PS2X_GS_SHADER_DEPTH=0 (a measurement): the GPU's own interpolated depth for GS-path draws too, not
            // rounded to the Z buffer's integer steps in the shader (which turns off early depth tests).
            if (!shaderDepth || k.depth16)
                k.depthOut = 0; // (D16: the GPU rounds to the Z steps itself)
            uint8_t wm = fbEnable;
            if (!ps.masks.rgb || blend.keepColor)
                wm &= 0x8;
            if (!ps.masks.alpha)
                wm &= 0x7;
            if (fpsm == GS_PSM_CT32 && ps.masks.rgb && !ps.masks.alpha)
                wm &= 0x7; // keeps the destination alpha
            if (blend.slow)
            {
                // The shader merges FBMSK itself.
                wm = 0xF;
                if (!ps.masks.rgb)
                    wm &= 0x8;
                if (!ps.masks.alpha)
                    wm &= 0x7;
            }
            k.writeMask = wm;
            if (!blend.slow && blend.enable)
            {
                k.blend = 1;
                k.srcColor = static_cast<uint8_t>(blend.srcColor);
                k.dstColor = static_cast<uint8_t>(blend.dstColor);
                k.colorOp = static_cast<uint8_t>(blend.op);
                if (blend.alphaOut == 1u)
                {
                    k.srcAlpha = static_cast<uint8_t>(SDL_GPU_BLENDFACTOR_CONSTANT_COLOR);
                    k.dstAlpha = static_cast<uint8_t>(SDL_GPU_BLENDFACTOR_ZERO);
                }
                else
                {
                    k.srcAlpha = static_cast<uint8_t>(SDL_GPU_BLENDFACTOR_ONE);
                    k.dstAlpha = static_cast<uint8_t>(SDL_GPU_BLENDFACTOR_ZERO);
                }
                k.alphaOp = static_cast<uint8_t>(SDL_GPU_BLENDOP_ADD);
            }
            if (wm == 0 && !zWrite)
                continue;
            const uint64_t packed = k.pack();
            if (packed != lastPipeKey)
            {
                if (pipeKeys.find(packed) == pipeKeys.end())
                    pipeKeys.emplace(packed, k);
                lastPipeKey = packed;
            }
            dc.pipe = packed;
            u.cfg0[3] = static_cast<uint32_t>(test & 0xFFFu) | (ps.mode << 12);
            emit(dc);
            wroteColor |= wm != 0;
            wroteDepth |= zWrite;
        }

        // Bookkeeping: these pages are now newer on the GPU.
        bool grew = false;
        if (wroteColor)
            grew |= noteDrawn(color, box);
        if (wroteDepth && depth)
            grew |= noteDrawn(depth, box);
        if (record)
            recordDrawMemo(*record, batch, coordMode, line, sc, fbw, color, depth, needDepth, entry, grew, wroteColor, wroteDepth,
                           blend, rtt != nullptr || replTex != nullptr || mipCopy != nullptr);
        return true;
    }

    // ---- presentation ----------------------------------------------------

    // Whether another target drew (and has not written back) in the
    // lines of t's buffer from its end down to `bottom`.
    bool drawnBelow(const Target &t, uint32_t bottom) const
    {
        bottom = std::min(bottom, kMaxTargetHeight);
        if (bottom <= t.height)
            return true;
        GSMem::PageMask below;
        GSMem::AddRectPages(below, t.psm, t.bp, t.bw, 0u, t.height, t.width - 1u, bottom - 1u);
        for (const auto &o : targets)
            if (o.get() != &t && !o->dirty.empty() && o->dirtyPages.intersects(below))
                return true;
        return false;
    }

    PresentationFrame present(const GSPresentationRequest &request)
    {
        traceReason = "present";
        PresentPicture &picture = pictures[std::min<uint32_t>(request.slot, kPresentationSlots - 1u)];
        picture.presented = false;
        PresentationFrame result{};
        const uint64_t pmode = request.pmode;
        const bool en1 = (pmode & 1u) != 0u, en2 = (pmode & 2u) != 0u;
        const GSFrameReg f1 = decodeDisplayFrame(request.dispfb1), f2 = decodeDisplayFrame(request.dispfb2);
        uint32_t w1 = 0, h1 = 0, w2 = 0, h2 = 0;
        decodeDisplaySize(request.display1, w1, h1);
        decodeDisplaySize(request.display2, w2, h2);
        bool valid1 = en1 && hasDisplaySetup(request.display1, f1);
        bool valid2 = en2 && hasDisplaySetup(request.display2, f2);
        if (!valid1 && !valid2)
            return result;
        const bool interlaced = (request.smode2 & 1u) != 0u, frameMode = ((request.smode2 >> 1) & 1u) != 0u;
        const bool fieldMode = interlaced && !frameMode;
        const bool odd = (request.vsyncTick & 1u) != 0u;

        struct Src
        {
            Target *t = nullptr;
            int ox = 0, oy = 0;
            uint32_t w = 0, h = 0;
            uint32_t bp = 0, bw = 0; // the buffer shown
            uint8_t psm = 0;
            uint32_t shown = 0; // lines of it shown
        };
        // Which buffer a read circuit shows, and how many of its lines.
        auto where = [&](const GSFrameReg &f, uint64_t dispfb, uint32_t w, uint32_t h, bool allowPreferred) -> Src
        {
            Src s;
            s.w = w;
            s.h = h;
            s.bp = f.fbp << 5;
            s.bw = f.fbw ? f.fbw : 10u;
            s.psm = f.psm;
            s.ox = static_cast<int>((dispfb >> 32) & 0x7FFu);
            s.oy = static_cast<int>((dispfb >> 43) & 0x7FFu);
            if (allowPreferred && request.hasPreferredSource && request.preferredDestFbp == f.fbp &&
                (request.preferredSource.fbw != 0u || request.preferredSource.fbp != f.fbp))
            {
                s.bp = request.preferredSource.fbp;
                s.bw = request.preferredSource.fbw ? request.preferredSource.fbw : 10u;
                s.psm = request.preferredSource.psm;
                s.ox = s.oy = 0;
            }
            s.shown = static_cast<uint32_t>(s.oy) + h;
            return s;
        };
        // Its target.
        auto lookUp = [&](Src &s)
        {
            if (!isColorPsm(s.psm))
                return;
            const uint32_t bp = s.bp, bw = s.bw, shown = s.shown;
            const uint8_t psm = s.psm;
            // An existing target is shown as it is (lines past its end
            // repeat the last one), not grown: it is often shorter than
            // DISPLAY says, with the next buffer right after it. Unless
            // local memory is current there (no other target drew those
            // lines since its last write-back): then it grows from memory,
            // as the CPU renderer reads it. A 448-line loading screen is
            // uploaded in 64-line strips, each at its own DBP, over the
            // front end's 224-line buffers; without this its lower lines
            // showed the target's last line repeated down the screen.
            Target *t = findTarget(bp, bw, psm);
            if (t && t->height < shown && !drawnBelow(*t, shown))
                t = getTarget(bp, bw, psm, shown);
            if (!t)
                t = getTarget(bp, bw, psm, shown);
            else if (t->stale)
                load(*t);
            s.t = t;
        };
        Src s1, s2;
        if (valid1)
            s1 = where(f1, request.dispfb1, w1, h1, !valid2);
        if (valid2)
            s2 = where(f2, request.dispfb2, w2, h2, !valid1);
        // Both circuits often show one buffer (blended, as a flicker
        // filter), and for a moment they can want different heights of it
        // (the game's DISPLAY1 and DISPLAY2 writes latched between them).
        // Then the buffer is looked up once, at the larger height: a second
        // lookup that grows it would destroy and remake the target the first
        // one returned, and the picture was drawn from the destroyed one's
        // texture (the intro-movie crash on Windows, BRINGUP "The intro-movie
        // quit"). Different buffers have different targets: one lookup never
        // destroys the other's.
        const bool oneBuffer = valid1 && valid2 && s1.bp == s2.bp && s1.bw == s2.bw &&
                               storageClass(s1.psm) == storageClass(s2.psm);
        if (oneBuffer)
            s1.shown = s2.shown = std::max(s1.shown, s2.shown);
        if (valid1)
            lookUp(s1);
        if (valid2)
        {
            if (oneBuffer)
                s2.t = s1.t;
            else
                lookUp(s2);
        }
        valid1 = valid1 && s1.t;
        valid2 = valid2 && s2.t;
        if (!valid1 && !valid2)
            return result;

        const uint32_t outW = (valid1 && valid2) ? std::max(w1, w2) : (valid1 ? w1 : w2);
        const uint32_t outH = (valid1 && valid2) ? std::max(h1, h2) : (valid1 ? h1 : h2);
        closePass();
        flushUploads();
        SDL_GPUTexture *out = presentTarget(picture, outW * scaleX, outH * scaleY);

        PresentUniforms pu{};
        pu.src1[0] = s1.ox; pu.src1[1] = s1.oy; pu.src1[2] = static_cast<int>(s1.w); pu.src1[3] = static_cast<int>(s1.h);
        pu.src2[0] = s2.ox; pu.src2[1] = s2.oy; pu.src2[2] = static_cast<int>(s2.w); pu.src2[3] = static_cast<int>(s2.h);
        pu.cfg[0] = (valid1 ? 1u : 0u) | (valid2 ? 2u : 0u) | (valid1 && is16BitPsm(s1.t->psm) ? 4u : 0u) |
                    (valid2 && is16BitPsm(s2.t->psm) ? 8u : 0u) | (fieldMode ? 16u : 0u) | (odd ? 32u : 0u) |
                    (((pmode >> 7) & 1u) ? 64u : 0u) | (((pmode >> 5) & 1u) ? 128u : 0u) | (((pmode >> 6) & 1u) ? 256u : 0u);
        pu.cfg[1] = static_cast<uint32_t>((pmode >> 8) & 0xFFu);
        pu.cfg[2] = static_cast<uint32_t>(request.bgcolor & 0xFFFFFFu);
        pu.cfg[3] = scaleX;
        pu.out[0] = static_cast<int>(outW);
        pu.out[1] = static_cast<int>(outH);
        pu.out[2] = static_cast<int>(scaleY);

        picture.mips = picture.levels > 1u && ps2x::gs_quality::fxaa();
        if (HeldFrame *held = holding(); held && request.frame != 0u)
        {
            PresentStep &st = held->present;
            // Whether the buffers shown were drawn in this frame's steps, or (a game that shows the frame
            // before the one it draws: DM2) in the frame before's.
            auto drawnHere = [&](Target *t)
            {
                const auto it = held->byLive.find(t->tex);
                if (it == held->byLive.end())
                    return false;
                for (const Step &step : held->steps)
                    if (step.kind == Step::Kind::Pass && step.color == it->second)
                        return true;
                return false;
            };
            st.ownDrawing = (!valid1 || drawnHere(s1.t)) && (!valid2 || drawnHere(s2.t));
            st.live1 = valid1 ? s1.t->tex : nullptr;
            st.live2 = valid2 ? s2.t->tex : nullptr;
            st.valid = true;
            st.src1 = valid1 ? holdTarget(*held, s1.t) : -1;
            st.src2 = valid2 ? holdTarget(*held, s2.t) : -1;
            st.pu = pu;
            st.w = outW;
            st.h = outH;
            st.mips = ps2x::gs_quality::fxaa();
            copyHeldSnapshots(*held);
        }
        recordPresentPass(out, valid1 ? s1.t->tex : s2.t->tex, valid2 ? s2.t->tex : s1.t->tex, pu, picture.mips);
        picture.presented = true;
        holdFrameMade(request);
        result.width = outW;
        result.height = outH;
        result.displayFbp = valid1 ? f1.fbp : f2.fbp;
        result.sourceFbp = (valid1 ? s1.t : s2.t)->bp >> 5;
        if (!options.readback)
        {
            // Shown from the slot's texture (the host window's device):
            // submitted here, before the host's frame that samples it.
            submit(false);
            result.onGpu = true;
            return result;
        }

        // Read the picture back (the replay tool and the CPU-side host
        // presentation need it in memory).
        const uint32_t sw = outW * scaleX, sh = outH * scaleY;
        ensureDownloadBuffer(sw * sh * 4u);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer());
        SDL_GPUTextureRegion region{};
        region.texture = out;
        region.w = sw;
        region.h = sh;
        region.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = downTb;
        dst.pixels_per_row = sw;
        dst.rows_per_layer = sh;
        texcheck::downloadFromTexture(copy, &region, &dst);
        SDL_EndGPUCopyPass(copy);
        submitAndWait();
        const uint8_t *map = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(dev, downTb, false));
        lastScaled.assign(map, map + static_cast<size_t>(sw) * sh * 4u);
        SDL_UnmapGPUTransferBuffer(dev, downTb);
        lastScaledW = sw;
        lastScaledH = sh;

        result.pixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
        const uint32_t halfX = scaleX / 2u, halfY = scaleY / 2u;
        for (uint32_t y = 0; y < outH; ++y)
            for (uint32_t x = 0; x < outW; ++x)
                std::memcpy(result.pixels.data() + (y * kHostFrameWidth + x) * 4u,
                            lastScaled.data() + ((static_cast<size_t>(y) * scaleY + halfY) * sw + x * scaleX + halfX) * 4u, 4);
        return result;
    }

    // The presentation's pass: the read circuits' buffers t1, t2 merged into out.
    void recordPresentPass(SDL_GPUTexture *out, SDL_GPUTexture *t1, SDL_GPUTexture *t2, const PresentUniforms &pu, bool mips)
    {
        SDL_GPUColorTargetInfo ct{};
        ct.texture = out;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass *rp = texcheck::beginRenderPass(commandBuffer(), &ct, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(rp, presentPipe);
        SDL_GPUTextureSamplerBinding tb[2] = {{t1, sampler}, {t2, sampler}};
        texcheck::bindFragmentSamplers(rp, 0, tb, 2);
        SDL_PushGPUFragmentUniformData(commandBuffer(), 0, &pu, sizeof(pu));
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        SDL_EndGPURenderPass(rp);
        if (mips)
            texcheck::generateMipmaps(commandBuffer(), out);
    }

    // A slot's texture holds the picture of the last present() into it
    // (unless that showed nothing).
    bool readPresented(uint32_t slot, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
    {
        return readPicture(pictures[std::min<uint32_t>(slot, kPresentationSlots - 1u)], rgba, width, height);
    }


    // One picture per presentation slot (GSPresentationRequest::slot): the
    // window shows one while the GS worker makes the next game frame's in
    // another (GS::capturePresentationFrame).
    struct PresentPicture
    {
        SDL_GPUTexture *tex = nullptr;
        uint32_t w = 0, h = 0;
        uint32_t levels = 1;    // its mipmap levels
        bool mips = false;      // made for the last picture
        bool presented = false; // holds a picture
    };
    std::array<PresentPicture, kPresentationSlots> pictures{};
    SDL_GPUTexture *presentTarget(PresentPicture &picture, uint32_t w, uint32_t h)
    {
        if (picture.tex && picture.w == w && picture.h == h)
            return picture.tex;
        if (picture.tex)
            releaseAfterSubmit.push_back(picture.tex);
        // Shown in a window: a few mipmap levels for a window much smaller
        // than the picture (made per frame with anti-aliasing on).
        picture.levels = 1;
        if (!options.readback)
            while (picture.levels < 4u && (std::min(w, h) >> picture.levels) >= 128u)
                ++picture.levels;
        picture.tex = createTexture(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                    SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, picture.levels);
        picture.w = w;
        picture.h = h;
        return picture.tex;
    }

    bool readPicture(const PresentPicture &picture, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
    {
        if (!ok || !picture.presented || !picture.tex)
            return false;
        ensureDownloadBuffer(picture.w * picture.h * 4u);
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer());
        SDL_GPUTextureRegion region{};
        region.texture = picture.tex;
        region.w = picture.w;
        region.h = picture.h;
        region.d = 1;
        SDL_GPUTextureTransferInfo dst{};
        dst.transfer_buffer = downTb;
        dst.pixels_per_row = picture.w;
        dst.rows_per_layer = picture.h;
        texcheck::downloadFromTexture(copy, &region, &dst);
        SDL_EndGPUCopyPass(copy);
        submitAndWait();
        const uint8_t *map = static_cast<const uint8_t *>(SDL_MapGPUTransferBuffer(dev, downTb, false));
        if (!map)
            return false;
        rgba.assign(map, map + static_cast<size_t>(picture.w) * picture.h * 4u);
        SDL_UnmapGPUTransferBuffer(dev, downTb);
        width = picture.w;
        height = picture.h;
        return true;
    }

    // ---- frame interpolation (runtime/gs/gs_interpolate.h; GPU_BACKEND.md "Frame interpolation") ----------
    //
    // While it is on, every GPU step of a game frame is kept as it is recorded (each pass of draws, the fog and
    // anti-aliasing passes, the presentation) with the motion of its native draws, from one Present to the next.
    // A picture between two frames is the newer one's steps recorded again into targets of the replay's own,
    // started from copies of what the frame's targets held when the frame first used them, with each native draw
    // found in both frames moved to the point between and the GS-path 3D vertices moved with the camera. A frame
    // that does something the steps do not hold (a target loaded from local memory, the CPU rasterizer, a texture
    // it drew with written again while held) is not replayed.

    // A native draw's motion, by DrawCall::motion.
    struct Motion
    {
        uint64_t key = 0;
        float world[4][4] = {};
        float camera[4][4] = {};
        float transform[4][4] = {};
        float clip[4][4] = {};
        bool sky = false;  // (its transform's Z column is the camera's W: not a view's reference)
        bool clipped = false; // clipTransform used (drop flags, the clipping programs)
        SDL_Rect scissor{};
        std::shared_ptr<const GSNativeSkinning> skin;
        std::shared_ptr<const GSNativeMesh> mesh; // (kept while the frame is held)
    };
    // A target the frame used: its texture then, its shape, and a copy of its pixels from before its first use.
    struct HeldTarget
    {
        SDL_GPUTexture *live = nullptr;
        uint32_t width = 0, height = 0; // GS pixels
        uint8_t psm = 0;
        bool depth = false;
        SDL_GPUTexture *start = nullptr;
    };
    struct Step
    {
        enum class Kind : uint8_t
        {
            Pass,
            Fog,
            Fxaa,
        };
        Kind kind = Kind::Pass;
        int color = -1, depth = -1; // HeldTarget indices
        // Pass
        Rect copyRegion;
        SDL_GPUTexture *copyTex = nullptr; // the live copy texture the pass's draws sample
        std::vector<GpuVertex> verts;
        std::vector<DrawCall> draws;
        std::vector<GpuSkinUniforms> skins;
        // Fog
        GpuFogUniforms fog{};
        SDL_Rect scissor{};
        bool writeAlpha = false;
        // Fxaa
        Rect box;
    };
    struct PresentStep
    {
        bool valid = false;
        int src1 = -1, src2 = -1;
        PresentUniforms pu{};
        uint32_t w = 0, h = 0; // the picture, in pixels
        bool mips = false;
        // The buffers shown (their textures then) were drawn in this frame's own steps; else in the frame
        // before's (the frame shows what the one before drew).
        bool ownDrawing = true;
        SDL_GPUTexture *live1 = nullptr, *live2 = nullptr;
    };
    struct HeldFrame
    {
        uint64_t frame = 0; // GS::capturePresentationFrame's number
        uint64_t vsync = 0;
        bool replayable = true;
        const char *why = nullptr;
        std::vector<HeldTarget> targets;
        std::unordered_map<SDL_GPUTexture *, int> byLive;
        std::vector<Step> steps;
        PresentStep present;
        std::vector<Motion> motions;
        std::unordered_map<uint64_t, uint32_t> byKey; // a key's first motion
        std::unordered_set<SDL_GPUTexture *> sampled; // textures (not targets) its draws sample
        std::vector<std::pair<SDL_GPUTexture *, SDL_GPUTexture *>> snapshots; // to copy before the next step
    };
    struct Interp
    {
        bool on = false;
        std::unique_ptr<HeldFrame> building;
        std::deque<std::unique_ptr<HeldFrame>> made; // newest last
        // Textures of the replay's own and of the held frames' start copies, by shape (formats, size, use).
        std::unordered_map<uint64_t, std::vector<SDL_GPUTexture *>> free;
        std::vector<SDL_GPUTexture *> deferredRelease; // given back while a held frame drew with them
        std::array<PresentPicture, 2> pictures{};
        uint32_t nextPicture = 0;
        // The last two pictures between's submits: one may still be on the GPU when the next is asked for (the
        // GPU a refresh behind), not two.
        std::shared_ptr<FenceRef> lastFence, olderFence;
        bool timing = false;                 // PS2X_INTERP_TIMING=1: each picture between timed alone
        bool force = false;                  // PS2X_INTERP_FORCE=1: made at a cut or with nothing moved too
        bool mathAtOne = false;              // PS2X_INTERP_MATH_AT_ONE=1: at t = 1 the matrices worked out too
        std::string leave;                   // PS2X_INTERP_LEAVE (measuring)
        double lastGpuMs = 0.0;
        uint64_t skips[static_cast<size_t>(ps2x::gs_interpolate::Skip::kCount)]{};
        uint64_t draws[static_cast<size_t>(ps2x::gs_interpolate::Draw::kCount)]{};
        uint64_t pictures_ = 0;
        double gpuMsSum = 0.0, gpuMsMax = 0.0; // submit to the fence (PS2X_INTERP_TIMING)
        double cpuMsSum = 0.0, cpuMsMax = 0.0; // recording the picture's commands
        uint64_t gpuTimed = 0;
        uint64_t unreplayable[8]{};
    } interp;

    static uint64_t heldShape(uint32_t w, uint32_t h, SDL_GPUTextureFormat f, bool depth)
    {
        return (static_cast<uint64_t>(f) << 48) ^ (static_cast<uint64_t>(depth) << 47) ^ (static_cast<uint64_t>(w) << 24) ^ h;
    }

    SDL_GPUTextureFormat heldFormat(const HeldTarget &t) const
    {
        return t.depth ? depthFormat(t.psm) : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    }

    SDL_GPUTexture *takeHeldTexture(uint32_t w, uint32_t h, SDL_GPUTextureFormat f, bool depth)
    {
        auto &list = interp.free[heldShape(w, h, f, depth)];
        if (!list.empty())
        {
            SDL_GPUTexture *t = list.back();
            list.pop_back();
            return t;
        }
        return createTexture(w, h, f,
                             (depth ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : SDL_GPU_TEXTUREUSAGE_COLOR_TARGET) |
                                 SDL_GPU_TEXTUREUSAGE_SAMPLER);
    }

    void giveHeldTexture(SDL_GPUTexture *t, uint32_t w, uint32_t h, SDL_GPUTextureFormat f, bool depth)
    {
        if (t)
            interp.free[heldShape(w, h, f, depth)].push_back(t);
    }

    void dropHeld(std::unique_ptr<HeldFrame> &f)
    {
        if (!f)
            return;
        for (HeldTarget &t : f->targets)
            giveHeldTexture(t.start, t.width * scaleX, t.height * scaleY, heldFormat(t), t.depth);
        f.reset();
    }

    // Whether a held frame draws with t (or holds it as a target).
    bool heldUses(SDL_GPUTexture *t) const
    {
        auto uses = [&](const std::unique_ptr<HeldFrame> &f)
        { return f && (f->sampled.count(t) != 0u || f->byLive.count(t) != 0u); };
        if (uses(interp.building))
            return true;
        for (const auto &f : interp.made)
            if (uses(f))
                return true;
        return false;
    }

    // The frame being drawn, when interpolation keeps it.
    HeldFrame *holding()
    {
        if (!interp.on)
            return nullptr;
        if (!interp.building)
        {
            interp.building = std::make_unique<HeldFrame>();
            // (begun in the middle of a frame: its first steps are not held)
            interp.building->replayable = false;
            interp.building->why = "partial";
        }
        return interp.building.get();
    }

    void notReplayable(const char *why)
    {
        if (HeldFrame *f = holding(); f && f->replayable)
        {
            f->replayable = false;
            f->why = why;
        }
    }

    // A texture is written: a held frame that drew with it earlier would draw with the new pixels.
    void heldTextureWritten(SDL_GPUTexture *t)
    {
        if (!interp.on)
            return;
        auto check = [&](std::unique_ptr<HeldFrame> &f)
        {
            if (f && f->replayable && f->sampled.count(t) != 0u)
            {
                f->replayable = false;
                f->why = "texture rewritten";
            }
        };
        check(interp.building);
        for (auto &f : interp.made)
            check(f);
    }

    // A target this frame uses: held (its pixels copied before the next step) the first time.
    int holdTarget(HeldFrame &f, Target *t)
    {
        if (!t || !t->tex)
            return -1;
        auto it = f.byLive.find(t->tex);
        if (it != f.byLive.end())
            return it->second;
        HeldTarget h;
        h.live = t->tex;
        h.width = t->width;
        h.height = t->height;
        h.psm = t->psm;
        h.depth = t->depth;
        h.start = takeHeldTexture(t->width * scaleX, t->height * scaleY, heldFormat(h), h.depth);
        const int index = static_cast<int>(f.targets.size());
        f.targets.push_back(h);
        f.byLive.emplace(t->tex, index);
        f.snapshots.push_back({t->tex, h.start});
        return index;
    }

    // The textures a pass's draws sample: targets held (before the pass: their start copied), the others noted
    // (after its uploads: a texture written again later would change what the frame drew).
    void holdSampled(HeldFrame &f, const std::vector<DrawCall> &draws, SDL_GPUTexture *copyTex, bool targetsOnly)
    {
        for (const DrawCall &d : draws)
            for (SDL_GPUTexture *tex : d.tex)
            {
                if (!tex || tex == copyTex || f.byLive.count(tex) != 0u)
                    continue;
                Target *target = nullptr;
                for (auto &t : targets)
                    if (t->tex == tex)
                        target = t.get();
                if (target)
                    holdTarget(f, target);
                else if (!targetsOnly)
                    f.sampled.insert(tex);
            }
    }

    // The start copies owed, before the step that first uses their targets.
    void copyHeldSnapshots(HeldFrame &f)
    {
        if (f.snapshots.empty())
            return;
        SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer());
        for (const auto &[live, start] : f.snapshots)
        {
            const HeldTarget &t = f.targets[f.byLive[live]];
            SDL_GPUTextureLocation src{}, dst{};
            src.texture = live;
            dst.texture = start;
            texcheck::copyTextureToTexture(copy, &src, &dst, t.width * scaleX, t.height * scaleY, 1, false);
        }
        SDL_EndGPUCopyPass(copy);
        f.snapshots.clear();
    }

    // closePass, before the pass is recorded: its targets held, their start copied.
    void holdPassBegin(Pass &p)
    {
        HeldFrame *f = holding();
        if (!f)
            return;
        holdTarget(*f, p.color);
        holdTarget(*f, p.depth);
        SDL_GPUTexture *copyTex = p.copyRegion.empty() ? nullptr : copyTexture(p.color->width * scaleX, p.color->height * scaleY);
        holdSampled(*f, p.draws, copyTex, true);
        copyHeldSnapshots(*f);
    }

    // closePass, after: the pass as recorded.
    void holdPass(const Pass &p)
    {
        HeldFrame *f = holding();
        if (!f)
            return;
        Step s;
        s.kind = Step::Kind::Pass;
        s.color = f->byLive[p.color->tex];
        s.depth = p.depth ? f->byLive[p.depth->tex] : -1;
        s.copyRegion = p.copyRegion;
        s.copyTex = p.copyRegion.empty() ? nullptr : copyTexture(p.color->width * scaleX, p.color->height * scaleY);
        holdSampled(*f, p.draws, s.copyTex, false);
        s.verts = p.verts;
        s.draws = p.draws;
        s.skins = skinUniforms;
        f->steps.push_back(std::move(s));
    }

    // A native draw's motion (submitNative); its index for the draw calls.
    uint32_t holdMotion(const GSNativeDraw &d, const GSDrawState &state)
    {
        HeldFrame *f = holding();
        if (!f)
            return UINT32_MAX;
        Motion m;
        m.key = d.motion.key;
        std::memcpy(m.world, d.motion.world, sizeof(m.world));
        std::memcpy(m.camera, d.motion.camera, sizeof(m.camera));
        std::memcpy(m.transform, d.transform, sizeof(m.transform));
        std::memcpy(m.clip, d.clipTransform, sizeof(m.clip));
        m.sky = d.clampXY;
        m.clipped = d.dropFlags != 0u || d.clip == GSNativeClip::NearSplit || d.clip == GSNativeClip::NearSplitAndFar;
        const auto &sc = state.context.scissor;
        m.scissor = SDL_Rect{static_cast<int>(sc.x0), static_cast<int>(sc.y0), static_cast<int>(sc.x1), static_cast<int>(sc.y1)};
        m.skin = d.skin;
        m.mesh = d.mesh;
        const uint32_t index = static_cast<uint32_t>(f->motions.size());
        if (m.key)
            f->byKey.emplace(m.key, index);
        f->motions.push_back(std::move(m));
        return index;
    }

    // Present (a frame of the frame queue): the frame complete, held as made; the next one begins.
    void holdFrameMade(const GSPresentationRequest &request)
    {
        HeldFrame *f = holding();
        if (!f || request.frame == 0u)
            return;
        f->frame = request.frame;
        f->vsync = request.vsyncTick;
        interp.made.push_back(std::move(interp.building));
        while (interp.made.size() > 3u)
        {
            dropHeld(interp.made.front());
            interp.made.pop_front();
        }
        interp.building = std::make_unique<HeldFrame>();
        releaseDeferred();
    }

    void releaseDeferred()
    {
        std::vector<SDL_GPUTexture *> keep;
        for (SDL_GPUTexture *t : interp.deferredRelease)
        {
            if (heldUses(t))
                keep.push_back(t);
            else
                releaseAfterSubmit.push_back(t);
        }
        interp.deferredRelease.swap(keep);
    }

    // Interpolation switched (at a Present): off, everything held goes.
    void setInterpolation(bool on)
    {
        if (on == interp.on)
            return;
        interp.on = on;
        if (!on)
        {
            dropHeld(interp.building);
            for (auto &f : interp.made)
                dropHeld(f);
            interp.made.clear();
            for (SDL_GPUTexture *t : interp.deferredRelease)
                releaseAfterSubmit.push_back(t);
            interp.deferredRelease.clear();
        }
    }

    HeldFrame *heldFrame(uint64_t frame)
    {
        for (auto &f : interp.made)
            if (f->frame == frame)
                return f.get();
        return nullptr;
    }

    // ---- the picture between -------------------------------------------------------------------------------

    // A view's camera in one frame: world -> GS window (as a draw's transform without its object), its inverse,
    // the view matrix and the rest (camera^-1 x that: the projection with the program's Z).
    struct ViewCamera
    {
        bool ok = false;
        float window[4][4] = {};
        float inverse[4][4] = {};
        float view[4][4] = {};
        float projection[4][4] = {};
        SDL_Rect scissor{};
    };

    static bool cameraOfMotion(const Motion &m, ViewCamera &c)
    {
        namespace gi = ps2x::gs_interpolate;
        float wi[4][4], vi[4][4];
        if (!gi::invert(m.world, wi) || !gi::invert(m.camera, vi))
            return false;
        gi::multiply(wi, m.transform, c.window);
        if (!gi::invert(c.window, c.inverse))
            return false;
        std::memcpy(c.view, m.camera, sizeof(c.view));
        gi::multiply(vi, c.window, c.projection);
        c.scissor = m.scissor;
        c.ok = true;
        return true;
    }

    // Each view's camera in a frame, from its first rigid draw (not the sky's, whose Z column is its W; a
    // skinned one only where there is no other).
    static void viewCameras(const HeldFrame &f, std::array<ViewCamera, 4> &out)
    {
        for (int pass = 0; pass < 2; ++pass)
            for (const Motion &m : f.motions)
            {
                if (!m.key || m.sky || (pass == 0 && m.skin))
                    continue;
                const uint32_t v = static_cast<uint32_t>(m.key & 0xFFu);
                if (v < out.size() && !out[v].ok)
                    cameraOfMotion(m, out[v]);
            }
    }

    // A draw's matrix m = world x camera x rest, at t between its two frames' (the rest of each its own: the
    // program's changes to the columns, the projection).
    static bool matrixBetween(const float (&worldA)[4][4], const float (&cameraA)[4][4], const float (&mA)[4][4],
                              const float (&worldB)[4][4], const float (&cameraB)[4][4], const float (&mB)[4][4],
                              const float (&worldT)[4][4], const float (&cameraT)[4][4], float t, float (&out)[4][4])
    {
        namespace gi = ps2x::gs_interpolate;
        float wa[4][4], wb[4][4], ca[4][4], cb[4][4], ra[4][4], rb[4][4], x[4][4];
        if (!gi::invert(worldA, wa) || !gi::invert(worldB, wb) || !gi::invert(cameraA, ca) || !gi::invert(cameraB, cb))
            return false;
        gi::multiply(wa, mA, x);
        gi::multiply(ca, x, ra);
        gi::multiply(wb, mB, x);
        gi::multiply(cb, x, rb);
        float r[4][4];
        gi::lerp(ra, rb, t, r);
        gi::multiply(cameraT, r, x);
        gi::multiply(worldT, x, out);
        return true;
    }

    // A skinned draw's bone palette at t (each bone's rows: rotation, translation).
    static void paletteBetween(const GSNativeSkinning &a, const GSNativeSkinning &b, float t, float (&out)[GSNativeSkinning::kPaletteRows][4])
    {
        namespace gi = ps2x::gs_interpolate;
        std::memcpy(out, b.palette, sizeof(out));
        for (int bone = 0; bone + 3 < GSNativeSkinning::kPaletteRows; bone += 4)
        {
            float ma[4][4], mb[4][4], m[4][4];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 3; ++c)
                {
                    ma[r][c] = a.palette[bone + r][c];
                    mb[r][c] = b.palette[bone + r][c];
                }
            for (int r = 0; r < 3; ++r)
                ma[r][3] = mb[r][3] = 0.0f;
            ma[3][3] = mb[3][3] = 1.0f;
            if (std::memcmp(ma, mb, sizeof(ma)) == 0 || !gi::betweenAffine(ma, mb, t, m))
                continue;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 3; ++c)
                    out[bone + r][c] = m[r][c];
        }
    }

    using Skip = ps2x::gs_interpolate::Skip;

    void countSkip(Skip s) { ++interp.skips[static_cast<size_t>(s)]; }
    void countDraw(ps2x::gs_interpolate::Draw d) { ++interp.draws[static_cast<size_t>(d)]; }

    // The picture at t between frames `from` and `to` (both made, `to` replayed) into one of the replay's own
    // picture textures; Skip::kCount when made, else why not.
    Skip presentBetween(uint64_t from, uint64_t to, float t, GSGpuPicture &out)
    {
        namespace gi = ps2x::gs_interpolate;
        using gi::Draw;
        if (!interp.on)
            return Skip::NoPair;
        // The frames shown (from, to), and the frames whose steps drew what they show (a, b: the same, or each the
        // frame before for a game that shows what it drew a frame earlier).
        HeldFrame *shown = heldFrame(to);
        if (!shown || !heldFrame(from))
            return Skip::NoPair;
        if (!shown->present.valid)
            return Skip::Unreplayable;
        const bool ownDrawing = shown->present.ownDrawing;
        HeldFrame *a = ownDrawing ? heldFrame(from) : heldFrame(from - 1u), *b = ownDrawing ? shown : heldFrame(to - 1u);
        if (!a || !b)
            return Skip::NoPair;
        if (!b->replayable)
            return Skip::Unreplayable;
        const PresentStep &ps = shown->present;
        const auto source = [&](SDL_GPUTexture *live)
        {
            const auto it = live ? b->byLive.find(live) : b->byLive.end();
            return it == b->byLive.end() ? -1 : it->second;
        };
        const int src1 = source(ps.live1), src2 = source(ps.live2);
        if ((ps.live1 && src1 < 0) || (ps.live2 && src2 < 0) || (src1 < 0 && src2 < 0))
            return Skip::Unreplayable;
        if (interp.olderFence && interp.olderFence->fence && !SDL_QueryGPUFence(dev, interp.olderFence->fence))
            return Skip::Budget;
        // The views' cameras, a cut, whether anything moved.
        std::array<ViewCamera, 4> camA, camB, camT;
        viewCameras(*a, camA);
        viewCameras(*b, camB);
        bool moved = false;
        for (size_t v = 0; v < camB.size(); ++v)
        {
            if (!camB[v].ok || !camA[v].ok)
                continue;
            if (gi::cameraCut(camA[v].view, camB[v].view) && !interp.force)
                return Skip::Cut;
            moved = moved || std::memcmp(camA[v].window, camB[v].window, sizeof(camA[v].window)) != 0;
            float view[4][4], projection[4][4];
            if (!gi::betweenView(camA[v].view, camB[v].view, t, view))
                continue;
            gi::lerp(camA[v].projection, camB[v].projection, t, projection);
            camT[v] = camB[v];
            std::memcpy(camT[v].view, view, sizeof(view));
            gi::multiply(view, projection, camT[v].window);
        }
        if (!moved)
            for (const Motion &m : b->motions)
            {
                auto it = m.key ? a->byKey.find(m.key) : a->byKey.end();
                if (it == a->byKey.end())
                    continue;
                const Motion &p = a->motions[it->second];
                if (std::memcmp(p.world, m.world, sizeof(m.world)) != 0 ||
                    (m.skin && p.skin && std::memcmp(p.skin->palette, m.skin->palette, sizeof(m.skin->palette)) != 0))
                {
                    moved = true;
                    break;
                }
            }
        if (!moved && !interp.force)
            return Skip::Still;
        // At t = 1 the newer frame exactly: its own matrices and vertices (the replay itself is what is checked).
        const bool exact = t >= 1.0f && !interp.mathAtOne;

        const auto t0 = std::chrono::steady_clock::now();
        closePass();
        flushUploads();
        if (interp.timing)
            submitAndWait(); // (the GPU idle: the picture's own time below)
        const auto tGpu = std::chrono::steady_clock::now();

        // The replay's targets, from the frame's start copies.
        std::vector<SDL_GPUTexture *> own(b->targets.size(), nullptr);
        {
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commandBuffer());
            for (size_t i = 0; i < b->targets.size(); ++i)
            {
                const HeldTarget &h = b->targets[i];
                own[i] = takeHeldTexture(h.width * scaleX, h.height * scaleY, heldFormat(h), h.depth);
                if (interp.leave.find('c') != std::string::npos)
                    continue;
                SDL_GPUTextureLocation src{}, dst{};
                src.texture = h.start;
                dst.texture = own[i];
                texcheck::copyTextureToTexture(copy, &src, &dst, h.width * scaleX, h.height * scaleY, 1, false);
            }
            SDL_EndGPUCopyPass(copy);
        }
        auto ownOf = [&](SDL_GPUTexture *live) -> SDL_GPUTexture *
        {
            auto it = b->byLive.find(live);
            return it == b->byLive.end() ? live : own[it->second];
        };
        std::vector<std::pair<SDL_GPUTexture *, uint64_t>> copies; // the replay's copy textures (shader blending)

        // A view by a draw's scissor (split screen), else the only one.
        auto viewOf = [&](const SDL_Rect &gsScissor) -> int
        {
            int only = -1, found = -1, count = 0;
            for (int v = 0; v < 4; ++v)
            {
                if (!camT[v].ok)
                    continue;
                ++count;
                only = v;
                const SDL_Rect &s = camB[v].scissor;
                if (s.x == gsScissor.x && s.y == gsScissor.y && s.w == gsScissor.w && s.h == gsScissor.h)
                    found = v;
            }
            return found >= 0 ? found : count == 1 ? only : -1;
        };

        for (const Step &s : b->steps)
        {
            if (interp.leave.find(s.kind == Step::Kind::Pass ? 'p' : 'f') != std::string::npos)
                continue;
            SDL_GPUTexture *colour = own[s.color];
            const HeldTarget &ct = b->targets[s.color];
            if (s.kind == Step::Kind::Fog)
            {
                recordFogDraw(colour, own[s.depth], ct.width, ct.height, s.scissor, s.fog, s.writeAlpha);
                continue;
            }
            if (s.kind == Step::Kind::Fxaa)
            {
                SDL_GPUTexture *copyTex = takeHeldTexture(ct.width * scaleX, ct.height * scaleY,
                                                          SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, false);
                copies.push_back({copyTex, heldShape(ct.width * scaleX, ct.height * scaleY,
                                                     SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, false)});
                recordFxaa(colour, copyTex, ct.width, ct.height, s.box);
                continue;
            }
            // A pass: its vertices and draws moved to t.
            std::vector<GpuVertex> verts = s.verts;
            std::vector<DrawCall> draws = s.draws;
            std::vector<GpuSkinUniforms> skins = s.skins;
            for (DrawCall &d : draws)
            {
                // Targets sampled: the replay's own.
                for (SDL_GPUTexture *&tex : d.tex)
                    if (tex && tex != s.copyTex)
                        tex = ownOf(tex);
                if (exact)
                {
                }
                else if (d.motion != UINT32_MAX)
                {
                    const Motion &m = b->motions[d.motion];
                    auto it = m.key ? a->byKey.find(m.key) : a->byKey.end();
                    if (!m.key)
                    {
                        countDraw(Draw::NoMotion);
                        continue;
                    }
                    if (it == a->byKey.end())
                    {
                        countDraw(Draw::Unmatched);
                        continue;
                    }
                    const Motion &p = a->motions[it->second];
                    if (gi::jumped(p.world, m.world))
                    {
                        countDraw(Draw::Jumped);
                        continue;
                    }
                    const int v = static_cast<int>(m.key & 0xFFu);
                    float world[4][4], camera[4][4];
                    if (!gi::betweenAffine(p.world, m.world, t, world) || v >= 4 || !camT[v].ok)
                    {
                        countDraw(Draw::Unmatched);
                        continue;
                    }
                    std::memcpy(camera, camT[v].view, sizeof(camera));
                    float transform[4][4];
                    if (!matrixBetween(p.world, p.camera, p.transform, m.world, m.camera, m.transform, world, camera, t,
                                       transform))
                    {
                        countDraw(Draw::Unmatched);
                        continue;
                    }
                    std::memcpy(d.nu.m, transform, sizeof(d.nu.m));
                    if (m.clipped)
                    {
                        float clip[4][4];
                        if (matrixBetween(p.world, p.camera, p.clip, m.world, m.camera, m.clip, world, camera, t, clip))
                            std::memcpy(d.nu.clip, clip, sizeof(d.nu.clip));
                    }
                    if (d.skin != UINT32_MAX && d.skin < skins.size() && m.skin && p.skin)
                    {
                        paletteBetween(*p.skin, *m.skin, t, skins[d.skin].palette);
                        gi::Matrix la, lb, l;
                        std::memcpy(la, p.skin->lightDir, sizeof(p.skin->lightDir));
                        std::memcpy(lb, m.skin->lightDir, sizeof(m.skin->lightDir));
                        la[3][0] = la[3][1] = la[3][2] = la[3][3] = lb[3][0] = lb[3][1] = lb[3][2] = lb[3][3] = 0.0f;
                        gi::lerp(la, lb, t, l);
                        std::memcpy(d.nu.lightDir, l, sizeof(d.nu.lightDir));
                    }
                    countDraw(Draw::Moved);
                }
                else if (d.gs.reproject)
                {
                    const int view = viewOf(d.gs.scissor);
                    if (view < 0 || !camB[view].ok || !camT[view].ok)
                        continue;
                    for (uint32_t i = d.first; i < d.first + d.count && i < verts.size(); ++i)
                    {
                        GpuVertex &g = verts[i];
                        float xyz[3] = {g.pos[0] + d.gs.ofx, g.pos[1] + d.gs.ofy, g.pos[2] * d.gs.zScale};
                        float ratio = 1.0f;
                        if (!gi::reproject(camB[view].inverse, camB[view].window, camT[view].window, xyz, ratio))
                            continue;
                        g.pos[0] = xyz[0] - d.gs.ofx;
                        g.pos[1] = xyz[1] - d.gs.ofy;
                        g.pos[2] = std::max(0.0f, xyz[2] / d.gs.zScale);
                        if (d.gs.stq)
                        {
                            g.tex[0] *= ratio;
                            g.tex[1] *= ratio;
                            g.tex[2] *= ratio;
                        }
                    }
                    countDraw(Draw::Reprojected);
                }
            }
            // The pass's copy for shader blending, from the replay's target.
            SDL_GPUTexture *copyTex = nullptr;
            if (s.copyTex)
            {
                const uint32_t cw = ct.width * scaleX, ch = ct.height * scaleY;
                copyTex = takeHeldTexture(cw, ch, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, false);
                copies.push_back({copyTex, heldShape(cw, ch, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, false)});
                for (DrawCall &d : draws)
                    for (SDL_GPUTexture *&tex : d.tex)
                        if (tex == s.copyTex)
                            tex = copyTex;
            }
            SDL_GPUCopyPass *copy = nullptr;
            const uint32_t vertexOffset =
                recordUploads(verts.data(), static_cast<uint32_t>(verts.size() * sizeof(GpuVertex)), &copy);
            if (copyTex)
            {
                if (!copy)
                    copy = SDL_BeginGPUCopyPass(commandBuffer());
                Rect r = s.copyRegion;
                r.x0 = std::max(r.x0, 0);
                r.y0 = std::max(r.y0, 0);
                r.x1 = std::min<int>(r.x1, static_cast<int>(ct.width) - 1);
                r.y1 = std::min<int>(r.y1, static_cast<int>(ct.height) - 1);
                if (!r.empty())
                {
                    SDL_GPUTextureLocation src{};
                    src.texture = colour;
                    src.x = static_cast<uint32_t>(r.x0) * scaleX;
                    src.y = static_cast<uint32_t>(r.y0) * scaleY;
                    SDL_GPUTextureLocation dst = src;
                    dst.texture = copyTex;
                    texcheck::copyTextureToTexture(copy, &src, &dst, static_cast<uint32_t>(r.x1 - r.x0 + 1) * scaleX,
                                                   static_cast<uint32_t>(r.y1 - r.y0 + 1) * scaleY, 1, false);
                }
            }
            if (copy)
                SDL_EndGPUCopyPass(copy);
            recordPassDraws(colour, s.depth >= 0 ? own[s.depth] : nullptr, ct.width, ct.height, draws, skins, vertexOffset);
        }

        // The picture.
        PresentPicture &picture = interp.pictures[interp.nextPicture];
        interp.nextPicture = (interp.nextPicture + 1u) % interp.pictures.size();
        SDL_GPUTexture *pic = presentTarget(picture, ps.w * scaleX, ps.h * scaleY);
        if (interp.leave.find('r') == std::string::npos)
            recordPresentPass(pic, own[src1 >= 0 ? src1 : src2], own[src2 >= 0 ? src2 : src1], ps.pu,
                              ps.mips && picture.levels > 1u);
        picture.mips = ps.mips && picture.levels > 1u;
        picture.presented = true;
        const auto tSubmit = std::chrono::steady_clock::now();
        interp.olderFence = std::move(interp.lastFence);
        interp.lastFence = submit(true);
        if (interp.timing && interp.lastFence && interp.lastFence->fence)
        {
            waitFence(interp.lastFence->fence);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tSubmit).count();
            const double cpu = std::chrono::duration<double, std::milli>(tSubmit - tGpu).count();
            interp.lastGpuMs = ms;
            interp.gpuMsSum += ms;
            interp.gpuMsMax = std::max(interp.gpuMsMax, ms);
            interp.cpuMsSum += cpu;
            interp.cpuMsMax = std::max(interp.cpuMsMax, cpu);
            ++interp.gpuTimed;
        }
        (void)t0;
        // The replay's textures back for the next one (the GPU uses them in submission order).
        for (size_t i = 0; i < own.size(); ++i)
        {
            const HeldTarget &h = b->targets[i];
            giveHeldTexture(own[i], h.width * scaleX, h.height * scaleY, heldFormat(h), h.depth);
        }
        for (const auto &[tex, shape] : copies)
            interp.free[shape].push_back(tex);
        ++interp.pictures_;
        out.texture = pic;
        out.width = ps.w;
        out.height = ps.h;
        out.scale = scaleX;
        out.levels = picture.mips ? picture.levels : 1u;
        return Skip::kCount;
    }

    // ---- housekeeping ----------------------------------------------------

    void resetAll()
    {
        if (!ok)
            return;
        submitAndWait();
        // Local memory is reset or new: owed pixels and CLUT entries go.
        for (OwedWriteBack &o : owed)
            retiringBuffers.push_back({o.tb, o.tbSize, o.fence});
        owed.clear();
        owedPages.clear();
        dropClutSnapshots();
        for (auto &t : targets)
            releaseAfterSubmit.push_back(t->tex);
        targets.clear();
        targetsChanged();
        for (auto &kv : textures)
            giveBackTexture(kv.second.tex);
        clearTextures();
        objTags.clear();
        objTagsChanged = true;
        clutEpoch++;
        markWrittenAll();
        submit(false);
    }

    void markWrittenAll()
    {
        GSMem::PageMask all;
        for (auto &w : all.bits)
            w = ~0ull;
        markWritten(all);
    }

    void reportStats()
    {
        const Stats s = stats;
        std::cout << "[gs-gpu] prims=" << s.primitives << " draws=" << s.drawCalls << " passes=" << s.renderPasses
                  << " slow=" << s.slowPrimitives << " cpu=" << s.cpuPrimitives << " texuploads=" << s.textureUploads
                  << " rt-down=" << s.targetDownloads << " (deferred " << s.deferredDownloads << ", waits " << s.owedWaits
                  << ") gpu-reads=" << s.gpuReads << " rt-up=" << s.targetUploads << " submits=" << s.submits
                  << " replaced=" << s.replacedPrimitives << " (by object " << s.replacedByObject << ", objects "
                  << objTags.size() << ")" << " repl-upload=" << s.replacementBytes
                  << " repl-gpu=" << replTextures.size() << "/" << (replGpuBytes >> 10) << "KB"
                  << " pack=" << s.packLookups << "/" << s.packHashes << " (lookups / hashes) " << (s.packNanoseconds / 1000) << "us"
                  << " targets=" << targets.size() << " textures=" << textures.size() << " native=" << s.nativeDraws
                  << "/" << s.nativeVertices << "v declined=" << s.nativeDeclined << " fog=" << s.fogPasses << " fxaa=" << s.fxaaPasses
                  << " native-upload=" << s.nativeMeshBytes
                  << " native-gpu=" << nativeMeshes.size() << "/" << (nativeGpuBytes >> 10) << "KB"
                  << " mip-copy=" << s.mipCopyPrimitives << " (deferred " << s.mipCopyDeferred << ") mip-upload="
                  << s.mipCopyBytes << " mip-gpu=" << mipCopies.size() << "/" << (mipCopyGpuBytes >> 10) << "KB"
                  << " tex-pool=" << poolHits << "/" << stockHits << "/" << poolMisses << " (reused / made ahead / made)"
                  << " mesh-chunks=" << meshChunks.size() << " memo=" << memoHits << std::endl;
        memoHits = 0;
    }
};

// ---- GSGpuBackend ----------------------------------------------------------

GSGpuBackend::GSGpuBackend(const GSGpuBackendOptions &options)
    : m_impl(std::make_unique<Impl>(options))
{
}

GSGpuBackend::~GSGpuBackend() = default;

bool GSGpuBackend::valid() const
{
    return m_impl->ok;
}

uint32_t GSGpuBackend::scale() const
{
    return m_impl->scaleX;
}

void GSGpuBackend::Initialize(uint8_t *vram, uint32_t vramSize)
{
    Impl::CallScope scope(*m_impl);
    if (m_impl->options.hostDevice && !m_impl->initTried)
    {
        m_impl->init();
        if (m_impl->ok)
            std::cerr << "[gs-gpu] GPU renderer, " << m_impl->scaleX << "x internal resolution"
                      << (m_impl->scaleY != m_impl->scaleX ? " (full height: " + std::to_string(m_impl->scaleY) + "x vertically), " : ", ")
                      << (m_impl->options.readback ? "private device, read back" : "shown from the window's GPU device")
                      << std::endl;
        else
            std::cerr << "[gs-gpu] no GPU device: the CPU renderer draws" << std::endl;
    }
    m_impl->cpu.Initialize(vram, vramSize);
    m_impl->vram = vram;
    m_impl->vramSize = vramSize;
    m_impl->resetAll();
}

void GSGpuBackend::Reset()
{
    Impl::CallScope scope(*m_impl);
    m_impl->fogHeld.clear();
    m_impl->fogNotes.clear();
    m_impl->syncAll();
    m_impl->cpu.Reset();
    m_impl->resetAll();
}

void GSGpuBackend::Submit(const GSPrimitiveBatch &batch)
{
    Impl::CallEntry scope(*m_impl, true);
    m_impl->drawThread = SDL_GetCurrentThreadID();
    m_impl->drawThreadId.store(m_impl->drawThread, std::memory_order_relaxed);
    if (!m_impl->ok)
    {
        m_impl->cpu.Submit(batch);
        return;
    }
    if (!m_impl->vram || batch.vertexCount == 0u)
        return;
    m_impl->submitPrimitive(batch);
}

void GSGpuBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut)
{
    Impl::CallEntry scope(*m_impl);
    m_impl->settleFog();
    Impl &im = *m_impl;
    if (im.ok && (isFourBitIndexedPsm(tex0.psm) || isEightBitIndexedPsm(tex0.psm)) && tex0.cld != 0u)
    {
        im.traceReason = "clut";
        if (im.trace)
        {
            static char reason[96];
            std::snprintf(reason, sizeof(reason), "clut cbp=%u cpsm=%u csm=%u csa=%u psm=%u cbw=%u cou=%u cov=%u", tex0.cbp, tex0.cpsm, tex0.csm,
                          tex0.csa, tex0.psm, texclut.cbw, texclut.cou, texclut.cov);
            im.traceReason = reason;
        }
        // The CLUT comes from local memory: write back what was drawn there.
        GSMem::PageMask pages;
        const bool fourBit = isFourBitIndexedPsm(tex0.psm);
        if (tex0.csm == 0u)
            GSMem::AddRectPages(pages, tex0.cpsm, tex0.cbp, 1u, 0u, 0u, 15u, fourBit ? 1u : 15u);
        else
        {
            const uint32_t width = texclut.cbw != 0u ? texclut.cbw : 1u;
            const uint32_t x0 = static_cast<uint32_t>(texclut.cou) << 4u;
            GSMem::AddRectPages(pages, tex0.cpsm, tex0.cbp, width, x0, texclut.cov, x0 + (fourBit ? 15u : 255u), texclut.cov);
        }
        // Deferred: a load from pages still owed by a write-back is done on
        // the GPU, from the target (see gatherClut); otherwise they are
        // materialized first.
        const bool due = im.cpu.ClutLoadDue(tex0);
        im.syncToCpu(pages, true);
        if (due && im.owedPages.intersects(pages))
        {
            if (im.gatherClut(tex0, texclut, pages))
            {
                ++im.clutEpoch;
                im.clutLoadValid = false;
                return;
            }
            im.materialize(pages);
        }
        if (due)
            im.noteCpuClutLoad(tex0, texclut);
        im.cpu.LoadClut(tex0, texclut);
        // Only a load changes the CLUT (TEX0 writes come with every
        // material; CLD 0 or a CBP already loaded change nothing).
        if (due)
        {
            ++im.clutEpoch;
            // Its source, for the palettes kept by it (pages written back
            // above, so their generation is the CLUT's).
            uint64_t gen = 0;
            for (uint32_t i = 0; i < kPageCount; ++i)
                if (pages.test(i))
                    gen = std::max(gen, im.pageGen[i]);
            im.clutLoad.source = static_cast<uint64_t>(tex0.cbp) | (static_cast<uint64_t>(tex0.cpsm) << 14) |
                                 (static_cast<uint64_t>(tex0.csm) << 18) | (static_cast<uint64_t>(tex0.csa) << 19) |
                                 (static_cast<uint64_t>(fourBit) << 24) |
                                 (tex0.csm ? (static_cast<uint64_t>(texclut.cbw) << 25) | (static_cast<uint64_t>(texclut.cou) << 31) |
                                                 (static_cast<uint64_t>(texclut.cov) << 37)
                                           : 0ull);
            im.clutLoad.gen = gen;
            im.clutLoad.select = Impl::paletteSelect(tex0);
            im.clutLoadValid = !im.clutOwned;
        }
        return;
    }
    im.cpu.LoadClut(tex0, texclut);
}

void GSGpuBackend::BeginTransfer(const GSTransferCommand &command)
{
    Impl::CallEntry scope(*m_impl);
    m_impl->settleFog();
    Impl &im = *m_impl;
    im.transfer = command;
    im.transferDest.clear();
    if (im.ok && command.trxreg.rrw != 0u && command.trxreg.rrh != 0u)
    {
        const auto &b = command.bitbltbuf;
        const auto &p = command.trxpos;
        const uint32_t w = command.trxreg.rrw, h = command.trxreg.rrh;
        GSMem::PageMask src;
        im.traceReason = "transfer";
        if (command.direction == 1u || command.direction == 2u)
        {
            GSMem::AddRectPages(src, b.spsm, b.sbp, std::max<uint32_t>(b.sbw, 1u), p.ssax, p.ssay, p.ssax + w - 1u, p.ssay + h - 1u);
            im.syncToCpu(src);
        }
        if (command.direction == 0u || command.direction == 2u)
        {
            GSMem::AddRectPages(im.transferDest, b.dpsm, b.dbp, std::max<uint32_t>(b.dbw, 1u), p.dsax, p.dsay, p.dsax + w - 1u,
                                p.dsay + h - 1u);
            im.syncToCpu(im.transferDest);
        }
        if (im.trace)
            std::cout << "[gs-gpu] transfer dir=" << command.direction << " dbp=" << b.dbp << " dbw=" << int(b.dbw) << " dpsm=" << int(b.dpsm)
                      << " " << w << "x" << h << " at " << p.dsax << "," << p.dsay << std::endl;
    }
    if (command.direction == 0u || command.direction == 2u)
    {
        // Texture notes under the destination go, but for the one this
        // upload is for (the game's own upload of a noted level).
        const auto &b = command.bitbltbuf;
        const auto &p = command.trxpos;
        uint32_t first = 0, last = 0;
        if (Impl::rectBlocks(b.dpsm, b.dbp, b.dbw, p.dsax, p.dsay, command.trxreg.rrw, command.trxreg.rrh, first, last))
            im.eraseObjTags(first, last, [&](const Impl::ObjTag &t)
                            { return command.direction == 0u && t.note.tbp == b.dbp && t.note.psm == b.dpsm && p.dsax == 0u &&
                                     p.dsay == 0u && t.note.width == command.trxreg.rrw &&
                                     t.note.height == command.trxreg.rrh; });
    }
    im.cpu.BeginTransfer(command);
    if (im.ok && command.direction == 2u)
    {
        im.markWritten(im.transferDest);
        im.markTargetsStale(im.transferDest);
    }
}

void GSGpuBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes)
{
    Impl::CallEntry scope(*m_impl);
    m_impl->settleFog();
    Impl &im = *m_impl;
    if (im.ok && !im.transferDest.empty())
        im.materialize(im.transferDest);
    im.cpu.UploadImage(data, sizeBytes);
    if (im.ok && !im.transferDest.empty())
    {
        im.markWritten(im.transferDest);
        im.markTargetsStale(im.transferDest);
    }
}

void GSGpuBackend::NoteTextureObjects(const GSTextureObjectNote *notes, size_t count)
{
    Impl::CallScope scope(*m_impl);
    m_impl->noteTextureObjects(notes, count);
}

bool GSGpuBackend::SupportsNative() const
{
    return m_impl->ok;
}

void GSGpuBackend::NoteFogPlanes(const GSFogPlanes &planes)
{
    Impl::CallScope scope(*m_impl);
    if (m_impl->ok)
        m_impl->noteFogPlanes(planes);
}

bool GSGpuBackend::SubmitNative(const GSNativeDraw &draw, const GSDrawState &state)
{
    Impl::CallEntry scope(*m_impl);
    m_impl->settleFog();
    m_impl->drawThread = SDL_GetCurrentThreadID();
    if (!m_impl->ok || !m_impl->vram || !draw.mesh || m_impl->nativeOff)
        return false;
    return m_impl->submitNative(draw, state);
}

void GSGpuBackend::BeginPacket()
{
    m_impl->beginPacket();
}

void GSGpuBackend::EndPacket()
{
    m_impl->endPacket();
}

void GSGpuBackend::Flush()
{
    Impl::CallScope scope(*m_impl);
    m_impl->settleFog();
    if (m_impl->ok)
        m_impl->submit(false);
}

void GSGpuBackend::WorkerIdle()
{
    if (!m_impl->threadOwnedCommands && m_impl->owed.empty())
        return;
    Impl::CallScope scope(*m_impl);
    if (m_impl->threadOwnedCommands && m_impl->cmd && m_impl->cmdThread == SDL_GetCurrentThreadID())
        m_impl->submit(false);
    // Owed write-backs the GPU has finished cost nothing to store now.
    if (m_impl->ok)
        m_impl->retireOwed();
}

void GSGpuBackend::TextureFlush()
{
    Impl::CallEntry scope(*m_impl);
    m_impl->settleFog();
    m_impl->cpu.TextureFlush();
}

void GSGpuBackend::Sync(GSSyncReason reason)
{
    Impl::CallScope scope(*m_impl);
    m_impl->settleFog();
    if (!m_impl->ok)
        return;
    m_impl->submit(false);
    if (reason == GSSyncReason::Presentation && m_impl->statsEnabled)
    {
        m_impl->reportStats();
        m_impl->stats = {};
    }
}

PresentationFrame GSGpuBackend::Present(const GSPresentationRequest &request)
{
    Impl::CallScope scope(*m_impl);
    m_impl->settleFog();
    if (!m_impl->ok)
        return m_impl->cpu.Present(request);
    m_impl->replUploadedThisFrame = 0;
    m_impl->packUploadedThisFrame = 0;
    m_impl->nativeUploadedThisFrame = 0;
    if ((++m_impl->poolFrame & 63u) == 0u)
    {
        m_impl->trimTexturePool(true);
        m_impl->trimStock();
    }
    m_impl->mipCopiesFrame();
    m_impl->sweepNativeMeshes();
    // Frame interpolation follows the setting from one presented frame to the next.
    m_impl->setInterpolation(ps2x::gs_interpolate::enabled());
    return m_impl->present(request);
}

ps2x::gs_interpolate::Skip GSGpuBackend::PresentBetween(uint64_t from, uint64_t to, float t, GSGpuPicture &out)
{
    Impl::CallScope scope(*m_impl);
    m_impl->settleFog();
    if (!m_impl->ok)
        return ps2x::gs_interpolate::Skip::NoPair;
    const ps2x::gs_interpolate::Skip why = m_impl->presentBetween(from, to, t, out);
    if (why != ps2x::gs_interpolate::Skip::kCount)
        m_impl->countSkip(why);
    return why;
}

bool GSGpuBackend::ReadBetweenPicture(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
{
    Impl::CallScope scope(*m_impl);
    Impl &im = *m_impl;
    const auto &picture = im.interp.pictures[(im.interp.nextPicture + im.interp.pictures.size() - 1u) % im.interp.pictures.size()];
    return im.readPicture(picture, rgba, width, height);
}

std::string GSGpuBackend::InterpolationReport()
{
    namespace gi = ps2x::gs_interpolate;
    Impl::CallScope scope(*m_impl);
    Impl::Interp &in = m_impl->interp;
    std::string line = "pictures=" + std::to_string(in.pictures_);
    for (size_t i = 0; i < static_cast<size_t>(gi::Skip::kCount); ++i)
        line += std::string(" ") + gi::skipName(static_cast<gi::Skip>(i)) + "=" + std::to_string(in.skips[i]);
    line += " | draws";
    for (size_t i = 0; i < static_cast<size_t>(gi::Draw::kCount); ++i)
        line += std::string(" ") + gi::drawName(static_cast<gi::Draw>(i)) + "=" + std::to_string(in.draws[i]);
    if (in.gpuTimed)
    {
        char buf[160];
        std::snprintf(buf, sizeof(buf), " | gpu ms mean=%.3f max=%.3f cpu ms mean=%.3f max=%.3f n=%llu",
                      in.gpuMsSum / static_cast<double>(in.gpuTimed), in.gpuMsMax,
                      in.cpuMsSum / static_cast<double>(in.gpuTimed), in.cpuMsMax, static_cast<unsigned long long>(in.gpuTimed));
        line += buf;
    }
    if (!in.made.empty() && !in.made.back()->replayable && in.made.back()->why)
        line += std::string(" | last frame not replayable: ") + in.made.back()->why;
    return line;
}

bool GSGpuBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba)
{
    Impl::CallScope scope(*m_impl);
    m_impl->settleFog();
    Impl &im = *m_impl;
    if (im.ok && context.frame.fbw != 0u && isColorPsm(context.frame.psm))
    {
        // As GSCpuBackend::ClearFramebuffer: the scissor rectangle of the
        // frame only, FBMSK and FBA apply. Drawn as an untextured sprite.
        GSPrimitiveBatch batch{};
        batch.vertexCount = 2;
        batch.state.context = context;
        batch.state.context.test = 1ull << 17; // ZTST ALWAYS, no alpha test
        batch.state.context.zbuf.zmask = true;
        batch.state.prim.type = GS_PRIM_SPRITE;
        batch.state.colclamp = 1;
        const float ox = static_cast<float>(context.xyoffset.ofx >> 4), oy = static_cast<float>(context.xyoffset.ofy >> 4);
        const uint32_t x1 = std::max<uint32_t>(context.scissor.x0, context.scissor.x1);
        const uint32_t y1 = std::max<uint32_t>(context.scissor.y0, context.scissor.y1);
        GSVertex &a = batch.vertices[0];
        GSVertex &b = batch.vertices[1];
        a.x = static_cast<float>(context.scissor.x0) + ox;
        a.y = static_cast<float>(context.scissor.y0) + oy;
        b.x = static_cast<float>(x1 + 1u) + ox;
        b.y = static_cast<float>(y1 + 1u) + oy;
        b.r = static_cast<uint8_t>(rgba);
        b.g = static_cast<uint8_t>(rgba >> 8);
        b.b = static_cast<uint8_t>(rgba >> 16);
        b.a = static_cast<uint8_t>(rgba >> 24);
        a.r = b.r; a.g = b.g; a.b = b.b; a.a = b.a;
        im.traceReason = "clear";
        im.submitPrimitive(batch);
        return true;
    }
    GSMem::PageMask pages;
    const uint32_t fbw = std::max<uint32_t>(context.frame.fbw, 1u);
    GSMem::AddRectPages(pages, context.frame.psm, context.frame.fbp << 5, fbw, context.scissor.x0, context.scissor.y0,
                        std::max(context.scissor.x0, context.scissor.x1), std::max(context.scissor.y0, context.scissor.y1));
    im.traceReason = "clear";
    if (im.ok)
        im.syncToCpu(pages);
    const bool done = im.cpu.ClearFramebuffer(context, rgba);
    im.cpu.Sync(GSSyncReason::DebugReadback);
    if (im.ok)
    {
        im.markWritten(pages);
        im.markTargetsStale(pages);
    }
    return done;
}

uint32_t GSGpuBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    Impl::CallScope scope(*m_impl);
    m_impl->settleFog();
    return m_impl->cpu.ConsumeLocalToHostBytes(dst, maxBytes);
}

uint32_t GSGpuBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    Impl::CallScope scope(*m_impl);
    m_impl->settleFog();
    Impl &im = *m_impl;
    im.traceReason = "readvram";
    if (im.ok)
    {
        GSMem::PageMask pages;
        GSMem::AddRectPages(pages, psm, base, std::max<uint32_t>(bw, 1u), x, y, x, y);
        im.syncToCpu(pages);
    }
    return im.cpu.ReadVram(psm, base, bw, x, y);
}

void GSGpuBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    Impl::CallScope scope(*m_impl);
    m_impl->settleFog();
    Impl &im = *m_impl;
    im.traceReason = "writevram";
    GSMem::PageMask pages;
    GSMem::AddRectPages(pages, psm, base, std::max<uint32_t>(bw, 1u), x, y, x, y);
    if (im.ok)
        im.syncToCpu(pages);
    im.cpu.WriteVram(psm, base, bw, x, y, value);
    im.eraseObjTagsInPages(pages);
    if (im.ok)
    {
        im.markWritten(pages);
        im.markTargetsStale(pages);
    }
}

void GSGpuBackend::SnapshotVram(std::vector<uint8_t> &out) const
{
    Impl::CallScope scope(*m_impl);
    m_impl->settleFog();
    m_impl->traceReason = "snapshot";
    if (m_impl->ok)
        m_impl->syncAll();
    m_impl->cpu.SnapshotVram(out);
}

GSTransferSnapshot GSGpuBackend::GetTransferSnapshot() const
{
    Impl::CallScope scope(*m_impl);
    return m_impl->cpu.GetTransferSnapshot();
}

void GSGpuBackend::SaveClutState(std::vector<uint8_t> &out) const
{
    Impl::CallScope scope(*m_impl);
    if (m_impl->ok)
        m_impl->materializeClut();
    m_impl->cpu.SaveClutState(out);
}

void GSGpuBackend::RestoreClutState(const std::vector<uint8_t> &in)
{
    Impl::CallScope scope(*m_impl);
    m_impl->cpu.RestoreClutState(in);
    m_impl->dropClutSnapshots();
    ++m_impl->clutEpoch;
    m_impl->clutLoadValid = false;
}

bool GSGpuBackend::LastPresentedScaled(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) const
{
    Impl::CallScope scope(*m_impl);
    if (m_impl->lastScaled.empty())
        return false;
    rgba = m_impl->lastScaled;
    width = m_impl->lastScaledW;
    height = m_impl->lastScaledH;
    return true;
}

bool GSGpuBackend::PresentedGpuPicture(GSGpuPicture &out, uint32_t slot) const
{
    Impl::CallScope scope(*m_impl);
    const Impl &im = *m_impl;
    const Impl::PresentPicture &picture = im.pictures[std::min<uint32_t>(slot, kPresentationSlots - 1u)];
    if (!im.ok || !picture.presented || !picture.tex || im.options.readback)
        return false;
    out.texture = picture.tex;
    out.width = picture.w;
    out.height = picture.h;
    out.scale = im.scaleX;
    out.levels = picture.mips ? picture.levels : 1u;
    return true;
}

bool GSGpuBackend::ReadPresentedGpuPicture(uint32_t slot, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
{
    Impl::CallScope scope(*m_impl);
    return m_impl->readPresented(slot, rgba, width, height);
}

GSGpuBackend::Stats GSGpuBackend::TakeStats()
{
    Impl::CallScope scope(*m_impl);
    Stats s = m_impl->stats;
    m_impl->stats = {};
    return s;
}

std::unique_ptr<GSRasterBackend> createGpuRasterBackendFromEnvironment()
{
    const char *backend = ps2x::boot::text("PS2X_GS_BACKEND");
    if (!backend || std::strcmp(backend, "gpu") != 0)
        return nullptr;
    GSGpuBackendOptions options;
    // The game window's device: its picture goes to the window as it is.
    options.hostDevice = true;
    if (const char *s = ps2x::boot::text("PS2X_GS_SCALE"))
        options.scale = static_cast<uint32_t>(std::max(1L, std::strtol(s, nullptr, 0)));
    options.fullHeight = envFlag("PS2X_GS_FULL_HEIGHT");
    options.debug = envFlag("PS2X_GS_GPU_DEBUG");
    if (const char *file = ps2x::boot::text("PS2X_GS_PIPELINE_CACHE"))
        options.pipelineFile = file;
    return std::make_unique<GSGpuBackend>(options);
}
