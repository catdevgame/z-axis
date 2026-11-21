#include "runtime/game_module.h"

#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// The runtime's table for the modules linked into this program (a shared module has its own copy of the pointer).
extern "C" const ZxRuntimeApi *zx_game_module_api = nullptr;

namespace zx::game_module
{
    namespace
    {
        PS2Runtime *runtimeOf(ZxRuntime *runtime) { return reinterpret_cast<PS2Runtime *>(runtime); }
        const PS2Runtime *runtimeOf(const ZxRuntime *runtime) { return reinterpret_cast<const PS2Runtime *>(runtime); }

        // Both sides of the boundary name the same branch kinds and read the same tail-call table.
        static_assert(static_cast<uint32_t>(PS2Runtime::GuestBranchKind::DirectJump) == ZX_BRANCH_DIRECT_JUMP);
        static_assert(static_cast<uint32_t>(PS2Runtime::GuestBranchKind::DirectCall) == ZX_BRANCH_DIRECT_CALL);
        static_assert(static_cast<uint32_t>(PS2Runtime::GuestBranchKind::IndirectJump) == ZX_BRANCH_INDIRECT_JUMP);
        static_assert(static_cast<uint32_t>(PS2Runtime::GuestBranchKind::IndirectCall) == ZX_BRANCH_INDIRECT_CALL);
        static_assert(static_cast<uint32_t>(PS2Runtime::GuestBranchKind::Return) == ZX_BRANCH_RETURN);
        static_assert(sizeof(PS2Runtime::TailCallTable) == sizeof(ZxTailCallTable));
        static_assert(offsetof(PS2Runtime::TailCallTable, base) == offsetof(ZxTailCallTable, base));
        static_assert(offsetof(PS2Runtime::TailCallTable, slotCount) == offsetof(ZxTailCallTable, slotCount));
        static_assert(offsetof(PS2Runtime::TailCallTable, table) == offsetof(ZxTailCallTable, table));
        static_assert(offsetof(PS2Runtime::TailCallTable, stats) == offsetof(ZxTailCallTable, stats));

        bool dispatchBranch(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t targetPc,
                            uint32_t sourcePc, uint32_t fallthroughPc, uint32_t kind, const char *debugName)
        {
            return runtimeOf(runtime)->dispatchGuestBranch(rdram, ctx, targetPc, sourcePc, fallthroughPc,
                                                           static_cast<PS2Runtime::GuestBranchKind>(kind), debugName);
        }
        bool checkpointDue(ZxRuntime *runtime, uint32_t cycles) { return runtimeOf(runtime)->eeCheckpointDue(cycles); }

        uint8_t load8(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address)
        {
            return runtimeOf(runtime)->Load8(rdram, ctx, address);
        }
        uint16_t load16(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address)
        {
            return runtimeOf(runtime)->Load16(rdram, ctx, address);
        }
        uint32_t load32(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address)
        {
            return runtimeOf(runtime)->Load32(rdram, ctx, address);
        }
        uint64_t load64(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address)
        {
            return runtimeOf(runtime)->Load64(rdram, ctx, address);
        }
        void load128(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address, void *value)
        {
            const __m128i loaded = runtimeOf(runtime)->Load128(rdram, ctx, address);
            std::memcpy(value, &loaded, sizeof(loaded));
        }
        void store8(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address, uint8_t value)
        {
            runtimeOf(runtime)->Store8(rdram, ctx, address, value);
        }
        void store16(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address, uint16_t value)
        {
            runtimeOf(runtime)->Store16(rdram, ctx, address, value);
        }
        void store32(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address, uint32_t value)
        {
            runtimeOf(runtime)->Store32(rdram, ctx, address, value);
        }
        void store64(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address, uint64_t value)
        {
            runtimeOf(runtime)->Store64(rdram, ctx, address, value);
        }
        void store128(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address, const void *value)
        {
            __m128i stored;
            std::memcpy(&stored, value, sizeof(stored));
            runtimeOf(runtime)->Store128(rdram, ctx, address, stored);
        }

        void handleSyscall(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId)
        {
            runtimeOf(runtime)->handleSyscall(rdram, ctx, encodedSyscallId);
        }
        void handleBreak(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx)
        {
            runtimeOf(runtime)->handleBreak(rdram, ctx);
        }
        void signalException(ZxRuntime *runtime, R5900Context *ctx, uint32_t exception)
        {
            runtimeOf(runtime)->SignalException(ctx, static_cast<PS2Exception>(exception));
        }
        void clearLLBit(ZxRuntime *runtime, R5900Context *ctx) { runtimeOf(runtime)->clearLLBit(ctx); }
        uint32_t readCop0Count(const ZxRuntime *runtime) { return runtimeOf(runtime)->readCop0Count(); }
        void vu0StartMicroProgram(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address)
        {
            runtimeOf(runtime)->vu0StartMicroProgram(rdram, ctx, address);
        }
        void vu0ExecuteMicroProgram(ZxRuntime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t address)
        {
            runtimeOf(runtime)->executeVU0Microprogram(rdram, ctx, address);
        }

