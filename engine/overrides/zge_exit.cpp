// Exit to Collection in a ZGE game's own menus (zge_host.h): the script
// function the item's script CALLs, registered among the game's own.
//
// The engine keeps the functions a UI script's CALL reaches by name in one
// table, filled by the game's registerScriptFunction(name, function) calls
// (zge/zgui/commands_ui.cpp; DM2's is one engine version older, BMX XXX's
// one newer, the same two arguments). Its wrapper here registers the host's
// script functions (pcExitToCollection, the Settings page's: zge_settings.h)
// right after one of the game's own functions that is registered with the
// menu's, every time it is (so a table built again gets them again; a name
// already there is not added twice). The function's
// entry is an address inside one of the game's functions that is no entry
// point (so nothing else ever enters it), its name is in guest memory of
// our own (reserveAsyncCallbackStack), as are the stack of that call.
#include "zge_host.h"
#include "host_backend.h"
#include "machine.h"
#include "runtime/session.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace zge_host
{
    namespace exit_detail
    {
        constexpr int kRegA0 = 4, kRegA1 = 5, kRegSp = 29, kRegRa = 31;
        // A function's name at the bottom, the stack of the registering
        // call above it.
        constexpr uint32_t kScratchSize = 0x1000u;
        constexpr size_t kNameLimit = 0x100u;

        struct HostFunction
        {
            const char *name;
            uint32_t entry; // its address
        };

        // One boot's hook (zx::session: made at the first function added,
        // gone with the boot).
        struct State
        {
            ScriptFunctions functions{};
            PS2Machine::RecompiledFunction registerOriginal = nullptr;
            uint32_t scratchTop = 0;
            uint32_t nextSlot = 0;
            std::vector<HostFunction> added;
        };
        State &state() { return zx::session::state<State>(); }

        std::string guestString(uint8_t *rdram, uint32_t address, size_t limit)
        {
            std::string out;
            for (size_t i = 0; i < limit; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, address + static_cast<uint32_t>(i));
                if (!p || *p == 0u)
                    break;
                out.push_back(static_cast<char>(*p));
            }
            return out;
        }

        bool putString(uint8_t *rdram, uint32_t address, const char *text)
        {
            const size_t length = std::strlen(text);
            for (size_t i = 0; i <= length; ++i)
            {
                uint8_t *p = getMemPtr(rdram, address + static_cast<uint32_t>(i));
                if (!p)
                    return false;
                *p = static_cast<uint8_t>(text[i]);
            }
            return true;
        }

        void exitFunction(uint8_t *, R5900Context *ctx, PS2Machine *)
        {
            exitToCollection(state().functions.game, "its main menu");
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        void registerHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const uint32_t name = getRegU32(ctx, kRegA0);
            const uint32_t function = getRegU32(ctx, kRegA1);
            uint32_t result = 0;
            runtime->callGuestFunction(rdram, ctx, st.registerOriginal, st.functions.registerFunction, {name, function},
                                       getRegU32(ctx, kRegSp), &result);
            if (guestString(rdram, name, 64) == st.functions.after)
            {
                // The engine keeps a copy of each name: one place for them all.
                const uint32_t base = st.scratchTop - kScratchSize;
                for (const HostFunction &added : st.added)
                {
                    if (!putString(rdram, base, added.name))
                        continue;
                    runtime->callGuestFunction(rdram, ctx, st.registerOriginal, st.functions.registerFunction,
                                               {base, added.entry}, st.scratchTop);
                    std::cout << "[" << st.functions.game << "] " << added.name << " registered (after "
                              << st.functions.after << ")" << std::endl;
                }
            }
            setReturnU32(ctx, result);
            // Return like generated code (jr ra): a thread the scheduler
            // resumes at this entry calls the hook again otherwise.
            ctx->pc = getRegU32(ctx, kRegRa);
        }
    }

    bool inCollection() { return ps2_host::menuAvailable(); }

    void exitToCollection(const char *game, const char *where)
    {
        std::cout << "[" << game << "] Exit to Collection (" << where << ")" << std::endl;
        ps2_host::requestExitToMenu();
    }

    bool addScriptFunction(PS2Machine &runtime, const ScriptFunctions &functions, const char *name,
                           PS2Machine::RecompiledFunction function)
    {
        using namespace exit_detail;
        State &st = state();
        if (std::strlen(name) >= kNameLimit)
            return false;
        if (!st.registerOriginal)
        {
            st.functions = functions;
            st.nextSlot = functions.slotsBegin;
            st.registerOriginal = runtime.lookupFunction(functions.registerFunction);
            st.scratchTop = runtime.reserveAsyncCallbackStack(kScratchSize, 16u);
            if (!st.registerOriginal || !st.scratchTop ||
                !runtime.replaceFunction(functions.registerFunction, &registerHook))
            {
                std::cerr << "[" << functions.game << "] script functions' hook not installed" << std::endl;
                st.registerOriginal = nullptr;
                return false;
            }
        }
        else if (st.functions.registerFunction != functions.registerFunction)
        {
            return false;
        }
        uint32_t entry = st.nextSlot;
        while (entry < st.functions.slotsEnd && runtime.hasFunction(entry))
            entry += 4u;
        if (entry >= st.functions.slotsEnd || !runtime.registerFunction(entry, function))
        {
            std::cerr << "[" << functions.game << "] " << name << " not installed (no slot)" << std::endl;
            return false;
        }
        st.nextSlot = entry + 4u;
        st.added.push_back({name, entry});
        return true;
    }

    bool installExitFunction(PS2Machine &runtime, const ScriptFunctions &functions)
    {
        return addScriptFunction(runtime, functions, kExitFunction, &exit_detail::exitFunction);
    }
}
