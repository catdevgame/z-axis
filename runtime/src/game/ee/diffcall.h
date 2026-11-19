#pragma once

// PS2X_DIFFCALL (ee/diffcall.cpp): generated calls of functions no recording
// reaches, each from a forked copy of the running game, recorded for
// ps2xFnCheck. Off unless set; the game itself never runs a case.

#include "game_id.h"

#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace game::diffcall
{
    // The recorded function's accesses the hardware would fault on, which the
    // runtime's masked memory lets through (a wild pointer, a null one's low
    // addresses, a misaligned word or quadword): the recorder counts them while
    // g_countWild is set (fncheck.cpp); such a call is an invalid input.
    extern bool g_countWild;
    extern uint64_t g_wildAccesses;
    void noteAccess(uint32_t address, uint32_t size, bool store = false);

    // A case's feedback to the search (tools/diffcall.py), told by the recorder
    // while g_countWild is set: the block of the traced original just entered,
    // its reads (address, value) by block, and a call's return, where the case
    // may give the function another result (the case's `^` items).
    void noteBlock(uint32_t block);
    void noteRead(uint32_t address, uint32_t size);
    void noteCall(uint32_t returnAddress, uint32_t target);
    void noteCallReturn(R5900Context *ctx, uint32_t returnAddress, uint32_t target);

    // Reads PS2X_DIFFCALL and hooks its function (after the readable functions
    // and the recorder are installed); nothing when it is unset.
    void install(PS2Runtime &runtime, game::Game game);
}
