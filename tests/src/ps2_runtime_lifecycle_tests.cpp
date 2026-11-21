// Several runtimes, one after another, in one process (the collection's
// menu boots a game, the game exits to it, the menu boots the next): each
// runs from its game's table as generated, with nothing left by the runs
// before it (override hooks, resume and return slots, functions waiting on
// their fibers, the kernel's RPC servers), and the host comes down only when
// its owner says so. Two small games stand in for AI and DM2: their own
// function tables at the same addresses, as the real ones are.
#include "MiniTest.h"

#include "ee/abi.h"
#include "Kernel/Syscalls/RPC.h"
#include "game_overrides.h"
#include "ps2_host_backend.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_boot_sequence.h"
#include "runtime/ps2_function_table.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    using Fn = PS2Runtime::RecompiledFunction;

    // Both games' code: one segment at the real games' base, the table's
    // slots from there.
    constexpr uint32_t kCodeBase = 0x00100000u;
    constexpr uint32_t kTableBase = 0x00100008u;
    constexpr uint32_t kSlots = 0x400u;
    constexpr uint32_t kTableEnd = kTableBase + kSlots * 4u;
    constexpr uint32_t kEntry = 0x00100108u;
    constexpr uint32_t kHooked = 0x00100200u;      // a function an override hooks
    constexpr uint32_t kWaiter = 0x00100300u;      // readable: calls kSleeper on its fiber
    constexpr uint32_t kSleeper = 0x00100400u;     // SleepThread: never woken
    constexpr uint32_t kPublishedRa = 0x00100508u; // the waiter's call's return address in its original
    constexpr uint32_t kNever = 0x00100600u;       // the waiter's caller's return address
    constexpr uint32_t kDm2Low = 0x0010000Cu;      // DM2 has code in the first free slot of AI's table
    constexpr uint32_t kRpcSid = 0x0BADF00Du;
    constexpr uint32_t kRpcServerData = 0x00180000u;

    enum Game
    {
        Ai,
        Dm2,
        kGames
    };
    const char *const kElfNames[kGames] = {"LIFE_AI.ELF", "LIFE_DM2.ELF"};

    // What a run saw, filled by the guest code and the hooks.
    struct Seen
    {
        int entries = 0;
        Fn tableAtEntry = nullptr;
        int wrapperCalls = 0;
        int originalCalls = 0;
        bool rpcServerAtEntry = false;
        bool reachedSleep = false;
        bool neverRan = false;
        size_t waitingAtStop = 0;
        Fn publishedAtStop = nullptr;
        bool timedOut = false;
        // What the boot's override read of its BootConfig, at its install
        // and on the game thread.
        std::string gameAtInstall;
        std::string profileAtInstall;
        std::string profileAtEntry;
    };
    Seen g_seen;
    std::atomic<bool> g_sleeping{false};
    std::atomic<uint64_t> g_vsyncAtSleep{0}; // the guest's vsync tick when the sleeper went to sleep

    std::array<Fn, kSlots> g_tables[kGames]{};

    uint32_t slotOf(uint32_t address) { return (address - kTableBase) >> 2; }

    void returnToRa(R5900Context *ctx) { ctx->pc = static_cast<uint32_t>(ee::gpr64(ctx, ee::reg::ra)); }

    void hookedOriginal(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        ++g_seen.originalCalls;
        returnToRa(ctx);
    }

    // An override's hook, as the game modules write them: the original
    // kept when it installs, called from the wrapper.
    Fn g_hookOriginal[kGames] = {};
    template <int G>
    void hookWrapper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (++g_seen.wrapperCalls > 4)
            return; // a wrapper around itself: stop the test, not the process
        g_hookOriginal[G](rdram, ctx, runtime);
    }

    void originalRest(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnToRa(ctx); }
    void dm2Low(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnToRa(ctx); }
    void never(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_seen.neverRan = true;
        ctx->pc = kNever;
    }

    void sleeper(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_seen.reachedSleep = true;
        g_vsyncAtSleep.store(runtime->memory().gs().vsyncTick.load(std::memory_order_acquire), std::memory_order_relaxed);
        g_sleeping.store(true, std::memory_order_release);
        ps2_syscalls::SleepThread(rdram, ctx, runtime);
        returnToRa(ctx);
    }

    // Readable, its callee's wait leaves it waiting on its fiber; the call's
    // return address published, so the call returns through a return slot.
    int32_t waitsForever()
    {
        ee::StackFrame frame(0x20);
        ee::calleeSaves({{31, kPublishedRa}});
        return ee::call<int32_t>(kSleeper, 0u) + 1;
    }

    // The game's start: what the run before left is checked, then a server
    // registered with the kernel (left behind for the next run), then the
    // readable function that waits.
    std::string bootText(const char *name)
    {
        const char *value = zx::boot::text(name);
        return value ? value : "(null)";
    }

    void entry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ++g_seen.entries;
        g_seen.profileAtEntry = bootText("ZX_SAVE_PROFILE");
        g_seen.tableAtEntry = zx::function_table::active().table ? zx::function_table::active().table[0] : nullptr;
        uint32_t serverData = 0;
        g_seen.rpcServerAtEntry = ps2_syscalls::lookupEeRpcServer(kRpcSid, serverData);

        R5900Context call = *ctx;
        ee::setGpr64(&call, ee::reg::ra, kEntry + 0x10u);
        runtime->lookupFunction(kHooked)(rdram, &call, runtime);

        R5900Context rpc = *ctx;
        ps2_syscalls::SifInitRpc(rdram, &rpc, runtime);
        ee::setGpr64(&rpc, ee::reg::a0, kRpcServerData);
        ee::setGpr64(&rpc, ee::reg::a1, kRpcSid);
        ee::setGpr64(&rpc, ee::reg::a2, 0u);
        ee::setGpr64(&rpc, ee::reg::a3, 0u);
        ee::setGpr64(&rpc, ee::reg::sp, 0x00170000u);
        ps2_syscalls::SifRegisterRpc(rdram, &rpc, runtime);

        ee::setGpr64(ctx, ee::reg::ra, kNever);
        ctx->pc = kWaiter;
    }

    // Distinct first slots tell the two tables apart.
    void aiMarker(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnToRa(ctx); }
    void dm2Marker(uint8_t *, R5900Context *ctx, PS2Runtime *) { returnToRa(ctx); }

    void fillTable(Game game)
    {
        auto &t = g_tables[game];
        t.fill(nullptr);
        t[0] = game == Ai ? &aiMarker : &dm2Marker;
        t[slotOf(kEntry)] = &entry;
        t[slotOf(kHooked)] = &hookedOriginal;
        t[slotOf(kWaiter)] = &ee::entrySuspendable<&waitsForever>;
        t[slotOf(kSleeper)] = &sleeper;
        t[slotOf(kPublishedRa)] = &originalRest;
        t[slotOf(kNever)] = &never;
        if (game == Dm2)
            t[slotOf(kDm2Low)] = &dm2Low;
    }

    template <int G>
    void applyGame(PS2Runtime &runtime)
    {
        ee::g_ram = runtime.memory().getRDRAM();
        g_seen.gameAtInstall = zx::boot::game();
        g_seen.profileAtInstall = bootText("ZX_SAVE_PROFILE");
        g_hookOriginal[G] = runtime.lookupFunction(kHooked);
        runtime.replaceFunction(kHooked, &hookWrapper<G>);
        ee::installSuspendableCalls(runtime);
    }
}

