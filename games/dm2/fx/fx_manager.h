#pragma once

// DM2's special effects manager (SpecialFX/zfxMgr.c; fx_manager.cpp): a list
// of effects, each with the function that draws it, drawn by a task every
// frame. The one kind of effect it makes is the lens flare (lens_flare.h).

#include "lens_flare.h"

#include "ee/ee.h"

#include <cstdint>

namespace game::dm2
{
    // newManagedFx's kind.
    constexpr uint32_t kManagedLensFlare = 1;

    // The manager's list made empty and its draw task started.
    void initFxManager();
    // An effect of `kind` made and listed; 0 for any other kind than the
    // lens flare, or before the manager's start.
    ee::Ptr<LensFlare> newManagedFx(uint32_t kind);
    // The task's frame: every listed effect drawn. Returns 1.
    uint32_t drawManagedFx();
    // The draw task deleted. Returns 1.
    // Kept from the original: the list and its effects stay as they are.
    uint32_t shutdownFxManager();
}