        // The kernel's and libraries' replacements, in their lists' order.
        template <void (*Function)(uint8_t *, R5900Context *, PS2Runtime *)>
        void guestCall(uint8_t *rdram, R5900Context *ctx, ZxRuntime *runtime)
        {
            Function(rdram, ctx, runtimeOf(runtime));
        }
        const ZxGuestFunction kSyscalls[] = {
#define ZX_SYSCALL_ENTRY(name) &guestCall<&ps2_syscalls::name>,
            PS2_SYSCALL_LIST(ZX_SYSCALL_ENTRY)
#undef ZX_SYSCALL_ENTRY
        };
        const ZxGuestFunction kStubs[] = {
#define ZX_STUB_ENTRY(name) &guestCall<&ps2_stubs::name>,
            PS2_STUB_LIST(ZX_STUB_ENTRY)
#undef ZX_STUB_ENTRY
        };

        void *libcMemcpy(void *destination, const void *source, size_t size) { return std::memcpy(destination, source, size); }
        void *libcMemmove(void *destination, const void *source, size_t size) { return std::memmove(destination, source, size); }
        void *libcMemset(void *destination, int value, size_t size) { return std::memset(destination, value, size); }
        float libcFloorf(float x) { return std::floor(x); }
        float libcCeilf(float x) { return std::ceil(x); }
        float libcRoundf(float x) { return std::round(x); }
        float libcTruncf(float x) { return std::trunc(x); }
        double libcFloor(double x) { return std::floor(x); }
        double libcCeil(double x) { return std::ceil(x); }
        double libcRound(double x) { return std::round(x); }
        double libcTrunc(double x) { return std::trunc(x); }

        ZxRuntimeApi makeApi()
        {
            ZxRuntimeApi api{};
            api.abi = ZX_GAME_MODULE_ABI;
            api.size = sizeof(ZxRuntimeApi);
            api.cpuContextSize = sizeof(R5900Context);
            api.cpuContextLayout = ps2CpuContextLayout();
            api.vu1CoreSize = sizeof(VU1Core);
            api.vu1CoreLayout = vu1CoreLayout();
            api.dispatchBranch = &dispatchBranch;
            api.checkpointDue = &checkpointDue;
            api.tailCalls = reinterpret_cast<const ZxTailCallTable *>(&PS2Runtime::s_tailCallTable);
            api.noteReplacedTailCall = &PS2Runtime::noteReplacedTailCall;
            api.load8 = &load8;
            api.load16 = &load16;
            api.load32 = &load32;
            api.load64 = &load64;
            api.load128 = &load128;
            api.store8 = &store8;
            api.store16 = &store16;
            api.store32 = &store32;
            api.store64 = &store64;
            api.store128 = &store128;
            api.handleSyscall = &handleSyscall;
            api.handleBreak = &handleBreak;
            api.signalException = &signalException;
            api.clearLLBit = &clearLLBit;
            api.readCop0Count = &readCop0Count;
            api.vu0StartMicroProgram = &vu0StartMicroProgram;
            api.vu0ExecuteMicroProgram = &vu0ExecuteMicroProgram;
            api.syscalls = kSyscalls;
            api.syscallCount = static_cast<uint32_t>(sizeof(kSyscalls) / sizeof(kSyscalls[0]));
            api.stubs = kStubs;
            api.stubCount = static_cast<uint32_t>(sizeof(kStubs) / sizeof(kStubs[0]));
            api.memcpy = &libcMemcpy;
            api.memmove = &libcMemmove;
            api.memset = &libcMemset;
            api.floorf = &libcFloorf;
            api.ceilf = &libcCeilf;
            api.roundf = &libcRoundf;
            api.truncf = &libcTruncf;
            api.floor = &libcFloor;
            api.ceil = &libcCeil;
            api.round = &libcRound;
            api.trunc = &libcTrunc;
            return api;
        }

        struct LinkedEntry
        {
            const char *id;
            ZxGameModuleEntry entry;
        };
        std::vector<LinkedEntry> &linkedEntries()
        {
            static std::vector<LinkedEntry> entries;
            return entries;
        }

