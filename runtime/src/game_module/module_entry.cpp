// A game module's own part (compiled into every module with its generated code, game_module.cmake): its entry,
// what it gives back, and, when it is a shared library, the C library functions the compiler may call, which it
// takes from the runtime (a module links no library: the app builds it on the player's machine).

#include "ps2_generated.h"
#include "runtime/ps2_vu1_core.h"

extern "C" ZX_GAME_MODULE_HIDDEN ZxGameModule ZX_GAME_MODULE_NAME(code) = {};

#if defined(ZX_GAME_MODULE_SHARED)

#if defined(_WIN32)
#define ZX_GAME_MODULE_EXPORT extern "C" __declspec(dllexport)
#else
#define ZX_GAME_MODULE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

extern "C" ZX_GAME_MODULE_HIDDEN const ZxRuntimeApi *zx_game_module_api = nullptr;

// Called before the entry has the runtime's table only if a static constructor needed them: a fault there, not a
// silent wrong answer.
#define ZX_GAME_MODULE_RUNTIME() (zx_game_module_api ? zx_game_module_api : (__builtin_trap(), zx_game_module_api))

extern "C"
{
    void *memcpy(void *destination, const void *source, size_t size)
    {
        return ZX_GAME_MODULE_RUNTIME()->memcpy(destination, source, size);
    }
    void *memmove(void *destination, const void *source, size_t size)
    {
        return ZX_GAME_MODULE_RUNTIME()->memmove(destination, source, size);
    }
    void *memset(void *destination, int value, size_t size)
    {
        return ZX_GAME_MODULE_RUNTIME()->memset(destination, value, size);
    }
    float floorf(float x) { return ZX_GAME_MODULE_RUNTIME()->floorf(x); }
    float ceilf(float x) { return ZX_GAME_MODULE_RUNTIME()->ceilf(x); }
    float roundf(float x) { return ZX_GAME_MODULE_RUNTIME()->roundf(x); }
    float truncf(float x) { return ZX_GAME_MODULE_RUNTIME()->truncf(x); }
    double floor(double x) { return ZX_GAME_MODULE_RUNTIME()->floor(x); }
    double ceil(double x) { return ZX_GAME_MODULE_RUNTIME()->ceil(x); }
    double round(double x) { return ZX_GAME_MODULE_RUNTIME()->round(x); }
    double trunc(double x) { return ZX_GAME_MODULE_RUNTIME()->trunc(x); }
}

#if defined(_WIN32)
// A DLL with no C runtime (/NOENTRY /NODEFAULTLIB): the compiler's marker for floating-point use, its stack probe,
// and the static constructors (the generated table's), run from the entry. The probe is compiler-rt's
// (lib/builtins/x86_64/chkstk.S, Apache-2.0 WITH LLVM-exception): it touches each page from the stack pointer down to
// rsp - rax, every register kept.
extern "C" int _fltused = 0;
extern "C" __attribute__((naked)) void __chkstk()
{
    __asm__ volatile("push %rcx\n\t"
                     "push %rax\n\t"
                     "cmp $0x1000, %rax\n\t"
                     "lea 24(%rsp), %rcx\n\t"
                     "jb 1f\n"
                     "2:\n\t"
                     "sub $0x1000, %rcx\n\t"
                     "test %rcx, (%rcx)\n\t"
                     "sub $0x1000, %rax\n\t"
                     "cmp $0x1000, %rax\n\t"
                     "ja 2b\n"
                     "1:\n\t"
                     "sub %rax, %rcx\n\t"
                     "test %rcx, (%rcx)\n\t"
                     "pop %rax\n\t"
                     "pop %rcx\n\t"
                     "ret");
}
using ZxStaticConstructor = void (*)();
#pragma section(".CRT$XCA", read)
#pragma section(".CRT$XCZ", read)
__declspec(allocate(".CRT$XCA")) __attribute__((used)) static const ZxStaticConstructor kConstructorsBegin = nullptr;
__declspec(allocate(".CRT$XCZ")) __attribute__((used)) static const ZxStaticConstructor kConstructorsEnd = nullptr;
static void runStaticConstructors()
{
    for (const ZxStaticConstructor *constructor = &kConstructorsBegin + 1; constructor < &kConstructorsEnd; ++constructor)
    {
        if (*constructor != nullptr)
            (*constructor)();
    }
}
#endif

#define ZX_GAME_MODULE_ENTRY ZX_GAME_MODULE_EXPORT const ZxGameModule *zx_game_module
#else
// Linked into the runner: zx_game_module_<id>, added by the build's game_modules_linked.cpp.
#define ZX_GAME_MODULE_ENTRY extern "C" const ZxGameModule *ZX_GAME_MODULE_NAME(entry)
#endif

ZX_GAME_MODULE_ENTRY(const ZxRuntimeApi *api)
{
    if (api == nullptr || api->abi != ZX_GAME_MODULE_ABI || api->size != sizeof(ZxRuntimeApi) ||
        api->cpuContextSize != sizeof(R5900Context) || api->cpuContextLayout != ps2CpuContextLayout() ||
        api->vu1CoreSize != sizeof(VU1Core) || api->vu1CoreLayout != vu1CoreLayout() ||
        api->syscallCount != ZX_SYSCALL_COUNT || api->stubCount != ZX_STUB_COUNT)
        return nullptr;
    zx_game_module_api = api;
#if defined(ZX_GAME_MODULE_SHARED) && defined(_WIN32)
    static bool constructed = false;
    if (!constructed)
    {
        constructed = true;
        runStaticConstructors();
    }
#endif
    ZxGameModule &code = ZX_GAME_MODULE_NAME(code);
    if (code.table == nullptr)
        return nullptr; // (no registration: not a module's generated code)
    code.abi = ZX_GAME_MODULE_ABI;
    code.size = sizeof(ZxGameModule);
    code.cpuContextSize = sizeof(R5900Context);
    code.cpuContextLayout = ps2CpuContextLayout();
    code.vu1CoreSize = sizeof(VU1Core);
    code.vu1CoreLayout = vu1CoreLayout();
    return &code;
}
