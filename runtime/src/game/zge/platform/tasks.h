#pragma once

// The engine's tasks (zge/platform/tasks.cpp): functions the engine calls
// each frame, kept on eight stage lists that runRenderStage walks (the
// display runs stages 0 to 2, the cameras 3 to 5, the frame step 6 before
// the game's frame and 7 after it). Each task is made with the C file and
// line that asked for it. Beside them, the deferred releases: geometry
// nodes and materials handed back after the frame that last drew them.

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/container/list.h"
#include "zge/memory/pool.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/platform/tasks.h.h"

namespace zge
{
    // A stage: its tasks in the order they run, and whether it runs. ZGE3
    // added the switch: ZGE2's stage is its list alone (12 bytes; DM2's
    // newTask and runRenderStage index the stages by 12), and always runs.
    struct RenderStage
    {
        List tasks;      // +0x00 Task records
#if !PS2X_GAME_DM2
        int32_t enabled; // +0x0C 0: runRenderStage skips the stage
#endif
    };
    static_assert(sizeof(RenderStage) == game::readable::perGame(0x10u, 0x0Cu));
#if !PS2X_GAME_DM2
    static_assert(offsetof(RenderStage, enabled) == 0x0C);
#endif
    // ZGE2 has seven stages (DM2's task pool follows the seventh, 0x54 bytes on), ZGE3 eight.
    constexpr uint32_t kRenderStages = game::readable::perGame(8u, 7u);

    // Whether runRenderStage walks a stage, and initRenderStages' switch on.
    inline bool renderStageEnabled(const RenderStage &stage)
    {
#if !PS2X_GAME_DM2
        return stage.enabled != 0;
#else
        (void)stage;
        return true;
#endif
    }
    inline void enableRenderStage(RenderStage &stage)
    {
#if !PS2X_GAME_DM2
        stage.enabled = 1;
#else
        (void)stage;
#endif
    }

    // Task::state: the source line in the low bits, and the flags above it.
    // ZGE3 added the enabled flag: ZGE2's state is the line in 31 bits and
    // the unprofiled flag in the top one (DM2's insertTask, newTask and
    // setTaskUnprofiled), and every task runs.
    enum TaskState : uint32_t
    {
        kTaskLineMask = game::readable::perGame(0x3FFFFFFFu, 0x7FFFFFFFu),
        kTaskUnprofiled = game::readable::perGame(1u << 30, 1u << 31), // runRenderStage times it with no profile bar
        kTaskEnabled = game::readable::perGame(1u << 31, 0u),          // runRenderStage calls it (every task is made so); ZGE2: none
    };

    // A task (the task pool's 28-byte record).
    struct Task
    {
        ListNode link;                                    // +0x00 on its stage's list
        ee::Ptr<List> stage;                              // +0x08 that list
        ee::FunctionPtr<int32_t(uint32_t data)> function; // +0x0C a 0 result ends the stage's walk this frame
        uint32_t data;                                    // +0x10 its argument
        ee::Ptr<const char> sourceFile;                   // +0x14 the C file that made it
        uint32_t state;                                   // +0x18 TaskState
    };
    static_assert(offsetof(Task, stage) == 0x08 && offsetof(Task, function) == 0x0C && offsetof(Task, data) == 0x10 &&
                  offsetof(Task, sourceFile) == 0x14 && offsetof(Task, state) == 0x18 && sizeof(Task) == 0x1C);

    // The frame step's stages: the one before the game's frame and the one
    // after it (updateFrameRate), the last two (ZGE2's 5 and 6).
    constexpr uint32_t kStageBeforeFrame = kRenderStages - 2u, kStageAfterFrame = kRenderStages - 1u;

    // newTask's first argument: the stage in the low byte, and this flag.
    constexpr uint32_t kNewTaskStageMask = 0xFFu;
    constexpr uint32_t kNewTaskUnprofiled = 0x100u;

    // What a deferred release frees (deferRelease's second argument).
    enum DeferredKind : int32_t
    {
        kDeferredGeometryNode = 0, // freeGeometryNode
        kDeferredMaterial = 1,     // freeMaterial
        kDeferredBillboardEffect = 2, // freeBillboardEffect (ZGE4: an effect still in use as its pool goes)
    };

    // A deferred release (the deferred pools' 8-byte record).
    struct DeferredRelease
    {
        uint32_t record; // +0x00 the node or material
        int32_t kind;    // +0x04 DeferredKind
    };
    static_assert(offsetof(DeferredRelease, kind) == 0x04 && sizeof(DeferredRelease) == 0x08);

    // startEngine's calls; shutdownEngine's.
    void initRenderStages();
    void initDeferredPools();
    void destroyDeferredPools();
}

namespace zge
{
    // (tasks.cpp)
    void releaseDeferred();
    ee::Ptr<Task> newTask(uint32_t stageAndFlags, uint32_t function, ee::Address data, uint32_t atEnd, uint32_t sourceFile, uint32_t sourceLine);
    uint32_t deleteTask(ee::Ptr<Task> task);
    int32_t runRenderStage(uint32_t stage);
}

namespace zge
{
    // (tasks.cpp)
    void deferRelease(ee::Address record, int32_t kind);
}
