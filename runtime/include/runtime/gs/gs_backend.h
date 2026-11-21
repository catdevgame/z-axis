#pragma once

#include "runtime/gs/gs_interpolate.h"
#include "runtime/gs/gs_native.h"
#include "runtime/gs/gs_types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_GPUTexture;

// A presented picture kept in a texture on the host window's SDL_GPU device.
struct GSGpuPicture
{
    SDL_GPUTexture *texture = nullptr;
    uint32_t width = 0;  // texels: the frame's size times scale
    uint32_t height = 0;
    uint32_t scale = 1;
    // Mipmap levels with the picture (box-filtered halvings, for a window
    // much smaller than the picture; 1: only the picture).
    uint32_t levels = 1;
};

// A texture of the game's own (one of its texture objects, or one mip level
// of it) that the GS holds at a block range from now on, as a game override
// tells it (GS::noteTextureObjects), in stream order with the uploads and
// draws. A renderer can draw a host picture for it, found by identity rather
// than by what GS memory holds (runtime/ps2_texture_replace.h; Aggressive
// Inline: the Xbox release's textures). The note holds until GS memory in its
// range is written by anything but its own upload, or another note covers it.
struct GSTextureObjectNote
{
    uint32_t object = 0;   // the game's texture object (guest address; identity, logs)
    uint32_t tbp = 0;      // this level's block address
    uint32_t blocks = 0;   // blocks allocated to it
    uint16_t width = 0;    // this level's size in texels
    uint16_t height = 0;
    uint8_t psm = 0;
    uint8_t level = 0;     // 0: the texture, k: its mip level k
    // The host picture's key: the texture's (level 0's) size and index hash.
    uint16_t keyWidth = 0;
    uint16_t keyHeight = 0;
    uint64_t keyHash = 0;
    char name[24] = {};    // the game's name for it (logs)
};

class GSRasterBackend
{
public:
    virtual ~GSRasterBackend() = default;

    virtual void Initialize(uint8_t *vram, uint32_t vramSize) = 0;
    virtual void Reset() = 0;

    virtual void Submit(const GSPrimitiveBatch &batch) = 0;
    virtual void LoadClut(const GSTex0Reg &tex0, const GSTexClutReg &texclut) = 0;

    virtual void BeginTransfer(const GSTransferCommand &command) = 0;
    virtual void UploadImage(const uint8_t *data, uint32_t sizeBytes) = 0;
    // Side-band identity of textures (see GSTextureObjectNote); ignored by
    // backends that draw only what GS memory holds.
    virtual void NoteTextureObjects(const GSTextureObjectNote *, size_t) {}

    // Native draws (runtime/gs/gs_native.h). SupportsNative: the backend
    // draws them itself (any thread may ask). SubmitNative: draws one with
    // `state`, the GS state at its place in the stream (the active context,
    // PRIM's attributes, TEXA, fog colour); false if it did not, and
    // GS::drawNative sends its triangles as primitives instead.
    virtual bool SupportsNative() const { return false; }
    virtual bool SubmitNative(const GSNativeDraw &, const GSDrawState &) { return false; }
    // A game's fog planes are coming next in the stream (GSFogPlanes,
    // runtime/gs/gs_native.h). Ignored by backends without native draws.
    virtual void NoteFogPlanes(const GSFogPlanes &) {}

    // Deferred: the backend may keep draws queued until something observes
    // local memory through it (any call other than Submit/LoadClut/
    // TextureFlush). Otherwise the caller's VRAM buffer is current when each
    // call returns. Off by default.
    virtual void SetDeferred(bool) {}

    // The calls of one GIF packet come between these two, on the thread
    // that feeds the backend (pairs may nest). A backend with a lock can
    // keep it over the packet instead of taking it per primitive, as long
    // as it lets other threads that wait for it in between calls.
    virtual void BeginPacket() {}
    virtual void EndPacket() {}

    virtual void Flush() = 0;
    // The thread that feeds the backend has nothing queued for now. A
    // backend with GPU commands recorded on that thread submits them
    // (SDL_GPU uses a command buffer only on the thread that acquired it).
    virtual void WorkerIdle() {}
    virtual void TextureFlush() = 0;
    virtual void Sync(GSSyncReason reason) = 0;
    virtual PresentationFrame Present(const GSPresentationRequest &request) = 0;

    virtual bool ClearFramebuffer(const GSContext &context, uint32_t rgba) = 0;
    virtual uint32_t ConsumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes) = 0;

    virtual uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const = 0;
    virtual void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value) = 0;
    virtual void SnapshotVram(std::vector<uint8_t> &out) const = 0;
    virtual GSTransferSnapshot GetTransferSnapshot() const = 0;

    // Opaque CLUT buffer state, used by GS trace recording/replay
    // (gs_record.cpp). Backends without a CPU-side CLUT can ignore these.
    virtual void SaveClutState(std::vector<uint8_t> &out) const { out.clear(); }
    virtual void RestoreClutState(const std::vector<uint8_t> &) {}

    // Backends drawing on the host window's GPU device (GSGpuBackend) can
    // leave the picture there: Present() then returns a frame without
    // pixels (PresentationFrame::onGpu) and the picture last presented into
    // a slot (GSPresentationRequest::slot) is this texture, valid until the
    // next Present() into the same slot.
    virtual bool PresentedGpuPicture(GSGpuPicture &, uint32_t /*slot*/) const { return false; }
    // That picture read back (screenshots), RGBA8, tightly packed, at the
    // internal resolution.
    virtual bool ReadPresentedGpuPicture(uint32_t /*slot*/, std::vector<uint8_t> &, uint32_t &, uint32_t &) { return false; }

    // Frame interpolation (runtime/gs/gs_interpolate.h): the picture `t` of the way from frame `from` to frame `to`
    // of the frame queue (GSPresentationRequest::frame; both presented with interpolation on), left on the GPU as
    // a presented picture is, valid until the next but one; Skip::kCount when made, else why not. Backends that
    // keep no frames: NoPair.
    virtual zx::gs_interpolate::Skip PresentBetween(uint64_t /*from*/, uint64_t /*to*/, float /*t*/, GSGpuPicture &)
    {
        return zx::gs_interpolate::Skip::NoPair;
    }
    // The last picture PresentBetween made, read back (RGBA8, internal resolution).
    virtual bool ReadBetweenPicture(std::vector<uint8_t> &, uint32_t &, uint32_t &) { return false; }
    // Its counts since the last call (a log line), and the GPU time of pictures between when timed
    // (ZX_INTERP_TIMING=1).
    virtual std::string InterpolationReport() { return {}; }
};
