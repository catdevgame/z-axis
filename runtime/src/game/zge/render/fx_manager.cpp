// The full-screen effect manager (zfxmgr.c): full-screen effects (motion
// blur, the lens flare) registered by name in a search tree, and the running
// ones, each a record of a pool, updated and drawn by two tasks.

#include "fx_manager.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/text/strings.h"
#include "zge/container/tree.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include <cstddef>
#include <cstdint>
#include "sym/zge/render/fx_manager.cpp.h"

namespace zge
{

    namespace
    {
        inline ee::Global<int32_t, sym::g_effectManagerReady> g_effectManagerReady;
        inline ee::Global<int32_t, sym::g_fullScreenEffectActive> g_fullScreenEffectActive; // g_fullScreenEffectActive; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_fullScreenEffectStopping> g_fullScreenEffectStopping; // g_fullScreenEffectStopping; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_fullScreenEffectPool> g_fullScreenEffectPool;
        constexpr uint32_t kTextNgengineZge3ZgeModsZfxZfxmgr = sym::g_textNgengineZge3ZgeModsZfxZfxmgr;
        inline ee::Global<ee::Ptr<Task>, sym::g_effectUpdateTask> g_effectUpdateTask;
        inline ee::Global<ee::Ptr<Task>, sym::g_effectDrawTask> g_effectDrawTask;
        constexpr uint32_t kPoolFree = sym::poolFree; // poolFree ((pool, record); readable: poolFree_, Verified)
    }

    constexpr uint32_t kEffectRunsShift = 3, kEffectRunsMask = 0x3FFu;
    // startFullScreenEffect's answer when no effect has the name.
    constexpr uint32_t kNoSuchEffect = 0xFFFFFFFFu;

    enum RunningEffectFlags : uint32_t
    {
        kRunUpdates = 1u,
        kRunDraws = 2u,
        kRunStopped = 4u, // stopped inside the update pass: freed at its end
    };

    namespace
    {
        // The effect's runs (flags bits 3-12).
        uint32_t runsOf(const FullScreenEffect &effect) { return effect.flags >> kEffectRunsShift & kEffectRunsMask; }
        void setRuns(FullScreenEffect &effect, uint32_t runs)
        {
            effect.flags = (effect.flags & ~(kEffectRunsMask << kEffectRunsShift)) | (runs & kEffectRunsMask) << kEffectRunsShift;
        }

        inline ee::Global<ee::Ptr<Pool>, sym::g_fullScreenEffectPool> g_runningEffects;
        inline ee::Global<SearchTree, sym::g_fullScreenEffectTree> g_effectTree;

        // The pool's walk, each step the game calls it is: the first run
        // (none when it has no link), the next.
        // (By address: these small pool functions have twins of identical code.)
        uint32_t linkOf(uint32_t record) { return poolLinkOf(record); }
        uint32_t recordOf(uint32_t link) { return poolRecord(link); }
        uint32_t firstRun(ee::Ptr<Pool> pool) { return recordOf(poolFirst(ee::Ptr<PoolList>(&pool->used))); }
        bool isRun(uint32_t record) { return linkOf(record) != 0; }
        uint32_t nextRun(uint32_t record) { return recordOf(poolNext(ee::Ptr<PoolLink>(linkOf(record)))); }
    }

    namespace
    {
        namespace routed
        {
        }
    }

    namespace
    {
        namespace routed
        {
            GAME_TABLE_ROUTE(poolFreeint32_t, ee::game<int32_t>, kPoolFree);
        }
    }

    ee::Ptr<FullScreenEffect> findFullScreenEffect(uint32_t nameHash)
    {
        ee::Local<int32_t> nameHashSlot;
        *nameHashSlot = nameHash;
        return searchTreeEntry<FullScreenEffect>(searchTreeFind(ee::Ptr<SearchTree>(static_cast<uint32_t>(sym::g_fullScreenEffectTree)), nameHashSlot));
    }
    READABLE_FUNCTION_WAITS(findFullScreenEffect, Verified);

