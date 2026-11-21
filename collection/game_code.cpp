#include "collection/game_code.h"

#include "runtime/game_module.h"
#include "vu1/vu1_aot_generator.h"

#include "collection/disc_check.h"
#include "game_module/abi.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

extern char **environ;
#endif

namespace zx::app
{
    namespace
    {
        namespace fs = std::filesystem;

        // What a module is built for: the target the compiler is given, the flags file's architecture, the module's
        // file name, and its linker's kind (lld's flavours: the host's linker program takes another flavour's
        // modules with -flavor).
        enum class LinkKind
        {
            MachO,
            Coff,
            Elf
        };
        struct Target
        {
            const char *name;   // GameCodeRequest::target
            const char *triple;
            const char *arch;
            const char *extension;
            LinkKind link;
        };
        constexpr Target kTargets[] = {
            {"macos-arm64", "arm64-apple-macos11.0", "arm64", ".dylib", LinkKind::MachO},
            {"macos-x86_64", "x86_64-apple-macos11.0", "x86_64", ".dylib", LinkKind::MachO},
            {"windows-x86_64", "x86_64-pc-windows-msvc", "x86_64", ".dll", LinkKind::Coff},
            // (the Thor's: Android arm64, API 33)
            {"android-arm64", "aarch64-linux-android33", "arm64", ".so", LinkKind::Elf},
            {"linux-arm64", "aarch64-linux-gnu", "arm64", ".so", LinkKind::Elf},
            {"linux-x86_64", "x86_64-linux-gnu", "x86_64", ".so", LinkKind::Elf},
        };
        // This app's own: the modules it loads.
#if defined(__APPLE__) && defined(__aarch64__)
        constexpr const char *kHostTarget = "macos-arm64";
#elif defined(__APPLE__)
        constexpr const char *kHostTarget = "macos-x86_64";
#elif defined(_WIN32)
        constexpr const char *kHostTarget = "windows-x86_64";
#elif defined(__ANDROID__)
        constexpr const char *kHostTarget = "android-arm64";
#elif defined(__aarch64__)
        constexpr const char *kHostTarget = "linux-arm64";
#else
        constexpr const char *kHostTarget = "linux-x86_64";
#endif
        const Target *findTarget(const std::string &name)
        {
            const std::string want = name.empty() ? kHostTarget : name;
            for (const Target &target : kTargets)
            {
                if (want == target.name)
                    return &target;
            }
            return nullptr;
        }
        const Target &hostTarget() { return *findTarget(kHostTarget); }
        // The request's target (an unknown name: none, which makeGameCode refuses; the key and file are the host's).
        const Target &requestTarget(const GameCodeRequest &request)
        {
            const Target *target = findTarget(request.target);
            return target ? *target : hostTarget();
        }
#if defined(_WIN32)
        constexpr const char *kExe = ".exe";
#else
        constexpr const char *kExe = "";
#endif
        // Generated sources compiled as one unit (the workshop's unity batch, ps2/CMakeLists.txt).
        constexpr size_t kUnitSources = 32;

#if defined(__ANDROID__)
        // The APK's native libraries, lib<name>.so (an Android app runs programs from its native library folder alone):
        // the compiler as libclang.so, so its name says nothing of C++ (the compile says --driver-mode=g++), the linker
        // as liblld.so, which says no flavour either (the link says -flavor gnu).
        constexpr bool kProgramsNamed = false;
        fs::path tool(const GameCodeTools &tools, const char *name)
        {
            const std::string program = name == std::string("clang++") ? "clang"
                                        : name == std::string("ld.lld") ? "lld" : name;
            return tools.programs / ("lib" + program + ".so");
        }
        // The folder of the library this code is in (libzx-runner.so): the app's native library folder.
        fs::path programsFolder(const fs::path &)
        {
            Dl_info info{};
            if (dladdr(reinterpret_cast<const void *>(&programsFolder), &info) && info.dli_fname)
                return fs::path(info.dli_fname).parent_path();
            return {};
        }
#else
        // (the programs' own names: clang++ is the C++ driver, and each linker's name is its flavour)
        constexpr bool kProgramsNamed = true;
        fs::path tool(const GameCodeTools &tools, const char *name) { return tools.programs / (std::string(name) + kExe); }
        fs::path programsFolder(const fs::path &root) { return root / "bin"; }
#endif
        fs::path linker(const GameCodeTools &tools)
        {
#if defined(__APPLE__)
            return tool(tools, "ld64.lld");
#elif defined(_WIN32)
            return tool(tools, "lld-link");
#else
            return tool(tools, "ld.lld");
#endif
        }
        // The compiler's own headers (lib/clang/<version>), as -resource-dir: where the compiler looks for them by
        // itself only when it is in bin/ beside them.
        fs::path resourceFolder(const GameCodeTools &tools)
        {
            std::error_code ec;
            for (const auto &item : fs::directory_iterator(tools.root / "lib" / "clang", ec))
            {
                if (item.is_directory(ec))
                    return item.path();
            }
            return {};
        }
        fs::path sdk(const GameCodeTools &tools) { return tools.root / "sdk"; }
        fs::path gameInputs(const GameCodeTools &tools, const std::string &id) { return tools.root / "games" / id; }

