#include "runtime/ps2_pad.h"
#include "runtime/ps2_debug_control.h"
#include "runtime/ps2_play_link.h"
#include "runtime/play_recording.h"
#include "runtime/ps2_pad_script.h"
#include "runtime/ps2_host_input.h"
#include "ps2_host_backend.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_boot_config.h"
#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;

    // ZX_PAD_SCRIPT: scripted port-0 input for automated runs, see
    // runtime/ps2_pad_script.h. ZX_PAD_SCRIPT_CLOCK=vsync counts script
    // time in emulated vsyncs (at 59.94 Hz) from the first pad read, so
    // scripts follow game time even when the runtime runs slower than real
    // time; the default is wall time. Each boot its own (its BootConfig).
    PadScript &padScript()
    {
        static zx::boot::PerBoot<PadScript> script([]()
        {
            PadScript parsed;
            if (const char *value = zx::boot::text("ZX_PAD_SCRIPT"))
            {
                std::vector<std::string> warnings;
                std::string spec = value;
                if (!spec.empty() && spec[0] == '@') // ZX_PAD_SCRIPT=@file: the script's text from a file
                {
                    std::ifstream file(spec.substr(1));
                    std::stringstream text;
                    text << file.rdbuf();
                    spec = text.str();
                    while (!spec.empty() && (spec.back() == '\n' || spec.back() == '\r'))
                        spec.pop_back();
                }
                parsed.parse(spec, &warnings);
                for (const std::string &item : warnings)
                    std::fprintf(stderr, "[pad] ZX_PAD_SCRIPT: ignoring unknown entry '%s'\n", item.c_str());
            }
            return parsed;
        });
        return script.get();
    }

    // ZX_HOST_INPUT=0: the host's keyboard, mouse and gamepads give the
    // game nothing (scripted check runs: a key or a click reaching the
    // runner's window as it opened came in at the game's first pad reads and
    // armed the boot screen's skip, so a career A/B started differing at tick
    // 190 in a few runs of each runner; drafter25).
    bool hostInputOff()
    {
        static zx::boot::PerBoot<bool> off([]()
        {
            const char *value = zx::boot::text("ZX_HOST_INPUT");
            return value && value[0] == '0';
        });
        return off.get();
    }

    bool scriptUsesVsyncClock()
    {
        static zx::boot::PerBoot<bool> vsync([]()
        {
            const char *value = zx::boot::text("ZX_PAD_SCRIPT_CLOCK");
            return value && std::strcmp(value, "vsync") == 0;
        });
        return vsync.get();
    }

    // Scripted pad input for one read: buttons to press and the stick bytes
    // (RX, RY, LX, LY) the script holds.
    struct ScriptFrame
    {
        uint16_t buttons = 0u;
        uint8_t sticks[4]{};
        bool axisActive[4]{};
    };

    // Advances ZX_PAD_SCRIPT on port 0 (other ports read the same script
    // moment) and evaluates SYNC anchors.
    ScriptFrame applyScript(int port, const std::atomic<uint64_t> *vsyncTick, const uint8_t *rdram)
    {
        ScriptFrame frame;
        PadScript &script = padScript();
        if (script.empty())
            return frame;
        // update()/current() only write the active axes: run them over two
        // different fills to tell which axes the script holds.
        uint8_t low[4] = {0x00, 0x00, 0x00, 0x00};
        uint8_t high[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        if (port != 0)
        {
            frame.buttons = script.current(low);
        }
        else
        {
            const uint64_t tick = vsyncTick ? vsyncTick->load(std::memory_order_acquire) : 0u;
            double clock = 0.0;
            if (scriptUsesVsyncClock() && vsyncTick)
            {
                clock = static_cast<double>(tick);
            }
            else
            {
                static zx::boot::PerBoot<std::chrono::steady_clock::time_point> origin(
                    []() { return std::chrono::steady_clock::now(); });
                clock = std::chrono::duration<double>(std::chrono::steady_clock::now() - origin.get()).count() *
                        PadScript::kVsyncsPerSecond;
            }
            static zx::boot::PerBoot<bool> reportedOrigin([]() { return false; });
            if (!reportedOrigin.get())
            {
                reportedOrigin.get() = true;
                std::fprintf(stderr, "[pad] script origin at vsync tick %llu\n", static_cast<unsigned long long>(tick));
            }
            PadScript::SyncEvent event{};
            event.index = static_cast<size_t>(-1);
            frame.buttons = script.update(clock, tick, rdram, low, &event);
            StateProbe::publishScriptTime(static_cast<int64_t>(std::floor(script.scriptTime() + 1e-6)));
            if (event.index != static_cast<size_t>(-1))
                std::fprintf(stderr, "[pad] sync %zu at script %.0fv (tick %llu): clock %+.0f vsyncs\n",
                             event.index, event.scriptTime, static_cast<unsigned long long>(tick), event.shift);
        }
        script.current(high);
        for (int i = 0; i < 4; ++i)
        {
            frame.axisActive[i] = low[i] == high[i];
            frame.sticks[i] = low[i];
        }
        return frame;
    }

    // ---- host keyboard / mouse / gamepad (runtime/ps2_host_input.h) --------

    namespace hi = ps2_host_input;

    struct HostInputHub
    {
        std::mutex mutex;
        std::string appName = "Z-Axis Collection";
        // The game's controls: its defaults and row names (setGame).
        std::atomic<const hi::GameControls *> controls{&hi::gameControls("ai")};
        bool configured = false;
        uint64_t configuredBoot = 0; // the boot whose controls.ini and input script these are
        hi::HostInputConfig config;

        // Filled by pollHost() on the main thread, consumed by the port-0
        // pad read (under mutex).
        hi::HostCodeSet heldNow;
        hi::HostCodeSet heldSince; // held at any poll since the last read (taps)
        hi::HostCodeSet pressedSince;
        float mouseDx = 0.0f;
        float mouseDy = 0.0f;
        bool focused = true;
        hi::GamepadState gamepad;
        // Player 2's own gamepad (slot 1, for port 1; not present: port 1
        // is empty, or mirrors port 0 with ZX_PAD_PORT1=mirror) and how
        // many gamepads are connected.
        hi::GamepadState gamepad2;
        int gamepadCount = 0;
        // Settings > Controls > Gamepads: the pads in player order (window
        // thread writes, any thread reads; under mutex).
        std::string padSummary = "None";

        // Game thread (port-0 reads).
        hi::HostInputMapper mapper;
        hi::PadReport lastReport;
        bool haveLastTick = false;
        double lastClock = 0.0;
        double scriptOrigin = -1.0;
        double lastScriptTime = -1.0;
        std::atomic<bool> wantCapture{false};

        // Main thread.
        hi::HostCodeSet prevHeld;
        bool osCaptured = false;
        int skipMouseFrames = 0;
        // Connected gamepads as last seen (ps2_host::gamepadGeneration).
        uint32_t padGeneration = 0;
        bool padsSeen = false;
        bool padsEnabledSeen = true;
        std::vector<ps2_host::GamepadInfo> pads;
        bool mappingsLoaded = false;

        hi::HostInputScript script;
        bool scripted = false;
        bool trace = false;

        // Settings pages (main thread).
        std::string path; // controls.ini
        std::vector<std::pair<std::string, std::string>> labels;
        bool settingsRegistered = false;
        std::atomic<int> captureTarget{-1}; // see CaptureTarget; -1: not capturing
        uint16_t capturePadBefore = 0;
        bool captureFirstFrame = false;
        bool holdUntilReleased = false;
        int pressedButton = -1; // pressButton (under mutex)
        int pressedReads = 0;

        // Prompts: the device last used (InputDevice, -1 before the first
        // poll) and the gamepad buttons held at the previous poll.
        std::atomic<int> device{-1};
        std::atomic<uint32_t> deviceChanges{0};
        // Per connected pad (slot order): buttons at the previous poll; and
        // the pad (SDL id) whose style the prompts show, 0 for none.
        std::vector<uint16_t> lastPadButtons;
        uint32_t promptPad = 0;
        // Menus (setMenuMouse): wheel steps since the last read and the
        // buttons they and clicks press, each for a few reads (game thread).
        std::atomic<bool> menuMouse{false};
        float wheelSince = 0.0f;
        std::vector<int> menuPulses;
        int menuPulseReads = 0;
        // setWheelStick: reads left of a wheel push of the right stick (up
        // positive, down negative; game thread).
        std::atomic<bool> wheelStick{false};
        int wheelStickReads = 0;
        // When each host code last went down (main thread writes, any reads).
        std::array<std::atomic<int64_t>, hi::kHostCodeCount> downAt{};
        // [hotkeys]: presses so far (main thread counts, any thread reads),
        // and the gamepad buttons held at the previous poll (chords).
        std::array<std::atomic<uint32_t>, hi::kHotkeyCount> hotkeyPresses{};
        uint16_t hotkeyPadBefore = 0;
        // setFreeInput: the host input since the last takeFreeInput (game
        // thread fills it at each port-0 read, under mutex), and a request
        // for pollHost to hold what is still held when it ends.
        std::atomic<bool> freeInput{false};
        PSPadBackend::FreeInput freeSince;
        uint16_t freePadBefore = 0;
        std::atomic<bool> holdAfterFree{false};
    };

    // What a capture binds: controls.ini [buttons] 0-15, [sticks] 16-23,
    // the stick modifier 24, [mouse] recentre 25, [hotkeys] 26-28; gamepad
    // buttons 100 + n.
    constexpr int kCaptureStick = 16;
    constexpr int kCaptureModifier = 24;
    constexpr int kCaptureRecentre = 25;
    constexpr int kCaptureHotkey = 26;
    constexpr int kCaptureGamepad = 100;

    HostInputHub &hub()
    {
        static HostInputHub instance;
        return instance;
    }

    std::atomic<bool> &port0Connected() // setPadPort0Connected
    {
        static std::atomic<bool> connected{true};
        return connected;
    }

    std::atomic<uint16_t> &overrideButtons() // PSPadBackend::setOverrideButtons
    {
        static std::atomic<uint16_t> mask{0};
        return mask;
    }

    // ZX_PAD_PORT1=mirror: the old routing for developer runs (port 1
    // always has a pad: player 2's own, else a copy of port 0, and the pad
    // script, ZX_CONTROL and a game override's presses reach every port).
    // Default: port 1 is player 2's gamepad alone, an empty port without
    // one (hi::routePort).
    bool mirrorPort1()
    {
        static const bool mirror = []()
        {
            const char *value = std::getenv("ZX_PAD_PORT1");
            const bool on = value && std::strcmp(value, "mirror") == 0;
            if (on)
                std::fprintf(stderr, "[pad] ZX_PAD_PORT1=mirror: port 1 copies port 0 without a pad of its own\n");
            return on;
        }();
        return mirror;
    }

    // ZX_PROMPTS=keyboard|playstation|xbox|nintendo: a fixed device for
    // the prompts (-1: follow the input).
    std::atomic<int> g_promptOverride{-1}; // PSPadBackend::forcePromptDevice

    int forcedDevice()
    {
        if (const int over = g_promptOverride.load(); over >= 0)
            return over;
        static const int forced = []()
        {
            const char *value = std::getenv("ZX_PROMPTS");
            const std::string v = value ? value : "";
            if (v == "keyboard")
                return static_cast<int>(PSPadBackend::InputDevice::Keyboard);
            if (v == "playstation")
                return static_cast<int>(PSPadBackend::InputDevice::PlayStation);
            if (v == "xbox")
                return static_cast<int>(PSPadBackend::InputDevice::Xbox);
            if (v == "nintendo")
                return static_cast<int>(PSPadBackend::InputDevice::Nintendo);
            return -1;
        }();
        return forced;
    }

    // [gamepad] prompts: one style always, unless ZX_PROMPTS (tests) fixes it.
    void applyPromptStyle(hi::PromptStyle style)
    {
        if (const char *env = std::getenv("ZX_PROMPTS"); env && env[0])
            return;
        PSPadBackend::forcePromptDevice(hi::promptStyleName(style));
    }

    PSPadBackend::InputDevice deviceFor(ps2_host::GamepadStyle style)
    {
        switch (style)
        {
        case ps2_host::GamepadStyle::PlayStation:
            return PSPadBackend::InputDevice::PlayStation;
        case ps2_host::GamepadStyle::Nintendo:
            return PSPadBackend::InputDevice::Nintendo;
        default:
            return PSPadBackend::InputDevice::Xbox;
        }
    }

    void setDevice(HostInputHub &h, PSPadBackend::InputDevice device)
    {
        const int value = static_cast<int>(device);
        if (h.device.exchange(value) != value)
        {
            h.deviceChanges.fetch_add(1u);
            static const char *const kNames[] = {"keyboard", "playstation", "xbox", "nintendo"};
            std::fprintf(stderr, "[input] prompts: %s\n", kNames[value]);
        }
    }

    // The prompts show this pad's buttons (nullptr: keyboard and mouse).
    void showPadPrompts(HostInputHub &h, const ps2_host::GamepadInfo *pad)
    {
        h.promptPad = pad ? pad->id : 0u;
        setDevice(h, pad ? deviceFor(pad->style) : PSPadBackend::InputDevice::Keyboard);
    }

    // Main thread (pollHost): the device whose input this frame has. pads
    // and states are the connected gamepads in slot order (none when
    // gamepads are off); the prompts follow the pad that was used.
    void updateActiveDevice(HostInputHub &h, const hi::HostCodeSet &keysDown, float dx, float dy,
                            const std::vector<ps2_host::GamepadInfo> &pads,
                            const std::vector<hi::GamepadState> &states)
    {
        if (const int forced = forcedDevice(); forced >= 0)
        {
            setDevice(h, static_cast<PSPadBackend::InputDevice>(forced));
            return;
        }
        h.lastPadButtons.resize(states.size(), 0u);
        const ps2_host::GamepadInfo *used = nullptr;
        for (size_t i = 0; i < states.size() && i < pads.size(); ++i)
        {
            const uint16_t padDown = static_cast<uint16_t>(states[i].buttons & ~h.lastPadButtons[i]);
            h.lastPadButtons[i] = states[i].buttons;
            bool stick = false;
            for (float axis : states[i].axes)
                stick = stick || std::fabs(axis) > 0.5f;
            if (!used && (padDown != 0u || stick))
                used = &pads[i];
        }
        if (h.device.load() < 0)
            showPadPrompts(h, pads.empty() ? nullptr : &pads.front());
        if (used)
            showPadPrompts(h, used);
        else if (keysDown.any() || std::fabs(dx) + std::fabs(dy) > 12.0f)
            showPadPrompts(h, nullptr);
        else if (pads.empty() && h.device.load() != static_cast<int>(PSPadBackend::InputDevice::Keyboard))
            showPadPrompts(h, nullptr);
    }

    // The connected gamepads changed (main thread): a pad that just
    // connected shows its prompts; if the pad shown went away, player 1's
    // pad (or the keyboard) takes over; a remapped pad shows its new style.
    void padsChanged(HostInputHub &h, const std::vector<ps2_host::GamepadInfo> &before,
                     const std::vector<ps2_host::GamepadInfo> &pads)
    {
        h.lastPadButtons.assign(pads.size(), 0u);
        for (size_t i = 0; i < pads.size(); ++i)
        {
            // A pad's buttons held while it moved slot are not new presses.
            hi::GamepadState state = ps2_host::gamepadState(static_cast<int>(i));
            h.lastPadButtons[i] = state.buttons;
        }
        if (forcedDevice() >= 0 || h.device.load() < 0)
            return;
        for (const ps2_host::GamepadInfo &pad : pads)
        {
            const bool isNew = std::none_of(before.begin(), before.end(),
                                            [&pad](const ps2_host::GamepadInfo &old) { return old.id == pad.id; });
            if (isNew && h.padsEnabledSeen)
            {
                showPadPrompts(h, &pad);
                return;
            }
        }
        if (h.promptPad == 0u)
            return;
        for (const ps2_host::GamepadInfo &pad : pads)
            if (pad.id == h.promptPad)
            {
                showPadPrompts(h, &pad);
                return;
            }
        showPadPrompts(h, pads.empty() ? nullptr : &pads.front());
    }

    // "DualSense Wireless Controller" -> "DualSense": what the Gamepads row
    // has room for (printable ASCII; the game's font has nothing else).
    std::string shortPadName(const ps2_host::GamepadInfo &pad)
    {
        std::string out;
        std::istringstream words(pad.name);
        std::string word;
        while (words >> word)
        {
            std::string lower = word;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower == "wireless" || lower == "controller" || lower == "gamepad")
                continue;
            std::string clean;
            for (unsigned char c : word)
                if (c >= 0x20 && c < 0x7F)
                    clean += static_cast<char>(c);
            if (clean.empty())
                continue;
            out += (out.empty() ? "" : " ") + clean;
        }
        if (out.empty() || out == "(unnamed)")
            out = ps2_host::gamepadStyleName(pad.style);
        if (out.size() > 20)
            out = out.substr(0, 20);
        return out;
    }

    // The Gamepads row: "DualSense", "1 Xbox 360, 2 DualSense", or the
    // styles when the names do not fit ("1 Xbox, 2 PlayStation").
    std::string padSummaryText(const std::vector<ps2_host::GamepadInfo> &pads)
    {
        if (pads.empty())
            return "None";
        if (pads.size() == 1)
            return shortPadName(pads.front());
        std::string names;
        std::string styles;
        for (const ps2_host::GamepadInfo &pad : pads)
        {
            const std::string number = std::to_string(pad.slot + 1) + " ";
            names += (names.empty() ? "" : ", ") + number + shortPadName(pad);
            styles += (styles.empty() ? "" : ", ") + number + ps2_host::gamepadStyleName(pad.style);
        }
        return names.size() <= 22 ? names : styles;
    }

    std::string controlsPath(const std::string &appName)
    {
        if (const char *path = zx::boot::text("ZX_CONTROLS_INI"); path && path[0])
            return path;
        const std::string dir = hi::userConfigDir(appName);
        if (dir.empty())
            return {};
        return (std::filesystem::path(dir) / "controls.ini").string();
    }

    // Loads controls.ini (writing the documented defaults on first run) and
    // ZX_HOST_INPUT_SCRIPT. Caller holds hub().mutex.
    void configureLocked(HostInputHub &h)
    {
        // Each boot reads its own (ZX_CONTROLS_INI, ZX_HOST_INPUT_SCRIPT
        // of its BootConfig).
        if (h.configured && h.configuredBoot == zx::boot::generation())
            return;
        h.configured = true;
        h.configuredBoot = zx::boot::generation();
        h.script = hi::HostInputScript{};
        h.scripted = false;
        h.haveLastTick = false;
        h.scriptOrigin = -1.0;
        h.lastScriptTime = -1.0;
        const hi::GameControls &game = *h.controls.load();
        h.config = hi::HostInputConfig::defaults(game.defaultIni);
        const std::string path = controlsPath(h.appName);
        h.path = path;
        if (path.empty())
        {
            std::fprintf(stderr, "[input] no user config folder; using the default controls\n");
        }
        else
        {
            std::error_code ec;
            const std::filesystem::path file(path);
            if (!std::filesystem::exists(file, ec))
            {
                if (file.has_parent_path())
                    std::filesystem::create_directories(file.parent_path(), ec);
                std::ofstream out(file, std::ios::binary);
                out << game.defaultIni;
                std::fprintf(stderr, "[input] wrote %s's default controls to %s\n", game.title.c_str(), path.c_str());
            }
            else
            {
                std::ifstream in(file, std::ios::binary);
                std::stringstream text;
                text << in.rdbuf();
                std::vector<std::string> warnings;
                h.config.applyIni(text.str(), &warnings);
                for (const std::string &warning : warnings)
                    std::fprintf(stderr, "[input] %s: %s\n", path.c_str(), warning.c_str());
                std::fprintf(stderr, "[input] controls from %s\n", path.c_str());
            }
        }
        h.mapper.setConfig(h.config);
        if (h.config.prompts != hi::PromptStyle::Auto)
            applyPromptStyle(h.config.prompts);

        if (const char *spec = zx::boot::text("ZX_HOST_INPUT_SCRIPT"); spec && spec[0])
        {
            std::vector<std::string> warnings;
            h.script.parse(spec, &warnings);
            for (const std::string &item : warnings)
                std::fprintf(stderr, "[input] ZX_HOST_INPUT_SCRIPT: ignoring '%s'\n", item.c_str());
            h.scripted = true;
            std::fprintf(stderr, "[input] host input script: %zu key, %zu mouse, %zu focus entries "
                                 "(keyboard, mouse and focus come from the script only)\n",
                         h.script.keys().size(), h.script.motions().size(), h.script.focus().size());
        }
        if (const char *trace = std::getenv("ZX_TRACE_HOST_INPUT"); trace && trace[0] == '1')
            h.trace = true;
    }

    // ---- settings pages: controls.ini values and rebinding ------------------

    struct IniEdit
    {
        std::string section;
        std::string key;
        std::string value;
    };

    // Writes changed values into controls.ini, keeping the rest of the file.
    void writeControls(const std::string &path, const std::vector<IniEdit> &edits)
    {
        if (path.empty() || edits.empty())
            return;
        std::string text;
        {
            std::ifstream in(path, std::ios::binary);
            if (in)
            {
                std::stringstream buffer;
                buffer << in.rdbuf();
                text = buffer.str();
            }
            else
            {
                text = hub().controls.load()->defaultIni;
            }
        }
        for (const IniEdit &edit : edits)
            text = hi::setIniValue(text, edit.section, edit.key, edit.value);
        const std::string temp = path + ".tmp";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << text;
            if (!out)
            {
                std::fprintf(stderr, "[input] cannot write %s\n", temp.c_str());
                return;
            }
        }
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec)
            std::fprintf(stderr, "[input] cannot replace %s: %s\n", path.c_str(), ec.message().c_str());
    }

    std::string formatNumber(float value)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", static_cast<double>(value));
        return text;
    }

    template <typename Config>
    auto captureList(Config &config, int target) -> decltype(&config.stickModifier)
    {
        if (target >= 0 && target < hi::kPadButtonCount)
            return &config.buttons[static_cast<size_t>(target)];
        if (target >= kCaptureStick && target < kCaptureStick + hi::kStickDirectionCount)
            return &config.stick[static_cast<size_t>(target - kCaptureStick)];
        if (target == kCaptureModifier)
            return &config.stickModifier;
        if (target == kCaptureRecentre)
            return &config.recentreKeys;
        if (target >= kCaptureHotkey && target < kCaptureHotkey + hi::kHotkeyCount)
            return &config.hotkeys[static_cast<size_t>(target - kCaptureHotkey)];
        return nullptr;
    }

    IniEdit captureEdit(const hi::HostInputConfig &config, int target)
    {
        if (target >= kCaptureGamepad)
        {
            std::string value;
            for (int physical : config.gamepadButtons[static_cast<size_t>(target - kCaptureGamepad)])
                value += (value.empty() ? "" : ", ") + hi::gamepadButtonName(physical);
            return {"gamepad", hi::padButtonName(target - kCaptureGamepad), value};
        }
        const std::string value = hi::formatBindings(*captureList(config, target));
        if (target < hi::kPadButtonCount)
            return {"buttons", hi::padButtonName(target), value};
        if (target < kCaptureStick + hi::kStickDirectionCount)
            return {"sticks", hi::stickDirectionName(target - kCaptureStick), value};
        if (target == kCaptureModifier)
            return {"sticks", "modifier", value};
        if (target >= kCaptureHotkey)
            return {"hotkeys", hi::hotkeyName(target - kCaptureHotkey), value};
        return {"mouse", "recentre", value};
    }

    // Binds `code` (a key or mouse button; a gamepad button for gamepad
    // targets; -1 unbinds) and saves. Caller holds hub().mutex.
    std::vector<IniEdit> bindCapturedLocked(HostInputHub &h, int target, int code)
    {
        std::vector<IniEdit> edits;
        hi::HostInputConfig &config = h.config;
        if (target >= kCaptureGamepad)
        {
            const size_t button = static_cast<size_t>(target - kCaptureGamepad);
            std::vector<int> &list = config.gamepadButtons[button];
            if (code < 0)
            {
                list.clear();
            }
            else
            {
                // A gamepad button presses one game button: swap with the
                // one that had it.
                const std::vector<int> previous = list;
                for (size_t other = 0; other < config.gamepadButtons.size(); ++other)
                {
                    std::vector<int> &otherList = config.gamepadButtons[other];
                    if (other == button || std::find(otherList.begin(), otherList.end(), code) == otherList.end())
                        continue;
                    otherList.erase(std::remove(otherList.begin(), otherList.end(), code), otherList.end());
                    if (otherList.empty() && !previous.empty())
                        otherList.push_back(previous.front());
                    edits.push_back(captureEdit(config, kCaptureGamepad + static_cast<int>(other)));
                }
                list = {code};
            }
        }
        else if (std::vector<hi::Binding> *list = captureList(config, target))
        {
            if (code < 0)
            {
                list->clear();
            }
            else
            {
                // The new key becomes the first binding (replacing the first
                // one), and no other game control keeps it.
                for (int other = 0; other < kCaptureHotkey + hi::kHotkeyCount; ++other)
                {
                    std::vector<hi::Binding> *otherList = captureList(config, other);
                    // Game controls and shortcuts (not the modifier or recentre keys).
                    if (other == target || !otherList || other == kCaptureModifier || other == kCaptureRecentre)
                        continue;
                    const size_t before = otherList->size();
                    otherList->erase(std::remove_if(otherList->begin(), otherList->end(),
                                                    [code](const hi::Binding &b) { return b.code == code; }),
                                     otherList->end());
                    if (otherList->size() != before)
                        edits.push_back(captureEdit(config, other));
                }
                list->erase(std::remove_if(list->begin(), list->end(),
                                           [code](const hi::Binding &b) { return b.code == code; }),
                            list->end());
                hi::Binding binding;
                binding.code = code;
                if (list->empty())
                    list->push_back(binding);
                else
                    list->front() = binding;
            }
        }
        else
        {
            return edits;
        }
        edits.push_back(captureEdit(config, target));
        h.mapper.setConfig(config);
        return edits;
    }

    // Mouse in menus: a wheel step presses up or down, a left click Cross
    // (accept; the left button's own Square is held back), each for two
    // reads with two released reads after it so repeats count. Caller holds
    // the mutex.
    void applyMenuMouse(HostInputHub &h, const hi::RawHostInput &raw, hi::PadReport &out)
    {
        constexpr int kLeft = hi::kMouseCodeBase + 0;
        while (h.wheelSince >= 1.0f && h.menuPulses.size() < 4u)
        {
            h.menuPulses.push_back(hi::kUp);
            h.wheelSince -= 1.0f;
        }
        while (h.wheelSince <= -1.0f && h.menuPulses.size() < 4u)
        {
            h.menuPulses.push_back(hi::kDown);
            h.wheelSince += 1.0f;
        }
        if (std::fabs(h.wheelSince) >= 1.0f)
            h.wheelSince = 0.0f;
        if (raw.pressed.test(kLeft) && h.menuPulses.size() < 4u)
            h.menuPulses.push_back(hi::kCross);
        if (raw.down.test(kLeft))
        {
            out.buttons = static_cast<uint16_t>(out.buttons & ~(1u << hi::kSquare));
            out.pressure[hi::kSquare] = 0u;
        }
        if (h.menuPulses.empty())
            return;
        const int button = h.menuPulses.front();
        if (h.menuPulseReads < 2)
        {
            out.buttons = static_cast<uint16_t>(out.buttons | (1u << button));
            out.pressure[static_cast<size_t>(button)] = 0xFFu;
        }
        if (++h.menuPulseReads >= 4)
        {
            h.menuPulseReads = 0;
            h.menuPulses.erase(h.menuPulses.begin());
        }
    }

    // The wheel on the right stick's Y axis: each step pushes it fully for
    // a few reads (a step the other way stops the push first). Caller holds
    // the mutex.
    void applyWheelStick(HostInputHub &h, hi::PadReport &out)
    {
        constexpr int kReadsPerStep = 6;
        constexpr int kMaxReads = 5 * kReadsPerStep;
        while (h.wheelSince >= 1.0f)
        {
            h.wheelStickReads = std::min(std::max(h.wheelStickReads, 0) + kReadsPerStep, kMaxReads);
            h.wheelSince -= 1.0f;
        }
        while (h.wheelSince <= -1.0f)
        {
            h.wheelStickReads = std::max(std::min(h.wheelStickReads, 0) - kReadsPerStep, -kMaxReads);
            h.wheelSince += 1.0f;
        }
        if (h.wheelStickReads > 0)
        {
            out.sticks[hi::kRy] = 0x00u;
            --h.wheelStickReads;
        }
        else if (h.wheelStickReads < 0)
        {
            out.sticks[hi::kRy] = 0xFFu;
            ++h.wheelStickReads;
        }
    }

    // Port-0 read: host input since the previous read, mapped to a pad report.
    hi::PadReport sampleHost(const std::atomic<uint64_t> *vsyncTick)
    {
        HostInputHub &h = hub();
        std::lock_guard<std::mutex> lock(h.mutex);
        configureLocked(h);

        // Emulated time since the previous read, in vsyncs.
        double clock = 0.0;
        if (vsyncTick)
        {
            clock = static_cast<double>(vsyncTick->load(std::memory_order_acquire));
        }
        else
        {
            static const auto origin = std::chrono::steady_clock::now();
            clock = std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count() *
                    PadScript::kVsyncsPerSecond;
        }
        const double dt = h.haveLastTick ? clock - h.lastClock : 1.0;
        h.haveLastTick = true;
        h.lastClock = clock;

        hi::RawHostInput raw;
        if (h.scripted)
        {
            // The ZX_PAD_SCRIPT clock when there is a pad script (so its
            // SYNC/WAIT anchors move these events too), else vsyncs since
            // the first pad read.
            double now = 0.0;
            if (!padScript().empty())
            {
                now = padScript().scriptTime();
            }
            else
            {
                if (h.scriptOrigin < 0.0)
                    h.scriptOrigin = clock;
                now = clock - h.scriptOrigin;
                StateProbe::publishScriptTime(static_cast<int64_t>(std::floor(now + 1e-6)));
            }
            raw = h.script.sample(h.lastScriptTime, now);
            h.lastScriptTime = now;
            h.wheelSince += raw.wheel;
        }
        else
        {
            raw.down = h.heldNow | h.heldSince;
            raw.pressed = h.pressedSince;
            raw.mouseDx = h.mouseDx;
            raw.mouseDy = h.mouseDy;
            raw.focused = h.focused;
        }
        raw.gamepad = h.gamepad;
        h.heldSince.reset();
        h.pressedSince.reset();
        h.mouseDx = 0.0f;
        h.mouseDy = 0.0f;

        if (h.freeInput.load())
        {
            // Everything to takeFreeInput; the game gets the gamepad alone
            // (readState adds the script and ZX_CONTROL, notes the pad
            // and sends the game an idle one).
            PSPadBackend::FreeInput &f = h.freeSince;
            f.down |= raw.down;
            f.pressed |= raw.pressed;
            f.mouseDx += raw.mouseDx;
            f.mouseDy += raw.mouseDy;
            f.wheel += h.scripted ? raw.wheel : h.wheelSince;
            f.focused = raw.focused;
            h.wheelSince = 0.0f;
            h.wheelStickReads = 0;
            h.menuPulses.clear();
            h.menuPulseReads = 0;
            h.wantCapture.store(raw.focused && !h.scripted, std::memory_order_release);
            hi::PadReport pad;
            pad.buttons = raw.gamepad.buttons;
            for (int i = 0; i < 4; ++i)
            {
                const float v = std::clamp(raw.gamepad.axes[static_cast<size_t>(i)], -1.0f, 1.0f);
                pad.sticks[static_cast<size_t>(i)] = static_cast<uint8_t>(std::lround(127.5f + v * 127.5f));
            }
            h.lastReport = hi::PadReport{};
            return pad;
        }

        const bool wasCaptured = h.mapper.captured();
        const hi::PadReport report = h.mapper.map(raw, dt);
        if (h.mapper.captured() != wasCaptured)
        {
            std::fprintf(stderr, "[input] mouse %s%s\n",
                         h.mapper.captured() ? "captured" : (raw.focused ? "released" : "released (focus lost)"),
                         h.scripted ? " (scripted, cursor not grabbed)" : "");
        }
        h.wantCapture.store(h.mapper.captured() && !h.scripted, std::memory_order_release);

        if (h.trace)
        {
            const bool changed = report.buttons != h.lastReport.buttons || report.sticks != h.lastReport.sticks ||
                                 report.pressure != h.lastReport.pressure;
            static bool s_lastFocused = true;
            static bool s_lastCaptured = false;
            if (changed || raw.focused != s_lastFocused || h.mapper.captured() != s_lastCaptured)
            {
                std::fprintf(stderr,
                             "[input] tick=%.0f script=%lld buttons=0x%04x rx=%02x ry=%02x lx=%02x ly=%02x "
                             "focused=%d captured=%d\n",
                             clock, static_cast<long long>(StateProbe::scriptTime()), report.buttons,
                             report.sticks[hi::kRx], report.sticks[hi::kRy], report.sticks[hi::kLx],
                             report.sticks[hi::kLy], raw.focused ? 1 : 0, h.mapper.captured() ? 1 : 0);
            }
            s_lastFocused = raw.focused;
            s_lastCaptured = h.mapper.captured();
        }
        hi::PadReport out = report;
        if (h.menuMouse.load())
        {
            applyMenuMouse(h, raw, out);
            h.wheelStickReads = 0;
        }
        else if (h.wheelStick.load())
        {
            applyWheelStick(h, out);
        }
        else
        {
            h.wheelSince = 0.0f;
            h.wheelStickReads = 0;
        }
        if (h.pressedReads > 0 && h.pressedButton >= 0)
        {
            --h.pressedReads;
            out.buttons = static_cast<uint16_t>(out.buttons | (1u << h.pressedButton));
            out.pressure[static_cast<size_t>(h.pressedButton)] = 0xFFu;
        }
        h.lastReport = out;
        return out;
    }

    // DualShock 2 pressure byte order in the pad data from byte 8.
    constexpr int kPressureOrder[12] = {hi::kRight, hi::kLeft, hi::kUp, hi::kDown,
                                        hi::kTriangle, hi::kCircle, hi::kCross, hi::kSquare,
                                        hi::kL1, hi::kR1, hi::kL2, hi::kR2};
}

