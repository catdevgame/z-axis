#pragma once

// Guest work done outside the game's time: a host override's look at what the game would do (the AI's hidden follow
// camera at the PS2's tuning, game_overrides/aggressive_inline_slus20327.cpp). While held, the EE scheduler counts
// no cycle and lets no checkpoint fall due (Kernel/EeScheduler.cpp), so the work moves no deadline and is never cut by
// a thread switch. Executor thread only; holds nest.
namespace ee_time
{
    void hold() noexcept;
    void release() noexcept;
    bool held() noexcept;

    struct Held
    {
        Held() noexcept { hold(); }
        ~Held() { release(); }
        Held(const Held &) = delete;
        Held &operator=(const Held &) = delete;
    };
}
