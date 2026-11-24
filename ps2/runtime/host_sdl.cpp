#include "host_backend.h"
#include "host_sdl.h"
#include "gs/gs_gpu_texture_check.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include "runtime/frame_trace.h"
#include "runtime/host_window_place.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hi = ps2_host_input;
namespace texcheck = zx::gpu_texture_check;

namespace
{
    // SDL scancode -> ps2_host_input code (the GLFW key numbering, which
    // names physical keys after the US layout, as SDL scancodes do).
    struct KeyTable
    {
        int code[SDL_SCANCODE_COUNT] = {};
        KeyTable()
        {
            for (int i = 0; i < 26; ++i)
                code[SDL_SCANCODE_A + i] = 65 + i;
            for (int i = 0; i < 9; ++i)
                code[SDL_SCANCODE_1 + i] = 49 + i;
            code[SDL_SCANCODE_0] = 48;
            code[SDL_SCANCODE_RETURN] = 257;
            code[SDL_SCANCODE_ESCAPE] = 256;
            code[SDL_SCANCODE_BACKSPACE] = 259;
            code[SDL_SCANCODE_TAB] = 258;
            code[SDL_SCANCODE_SPACE] = 32;
            code[SDL_SCANCODE_MINUS] = 45;
            code[SDL_SCANCODE_EQUALS] = 61;
            code[SDL_SCANCODE_LEFTBRACKET] = 91;
            code[SDL_SCANCODE_RIGHTBRACKET] = 93;
            code[SDL_SCANCODE_BACKSLASH] = 92;
            code[SDL_SCANCODE_SEMICOLON] = 59;
            code[SDL_SCANCODE_APOSTROPHE] = 39;
            code[SDL_SCANCODE_GRAVE] = 96;
            code[SDL_SCANCODE_COMMA] = 44;
            code[SDL_SCANCODE_PERIOD] = 46;
            code[SDL_SCANCODE_SLASH] = 47;
            code[SDL_SCANCODE_CAPSLOCK] = 280;
            for (int i = 0; i < 12; ++i)
                code[SDL_SCANCODE_F1 + i] = 290 + i;
            for (int i = 0; i < 12; ++i)
                code[SDL_SCANCODE_F13 + i] = 302 + i;
            code[SDL_SCANCODE_PRINTSCREEN] = 283;
            code[SDL_SCANCODE_SCROLLLOCK] = 281;
            code[SDL_SCANCODE_PAUSE] = 284;
            code[SDL_SCANCODE_INSERT] = 260;
            code[SDL_SCANCODE_HOME] = 268;
            code[SDL_SCANCODE_PAGEUP] = 266;
            code[SDL_SCANCODE_DELETE] = 261;
            code[SDL_SCANCODE_END] = 269;
            code[SDL_SCANCODE_PAGEDOWN] = 267;
            code[SDL_SCANCODE_RIGHT] = 262;
            code[SDL_SCANCODE_LEFT] = 263;
            code[SDL_SCANCODE_DOWN] = 264;
            code[SDL_SCANCODE_UP] = 265;
            code[SDL_SCANCODE_NUMLOCKCLEAR] = 282;
            code[SDL_SCANCODE_KP_DIVIDE] = 331;
            code[SDL_SCANCODE_KP_MULTIPLY] = 332;
            code[SDL_SCANCODE_KP_MINUS] = 333;
            code[SDL_SCANCODE_KP_PLUS] = 334;
            code[SDL_SCANCODE_KP_ENTER] = 335;
            for (int i = 0; i < 9; ++i)
                code[SDL_SCANCODE_KP_1 + i] = 321 + i;
            code[SDL_SCANCODE_KP_0] = 320;
            code[SDL_SCANCODE_KP_PERIOD] = 330;
            code[SDL_SCANCODE_KP_EQUALS] = 336;
            code[SDL_SCANCODE_APPLICATION] = 348;
            code[SDL_SCANCODE_LCTRL] = 341;
            code[SDL_SCANCODE_LSHIFT] = 340;
            code[SDL_SCANCODE_LALT] = 342;
            code[SDL_SCANCODE_LGUI] = 343;
            code[SDL_SCANCODE_RCTRL] = 345;
            code[SDL_SCANCODE_RSHIFT] = 344;
            code[SDL_SCANCODE_RALT] = 346;
            code[SDL_SCANCODE_RGUI] = 347;
        }
    };
    const KeyTable kKeys;

    // SDL mouse button -> ps2_host_input mouse index (left, right, middle,
    // side, extra as GLFW numbers them).
    int mouseIndex(int sdlButton)
    {
        switch (sdlButton)
        {
        case SDL_BUTTON_LEFT:
            return 0;
        case SDL_BUTTON_RIGHT:
            return 1;
        case SDL_BUTTON_MIDDLE:
            return 2;
        case SDL_BUTTON_X1:
            return 3;
        case SDL_BUTTON_X2:
            return 4;
        default:
            return -1;
        }
    }

    struct OpenPad
    {
        SDL_Gamepad *handle = nullptr;
        SDL_JoystickID id = 0;
        std::string name; // kept for the log line when it goes away
        int playerIndex = -1; // last given to SDL_SetGamepadPlayerIndex
        // What its motors were last told (window thread).
        uint16_t appliedLow = 0;
        uint16_t appliedHigh = 0;
        std::chrono::steady_clock::time_point appliedAt{};
    };

    struct RumbleRequest
    {
        uint16_t low = 0;
        uint16_t high = 0;
        std::chrono::steady_clock::time_point at{};
    };

    struct Host
    {
        bool initialized = false;
        // WindowOptions::headless: no window and no GPU device, gamepads and
        // audio as usual.
        bool headless = false;
        SDL_Window *window = nullptr;
        SDL_GPUDevice *gpu = nullptr;
        SDL_GPUTextureFormat swapFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
        SDL_AudioDeviceID audio = 0;
        std::atomic<bool> quit{false};
        // requestExitToMenu, and whether the app has a menu for it.
        std::atomic<bool> exitToMenu{false};
        std::atomic<bool> menuAvailable{false};
        bool vsync = !std::getenv("ZX_PRESENT_HZ"); // (a timed present rate instead: present())
        bool variableRefresh = false; // the app's setting
        bool interpolatedPresents = false; // setInterpolatedPresents
        bool d3d12 = false;                // the GPU device is SDL's Direct3D 12 (waitForRefresh)
        bool variableRefreshAsked = [] { // ZX_VRR=1
            const char *v = std::getenv("ZX_VRR");
            return v && v[0] == '1';
        }();

        // Presentation.
        // waitForRefresh's command buffer and swapchain picture, for present().
        SDL_GPUCommandBuffer *nextCommands = nullptr;
        SDL_GPUTexture *nextSwapchain = nullptr;
        uint32_t nextWidth = 0, nextHeight = 0;
        bool nextAcquired = false;
        SDL_GPUTexture *frameTexture = nullptr;
        SDL_GPUTransferBuffer *upload = nullptr;
        uint32_t texWidth = 0;
        uint32_t texHeight = 0;
        bool haveFrame = false;
        bool lastFrameOnGpu = false;

        std::mutex deviceHookMutex;
        std::vector<std::pair<int, std::function<void()>>> deviceHooks;
        int nextDeviceHook = 1;

        // Input of the current frame.
        ps2_host::InputFrame input;
        std::mutex snapshotMutex;
        ps2_host::InputSnapshot snapshot;

        // Every connected gamepad (window thread), and which player each is.
        std::vector<OpenPad> pads;
        hi::GamepadSlots slots;
        std::atomic<uint32_t> padGeneration{0};
        std::atomic<bool> padsEnabled{true};

        // Vibration: the guest's latest request per pad port (any thread,
        // under rumbleMutex); what each pad was last told is in OpenPad.
        std::mutex rumbleMutex;
        std::array<RumbleRequest, hi::GamepadSlots::kPlayerSlots> rumble{};
        // Test Rumble (window thread).
        bool testRunning = false;
        std::chrono::steady_clock::time_point testStart{};
        int testPads = 0;
        int testLastStep = -2;
        int testSlot = -1;
        int testMotor = -1;

        ps2_host::EventHook eventHook;
        ps2_host::Overlay overlay;
        // Where present() last put the picture, as fractions of the window
        // (shownPicture).
        SDL_FRect shownPicture{};

        // The map window (ps2_host::openMapWindow): its own renderer and one
        // streaming texture of the picture shown.
        struct MapWindow
        {
            SDL_Window *window = nullptr;
            SDL_Renderer *renderer = nullptr;
            SDL_Texture *texture = nullptr;
            uint32_t texWidth = 0;
            uint32_t texHeight = 0;
            SDL_FRect shown{}; // where the picture is in the window, in points
            bool closed = false;
            std::vector<ps2_host::MapPointerEvent> events;
        } map;

        // The picture over the game's (ps2_host::setOverlayPicture,
        // placeOverlay): its texture, the upload of its newest picture, its
        // place and the pointer it holds.
        struct PictureOverlay
        {
            SDL_GPUTexture *texture = nullptr;
            SDL_GPUTransferBuffer *upload = nullptr;
            uint32_t width = 0;
            uint32_t height = 0;
            bool pending = false; // a picture in the upload, not yet in the texture
            bool ready = false;   // the texture has a picture
            SDL_FRect place{};    // fractions of the window; empty: not drawn
            SDL_MouseButtonFlags buttons = 0; // the mouse's buttons that went down over it, until up
            bool touchMouse = false;          // the mouse events SDL makes of a finger it holds
            std::vector<SDL_FingerID> fingers; // a touch screen's fingers that went down over it
            std::vector<ps2_host::OverlayPointerEvent> events;
        } pictureOverlay;
    };

    Host &host()
    {
        static Host instance;
        return instance;
    }

    ps2_host::GamepadStyle styleOf(SDL_GamepadType type)
    {
        switch (type)
        {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5:
            return ps2_host::GamepadStyle::PlayStation;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            return ps2_host::GamepadStyle::Nintendo;
        default:
            return ps2_host::GamepadStyle::Xbox;
        }
    }

    OpenPad *findPad(Host &h, SDL_JoystickID id)
    {
        for (OpenPad &pad : h.pads)
            if (pad.id == id)
                return &pad;
        return nullptr;
    }

    SDL_Gamepad *padInSlot(Host &h, int slot)
    {
        const OpenPad *pad = findPad(h, static_cast<SDL_JoystickID>(h.slots.idAt(slot)));
        return pad ? pad->handle : nullptr;
    }

    const char *typeText(SDL_Gamepad *pad)
    {
        const char *text = SDL_GetGamepadStringForType(SDL_GetGamepadType(pad));
        return text && text[0] ? text : "unknown";
    }

