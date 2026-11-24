#pragma once

// Practice in the collection: a restart point and a timer, off by default
// (the games' side: engine/overrides/zge_practice.h). One row for the app,
// game.practice (Settings > Game), kept in the app's
// settings.ini ([general] practice); the game reads the row's value at each
// VBlank, so a change applies at once. A game offers it once its override
// can put the player back at a point (practiceOffered).

#include "runtime/settings_registry.h"

#include <functional>
#include <string_view>

namespace zx::collection
{
    // The row for a game ("ai", "dm2", "bmx": its help says what a return
    // gives back): `current` and `apply` read and keep the app's value.
    zx::settings::Setting practiceSetting(std::string_view gameId, std::function<int()> current,
                                          std::function<void(int)> apply);

    // Whether the game ("ai", "dm2", "bmx") has its restart point.
    bool practiceOffered(std::string_view gameId);
}
