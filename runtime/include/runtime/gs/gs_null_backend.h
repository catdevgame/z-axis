#pragma once

#include "runtime/gs/gs_backend.h"

#include <atomic>
#include <cstdint>
#include <memory>

// Raster backend that keeps every GS memory semantic (image transfers in
// both directions, local-to-local copies, CLUT loads, framebuffer clears,
// VRAM reads/writes, presentation of whatever is in local memory) but drops
// drawn primitives. Selected with ZX_GS_NULL=1 so game logic can run at
// full speed when only guest state matters (e.g. comparing against a
// reference run). Everything except Submit() forwards to a wrapped backend.
class GSNullBackend final : public GSRasterBackend
{
public:
    explicit GSNullBackend(std::unique_ptr<GSRasterBackend> inner);

    void Initialize(uint8_t *vram, uint32_t vramSize) override;
    void Reset() override;

    void Submit(const GSPrimitiveBatch &batch) override;
    void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) override;
    void BeginTransfer(const GSTransferCommand &command) override;
    void UploadImage(const uint8_t *data, uint32_t sizeBytes) override;

    void Flush() override;
    void WorkerIdle() override;
    void TextureFlush() override;
    void Sync(GSSyncReason reason) override;
    PresentationFrame Present(const GSPresentationRequest &request) override;

    bool ClearFramebuffer(const GSContext &context, uint32_t rgba) override;
    uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) override;

    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const override;
    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) override;
    void SnapshotVram(std::vector<uint8_t> &out) const override;
    GSTransferSnapshot GetTransferSnapshot() const override;
    void SaveClutState(std::vector<uint8_t> &out) const override;
    void RestoreClutState(const std::vector<uint8_t> &in) override;

    uint64_t droppedPrimitives() const { return m_dropped.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<GSRasterBackend> m_inner;
    std::atomic<uint64_t> m_dropped{0};
};

// The raster backend selected by the environment: GSNullBackend around a
// GSCpuBackend with ZX_GS_NULL=1, else the first registered factory's
// backend (if it returns one), else a plain GSCpuBackend.
std::unique_ptr<GSRasterBackend> createConfiguredRasterBackend();

// ZX_GS_NULL is set and not 0.
bool gsNullRequested();

// Backends linked into the executable register a factory here (the SDL_GPU
// one: src/lib/gs/gs_gpu_register.cpp, ZX_GS_BACKEND=gpu). A factory
// returns nullptr when it is not selected.
using GSRasterBackendFactory = std::unique_ptr<GSRasterBackend> (*)();
void registerRasterBackendFactory(GSRasterBackendFactory factory);