    int32_t initEffectManager()
    {
        if (*g_effectManagerReady == 1)
        {
            return 0;
        }
        *g_effectManagerReady = 1;
        const int32_t poolResult = createPool(24, 0x40, 0);
        *g_fullScreenEffectPool = poolResult;
        initSearchTree(g_effectTree.get(), sym::lessWord);
        const ee::Ptr<Task> task = newTask(7, sym::zfxUpdate, 0, 1, kTextNgengineZge3ZgeModsZfxZfxmgr, 196);
        *g_effectUpdateTask = task;
        const ee::Ptr<Task> task2 = newTask(1, sym::zfxDraw, 0, 1, kTextNgengineZge3ZgeModsZfxZfxmgr, 197);
        *g_effectDrawTask = task2;
        zge::registerMotionBlurEffect();
        registerLensFlareEffect();
        return 1;
        return 0;
    }
    READABLE_FUNCTION_WAITS(initEffectManager, Verified);

    namespace
    {
        // poolFree with what it leaves in the result register (the stop's result).
        int32_t poolFree(ee::Ptr<Pool> pool, ee::Ptr<RunningEffect> run) { return ee::game<int32_t>(sym::poolFree, pool, run); }
    }

    // zfxUpdate (the update task): every run that updates updated; a run
    // stopped meanwhile is freed after the pass. 1.
    uint32_t zfxUpdate()
    {
        uint32_t record = firstRun(*g_runningEffects);
        *g_fullScreenEffectActive = 1;
        *g_fullScreenEffectStopping = 0;
        for (; isRun(record); record = nextRun(record))
        {
            const ee::Ptr<RunningEffect> run(record);
            if ((run->flags & kRunUpdates) && run->effect->update)
                run->effect->update(run);
        }
        const int32_t stopping = *g_fullScreenEffectStopping;
        *g_fullScreenEffectActive = 0;
        if (stopping == 0)
            return 1;
        record = firstRun(*g_runningEffects);
        while (isRun(record))
        {
            if (ee::Ptr<RunningEffect>(record)->flags & kRunStopped)
            {
                poolFree(*g_runningEffects, record);
                record = firstRun(*g_runningEffects);
            }
            else
                record = nextRun(record);
        }
        return 1;
    }
    READABLE_FUNCTION(zfxUpdate, Verified);

    // zfxDraw (the draw task): every run that draws drawn. 1.
    uint32_t zfxDraw()
    {
        for (uint32_t record = firstRun(*g_runningEffects); isRun(record); record = nextRun(record))
        {
            const ee::Ptr<RunningEffect> run(record);
            if ((run->flags & kRunDraws) && run->effect->draw)
                run->effect->draw(run);
        }
        return 1;
    }
    READABLE_FUNCTION(zfxDraw, Verified);

    // freeFullScreenEffect (the tree's visit as the manager shuts down): the
    // effect released and freed if nothing runs it. 1 when it went.
    uint32_t freeFullScreenEffect(ee::Ptr<FullScreenEffect> effect)
    {
        if (runsOf(*effect) != 0)
            return 0;
        if (effect->released)
            effect->released(effect);
        heapFree(effect);
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeFullScreenEffect, Verified);

    // stopFullScreenEffect: the run's stop method called and one run fewer
    // counted; the run freed, or inside the update pass marked for the end
    // of it (not with kEffectHandsBackRun: then it goes on running). 0 when
    // the record is not a run.
    int32_t stopFullScreenEffect(ee::Ptr<RunningEffect> run)
    {
        int32_t result = poolContains(*g_runningEffects, run);
        if (result == 0)
            return result;
        if (run->effect->stop)
            run->effect->stop(run, run->effect);
        FullScreenEffect &effect = *run->effect;
        setRuns(effect, runsOf(effect) - 1);
        if (*g_fullScreenEffectActive != 0)
        {
            result = run->effect->flags & kEffectHandsBackRun;
            if (result != 0)
                return result;
            *g_fullScreenEffectStopping = 1;
            run->flags |= kRunStopped;
            return static_cast<int32_t>(run->flags);
        }
        return poolFree(*g_runningEffects, run);
    }
    READABLE_FUNCTION(stopFullScreenEffect, Verified);