        std::mutex g_loadMutex;
        std::vector<std::string> g_registeredElfs; // (lower case)

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // The module's code as the runtime's generated code: AI's as the default, the others by their ELF.
        bool registerModule(const ZxGameModule *module, const std::string &from, std::string &error)
        {
            if (module == nullptr)
            {
                error = from + ": built for another runtime (its boundary, ZX_GAME_MODULE_ABI, or R5900Context differ)";
                return false;
            }
            if (module->abi != ZX_GAME_MODULE_ABI || module->size != sizeof(ZxGameModule) ||
                module->cpuContextSize != sizeof(R5900Context) || module->cpuContextLayout != ps2CpuContextLayout() ||
                module->vu1CoreSize != sizeof(VU1Core) || module->vu1CoreLayout != vu1CoreLayout())
            {
                error = from + ": built for another runtime (its ZxGameModule, R5900Context or VU1Core differ)";
                return false;
            }
            // (A generated function takes the runtime as ZxRuntime *, the runtime's table holds PS2Runtime *: the
            // same pointer, the same calling convention.)
            const ps2_generated::Code code{module->id, module->elfName, module->base, module->end, module->slotCount,
                                           reinterpret_cast<PS2Runtime::RecompiledFunction *>(module->table)};
            if (code.id[0] == '\0')
                ps2_generated::setDefault(code);
            else
                ps2_generated::add(code);
            g_registeredElfs.push_back(lower(code.elfName));
            // Its VU1 microprograms compiled ahead of time, run whenever micro memory has their hash.
            uint32_t vu1Programs = 0;
            for (const ZxVu1Program *program = module->vu1Programs; program != nullptr; program = program->next)
            {
                VU1Interpreter::registerAotProgram(program->hash, program->run);
                ++vu1Programs;
            }
            std::cerr << "[game-module] " << from << ": " << code.elfName << ", " << vu1Programs
                      << " VU1 microprogram" << (vu1Programs == 1u ? "" : "s") << " compiled ahead of time" << std::endl;
            return true;
        }

        std::filesystem::path gameCodeFolder()
        {
            if (const char *dir = std::getenv("ZX_GAME_CODE_DIR"); dir && *dir)
                return dir;
#if defined(ZX_GAME_CODE_DIR)
            return ZX_GAME_CODE_DIR;
#else
            return {};
#endif
        }

        bool isSharedLibrary(const std::filesystem::path &path)
        {
#if defined(_WIN32)
            return path.extension() == ".dll";
#elif defined(__APPLE__)
            return path.extension() == ".dylib";
#else
            return path.extension() == ".so";
#endif
        }
    }

    bool hasCodeFor(const std::string &elfName)
    {
        ensureLoaded();
        const std::string want = lower(std::filesystem::path(elfName).filename().string());
        return std::find(g_registeredElfs.begin(), g_registeredElfs.end(), want) != g_registeredElfs.end();
    }

    const ZxRuntimeApi &runtimeApi()
    {
        static const ZxRuntimeApi api = makeApi();
        return api;
    }

    bool registerEntry(ZxGameModuleEntry entry, const std::string &from, std::string &error)
    {
        zx_game_module_api = &runtimeApi();
        return registerModule(entry(&runtimeApi()), from, error);
    }

    Linked::Linked(const char *id, ZxGameModuleEntry entry)
    {
        linkedEntries().push_back({id, entry});
    }

    bool load(const std::string &path, std::string &error)
    {
        std::lock_guard<std::mutex> lock(g_loadMutex);
#if defined(_WIN32)
        HMODULE library = LoadLibraryW(std::filesystem::path(path).wstring().c_str());
        if (library == nullptr)
        {
            error = path + ": LoadLibrary failed (" + std::to_string(GetLastError()) + ")";
            return false;
        }
        const auto entry = reinterpret_cast<ZxGameModuleEntry>(GetProcAddress(library, "zx_game_module"));
#else
        void *library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (library == nullptr)
        {
            const char *why = dlerror();
            error = path + ": " + (why ? why : "dlopen failed");
            return false;
        }
        const auto entry = reinterpret_cast<ZxGameModuleEntry>(dlsym(library, "zx_game_module"));
#endif
        if (entry == nullptr)
        {
            error = path + ": not a game module (no zx_game_module)";
            return false;
        }
        // (A module stays loaded: its functions are in the runtime's tables for the rest of the process.)
        return registerEntry(entry, path, error);
    }

    void ensureLoaded()
    {
        static std::once_flag once;
        std::call_once(once, []
        {
            std::string error;
            for (const LinkedEntry &linked : linkedEntries())
            {
                if (!registerEntry(linked.entry, std::string("linked game module ") + linked.id, error))
                    std::cerr << "[game-module] " << error << std::endl;
            }
            const std::filesystem::path folder = gameCodeFolder();
            std::error_code ec;
            if (folder.empty() || !std::filesystem::is_directory(folder, ec))
                return;
            std::vector<std::filesystem::path> modules;
            for (const auto &item : std::filesystem::directory_iterator(folder, ec))
            {
                if (item.is_regular_file(ec) && isSharedLibrary(item.path()))
                    modules.push_back(item.path());
            }
            std::sort(modules.begin(), modules.end());
            for (const auto &module : modules)
            {
                if (!load(module.string(), error))
                    std::cerr << "[game-module] " << error << std::endl;
            }
        });
    }
}