        bool readFile(const fs::path &path, std::string &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            std::ostringstream text;
            text << in.rdbuf();
            out = text.str();
            return true;
        }

        std::string fileSha256(const fs::path &path)
        {
            std::string bytes;
            if (!readFile(path, bytes))
                return "missing";
            return sha256Hex(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
        }

        // A program run to its end: its exit code, and what it printed (stdout and stderr together).
#if defined(__ANDROID__)
        // (SDL makes no processes on Android: posix_spawn, as its POSIX processes are made elsewhere; a program a
        // signal stopped, the system's low-memory killer's included, exits 128 + the signal)
        bool runProgram(const std::vector<std::string> &args, std::string &output, int &exitCode)
        {
            std::vector<char *> argv;
            for (const std::string &a : args)
                argv.push_back(const_cast<char *>(a.c_str()));
            argv.push_back(nullptr);
            exitCode = -1;
            int pipes[2];
            if (pipe2(pipes, O_CLOEXEC) != 0)
            {
                output = "could not start " + args[0] + ": " + std::strerror(errno);
                return false;
            }
            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
            posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&actions, pipes[1], STDERR_FILENO);
            pid_t pid = 0;
            const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            close(pipes[1]);
            if (spawned != 0)
            {
                close(pipes[0]);
                output = "could not start " + args[0] + ": " + std::strerror(spawned);
                return false;
            }
            output.clear();
            char buffer[4096];
            for (;;)
            {
                const ssize_t got = read(pipes[0], buffer, sizeof buffer);
                if (got > 0)
                    output.append(buffer, static_cast<size_t>(got));
                else if (got == 0 || errno != EINTR)
                    break;
            }
            close(pipes[0]);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }
            exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
            return exitCode == 0;
        }
#else
        bool runProgram(const std::vector<std::string> &args, std::string &output, int &exitCode)
        {
            std::vector<const char *> argv;
            for (const std::string &a : args)
                argv.push_back(a.c_str());
            argv.push_back(nullptr);
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
            SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
            SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
            SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
            SDL_Process *process = SDL_CreateProcessWithProperties(props);
            SDL_DestroyProperties(props);
            if (!process)
            {
                output = std::string("could not start ") + args[0] + ": " + SDL_GetError();
                exitCode = -1;
                return false;
            }
            size_t size = 0;
            exitCode = -1;
            void *data = SDL_ReadProcess(process, &size, &exitCode);
            output.assign(data ? static_cast<const char *>(data) : "", data ? size : 0);
            SDL_free(data);
            SDL_DestroyProcess(process);
            return exitCode == 0;
        }
#endif