PS2_REGISTER_GAME_OVERRIDE("lifecycle test AI", "LIFE_AI.ELF", 0u, 0u, &applyGame<Ai>)
PS2_REGISTER_GAME_OVERRIDE("lifecycle test DM2", "LIFE_DM2.ELF", 0u, 0u, &applyGame<Dm2>)

namespace
{
    void registerGames()
    {
        static const bool registered = []
        {
            for (int g = 0; g < kGames; ++g)
            {
                fillTable(static_cast<Game>(g));
                ps2_generated::add({g == Ai ? "life_ai" : "life_dm2", kElfNames[g], kTableBase, kTableEnd, kSlots,
                                    g_tables[g].data()});
            }
            return true;
        }();
        (void)registered;
    }

    // One PT_LOAD segment of code at kCodeBase, entry kEntry.
    std::vector<uint8_t> elfImage()
    {
        constexpr uint32_t kHeader = 52u, kProgramHeader = 32u, kCodeBytes = 0x1000u;
        std::vector<uint8_t> elf(kHeader + kProgramHeader + kCodeBytes, 0u);
        auto put16 = [&](size_t at, uint16_t v) { std::memcpy(elf.data() + at, &v, 2); };
        auto put32 = [&](size_t at, uint32_t v) { std::memcpy(elf.data() + at, &v, 4); };
        const uint8_t ident[] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
        std::memcpy(elf.data(), ident, sizeof(ident));
        put16(16, 2);     // ET_EXEC
        put16(18, 8);     // EM_MIPS
        put32(20, 1);     // version
        put32(24, kEntry);
        put32(28, kHeader); // phoff
        put16(40, kHeader); // ehsize
        put16(42, kProgramHeader);
        put16(44, 1);       // phnum
        const size_t ph = kHeader;
        put32(ph + 0, 1);   // PT_LOAD
        put32(ph + 4, kHeader + kProgramHeader);
        put32(ph + 8, kCodeBase);
        put32(ph + 12, kCodeBase);
        put32(ph + 16, kCodeBytes);
        put32(ph + 20, kCodeBytes);
        put32(ph + 24, 5); // R X
        put32(ph + 28, 0x1000);
        return elf;
    }

