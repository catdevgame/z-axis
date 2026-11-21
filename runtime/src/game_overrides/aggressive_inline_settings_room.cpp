// Aggressive Inline (SLUS-20327): room in zgui's heap for the port's own
// pages.
//
// zgui (game/zge/zgui) cuts every node, property and name copy of the UI
// scripts from one heap that initZguiSystem makes once, at boot, of 768 KB
// (initZgui(0xC0000)). The original fills it close to the top in the levels
// with an in-game editor (Cannery, Airfield, Industrial: their level setup
// loads ParkEditor.zsc, 124 KB of it, beside InGameUI), and the save and load
// screen (SaveLoad, openSaveUi) is read into what is left when it opens. The
// port's pages live in the same heap for as long as their script does: the
// pause menu's Settings page (InGameUI...Frame.PcPause, 37,344 bytes),
// Are You Sure (InGameUI.PcConfirm, 9,840 bytes) and the pause menu's
// extra actions and items, and in the front end its Settings page and quit
// prompt. With them, Industrial's pause menu Save found 46,848 bytes in one
// piece where the original had those and the pages' room more, and a widget
// of the screen came back from heapTake as 0: zgui made it at guest address
// 0, its name copy (also at 0) wrote over its child list, and appending its
// first property walked that garbage for good (the game hung on
// Industrial's pause menu Save).
//
// The pages are the collection's, so their room is not the game's: when
// they are on, zgui's heap gets it from host memory above the PS2's 32 MB
// (zge_heap_room.h), after its own 768 KB. The game's memory is as it was,
// every block of it where it was; zgui takes from its own 768 KB first, as
// before, and from the new room only what those cannot give.
#include "aggressive_inline_settings_room.h"
#include "sym/game_overrides/aggressive_inline_settings_room.cpp.h"

#include "zge_heap_room.h"

#include "ps2_runtime.h"
#include "runtime/ps2_session.h"

#include <cstdint>
#include <cstring>
#include <iostream>

namespace aggressive_inline::settings_room
{
    namespace
    {
        // The pages' room: what they take in the largest tree measured (the
        // Settings page and Are You Sure, 47,184 bytes in Industrial's, with
        // the heap blocks' headers), rounded up to 64 KB for the pause menu's
        // extra actions and items and the front end's pages.
        constexpr uint32_t kPagesRoom = 0x10000u;
        constexpr int kRegA0 = 4, kRegSp = 29, kRegRa = 31;

        struct State
        {
            PS2Runtime::RecompiledFunction initZgui = nullptr;
        };
        State &state() { return zx::session::state<State>(); }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        // initZgui as the game calls it, then the pages' room after the heap
        // it made (none when zgui was up already: the heap is made once).
        void initZguiHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t heapBefore = readWord(rdram, sym::g_heap);
            uint32_t v0 = 0;
            runtime->callGuestFunction(rdram, ctx, state().initZgui, sym::initZgui, {getRegU32(ctx, kRegA0)},
                                       getRegU32(ctx, kRegSp), &v0);
            const uint32_t heap = readWord(rdram, sym::g_heap);
            if (heap != 0u && heap != heapBefore)
            {
                if (zge_host::extendHeap(*runtime, rdram, heap, kPagesRoom))
                    std::cout << "[aggressive-inline] zgui's heap: " << kPagesRoom / 1024u
                              << " KB for the settings pages above the 32 MB, after the game's own" << std::endl;
                else
                    std::cerr << "[aggressive-inline] zgui's heap: no room for the settings pages above the 32 MB (the "
                                 "game's screens share its own with them)"
                              << std::endl;
            }
            setReturnU32(ctx, v0);
            ctx->pc = getRegU32(ctx, kRegRa);
        }
    }

    bool install(PS2Runtime &runtime)
    {
        State &st = state();
        if (st.initZgui)
            return true;
        st.initZgui = runtime.lookupFunction(sym::initZgui);
        if (!st.initZgui || !runtime.replaceFunction(sym::initZgui, &initZguiHook))
        {
            st.initZgui = nullptr;
            std::cerr << "[aggressive-inline] settings: zgui's heap not made larger (the game's screens share it with the pages)"
                      << std::endl;
            return false;
        }
        return true;
    }
}
