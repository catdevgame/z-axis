#pragma once

#include "runtime/settings_registry.h"

#include <functional>

class PS2Machine;

namespace aggressive_inline
{
    // Native drawing of the game's level meshes: the rigid prelit meshes (render type 4, 93% of every level's
    // meshes) and the terrain with its grass shells (type 0x20) are drawn by
    // the GPU renderer from host vertex buffers in place of their VU1 work,
    // at the same place in the GS stream, with the game's own state. Game
    // RAM and EE timing are unchanged.
    //
    // Render path (ZX_RENDER_PATH, settings.ini [graphics] render_path):
    //   native  native draws (the default; with the CPU renderer the same
    //           triangles go through as GS primitives)
    //   strict  every mesh through VIF1, VU1 and the GS as on the PS2
    //   records native draws made by the readable class renderers from their
    //           draw records (zge/host/class_draws.cpp, engine/host/
    //           native_draws.h): no VU1 work in the mesh packet; with either
    //           renderer (the CPU one gets GS primitives)
    // Unset: native with a renderer that draws them (the GPU renderer),
    // strict otherwise (the CPU renderer stays the bit-exact reference).
    void installNativeDraw(PS2Machine &runtime);

    // The Display row ("Native Meshes"): live switch between the two paths.
    zx::settings::Setting nativeDrawSetting(std::function<void(bool)> save);
}
