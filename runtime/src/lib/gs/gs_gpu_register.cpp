// Makes the SDL_GPU GS backend selectable in the runner with
// PS2X_GS_BACKEND=gpu (PS2X_GS_SCALE=n). Compiled into the executable so the
// registration is not dropped by the linker.
#include "runtime/gs/gs_gpu_backend.h"
#include "runtime/gs/gs_null_backend.h"

namespace
{
    const bool registered = []()
    {
        registerRasterBackendFactory(&createGpuRasterBackendFromEnvironment);
        return true;
    }();
}