    // shutdownEffectManager: every run stopped, the unused effects freed, the
    // two tasks and the pool gone. 1, 0 when it was not running.
    uint32_t shutdownEffectManager()
    {
        if (*g_effectManagerReady == 0)
            return 0;
        const ee::Ptr<Pool> runs = *g_runningEffects;
        *g_effectManagerReady = 0;
        for (uint32_t record = firstRun(runs); isRun(record); record = firstRun(*g_runningEffects))
            stopFullScreenEffect(ee::Ptr<RunningEffect>(record));
        searchTreeClear(g_effectTree.get(), sym::freeFullScreenEffect, 0);
        deleteTask(*g_effectUpdateTask);
        deleteTask(*g_effectDrawTask);
        destroyPool(*g_runningEffects);
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutdownEffectManager, Verified);

    // registerFullScreenEffect: a copy of the definition registered under the
    // name, its run count cleared and its registered method called. Returns
    // the name's hash, 0 when an effect has the name already.
    uint32_t registerFullScreenEffect(uint32_t name, ee::Ptr<FullScreenEffect> definition)
    {
        const uint32_t hash = nameHash(ee::Ptr<const char>(name));
        if (findFullScreenEffect(hash))
            return 0;
        const ee::Ptr<FullScreenEffect> effect(heapAllocate(sizeof(FullScreenEffect)));
        memoryCopy(effect, definition, sizeof(FullScreenEffect));
        effect->clearedAtRegister = 0;
        effect->nameHash = hash;
        setRuns(*effect, 0);
        searchTreeInsert(g_effectTree.get(), &effect->node);
        if (effect->registered)
            effect->registered(effect);
        return hash;
    }
    READABLE_FUNCTION_WAITS(registerFullScreenEffect, Verified);

    // releaseFullScreenEffect: the effect of that name's hash taken out of
    // the tree, released and freed, if nothing runs it (1; else 0). It does
    // not check that one has the name.
    uint32_t releaseFullScreenEffect(uint32_t hash)
    {
        const ee::Ptr<FullScreenEffect> effect(findFullScreenEffect(hash));
        if (runsOf(*effect) != 0)
            return 0;
        searchTreeRemove(g_effectTree.get(), effect);
        if (effect->released)
            effect->released(effect);
        heapFree(effect);
        return 1;
    }
    READABLE_FUNCTION_WAITS(releaseFullScreenEffect, Draft);

    // startFullScreenEffect: a run of the effect of that name's hash, set to
    // update and draw as the effect does, one more run counted, its start
    // method called with the request. Returns the run with
    // kEffectHandsBackRun (else 0), kNoSuchEffect when no effect has the name.
    ee::Ptr<RunningEffect> startFullScreenEffect(uint32_t hash, ee::Address request)
    {
        const ee::Ptr<FullScreenEffect> effect(findFullScreenEffect(hash));
        if (!effect)
            return ee::Ptr<RunningEffect>(kNoSuchEffect);
        const ee::Ptr<RunningEffect> run(poolAlloc(*g_runningEffects));
        const uint32_t updates = (run->flags & ~kRunUpdates) | (effect->flags >> 1 & kRunUpdates);
        run->flags = updates;
        run->effect = effect;
        run->clearedAtStart = 0;
        run->flags = ((updates & ~kRunDraws) | (effect->flags >> 1 & kRunDraws)) & ~kRunStopped;
        setRuns(*effect, runsOf(*effect) + 1);
        if (run->effect->start)
            run->effect->start(run, request);
        return (effect->flags & kEffectHandsBackRun) ? run : ee::Ptr<RunningEffect>();
    }
    READABLE_FUNCTION_WAITS(startFullScreenEffect, Verified);

    // setEffectValue: the run's set-value method called with the value.
    // Returns what it did, 0 when the record is not a run or has none.
    int32_t setEffectValue(ee::Ptr<RunningEffect> run, ee::Address value)
    {
        if (poolContains(*g_runningEffects, run) == 0)
            return 0;
        if (!run->effect->setValue)
            return 0;
        return run->effect->setValue(run, value);
    }
    READABLE_FUNCTION(setEffectValue, Verified);
}
