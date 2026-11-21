#include "runtime/ps2_debug_control.h"
#include "runtime/ps2_frame_trace.h"
#include "runtime/ps2_memory.h"

#include "runtime/ps2_pad_script.h"
#include "runtime/ps2_state_probe.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_boot_config.h"

void setPadPort0Connected(bool connected); // ps2_pad.cpp

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

namespace ps2_debug_control
{
namespace
{
    constexpr uint32_t kRamMask = PS2_RAM_MASK; // extended memory too

    struct State
    {
        std::string dir;
        long readOffset = 0;
        std::string partial;
        uint64_t nextId = 1;
        struct Command
        {
            uint64_t id = 0;
            std::vector<std::string> words;
        };
        std::deque<Command> queue;
        // Active command.
        bool active = false;
        Command current;
        int phase = 0;
        uint64_t phaseEnd = 0;
        std::vector<StateProbe::Condition> conditions;

        std::atomic<uint16_t> heldButtons{0};    // down/up
        std::atomic<uint16_t> commandButtons{0}; // press/hold
        std::atomic<uint32_t> stickBytes{0};     // RX RY LX LY
        std::atomic<unsigned> stickMask{0};
        uint64_t stickEnd[4]{};

        std::mutex shotMutex;
        std::string pendingShot;
        std::string pendingRenderer;
        std::string pendingGsDump;
        uint32_t pendingGsDumpFrames = 2;
        bool shotDone = false;
        bool shotOk = false;
    };

    State &state()
    {
        static State s;
        return s;
    }

    std::string &controlDirStorage()
    {
        static std::string dir = []()
        {
            const char *value = std::getenv("ZX_CONTROL");
            return std::string(value ? value : "");
        }();
        return dir;
    }

    const std::string &controlDir()
    {
        return controlDirStorage();
    }

    void reply(const State &s, uint64_t id, const std::string &text)
    {
        if (FILE *out = std::fopen((s.dir + "/out").c_str(), "a"))
        {
            std::fprintf(out, "%llu %s\n", static_cast<unsigned long long>(id), text.c_str());
            std::fclose(out);
        }
    }

    void readCommands(State &s)
    {
        FILE *in = std::fopen((s.dir + "/cmd").c_str(), "rb");
        if (!in)
            return;
        if (std::fseek(in, 0, SEEK_END) == 0)
        {
            const long size = std::ftell(in);
            if (size < s.readOffset)
                s.readOffset = 0; // truncated: start over
            if (size > s.readOffset)
            {
                std::fseek(in, s.readOffset, SEEK_SET);
                std::string chunk(static_cast<size_t>(size - s.readOffset), '\0');
                const size_t got = std::fread(chunk.data(), 1, chunk.size(), in);
                chunk.resize(got);
                s.readOffset += static_cast<long>(got);
                s.partial += chunk;
                size_t nl;
                while ((nl = s.partial.find('\n')) != std::string::npos)
                {
                    std::string line = s.partial.substr(0, nl);
                    s.partial.erase(0, nl + 1);
                    State::Command command;
                    command.id = s.nextId++;
                    std::istringstream words(line);
                    std::string word;
                    while (words >> word)
                        command.words.push_back(word);
                    if (command.words.empty() || command.words[0][0] == '#')
                    {
                        reply(s, command.id, "skip");
                        continue;
                    }
                    s.queue.push_back(std::move(command));
                }
            }
        }
        std::fclose(in);
    }

    uint32_t parseU32(const std::string &text)
    {
        return static_cast<uint32_t>(std::strtoull(text.c_str(), nullptr, 0));
    }

    // ZX_POKE_TRACE=<file>: a line "<vsync tick> <address> <value>" (hex) per
    // poke of a client, for tooling/checks/career_ab.py script --pokes; ZX_POKE_SCRIPT
    // ("<tick>:<address>=<value>,...", hex, in tick order) makes the same
    // writes at the same vsyncs, so a route that pokes (a teleport to a giver,
    // a saved key) replays tick by tick.
    void tracePoke(uint64_t tick, uint32_t address, uint32_t value)
    {
        static std::FILE *const file = []() -> std::FILE *
        {
            const char *path = std::getenv("ZX_POKE_TRACE");
            return path && path[0] ? std::fopen(path, "w") : nullptr;
        }();
        if (file)
        {
            std::fprintf(file, "%llx %x %x\n", static_cast<unsigned long long>(tick), address, value);
            std::fflush(file);
        }
    }

    struct ScriptedPoke
    {
        uint64_t tick;
        uint32_t address, value;
    };

