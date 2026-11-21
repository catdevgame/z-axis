// The engine's tasks and deferred releases (zge/platform/tasks.h; the
// engine block of zge.c, DM2 and BMX XXX have them too): the stage lists and
// the task pool made at start-up, tasks made and deleted, and the two pools of
// deferred releases the frame swaps between.

#include "tasks.h"

#include "ee/plain.h"
#include "ee/route.h"
#include "ee/readable.h"
#include "zge/render/billboards.h"
#include "zge/render/material.h"
#include "zge/scene/node.h"
#include "zge/debug/profiler.h"

#include <cstdint>
#include "sym/zge/platform/tasks.cpp.h"

namespace zge
{

    namespace
    {
        inline ee::Global<RenderStage[kRenderStages], sym::g_taskList> g_taskList;
        inline ee::Global<ee::Ptr<Pool>, sym::g_taskNodePool> g_taskNodePool; // Task records
        // Deferred releases: the pool requests go to now (g_deferredPoolIndex)
        // and the one the frame's end empties.
        inline ee::Global<ee::Ptr<Pool>[2], sym::g_deferredPools> g_deferredPools;
        inline ee::Global<uint32_t, sym::g_deferredPoolIndex> g_deferredPoolIndex;

        constexpr uint32_t kPoolBlockRecords = 0x20;
        // ZGE4 grows the deferred pools 640 requests at a time (BMX XXX's initDeferredPools), ZGE2 and ZGE3 by 32.
        constexpr uint32_t kDeferredBlockRecords = game::readable::perGame(0x20u, 0x20u, 0x280u);

        void freeMaterial(uint32_t material) { zge::freeMaterial(ee::Ptr<Material>(material)); }
    }

    // initRenderStages (startEngine): the task pool made, and the eight stages
    // emptied and enabled.
    void initRenderStages()
    {
        *g_taskNodePool = ee::Ptr<Pool>(createPool(sizeof(Task), kPoolBlockRecords, 0));
        for (RenderStage &stage : *g_taskList)
        {
            initList(&stage.tasks);
            enableRenderStage(stage);
        }
    }
    READABLE_FUNCTION_WAITS(initRenderStages, Verified);

    // initDeferredPools (startEngine): the two pools of deferred releases.
    void initDeferredPools()
    {
        for (ee::Ptr<Pool> &pool : *g_deferredPools)
            pool = ee::Ptr<Pool>(createPool(sizeof(DeferredRelease), kDeferredBlockRecords, 0));
    }
    READABLE_FUNCTION_WAITS(initDeferredPools, Verified);

    // destroyDeferredPools (shutdownEngine): both pools destroyed, with
    // whatever releases they still hold (their records have no destructor:
    // the nodes and materials are not freed).
    void destroyDeferredPools()
    {
        for (ee::Ptr<Pool> &pool : *g_deferredPools)
        {
            destroyPool(pool);
            pool = ee::Ptr<Pool>();
        }
    }
    READABLE_FUNCTION_WAITS(destroyDeferredPools, Verified);

    // deferRelease: a geometry node or material (kind: DeferredKind) to be
    // freed at the end of the frame (releaseNode, dropMaterialReference), when
    // nothing draws it any more.
    void deferRelease(ee::Address record, int32_t kind)
    {
        const ee::Ptr<DeferredRelease> request(poolAlloc((*g_deferredPools)[*g_deferredPoolIndex]));
        request->kind = kind;
        request->record = record;
    }
    READABLE_FUNCTION_WAITS(deferRelease, Verified);

    // releaseDeferred (finishFrame): the requests made since the last call
    // carried out, oldest last; new requests go to the other pool from now.
    // A kind other than a node or a material (or, in ZGE4, a billboard effect)
    // is only given back.
    void releaseDeferred()
    {
        const ee::Ptr<Pool> pool = (*g_deferredPools)[*g_deferredPoolIndex];
        *g_deferredPoolIndex = *g_deferredPoolIndex ^ 1u;
        while (const ee::Ptr<ListNode> link = firstPoolLink(pool))
        {
            const ee::Ptr<DeferredRelease> request = poolRecordAs<DeferredRelease>(link);
            if (request->kind == kDeferredGeometryNode)
                zge::freeGeometryNode(ee::Ptr<Node>(request->record));
            else if (request->kind == kDeferredMaterial)
                freeMaterial(request->record);
            else if constexpr (game::readable::kIsBmx)
            {
                if (request->kind == kDeferredBillboardEffect)
                    freeBillboardEffect(ee::Ptr<BillboardEffect>(request->record));
            }
            poolFree(pool, request);
        }
    }
    READABLE_FUNCTION_WAITS(releaseDeferred, Verified);