    void setEnv(const char *name, const char *value)
    {
#if defined(_WIN32)
        _putenv_s(name, value ? value : "");
#else
        if (value)
            ::setenv(name, value, 1);
        else
            ::unsetenv(name);
#endif
    }

    // Headless and unpaced, the cards and host folder in a scratch folder of
    // this test run's own. (The EE scheduler reads ZX_UNPACED once a
    // process: after the suites before these have run a runtime, the guest's
    // vblanks here are paced on the host clock.)
    struct HeadlessRuns
    {
        std::filesystem::path folder = std::filesystem::temp_directory_path() /
                                       ("zx-lifecycle-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        PS2Runtime::IoPaths savedPaths = PS2Runtime::getIoPaths();
        uint8_t *savedRam = ee::g_ram;
        HeadlessRuns()
        {
            registerGames();
            setEnv("ZX_GS_NULL", "1");
            setEnv("ZX_HEADLESS", "1");
            setEnv("ZX_UNPACED", "1");
            std::filesystem::create_directories(folder / "mc");
            PS2Runtime::IoPaths paths;
            paths.elfDirectory = folder;
            paths.hostRoot = folder;
            paths.cdRoot = folder;
            paths.mcRoot = folder / "mc";
            PS2Runtime::setIoPaths(paths);
        }
        ~HeadlessRuns()
        {
            if (ps2_host::initialized())
                ps2_host::shutdown();
            setEnv("ZX_GS_NULL", nullptr);
            setEnv("ZX_HEADLESS", nullptr);
            setEnv("ZX_UNPACED", nullptr);
            PS2Runtime::setIoPaths(savedPaths);
            // Back to the global table, for the tests after these.
            ps2_generated::select("NOT_A_LIFECYCLE_GAME.ELF");
            ee::g_ram = savedRam;
            std::error_code ec;
            std::filesystem::remove_all(folder, ec);
        }
    };

    void observeStop(PS2Runtime &runtime)
    {
        g_seen.waitingAtStop = ee::waitingFunctions();
        g_seen.publishedAtStop = runtime.tableEntry(kPublishedRa);
    }

    struct RunResult
    {
        bool booted = false;
        Seen seen;
        size_t waitingAfter = 0;
        Fn publishedAfter = nullptr;
        Fn lowAfterInstall = nullptr;
        Fn nextAfterInstall = nullptr;
        bool hostUpAfter = false;
    };