    void applyPokeScript(uint8_t *rdram, uint64_t tick)
    {
        // Each boot its own script, from its start.
        struct Script
        {
            std::vector<ScriptedPoke> pokes;
            size_t next = 0;
        };
        static zx::boot::PerBoot<Script> script([]()
        {
            Script out;
            const char *text = zx::boot::text("ZX_POKE_SCRIPT");
            std::istringstream items(text ? text : "");
            std::string item;
            while (std::getline(items, item, ','))
            {
                unsigned long long at = 0;
                unsigned address = 0, value = 0;
                if (std::sscanf(item.c_str(), "%llx:%x=%x", &at, &address, &value) == 3)
                    out.pokes.push_back({at, address, value});
            }
            return out;
        });
        const std::vector<ScriptedPoke> &pokes = script.get().pokes;
        size_t &next = script.get().next;
        while (next < pokes.size() && pokes[next].tick <= tick)
        {
            std::memcpy(rdram + (pokes[next].address & kRamMask & ~3u), &pokes[next].value, 4);
            ++next;
        }
    }

    uint32_t readU32(const uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + (address & kRamMask & ~3u), 4);
        return value;
    }

    int axisIndex(const std::string &name)
    {
        if (name == "RX")
            return 0;
        if (name == "RY")
            return 1;
        if (name == "LX")
            return 2;
        if (name == "LY")
            return 3;
        return -1;
    }

    void setStick(State &s, int axis, uint8_t value, bool active)
    {
        uint32_t bytes = s.stickBytes.load();
        bytes = (bytes & ~(0xFFu << (axis * 8))) | (static_cast<uint32_t>(value) << (axis * 8));
        s.stickBytes.store(bytes);
        unsigned mask = s.stickMask.load();
        mask = active ? (mask | (1u << axis)) : (mask & ~(1u << axis));
        s.stickMask.store(mask);
    }