namespace
{
    namespace st = zx::settings;

    // A [section] key = number setting of controls.ini.
    void addControlsNumber(HostInputHub &h, st::Setting setting, float hi::HostInputConfig::*field, float scale,
                           std::vector<IniEdit> (*edits)(const hi::HostInputConfig &))
    {
        setting.current = [&h, field, scale]()
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            return static_cast<int>(std::lround(h.config.*field * scale));
        };
        setting.apply = [&h, field, scale, edits](int value)
        {
            std::vector<IniEdit> changes;
            std::string path;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                h.config.*field = static_cast<float>(value) / scale;
                h.mapper.setConfig(h.config);
                changes = edits(h.config);
                path = h.path;
            }
            writeControls(path, changes);
        };
        st::add(std::move(setting));
    }

    std::string captureText(HostInputHub &h, int target)
    {
        if (h.captureTarget.load() == target)
            return "Press a key";
        std::lock_guard<std::mutex> lock(h.mutex);
        const IniEdit edit = captureEdit(h.config, target);
        return edit.value.empty() ? std::string("None") : edit.value;
    }

    void addCaptureRow(HostInputHub &h, const std::string &key, const std::string &label, int target, const std::string &help)
    {
        st::Setting setting;
        setting.key = key;
        setting.label = label;
        setting.kind = st::Kind::Action;
        setting.help = help;
        setting.format = [&h, target](int) { return captureText(h, target); };
        setting.activate = [&h, target]()
        {
            int index = -1;
            std::string name;
            h.capturePadBefore = ps2_host::gamepad(index, name).buttons;
            h.captureFirstFrame = true;
            h.captureTarget.store(target);
        };
        st::add(std::move(setting));
    }

    int captureTargetForKey(const std::string &key)
    {
        for (int i = 0; i < hi::kPadButtonCount; ++i)
            if (key == hi::padButtonName(i))
                return i;
        for (int i = 0; i < hi::kStickDirectionCount; ++i)
            if (key == hi::stickDirectionName(i))
                return kCaptureStick + i;
        if (key == "modifier")
            return kCaptureModifier;
        if (key == "recentre")
            return kCaptureRecentre;
        for (int i = 0; i < hi::kHotkeyCount; ++i)
            if (key == hi::hotkeyName(i))
                return kCaptureHotkey + i;
        return -1;
    }

    // controls.* settings (main thread, once the config is loaded, and again
    // for each game: the rows are the game's, its action names and its keys).
    void registerControlSettings(HostInputHub &h)
    {
        // The game before's rows (the collection boots another game in the
        // same process): gone before this game's go in.
        for (const char *prefix : {"controls.", "hotkey."})
            for (const std::string &key : st::keys(prefix))
                st::remove(key);

        st::Setting look;
        look.key = "controls.mouse_sensitivity";
        look.label = "Mouse Sensitivity";
        look.kind = st::Kind::Range;
        look.minValue = 10;
        look.maxValue = 400;
        look.step = 10;
        look.unit = "%";
        look.help = "How far the camera turns with the mouse.";
        look.current = [&h]()
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            return static_cast<int>(std::lround(h.config.mouseSensitivityX * 100.0f));
        };
        look.apply = [&h](int value)
        {
            std::string path;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                h.config.mouseSensitivityX = h.config.mouseSensitivityY = static_cast<float>(value) / 100.0f;
                h.mapper.setConfig(h.config);
                path = h.path;
            }
            const std::string v = formatNumber(static_cast<float>(value) / 100.0f);
            writeControls(path, {{"mouse", "sensitivity_x", v}, {"mouse", "sensitivity_y", v}});
        };
        st::add(std::move(look));

        st::Setting invert;
        invert.key = "controls.invert_y";
        invert.label = "Invert Mouse Y";
        invert.help = "Mouse up looks down.";
        // invert_y = true in controls.ini is the usual direction (mouse up
        // looks up), so the setting shown is its opposite.
        invert.current = [&h]()
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            return h.config.mouseInvertY ? 0 : 1;
        };
        invert.apply = [&h](int value)
        {
            std::string path;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                h.config.mouseInvertY = value == 0;
                h.mapper.setConfig(h.config);
                path = h.path;
            }
            writeControls(path, {{"mouse", "invert_y", value == 0 ? "true" : "false"}});
        };
        st::add(std::move(invert));

        st::Setting mode;
        mode.key = "controls.mouse_mode";
        mode.label = "Mouse Look";
        mode.kind = st::Kind::Choice;
        mode.choices = {"Free Look", "Right Stick"};
        mode.help = "The mouse turns the view or moves the stick.";
        mode.current = [&h]()
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            return h.config.mouseCamera == hi::MouseCamera::Stick ? 1 : 0;
        };
        mode.apply = [&h](int value)
        {
            std::string path;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                h.config.mouseCamera = value == 1 ? hi::MouseCamera::Stick : hi::MouseCamera::Look;
                h.mapper.setConfig(h.config);
                path = h.path;
            }
            writeControls(path, {{"mouse", "mode", value == 1 ? "stick" : "look"}});
        };
        st::add(std::move(mode));

        st::Setting deadzone;
        deadzone.key = "controls.stick_deadzone";
        deadzone.label = "Stick Dead Zone";
        deadzone.kind = st::Kind::Range;
        deadzone.minValue = 0;
        deadzone.maxValue = 60;
        deadzone.step = 5;
        deadzone.unit = "%";
        deadzone.help = "Smaller stick pushes are ignored.";
        addControlsNumber(h, deadzone, &hi::HostInputConfig::gamepadStickThreshold, 100.0f,
                          [](const hi::HostInputConfig &c) {
                              return std::vector<IniEdit>{{"gamepad", "stick_threshold", formatNumber(c.gamepadStickThreshold)}};
                          });

        st::Setting gamepad;
        gamepad.key = "controls.gamepad";
        gamepad.label = "Gamepad";
        gamepad.help = "Off ignores connected gamepads.";
        gamepad.current = [&h]()
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            return h.config.gamepadEnabled ? 1 : 0;
        };
        gamepad.apply = [&h](int value)
        {
            std::string path;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                h.config.gamepadEnabled = value != 0;
                h.mapper.setConfig(h.config);
                path = h.path;
            }
            writeControls(path, {{"gamepad", "enabled", value != 0 ? "true" : "false"}});
        };
        st::add(std::move(gamepad));

        // Diagnostics: which pads are connected (live), their motors, and
        // the player order (for split screen).
        st::Setting pads;
        pads.key = "controls.gamepads";
        pads.label = "Connected Gamepads";
        pads.kind = st::Kind::Action;
        pads.help = "Player 1 first.";
        pads.format = [&h](int)
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            return h.padSummary;
        };
        pads.activate = []() { ps2_host::logGamepads(); };
        st::add(std::move(pads));

        st::Setting rumble;
        rumble.key = "controls.test_rumble";
        rumble.label = "Test Rumble";
        rumble.kind = st::Kind::Action;
        rumble.help = "Rumbles each gamepad.";
        rumble.format = [](int) { return std::string(); };
        rumble.activate = []() { ps2_host::startRumbleTest(); };
        st::add(std::move(rumble));

        st::Setting swap;
        swap.key = "controls.swap_players";
        swap.label = "Swap Pads 1 and 2";
        swap.kind = st::Kind::Action;
        swap.help = "Players 1 and 2 trade gamepads.";
        swap.format = [](int) { return std::string(); };
        swap.activate = []()
        {
            if (ps2_host::gamepadCount() < 2)
            {
                std::fprintf(stderr, "[pad] swap players: needs two gamepads\n");
                return;
            }
            ps2_host::swapGamepadPlayers();
            std::fprintf(stderr, "[pad] players 1 and 2 swapped gamepads\n");
        };
        st::add(std::move(swap));

        st::Setting prompts;
        prompts.key = "controls.prompts";
        prompts.label = "Button Prompts";
        prompts.kind = st::Kind::Choice;
        prompts.choices = {"Auto", "Keyboard", "Xbox", "PlayStation", "Nintendo"};
        prompts.help = "Auto matches the device in use.";
        prompts.current = [&h]()
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            return static_cast<int>(h.config.prompts);
        };
        prompts.apply = [&h](int value)
        {
            const hi::PromptStyle style = static_cast<hi::PromptStyle>(std::clamp(value, 0, 4));
            std::string path;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                h.config.prompts = style;
                path = h.path;
            }
            applyPromptStyle(style);
            writeControls(path, {{"gamepad", "prompts", hi::promptStyleName(style)}});
        };
        st::add(std::move(prompts));

        std::vector<std::pair<std::string, std::string>> labels = h.labels;
        if (labels.empty())
            labels = h.controls.load()->labels;
        if (labels.empty())
        {
            const char *names[] = {"Cross", "Square", "Triangle", "Circle", "L1", "R1", "L2", "R2", "Start", "Select",
                                   "Up", "Down", "Left", "Right", "L3", "R3"};
            const int order[] = {hi::kCross, hi::kSquare, hi::kTriangle, hi::kCircle, hi::kL1, hi::kR1, hi::kL2, hi::kR2,
                                 hi::kStart, hi::kSelect, hi::kUp, hi::kDown, hi::kLeft, hi::kRight, hi::kL3, hi::kR3};
            for (int i = 0; i < hi::kPadButtonCount; ++i)
                labels.emplace_back(hi::padButtonName(order[i]), names[i]);
        }
        const std::string keyHelp = "Cross, then a key. Esc cancels, Backspace clears.";
        for (const auto &[key, label] : labels)
        {
            const int target = captureTargetForKey(key);
            if (target >= 0)
                addCaptureRow(h, "controls.key." + key, label, target, keyHelp);
        }
        const std::string padHelp = "Cross, then a pad button. Esc cancels.";
        for (const auto &[key, label] : labels)
        {
            const int target = captureTargetForKey(key);
            if (target >= 0 && target < hi::kPadButtonCount)
                addCaptureRow(h, "controls.pad." + key, label, kCaptureGamepad + target, padHelp);
        }

        // The shortcuts as settings too, for ZX_CONTROL (`set
        // hotkey.screenshot 1`), every game's; no page shows the "hotkey."
        // group.
        for (int i = 0; i < hi::kHotkeyCount; ++i)
        {
            st::Setting key;
            key.key = std::string("hotkey.") + hi::hotkeyName(i);
            key.label = key.key;
            key.current = []() { return 0; };
            key.apply = [i](int value)
            {
                if (value != 0)
                    PSPadBackend::triggerHotkey(i);
            };
            st::add(std::move(key));
        }

        st::Setting reset;
        reset.key = "controls.reset";
        reset.label = "Reset Controls";
        reset.kind = st::Kind::Action;
        reset.help = "All controls back to the defaults.";
        reset.format = [](int) { return std::string(); };
        reset.activate = [&h]()
        {
            std::string path;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                h.config = hi::HostInputConfig::defaults(h.controls.load()->defaultIni);
                h.mapper.setConfig(h.config);
                path = h.path;
            }
            if (path.empty())
                return;
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << h.controls.load()->defaultIni;
            std::fprintf(stderr, "[input] controls reset to the defaults (%s)\n", path.c_str());
        };
        st::add(std::move(reset));
    }
}

