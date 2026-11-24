#pragma once

// The threads that set the frame rate (the game's EE thread, the VIF1/VU1
// and GS workers) kept on the fast cores while the machine is busy, as the
// Mac keeps them with the user-interactive QoS class (machine.cpp and
// async_stage.h set it there).
//
// Windows: the thread at above-normal priority, with its power throttling
// (EcoQoS) turned off. Windows lowers the quality of service of a process
// whose window is in the background, minimized or absent (a headless run),
// and on a hybrid CPU (performance and efficient cores) a thread of low
// quality of service goes to the efficient cores; a thread that turned
// throttling off keeps high quality of service whatever its window does.
// Elsewhere nothing: Linux and Android place the threads by core tier
// behind ZX_THREAD_PLACEMENT (thread_placement.h).
namespace zx::thread_priority
{
    // The calling thread. Says on stderr what Windows refused.
    void raiseFrameRateThread(const char *name);
}
