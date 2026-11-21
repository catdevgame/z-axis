// The NPC constructor's first hide (aggressive_inline_npcs.h).

#include "aggressive_inline_npcs.h"
#include "sym/game_overrides/aggressive_inline_npcs.cpp.h"
#include "overrides/zge_sites.h"

#include "machine.h"
#include "runtime/session.h"

#include <cstring>
#include <iostream>

namespace aggressive_inline::npcs
{
    namespace
    {
        constexpr uint32_t kInitNpcObject = sym::initNpcObject; // (object)
        constexpr uint32_t kObjectComponent = sym::objectComponent; // (object): its tag record
        constexpr uint32_t kTagWord = sym::tagWord;          // (record, kind, tag)
        constexpr uint32_t kWordTags = 8u, kTagNpc = 0x40u; // the NPC record's tag (pickups.cpp)
        constexpr uint32_t kNpcInvisible = 0x30u;           // Npc::invisible (npc.h)
        constexpr int kRegA0 = 4, kRegA1 = 5, kRegSp = 29;

        struct State
        {
            PS2Machine::RecompiledFunction initNpcObject = nullptr;
            uint32_t object = 0; // the constructor's object while it runs, else 0
        };
        State &state() { return zx::session::state<State>(); }

        void initNpcObjectHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &st = state();
            const uint32_t outer = st.object;
            st.object = getRegU32(ctx, kRegA0);
            st.initNpcObject(rdram, ctx, runtime);
            st.object = outer;
        }
    }

    bool installNpcStart(PS2Machine &runtime)
    {
        State &st = state();
        if (st.initNpcObject)
            return true;
        st.initNpcObject = runtime.lookupFunction(kInitNpcObject);
        if (!st.initNpcObject || !runtime.replaceFunction(kInitNpcObject, &initNpcObjectHook))
        {
            st.initNpcObject = nullptr;
            std::cerr << "[npcs] the NPC constructor was not wrapped" << std::endl;
            return false;
        }
        return true;
    }

    bool isNpcStartHide(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        const State &st = state();
        const uint32_t object = getRegU32(ctx, kRegA0);
        if (!st.object || object != st.object || getRegU32(ctx, kRegA1) != 0u)
            return false;
        // The NPC record (set before either hide) is not marked invisible yet.
        const uint32_t sp = (getRegU32(ctx, kRegSp) - 0x200u) & ~0xFu;
        uint32_t component = 0, npc = 0, invisible = 1;
        if (!runtime->callGuestFunction(rdram, ctx, kObjectComponent, {object}, sp, &component) || !component ||
            !runtime->callGuestFunction(rdram, ctx, kTagWord, {component, kWordTags, kTagNpc}, sp, &npc) || !npc)
            return false;
        if (const uint8_t *p = getConstMemPtr(rdram, npc + kNpcInvisible))
            std::memcpy(&invisible, p, sizeof(invisible));
        if (invisible != 0u)
            return false;
        zge_host::sites::seen("ai.npcs.start_hide");
        return true;
    }
}