void PSPadBackend::setControlLabels(std::vector<std::pair<std::string, std::string>> labels)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    h.labels = std::move(labels);
}

void PSPadBackend::pressButton(int button, int reads)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    h.pressedButton = button >= 0 && button < hi::kPadButtonCount ? button : -1;
    h.pressedReads = reads;
}

uint32_t PSPadBackend::hotkeyPresses(int hotkey)
{
    if (hotkey < 0 || hotkey >= hi::kHotkeyCount)
        return 0u;
    return play_recording::read(play_recording::Channel::Hotkey, hub().hotkeyPresses[static_cast<size_t>(hotkey)].load(),
                                static_cast<uint32_t>(hotkey));
}

std::vector<hi::Binding> PSPadBackend::hotkeyKeys(int hotkey)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    configureLocked(h);
    if (hotkey < 0 || hotkey >= hi::kHotkeyCount)
        return {};
    return h.config.hotkeys[static_cast<size_t>(hotkey)];
}

void PSPadBackend::triggerHotkey(int hotkey)
{
    if (hotkey >= 0 && hotkey < hi::kHotkeyCount)
        hub().hotkeyPresses[static_cast<size_t>(hotkey)].fetch_add(1u);
}

bool PSPadBackend::captureActive()
{
    return hub().captureTarget.load() >= 0;
}

