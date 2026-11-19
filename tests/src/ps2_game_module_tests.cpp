// Game modules (runtime/game_module.h, game_module/abi.h): the runtime's table for them, and a module's code
// registered as the generated code of its ELF.
#include "MiniTest.h"

#include "collection/game_code.h"

#include "ps2_call_list.h"
#include "ps2_runtime.h"
#include "runtime/game_module.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_function_table.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    int g_calls = 0;
    void moduleFunction(uint8_t *, R5900Context *, ZxRuntime *) { ++g_calls; }

    ZxGuestFunction g_table[4] = {&moduleFunction, nullptr, nullptr, &moduleFunction};
    ZxGameModule g_module{};

    // A module's entry as module_entry.cpp writes it, built against this runtime.
    const ZxGameModule *testEntry(const ZxRuntimeApi *api)
    {
        if (api == nullptr || api->abi != ZX_GAME_MODULE_ABI || api->cpuContextLayout != ps2CpuContextLayout())
            return nullptr;
        g_module = {ZX_GAME_MODULE_ABI, sizeof(ZxGameModule), sizeof(R5900Context), ps2CpuContextLayout(),
                    "zxtest", "ZXTEST.ELF", 0x00200000u, 0x00200010u, 4u, g_table,
                    sizeof(VU1Core), vu1CoreLayout(), nullptr};
        return &g_module;
    }
    // One with a VU1 microprogram compiled ahead of time, for the micro memory image with hash g_vu1Hash: it ends the
    // run at once, and counts its calls.
    int g_vu1Runs = 0;
    uint64_t g_vu1Hash = 0;
    bool vu1Program(void *core, void *runContext)
    {
        ++g_vu1Runs;
        static_cast<VU1Core::RunContext *>(runContext)->programEnded = true;
        return core != nullptr;
    }
    ZxVu1Program g_vu1Program{};
    const ZxGameModule *vu1Entry(const ZxRuntimeApi *api)
    {
        if (api == nullptr || api->abi != ZX_GAME_MODULE_ABI || api->vu1CoreLayout != vu1CoreLayout())
            return nullptr;
        g_vu1Program = {g_vu1Hash, &vu1Program, nullptr};
        g_module = {ZX_GAME_MODULE_ABI, sizeof(ZxGameModule), sizeof(R5900Context), ps2CpuContextLayout(),
                    "zxvu1", "ZXVU1.ELF", 0x00200000u, 0x00200010u, 4u, g_table,
                    sizeof(VU1Core), vu1CoreLayout(), &g_vu1Program};
        return &g_module;
    }
    // One built for another R5900Context.
    const ZxGameModule *otherEntry(const ZxRuntimeApi *)
    {
        g_module = {ZX_GAME_MODULE_ABI, sizeof(ZxGameModule), sizeof(R5900Context), ps2CpuContextLayout() + 1u,
                    "zxother", "ZXOTHER.ELF", 0x00200000u, 0x00200010u, 4u, g_table,
                    sizeof(VU1Core), vu1CoreLayout(), nullptr};
        return &g_module;
    }

    uint32_t listed(int list)
    {
        uint32_t n = 0;
#define ZX_COUNT(name) ++n;
        if (list == 0)
        {
            PS2_SYSCALL_LIST(ZX_COUNT)
        }
        else
        {
            PS2_STUB_LIST(ZX_COUNT)
        }
#undef ZX_COUNT
        return n;
    }
}

