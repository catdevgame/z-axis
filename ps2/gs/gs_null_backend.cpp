#include "gs/gs_null_backend.h"
#include "gs/gs_cpu_backend.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

GSNullBackend::GSNullBackend(std::unique_ptr<GSRasterBackend> inner)
    : m_inner(std::move(inner))
{
}

void GSNullBackend::Initialize(uint8_t *vram, uint32_t vramSize) { m_inner->Initialize(vram, vramSize); }
void GSNullBackend::Reset() { m_inner->Reset(); }

void GSNullBackend::Submit(const GSPrimitiveBatch &batch)
{
    if (batch.vertexCount != 0u)
        m_dropped.fetch_add(1u, std::memory_order_relaxed);
}

void GSNullBackend::LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) { m_inner->LoadClut(tex0, texclut); }
void GSNullBackend::BeginTransfer(const GSTransferCommand &command) { m_inner->BeginTransfer(command); }
void GSNullBackend::UploadImage(const uint8_t *data, uint32_t sizeBytes) { m_inner->UploadImage(data, sizeBytes); }
void GSNullBackend::Flush() { m_inner->Flush(); }
void GSNullBackend::WorkerIdle() { m_inner->WorkerIdle(); }
void GSNullBackend::TextureFlush() { m_inner->TextureFlush(); }
void GSNullBackend::Sync(GSSyncReason reason) { m_inner->Sync(reason); }
PresentationFrame GSNullBackend::Present(const GSPresentationRequest &request) { return m_inner->Present(request); }
bool GSNullBackend::ClearFramebuffer(const GSContext &context, uint32_t rgba) { return m_inner->ClearFramebuffer(context, rgba); }
uint32_t GSNullBackend::ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) { return m_inner->ConsumeLocalToHostBytes(dst, maxBytes); }

uint32_t GSNullBackend::ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const
{
    return m_inner->ReadVram(psm, base, bw, x, y);
}

void GSNullBackend::WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value)
{
    m_inner->WriteVram(psm, base, bw, x, y, value);
}

void GSNullBackend::SnapshotVram(std::vector<uint8_t> &out) const { m_inner->SnapshotVram(out); }
GSTransferSnapshot GSNullBackend::GetTransferSnapshot() const { return m_inner->GetTransferSnapshot(); }
void GSNullBackend::SaveClutState(std::vector<uint8_t> &out) const { m_inner->SaveClutState(out); }
void GSNullBackend::RestoreClutState(const std::vector<uint8_t> &in) { m_inner->RestoreClutState(in); }

namespace
{
    GSRasterBackendFactory &registeredFactory()
    {
        static GSRasterBackendFactory factory = nullptr;
        return factory;
    }
}

void registerRasterBackendFactory(GSRasterBackendFactory factory)
{
    registeredFactory() = factory;
}

bool gsNullRequested()
{
    const char *value = std::getenv("ZX_GS_NULL");
    return value && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

std::unique_ptr<GSRasterBackend> createConfiguredRasterBackend()
{
    static const bool useNull = []()
    {
        const bool enabled = gsNullRequested();
        if (enabled)
            std::fprintf(stderr, "[gs] ZX_GS_NULL: primitives are not rasterized\n");
        return enabled;
    }();
    if (useNull)
        return std::make_unique<GSNullBackend>(std::make_unique<GSCpuBackend>());
    if (GSRasterBackendFactory factory = registeredFactory())
        if (std::unique_ptr<GSRasterBackend> backend = factory())
            return backend;
    return std::make_unique<GSCpuBackend>();
}