void PSPadBackend::setGame(const std::string &id)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    const hi::GameControls &game = hi::gameControls(id);
    if (h.controls.load() != &game)
    {
        h.controls = &game;
        h.configured = false;         // its defaults from the next use on
        h.settingsRegistered = false; // and its own rows (the next pollHost)
    }
    h.appName = game.title;
}

void PSPadBackend::setAppName(const std::string &name)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    if (!name.empty())
        h.appName = name;
}

ps2_host_input::LookFrame PSPadBackend::mouseLookFrame(float basePitch, hi::HostInputMapper::LookContext context)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    configureLocked(h);
    const hi::LookFrame frame = h.mapper.cameraFrame(basePitch, context);
    if (h.trace)
    {
        static hi::LookFrame s_last;
        if (frame.active != s_last.active || frame.yaw != s_last.yaw || frame.pitch != s_last.pitch)
        {
            std::fprintf(stderr, "[input] look script=%lld yaw=%.2f pitch=%.2f base=%.2f active=%d\n",
                         static_cast<long long>(StateProbe::scriptTime()), frame.yaw, frame.pitch, basePitch,
                         frame.active ? 1 : 0);
        }
        s_last = frame;
    }
    if (!play_recording::active())
        return frame;
    // A play recording: the frame's fields as bytes (not the struct, whose padding is not part of it).
    float fields[6] = {frame.yaw, frame.pitch, frame.snapYaw, frame.snapPitch, frame.upLimit, frame.downLimit};
    uint8_t bytes[1 + sizeof(fields)];
    bytes[0] = frame.active ? 1u : 0u;
    std::memcpy(bytes + 1, fields, sizeof(fields));
    play_recording::read(play_recording::Channel::MouseLook, 0u, bytes, sizeof(bytes));
    std::memcpy(fields, bytes + 1, sizeof(fields));
    hi::LookFrame recorded;
    recorded.active = bytes[0] != 0u;
    recorded.yaw = fields[0];
    recorded.pitch = fields[1];
    recorded.snapYaw = fields[2];
    recorded.snapPitch = fields[3];
    recorded.upLimit = fields[4];
    recorded.downLimit = fields[5];
    return recorded;
}