    // One line per pad for the log (controller problems are reported
    // with it): what SDL made of the device and which player it is.
    void logPad(const OpenPad &pad, int slot, const char *what, bool withMapping)
    {
        SDL_Gamepad *g = pad.handle;
        char guid[64] = {};
        SDL_GUIDToString(SDL_GetGamepadGUIDForID(pad.id), guid, static_cast<int>(sizeof(guid)));
        char *mapping = SDL_GetGamepadMapping(g);
        const SDL_PropertiesID props = SDL_GetGamepadProperties(g);
        const bool rumble = props && SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        const char *link = "unknown link";
        switch (SDL_GetGamepadConnectionState(g))
        {
        case SDL_JOYSTICK_CONNECTION_WIRED:
            link = "wired";
            break;
        case SDL_JOYSTICK_CONNECTION_WIRELESS:
            link = "wireless";
            break;
        default:
            break;
        }
        std::fprintf(stderr,
                     "[pad] gamepad %s: player %d, \"%s\", type %s (%s prompts), vendor 0x%04x product 0x%04x, "
                     "guid %s, mapping %s, rumble %s, %s, SDL player index %d\n",
                     what, slot + 1, pad.name.c_str(), typeText(g),
                     ps2_host::gamepadStyleName(styleOf(SDL_GetGamepadType(g))), SDL_GetGamepadVendor(g),
                     SDL_GetGamepadProduct(g), guid, mapping ? "yes" : "no", rumble ? "yes" : "no", link,
                     SDL_GetGamepadPlayerIndex(g));
        if (withMapping && mapping)
            std::fprintf(stderr, "[pad]   mapping: %s\n", mapping);
        SDL_free(mapping);
    }

    std::string padName(SDL_Gamepad *pad)
    {
        const char *name = SDL_GetGamepadName(pad);
        return name && name[0] ? name : "(unnamed)";
    }

    // Test Rumble runs over the pads it started with: a pad connecting,
    // going away or changing player ends it.
    void stopRumbleTest(Host &h)
    {
        if (!h.testRunning)
            return;
        h.testRunning = false;
        h.testSlot = -1;
        h.testMotor = -1;
        std::fprintf(stderr, "[pad] rumble test: stopped (the gamepads changed)\n");
    }

    // Player lights (Xbox ring, DualSense LEDs, Switch lights) show each
    // pad's slot; a pad that moved to another player is logged.
    void assignPlayers(Host &h)
    {
        for (OpenPad &pad : h.pads)
        {
            const int slot = h.slots.slotOf(pad.id);
            if (slot == pad.playerIndex)
                continue;
            if (pad.playerIndex >= 0)
                std::fprintf(stderr, "[pad] gamepad \"%s\" is now player %d\n", pad.name.c_str(), slot + 1);
            SDL_SetGamepadPlayerIndex(pad.handle, slot);
            pad.playerIndex = slot;
        }
    }

    // Idempotent: SDL reports pads present at start, and a mapping added
    // for a joystick it did not know as a gamepad, as GAMEPAD_ADDED too.
    void openPad(Host &h, SDL_JoystickID id)
    {
        if (findPad(h, id))
            return;
        SDL_Gamepad *handle = SDL_OpenGamepad(id);
        if (!handle)
        {
            std::fprintf(stderr, "[pad] cannot open gamepad %u: %s\n", static_cast<unsigned>(id), SDL_GetError());
            return;
        }
        OpenPad pad;
        pad.handle = handle;
        pad.id = id;
        pad.name = padName(handle);
        h.pads.push_back(pad);
        stopRumbleTest(h);
        const int slot = h.slots.connect(id);
        assignPlayers(h);
        h.padGeneration.fetch_add(1u);
        logPad(h.pads.back(), slot, "connected", false);
    }

    // Removed (or no longer a gamepad after a mapping change): its motors
    // are stopped and forgotten, the other pads may move up a player.
    void closePad(Host &h, SDL_JoystickID id)
    {
        auto it = std::find_if(h.pads.begin(), h.pads.end(), [id](const OpenPad &pad) { return pad.id == id; });
        if (it == h.pads.end())
            return;
        std::fprintf(stderr, "[pad] gamepad removed: player %d, \"%s\"%s\n", h.slots.slotOf(id) + 1, it->name.c_str(),
                     it->appliedLow || it->appliedHigh ? " (its motors were running)" : "");
        SDL_RumbleGamepad(it->handle, 0, 0, 0);
        SDL_CloseGamepad(it->handle);
        h.pads.erase(it);
        stopRumbleTest(h);
        h.slots.disconnect(id);
        assignPlayers(h);
        h.padGeneration.fetch_add(1u);
        if (h.pads.empty())
            std::fprintf(stderr, "[pad] no gamepad connected\n");
    }

    void remapPad(Host &h, SDL_JoystickID id)
    {
        OpenPad *pad = findPad(h, id);
        if (!pad)
            return;
        pad->name = padName(pad->handle);
        h.padGeneration.fetch_add(1u);
        logPad(*pad, h.slots.slotOf(id), "remapped", true);
    }

