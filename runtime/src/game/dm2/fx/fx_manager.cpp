// DM2's special effects manager (fx_manager.h). DM2's own code
// (SpecialFX/zfxMgr.c).

#include "fx_manager.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/container/list.h"
#include "zge/memory/regions.h"
#include "zge/platform/tasks.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/fx/fx_manager.cpp.h"

namespace game::dm2
{
    namespace
    {
        // A listed effect: the effect and what draws it.
        struct ManagedFx
        {
            zge::ListNode link;                                // +0x00
            uint32_t _08;
            ee::Ptr<LensFlare> effect;                         // +0x0C
            ee::FunctionPtr<void(ee::Ptr<LensFlare>)> draw;    // +0x10
        };
        static_assert(offsetof(ManagedFx, effect) == 0x0C && sizeof(ManagedFx) == 0x14);

        struct FxManager
        {
            uint32_t started;        // +0x00
            ee::Ptr<zge::Task> task; // +0x04 the draw task
            zge::List effects;       // +0x08 ManagedFx
        };
        static_assert(offsetof(FxManager, effects) == 0x08);
        inline ee::Global<FxManager, sym::g_fxManager> g_fxManager;

        // The draw task: its stage, its source file and the line it is made at.
        constexpr uint32_t kDrawStage = 1;
        constexpr uint32_t kSourcePath = sym::fxManagerSourcePath; // "./src/com/SpecialFX/zfxMgr.c"
        constexpr uint32_t kTaskLine = 24;
    }

    void initFxManager()
    {
        zge::initList(ee::Ptr<zge::List>(&g_fxManager->effects));
        g_fxManager->task = zge::newTask(kDrawStage, sym::drawManagedFx, 0, 1, kSourcePath, kTaskLine);
        g_fxManager->started = 1;
    }
    READABLE_FUNCTION_WAITS(initFxManager, Verified);

    ee::Ptr<LensFlare> newManagedFx(uint32_t kind)
    {
        if (!g_fxManager->started)
            return {};
        if (kind != kManagedLensFlare)
            return {};
        const ee::Ptr<ManagedFx> listed(zge::heapAllocate(sizeof(ManagedFx)));
        listed->effect = newLensFlare();
        listed->draw = sym::drawLensFlare;
        zge::listPush(ee::Ptr<zge::List>(&g_fxManager->effects), listed);
        return listed->effect;
    }
    READABLE_FUNCTION_WAITS(newManagedFx, Verified);

    uint32_t drawManagedFx()
    {
        if (g_fxManager->started)
        {
            for (ee::Ptr<ManagedFx> listed = zge::recordOfNode<ManagedFx>(g_fxManager->effects.head); listed;
                 listed = zge::recordOfNode<ManagedFx>(listed->link.next))
            {
                if (listed->draw)
                    listed->draw(listed->effect);
            }
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawManagedFx, Verified);

    uint32_t shutdownFxManager()
    {
        if (g_fxManager->started)
            zge::deleteTask(g_fxManager->task);
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownFxManager, Verified);
}