void PSPadBackend::pollHost()
{
    HostInputHub &h = hub();
    // ps2_host::pumpEvents ran for this frame.
    const ps2_host::InputFrame &frame = ps2_host::input();
    const hi::HostCodeSet held = frame.held;
    const bool focused = frame.focused;
    float dx = frame.mouseDx;
    float dy = frame.mouseDy;
    if (h.skipMouseFrames > 0)
    {
        // Grabbing or freeing the cursor moves it; that jump is not motion.
        --h.skipMouseFrames;
        dx = 0.0f;
        dy = 0.0f;
    }

    bool gamepadEnabled = true;
    std::string controlsFile;
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        configureLocked(h);
        gamepadEnabled = h.config.gamepadEnabled;
        controlsFile = h.path;
    }
    if (!h.settingsRegistered)
    {
        h.settingsRegistered = true;
        registerControlSettings(h);
    }
    if (!h.mappingsLoaded)
    {
        // The user's own gamecontrollerdb.txt: ZX_GAMEPAD_MAPPINGS (the
        // collection's, one for every game), else next to controls.ini
        // (SDL remaps pads it covers that are already connected).
        h.mappingsLoaded = true;
        if (const char *mappings = zx::boot::text("ZX_GAMEPAD_MAPPINGS"); mappings && mappings[0])
            ps2_host::loadGamepadMappings(mappings);
        else if (!controlsFile.empty())
            ps2_host::loadGamepadMappings(
                (std::filesystem::path(controlsFile).parent_path() / "gamecontrollerdb.txt").string());
    }
    ps2_host::setGamepadsEnabled(gamepadEnabled);

    // Every connected gamepad in player order; with gamepads off, none.
    if (const uint32_t generation = ps2_host::gamepadGeneration();
        !h.padsSeen || generation != h.padGeneration || gamepadEnabled != h.padsEnabledSeen)
    {
        const std::vector<ps2_host::GamepadInfo> before = h.pads;
        const bool wasSeen = h.padsSeen;
        h.padsSeen = true;
        h.padGeneration = generation;
        h.padsEnabledSeen = gamepadEnabled;
        h.pads = gamepadEnabled ? ps2_host::gamepads() : std::vector<ps2_host::GamepadInfo>{};
        if (h.pads.empty())
        {
            std::fprintf(stderr, "[pad] no host gamepad%s; using keyboard and mouse (controls.ini)\n",
                         gamepadEnabled ? "" : " (gamepads off)");
        }
        else
        {
            std::string list;
            for (const ps2_host::GamepadInfo &pad : h.pads)
                list += (list.empty() ? "" : ", ") + std::string("player ") + std::to_string(pad.slot + 1) + " \"" +
                        pad.name + "\" (" + ps2_host::gamepadStyleName(pad.style) + ")";
            std::fprintf(stderr, "[pad] gamepads: %s%s\n", list.c_str(),
                         h.pads.size() >= 2 ? "; port 1 (player 2) reads player 2's pad" : "");
        }
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            h.padSummary = padSummaryText(gamepadEnabled ? h.pads : ps2_host::gamepads());
            h.gamepadCount = static_cast<int>(h.pads.size());
        }
        if (wasSeen)
            padsChanged(h, before, h.pads);
    }
    std::vector<hi::GamepadState> states;
    for (size_t i = 0; i < h.pads.size(); ++i)
        states.push_back(ps2_host::gamepadState(static_cast<int>(i)));
    const hi::PortSource port0 = hi::routePort(0, static_cast<int>(h.pads.size()), gamepadEnabled);
    const hi::PortSource port1 = hi::routePort(1, static_cast<int>(h.pads.size()), gamepadEnabled);
    hi::GamepadState gamepad = port0.gamepadSlot >= 0 ? states[static_cast<size_t>(port0.gamepadSlot)] : hi::GamepadState{};
    hi::GamepadState gamepad2 = port1.gamepadSlot >= 0 ? states[static_cast<size_t>(port1.gamepadSlot)] : hi::GamepadState{};

    updateActiveDevice(h, (held & ~h.prevHeld) | frame.pressed, dx, dy, h.pads, states);
    {
        const hi::HostCodeSet down = (held & ~h.prevHeld) | frame.pressed;
        if (down.any())
        {
            const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
            for (int c = 0; c < hi::kHostCodeCount; ++c)
                if (down.test(static_cast<size_t>(c)))
                    h.downAt[static_cast<size_t>(c)].store(now);
        }
        // [hotkeys]: a bound key pressed, or a gamepad chord completed (not
        // while a binding is being captured).
        std::array<std::vector<hi::Binding>, hi::kHotkeyCount> keys;
        std::array<std::vector<int>, hi::kHotkeyCount> chords;
        {
            std::lock_guard<std::mutex> lock(h.mutex);
            keys = h.config.hotkeys;
            chords = h.config.hotkeyPads;
        }
        const uint16_t padBefore = h.hotkeyPadBefore;
        h.hotkeyPadBefore = gamepad.buttons;
        if (h.captureTarget.load() < 0 && !h.holdUntilReleased)
            for (int i = 0; i < hi::kHotkeyCount; ++i)
            {
                bool hit = false;
                if (focused)
                    for (const hi::Binding &binding : keys[static_cast<size_t>(i)])
                        hit = hit || (binding.code >= 0 && binding.code < hi::kHostCodeCount &&
                                      down.test(static_cast<size_t>(binding.code)));
                uint16_t chord = 0;
                for (int physical : chords[static_cast<size_t>(i)])
                    if (physical >= 0 && physical < hi::kPadButtonCount)
                        chord = static_cast<uint16_t>(chord | (1u << physical));
                if (chord != 0u && (gamepad.buttons & chord) == chord && (padBefore & chord) != chord)
                    hit = true;
                if (hit)
                    h.hotkeyPresses[static_cast<size_t>(i)].fetch_add(1u);
            }
    }

    // Rebinding: the input of the frame goes to the capture, not the game;
    // afterwards everything held stays away from the game until released.
    hi::HostCodeSet forwardHeld = held;
    hi::HostCodeSet forwardPressed = frame.pressed;
    const int target = h.captureTarget.load();
    if (target >= 0)
    {
        const hi::HostCodeSet pressed = (held & ~h.prevHeld) | frame.pressed;
        const uint16_t padPressed = h.captureFirstFrame ? 0u : static_cast<uint16_t>(gamepad.buttons & ~h.capturePadBefore);
        h.capturePadBefore = gamepad.buttons;
        h.captureFirstFrame = false;
        constexpr int kEscape = 256, kBackspace = 259, kDelete = 261;
        bool done = false;
        int code = -1;
        if (pressed.test(kEscape))
        {
            done = true;
        }
        else if (pressed.test(kBackspace) || pressed.test(kDelete))
        {
            done = true;
            std::vector<IniEdit> edits;
            std::string path;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                edits = bindCapturedLocked(h, target, -1);
                path = h.path;
            }
            writeControls(path, edits);
        }
        else if (target >= kCaptureGamepad)
        {
            for (int bit = 0; bit < hi::kPadButtonCount && !done; ++bit)
                if (padPressed & (1u << bit))
                {
                    done = true;
                    code = bit;
                }
        }
        else
        {
            for (int c = 1; c < hi::kHostCodeCount && !done; ++c)
                if (pressed.test(static_cast<size_t>(c)))
                {
                    done = true;
                    code = c;
                }
            // A gamepad's Circle or Start gives up (keyboard rows).
            if (!done && (padPressed & ((1u << hi::kCircle) | (1u << hi::kStart))))
                done = true;
        }
        if (done && code >= 0)
        {
            std::vector<IniEdit> edits;
            std::string path;
            {
                std::lock_guard<std::mutex> lock(h.mutex);
                edits = bindCapturedLocked(h, target, code);
                path = h.path;
            }
            writeControls(path, edits);
            std::fprintf(stderr, "[input] bound %s\n", edits.empty() ? "?" : (edits.back().key + " = " + edits.back().value).c_str());
        }
        if (done)
        {
            h.captureTarget.store(-1);
            h.holdUntilReleased = true;
        }
        forwardHeld.reset();
        forwardPressed.reset();
        gamepad.buttons = 0;
        gamepad2.buttons = 0;
        dx = dy = 0.0f;
    }
    else if (h.holdAfterFree.exchange(false) || h.holdUntilReleased)
    {
        h.holdUntilReleased = true;
        if (held.none() && gamepad.buttons == 0 && gamepad2.buttons == 0)
            h.holdUntilReleased = false;
        forwardHeld.reset();
        forwardPressed.reset();
        gamepad.buttons = 0;
        gamepad2.buttons = 0;
    }

    {
        std::lock_guard<std::mutex> lock(h.mutex);
        // A key pressed and released within one frame still counts.
        h.pressedSince |= (forwardHeld & ~h.prevHeld) | forwardPressed;
        h.heldSince |= forwardHeld | forwardPressed;
        h.heldNow = forwardHeld;
        h.mouseDx += dx;
        h.mouseDy += dy;
        if (target < 0 && focused)
            h.wheelSince += frame.wheel;
        h.focused = focused;
        h.gamepad = gamepad;
        h.gamepad2 = gamepad2;
    }
    h.prevHeld = held;

    const bool want = h.wantCapture.load(std::memory_order_acquire) && focused;
    if (want != h.osCaptured)
    {
        ps2_host::setMouseCaptured(want);
        h.osCaptured = want;
        h.skipMouseFrames = 2;
    }
}

