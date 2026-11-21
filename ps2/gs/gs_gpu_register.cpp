// Makes the SDL_GPU GS backend selectable in the runner with
// ZX_GS_BACKEND=gpu (ZX_GS_SCALE=n). Compiled into the executable so the
// registration is not dropped by the linker.
#include "gs/gs_gpu_backend.h"
#include "gs/gs_null_backend.h"

namespace
{
    const bool registered = []()
    {
        registerRasterBackendFactory(&createGpuRasterBackendFromEnvironment);
        return true;
    }();
}