    void openAllPads(Host &h)
    {
        int count = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&count);
        for (int i = 0; ids && i < count; ++i)
            openPad(h, ids[i]);
        SDL_free(ids);
    }

    bool rumbleEnabled()
    {
        static const bool value = []()
        {
            const char *env = std::getenv("ZX_RUMBLE");
            return !(env && env[0] == '0');
        }();
        return value;
    }

    // Test Rumble: per pad (slot order), the large motor, a gap, the small
    // one, a longer gap.
    constexpr double kTestMotorSeconds = 0.6;
    constexpr double kTestGapSeconds = 0.25;
    constexpr double kTestPadSeconds = 2.0 * kTestMotorSeconds + 3.0 * kTestGapSeconds;

    void updateRumbleTest(Host &h, std::chrono::steady_clock::time_point now)
    {
        if (!h.testRunning)
            return;
        const double t = std::chrono::duration<double>(now - h.testStart).count();
        if (t >= kTestPadSeconds * h.testPads)
        {
            h.testRunning = false;
            h.testSlot = -1;
            h.testMotor = -1;
            std::fprintf(stderr, "[pad] rumble test: done\n");
            return;
        }
        const int slot = static_cast<int>(t / kTestPadSeconds);
        const double inPad = t - slot * kTestPadSeconds;
        int motor = -1;
        if (inPad < kTestMotorSeconds)
            motor = 0;
        else if (inPad >= kTestMotorSeconds + kTestGapSeconds && inPad < 2.0 * kTestMotorSeconds + kTestGapSeconds)
            motor = 1;
        h.testSlot = slot;
        h.testMotor = motor;
        const int step = motor < 0 ? -1 : slot * 2 + motor;
        if (step != h.testLastStep && motor >= 0)
        {
            const OpenPad *pad = findPad(h, static_cast<SDL_JoystickID>(h.slots.idAt(slot)));
            std::fprintf(stderr, "[pad] rumble test: player %d \"%s\", %s motor\n", slot + 1,
                         pad ? pad->name.c_str() : "(gone)", motor == 0 ? "large (low-frequency)" : "small (high-frequency)");
        }
        h.testLastStep = step;
    }

    // The motors the test wants for a slot now (false when no test runs:
    // the guest's requests apply). Window thread, after updateRumbleTest.
    bool rumbleTestFor(const Host &h, int slot, uint16_t &low, uint16_t &high)
    {
        if (!h.testRunning)
            return false;
        const bool here = slot >= 0 && slot == h.testSlot;
        low = here && h.testMotor == 0 ? 0xFFFFu : 0u;
        high = here && h.testMotor == 1 ? 0xFFFFu : 0u;
        return true;
    }

    // Window thread, once per pumpEvents: passes each port's motor request
    // to the pad in its slot. A request older than kRumbleHold counts as
    // off, and a running motor is re-sent (SDL stops it after the given
    // duration), so nothing keeps vibrating when the guest stops updating
    // it. A pad that changed player picks up its new port's request here.
    void applyRumble(Host &h)
    {
        constexpr auto kRumbleHold = std::chrono::milliseconds(250);
        constexpr auto kRumbleRefresh = std::chrono::milliseconds(100);
        constexpr uint32_t kRumbleDurationMs = 300u;
        const auto now = std::chrono::steady_clock::now();
        std::array<RumbleRequest, hi::GamepadSlots::kPlayerSlots> requests;
        {
            std::lock_guard<std::mutex> lock(h.rumbleMutex);
            requests = h.rumble;
        }
        updateRumbleTest(h, now);
        const bool enabled = h.padsEnabled.load();
        for (OpenPad &pad : h.pads)
        {
            const int slot = h.slots.slotOf(pad.id);
            uint16_t low = 0;
            uint16_t high = 0;
            if (!rumbleTestFor(h, slot, low, high) && slot >= 0 && slot < hi::GamepadSlots::kPlayerSlots &&
                now - requests[static_cast<size_t>(slot)].at < kRumbleHold)
            {
                low = requests[static_cast<size_t>(slot)].low;
                high = requests[static_cast<size_t>(slot)].high;
            }
            if (!enabled)
            {
                low = 0;
                high = 0;
            }
            const bool running = low != 0 || high != 0;
            if (low == pad.appliedLow && high == pad.appliedHigh && (!running || now - pad.appliedAt < kRumbleRefresh))
                continue;
            SDL_RumbleGamepad(pad.handle, low, high, running ? kRumbleDurationMs : 0u);
            pad.appliedLow = low;
            pad.appliedHigh = high;
            pad.appliedAt = now;
        }
    }

    float axis(SDL_Gamepad *pad, SDL_GamepadAxis which)
    {
        return std::clamp(static_cast<float>(SDL_GetGamepadAxis(pad, which)) / 32767.0f, -1.0f, 1.0f);
    }

    hi::GamepadState readGamepad(SDL_Gamepad *pad)
    {
        hi::GamepadState state;
        if (!pad)
            return state;
        state.present = true;
        static const struct
        {
            SDL_GamepadButton button;
            int bit;
        } kMap[] = {
            {SDL_GAMEPAD_BUTTON_DPAD_UP, hi::kUp}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, hi::kDown},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, hi::kLeft}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, hi::kRight},
            {SDL_GAMEPAD_BUTTON_SOUTH, hi::kCross}, {SDL_GAMEPAD_BUTTON_EAST, hi::kCircle},
            {SDL_GAMEPAD_BUTTON_WEST, hi::kSquare}, {SDL_GAMEPAD_BUTTON_NORTH, hi::kTriangle},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, hi::kL1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, hi::kR1},
            {SDL_GAMEPAD_BUTTON_START, hi::kStart}, {SDL_GAMEPAD_BUTTON_BACK, hi::kSelect},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, hi::kL3}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, hi::kR3}};
        for (const auto &entry : kMap)
            if (SDL_GetGamepadButton(pad, entry.button))
                state.buttons = static_cast<uint16_t>(state.buttons | (1u << entry.bit));
        // Analog triggers count as L2/R2 past 10 % (as raylib did).
        if (axis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 0.1f)
            state.buttons = static_cast<uint16_t>(state.buttons | (1u << hi::kL2));
        if (axis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 0.1f)
            state.buttons = static_cast<uint16_t>(state.buttons | (1u << hi::kR2));
        state.axes[hi::kLx] = axis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        state.axes[hi::kLy] = axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        state.axes[hi::kRx] = axis(pad, SDL_GAMEPAD_AXIS_RIGHTX);
        state.axes[hi::kRy] = axis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
        return state;
    }

    // ---- ZX_VIRTUAL_PADS: virtual gamepads (developer test only) ---------
    //
    // Off unless the variable is set. SDL virtual joysticks that look like
    // gamepads, for testing hot-plug, several pads, player order, prompts
    // and rumble routing without hardware: they go through the same SDL
    // events and gamepad API as real pads, and their Rumble callback logs
    // the motor values the game would send to a real one ("[vpad]").
    // Comma-separated entries:
    //   2                        two pads at start (xbox, ps)
    //   xbox | ps | switch | generic   one pad of that kind at start
    //   <t>:add[=<kind>]         attach one at t seconds (default xbox)
    //   <t>:remove=<n>           detach virtual pad n (1 = the first attached)
    //   <t>:<n>:<control>[=<v>][:<dur>]  press a control of pad n for dur
    //                            seconds (default 0.25): south, east, west,
    //                            north, back, start, l1, r1, l3, r3, up,
    //                            down, left, right; lx, ly, rx, ry (v in
    //                            -1..1), l2, r2 (v in 0..1); v defaults to 1
    // Times are seconds from the window's creation.
    // ZX_VIRTUAL_PADS_FILE=<path>: lines appended to that file later run
    // the same entries, timed from when they are read (0 = at once), so a
    // test can act when the game is ready ("0:2:ly=-1:3").
    struct VirtualPad
    {
        int number = 0;
        std::string kind;
        SDL_JoystickID id = 0;
        SDL_Joystick *joystick = nullptr;
        uint16_t low = 0;
        uint16_t high = 0;
        int player = -1;
    };

    struct VirtualEvent
    {
        double time = 0.0;
        enum Kind
        {
            Add,
            Remove,
            Button,
            Axis
        } kind = Add;
        int pad = 0; // 1-based
        int index = 0; // SDL_GamepadButton or SDL_GamepadAxis
        float value = 1.0f;
        double duration = 0.25;
        std::string padKind = "xbox";
    };

    struct VirtualRelease
    {
        double time = 0.0;
        int pad = 0;
        bool axis = false;
        int index = 0;
    };

    struct VirtualPads
    {
        bool active = false;
        Uint64 startMs = 0;
        std::vector<VirtualEvent> events; // not run yet, by time
        std::string file; // ZX_VIRTUAL_PADS_FILE
        std::streamoff fileRead = 0;
        double fileCheckedAt = -1.0;
        std::vector<VirtualRelease> releases;
        std::vector<std::unique_ptr<VirtualPad>> pads; // stable addresses (callback userdata)
    };

    VirtualPads &virtualPads()
    {
        static VirtualPads instance;
        return instance;
    }

    bool SDLCALL virtualRumble(void *userdata, Uint16 low, Uint16 high)
    {
        auto *pad = static_cast<VirtualPad *>(userdata);
        if (pad->low != low || pad->high != high)
            std::fprintf(stderr, "[vpad] pad %d (%s, player %d) rumble low=%u high=%u\n", pad->number, pad->kind.c_str(),
                         pad->player + 1, low, high);
        pad->low = low;
        pad->high = high;
        return true;
    }

    void SDLCALL virtualPlayerIndex(void *userdata, int index)
    {
        auto *pad = static_cast<VirtualPad *>(userdata);
        pad->player = index;
        std::fprintf(stderr, "[vpad] pad %d player lights: %d\n", pad->number, index + 1);
    }

    void attachVirtualPad(VirtualPads &v, const std::string &kind)
    {
        auto pad = std::make_unique<VirtualPad>();
        pad->number = static_cast<int>(v.pads.size()) + 1;
        pad->kind = kind;
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1; // SOUTH..DPAD_RIGHT, in SDL_GamepadButton order
        std::string name = "Virtual Gamepad";
        if (kind == "ps")
        {
            desc.vendor_id = 0x054C; // DualShock 4
            desc.product_id = 0x09CC;
            name = "Virtual PlayStation Pad";
        }
        else if (kind == "switch")
        {
            desc.vendor_id = 0x057E; // Switch Pro Controller
            desc.product_id = 0x2009;
            name = "Virtual Switch Pad";
        }
        else if (kind == "xbox")
        {
            desc.vendor_id = 0x045E; // Xbox 360 Controller
            desc.product_id = 0x028E;
            name = "Virtual Xbox Pad";
        }
        name += " " + std::to_string(pad->number);
        desc.name = name.c_str();
        desc.userdata = pad.get();
        desc.Rumble = virtualRumble;
        desc.SetPlayerIndex = virtualPlayerIndex;
        pad->id = SDL_AttachVirtualJoystick(&desc);
        if (!pad->id)
        {
            std::fprintf(stderr, "[vpad] cannot attach a virtual pad: %s\n", SDL_GetError());
            return;
        }
        pad->joystick = SDL_OpenJoystick(pad->id);
        std::fprintf(stderr, "[vpad] attached pad %d (%s, joystick %u)\n", pad->number, kind.c_str(),
                     static_cast<unsigned>(pad->id));
        v.pads.push_back(std::move(pad));
    }

    VirtualPad *virtualPad(VirtualPads &v, int number)
    {
        if (number < 1 || number > static_cast<int>(v.pads.size()))
            return nullptr;
        VirtualPad *pad = v.pads[static_cast<size_t>(number - 1)].get();
        return pad->joystick ? pad : nullptr;
    }

    Sint16 virtualAxisValue(int axis, float value)
    {
        if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
            return static_cast<Sint16>(std::lround(-32768.0f + std::clamp(value, 0.0f, 1.0f) * 65535.0f));
        return static_cast<Sint16>(std::lround(std::clamp(value, -1.0f, 1.0f) * 32767.0f));
    }

    void parseVirtualPads(VirtualPads &v, const std::string &spec, double offset = 0.0)
    {
        static const struct
        {
            const char *name;
            bool axis;
            int index;
        } kControls[] = {
            {"south", false, SDL_GAMEPAD_BUTTON_SOUTH}, {"east", false, SDL_GAMEPAD_BUTTON_EAST},
            {"west", false, SDL_GAMEPAD_BUTTON_WEST}, {"north", false, SDL_GAMEPAD_BUTTON_NORTH},
            {"back", false, SDL_GAMEPAD_BUTTON_BACK}, {"start", false, SDL_GAMEPAD_BUTTON_START},
            {"l1", false, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {"r1", false, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
            {"l3", false, SDL_GAMEPAD_BUTTON_LEFT_STICK}, {"r3", false, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
            {"up", false, SDL_GAMEPAD_BUTTON_DPAD_UP}, {"down", false, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
            {"left", false, SDL_GAMEPAD_BUTTON_DPAD_LEFT}, {"right", false, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
            {"lx", true, SDL_GAMEPAD_AXIS_LEFTX}, {"ly", true, SDL_GAMEPAD_AXIS_LEFTY},
            {"rx", true, SDL_GAMEPAD_AXIS_RIGHTX}, {"ry", true, SDL_GAMEPAD_AXIS_RIGHTY},
            {"l2", true, SDL_GAMEPAD_AXIS_LEFT_TRIGGER}, {"r2", true, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER}};
        auto split = [](const std::string &text, char sep)
        {
            std::vector<std::string> parts;
            size_t start = 0;
            while (true)
            {
                const size_t end = text.find(sep, start);
                parts.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
                if (end == std::string::npos)
                    return parts;
                start = end + 1;
            }
        };
        auto isKind = [](const std::string &k) { return k == "xbox" || k == "ps" || k == "switch" || k == "generic"; };
        for (const std::string &entry : split(spec, ','))
        {
            if (entry.empty())
                continue;
            const std::vector<std::string> f = split(entry, ':');
            VirtualEvent e;
            bool ok = true;
            if (f.size() == 1)
            {
                // At start: a count or a kind.
                if (isKind(f[0]))
                {
                    e.padKind = f[0];
                    v.events.push_back(e);
                }
                else if (const int n = std::atoi(f[0].c_str()); n > 0)
                {
                    for (int i = 0; i < n && i < 8; ++i)
                    {
                        e.padKind = i % 2 == 0 ? "xbox" : "ps";
                        v.events.push_back(e);
                    }
                }
                else
                {
                    ok = false;
                }
            }
            else
            {
                e.time = offset + std::atof(f[0].c_str());
                if (f[1].rfind("add", 0) == 0)
                {
                    e.kind = VirtualEvent::Add;
                    if (f[1].size() > 4 && f[1][3] == '=')
                        e.padKind = f[1].substr(4);
                    ok = isKind(e.padKind);
                }
                else if (f[1].rfind("remove=", 0) == 0)
                {
                    e.kind = VirtualEvent::Remove;
                    e.pad = std::atoi(f[1].c_str() + 7);
                }
                else if (f.size() >= 3)
                {
                    e.pad = std::atoi(f[1].c_str());
                    const std::vector<std::string> control = split(f[2], '=');
                    ok = false;
                    for (const auto &c : kControls)
                        if (control[0] == c.name)
                        {
                            ok = true;
                            e.kind = c.axis ? VirtualEvent::Axis : VirtualEvent::Button;
                            e.index = c.index;
                        }
                    if (control.size() > 1)
                        e.value = static_cast<float>(std::atof(control[1].c_str()));
                    if (f.size() >= 4)
                        e.duration = std::atof(f[3].c_str());
                }
                else
                {
                    ok = false;
                }
            }
            if (!ok)
                std::fprintf(stderr, "[vpad] ZX_VIRTUAL_PADS: ignoring '%s'\n", entry.c_str());
            else if (f.size() > 1)
                v.events.push_back(e);
        }
        std::stable_sort(v.events.begin(), v.events.end(),
                         [](const VirtualEvent &a, const VirtualEvent &b) { return a.time < b.time; });
    }

    // Window thread, before the events of a frame are polled.
    void updateVirtualPads()
    {
        VirtualPads &v = virtualPads();
        if (!v.active)
            return;
        const double now = static_cast<double>(SDL_GetTicks() - v.startMs) / 1000.0;
        if (!v.file.empty() && now - v.fileCheckedAt >= 0.25)
        {
            v.fileCheckedAt = now;
            std::ifstream in(v.file, std::ios::binary);
            in.seekg(0, std::ios::end);
            const std::streamoff size = in ? static_cast<std::streamoff>(in.tellg()) : 0;
            if (size > v.fileRead)
            {
                in.seekg(v.fileRead);
                std::string text(static_cast<size_t>(size - v.fileRead), '\0');
                in.read(text.data(), static_cast<std::streamsize>(text.size()));
                const size_t complete = text.rfind('\n');
                if (complete != std::string::npos)
                {
                    v.fileRead += static_cast<std::streamoff>(complete + 1);
                    std::string entries = text.substr(0, complete);
                    std::replace(entries.begin(), entries.end(), '\n', ',');
                    std::replace(entries.begin(), entries.end(), '\r', ',');
                    std::fprintf(stderr, "[vpad] from %s: %s\n", v.file.c_str(), entries.c_str());
                    parseVirtualPads(v, entries, now);
                }
            }
        }
        for (auto it = v.releases.begin(); it != v.releases.end();)
        {
            if (it->time > now)
            {
                ++it;
                continue;
            }
            if (VirtualPad *pad = virtualPad(v, it->pad))
            {
                if (it->axis)
                    SDL_SetJoystickVirtualAxis(pad->joystick, it->index, virtualAxisValue(it->index, 0.0f));
                else
                    SDL_SetJoystickVirtualButton(pad->joystick, it->index, false);
            }
            it = v.releases.erase(it);
        }
        while (!v.events.empty() && v.events.front().time <= now)
        {
            const VirtualEvent e = v.events.front();
            v.events.erase(v.events.begin());
            switch (e.kind)
            {
            case VirtualEvent::Add:
                attachVirtualPad(v, e.padKind);
                break;
            case VirtualEvent::Remove:
                if (VirtualPad *pad = virtualPad(v, e.pad))
                {
                    std::fprintf(stderr, "[vpad] detaching pad %d\n", pad->number);
                    SDL_CloseJoystick(pad->joystick);
                    pad->joystick = nullptr;
                    SDL_DetachVirtualJoystick(pad->id);
                }
                break;
            case VirtualEvent::Button:
            case VirtualEvent::Axis:
                if (VirtualPad *pad = virtualPad(v, e.pad))
                {
                    const bool axis = e.kind == VirtualEvent::Axis;
                    if (axis)
                        SDL_SetJoystickVirtualAxis(pad->joystick, e.index, virtualAxisValue(e.index, e.value));
                    else
                        SDL_SetJoystickVirtualButton(pad->joystick, e.index, true);
                    v.releases.push_back({e.time + e.duration, e.pad, axis, e.index});
                }
                break;
            }
        }
    }

    void startVirtualPads()
    {
        const char *spec = std::getenv("ZX_VIRTUAL_PADS");
        const char *file = std::getenv("ZX_VIRTUAL_PADS_FILE");
        if ((!spec || !spec[0]) && (!file || !file[0]))
            return;
        VirtualPads &v = virtualPads();
        v.active = true;
        v.startMs = SDL_GetTicks();
        if (file && file[0])
        {
            // Only what is appended from now on.
            v.file = file;
            std::ifstream in(v.file, std::ios::binary | std::ios::ate);
            v.fileRead = in ? static_cast<std::streamoff>(in.tellg()) : 0;
        }
        if (spec)
            parseVirtualPads(v, spec);
        std::fprintf(stderr, "[vpad] ZX_VIRTUAL_PADS: %zu events (developer test pads)\n", v.events.size());
        updateVirtualPads();
    }

    void stopVirtualPads()
    {
        VirtualPads &v = virtualPads();
        for (auto &pad : v.pads)
            if (pad->joystick)
            {
                SDL_CloseJoystick(pad->joystick);
                pad->joystick = nullptr;
                SDL_DetachVirtualJoystick(pad->id);
            }
        v.active = false;
    }

    void setCode(hi::HostCodeSet &set, int code)
    {
        if (code > 0 && code < hi::kHostCodeCount)
            set.set(static_cast<size_t>(code));
    }

    bool ensureFrameTexture(Host &h, uint32_t width, uint32_t height)
    {
        if (h.frameTexture && h.texWidth == width && h.texHeight == height)
            return true;
        if (h.frameTexture)
            texcheck::releaseTexture(h.gpu, h.frameTexture);
        if (h.upload)
            SDL_ReleaseGPUTransferBuffer(h.gpu, h.upload);
        h.frameTexture = nullptr;
        h.upload = nullptr;
        h.haveFrame = false;

        SDL_GPUTextureCreateInfo texInfo{};
        texInfo.type = SDL_GPU_TEXTURETYPE_2D;
        texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texInfo.width = width;
        texInfo.height = height;
        texInfo.layer_count_or_depth = 1;
        texInfo.num_levels = 1;
        h.frameTexture = texcheck::createTexture(h.gpu, &texInfo);
        SDL_GPUTransferBufferCreateInfo bufInfo{};
        bufInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        bufInfo.size = width * height * 4u;
        h.upload = SDL_CreateGPUTransferBuffer(h.gpu, &bufInfo);
        if (!h.frameTexture || !h.upload)
        {
            std::fprintf(stderr, "[host] cannot create the frame texture: %s\n", SDL_GetError());
            return false;
        }
        h.texWidth = width;
        h.texHeight = height;
        return true;
    }

    // ZX_FRAMES_IN_FLIGHT=1..3 (developers): the window's presented pictures the GPU may be behind on; 0: not
    // set (ps2_host::setInterpolatedPresents).
    uint32_t framesInFlightAsked()
    {
        static const uint32_t n = []() -> uint32_t
        {
            const char *v = std::getenv("ZX_FRAMES_IN_FLIGHT");
            return v && v[0] >= '1' && v[0] <= '3' && !v[1] ? static_cast<uint32_t>(v[0] - '0') : 0u;
        }();
        return n;
    }

    bool createWindow(Host &h, const char *title, const ps2_host::WindowOptions &options)
    {
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN;
#if defined(__ANDROID__)
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        flags |= SDL_WINDOW_FULLSCREEN;
#endif
        if (options.highDpi)
            flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
        h.window = SDL_CreateWindow(title, options.width, options.height, flags);
        if (!h.window)
        {
            std::fprintf(stderr, "[host] cannot create the window: %s\n", SDL_GetError());
            return false;
        }
        h.gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL |
                                        SDL_GPU_SHADERFORMAT_METALLIB,
                                    false, nullptr);
        if (!h.gpu || !SDL_ClaimWindowForGPUDevice(h.gpu, h.window))
        {
            std::fprintf(stderr, "[host] no GPU device for the window: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetGPUSwapchainParameters(h.gpu, h.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);
        if (const char *driver = SDL_GetGPUDeviceDriver(h.gpu))
            h.d3d12 = std::strcmp(driver, "direct3d12") == 0;
        if (const uint32_t n = framesInFlightAsked())
            SDL_SetGPUAllowedFramesInFlight(h.gpu, n);
        if (!h.vsync)
            ps2_host::setVsync(false);
        h.swapFormat = SDL_GetGPUSwapchainTextureFormat(h.gpu, h.window);
        SDL_ShowWindow(h.window);
        std::fprintf(stderr, "[host] SDL %d.%d.%d, video %s, GPU %s\n", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                     SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
                     SDL_GetCurrentVideoDriver(), SDL_GetGPUDeviceDriver(h.gpu));
        return true;
    }

    // ZX_AUDIO=off: the audio device is SDL's dummy driver, which takes the sound at a device's pace and plays it
    // nowhere, so test runs never reach the speakers. Everything before the device is as with sound: the SPU2 mix,
    // the host streams and their rate (the game's logic never reads the device). An unpaced run (ZX_UNPACED=1)
    // cannot play sound in time and gets it too; ZX_AUDIO=on plays sound whatever the pacing.
    const char *audioOffReason()
    {
        const char *audio = std::getenv("ZX_AUDIO");
        if (audio && std::strcmp(audio, "off") == 0)
            return "ZX_AUDIO=off";
        if (audio && std::strcmp(audio, "on") == 0)
            return nullptr;
        const char *unpaced = std::getenv("ZX_UNPACED");
        if (unpaced && unpaced[0] == '1')
            return "ZX_UNPACED=1";
        return nullptr;
    }
}

namespace ps2_host
{
    bool initialize(const char *title, const WindowOptions &options)
    {
        Host &h = host();
        if (h.initialized)
            return true;
        // The gamepad keeps working while another window has the focus.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef SDL_PLATFORM_MACOS
        // On the Mac SDL's GameController (MFi) backend claims every HID gamepad Apple's framework says it supports,
        // so SDL's own IOKit and HIDAPI drivers leave it alone, and then never reports some of them: an 8BitDo
        // Ultimate wireless for PC (2.4G dongle, 0x2dc8:0x3013) never appeared at all. SDL's own drivers know the
        // usual pads (Xbox, PlayStation, Switch, 8BitDo) and give them their mappings. SDL_JOYSTICK_MFI=1 in the
        // environment turns the GameController backend back on.
        SDL_SetHint(SDL_HINT_JOYSTICK_MFI, "0");
#endif
        SDL_SetAppMetadata(title, nullptr, nullptr);
        if (!SDL_Init(options.headless ? SDL_INIT_GAMEPAD : SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
        {
            std::fprintf(stderr, "[host] SDL_Init failed: %s\n", SDL_GetError());
            return false;
        }
        h.headless = options.headless;
        if (h.headless)
            std::fprintf(stderr, "[host] headless: no window, no GPU device\n");
        else if (!createWindow(h, title, options))
            return false;

        const char *audioOff = audioOffReason();
        if (audioOff)
            SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
        else
            SDL_ResetHint(SDL_HINT_AUDIO_DRIVER); // (a muted host before this one in the process)
        if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        {
            h.audio = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
            if (!h.audio)
                std::fprintf(stderr, "[host] no audio device: %s\n", SDL_GetError());
            else if (audioOff)
                std::fprintf(stderr, "[host] audio: SDL's %s driver (%s), no sound reaches a speaker\n",
                             SDL_GetCurrentAudioDriver(), audioOff);
            else
                std::fprintf(stderr, "[host] audio: SDL's %s driver\n", SDL_GetCurrentAudioDriver());
        }
        else
        {
            std::fprintf(stderr, "[host] audio unavailable: %s\n", SDL_GetError());
        }
        // Extra controller mappings shipped next to the executable (in the
        // app: Contents/Resources); the user's own one, next to
        // controls.ini, is loaded by the pad backend later and wins.
        if (const char *base = SDL_GetBasePath())
            loadGamepadMappings(std::string(base) + "gamecontrollerdb.txt");
        startVirtualPads();
        openAllPads(h);
        h.initialized = true;
        return true;
    }

    void shutdown()
    {
        Host &h = host();
        if (!h.initialized)
            return;
        closeMapWindow();
        if (h.audio)
            SDL_CloseAudioDevice(h.audio);
        h.audio = 0;
        for (OpenPad &pad : h.pads)
        {
            SDL_RumbleGamepad(pad.handle, 0, 0, 0);
            SDL_CloseGamepad(pad.handle);
        }
        h.pads.clear();
        h.slots.clear();
        stopVirtualPads();
        if (h.gpu)
        {
            std::vector<std::pair<int, std::function<void()>>> hooks;
            {
                std::lock_guard<std::mutex> lock(h.deviceHookMutex);
                hooks.swap(h.deviceHooks);
            }
            for (auto &hook : hooks)
                hook.second();
            SDL_WaitForGPUIdle(h.gpu);
            if (h.frameTexture)
                texcheck::releaseTexture(h.gpu, h.frameTexture);
            if (h.upload)
                SDL_ReleaseGPUTransferBuffer(h.gpu, h.upload);
            h.frameTexture = nullptr;
            h.upload = nullptr;
            if (h.pictureOverlay.texture)
                texcheck::releaseTexture(h.gpu, h.pictureOverlay.texture);
            if (h.pictureOverlay.upload)
                SDL_ReleaseGPUTransferBuffer(h.gpu, h.pictureOverlay.upload);
            h.pictureOverlay = {};
            if (h.window)
                SDL_ReleaseWindowFromGPUDevice(h.gpu, h.window);
            texcheck::releaseHeld(h.gpu);
            SDL_DestroyGPUDevice(h.gpu);
            h.gpu = nullptr;
        }
        if (h.window)
            SDL_DestroyWindow(h.window);
        h.window = nullptr;
        SDL_Quit();
        h.initialized = false;
    }

    bool initialized() { return host().initialized; }

    bool windowReady()
    {
        Host &h = host();
        return h.headless || h.window != nullptr;
    }

    namespace
    {
        // An event of the map window: its pointer goes to its own list, in
        // the shown picture's pixels; its close button closes it; the rest
        // of its events are nobody else's. False: not that window's.
        bool mapWindowEvent(Host &h, const SDL_Event &event)
        {
            if (!h.map.window)
                return false;
            const SDL_WindowID id = SDL_GetWindowID(h.map.window);
            auto pointer = [&h](float x, float y, MapPointerEvent::Kind kind, float wheel)
            {
                MapPointerEvent e;
                e.kind = kind;
                e.x = (x - h.map.shown.x) / std::max(1.0f, h.map.shown.w) * static_cast<float>(h.map.texWidth);
                e.y = (y - h.map.shown.y) / std::max(1.0f, h.map.shown.h) * static_cast<float>(h.map.texHeight);
                e.wheel = wheel;
                h.map.events.push_back(e);
            };
            switch (event.type)
            {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (event.window.windowID != id)
                    return false;
                h.map.closed = true;
                closeMapWindow();
                return true;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.windowID != id)
                    return false;
                if (event.button.button == SDL_BUTTON_LEFT)
                    pointer(event.button.x, event.button.y,
                            event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? MapPointerEvent::Down : MapPointerEvent::Up, 0.0f);
                return true;
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.windowID != id)
                    return false;
                if ((event.motion.state & SDL_BUTTON_LMASK) != 0)
                    pointer(event.motion.x, event.motion.y, MapPointerEvent::Move, 0.0f);
                return true;
            case SDL_EVENT_MOUSE_WHEEL:
                if (event.wheel.windowID != id)
                    return false;
                pointer(event.wheel.mouse_x, event.wheel.mouse_y, MapPointerEvent::Wheel,
                        event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y);
                return true;
            default:
                return event.type >= SDL_EVENT_WINDOW_FIRST && event.type <= SDL_EVENT_WINDOW_LAST &&
                       event.window.windowID == id;
            }
        }

        // An event of the pointer over the picture over the game's
        // (placeOverlay): its own list, in the picture's pixels, and not the
        // game's. False: the game's.
        bool pictureOverlayEvent(Host &h, const SDL_Event &event)
        {
            Host::PictureOverlay &o = h.pictureOverlay;
            if (!h.window || o.place.w <= 0.0f || o.place.h <= 0.0f)
                return false;
            int windowWidth = 0, windowHeight = 0;
            SDL_GetWindowSize(h.window, &windowWidth, &windowHeight);
            const SDL_FRect area{o.place.x * windowWidth, o.place.y * windowHeight, o.place.w * windowWidth,
                                 o.place.h * windowHeight};
            const bool cursorFree = !SDL_GetWindowRelativeMouseMode(h.window); // (a captured cursor has no place)
            auto inside = [&area](float x, float y)
            { return x >= area.x && y >= area.y && x < area.x + area.w && y < area.y + area.h; };
            auto add = [&](OverlayPointerEvent::Kind kind, int pointer, float x, float y, float wheel)
            {
                OverlayPointerEvent e;
                e.kind = kind;
                e.pointer = pointer;
                e.x = (x - area.x) / std::max(1.0f, area.w) * static_cast<float>(o.width);
                e.y = (y - area.y) / std::max(1.0f, area.h) * static_cast<float>(o.height);
                e.wheel = wheel;
                o.events.push_back(e);
            };
            const SDL_WindowID id = SDL_GetWindowID(h.window);
            switch (event.type)
            {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.windowID != id)
                    return false;
                if (event.button.which == SDL_TOUCH_MOUSEID) // (a finger's: its own events below)
                {
                    o.touchMouse = o.touchMouse || inside(event.button.x, event.button.y);
                    return o.touchMouse;
                }
                if (!cursorFree || !inside(event.button.x, event.button.y))
                    return false;
                o.buttons |= SDL_BUTTON_MASK(event.button.button);
                if (event.button.button == SDL_BUTTON_LEFT)
                    add(OverlayPointerEvent::Down, 0, event.button.x, event.button.y, 0.0f);
                return true;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.windowID != id)
                    return false;
                if (event.button.which == SDL_TOUCH_MOUSEID)
                {
                    const bool held = o.touchMouse;
                    o.touchMouse = false;
                    return held;
                }
                if ((o.buttons & SDL_BUTTON_MASK(event.button.button)) == 0)
                    return false;
                o.buttons &= ~SDL_BUTTON_MASK(event.button.button);
                if (event.button.button == SDL_BUTTON_LEFT)
                    add(OverlayPointerEvent::Up, 0, event.button.x, event.button.y, 0.0f);
                return true;
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.windowID != id)
                    return false;
                if (event.motion.which == SDL_TOUCH_MOUSEID)
                    return o.touchMouse;
                if ((o.buttons & SDL_BUTTON_LMASK) != 0)
                    add(OverlayPointerEvent::Move, 0, event.motion.x, event.motion.y, 0.0f);
                return o.buttons != 0;
            case SDL_EVENT_MOUSE_WHEEL:
                if (event.wheel.windowID != id || !cursorFree || !inside(event.wheel.mouse_x, event.wheel.mouse_y))
                    return false;
                add(OverlayPointerEvent::Wheel, 0, event.wheel.mouse_x, event.wheel.mouse_y,
                    event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y);
                return true;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_CANCELED:
            {
                // A touch screen's (a Mac trackpad's fingers are the mouse), in fractions of the window.
                if (event.tfinger.windowID != id || SDL_GetTouchDeviceType(event.tfinger.touchID) != SDL_TOUCH_DEVICE_DIRECT)
                    return false;
                const float x = event.tfinger.x * windowWidth, y = event.tfinger.y * windowHeight;
                const int pointer = 1 + static_cast<int>(event.tfinger.fingerID & 0x7FFFFFFF);
                const auto held = std::find(o.fingers.begin(), o.fingers.end(), event.tfinger.fingerID);
                if (event.type == SDL_EVENT_FINGER_DOWN)
                {
                    if (held != o.fingers.end() || !inside(x, y))
                        return false;
                    o.fingers.push_back(event.tfinger.fingerID);
                    add(OverlayPointerEvent::Down, pointer, x, y, 0.0f);
                    return true;
                }
                if (held == o.fingers.end())
                    return false;
                if (event.type == SDL_EVENT_FINGER_MOTION)
                {
                    add(OverlayPointerEvent::Move, pointer, x, y, 0.0f);
                    return true;
                }
                o.fingers.erase(held);
                add(OverlayPointerEvent::Up, pointer, x, y, 0.0f);
                return true;
            }
            default:
                return false;
            }
        }
    }

    void pumpEvents()
    {
        Host &h = host();
        if (!h.initialized)
            return;
        h.input.pressed.reset();
        h.input.mouseDx = 0.0f;
        h.input.mouseDy = 0.0f;
        h.input.wheel = 0.0f;
        h.input.mouseMoved = false;
        h.input.dropped.clear();
        h.input.touched = false;
        updateVirtualPads();
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (h.eventHook)
                h.eventHook(event);
            if (mapWindowEvent(h, event))
                continue;
            if (pictureOverlayEvent(h, event))
                continue;
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                h.quit = true;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!event.key.repeat && event.key.scancode < SDL_SCANCODE_COUNT)
                    setCode(h.input.pressed, kKeys.code[event.key.scancode]);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (const int index = mouseIndex(event.button.button); index >= 0)
                    setCode(h.input.pressed, hi::kMouseCodeBase + index);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                h.input.mouseDx += event.motion.xrel;
                h.input.mouseDy += event.motion.yrel;
                h.input.mouseMoved = true;
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_MOTION:
                // A touch screen's (a Mac trackpad's fingers are the mouse).
                if (SDL_GetTouchDeviceType(event.tfinger.touchID) == SDL_TOUCH_DEVICE_DIRECT)
                    h.input.touched = true;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                h.input.wheel += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                openPad(h, event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                closePad(h, event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMAPPED:
                remapPad(h, event.gdevice.which);
                break;
            case SDL_EVENT_DROP_FILE:
                if (event.drop.data)
                    h.input.dropped.emplace_back(event.drop.data);
                break;
            default:
                break;
            }
        }

        hi::HostCodeSet held;
        if (h.window)
        {
            int count = 0;
            const bool *keys = SDL_GetKeyboardState(&count);
            for (int sc = 0; keys && sc < count && sc < SDL_SCANCODE_COUNT; ++sc)
                if (keys[sc])
                    setCode(held, kKeys.code[sc]);
            // (less the buttons pressed over the picture over the game's: its own until released)
            const SDL_MouseButtonFlags buttons =
                SDL_GetMouseState(&h.input.mouseX, &h.input.mouseY) & ~h.pictureOverlay.buttons;
            for (int b = SDL_BUTTON_LEFT; b <= SDL_BUTTON_X2; ++b)
                if ((buttons & SDL_BUTTON_MASK(b)) != 0)
                    if (const int index = mouseIndex(b); index >= 0)
                        setCode(held, hi::kMouseCodeBase + index);
        }
        h.input.held = held;
        h.input.focused = windowFocused();

        applyRumble(h);

        std::lock_guard<std::mutex> lock(h.snapshotMutex);
        h.snapshot.held = held;
        h.snapshot.gamepad = readGamepad(padInSlot(h, 0));
    }

    void setRumble(int port, uint16_t low, uint16_t high)
    {
        if (!rumbleEnabled() || port < 0 || port >= hi::GamepadSlots::kPlayerSlots)
            return;
        Host &h = host();
        std::lock_guard<std::mutex> lock(h.rumbleMutex);
        RumbleRequest &request = h.rumble[static_cast<size_t>(port)];
        request.low = low;
        request.high = high;
        request.at = std::chrono::steady_clock::now();
    }

    void startRumbleTest()
    {
        Host &h = host();
        if (!rumbleEnabled())
        {
            std::fprintf(stderr, "[pad] rumble test: vibration is off (ZX_RUMBLE=0)\n");
            return;
        }
        if (!h.padsEnabled.load())
        {
            std::fprintf(stderr, "[pad] rumble test: gamepads are off ([gamepad] enabled = false)\n");
            return;
        }
        if (h.pads.empty())
        {
            std::fprintf(stderr, "[pad] rumble test: no gamepad connected\n");
            return;
        }
        h.testRunning = true;
        h.testStart = std::chrono::steady_clock::now();
        h.testPads = h.slots.count();
        h.testLastStep = -2;
        std::fprintf(stderr, "[pad] rumble test: %d gamepad%s, each large motor then small motor\n", h.testPads,
                     h.testPads == 1 ? "" : "s");
    }

    const char *gamepadStyleName(GamepadStyle style)
    {
        switch (style)
        {
        case GamepadStyle::PlayStation:
            return "PlayStation";
        case GamepadStyle::Xbox:
            return "Xbox";
        case GamepadStyle::Nintendo:
            return "Nintendo";
        default:
            return "None";
        }
    }

    std::vector<GamepadInfo> gamepads()
    {
        Host &h = host();
        std::vector<GamepadInfo> out;
        for (int slot = 0; slot < h.slots.count(); ++slot)
        {
            const OpenPad *pad = findPad(h, static_cast<SDL_JoystickID>(h.slots.idAt(slot)));
            if (!pad)
                continue;
            GamepadInfo info;
            info.slot = slot;
            info.id = pad->id;
            info.name = pad->name;
            info.type = typeText(pad->handle);
            info.style = styleOf(SDL_GetGamepadType(pad->handle));
            const SDL_PropertiesID props = SDL_GetGamepadProperties(pad->handle);
            info.rumble = props && SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
            char *mapping = SDL_GetGamepadMapping(pad->handle);
            info.mapped = mapping != nullptr;
            SDL_free(mapping);
            out.push_back(std::move(info));
        }
        return out;
    }

    int gamepadCount()
    {
        return static_cast<int>(host().pads.size());
    }

    uint32_t gamepadGeneration()
    {
        return host().padGeneration.load();
    }

    hi::GamepadState gamepadState(int slot)
    {
        return readGamepad(padInSlot(host(), slot));
    }

    void swapGamepadPlayers()
    {
        Host &h = host();
        if (h.slots.count() < 2)
            return;
        stopRumbleTest(h);
        h.slots.swapPlayers();
        assignPlayers(h);
        h.padGeneration.fetch_add(1u);
    }

    void setGamepadsEnabled(bool enabled)
    {
        Host &h = host();
        if (h.padsEnabled.exchange(enabled) != enabled)
            h.padGeneration.fetch_add(1u);
    }

    int loadGamepadMappings(const std::string &path)
    {
        SDL_PathInfo info;
        if (path.empty() || !SDL_GetPathInfo(path.c_str(), &info) || info.type != SDL_PATHTYPE_FILE)
            return -1;
        const int added = SDL_AddGamepadMappingsFromFile(path.c_str());
        if (added < 0)
            std::fprintf(stderr, "[pad] cannot read gamepad mappings from %s: %s\n", path.c_str(), SDL_GetError());
        else
            std::fprintf(stderr, "[pad] gamepad mappings from %s: %d new\n", path.c_str(), added);
        return added;
    }

    void logGamepads()
    {
        Host &h = host();
        if (h.pads.empty())
        {
            std::fprintf(stderr, "[pad] no gamepad connected%s\n", h.padsEnabled.load() ? "" : " (gamepads off)");
            return;
        }
        std::fprintf(stderr, "[pad] %zu gamepad%s connected%s\n", h.pads.size(), h.pads.size() == 1 ? "" : "s",
                     h.padsEnabled.load() ? "" : " (gamepads off in the settings)");
        for (int slot = 0; slot < h.slots.count(); ++slot)
            if (const OpenPad *pad = findPad(h, static_cast<SDL_JoystickID>(h.slots.idAt(slot))))
                logPad(*pad, slot, "details", true);
    }

    bool quitRequested()
    {
        return host().quit.load();
    }

    void requestQuit()
    {
        host().quit.store(true);
    }

    void requestExitToMenu()
    {
        Host &h = host();
        if (h.menuAvailable.load())
            h.exitToMenu.store(true);
        else
            h.quit.store(true);
    }

    bool exitToMenuRequested() { return host().exitToMenu.load(); }

    void clearExitToMenu() { host().exitToMenu.store(false); }

    void setMenuAvailable(bool available) { host().menuAvailable.store(available); }

    bool menuAvailable() { return host().menuAvailable.load(); }

    void setWindowTitle(const char *title)
    {
        Host &h = host();
        if (h.window && title)
            SDL_SetWindowTitle(h.window, title);
    }

    GamepadStyle gamepadStyle(int slot)
    {
        SDL_Gamepad *pad = padInSlot(host(), slot);
        return pad ? styleOf(SDL_GetGamepadType(pad)) : GamepadStyle::None;
    }

    bool windowFocused()
    {
        Host &h = host();
        if (h.headless)
            return true; // no window to lose the focus: no pause, no mute
        return h.window && (SDL_GetWindowFlags(h.window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    }

    bool windowMinimized()
    {
        Host &h = host();
        return h.window && (SDL_GetWindowFlags(h.window) & SDL_WINDOW_MINIMIZED) != 0;
    }

    bool windowFullscreen()
    {
        Host &h = host();
        return h.window && (SDL_GetWindowFlags(h.window) & SDL_WINDOW_FULLSCREEN) != 0;
    }

    void setWindowFullscreen(bool fullscreen)
    {
        if (SDL_Window *window = host().window)
            SDL_SetWindowFullscreen(window, fullscreen);
    }

    void windowSize(int &width, int &height)
    {
        width = 0;
        height = 0;
        if (SDL_Window *window = host().window)
            SDL_GetWindowSize(window, &width, &height);
    }

    bool keyDown(int code)
    {
        return code > 0 && code < hi::kHostCodeCount && host().input.held.test(static_cast<size_t>(code));
    }

    bool keyPressed(int code)
    {
        return code > 0 && code < hi::kHostCodeCount && host().input.pressed.test(static_cast<size_t>(code));
    }

    const InputFrame &input()
    {
        return host().input;
    }

    hi::GamepadState gamepad(int &index, std::string &name)
    {
        Host &h = host();
        SDL_Gamepad *pad = padInSlot(h, 0);
        index = pad ? 0 : -1;
        name.clear();
        if (!pad)
            return {};
        name = padName(pad);
        return readGamepad(pad);
    }

    InputSnapshot inputSnapshot()
    {
        Host &h = host();
        std::lock_guard<std::mutex> lock(h.snapshotMutex);
        return h.snapshot;
    }

    void setMouseCaptured(bool captured)
    {
        if (SDL_Window *window = host().window)
            SDL_SetWindowRelativeMouseMode(window, captured);
    }

    // ZX_PRESENT_HZ=<n> with ZX_PRESENT_OFFSCREEN=1: the window loop timed at n a second and nothing shown
    // (a display of that rate the panel cannot be, for timing runs: frame interpolation's 144 Hz).
    static bool timedOffscreen()
    {
        static const bool on = []
        {
            const char *hz = std::getenv("ZX_PRESENT_HZ");
            const char *off = std::getenv("ZX_PRESENT_OFFSCREEN");
            return hz && hz[0] && off && off[0] == '1';
        }();
        return on;
    }

    // The window loop held at ZX_PRESENT_HZ a second (at least 30), as a display of that rate would hold it.
    static void waitTimedRefresh()
    {
        using Clock = std::chrono::steady_clock;
        static const std::chrono::microseconds period(1000000 / std::max(30L, std::strtol(std::getenv("ZX_PRESENT_HZ"), nullptr, 10)));
        static Clock::time_point next = Clock::now();
        const Clock::time_point now = Clock::now();
        next = std::max(next + period, now - std::chrono::milliseconds(50));
        if (next > now)
            std::this_thread::sleep_until(next);
    }

    void waitForRefresh()
    {
        Host &h = host();
        if (h.headless || !h.gpu || !h.window || h.nextCommands || timedOffscreen())
            return;
        SDL_GPUCommandBuffer *commands = SDL_AcquireGPUCommandBuffer(h.gpu);
        if (!commands)
            return;
        SDL_GPUTexture *swapchain = nullptr;
        uint32_t swapWidth = 0, swapHeight = 0;
        bool acquired = false;
        {
            zx::ftrace::Scope traceAcquire(zx::ftrace::kUser1, zx::ftrace::kUser1 + 100);
            // Interpolating on Direct3D 12: SDL waits for the swapchain's oldest picture holding the lock every
            // submit takes (SDL 3.4's D3D12_WaitForFences), most of a refresh at a fixed rate, and the GS worker's
            // frames, held at their submit and fence waits, were made only after the latch, a refresh later than
            // the display time asked for them. Asked without waiting instead (no lock while not ready), and again
            // until it is ready.
            if (h.interpolatedPresents && h.d3d12)
            {
                using Clock = std::chrono::steady_clock;
                const Clock::time_point until = Clock::now() + std::chrono::milliseconds(100);
                while ((acquired = SDL_AcquireGPUSwapchainTexture(commands, h.window, &swapchain, &swapWidth, &swapHeight)) &&
                       !swapchain && Clock::now() < until)
                    SDL_DelayNS(200'000);
            }
            if (!swapchain)
                acquired = SDL_WaitAndAcquireGPUSwapchainTexture(commands, h.window, &swapchain, &swapWidth, &swapHeight);
        }
        h.nextCommands = commands;
        h.nextSwapchain = swapchain;
        h.nextWidth = swapWidth;
        h.nextHeight = swapHeight;
        h.nextAcquired = acquired;
    }

    void present(const Frame &frame)
    {
        Host &h = host();
        if (h.headless)
        {
            // Nothing to show; keep the loop near 60 Hz, as for a hidden window (ZX_PRESENT_HZ=<n>: at n, a display
            // of that rate for timing runs without a window: frame interpolation's 75 or 144 Hz).
            if (const char *hz = std::getenv("ZX_PRESENT_HZ"); hz && hz[0])
                waitTimedRefresh();
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            return;
        }
        if (!h.gpu || !h.window)
            return;
        if (timedOffscreen())
        {
            // Nothing shown: only the window loop's rate, as a display of that rate would set it.
            waitTimedRefresh();
            return;
        }
        // (waitForRefresh took the swapchain picture already, or this does)
        SDL_GPUCommandBuffer *const early = h.nextCommands;
        h.nextCommands = nullptr;
        SDL_GPUCommandBuffer *commands = early ? early : SDL_AcquireGPUCommandBuffer(h.gpu);
        if (!commands)
            return;

        const bool onGpu = frame.gpuTexture != nullptr;
        if (h.lastFrameOnGpu && !onGpu)
            h.haveFrame = false; // the frame texture holds an old picture
        h.lastFrameOnGpu = onGpu;
        const bool sizeOk = !onGpu && frame.rgba && frame.texWidth && frame.texHeight &&
                            ensureFrameTexture(h, frame.texWidth, frame.texHeight);
        if (sizeOk && (frame.changed || !h.haveFrame))
        {
            if (auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(h.gpu, h.upload, true)))
            {
                const size_t row = static_cast<size_t>(frame.texWidth) * 4u;
                for (uint32_t y = 0; y < frame.texHeight; ++y)
                    std::memcpy(mapped + y * row, frame.rgba + static_cast<size_t>(y) * frame.stride, row);
                SDL_UnmapGPUTransferBuffer(h.gpu, h.upload);
                SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
                SDL_GPUTextureTransferInfo source{};
                source.transfer_buffer = h.upload;
                source.pixels_per_row = frame.texWidth;
                source.rows_per_layer = frame.texHeight;
                SDL_GPUTextureRegion target{};
                target.texture = h.frameTexture;
                target.w = frame.texWidth;
                target.h = frame.texHeight;
                target.d = 1;
                texcheck::uploadToTexture(copy, &source, &target, true);
                SDL_EndGPUCopyPass(copy);
                h.haveFrame = true;
            }
        }
        Host::PictureOverlay &over = h.pictureOverlay;
        if (over.pending && over.texture && over.upload)
        {
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(commands);
            SDL_GPUTextureTransferInfo source{};
            source.transfer_buffer = over.upload;
            source.pixels_per_row = over.width;
            source.rows_per_layer = over.height;
            SDL_GPUTextureRegion target{};
            target.texture = over.texture;
            target.w = over.width;
            target.h = over.height;
            target.d = 1;
            texcheck::uploadToTexture(copy, &source, &target, true);
            SDL_EndGPUCopyPass(copy);
            over.pending = false;
            over.ready = true;
        }
        if (h.overlay.prepare)
            h.overlay.prepare(commands);

        SDL_GPUTexture *swapchain = h.nextSwapchain;
        uint32_t swapWidth = h.nextWidth;
        uint32_t swapHeight = h.nextHeight;
        bool acquired = h.nextAcquired;
        if (!early)
        {
            zx::ftrace::Scope traceAcquire(zx::ftrace::kUser1, zx::ftrace::kUser1 + 100);
            acquired = SDL_WaitAndAcquireGPUSwapchainTexture(commands, h.window, &swapchain, &swapWidth, &swapHeight);
        }
        h.nextSwapchain = nullptr;
        h.nextAcquired = false;
        if (!acquired || !swapchain)
        {
            // Minimized or hidden: nothing to show; keep the loop near 60 Hz.
            SDL_SubmitGPUCommandBuffer(commands);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            return;
        }

        // A PS2 picture fills a 4:3 TV (or a 16:9 one) whatever the buffer
        // size: fit that aspect, in swapchain pixels.
        const float screenWidth = static_cast<float>(swapWidth);
        const float screenHeight = static_cast<float>(swapHeight);
        const float aspect = frame.aspect > 0.0f ? frame.aspect : 4.0f / 3.0f;
        float dstWidth = std::min(screenWidth, screenHeight * aspect);
        float dstHeight = dstWidth / aspect;
        if (frame.integerScale)
        {
            // Whole multiples of 480 lines (the width follows the aspect).
            const float multiple = std::floor(std::min(screenWidth / (480.0f * aspect), screenHeight / 480.0f));
            if (multiple >= 1.0f)
            {
                dstHeight = 480.0f * multiple;
                dstWidth = dstHeight * aspect;
            }
        }
        const uint32_t w = static_cast<uint32_t>(std::lround(dstWidth));
        const uint32_t hgt = static_cast<uint32_t>(std::lround(dstHeight));
        const bool drawPicture = (onGpu || h.haveFrame) && frame.width && frame.height && w && hgt;
        if (swapWidth && swapHeight)
        {
            const float shownWidth = static_cast<float>(std::min(w, swapWidth));
            const float shownHeight = static_cast<float>(std::min(hgt, swapHeight));
            h.shownPicture = {(screenWidth - shownWidth) * 0.5f / screenWidth, (screenHeight - shownHeight) * 0.5f / screenHeight,
                              shownWidth / screenWidth, shownHeight / screenHeight};
        }
        if (drawPicture)
        {
            SDL_GPUBlitInfo blit{};
            blit.source.texture = onGpu ? frame.gpuTexture : h.frameTexture;
            blit.source.w = onGpu ? frame.width : std::min(frame.width, h.texWidth);
            blit.source.h = onGpu ? frame.height : std::min(frame.height, h.texHeight);
            // A picture more than twice the window's size (a high internal
            // resolution in a small window): bilinear from its mipmap level
            // nearest above the window's size, so every pixel of the picture
            // counts (plain bilinear would skip rows and shimmer).
            if (onGpu && frame.gpuLevels > 1u && frame.linearFilter && !frame.integerScale && w && hgt)
            {
                uint32_t level = 0;
                while (level + 1u < frame.gpuLevels && (blit.source.w >> (level + 1u)) >= w &&
                       (blit.source.h >> (level + 1u)) >= hgt)
                    ++level;
                blit.source.mip_level = level;
                blit.source.w = std::max<uint32_t>(blit.source.w >> level, 1u);
                blit.source.h = std::max<uint32_t>(blit.source.h >> level, 1u);
            }
            blit.destination.texture = swapchain;
            blit.destination.x = (swapWidth - std::min(w, swapWidth)) / 2u;
            blit.destination.y = (swapHeight - std::min(hgt, swapHeight)) / 2u;
            blit.destination.w = std::min(w, swapWidth);
            blit.destination.h = std::min(hgt, swapHeight);
            blit.load_op = SDL_GPU_LOADOP_CLEAR;
            blit.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
            blit.filter = frame.linearFilter && !frame.integerScale ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
            texcheck::blitTexture(commands, &blit);
            // The picture over the game's, where it was placed (opaque).
            if (over.ready && over.place.w > 0.0f && over.place.h > 0.0f)
            {
                SDL_GPUBlitInfo on{};
                on.source.texture = over.texture;
                on.source.w = over.width;
                on.source.h = over.height;
                on.destination.texture = swapchain;
                on.destination.x = static_cast<uint32_t>(std::lround(std::clamp(over.place.x, 0.0f, 1.0f) * screenWidth));
                on.destination.y = static_cast<uint32_t>(std::lround(std::clamp(over.place.y, 0.0f, 1.0f) * screenHeight));
                on.destination.w = std::min(static_cast<uint32_t>(std::lround(over.place.w * screenWidth)), swapWidth - on.destination.x);
                on.destination.h = std::min(static_cast<uint32_t>(std::lround(over.place.h * screenHeight)), swapHeight - on.destination.y);
                on.load_op = SDL_GPU_LOADOP_LOAD;
                on.filter = SDL_GPU_FILTER_LINEAR;
                if (on.destination.w > 0u && on.destination.h > 0u)
                    texcheck::blitTexture(commands, &on);
            }
        }
        if (!drawPicture || h.overlay.render)
        {
            SDL_GPUColorTargetInfo target{};
            target.texture = swapchain;
            target.load_op = drawPicture ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
            target.store_op = SDL_GPU_STOREOP_STORE;
            target.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
            SDL_GPURenderPass *pass = texcheck::beginRenderPass(commands, &target, 1, nullptr);
            if (pass && h.overlay.render)
                h.overlay.render(commands, pass);
            if (pass)
                SDL_EndGPURenderPass(pass);
        }
        {
            zx::ftrace::Scope traceSubmit(zx::ftrace::kGpuSubmitBegin, zx::ftrace::kGpuSubmitEnd);
            SDL_SubmitGPUCommandBuffer(commands);
        }

        // Without vsync nothing else paces the window loop: at most 240
        // frames a second (ZX_PRESENT_HZ=<n>: n, a display of that rate
        // for timing runs, frame interpolation's 144 Hz on a 120 Hz Mac).
        if (!h.vsync)
        {
            using Clock = std::chrono::steady_clock;
            static const std::chrono::microseconds period = []
            {
                const char *v = std::getenv("ZX_PRESENT_HZ");
                const long hz = v ? std::strtol(v, nullptr, 10) : 0;
                return std::chrono::microseconds(hz >= 30 && hz <= 1000 ? 1000000 / hz : 4166);
            }();
            static Clock::time_point next = Clock::now();
            const Clock::time_point now = Clock::now();
            next = std::max(next + period, now - std::chrono::milliseconds(50));
            if (next > now)
                std::this_thread::sleep_until(next);
        }
    }

    bool audioReady()
    {
        return host().audio != 0;
    }

    uint32_t audioDevice()
    {
        return host().audio;
    }

    const char *audioDriver()
    {
        const char *driver = host().audio ? SDL_GetCurrentAudioDriver() : nullptr;
        return driver ? driver : "";
    }

    void setAudioGain(float gain)
    {
        if (const SDL_AudioDeviceID device = host().audio)
            SDL_SetAudioDeviceGain(device, std::clamp(gain, 0.0f, 4.0f));
    }

    void setVsync(bool vsync)
    {
        Host &h = host();
        h.vsync = vsync;
        if (!h.gpu || !h.window)
            return;
        SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
        if (!vsync)
        {
            if (SDL_WindowSupportsGPUPresentMode(h.gpu, h.window, SDL_GPU_PRESENTMODE_IMMEDIATE))
                mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
            else if (SDL_WindowSupportsGPUPresentMode(h.gpu, h.window, SDL_GPU_PRESENTMODE_MAILBOX))
                mode = SDL_GPU_PRESENTMODE_MAILBOX;
        }
        if (!SDL_SetGPUSwapchainParameters(h.gpu, h.window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
            std::fprintf(stderr, "[host] cannot change the present mode: %s\n", SDL_GetError());
    }

    bool vsync()
    {
        return host().vsync;
    }

    void setInterpolatedPresents(bool on)
    {
        Host &h = host();
        h.interpolatedPresents = on;
        if (!h.gpu || !h.window || framesInFlightAsked())
            return;
#if defined(__ANDROID__)
        SDL_SetGPUAllowedFramesInFlight(h.gpu, on ? 1u : 2u);
#else
        (void)on;
#endif
    }

    bool headless()
    {
        return host().headless;
    }

    void setVariableRefresh(bool on)
    {
        host().variableRefresh = on;
    }

    bool variableRefresh()
    {
        const Host &h = host();
        return h.variableRefresh || h.variableRefreshAsked;
    }

    int64_t displayRefreshNs()
    {
        const Host &h = host();
        if (h.headless || !h.window)
            return 0;
        const SDL_DisplayID display = SDL_GetDisplayForWindow(h.window);
        const SDL_DisplayMode *mode = display ? SDL_GetCurrentDisplayMode(display) : nullptr;
        if (!mode)
            return 0;
        if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
            return static_cast<int64_t>(1'000'000'000LL * mode->refresh_rate_denominator / mode->refresh_rate_numerator);
        return mode->refresh_rate > 1.0f ? static_cast<int64_t>(1e9 / static_cast<double>(mode->refresh_rate)) : 0;
    }

    // ---- the map window --------------------------------------------------------

    bool openMapWindow(const char *title, const MapWindowRect &rect)
    {
        Host &h = host();
        if (!h.initialized || h.headless)
            return false;
        Host::MapWindow &m = h.map;
        if (m.window)
            return true;
        m.window = SDL_CreateWindow(title, std::max(160, rect.width), std::max(140, rect.height),
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
        if (!m.window)
        {
            std::fprintf(stderr, "[host] cannot create the map window: %s\n", SDL_GetError());
            return false;
        }
        if (rect.x != MapWindowRect{}.x || rect.y != MapWindowRect{}.y)
        {
            // Where it was, unless no display shows enough of it now
            // (host_window_place.h: a second display unplugged since).
            std::vector<ScreenRect> displays;
            int count = 0;
            if (SDL_DisplayID *ids = SDL_GetDisplays(&count))
            {
                const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
                for (int i = 0; i < count; ++i)
                    if (SDL_Rect usable; SDL_GetDisplayUsableBounds(ids[i], &usable))
                    {
                        const ScreenRect area{usable.x, usable.y, usable.w, usable.h};
                        if (ids[i] == primary)
                            displays.insert(displays.begin(), area);
                        else
                            displays.push_back(area);
                    }
                SDL_free(ids);
            }
            int titleBar = 0;
            if (!SDL_GetWindowBordersSize(m.window, &titleBar, nullptr, nullptr, nullptr))
                titleBar = 0; // (macOS reports none)
            int width = 0, height = 0;
            SDL_GetWindowSize(m.window, &width, &height);
            const ScreenRect placed = keptOnDisplays({rect.x, rect.y, width, height}, titleBar, displays);
            if (placed.x != rect.x || placed.y != rect.y || placed.width != width || placed.height != height)
                std::fprintf(stderr, "[host] map window: %dx%d at %d,%d is off the displays; %dx%d at %d,%d\n", width, height,
                             rect.x, rect.y, placed.width, placed.height, placed.x, placed.y);
            if (placed.width != width || placed.height != height)
                SDL_SetWindowSize(m.window, placed.width, placed.height);
            SDL_SetWindowPosition(m.window, placed.x, placed.y);
        }
        m.renderer = SDL_CreateRenderer(m.window, nullptr);
        if (!m.renderer)
        {
            std::fprintf(stderr, "[host] no renderer for the map window: %s\n", SDL_GetError());
            SDL_DestroyWindow(m.window);
            m.window = nullptr;
            return false;
        }
        // The game's own present waits for the display; this one must not.
        SDL_SetRenderVSync(m.renderer, 0);
        m.closed = false;
        m.events.clear();
        m.shown = {};
        return true;
    }

    void closeMapWindow()
    {
        Host::MapWindow &m = host().map;
        if (m.texture)
            SDL_DestroyTexture(m.texture);
        if (m.renderer)
            SDL_DestroyRenderer(m.renderer);
        if (m.window)
            SDL_DestroyWindow(m.window);
        m.texture = nullptr;
        m.renderer = nullptr;
        m.window = nullptr;
        m.texWidth = m.texHeight = 0;
        m.events.clear();
    }

    bool mapWindowOpen() { return host().map.window != nullptr; }

    MapWindowRect mapWindowRect()
    {
        MapWindowRect rect;
        if (SDL_Window *window = host().map.window)
        {
            SDL_GetWindowPosition(window, &rect.x, &rect.y);
            SDL_GetWindowSize(window, &rect.width, &rect.height);
        }
        return rect;
    }

    void setMapWindowOnTop(bool on)
    {
        if (SDL_Window *window = host().map.window)
            SDL_SetWindowAlwaysOnTop(window, on);
    }

    void presentMap(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride)
    {
        Host::MapWindow &m = host().map;
        if (!m.window || !m.renderer || !rgba || width == 0 || height == 0)
            return;
        if (!m.texture || m.texWidth != width || m.texHeight != height)
        {
            if (m.texture)
                SDL_DestroyTexture(m.texture);
            m.texture = SDL_CreateTexture(m.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                          static_cast<int>(width), static_cast<int>(height));
            if (!m.texture)
            {
                std::fprintf(stderr, "[host] no texture for the map window: %s\n", SDL_GetError());
                return;
            }
            SDL_SetTextureScaleMode(m.texture, SDL_SCALEMODE_LINEAR);
            m.texWidth = width;
            m.texHeight = height;
        }
        SDL_UpdateTexture(m.texture, nullptr, rgba, static_cast<int>(stride));
        // The picture as large as fits, its aspect kept, centred: in output
        // pixels for the drawing, in points for the pointer.
        auto fitted = [&](float boxWidth, float boxHeight)
        {
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            const float w = std::min(boxWidth, boxHeight * aspect), hgt = w / aspect;
            return SDL_FRect{(boxWidth - w) / 2.0f, (boxHeight - hgt) / 2.0f, w, hgt};
        };
        int outWidth = 0, outHeight = 0, pointsWidth = 0, pointsHeight = 0;
        SDL_GetRenderOutputSize(m.renderer, &outWidth, &outHeight);
        SDL_GetWindowSize(m.window, &pointsWidth, &pointsHeight);
        m.shown = fitted(static_cast<float>(pointsWidth), static_cast<float>(pointsHeight));
        const SDL_FRect target = fitted(static_cast<float>(outWidth), static_cast<float>(outHeight));
        SDL_SetRenderDrawColor(m.renderer, 0, 0, 0, 255);
        SDL_RenderClear(m.renderer);
        SDL_RenderTexture(m.renderer, m.texture, nullptr, &target);
        SDL_RenderPresent(m.renderer);
    }

    std::vector<MapPointerEvent> takeMapEvents()
    {
        std::vector<MapPointerEvent> out;
        out.swap(host().map.events);
        return out;
    }

    bool takeMapWindowClosed()
    {
        Host::MapWindow &m = host().map;
        const bool closed = m.closed;
        m.closed = false;
        return closed;
    }

    void setOverlayPicture(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride)
    {
        Host &h = host();
        Host::PictureOverlay &o = h.pictureOverlay;
        if (!h.gpu || !rgba || width == 0 || height == 0)
            return;
        if (!o.texture || o.width != width || o.height != height)
        {
            if (o.texture)
                texcheck::releaseTexture(h.gpu, o.texture);
            if (o.upload)
                SDL_ReleaseGPUTransferBuffer(h.gpu, o.upload);
            o.texture = nullptr;
            o.upload = nullptr;
            o.ready = o.pending = false;
            SDL_GPUTextureCreateInfo texInfo{};
            texInfo.type = SDL_GPU_TEXTURETYPE_2D;
            texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
            texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
            texInfo.width = width;
            texInfo.height = height;
            texInfo.layer_count_or_depth = 1;
            texInfo.num_levels = 1;
            o.texture = texcheck::createTexture(h.gpu, &texInfo);
            SDL_GPUTransferBufferCreateInfo bufInfo{};
            bufInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            bufInfo.size = width * height * 4u;
            o.upload = SDL_CreateGPUTransferBuffer(h.gpu, &bufInfo);
            if (!o.texture || !o.upload)
            {
                std::fprintf(stderr, "[host] cannot create the overlay picture's texture: %s\n", SDL_GetError());
                return;
            }
            o.width = width;
            o.height = height;
        }
        auto *mapped = static_cast<uint8_t *>(SDL_MapGPUTransferBuffer(h.gpu, o.upload, true));
        if (!mapped)
            return;
        const size_t row = static_cast<size_t>(width) * 4u;
        if (stride == row)
            std::memcpy(mapped, rgba, row * height);
        else
            for (uint32_t y = 0; y < height; ++y)
                std::memcpy(mapped + y * row, rgba + static_cast<size_t>(y) * stride, row);
        SDL_UnmapGPUTransferBuffer(h.gpu, o.upload);
        o.pending = true;
    }

    void placeOverlay(float x, float y, float width, float height)
    {
        Host::PictureOverlay &o = host().pictureOverlay;
        o.place = SDL_FRect{x, y, std::max(0.0f, width), std::max(0.0f, height)};
        if (o.place.w <= 0.0f || o.place.h <= 0.0f)
        {
            o.buttons = 0;
            o.touchMouse = false;
            o.fingers.clear();
            o.events.clear();
        }
    }

    std::vector<OverlayPointerEvent> takeOverlayPointerEvents()
    {
        std::vector<OverlayPointerEvent> out;
        out.swap(host().pictureOverlay.events);
        return out;
    }

    bool writePng(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride,
                  uint32_t outWidth, uint32_t outHeight)
    {
        if (!rgba || width == 0 || height == 0)
            return false;
        // Opaque, as shown (the window ignores the GS alpha).
        SDL_Surface *picture = SDL_CreateSurface(static_cast<int>(width), static_cast<int>(height),
                                                 SDL_PIXELFORMAT_RGBA32);
        if (!picture)
            return false;
        for (uint32_t y = 0; y < height; ++y)
        {
            auto *dst = static_cast<uint8_t *>(picture->pixels) + static_cast<size_t>(y) * picture->pitch;
            std::memcpy(dst, rgba + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4u);
            for (uint32_t x = 0; x < width; ++x)
                dst[x * 4u + 3u] = 0xFFu;
        }
        SDL_Surface *out = picture;
        if (outWidth && outHeight && (outWidth != width || outHeight != height))
        {
            out = SDL_ScaleSurface(picture, static_cast<int>(outWidth), static_cast<int>(outHeight),
                                   SDL_SCALEMODE_LINEAR);
            if (!out)
                out = picture;
        }
        const bool ok = SDL_SavePNG(out, path.c_str());
        if (out != picture)
            SDL_DestroySurface(out);
        SDL_DestroySurface(picture);
        return ok;
    }

    SDL_Window *sdlWindow()
    {
        return host().window;
    }

    SDL_GPUDevice *gpuDevice()
    {
        return host().gpu;
    }

    SDL_GPUTextureFormat swapchainFormat()
    {
        return host().swapFormat;
    }

    int addGpuDeviceReleaseHook(std::function<void()> hook)
    {
        Host &h = host();
        std::lock_guard<std::mutex> lock(h.deviceHookMutex);
        const int id = h.nextDeviceHook++;
        h.deviceHooks.emplace_back(id, std::move(hook));
        return id;
    }

    void removeGpuDeviceReleaseHook(int id)
    {
        Host &h = host();
        std::lock_guard<std::mutex> lock(h.deviceHookMutex);
        h.deviceHooks.erase(std::remove_if(h.deviceHooks.begin(), h.deviceHooks.end(),
                                           [id](const auto &entry) { return entry.first == id; }),
                            h.deviceHooks.end());
    }

        void setEventHook(EventHook hook)
    {
        host().eventHook = std::move(hook);
    }

    void setOverlay(Overlay overlay)
    {
        host().overlay = std::move(overlay);
    }

    SDL_FRect shownPicture()
    {
        return host().shownPicture;
    }
}