void register_ps2_game_module_tests()
{
    MiniTest::Case("GameModule", [](TestCase &tc)
    {
        tc.Run("the runtime's table names every call a module makes", [](TestCase &t)
        {
            const ZxRuntimeApi &api = ps2x::game_module::runtimeApi();
            t.Equals(api.abi, ZX_GAME_MODULE_ABI, "the boundary's version");
            t.Equals(api.size, static_cast<uint32_t>(sizeof(ZxRuntimeApi)), "its size");
            t.Equals(api.cpuContextSize, static_cast<uint32_t>(sizeof(R5900Context)), "R5900Context's size");
            t.Equals(api.vu1CoreSize, static_cast<uint32_t>(sizeof(VU1Core)), "VU1Core's size");
            t.Equals(api.vu1CoreLayout, vu1CoreLayout(), "VU1Core's layout");
            t.Equals(api.syscallCount, listed(0), "one entry per PS2_SYSCALL_LIST name");
            t.Equals(api.stubCount, listed(1), "one entry per PS2_STUB_LIST name");
            bool all = api.dispatchBranch && api.checkpointDue && api.tailCalls && api.noteReplacedTailCall &&
                       api.load8 && api.load128 && api.store8 && api.store128 && api.handleSyscall && api.handleBreak &&
                       api.signalException && api.clearLLBit && api.readCop0Count && api.vu0StartMicroProgram &&
                       api.vu0ExecuteMicroProgram && api.memcpy && api.memset && api.floorf && api.roundf;
            for (uint32_t i = 0; i < api.syscallCount; ++i)
                all = all && api.syscalls[i] != nullptr;
            for (uint32_t i = 0; i < api.stubCount; ++i)
                all = all && api.stubs[i] != nullptr;
            t.IsTrue(all, "every entry is set");
            t.IsTrue(api.tailCalls == reinterpret_cast<const ZxTailCallTable *>(&PS2Runtime::s_tailCallTable),
                     "tail calls read the runtime's own table");
        });

        tc.Run("a module's code is its ELF's generated code", [](TestCase &t)
        {
            std::string error;
            t.IsTrue(ps2x::game_module::registerEntry(&testEntry, "test module", error), "registered: " + error);
            t.Equals(ps2_generated::select("ZXTEST.ELF"), std::string("zxtest"), "chosen by its ELF's name");
            const ps2x::function_table::View view = ps2x::function_table::active();
            t.Equals(view.base, 0x00200000u, "its table's first address");
            t.Equals(view.slotCount, 4u, "its slots");
            t.IsTrue(view.table == reinterpret_cast<PS2Runtime::RecompiledFunction *>(g_table), "its functions");
            ps2_generated::select("NONE.ELF");
        });

        tc.Run("a module's VU1 microprograms run on the micro memory they were made for", [](TestCase &t)
        {
            PS2Memory memory;
            GS gs;
            t.IsTrue(memory.initialize(), "memory");
            // NOPs, the last pair an I-bit pair with an immediate no other test or module has
            uint8_t *code = memory.getVU1Code();
            const uint32_t nop[2] = {0x8000033Cu, 0x000002FFu};
            for (uint32_t at = 0; at < PS2_VU1_CODE_SIZE; at += 8u)
                std::memcpy(code + at, nop, sizeof(nop));
            const uint32_t marked[2] = {0x2BAD5EEDu, 0x800002FFu};
            std::memcpy(code + PS2_VU1_CODE_SIZE - 8u, marked, sizeof(marked));
            g_vu1Hash = VU1Interpreter::codeHash(code, PS2_VU1_CODE_SIZE);
            std::string error;
            t.IsTrue(ps2x::game_module::registerEntry(&vu1Entry, "VU1 test module", error), "registered: " + error);
            VU1Interpreter vu1;
            g_vu1Runs = 0;
            vu1.execute(code, PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE, gs, &memory, 0u, 0u, 0u, 64u);
            t.Equals(g_vu1Runs, 1, "the module's program ran the run");
            t.IsTrue(vu1.usingAotCode(), "as the micro memory's ahead-of-time code");
            code[PS2_VU1_CODE_SIZE - 8u] ^= 1u; // (another immediate; a new interpreter decodes it afresh)
            VU1Interpreter other;
            g_vu1Runs = 0;
            other.execute(code, PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE, gs, &memory, 0u, 0u, 0u, 64u);
            t.Equals(g_vu1Runs, 0, "another image runs on the interpreter");
            t.IsFalse(other.usingAotCode(), "with no ahead-of-time code");
            ps2_generated::select("NONE.ELF");
        });

        tc.Run("a module built for another runtime is refused", [](TestCase &t)
        {
            std::string error;
            t.IsFalse(ps2x::game_module::registerEntry(&otherEntry, "other module", error), "not registered");
            t.IsTrue(error.find("another runtime") != std::string::npos, "and says why: " + error);
            t.IsFalse(ps2_generated::select("ZXOTHER.ELF") == "zxother", "its ELF gets the default code");
        });

        tc.Run("the setup's code step without the app's tools says what is missing", [](TestCase &t)
        {
            ps2x::app::BuiltGameCode step(std::filesystem::path("no-game-code-tools-here"));
            const ps2x::app::SetupGame game{"zxnone", "No Game", "NONE.ELF", ""};
            const std::filesystem::path here = std::filesystem::path("no-game-folder-here");
            t.IsFalse(step.ready(game, here, here), "not ready: no code, no tools");
            std::string error;
            t.IsFalse(step.make(game, here, here, {}, &error), "nothing made");
            t.IsTrue(error.find("missing") != std::string::npos, "and says why: " + error);
        });

        tc.Run("a single-game app's game is found in the tools by the ELF its config names", [](TestCase &t)
        {
            const std::filesystem::path root = std::filesystem::temp_directory_path() / "ps2x-game-code-tools-test";
            std::filesystem::remove_all(root);
            std::filesystem::create_directories(root / "games" / "ai");
            {
                std::ofstream config(root / "games" / "ai" / "config.toml");
                config << "[general]\ninput = \"../extracted/SLUS_203.27\"\noutput = \"generated\"\n";
            }
            ps2x::app::GameCodeTools tools;
            tools.root = root;
            t.Equals(ps2x::app::gameCodeGame(tools, "game", "SLUS_203.27"), std::string("ai"), "found by its ELF");
            t.Equals(ps2x::app::gameCodeGame(tools, "ai", "OTHER.ELF"), std::string("ai"), "found by its id");
            t.Equals(ps2x::app::gameCodeGame(tools, "game", "SLUS_999.99"), std::string("game"), "none: the id as it is");
            std::filesystem::remove_all(root);
        });

#if defined(PS2X_TEST_LINKED_MODULES)
        tc.Run("the setup's code step is ready for a game whose code the program links", [](TestCase &t)
        {
            ps2x::app::BuiltGameCode step(std::filesystem::path("no-game-code-tools-here"));
            const ps2x::app::SetupGame game{"zxa", "Test A", "ZXA.ELF", ""};
            const std::filesystem::path here = std::filesystem::path("no-game-folder-here");
            t.IsTrue(step.ready(game, here, here), "ready with no tools: its code is linked");
            uint64_t done = 0, total = 0;
            std::string error;
            t.IsTrue(step.make(game, here, here, [&](const ps2x::app::SetupProgress &p)
            {
                done = p.done;
                total = p.total;
                return true;
            }, &error), "made at once: " + error);
            t.IsTrue(done == 1 && total == 1, "its step shown done");
        });

        // (the runner links each game's module the same: when they shared one registration, only one game's code
        // was registered and the others' ELFs ran the default code)
        tc.Run("modules linked into one program each register their own code", [](TestCase &t)
        {
            const ps2_generated::Code *zxa = nullptr;
            const ps2_generated::Code *zxb = nullptr;
            for (const ps2_generated::Code &code : ps2_generated::all())
            {
                if (std::string(code.elfName) == "ZXA.ELF")
                    zxa = &code;
                if (std::string(code.elfName) == "ZXB.ELF")
                    zxb = &code;
            }
            t.IsTrue(zxa != nullptr && zxb != nullptr, "both registered");
            if (zxa == nullptr || zxb == nullptr)
                return;
            t.Equals(std::string(zxa->id), std::string("zxa"), "zxa's code is zxa's");
            t.Equals(std::string(zxb->id), std::string("zxb"), "zxb's code is zxb's");
            t.Equals(zxa->base, 0x00300000u, "zxa's table starts at its own address");
            t.Equals(zxb->base, 0x00400000u, "zxb's table starts at its own address");
            t.IsTrue(zxa->table != zxb->table, "each its own table");
            t.IsTrue(zxa->table[0] != nullptr && zxb->table[0] != nullptr && zxa->table[0] != zxb->table[0],
                     "each filled with its own functions");
        });
#endif
    });
}