bool PSPadBackend::readState(int port, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    data[0] = 0x01;
    data[1] = kPadAnalogMarker;

    // Host keyboard/mouse/gamepad through controls.ini. Port 1 reads
    // player 2's own gamepad when one is connected; without one the port
    // is empty (portConnected; the pad stubs report no pad and do not read
    // it), or mirrors the last port-0 report with ZX_PAD_PORT1=mirror
    // (hi::routePort).
    const hi::PadReport report = hostInputOff() ? hi::PadReport{} : port == 0 ? sampleHost(m_vsyncTick) : [port]()
    {
        HostInputHub &h = hub();
        std::lock_guard<std::mutex> lock(h.mutex);
        configureLocked(h);
        const hi::PortSource source = hi::routePort(port, h.gamepadCount, h.config.gamepadEnabled, mirrorPort1());
        if (source.gamepadSlot == 1 && h.gamepad2.present)
        {
            hi::PadReport own;
            hi::applyGamepad(h.config, h.gamepad2, own);
            return own;
        }
        return source.mirrorPort0 ? h.lastReport : hi::PadReport{};
    }();
    // The developer inputs below (pad script, ZX_CONTROL, a game
    // override's presses) are player 1's: port 0 only, unless
    // ZX_PAD_PORT1=mirror (then every port, as before).
    const bool developerInput = port == 0 || mirrorPort1();

    uint16_t pressed = report.buttons;
    data[4] = report.sticks[hi::kRx];
    data[5] = report.sticks[hi::kRy];
    data[6] = report.sticks[hi::kLx];
    data[7] = report.sticks[hi::kLy];
    // ZX_PAD_STICK_CENTER=<n>: report idle sticks as n instead of 0x80
    // (debug aid for comparing against other pad sources).
    static zx::boot::PerBoot<uint8_t> stickCenterOfBoot([]()
    {
        const char *value = zx::boot::text("ZX_PAD_STICK_CENTER");
        return value && value[0] ? static_cast<uint8_t>(std::strtoul(value, nullptr, 0)) : PadScript::kStickCenter;
    });
    const uint8_t stickCenter = stickCenterOfBoot.get();
    if (stickCenter != PadScript::kStickCenter)
        for (int i = 4; i < 8; ++i)
            if (data[i] == PadScript::kStickCenter)
                data[i] = stickCenter;
    uint8_t pressure[hi::kPadButtonCount];
    std::memcpy(pressure, report.pressure.data(), sizeof(pressure));

    // ZX_PAD_SCRIPT on top: its buttons press at full pressure, its
    // stick entries override the host sticks.
    const ScriptFrame script = developerInput ? applyScript(port, m_vsyncTick, m_rdram) : ScriptFrame{};
    pressed = static_cast<uint16_t>(pressed | script.buttons);
    for (int bit = 0; bit < hi::kPadButtonCount; ++bit)
        if (script.buttons & (1u << bit))
            pressure[bit] = 0xFFu;
    for (int i = 0; i < 4; ++i)
        if (script.axisActive[i])
            data[4 + i] = script.sticks[i];
    // ZX_CONTROL (runtime/ps2_debug_control.h) on top of both, on the
    // same ports as the script.
    if (developerInput && ps2_debug_control::enabled())
    {
        const uint16_t control = ps2_debug_control::buttons();
        pressed = static_cast<uint16_t>(pressed | control);
        for (int bit = 0; bit < hi::kPadButtonCount; ++bit)
            if (control & (1u << bit))
                pressure[bit] = 0xFFu;
        ps2_debug_control::sticks(data + 4);
    }
    // ZX_PLAY_LINK (runtime/ps2_play_link.h): a player in lock step.
    if (port == 0 && ps2_play_link::enabled())
    {
        ps2_play_link::onPadRead(m_rdram, m_vsyncTick ? m_vsyncTick->load(std::memory_order_acquire) : 0u,
                                 StateProbe::scriptTime(), pressed, data + 4, pressure);
    }
    else if (port == 1 && mirrorPort1() && ps2_play_link::enabled())
        ps2_play_link::onMirroredRead(pressed, data + 4, pressure);

    // A game override's own presses (setOverrideButtons), the same way.
    if (const uint16_t own = developerInput ? overrideButtons().load() : 0u; own != 0u)
    {
        pressed = static_cast<uint16_t>(pressed | own);
        for (int bit = 0; bit < hi::kPadButtonCount; ++bit)
            if (own & (1u << bit))
                pressure[bit] = 0xFFu;
    }

    if (hub().freeInput.load())
    {
        // Photo mode and the like: the pad goes to takeFreeInput, the game
        // reads an idle one on every port.
        if (port == 0)
        {
            HostInputHub &h = hub();
            std::lock_guard<std::mutex> lock(h.mutex);
            PSPadBackend::FreeInput &f = h.freeSince;
            f.padPressed = static_cast<uint16_t>(f.padPressed | (pressed & ~h.freePadBefore));
            f.padButtons = pressed;
            h.freePadBefore = pressed;
            for (int i = 0; i < 4; ++i)
                f.sticks[static_cast<size_t>(i)] =
                    std::clamp((static_cast<float>(data[4 + i]) - 127.5f) / 127.5f, -1.0f, 1.0f);
        }
        pressed = 0u;
        std::memset(pressure, 0, sizeof(pressure));
        for (int i = 4; i < 8; ++i)
            data[i] = PadScript::kStickCenter;
    }

    const uint16_t btns = static_cast<uint16_t>(~pressed);
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    for (int i = 0; i < 12; ++i)
        data[8 + i] = pressure[kPressureOrder[i]];
    // A play recording (runtime/play_recording.h): the pad as the game read
    // it, buttons, sticks and pressures; a replay's in its place.
    play_recording::read(port == 0 ? play_recording::Channel::Pad0 : play_recording::Channel::Pad1, 0u, data + 2, 18u);
    return true;
}

