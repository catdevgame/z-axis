#include "runtime/spot_fork.h"

#include "machine.h"
#include "game_overrides.h"
#include "runtime/play_recording.h"
#include "runtime/play_link.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) || defined(__ANDROID__)
namespace zx::spot
{
    bool enabled() { return false; }
    void setHostLoop(std::function<void()>)
    {
        // (once a run: say why a spot never comes)
        if (const char *dir = std::getenv("ZX_SPOT_FORK"); dir && dir[0])
            std::fprintf(stderr, "[spot] ZX_SPOT_FORK is served on the Mac and Linux only (Windows has no fork)\n");
    }
    void hostLoopTop() {}
    void onVsync(PS2Machine &, uint64_t, int64_t) {}
    void addChildHook(std::function<void(PS2Machine &)>) {}
}
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace zx::spot
{
namespace
{
    namespace fs = std::filesystem;

    std::function<void()> g_hostLoop;
    std::vector<std::function<void(PS2Machine &)>> g_childHooks;
    bool g_done = false;  // the spot was reached (served or refused): never again in this process

    // The host loop's park (hostLoopTop): the game thread asks, the host
    // thread says it is there and stays.
    std::mutex g_parkMutex;
    std::condition_variable g_parkCv;
    bool g_parkWanted = false, g_parked = false;

    const char *env(const char *name)
    {
        const char *v = std::getenv(name);
        return v && v[0] ? v : nullptr;
    }

    std::string dir() { return env("ZX_SPOT_FORK") ? env("ZX_SPOT_FORK") : ""; }

    // ZX_SPOT_AT: "tick>=N" or "script>=N"; none: the file <dir>/now only.
    bool atSpot(uint64_t tick, int64_t scriptTime)
    {
        if (const char *at = env("ZX_SPOT_AT"))
        {
            const std::string s(at);
            const size_t op = s.find(">=");
            if (op != std::string::npos)
            {
                const std::string what = s.substr(0, op);
                const long long n = std::strtoll(s.c_str() + op + 2, nullptr, 0);
                if (what == "tick" && static_cast<long long>(tick) >= n)
                    return true;
                if (what == "script" && scriptTime >= n)
                    return true;
            }
        }
        std::error_code ec;
        return fs::exists(fs::path(dir()) / "now", ec);
    }

    void note(const std::string &text)
    {
        std::fprintf(stderr, "[spot] %s\n", text.c_str());
        std::fflush(stderr);
    }

    void writeFile(const fs::path &path, const std::string &text)
    {
        const fs::path tmp = path.string() + ".tmp";
        {
            std::ofstream out(tmp);
            out << text;
        }
        std::error_code ec;
        fs::rename(tmp, path, ec);
    }

    // Why the machine cannot be copied exactly here, or empty.
    std::string refusal(PS2Machine &runtime)
    {
        if (!g_hostLoop)
            return "no host loop to run again (not PS2Machine::run)";
        if (!env("ZX_GS_NULL") || std::string(env("ZX_GS_NULL")) != "1")
            return "ZX_GS_NULL=1 is needed (a GS renderer's threads and GPU state are not rebuilt)";
        if (!env("ZX_AUDIO") || std::string(env("ZX_AUDIO")) != "off")
            return "ZX_AUDIO=off is needed (the audio device's thread is not rebuilt)";
        if (env("ZX_FNREC"))
            return "the parent records (ZX_FNREC): a child records from the spot, by its request";
        (void)runtime;
        return {};
    }

    // The child: its environment and log, then the threads fork did not copy.
    void becomeChild(PS2Machine &runtime, const std::string &name, const std::map<std::string, std::string> &vars,
                     bool graphics)
    {
        const std::string log = (fs::path(dir()) / (name + ".log")).string();
        for (const auto &[k, v] : vars)
            ::setenv(k.c_str(), v.c_str(), 1);
        ::unsetenv("ZX_SPOT_FORK");
        const int fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0)
        {
            std::fflush(stdout);
            std::fflush(stderr);
            ::dup2(fd, 1);
            ::dup2(fd, 2);
            ::close(fd);
        }
        g_forkEpoch.fetch_add(1u, std::memory_order_acq_rel);
        note("child " + name + " (pid " + std::to_string(::getpid()) + ") goes on from the spot");
        ps2_play_link::relink();
        if (graphics)
            runtime.memory().setAsyncGraphics(true);
        for (const auto &hook : g_childHooks)
            hook(runtime);
        // The host loop: a thread of its own, from its loop's top to the run's
        // end, then the process ends as main() would end it.
        {
            std::lock_guard<std::mutex> lock(g_parkMutex);
            g_parkWanted = false;
            g_parked = false;
        }
        std::thread([&runtime]()
                    {
                        g_hostLoop();
                        ps2_game_overrides::runShutdownHooks(runtime);
                        std::fflush(nullptr);
                        _exit(0);
                    })
            .detach();
    }

    std::map<std::string, std::string> readRequest(const fs::path &path)
    {
        std::map<std::string, std::string> vars;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            const size_t eq = line.find('=');
            if (eq != std::string::npos && eq > 0 && line[0] != '#')
                vars[line.substr(0, eq)] = line.substr(eq + 1);
        }
        return vars;
    }

    // The parent: serves copies of itself until <dir>/stop or an idle limit,
    // then ends. Returns only in a child.
    bool serve(PS2Machine &runtime, uint64_t tick, bool graphics)
    {
        const fs::path root(dir());
        const fs::path requests = root / "req";
        std::error_code ec;
        fs::create_directories(requests, ec);
        const int maxChildren = std::max(1, env("ZX_SPOT_FORK_MAX") ? std::atoi(env("ZX_SPOT_FORK_MAX")) : 2);
        const int idleSeconds = std::max(1, env("ZX_SPOT_FORK_IDLE") ? std::atoi(env("ZX_SPOT_FORK_IDLE")) : 600);
        writeFile(root / "ready", "tick " + std::to_string(tick) + "\npid " + std::to_string(::getpid()) + "\n");
        note("serving at tick " + std::to_string(tick) + " from " + root.string());
        std::map<pid_t, std::string> running;
        auto lastActivity = std::chrono::steady_clock::now();
        for (;;)
        {
            for (auto it = running.begin(); it != running.end();)
            {
                int status = 0;
                if (::waitpid(it->first, &status, WNOHANG) == it->first)
                {
                    writeFile(root / (it->second + ".exit"),
                              std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status)) + "\n");
                    it = running.erase(it);
                    lastActivity = std::chrono::steady_clock::now();
                }
                else
                    ++it;
            }
            if (fs::exists(root / "stop", ec) && running.empty())
                break;
            if (play_recording::stopRequested())
            {
                note("told to end (SIGTERM or SIGINT): the server ends; its children go on");
                break;
            }
            if (running.empty() && std::chrono::steady_clock::now() - lastActivity > std::chrono::seconds(idleSeconds))
            {
                note("no request for " + std::to_string(idleSeconds) + " s: the server ends");
                break;
            }
            std::vector<fs::path> todo;
            if (!fs::exists(root / "stop", ec))
                for (const auto &e : fs::directory_iterator(requests, ec))
                    if (e.is_regular_file() && e.path().extension() != ".tmp")
                        todo.push_back(e.path());
            std::sort(todo.begin(), todo.end());
            for (const fs::path &req : todo)
            {
                if (static_cast<int>(running.size()) >= maxChildren)
                    break;
                const std::string name = req.filename().string();
                const std::map<std::string, std::string> vars = readRequest(req);
                fs::remove(req, ec);
                std::fflush(nullptr);
                const pid_t pid = ::fork();
                if (pid == 0)
                {
                    becomeChild(runtime, name, vars, graphics);
                    return true;
                }
                if (pid < 0)
                {
                    note("fork failed for " + name + ": " + std::strerror(errno));
                    continue;
                }
                running[pid] = name;
                writeFile(root / (name + ".pid"), std::to_string(pid) + "\n");
                lastActivity = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        writeFile(root / "done", "served\n");
        std::fflush(nullptr);
        _exit(0);
    }
}

