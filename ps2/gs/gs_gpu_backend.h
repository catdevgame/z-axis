#pragma once

// Hardware GS renderer on SDL_GPU (prototype). Design and limits:
// docs/GPU_BACKEND.md.
//
// Draws go to GPU render targets that stand in for GS frame and Z buffers,
// optionally at an integer upscale factor. Everything else (image
// transfers, CLUT loads, VRAM reads and writes) is done on the GS local
// memory by a wrapped GSCpuBackend; render targets are written back to local
// memory when something reads the pages they own, and reloaded from it when
// something else writes there.

#include "gs/gs_backend.h"

#include <cstdint>
#include <string>
#include <memory>
#include <vector>

struct SDL_GPUDevice;

struct GSGpuBackendOptions
{
    // Internal resolution multiplier (1 = native GS resolution).
    uint32_t scale = 1;
    // Twice that vertically: games that draw 224-line field frames (shown
    // line-doubled) get as much detail vertically as horizontally.
    bool fullHeight = false;
    // Device to render with; nullptr creates a private one (headless, no
    // window needed). A device passed in must accept MSL shaders.
    SDL_GPUDevice *device = nullptr;
    // Present() reads the picture back: pixels in the returned frame and
    // LastPresentedScaled(). Off (a device shared with the host window):
    // the picture stays in a texture (PresentedGpuPicture()), shown without
    // a GPU round trip.
    bool readback = true;
    // Draw on the host window's device (ps2_host::gpuDevice(), without
    // readback), taken at Initialize(): the backend is made before the
    // window. No window: a private device with readback. No device at all:
    // the CPU renderer inside the backend draws.
    bool hostDevice = false;
    // SDL_GPU validation layer (only for a private device).
    bool debug = false;
    // A game's pipeline file (ZX_GS_PIPELINE_CACHE; the app gives each
    // game its own): the pipelines it made are made again ahead, on a
    // thread of their own, at the next start. Empty: made at first use.
    std::string pipelineFile;
};

class GSGpuBackend final : public GSRasterBackend
{
public:
    explicit GSGpuBackend(const GSGpuBackendOptions &options = {});
    ~GSGpuBackend() override;

    // False if no SDL_GPU device could be created (the CPU renderer inside
    // the backend then draws). With hostDevice, known after Initialize().
    bool valid() const;
    uint32_t scale() const;

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;
    void NoteTextureObjects(const GSTextureObjectNote *notes, size_t count) override;
    bool SupportsNative() const override;
    bool SubmitNative(const GSNativeDraw &draw, const GSDrawState &state) override;
    void NoteFogPlanes(const GSFogPlanes &planes) override;
    void BeginPacket() override;
    void EndPacket() override;

    void Flush() override;
    void WorkerIdle() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    // Returns the picture at native resolution (as GSCpuBackend does); the
    // upscaled one is kept for LastPresentedScaled(). Without readback, a
    // frame without pixels (PresentationFrame::onGpu).
    PresentationFrame Present(const GSPresentationRequest &request) override;
    bool PresentedGpuPicture(GSGpuPicture &out, uint32_t slot) const override;
    bool ReadPresentedGpuPicture(uint32_t slot, std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) override;
    zx::gs_interpolate::Skip PresentBetween(uint64_t from, uint64_t to, float t, GSGpuPicture &out) override;
    bool ReadBetweenPicture(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) override;
    std::string InterpolationReport() override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
    void SaveClutState(std::vector<uint8_t> &out) const override;
    void RestoreClutState(const std::vector<uint8_t> &in) override;

    // The last picture Present() made, at the internal resolution, RGBA8,
    // tightly packed. False if there is none (or without readback).
    bool LastPresentedScaled(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height) const;

    struct Stats
    {
        uint64_t primitives = 0;
        uint64_t drawCalls = 0;
        uint64_t renderPasses = 0;
        uint64_t slowPrimitives = 0;   // drawn with the destination copied (shader blending)
        uint64_t cpuPrimitives = 0;    // drawn by the CPU rasterizer
        uint64_t textureUploads = 0;
        uint64_t targetDownloads = 0;  // render targets written back to local memory
        uint64_t targetUploads = 0;    // render targets (re)loaded from local memory
        uint64_t submits = 0;
        uint64_t deferredDownloads = 0; // write-backs whose pixels reach local memory later (part of targetDownloads)
        uint64_t gpuReads = 0;          // CLUT loads and 8-bit textures read from a target on the GPU
        uint64_t owedWaits = 0;         // waits for an owed write-back or CLUT snapshot
        uint64_t supersededDownloads = 0; // deferred write-backs a later one of the same target made unneeded
        uint64_t replacedPrimitives = 0; // drawn with a replacement picture (runtime/texture_replace.h)
        uint64_t replacementBytes = 0;   // replacement pictures uploaded
        uint64_t packLookups = 0;        // texture pack lookups (a decode, palette or pack set changed)
        uint64_t packHashes = 0;         // texture pack names hashed
        uint64_t packNanoseconds = 0;    // time naming and looking up (texture_pack)
        uint64_t replacedByObject = 0;   // of those, found by the texture's identity (GSTextureObjectNote)
        uint64_t nativeDraws = 0;        // native draws (SubmitNative)
        uint64_t nativeVertices = 0;     // their vertices
        uint64_t nativeDeclined = 0;     // left to GS::drawNative's primitives
        uint64_t nativeMeshBytes = 0;    // native meshes uploaded
        uint64_t fogPasses = 0;          // smooth fog in place of a game's fog planes (gs_quality Fog)
        uint64_t fxaaPasses = 0;         // anti-aliased views (gs_quality AntiAliasing)
        uint64_t mipCopyPrimitives = 0;  // native draws of a PS2 texture's mipmapped copy (gs_quality Anisotropy)
        uint64_t mipCopyBytes = 0;       // those copies uploaded
        uint64_t mipCopyDeferred = 0;    // drawn as the GS does: the frame's budget for copies spent
    };
    // Counters since the last call (ZX_GS_STATS=1 prints them per frame).
    Stats TakeStats();

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

// The GPU backend if ZX_GS_BACKEND=gpu (ZX_GS_SCALE=n for the
// resolution, ZX_GS_FULL_HEIGHT=1 for twice that vertically), else nullptr. Draws on the host window's GPU device if there
// is one (presenting without readback), else on a private one. Only in
// builds with the GPU backend (ZX_HAS_GS_GPU).
std::unique_ptr<GSRasterBackend> createGpuRasterBackendFromEnvironment();