PSPadBackend::InputDevice PSPadBackend::activeDevice()
{
    const int device = hub().device.load();
    const InputDevice live = device >= 0 ? static_cast<InputDevice>(device)
                             : forcedDevice() >= 0 ? static_cast<InputDevice>(forcedDevice())
                                                   : InputDevice::Keyboard;
    return play_recording::read(play_recording::Channel::PromptDevice, live);
}

uint32_t PSPadBackend::promptGeneration()
{
    HostInputHub &h = hub();
    uint32_t hash = 2166136261u;
    const auto mix = [&hash](uint32_t value)
    {
        hash = (hash ^ value) * 16777619u;
    };
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        configureLocked(h);
        for (const auto &list : h.config.buttons)
        {
            for (const hi::Binding &b : list)
                mix(static_cast<uint32_t>(b.code));
            mix(0xFFFFu);
        }
        for (const auto &list : h.config.stick)
        {
            for (const hi::Binding &b : list)
                mix(static_cast<uint32_t>(b.code));
            mix(0xFFFFu);
        }
        for (const auto &list : h.config.gamepadButtons)
        {
            for (int b : list)
                mix(static_cast<uint32_t>(b));
            mix(0xFFFFu);
        }
    }
    mix(static_cast<uint32_t>(h.device.load()));
    mix(h.deviceChanges.load());
    return play_recording::read(play_recording::Channel::PromptGeneration, hash);
}

