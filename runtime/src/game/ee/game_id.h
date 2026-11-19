#pragma once

// Which game a readable registration, a traced original or a recording
// belongs to. One runner (the collection) holds several games' recompiled
// code at the same addresses; a function is (game, address), never the
// address alone.
//
//   Ai    Aggressive Inline (SLUS-20327): every registration made with the
//         plain macros (READABLE_FUNCTION, READABLE_PLANNED, ...)
//   Dm2   Dave Mirra Freestyle BMX 2 (SLUS-20159): READABLE_PLANNED_IN(dm2,
//         ...) and the other _IN macros; its traced originals come from its
//         namespaced generated code (game.cmake)
//   Bmx   BMX XXX (SLUS-20415): the same way
//
// The numbers are stored in recordings (fncheck::FileHeader::game, version
// 6 on; 0 is AI, as every older recording is): never renumber.

#include <cstdint>
#include <string>

namespace game
{
    enum class Game : uint8_t
    {
        Ai = 0,
        Dm2 = 1,
        Bmx = 2,
    };
    constexpr uint32_t kGameCount = 3;

    // "ai", "dm2", "bmx" (the prefix of `dm2:ADDRESS` and of DM2's recording
    // files), or "?" for a number no game has.
    inline const char *gameName(Game game)
    {
        switch (game)
        {
        case Game::Ai:
            return "ai";
        case Game::Dm2:
            return "dm2";
        case Game::Bmx:
            return "bmx";
        }
        return "?";
    }

    inline bool gameFromNumber(uint32_t number, Game &game)
    {
        if (number >= kGameCount)
            return false;
        game = static_cast<Game>(number);
        return true;
    }

    inline bool parseGame(const std::string &name, Game &game)
    {
        for (uint32_t i = 0; i < kGameCount; ++i)
            if (name == gameName(static_cast<Game>(i)))
            {
                game = static_cast<Game>(i);
                return true;
            }
        return false;
    }

    // The tokens of the _IN macros (READABLE_PLANNED_IN(dm2, ...)).
    namespace games
    {
        constexpr Game ai = Game::Ai;
        constexpr Game dm2 = Game::Dm2;
        constexpr Game bmx = Game::Bmx;
    }
}