        // The recompiler's config for this disc: the bundled one with its paths made this build's.
        std::string recompilerConfig(const std::string &config, const fs::path &elf, const fs::path &output,
                                     const fs::path &functions)
        {
            std::istringstream in(config);
            std::ostringstream out;
            std::string line;
            auto quoted = [](const fs::path &p) { return "\"" + p.generic_string() + "\""; };
            while (std::getline(in, line))
            {
                if (line.rfind("input =", 0) == 0 || line.rfind("input=", 0) == 0)
                    line = "input = " + quoted(elf);
                else if (line.rfind("output =", 0) == 0 || line.rfind("output=", 0) == 0)
                    line = "output = " + quoted(output);
                else if (line.rfind("ghidra_output =", 0) == 0 || line.rfind("ghidra_output=", 0) == 0)
                    line = "ghidra_output = " + quoted(functions);
                out << line << '\n';
            }
            return out.str();
        }

        // The compile's flags: game_module_flags.txt's lines of these kinds ("all" and an architecture's for every
        // unit; "vu1", "vu1aot" for VU1's).
        std::vector<std::string> moduleFlags(const GameCodeTools &tools, std::vector<std::string> kinds)
        {
            std::vector<std::string> flags;
            std::ifstream in(sdk(tools) / "game_module_flags.txt");
            std::string line;
            while (std::getline(in, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                const size_t space = line.find(' ');
                if (space == std::string::npos)
                    continue;
                const std::string kind = line.substr(0, space);
                if (std::find(kinds.begin(), kinds.end(), kind) != kinds.end())
                    flags.push_back(line.substr(space + 1));
            }
            return flags;
        }

        std::string firstLines(const std::string &text, size_t lines)
        {
            size_t at = 0;
            for (size_t n = 0; n < lines && at != std::string::npos; ++n)
                at = text.find('\n', at + 1);
            return at == std::string::npos ? text : text.substr(0, at);
        }
    }

    bool GameCodeTools::open(const fs::path &folder, std::string *error)
    {
        root = folder;
        programs = programsFolder(folder);
        for (const fs::path &need : {tool(*this, "clang++"), linker(*this), tool(*this, "ps2_recomp"),
                                     sdk(*this) / "include" / "ps2_generated.h", sdk(*this) / "module_entry.cpp",
                                     sdk(*this) / "vu" / "vu1_core.cpp",
                                     sdk(*this) / "game_module_flags.txt", sdk(*this) / "key.txt",
                                     folder / "toolchain.txt"})
        {
            std::error_code ec;
            if (!fs::exists(need, ec))
            {
                if (error)
                    *error = "The app is missing " + need.filename().string() + ". Reinstall the app.";
                return false;
            }
        }
        return true;
    }

    std::string gameCodeKey(const GameCodeTools &tools, const GameCodeRequest &request)
    {
        std::string sdkKey, toolchain;
        readFile(sdk(tools) / "key.txt", sdkKey);
        readFile(tools.root / "toolchain.txt", toolchain);
        const fs::path inputs = gameInputs(tools, request.id);
        const std::string all = "abi " + std::to_string(ZX_GAME_MODULE_ABI) + "\ntarget " +
                                requestTarget(request).triple + "\nsdk " + sdkKey + "\ntoolchain " + toolchain +
                                "\nvu1 generator " +
                                zx::vu1aot::generatorKey() + "\nrecompiler " +
                                fileSha256(tool(tools, "ps2_recomp")) + "\nconfig " +
                                fileSha256(inputs / "config.toml") + "\nfunctions " +
                                fileSha256(inputs / "ghidra-functions.csv") + "\nelf " + fileSha256(request.elf) + "\n";
        return sha256Hex(reinterpret_cast<const uint8_t *>(all.data()), all.size()).substr(0, 16);
    }

    fs::path gameCodeFile(const GameCodeTools &tools, const GameCodeRequest &request)
    {
        return request.cache / (request.id + "-" + gameCodeKey(tools, request) + requestTarget(request).extension);
    }