std::vector<int> PSPadBackend::keyBindings(int padButton)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    configureLocked(h);
    std::vector<int> out;
    if (padButton >= 0 && padButton < hi::kPadButtonCount)
        for (const hi::Binding &b : h.config.buttons[static_cast<size_t>(padButton)])
            out.push_back(b.code);
    return out;
}

std::vector<int> PSPadBackend::stickKeyBindings(int direction)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    configureLocked(h);
    std::vector<int> out;
    if (direction >= 0 && direction < hi::kStickDirectionCount)
        for (const hi::Binding &b : h.config.stick[static_cast<size_t>(direction)])
            out.push_back(b.code);
    return out;
}

int PSPadBackend::gamepadButtonFor(int padButton)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    configureLocked(h);
    if (padButton < 0 || padButton >= hi::kPadButtonCount)
        return -1;
    const std::vector<int> &physical = h.config.gamepadButtons[static_cast<size_t>(padButton)];
    return physical.empty() ? -1 : physical.front();
}

bool PSPadBackend::mouseLook()
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    configureLocked(h);
    return h.config.mouseCamera == hi::MouseCamera::Look;
}

bool PSPadBackend::keyPressedWithin(int code, double seconds)
{
    if (code < 0 || code >= hi::kHostCodeCount)
        return false;
    const int64_t at = hub().downAt[static_cast<size_t>(code)].load();
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const bool within = at != 0 && static_cast<double>(now - at) <= seconds * 1000.0;
    return play_recording::read(play_recording::Channel::KeyRecent, within, static_cast<uint32_t>(code));
}

bool PSPadBackend::forcePromptDevice(const std::string &name)
{
    static const char *const kNames[] = {"keyboard", "playstation", "xbox", "nintendo"};
    for (int i = 0; i < 4; ++i)
        if (name == kNames[i])
        {
            g_promptOverride = i;
            setDevice(hub(), static_cast<InputDevice>(i));
            return true;
        }
    if (name == "auto")
    {
        g_promptOverride = -1;
        return true;
    }
    return false;
}

void PSPadBackend::setFreeInput(bool on)
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    if (h.freeInput.exchange(on) == on)
        return;
    h.freeSince = FreeInput{};
    h.freePadBefore = 0xFFFFu; // a button held at the start is not a press
    if (!on)
    {
        h.holdAfterFree = true;
        h.wantCapture.store(h.mapper.captured() && !h.scripted, std::memory_order_release);
    }
    std::fprintf(stderr, "[input] free input %s\n", on ? "on (the game reads an idle pad)" : "off");
}

void PSPadBackend::setOverrideButtons(uint16_t mask)
{
    overrideButtons().store(mask);
}

// ZX_CONTROL padport: the pad taken out of port 0 and put back, as a player
// pulling the cable; the game sees libpad's state go.
void setPadPort0Connected(bool connected)
{
    port0Connected().store(connected);
}

bool PSPadBackend::portConnected(int port)
{
    const bool connected = [port]()
    {
        if (port == 0)
            return port0Connected().load();
        HostInputHub &h = hub();
        std::lock_guard<std::mutex> lock(h.mutex);
        configureLocked(h);
        const hi::PortSource source = hi::routePort(port, h.gamepadCount, h.config.gamepadEnabled, mirrorPort1());
        return source.connected && (source.gamepadSlot != 1 || h.gamepad2.present || source.mirrorPort0);
    }();
    return play_recording::read(play_recording::Channel::PortConnected, connected, static_cast<uint32_t>(port));
}

bool PSPadBackend::freeInput()
{
    return hub().freeInput.load();
}

PSPadBackend::FreeInput PSPadBackend::takeFreeInput()
{
    HostInputHub &h = hub();
    std::lock_guard<std::mutex> lock(h.mutex);
    FreeInput out = h.freeSince;
    FreeInput &f = h.freeSince;
    f.down.reset();
    f.pressed.reset();
    f.mouseDx = f.mouseDy = f.wheel = 0.0f;
    f.padPressed = 0u;
    if (play_recording::active())
    {
        // A play recording: photo mode's input as one value (its key sets
        // as bits, then the rest).
        constexpr size_t kSetBytes = (hi::kHostCodeCount + 7) / 8;
        uint8_t bytes[2 * kSetBytes + 4 * 3 + 1 + 2 * 2 + 4 * 4] = {};
        size_t at = 0;
        for (const hi::HostCodeSet *set : {&out.down, &out.pressed})
        {
            for (size_t i = 0; i < set->size(); ++i)
                if (set->test(i))
                    bytes[at + i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            at += kSetBytes;
        }
        auto put = [&bytes, &at](const void *value, size_t size)
        {
            std::memcpy(bytes + at, value, size);
            at += size;
        };
        const uint8_t focused = out.focused ? 1u : 0u;
        put(&out.mouseDx, 4);
        put(&out.mouseDy, 4);
        put(&out.wheel, 4);
        put(&focused, 1);
        put(&out.padButtons, 2);
        put(&out.padPressed, 2);
        put(out.sticks.data(), 16);
        play_recording::read(play_recording::Channel::FreeInput, 0u, bytes, sizeof(bytes));
        at = 0;
        for (hi::HostCodeSet *set : {&out.down, &out.pressed})
        {
            for (size_t i = 0; i < set->size(); ++i)
                set->set(i, (bytes[at + i / 8] >> (i % 8)) & 1u);
            at += kSetBytes;
        }
        auto get = [&bytes, &at](void *value, size_t size)
        {
            std::memcpy(value, bytes + at, size);
            at += size;
        };
        uint8_t focusedBack = 0;
        get(&out.mouseDx, 4);
        get(&out.mouseDy, 4);
        get(&out.wheel, 4);
        get(&focusedBack, 1);
        out.focused = focusedBack != 0u;
        get(&out.padButtons, 2);
        get(&out.padPressed, 2);
        get(out.sticks.data(), 16);
    }
    return out;
}

void PSPadBackend::setMenuMouse(bool menus)
{
    hub().menuMouse.store(menus);
}

void PSPadBackend::setWheelStick(bool on)
{
    hub().wheelStick.store(on);
}
