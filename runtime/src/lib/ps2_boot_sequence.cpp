#include "runtime/ps2_boot_sequence.h"

#include "game_overrides.h"
#include "ps2_host_backend.h"
#include "ps2_runtime.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace zx::sequence
{
    namespace
    {
        void fail(std::string *error, const std::string &text)
        {
            if (error)
                *error = text;
        }

        bool parseCount(const std::string &text, uint64_t &out)
        {
            if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
                return false;
            try
            {
                out = std::stoull(text);
            }
            catch (const std::exception &)
            {
                return false;
            }
            return true;
        }

        // Fewer VBlanks than this (a second of game time) in a stall window:
        // the game has hung (it waits on something that never comes; a few
        // VBlanks still trickle in).
        constexpr uint64_t kStallVBlanks = 60;

        double residentMb()
        {
#if defined(__APPLE__)
            mach_task_basic_info_data_t info{};
            mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
            if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
                return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
#elif defined(__linux__)
            std::ifstream statm("/proc/self/statm");
            unsigned long long size = 0, resident = 0;
            if (statm >> size >> resident)
                return static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
#endif
            return 0.0;
        }
    }

    bool parseStep(const std::string &game, const std::string &text, Step &step, std::string *error)
    {
        Step parsed;
        parsed.game = game;
        parsed.config.game = game;
        std::istringstream lines(text);
        std::string line;
        int number = 0;
        while (std::getline(lines, line))
        {
            ++number;
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            const size_t equals = line.find('=');
            if (equals == std::string::npos || equals == 0)
            {
                fail(error, "line " + std::to_string(number) + ": not NAME=VALUE");
                return false;
            }
            const std::string name = line.substr(0, equals);
            const std::string value = line.substr(equals + 1);
            if (name == "elf")
                parsed.elf = value;
            else if (name == "ticks")
            {
                if (!parseCount(value, parsed.tickLimit) || parsed.tickLimit == 0)
                {
                    fail(error, "line " + std::to_string(number) + ": ticks is not a count");
                    return false;
                }
            }
            else if (name == "seconds")
            {
                uint64_t seconds = 0;
                if (!parseCount(value, seconds))
                {
                    fail(error, "line " + std::to_string(number) + ": seconds is not a count");
                    return false;
                }
                parsed.secondsLimit = static_cast<double>(seconds);
            }
            else if (name == "stall_seconds")
            {
                uint64_t seconds = 0;
                if (!parseCount(value, seconds))
                {
                    fail(error, "line " + std::to_string(number) + ": stall_seconds is not a count");
                    return false;
                }
                parsed.stallSeconds = static_cast<double>(seconds);
            }
            else if (name == "stop_after")
                parsed.stopAfter = value;
            else if (name == "mc_root")
                parsed.mcRoot = value;
            else if (name == "cwd")
                parsed.workingDirectory = value;
            else if (!parsed.config.set(name, value))
            {
                fail(error, "line " + std::to_string(number) + ": " + name + " is not a step setting or a name BootConfig carries");
                return false;
            }
        }
        if (parsed.elf.empty())
        {
            fail(error, "no elf");
            return false;
        }
        if (parsed.tickLimit == 0)
        {
            fail(error, "no tick limit (ticks=N)");
            return false;
        }
        step = std::move(parsed);
        return true;
    }

    bool parseArgument(const std::string &argument, std::vector<Step> &steps, std::string *error)
    {
        std::vector<Step> parsed;
        std::istringstream items(argument);
        std::string item;
        while (std::getline(items, item, ','))
        {
            const size_t colon = item.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 == item.size())
            {
                fail(error, "'" + item + "': not game:FILE");
                return false;
            }
            const std::string game = item.substr(0, colon);
            const std::string path = item.substr(colon + 1);
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                fail(error, path + ": cannot be read");
                return false;
            }
            std::stringstream text;
            text << file.rdbuf();
            Step step;
            std::string why;
            if (!parseStep(game, text.str(), step, &why))
            {
                fail(error, path + ": " + why);
                return false;
            }
            parsed.push_back(std::move(step));
        }
        if (parsed.empty())
        {
            fail(error, "no step");
            return false;
        }
        steps = std::move(parsed);
        return true;
    }

    std::vector<Outcome> run(const std::vector<Step> &steps, const char *title,
                             const std::function<void(PS2Runtime &)> &afterLoad)
    {
        std::vector<Outcome> outcomes;
        // What a process of its own would start each game with.
        const PS2Runtime::IoPaths initialPaths = PS2Runtime::getIoPaths();
        std::error_code ec;
        const std::filesystem::path initialDirectory = std::filesystem::current_path(ec);

        for (size_t i = 0; i < steps.size(); ++i)
        {
            const Step &step = steps[i];
            Outcome outcome;
            std::fprintf(stderr, "[seq] step %zu of %zu: %s begin (%s)\n", i + 1, steps.size(), step.game.c_str(),
                         step.elf.string().c_str());
            PS2Runtime::setIoPaths(initialPaths);
            std::filesystem::current_path(step.workingDirectory.empty() ? initialDirectory : step.workingDirectory, ec);
            if (ec)
                std::fprintf(stderr, "[seq] step %zu: working directory %s: %s\n", i + 1,
                             step.workingDirectory.string().c_str(), ec.message().c_str());

            const auto start = std::chrono::steady_clock::now();
            std::atomic<uint64_t> tick{0};
            std::string stop;
            {
                PS2Runtime runtime(step.config);
                PS2Runtime::HostOptions options;
                options.hostOwned = true;
                runtime.setHostOptions(options);
                if (!runtime.initialize(title) || !runtime.loadELF(step.elf.string()))
                {
                    std::fprintf(stderr, "[seq] step %zu: %s did not boot\n", i + 1, step.game.c_str());
                    outcomes.push_back(outcome);
                    break;
                }
                outcome.booted = true;
                if (!step.mcRoot.empty())
                {
                    PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
                    paths.mcRoot = step.mcRoot;
                    PS2Runtime::setIoPaths(paths);
                }
                if (afterLoad)
                    afterLoad(runtime);

                runtime.addVSyncHook([&tick](uint8_t *, uint64_t now) { tick.store(now, std::memory_order_relaxed); });
                // The stall window: where the VBlank count stood when it began.
                uint64_t windowTick = 0;
                auto windowAt = std::chrono::steady_clock::now();
                runtime.setHostFrameCallback(
                    [&](PS2Runtime &rt)
                    {
                        if (!stop.empty())
                            return;
                        const auto now = std::chrono::steady_clock::now();
                        const uint64_t ticks = tick.load(std::memory_order_relaxed);
                        bool stalled = false;
                        if (step.stallSeconds > 0.0 && std::chrono::duration<double>(now - windowAt).count() >= step.stallSeconds)
                        {
                            stalled = ticks - windowTick < kStallVBlanks;
                            windowTick = ticks;
                            windowAt = now;
                        }
                        std::error_code exists;
                        if (!step.stopAfter.empty() && std::filesystem::exists(step.stopAfter, exists))
                            stop = "file";
                        else if (ticks >= step.tickLimit)
                            stop = "ticks";
                        else if (step.secondsLimit > 0.0 && std::chrono::duration<double>(now - start).count() >= step.secondsLimit)
                            stop = "seconds";
                        else if (stalled)
                            stop = "stalled";
                        if (!stop.empty())
                            rt.requestStop();
                    });
                runtime.run();
                // Leaving the game as exit to the menu does: its shutdown
                // hooks with its memory still there, then the runtime.
                ps2_game_overrides::runShutdownHooks(runtime);
            }
            outcome.stop = stop.empty() ? "exit" : stop;
            outcome.ticks = tick.load(std::memory_order_relaxed);
            outcome.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            outcome.residentMb = residentMb();
            std::fprintf(stderr, "[seq] step %zu of %zu: %s end: stop %s at tick %llu after %.1f s, resident %.0f MB\n", i + 1,
                         steps.size(), step.game.c_str(), outcome.stop.c_str(), static_cast<unsigned long long>(outcome.ticks),
                         outcome.seconds, outcome.residentMb);
            outcomes.push_back(outcome);
            if (ps2_host::quitRequested())
                break;
        }

        PS2Runtime::setIoPaths(initialPaths);
        std::filesystem::current_path(initialDirectory, ec);
        if (ps2_host::initialized())
            ps2_host::shutdown();
        return outcomes;
    }
}