    bool makeGameCode(const GameCodeTools &tools, const GameCodeRequest &request, const GameCodeProgressFn &progress,
                      std::string *error)
    {
        auto fail = [&](const std::string &why)
        {
            if (error)
                *error = why;
            return false;
        };
        auto report = [&](const char *step, uint64_t done, uint64_t total)
        { return !progress || progress(GameCodeProgress{step, done, total}); };
        if (!findTarget(request.target))
            return fail("No such target for the game's code: " + request.target + ".");
        const Target &platform = requestTarget(request);
        const fs::path target = gameCodeFile(tools, request);
        const fs::path work = request.cache / (request.id + ".making");
        std::error_code ec;
        fs::remove_all(work, ec);
        fs::create_directories(work / "unity", ec);
        fs::create_directories(work / "objects", ec);
        if (ec)
            return fail("Could not make a folder for the game's code in " + request.cache.string() + ": " + ec.message());
        // (removed when done, or kept for a look with ZX_GAME_CODE_KEEP=1)
        struct Cleanup
        {
            fs::path dir;
            ~Cleanup()
            {
                const char *keep = std::getenv("ZX_GAME_CODE_KEEP");
                std::error_code ignored;
                if (!(keep && keep[0] == '1'))
                    fs::remove_all(dir, ignored);
            }
        } cleanup{work};

        // 1. The recompiler: the ELF's code as C++.
        if (!report("Recompiling", 0, 1))
            return fail("");
        std::string config;
        const fs::path inputs = gameInputs(tools, request.id);
        if (!readFile(inputs / "config.toml", config))
            return fail("The app has no recompiler settings for this game (" + request.id + ").");
        const fs::path generated = work / "generated";
        {
            std::ofstream out(work / "config.toml", std::ios::binary);
            out << recompilerConfig(config, fs::absolute(request.elf), generated, inputs / "ghidra-functions.csv");
        }
        std::string output;
        int exitCode = 0;
        // (run so that the setup's Stop ends it: the recompiler takes a minute or two on a desktop, its output read
        // on a thread while the setup is asked every tenth of a second, the process killed when it says stop;
        // Android's, a few seconds, runs as runProgram runs it)
        bool stopped = false;
        auto recompile = [&](const std::vector<std::string> &args) -> bool
        {
#if defined(__ANDROID__)
            return runProgram(args, output, exitCode);
#else
            std::vector<const char *> argv;
            for (const std::string &a : args)
                argv.push_back(a.c_str());
            argv.push_back(nullptr);
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
            SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
            SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
            SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
            SDL_Process *process = SDL_CreateProcessWithProperties(props);
            SDL_DestroyProperties(props);
            if (!process)
            {
                output = std::string("could not start ") + args[0] + ": " + SDL_GetError();
                exitCode = -1;
                return false;
            }
            std::atomic<bool> ended{false};
            void *data = nullptr;
            size_t size = 0;
            int code = -1;
            std::thread reader([&]
            {
                data = SDL_ReadProcess(process, &size, &code);
                ended = true;
            });
            while (!ended)
            {
                if (!report("Recompiling", 0, 1))
                {
                    SDL_KillProcess(process, true);
                    stopped = true;
                    break;
                }
                SDL_Delay(100);
            }
            reader.join();
            output.assign(data ? static_cast<const char *>(data) : "", data ? size : 0);
            SDL_free(data);
            SDL_DestroyProcess(process);
            exitCode = code;
            return !stopped && exitCode == 0;
#endif
        };
        if (!recompile({tool(tools, "ps2_recomp").string(), (work / "config.toml").string()}) && stopped)
            return fail("");
        if (exitCode != 0 || !fs::exists(generated / "register_functions.cpp", ec))
            return fail("Recompiling the game's code failed (" + std::to_string(exitCode) + "):\n" +
                        firstLines(output.substr(output.size() > 4000 ? output.size() - 4000 : 0), 20));

        // 1b. The VU1 microprograms the recompiler found (the micro memory image the game's renderer uploads,
        // <hash>.vu1code beside the generated code), compiled ahead of time by the app's own generator: C++ for the
        // module, run on the runtime's VU1 core (vu1/vu1_core.h). None found: VU1 runs on the interpreter.
        const fs::path vu1 = work / "vu1";
        std::vector<fs::path> vu1Sources;
        for (const auto &item : fs::directory_iterator(generated, ec))
        {
            if (item.path().extension() != ".vu1code")
                continue;
            std::string bytes;
            readFile(item.path(), bytes);
            const std::vector<uint8_t> image(bytes.begin(), bytes.end());
            zx::vu1aot::Written written;
            std::string why;
            if (!zx::vu1aot::writeProgram(image, vu1, written, why))
                return fail("Preparing the game's VU1 code failed (" + item.path().filename().string() + "): " + why);
            for (const fs::path &file : written.files)
            {
                if (file.extension() == ".cpp")
                    vu1Sources.push_back(file);
            }
        }
        std::sort(vu1Sources.begin(), vu1Sources.end());

        // 2. The compiler: units of 32 generated sources, at once on the host's cores, and the module's entry.
        std::vector<fs::path> sources;
        for (const auto &item : fs::directory_iterator(generated, ec))
        {
            if (item.path().extension() == ".cpp" && item.path().filename() != "register_functions.cpp")
                sources.push_back(item.path());
        }
        std::sort(sources.begin(), sources.end());
        std::vector<fs::path> units;
        for (size_t i = 0; i < sources.size(); i += kUnitSources)
        {
            const fs::path unit = work / "unity" / ("unit_" + std::to_string(units.size()) + ".cpp");
            std::ofstream out(unit, std::ios::binary);
            for (size_t k = i; k < std::min(sources.size(), i + kUnitSources); ++k)
                out << "#include \"" << sources[k].generic_string() << "\"\n";
            units.push_back(unit);
        }
        units.push_back(sdk(tools) / "module_entry.cpp");
        // (the function table's filler alone and unoptimized, as cmake/game_module.cmake builds it: only stores, run once;
        // optimized, the compiler folds it into the table and takes over 8 GB for AI's)
        const fs::path tableFiller = generated / "register_functions.cpp";
        units.push_back(tableFiller);
        // (VU1's units: each microprogram source alone, and the core's out-of-line pipelines, with the flags the
        // runtime compiles them with: game_module_flags.txt's vu1 and vu1aot lines)
        const size_t firstVu1Unit = units.size();
        if (!vu1Sources.empty())
            units.push_back(sdk(tools) / "vu" / "vu1_core.cpp");
        for (const fs::path &source : vu1Sources)
            units.push_back(source);
        std::vector<std::string> vu1Flags = {"-I" + (sdk(tools) / "vu").string()};
        for (const std::string &flag : moduleFlags(tools, {"vu1"}))
            vu1Flags.push_back(flag);
        std::vector<std::string> vu1AotFlags = vu1Flags;
        for (const std::string &flag : moduleFlags(tools, {"vu1aot"}))
            vu1AotFlags.push_back(flag);
        std::vector<std::string> common = {tool(tools, "clang++").string()};
        if (!kProgramsNamed)
            common.push_back("--driver-mode=g++");
        if (const fs::path resources = resourceFolder(tools); !kProgramsNamed && !resources.empty())
            common.insert(common.end(), {"-resource-dir", resources.string()});
        common.insert(common.end(), {std::string("--target=") + platform.triple,
                                           "-std=gnu++20", "-O3", "-DNDEBUG", "-DZX_GAME_MODULE_ID=" + request.id,
                                           "-DZX_GAME_MODULE_SHARED=1", "-I" + generated.string(),
                                           "-I" + (sdk(tools) / "include").string(), "-isystem",
                                           (sdk(tools) / "include" / "game_module" / "libc").string(),
                                           "-Wno-return-type"});
        for (const std::string &flag : moduleFlags(tools, {"all", platform.arch}))
            common.push_back(flag);
        if (platform.link == LinkKind::Elf)
            common.push_back("-fPIC");
        // (the host's cores but one, and no more compiles than 1 GB of its memory each: a unit takes up to ~0.4 GB)
        // (ZX_GAME_CODE_JOBS: a count of the developer's, for a check on a machine that runs other work)
        unsigned jobs = request.jobs;
        if (const char *forced = std::getenv("ZX_GAME_CODE_JOBS"); !jobs && forced && std::atoi(forced) > 0)
            jobs = static_cast<unsigned>(std::atoi(forced));
        if (!jobs)
        {
            const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
            const int ramMb = SDL_GetSystemRAM();
            const unsigned byMemory = ramMb > 2048 ? static_cast<unsigned>(ramMb / 1024 - 1) : 1u;
            jobs = std::max(1u, std::min(cores > 1 ? cores - 1 : 1u, byMemory));
        }
        std::atomic<size_t> next{0}, done{0};
        std::atomic<bool> stop{false};
        std::mutex failMutex;
        std::string firstFailure;
        std::vector<fs::path> objects(units.size());
        for (size_t i = 0; i < units.size(); ++i)
            objects[i] = work / "objects" / ("unit_" + std::to_string(i) + ".o");
        auto worker = [&]
        {
            for (size_t i; !stop && (i = next.fetch_add(1)) < units.size();)
            {
                std::vector<std::string> args = common;
                if (units[i] == tableFiller)
                    args.push_back("-O0");
                else if (i == firstVu1Unit)
                    args.insert(args.end(), vu1Flags.begin(), vu1Flags.end());
                else if (i > firstVu1Unit)
                    args.insert(args.end(), vu1AotFlags.begin(), vu1AotFlags.end());
                args.insert(args.end(), {"-c", units[i].string(), "-o", objects[i].string()});
                std::string text;
                int code = 0;
                if (!runProgram(args, text, code))
                {
                    std::lock_guard<std::mutex> lock(failMutex);
                    if (firstFailure.empty())
                        firstFailure = units[i].filename().string() + " (exit " + std::to_string(code) + ")" +
                                       (text.empty() ? std::string(": the compiler stopped with no message")
                                                     : ":\n" + firstLines(text, 30));
                    stop = true;
                }
                ++done;
            }
        };
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < std::min<size_t>(jobs, units.size()); ++t)
            threads.emplace_back(worker);
        while (done < units.size() && !stop)
        {
            if (!report("Compiling", done, units.size()))
                stop = true;
            SDL_Delay(250);
        }
        for (std::thread &t : threads)
            t.join();
        if (!firstFailure.empty())
            return fail("Compiling the game's code failed:\n" + firstFailure);
        if (done < units.size())
            return fail("");