    // Construct, initialize, load, run until the readable function has waited
    // three of the guest's vblanks, destroy: one game's life in the
    // collection. The stop goes by the guest's vsync clock, not the host's
    // frames: what the boot does at a vblank (the state probe's log) has run
    // by then, paced or not, however late the EE thread gets the CPU.
    RunResult runGame(Game game, bool hostOwned, zx::BootConfig config = {})
    {
        RunResult result;
        g_seen = Seen{};
        g_sleeping.store(false, std::memory_order_release);
        g_vsyncAtSleep.store(0u, std::memory_order_relaxed);
        {
            PS2Runtime runtime(std::move(config));
            PS2Runtime::HostOptions options;
            options.hostOwned = hostOwned;
            runtime.setHostOptions(options);
            runtime.addStoppedHook(&observeStop); // before the readable install's: it sees what is left
            if (!runtime.initialize("lifecycle test") ||
                !runtime.loadELFImage((PS2Runtime::getIoPaths().hostRoot / kElfNames[game]).string(), elfImage(), false))
                return result;
            result.booted = true;
            result.lowAfterInstall = runtime.tableEntry(kDm2Low);
            result.nextAfterInstall = runtime.tableEntry(kDm2Low + 4u);
            int frames = 0;
            runtime.setHostFrameCallback(
                [&](PS2Runtime &rt)
                {
                    const bool waited = g_sleeping.load(std::memory_order_acquire) &&
                                        rt.memory().gs().vsyncTick.load(std::memory_order_acquire) >=
                                            g_vsyncAtSleep.load(std::memory_order_relaxed) + 3u;
                    if (waited || ++frames > 3000)
                    {
                        g_seen.timedOut = !waited;
                        rt.requestStop();
                    }
                });
            runtime.run();
            result.seen = g_seen;
            result.waitingAfter = ee::waitingFunctions();
            result.publishedAfter = runtime.tableEntry(kPublishedRa);
        }
        result.hostUpAfter = ps2_host::initialized();
        return result;
    }

