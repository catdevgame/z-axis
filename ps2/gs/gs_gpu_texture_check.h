#pragma once
// SDL_GPU texture handles, created and released in one place, with a
// validation mode for handles used after their release.
//
// SDL_ReleaseGPUTexture frees the handle (the texture's container) at once
// on Direct3D 12 and Metal; only the GPU memory under it waits for the
// command buffers that use it. A handle bound, blitted, copied or drawn to
// after its release reads freed memory: on Direct3D 12 that is the access
// violation in D3D12_BindFragmentSamplers (BRINGUP "The intro-movie quit").
//
// ZX_GPU_TEXTURE_CHECK=1 (a developer check, off by default): released
// handles are kept (not given to SDL) for about 2 s, and every use of one
// through the calls below is reported on stderr with the stack of the use
// and the stack of the release ("[gpu-check] ..."); so is a sampled or
// copied handle that was never made here (garbage, or released long ago)
// and a null one. Without it the calls are SDL's, with one test of a flag.
//
// Every SDL_GPUTexture the GS backend and the host window make or use goes
// through here (gs_gpu_backend.cpp, host_sdl.cpp).
#include <SDL3/SDL_gpu.h>

#include <cstdint>

namespace zx::gpu_texture_check
{
    bool enabled();

    SDL_GPUTexture *createTexture(SDL_GPUDevice *device, const SDL_GPUTextureCreateInfo *info);
    // With the check on: kept for holdSeconds, then released.
    void releaseTexture(SDL_GPUDevice *device, SDL_GPUTexture *texture);
    // Gives SDL every handle still kept for the device (before it is
    // destroyed).
    void releaseHeld(SDL_GPUDevice *device);

    // What a use needs of its texture: MadeHere, one made here and not
    // released (the GS backend's and the window's own textures); Present,
    // any texture but null (render targets, which may be the swapchain's);
    // Optional, null allowed.
    enum class Need
    {
        MadeHere,
        Present,
        Optional,
    };
    // False (and reported, with what) when the texture was released, is
    // null, or (MadeHere) was never made here.
    bool check(const SDL_GPUTexture *texture, const char *what, Need need = Need::MadeHere);

    // With the check on: a token to put in place of the handle of an owner
    // that goes (a GS target), which the owner's code keeps a while instead
    // of freeing it. A use of the token (through a stale pointer to the
    // owner) is reported with the stack of the owner's end and is not
    // passed to SDL. Null with the check off.
    SDL_GPUTexture *ownerGone(const char *owner);

    // SDL's calls, each texture they name checked first.
    void bindFragmentSamplers(SDL_GPURenderPass *pass, Uint32 firstSlot, const SDL_GPUTextureSamplerBinding *bindings,
                              Uint32 count);
    SDL_GPURenderPass *beginRenderPass(SDL_GPUCommandBuffer *commands, const SDL_GPUColorTargetInfo *colorTargets,
                                       Uint32 colorCount, const SDL_GPUDepthStencilTargetInfo *depthTarget);
    void blitTexture(SDL_GPUCommandBuffer *commands, const SDL_GPUBlitInfo *info);
    void copyTextureToTexture(SDL_GPUCopyPass *pass, const SDL_GPUTextureLocation *source,
                              const SDL_GPUTextureLocation *destination, Uint32 w, Uint32 h, Uint32 d, bool cycle);
    void uploadToTexture(SDL_GPUCopyPass *pass, const SDL_GPUTextureTransferInfo *source,
                         const SDL_GPUTextureRegion *destination, bool cycle);
    void downloadFromTexture(SDL_GPUCopyPass *pass, const SDL_GPUTextureRegion *source,
                             const SDL_GPUTextureTransferInfo *destination);
    void generateMipmaps(SDL_GPUCommandBuffer *commands, SDL_GPUTexture *texture);

    struct Counts
    {
        uint64_t created = 0;
        uint64_t released = 0;
        uint64_t held = 0;    // released, not yet given to SDL
        uint64_t misuses = 0; // uses of a released handle
    };
    Counts counts();

    // Unit tests: the check on or off, how long a handle is kept, and what
    // gives a handle to SDL (null: SDL_ReleaseGPUTexture). Clears the
    // counts and the kept handles (without releasing them).
    void configureForTest(bool on, double holdSeconds, void (*release)(SDL_GPUDevice *, SDL_GPUTexture *));
}