        // 3. The linker: one shared library, no library of any kind (an undefined symbol is a failure here).
        if (!report("Linking", 0, 1))
            return fail("");
        const fs::path rsp = work / "objects.rsp";
        {
            std::ofstream out(rsp, std::ios::binary);
            for (const fs::path &o : objects)
                out << '"' << o.generic_string() << "\"\n";
        }
        const fs::path built = work / (std::string("module") + platform.extension);
        std::vector<std::string> link = {linker(tools).string()};
        if (platform.link != hostTarget().link || !kProgramsNamed)
            link.insert(link.end(), {"-flavor", platform.link == LinkKind::MachO ? "darwin"
                                                : platform.link == LinkKind::Coff ? "link" : "gnu"});
        if (platform.link == LinkKind::MachO)
            link.insert(link.end(), {"-dylib", "-arch", platform.arch, "-platform_version", "macos", "11.0", "11.0",
                                     "-undefined", "error", "-o", built.string(), "@" + rsp.string()});
        else if (platform.link == LinkKind::Coff)
            link.insert(link.end(), {"/DLL", "/NOENTRY", "/NODEFAULTLIB", "/OUT:" + built.string(), "@" + rsp.string()});
        else
            link.insert(link.end(), {"-shared", "-z", "defs", "--eh-frame-hdr", "-o", built.string(), "@" + rsp.string()});
        if (!runProgram(link, output, exitCode))
            return fail("Linking the game's code failed (" + std::to_string(exitCode) + "):\n" + firstLines(output, 30));