    void checkRun(TestCase &t, const RunResult &r, Game game, const std::string &label)
    {
        t.IsTrue(r.booted, label + ": booted");
        if (!r.booted)
            return;
        t.Equals(r.seen.entries, 1, label + ": the entry ran once");
        t.IsTrue(r.seen.tableAtEntry == (game == Ai ? Fn(&aiMarker) : Fn(&dm2Marker)), label + ": its own table");
        t.Equals(r.seen.wrapperCalls, 1, label + ": the override's hook wraps the generated function, once");
        t.Equals(r.seen.originalCalls, 1, label + ": and that ran");
        t.IsFalse(r.seen.rpcServerAtEntry, label + ": no RPC server from the run before");
        t.IsTrue(r.seen.reachedSleep && !r.seen.timedOut, label + ": the readable function's callee went to sleep");
        t.IsFalse(r.seen.neverRan, label + ": the readable function did not return");
        t.Equals(r.seen.waitingAtStop, size_t{1}, label + ": one function waiting at the stop");
        t.IsTrue(r.seen.publishedAtStop != Fn(&originalRest), label + ": its call's return slot set up");
        t.Equals(r.waitingAfter, size_t{0}, label + ": dropped when the run stopped");
        t.IsTrue(r.publishedAfter == Fn(&originalRest), label + ": the return slot back to the original's");
        if (game == Dm2)
            t.IsTrue(r.lowAfterInstall == Fn(&dm2Low), label + ": the resume slots chosen around DM2's own code");
        else
            t.IsTrue(r.lowAfterInstall != nullptr, label + ": a resume slot in AI's first free slot");
    }
}

void register_ps2_runtime_lifecycle_tests()
{
    MiniTest::Case("RuntimeLifecycle", [](TestCase &tc)
    {
        tc.Run("AI, DM2, AI in one process: each from its table as generated, the host kept by its owner", [](TestCase &t)
        {
            HeadlessRuns runs;
            const Game order[] = {Ai, Dm2, Ai};
            RunResult results[std::size(order)];
            for (size_t i = 0; i < std::size(order); ++i)
            {
                const std::string label = std::string("run ") + std::to_string(i + 1) + " (" + kElfNames[order[i]] + ")";
                results[i] = runGame(order[i], true);
                checkRun(t, results[i], order[i], label);
                t.IsTrue(results[i].hostUpAfter, label + ": the host still up after the runtime");
            }
            // AI's first free slot holds the resume slot; in DM2's table that
            // address is DM2's code, and the resume slot is the next one.
            t.IsTrue(results[1].nextAfterInstall == results[0].lowAfterInstall,
                     "DM2: the resume slot in its own table's first free slot");
            t.IsTrue(results[2].lowAfterInstall == results[0].lowAfterInstall, "AI again: the same resume slot");
        });

        tc.Run("the same game twice, each runtime with its own host: torn down in order, up again", [](TestCase &t)
        {
            HeadlessRuns runs;
            for (int i = 1; i <= 2; ++i)
            {
                const std::string label = "run " + std::to_string(i);
                const RunResult r = runGame(Ai, false);
                checkRun(t, r, Ai, label);
                t.IsFalse(r.hostUpAfter, label + ": the host shut down with the runtime");
            }
        });

        tc.Run("a table chosen again gets its generated entries back; other tables keep theirs", [](TestCase &t)
        {
            HeadlessRuns runs;
            ps2_generated::select(kElfNames[Ai]);
            PS2Runtime runtime;
            runtime.replaceFunction(kHooked, &originalRest);
            runtime.replaceFunction(kHooked, &never); // (the first entry is what goes back)
            t.IsTrue(runtime.tableEntry(kHooked) == Fn(&never), "AI: replaced");
            ps2_generated::select(kElfNames[Dm2]);
            t.IsTrue(runtime.tableEntry(kHooked) == Fn(&hookedOriginal), "DM2: its own");
            runtime.replaceFunction(kEntry, &never);
            ps2_generated::select(kElfNames[Ai]);
            t.IsTrue(runtime.tableEntry(kHooked) == Fn(&hookedOriginal), "AI again: as generated");
            t.IsTrue(g_tables[Dm2][slotOf(kEntry)] == Fn(&never), "DM2's change waits for DM2's select");
            ps2_generated::select(kElfNames[Dm2]);
            t.IsTrue(runtime.tableEntry(kEntry) == Fn(&entry), "DM2 again: as generated");
        });

        tc.Run("the same game booted three times with no table chosen between (a lone game module): each boot's hook wraps the generated function", [](TestCase &t)
        {
            // Android with one game's code: no other game's to choose, so the
            // ELF load's select() does not run; what puts the table back is
            // each boot's end. Every hook takes its original as the game
            // overrides do (lookupFunction, then replaceFunction).
            HeadlessRuns runs;
            ps2_generated::select(kElfNames[Ai]);
            for (int boot = 1; boot <= 3; ++boot)
            {
                const std::string label = "boot " + std::to_string(boot);
                {
                    PS2Runtime runtime;
                    g_hookOriginal[Ai] = runtime.lookupFunction(kHooked);
                    t.IsTrue(g_hookOriginal[Ai] == Fn(&hookedOriginal), label + ": the original is the generated function");
                    runtime.replaceFunction(kHooked, &hookWrapper<Ai>);
                    runtime.replaceFunction(kNever, &originalRest);
                    t.IsTrue(runtime.tableEntry(kHooked) == Fn(&hookWrapper<Ai>), label + ": hooked");
                }
                t.IsTrue(g_tables[Ai][slotOf(kHooked)] == Fn(&hookedOriginal) && g_tables[Ai][slotOf(kNever)] == Fn(&never),
                         label + ": its end puts the table back as generated");
            }
        });

        tc.Run("a boot's end puts back every table it changed, the one in use and the others", [](TestCase &t)
        {
            HeadlessRuns runs;
            ps2_generated::select(kElfNames[Dm2]);
            {
                PS2Runtime runtime;
                runtime.replaceFunction(kEntry, &never);
                ps2_generated::select(kElfNames[Ai]);
                runtime.replaceFunction(kHooked, &never);
                t.IsTrue(g_tables[Dm2][slotOf(kEntry)] == Fn(&never) && g_tables[Ai][slotOf(kHooked)] == Fn(&never),
                         "both changed while it runs");
            }
            t.IsTrue(g_tables[Dm2][slotOf(kEntry)] == Fn(&entry), "DM2's table as generated after it");
            t.IsTrue(g_tables[Ai][slotOf(kHooked)] == Fn(&hookedOriginal), "AI's too");
            t.Equals(zx::function_table::restoreGenerated(), size_t{0}, "nothing left to put back");
        });

        tc.Run("two boots in one process with different BootConfigs: each reads its own, the environment wins", [](TestCase &t)
        {
            HeadlessRuns runs;
            setEnv("ZX_SAVE_PROFILE", nullptr);
            auto configFor = [&](const char *game, const char *profile, const std::string &log)
            {
                zx::BootConfig config;
                config.game = game;
                config.set("ZX_SAVE_PROFILE", profile);
                // The state probe (a per-boot read): the entry's first word,
                // logged to the boot's own file.
                config.set("ZX_STATE_LOG", "0x00100108");
                config.set("ZX_STATE_LOG_FILE", (runs.folder / log).string());
                return config;
            };
            const RunResult ai = runGame(Ai, true, configFor("ai", "AiCareer", "ai.state"));
            const RunResult dm2 = runGame(Dm2, true, configFor("dm2", "Dm2Career", "dm2.state"));
            checkRun(t, ai, Ai, "AI");
            checkRun(t, dm2, Dm2, "DM2");
            t.Equals(ai.seen.gameAtInstall, std::string("ai"), "AI: its game");
            t.Equals(ai.seen.profileAtInstall, std::string("AiCareer"), "AI: its profile at install");
            t.Equals(ai.seen.profileAtEntry, std::string("AiCareer"), "AI: on the game thread");
            t.Equals(dm2.seen.gameAtInstall, std::string("dm2"), "DM2: its game");
            t.Equals(dm2.seen.profileAtInstall, std::string("Dm2Career"), "DM2: its own profile, not AI's");
            t.Equals(dm2.seen.profileAtEntry, std::string("Dm2Career"), "DM2: on the game thread");
            auto lines = [&](const char *log)
            {
                std::ifstream in(runs.folder / log);
                std::stringstream text;
                text << in.rdbuf();
                return text.str();
            };
            t.IsTrue(lines("ai.state").find("[state] tick=") != std::string::npos, "AI: its state log in its file");
            t.IsTrue(lines("dm2.state").find("[state] tick=") != std::string::npos, "DM2: its state log in its own file");

            setEnv("ZX_SAVE_PROFILE", "FromEnvironment");
            const RunResult env = runGame(Ai, true, configFor("ai", "AiCareer", "ai2.state"));
            setEnv("ZX_SAVE_PROFILE", nullptr);
            checkRun(t, env, Ai, "AI with the environment");
            t.Equals(env.seen.profileAtInstall, std::string("FromEnvironment"), "the environment wins over the boot's");
            const RunResult bare = runGame(Ai, true);
            checkRun(t, bare, Ai, "AI with no config");
            t.Equals(bare.seen.gameAtInstall, std::string(""), "no config: no game");
            t.Equals(bare.seen.profileAtInstall, std::string("(null)"), "no config: nothing of the boots before");
        });

        tc.Run("a step file: its settings, its BootConfig names, and what it must have", [](TestCase &t)
        {
            zx::sequence::Step step;
            std::string error;
            t.IsTrue(zx::sequence::parseStep("dm2",
                                               "# a check's step\nelf=/discs/SLUS_201.59\nticks=9000\nseconds=600\r\nstall_seconds=60\n"
                                               "stop_after=/dumps/n-last.sp\nmc_root=/cards/mc0\ncwd=/run\n"
                                               "ZX_PAD_SCRIPT=10v:START:2v,20v:SYNC=0x259770==1\nZX_DUMP_PREFIX=/dumps/n\n",
                                               step, &error),
                     "parsed: " + error);
            t.Equals(step.game, std::string("dm2"), "its game");
            t.Equals(step.config.game, std::string("dm2"), "the BootConfig's game");
            t.Equals(step.elf.string(), std::string("/discs/SLUS_201.59"), "elf");
            t.Equals(step.tickLimit, uint64_t{9000}, "tick limit");
            t.IsTrue(step.secondsLimit == 600.0, "seconds (a CR LF line)");
            t.IsTrue(step.stallSeconds == 60.0, "stall_seconds");
            t.Equals(step.stopAfter.string(), std::string("/dumps/n-last.sp"), "stop_after");
            t.Equals(step.mcRoot.string(), std::string("/cards/mc0"), "mc_root");
            t.Equals(step.workingDirectory.string(), std::string("/run"), "cwd");
            const std::string *script = step.config.find("ZX_PAD_SCRIPT");
            t.IsTrue(script && *script == "10v:START:2v,20v:SYNC=0x259770==1", "a value with = and : in it, whole");
            t.IsFalse(zx::sequence::parseStep("ai", "ticks=10\n", step, &error), "no elf");
            t.IsFalse(zx::sequence::parseStep("ai", "elf=/a\n", step, &error), "no tick limit");
            t.IsFalse(zx::sequence::parseStep("ai", "elf=/a\nticks=ten\n", step, &error), "a tick limit that is no count");
            t.IsFalse(zx::sequence::parseStep("ai", "elf=/a\nticks=10\nZX_GS_NULL=1\n", step, &error),
                      "a process-wide switch is not a step's");
            t.IsTrue(error.find("ZX_GS_NULL") != std::string::npos, "and the error names it");
            std::vector<zx::sequence::Step> steps;
            t.IsFalse(zx::sequence::parseArgument("ai", steps, &error), "an argument item without a file");
            t.IsFalse(zx::sequence::parseArgument("ai:/no/such/step/file", steps, &error), "a step file that is not there");
        });

        tc.Run("a sequence: AI then DM2 in one process, each its own config and stop, the host down at the end", [](TestCase &t)
        {
            HeadlessRuns runs;
            const std::vector<uint8_t> elf = elfImage();
            for (int g = 0; g < kGames; ++g)
                std::ofstream(runs.folder / kElfNames[g], std::ios::binary)
                    .write(reinterpret_cast<const char *>(elf.data()), static_cast<std::streamsize>(elf.size()));
            // DM2 stops once a file its run writes is there: its state log.
            const std::filesystem::path marker = runs.folder / "dm2.state";
            auto stepFile = [&](const char *name, const std::string &text)
            {
                std::ofstream(runs.folder / name) << text;
                return (runs.folder / name).string();
            };
            const std::string ai = stepFile("ai.step", "elf=" + (runs.folder / kElfNames[Ai]).string() +
                                                           "\nticks=40\nZX_SAVE_PROFILE=AiCareer\nZX_STATE_LOG=0x00100108\n"
                                                           "ZX_STATE_LOG_FILE=" + (runs.folder / "ai.state").string() + "\n");
            const std::string dm2 = stepFile("dm2.step", "elf=" + (runs.folder / kElfNames[Dm2]).string() +
                                                             "\nticks=100000\nstop_after=" + marker.string() +
                                                             "\nZX_SAVE_PROFILE=Dm2Career\nZX_STATE_LOG=0x00100108\n"
                                                             "ZX_STATE_LOG_FILE=" + marker.string() + "\n");
            std::vector<zx::sequence::Step> steps;
            std::string error;
            t.IsTrue(zx::sequence::parseArgument("ai:" + ai + ",dm2:" + dm2, steps, &error), "parsed: " + error);
            t.Equals(steps.size(), size_t{2}, "two steps");
            if (steps.size() != 2)
                return;
            setEnv("ZX_SAVE_PROFILE", nullptr);
            g_seen = Seen{};
            const std::filesystem::path cwd = std::filesystem::current_path();
            const auto outcomes = zx::sequence::run(steps, "lifecycle sequence");
            t.Equals(outcomes.size(), size_t{2}, "both steps ran");
            if (outcomes.size() != 2)
                return;
            t.IsTrue(outcomes[0].booted && outcomes[1].booted, "both booted");
            t.Equals(outcomes[0].stop, std::string("ticks"), "AI stopped at its tick limit");
            t.IsTrue(outcomes[0].ticks >= 40, "at tick 40 or after");
            t.Equals(outcomes[1].stop, std::string("file"), "DM2 stopped once its file was there");
            t.IsTrue(outcomes[1].ticks < 100000, "before its tick limit");
            t.Equals(g_seen.entries, 2, "each game's entry ran");
            t.Equals(g_seen.gameAtInstall, std::string("dm2"), "the last install was DM2's, with its game");
            t.Equals(g_seen.profileAtInstall, std::string("Dm2Career"), "and its own profile");
            auto has = [&](const char *log)
            {
                std::ifstream in(runs.folder / log);
                std::stringstream text;
                text << in.rdbuf();
                return text.str().find("[state] tick=") != std::string::npos;
            };
            t.IsTrue(has("ai.state") && has("dm2.state"), "each step's state log in its own file");
            t.IsFalse(ps2_host::initialized(), "the host shut down after the last step");
            t.Equals(std::filesystem::current_path().string(), cwd.string(), "the working directory as it was");
            t.Equals(PS2Runtime::getIoPaths().mcRoot.string(), (runs.folder / "mc").string(), "the I/O folders as they were");
        });

        tc.Run("a test run's sound goes to SDL's dummy device (ZX_AUDIO=off, unpaced runs); ZX_AUDIO=on plays it", [](TestCase &t)
        {
            auto saved = [](const char *name) -> std::optional<std::string>
            {
                const char *value = std::getenv(name);
                return value ? std::optional<std::string>(value) : std::nullopt;
            };
            const auto savedAudio = saved("ZX_AUDIO");
            const auto savedUnpaced = saved("ZX_UNPACED");
            const auto savedDriver = saved("SDL_AUDIO_DRIVER");
            const auto savedDiskFile = saved("SDL_AUDIO_DISK_OUTPUT_FILE");
            const std::filesystem::path diskFile = std::filesystem::temp_directory_path() /
                ("zx-audio-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".raw");
            ps2_host::WindowOptions options;
            options.headless = true;
            auto driverWith = [&](const char *audio, const char *unpaced) -> std::string
            {
                setEnv("ZX_AUDIO", audio);
                setEnv("ZX_UNPACED", unpaced);
                if (!ps2_host::initialize("audio test", options))
                    return "no host";
                const std::string driver = ps2_host::audioReady() ? ps2_host::audioDriver() : "no device";
                ps2_host::shutdown();
                return driver;
            };
            t.Equals(driverWith("off", nullptr), std::string("dummy"), "ZX_AUDIO=off: a device, SDL's dummy one");
            t.Equals(driverWith(nullptr, "1"), std::string("dummy"), "an unpaced run: the same");
            // The system's driver otherwise, here SDL's disk one (to a file), so the test makes no sound either.
            setEnv("SDL_AUDIO_DRIVER", "disk");
            setEnv("SDL_AUDIO_DISK_OUTPUT_FILE", diskFile.string().c_str());
            t.Equals(driverWith("on", "1"), std::string("disk"), "ZX_AUDIO=on: the system's driver, unpaced or not");
            t.Equals(driverWith(nullptr, nullptr), std::string("disk"), "a paced run without ZX_AUDIO: the same");
            setEnv("ZX_AUDIO", savedAudio ? savedAudio->c_str() : nullptr);
            setEnv("ZX_UNPACED", savedUnpaced ? savedUnpaced->c_str() : nullptr);
            setEnv("SDL_AUDIO_DRIVER", savedDriver ? savedDriver->c_str() : nullptr);
            setEnv("SDL_AUDIO_DISK_OUTPUT_FILE", savedDiskFile ? savedDiskFile->c_str() : nullptr);
            std::error_code ec;
            std::filesystem::remove(diskFile, ec);
        });
    });
}
