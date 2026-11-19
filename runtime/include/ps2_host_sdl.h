#pragma once

// SDL-typed side of the host layer (ps2_host_backend.h), for code that
// draws on top of the game picture or wants the raw events (the debug UI).

#include <SDL3/SDL.h>

#include <functional>

namespace ps2_host
{
    SDL_Window *sdlWindow();
    SDL_GPUDevice *gpuDevice();
    SDL_GPUTextureFormat swapchainFormat();

    // Other users of gpuDevice() (the GPU GS renderer) release their
    // resources in these hooks, which shutdown() runs before it destroys
    // the device. Any thread.
    int addGpuDeviceReleaseHook(std::function<void()> hook);
    void removeGpuDeviceReleaseHook(int id);

    // Every event pumpEvents sees, before the host layer handles it.
    using EventHook = std::function<void(const SDL_Event &event)>;
    void setEventHook(EventHook hook);

    // Drawn over the game picture: prepare runs on the frame's command
    // buffer before any pass (uploads), render inside a pass on the
    // swapchain texture.
    struct Overlay
    {
        std::function<void(SDL_GPUCommandBuffer *commands)> prepare;
        std::function<void(SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass)> render;
    };
    void setOverlay(Overlay overlay);
}