        // 4. In place of any other module of this game.
        for (const auto &item : fs::directory_iterator(request.cache, ec))
        {
            const std::string name = item.path().filename().string();
            if (name.rfind(request.id + "-", 0) == 0 && item.path().extension() == platform.extension && item.path() != target)
                fs::remove(item.path(), ec);
        }
        fs::rename(built, target, ec);
        if (ec)
            return fail("Could not keep the game's code in " + request.cache.string() + ": " + ec.message());
        report("Done", 1, 1);
        return true;
    }
    std::filesystem::path gameCodeCache(const std::filesystem::path &gameDir)
    {
#if defined(__ANDROID__)
        // (the external files folder, where the game's folder is, is mounted noexec: Android loads no library there)
        if (const char *internal = SDL_GetAndroidInternalStoragePath())
            return fs::path(internal) / "game-code" / gameDir.filename();
#endif
        return gameDir / "code";
    }

    std::string gameCodeGame(const GameCodeTools &tools, const std::string &id, const std::filesystem::path &elf)
    {
        std::error_code ec;
        if (!id.empty() && fs::exists(gameInputs(tools, id) / "config.toml", ec))
            return id;
        auto lower = [](std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        };
        const std::string want = lower(elf.filename().string());
        for (const auto &entry : fs::directory_iterator(tools.root / "games", ec))
        {
            std::ifstream in(entry.path() / "config.toml");
            for (std::string line; std::getline(in, line);)
            {
                // (input = "<path>/SLUS_203.27": the config's ELF, by its file's name)
                if (line.rfind("input", 0) != 0 || line.find('=') == std::string::npos)
                    continue;
                const size_t open = line.find('"'), close = line.rfind('"');
                if (open != std::string::npos && close > open &&
                    lower(fs::path(line.substr(open + 1, close - open - 1)).filename().string()) == want)
                    return entry.path().filename().string();
            }
        }
        return id;
    }

    namespace
    {
        GameCodeRequest setupRequest(const GameCodeTools &tools, const SetupGame &game, const std::filesystem::path &discFolder,
                                     const std::filesystem::path &gameDir)
        {
            GameCodeRequest request;
            request.id = gameCodeGame(tools, game.id, game.elf);
            request.elf = discFolder / game.elf;
            request.cache = gameCodeCache(gameDir);
            return request;
        }
    }

    bool BuiltGameCode::ready(const SetupGame &game, const std::filesystem::path &discFolder,
                              const std::filesystem::path &gameDir) const
    {
        if (zx::game_module::hasCodeFor(game.elf))
            return true;   // (a program with the game's code linked in: a workshop build)
        GameCodeTools tools;
        std::error_code ec;
        return tools.open(m_tools, nullptr) &&
               std::filesystem::exists(gameCodeFile(tools, setupRequest(tools, game, discFolder, gameDir)), ec);
    }

    bool BuiltGameCode::make(const SetupGame &game, const std::filesystem::path &discFolder,
                             const std::filesystem::path &gameDir, const SetupProgressFn &progress, std::string *error)
    {
        if (zx::game_module::hasCodeFor(game.elf))
            return !progress || progress({name(), "", 1, 1});
        GameCodeTools tools;
        if (!tools.open(m_tools, error))
            return false;
        const GameCodeRequest request = setupRequest(tools, game, discFolder, gameDir);
        std::error_code ec;
        if (std::filesystem::exists(gameCodeFile(tools, request), ec))
            return !progress || progress({name(), "", 1, 1});
        std::filesystem::create_directories(request.cache, ec);
        // (each of the build's steps as the setup shows a step: its name, the build's own step as the file line)
        return makeGameCode(tools, request, [&](const GameCodeProgress &p)
        {
            return !progress || progress({name(), p.step, p.done, p.total});
        }, error);
    }
}