    // insertTask (newTask's): a task from the pool put on a stage's list, at
    // its end (atEnd non-zero) or its front, and enabled. 0 when the pool has
    // no record.
    // Kept bug: the record's old flags are kept, so a task made in the record
    // of an unprofiled task that was deleted is unprofiled too.
    ee::Ptr<Task> insertTask(ee::Ptr<List> stage, uint32_t function, uint32_t data, uint32_t atEnd, uint32_t sourceFile,
                              uint32_t sourceLine)
    {
        const ee::Ptr<Task> task(poolAlloc(*g_taskNodePool));
        if (!task)
            return task;
        if (atEnd == 0)
            listPushFront(stage, ee::Ptr<ListNode>(&task->link));
        else
            listPush(stage, ee::Ptr<ListNode>(&task->link));
        const uint32_t oldFlags = task->state & (kTaskEnabled | kTaskUnprofiled);
        task->stage = stage;
        task->function = ee::FunctionPtr<int32_t(uint32_t)>(function);
        task->data = data;
        task->sourceFile = ee::Ptr<const char>(sourceFile);
        task->state = oldFlags | (sourceLine & kTaskLineMask) | kTaskEnabled;
        return task;
    }
    READABLE_FUNCTION_WAITS(insertTask, Verified);

    // newTask: function(data) run every frame in a stage (the low byte of
    // stageAndFlags; kNewTaskUnprofiled: with no profile bar), made by
    // sourceFile at sourceLine. 0 when the pool has no record.
    // Kept bug: the unprofiled flag is set without a check, so a failed
    // newTask with it writes the null task's state word.
    ee::Ptr<Task> newTask(uint32_t stageAndFlags, uint32_t function, ee::Address data, uint32_t atEnd, uint32_t sourceFile, uint32_t sourceLine)
    {
        RenderStage &stage = (*g_taskList)[stageAndFlags & kNewTaskStageMask];
        const ee::Ptr<Task> task = insertTask(ee::Ptr<List>(&stage.tasks), function, data, atEnd, sourceFile, sourceLine);
        if (stageAndFlags & kNewTaskUnprofiled)
            task->state |= kTaskUnprofiled;
        return task;
    }
    READABLE_FUNCTION_WAITS(newTask, Verified);

    // setTaskUnprofiled (no caller in this build): the task run with no
    // profile bar from now on.
    void setTaskUnprofiled(ee::Ptr<Task> task)
    {
        task->state |= kTaskUnprofiled;
    }
    READABLE_FUNCTION(setTaskUnprofiled, Draft);

    // deleteTask: the task taken off its stage and given back to the pool.
    // Returns 1.
    uint32_t deleteTask(ee::Ptr<Task> task)
    {
        listRemove(task->stage, ee::Ptr<ListNode>(&task->link));
        poolFree(*g_taskNodePool, task);
        return 1;
    }
    READABLE_FUNCTION(deleteTask, Verified);

    namespace
    {
        namespace routed
        {
        }
    }

    // runRenderStage (the display's stages, the cameras', the frame step's):
    // a stage's tasks run in their order. A task with no function, or not
    // enabled, is passed over; one not unprofiled runs inside the
    // application's profile bar, which it stops after unless the bar was
    // already running. A task that returns 0 ends the walk, and the stage
    // returns 0; otherwise 1, as for a stage switched off. A task's state is
    // read again after it runs (a task may change its own).
    int32_t runRenderStage(uint32_t stage)
    {
        const RenderStage &tasks = (*g_taskList)[stage];
        if (!renderStageEnabled(tasks))
            return 1;
        uint32_t barWasRunning = 0;
        for (ee::Ptr<ListNode> link = tasks.tasks.head; link;)
        {
            const ee::Ptr<Task> task = recordOfNode<Task>(link);
            const ee::Ptr<ListNode> next = link->next; // read before the task runs
            if (task->function && (kTaskEnabled == 0 || (task->state & kTaskEnabled) != 0)) // (ZGE2 has no enabled flag)
            {
                if ((task->state & kTaskUnprofiled) == 0)
                    barWasRunning = startProfileBar(zge::appProfileBar());
                if (task->function(task->data) == 0)
                    return 0;
                if ((task->state & kTaskUnprofiled) == 0 && barWasRunning == 0)
                    stopProfileBar(zge::appProfileBar());
            }
            link = next;
        }
        return 1;
    }
    READABLE_FUNCTION(runRenderStage, Verified);
}
