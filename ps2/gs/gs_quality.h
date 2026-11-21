#pragma once

// Picture-quality options of the GPU renderer that go beyond what the PS2
// could do (the "PC look"): anti-aliasing, texture filtering, smooth fog,
// shadows, depth precision, perspective-correct shading, the Xbox release's
// character lighting. Each is a setting
// (settings.ini [graphics], a Display row in the game's Settings pages, an
// environment variable for the developer runner and the tests).
//
// They only change how the GPU renderer draws (mostly its native draws,
// gs/gs_native.h); the game's memory and timing never depend on
// them, the CPU renderer ignores them (it stays the bit-exact reference),
// and the strict render path (ZX_RENDER_PATH=strict, the logic checks'
// and PCSX2 comparisons' mode) draws as the console whatever they are.
//
// Values are plain integers read from any thread (atomics): the backend
// and the game overrides read them per frame or per draw, so changes apply
// at once.

#include "runtime/settings.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace zx::gs_quality
{
    enum class Option : int
    {
        AntiAliasing = 0, // 0 off, 1 FXAA (on the 3D picture, before the HUD)
        Anisotropy,       // 0 off (the game's filter), 1..4: trilinear and 2x/4x/8x/16x anisotropic
        Fog,              // 0 the game's eight fog planes, 1 smooth per-pixel fog
        Shadows,          // 0 as before, 1 sharp: shadow volume counters with exact flat colours (GPU_BACKEND.md)
        DepthPrecision,   // 0 the GS's 16-bit steps, 1 full float depth for native meshes
        Perspective,      // 0 as the PS2 (screen-linear colour, affine skinned textures), 1 perspective-correct
        Lighting,         // characters: 0 the PS2's lights, 1 the Xbox release's (GPU_BACKEND.md)
        DepthTest,        // 0 exact (the shader rounds Z to the GS's steps), 1 fast (16-bit Z as D16, GPU_BACKEND.md)
        Count
    };
    constexpr size_t kOptionCount = static_cast<size_t>(Option::Count);

    struct OptionInfo
    {
        Option option;
        const char *settingKey;  // zx::settings key ("display.anti_aliasing")
        const char *iniKey;      // settings.ini [graphics] key
        const char *env;         // environment variable (a number or a value name)
        const char *label;       // the Display row
        const char *help;
        std::vector<std::string> choices; // settings.ini value names, lower case
        std::vector<std::string> labels;  // the row's value names
        int defaultValue;
    };

    // The defaults, in Option order.
    std::array<int, kOptionCount> defaults();

    // Every option, in Option order.
    const std::vector<OptionInfo> &options();
    const OptionInfo &info(Option option);

    int get(Option option);
    void set(Option option, int value); // clamped to the choices

    // Convenience readers.
    inline bool fxaa() { return get(Option::AntiAliasing) == 1; }
    // Anisotropy level for samplers (1 = off; 2, 4, 8, 16).
    inline int anisotropy()
    {
        const int v = get(Option::Anisotropy);
        return v <= 0 ? 1 : (1 << v);
    }
    inline bool smoothFog() { return get(Option::Fog) == 1; }
    inline bool nativeShadows() { return get(Option::Shadows) == 1; }
    inline bool preciseDepth() { return get(Option::DepthPrecision) == 1; }
    inline bool perspectiveCorrect() { return get(Option::Perspective) == 1; }
    inline bool xboxLighting() { return get(Option::Lighting) == 1; }
    inline bool fastDepth() { return get(Option::DepthTest) == 1; }

    // A value from settings.ini or the environment: a choice name ("fxaa"),
    // or its index. False if it is neither.
    bool parseValue(Option option, const std::string &text, int &out);
    // The settings.ini name of a value.
    std::string valueName(Option option, int value);

    // Applies the environment variables that are set (the developer runner
    // and the tests; the app's settings.ini values are applied first, so the
    // environment wins). Called by the GPU backend when it is made.
    void applyEnvironment();

    // The Display rows. `save` (may be null) is called after a change with
    // the option and its new value (the app writes settings.ini). Rows whose
    // key is registered already are left alone. `sessionRows`: rows of the
    // game's session (zx::session::addSetting), gone when the game is
    // left, so the next game's page gets them again in its own order (the
    // app; the developer runner's rows stay).
    void addSettings(std::function<void(Option, int)> save, bool sessionRows = false);
}