    // Starts or continues the active command; returns true when it is done.
    bool step(State &s, uint8_t *rdram, uint64_t tick)
    {
        State::Command &c = s.current;
        const std::string &op = c.words[0];
        auto arg = [&](size_t i, const std::string &fallback = "") -> std::string
        { return i < c.words.size() ? c.words[i] : fallback; };

        if (op == "press" || op == "hold")
        {
            const uint16_t mask = parseButtons(arg(1));
            const uint64_t holdFor = op == "hold" ? std::strtoull(arg(2, "6").c_str(), nullptr, 0)
                                                  : std::strtoull(arg(2, "6").c_str(), nullptr, 0);
            const uint64_t gap = op == "hold" ? 0 : std::strtoull(arg(3, "6").c_str(), nullptr, 0);
            if (s.phase == 0)
            {
                if (mask == 0)
                {
                    reply(s, c.id, "error bad buttons " + arg(1));
                    return true;
                }
                s.commandButtons.store(mask);
                s.phase = 1;
                s.phaseEnd = tick + holdFor;
                return false;
            }
            if (s.phase == 1)
            {
                if (tick < s.phaseEnd)
                    return false;
                s.commandButtons.store(0);
                s.phase = 2;
                s.phaseEnd = tick + gap;
            }
            if (tick < s.phaseEnd)
                return false;
            reply(s, c.id, "ok");
            return true;
        }
        if (op == "down" || op == "up")
        {
            const uint16_t mask = parseButtons(arg(1));
            uint16_t held = s.heldButtons.load();
            held = op == "down" ? static_cast<uint16_t>(held | mask) : static_cast<uint16_t>(held & ~mask);
            s.heldButtons.store(held);
            reply(s, c.id, "ok");
            return true;
        }
        if (op == "stick")
        {
            const int axis = axisIndex(arg(1));
            if (axis < 0)
            {
                reply(s, c.id, "error bad axis");
                return true;
            }
            const double v = std::max(-1.0, std::min(1.0, std::atof(arg(2, "0").c_str())));
            const uint64_t vsyncs = std::strtoull(arg(3, "0").c_str(), nullptr, 0);
            const uint8_t byte = static_cast<uint8_t>(std::lround(PadScript::kStickCenter + v * (v < 0 ? 128.0 : 127.0)));
            const bool centre = v == 0.0;
            setStick(s, axis, byte, !centre);
            s.stickEnd[axis] = (centre || vsyncs == 0) ? 0 : tick + vsyncs;
            reply(s, c.id, "ok");
            return true;
        }
        if (op == "padport")
        {
            setPadPort0Connected(arg(1, "1") != "0");
            reply(s, c.id, "ok");
            return true;
        }
        if (op == "wait")
        {
            if (s.phase == 0)
            {
                s.phase = 1;
                s.phaseEnd = tick + std::strtoull(arg(1, "1").c_str(), nullptr, 0);
            }
            if (tick < s.phaseEnd)
                return false;
            reply(s, c.id, "ok");
            return true;
        }
        if (op == "until")
        {
            if (s.phase == 0)
            {
                s.conditions.clear();
                const std::string spec = arg(1);
                size_t pos = 0;
                while (pos <= spec.size())
                {
                    size_t amp = spec.find('&', pos);
                    if (amp == std::string::npos)
                        amp = spec.size();
                    StateProbe::Condition condition;
                    if (!StateProbe::parseCondition(spec.substr(pos, amp - pos), condition))
                    {
                        reply(s, c.id, "error bad condition " + spec);
                        return true;
                    }
                    s.conditions.push_back(condition);
                    pos = amp + 1;
                }
                s.phase = 1;
                s.phaseEnd = tick + std::strtoull(arg(2, "3600").c_str(), nullptr, 0);
            }
            bool all = true;
            for (const auto &condition : s.conditions)
                all = all && StateProbe::evaluate(condition, rdram, tick);
            if (all)
            {
                reply(s, c.id, "ok tick=" + std::to_string(tick));
                return true;
            }
            if (tick >= s.phaseEnd)
            {
                reply(s, c.id, "timeout");
                return true;
            }
            return false;
        }
        if (op == "peek" || op == "peekf")
        {
            const uint32_t address = parseU32(arg(1));
            const uint32_t count = std::max<uint32_t>(1u, std::min<uint32_t>(4096u, parseU32(arg(2, "1"))));
            std::ostringstream text;
            text << "ok";
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t value = readU32(rdram, address + 4u * i);
                char buffer[32];
                if (op == "peek")
                    std::snprintf(buffer, sizeof(buffer), " %08x", value);
                else
                {
                    float f;
                    std::memcpy(&f, &value, 4);
                    std::snprintf(buffer, sizeof(buffer), " %g", static_cast<double>(f));
                }
                text << buffer;
            }
            reply(s, c.id, text.str());
            return true;
        }
        if (op == "poke" || op == "or")
        {
            const uint32_t address = parseU32(arg(1)) & kRamMask & ~3u;
            uint32_t value = parseU32(arg(2));
            if (op == "or")
                value |= readU32(rdram, address);
            std::memcpy(rdram + address, &value, 4);
            tracePoke(tick, address, value);
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "ok %08x", value);
            reply(s, c.id, buffer);
            return true;
        }
        if (op == "shot")
        {
            std::lock_guard<std::mutex> lock(s.shotMutex);
            if (s.phase == 0)
            {
                s.pendingShot = arg(1);
                s.shotDone = false;
                s.phase = 1;
                return false;
            }
            if (!s.shotDone)
                return false;
            reply(s, c.id, s.shotOk ? "ok" : "error shot failed");
            return true;
        }
        if (op == "renderer")
        {
            const std::string kind = arg(1);
            if (kind != "cpu" && kind != "gpu")
            {
                reply(s, c.id, "error renderer cpu|gpu [scale]");
                return true;
            }
            std::lock_guard<std::mutex> lock(s.shotMutex);
            s.pendingRenderer = kind + " " + (arg(2).empty() ? std::string("1") : arg(2));
            reply(s, c.id, "ok");
            return true;
        }
        if (op == "gsdump")
        {
            std::lock_guard<std::mutex> lock(s.shotMutex);
            s.pendingGsDump = arg(1);
            s.pendingGsDumpFrames = static_cast<uint32_t>(std::strtoul(arg(2, "2").c_str(), nullptr, 0));
            reply(s, c.id, "ok");
            return true;
        }
        if (op == "prompts")
        {
            const bool ok = PSPadBackend::forcePromptDevice(arg(1));
            reply(s, c.id, ok ? "ok" : "error prompts keyboard|playstation|xbox|nintendo|auto");
            return true;
        }
        if (op == "set" || op == "get")
        {
            // A setting of the settings model (runtime/ps2_settings.h).
            const std::string key = arg(1);
            if (!zx::settings::has(key))
            {
                reply(s, c.id, "error unknown setting " + key);
                return true;
            }
            // An action row (no value) runs as if chosen.
            zx::settings::Setting described;
            const bool action = zx::settings::describe(key, described) && described.kind == zx::settings::Kind::Action;
            if (op == "set" && action)
            {
                zx::settings::activate(key);
                reply(s, c.id, "ok");
                return true;
            }
            if (op == "set" && !zx::settings::set(key, static_cast<int>(std::strtol(arg(2).c_str(), nullptr, 0))))
            {
                reply(s, c.id, "error set " + key);
                return true;
            }
            reply(s, c.id, "ok " + std::to_string(zx::settings::value(key)));
            return true;
        }
        if (op == "settings")
        {
            // As the app menu's Settings... (Cmd+,), on the window thread.
            const bool ok = zx::settings::requestOpen();
            reply(s, c.id, ok ? "ok" : "error no settings screen");
            return true;
        }
        if (op == "dump")
        {
            bool ok = false;
            if (FILE *out = std::fopen(arg(1).c_str(), "wb"))
            {
                ok = std::fwrite(rdram, 1, 32u * 1024u * 1024u, out) == 32u * 1024u * 1024u;
                std::fclose(out);
            }
            reply(s, c.id, ok ? "ok" : "error dump failed");
            return true;
        }
        if (op == "ftrace")
        {
            // Frame timeline trace: "ftrace start", "ftrace stop FILE".
            bool ok = false;
            if (arg(1) == "start")
                ok = zx::ftrace::start();
            else if (arg(1) == "stop")
                ok = zx::ftrace::stop(arg(2));
            reply(s, c.id, ok ? "ok" : "error ftrace");
            return true;
        }
        if (op == "echo")
        {
            std::string text = "ok";
            for (size_t i = 1; i < c.words.size(); ++i)
                text += " " + c.words[i];
            reply(s, c.id, text + " tick=" + std::to_string(tick));
            return true;
        }
        reply(s, c.id, "error unknown command " + op);
        return true;
    }
}