bool enabled() { return !dir().empty(); }

void setHostLoop(std::function<void()> loop) { g_hostLoop = std::move(loop); }

void addChildHook(std::function<void(PS2Machine &)> hook) { g_childHooks.push_back(std::move(hook)); }

void hostLoopTop()
{
    if (!enabled())
        return;
    std::unique_lock<std::mutex> lock(g_parkMutex);
    if (!g_parkWanted)
        return;
    g_parked = true;
    g_parkCv.notify_all();
    // The parent's host thread stays here: the server ends the process.
    g_parkCv.wait(lock, []() { return false; });
}

void onVsync(PS2Machine &runtime, uint64_t tick, int64_t scriptTime)
{
    if (g_done || !enabled() || !atSpot(tick, scriptTime))
        return;
    g_done = true;
    if (const std::string why = refusal(runtime); !why.empty())
    {
        note("not served: " + why + "; the run goes on");
        writeFile(fs::path(dir()) / "refused", why + "\n");
        return;
    }
    // The quiet point: every posted graphics task done and the workers gone
    // (a child starts its own), the host loop at its loop's top.
    const bool graphics = runtime.memory().asyncGraphics();
    runtime.memory().setAsyncGraphics(false);
    {
        std::unique_lock<std::mutex> lock(g_parkMutex);
        g_parkWanted = true;
        if (!g_parkCv.wait_for(lock, std::chrono::seconds(30), []() { return g_parked; }))
        {
            g_parkWanted = false;
            lock.unlock();
            note("not served: the host loop did not park within 30 s; the run goes on");
            if (graphics)
                runtime.memory().setAsyncGraphics(true);
            return;
        }
    }
    serve(runtime, tick, graphics);
}
}
#endif