void setDirectory(const std::string &dir)
{
    State &s = state();
    s.dir.clear();
    s.readOffset = 0;
    s.partial.clear();
    s.nextId = 1;
    s.queue.clear();
    s.active = false;
    s.heldButtons.store(0);
    s.commandButtons.store(0);
    s.stickMask.store(0);
    controlDirStorage() = dir;
}

bool enabled()
{
    return !controlDir().empty();
}

uint16_t parseButtons(const std::string &spec)
{
    uint16_t mask = 0;
    size_t pos = 0;
    while (pos <= spec.size())
    {
        size_t plus = spec.find('+', pos);
        if (plus == std::string::npos)
            plus = spec.size();
        const uint16_t one = PadScript::buttonMask(spec.substr(pos, plus - pos));
        if (one == 0)
            return 0;
        mask = static_cast<uint16_t>(mask | one);
        pos = plus + 1;
    }
    return mask;
}

void onVsync(uint8_t *rdram, uint64_t tick)
{
    if (rdram)
        applyPokeScript(rdram, tick);
    if (!enabled() || !rdram)
        return;
    State &s = state();
    if (s.dir.empty())
    {
        s.dir = controlDir();
        std::fprintf(stderr, "[control] reading commands from %s/cmd\n", s.dir.c_str());
    }
    readCommands(s);
    for (int axis = 0; axis < 4; ++axis)
        if (s.stickEnd[axis] != 0 && tick >= s.stickEnd[axis])
        {
            s.stickEnd[axis] = 0;
            setStick(s, axis, PadScript::kStickCenter, false);
        }
    // Run instantaneous commands back to back; stop at one that waits.
    for (int guard = 0; guard < 256; ++guard)
    {
        if (!s.active)
        {
            if (s.queue.empty())
                return;
            s.current = std::move(s.queue.front());
            s.queue.pop_front();
            s.active = true;
            s.phase = 0;
        }
        if (!step(s, rdram, tick))
            return;
        s.active = false;
    }
}

uint16_t buttons()
{
    if (!enabled())
        return 0;
    State &s = state();
    return static_cast<uint16_t>(s.heldButtons.load() | s.commandButtons.load());
}

unsigned sticks(uint8_t *out)
{
    if (!enabled())
        return 0;
    State &s = state();
    const unsigned mask = s.stickMask.load();
    const uint32_t bytes = s.stickBytes.load();
    for (int axis = 0; axis < 4; ++axis)
        if (mask & (1u << axis))
            out[axis] = static_cast<uint8_t>(bytes >> (axis * 8));
    return mask;
}

std::string takePendingShot()
{
    if (!enabled())
        return {};
    State &s = state();
    std::lock_guard<std::mutex> lock(s.shotMutex);
    std::string path;
    path.swap(s.pendingShot);
    return path;
}

std::string takePendingRenderer()
{
    if (!enabled())
        return {};
    State &s = state();
    std::lock_guard<std::mutex> lock(s.shotMutex);
    std::string spec;
    spec.swap(s.pendingRenderer);
    return spec;
}

std::string takePendingGsDump(uint32_t &frames)
{
    if (!enabled())
        return {};
    State &s = state();
    std::lock_guard<std::mutex> lock(s.shotMutex);
    std::string path;
    path.swap(s.pendingGsDump);
    frames = s.pendingGsDumpFrames;
    return path;
}

void shotDone(const std::string &path, bool ok)
{
    State &s = state();
    std::lock_guard<std::mutex> lock(s.shotMutex);
    s.shotDone = true;
    s.shotOk = ok;
    std::fprintf(stderr, "[control] shot %s%s\n", path.c_str(), ok ? "" : " (failed)");
}
}
