#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "runtime/host_input.h"

class PSPadBackend
{
public:
    PSPadBackend() = default;
    ~PSPadBackend() = default;

    bool readState(int port, int slot, uint8_t *data, size_t size);

    // Whether a pad is plugged into the port, as the game should see it:
    // port 0 always (keyboard and mouse, the first gamepad); port 1 while
    // player 2's own gamepad is connected (hi::routePort), else it is
    // empty, as on a PS2 with one controller (ZX_PAD_PORT1=mirror: always,
    // a copy of port 0). Any thread.
    static bool portConnected(int port);

    // Emulated vsync counter used by ZX_PAD_SCRIPT_CLOCK=vsync.
    void setVsyncSource(const std::atomic<uint64_t> *vsyncTick) { m_vsyncTick = vsyncTick; }
    // EE RAM for ZX_PAD_SCRIPT SYNC anchors.
    void setGuestMemory(const uint8_t *rdram) { m_rdram = rdram; }

    // Per-user folder name for controls.ini (runtime/host_input.h,
    // userConfigDir). Set before the first pollHost(); default "Z-Axis Collection".
    static void setAppName(const std::string &name);
    // The game whose controls these are (ps2_host_input::gameControls:
    // "ai", "dm2", "bmx"): its default controls.ini and the names of its
    // actions on the Controls rows; the config folder is named after it
    // (as setAppName) unless ZX_CONTROLS_INI names the file. A game
    // override calls it before the first pollHost(); default "ai".
    static void setGame(const std::string &id);

    // Names for the controls in the settings pages (runtime/settings.h),
    // in the order shown: controls.ini keys ("cross", "left_up",
    // "modifier", "recentre") with what they do in the game ("Jump"). Keys
    // left out are not shown. Set before the first pollHost(); default:
    // the game's (setGame), else the DualShock 2 names.
    static void setControlLabels(std::vector<std::pair<std::string, std::string>> labels);

    // Rebinding from the settings pages: the next key or mouse button (or
    // gamepad button, for a gamepad row) is bound instead of reaching the
    // game. Escape cancels, Backspace or Delete unbinds. True while waiting.
    static bool captureActive();

    // Holds a DualShock 2 button (runtime/host_input.h PadButton) for
    // the next `reads` port-0 pad reads, as if pressed (opening the game's
    // pause menu from the app menu).
    static void pressButton(int button, int reads);

    // What the player last used, for on-screen button prompts: keyboard and
    // mouse, or the gamepad (by its face-button labels). A key, mouse
    // button or clear mouse motion picks the keyboard; a gamepad button or
    // a stick pushed past half picks the gamepad. At start: the gamepad if
    // one is connected. ZX_PROMPTS=keyboard|playstation|xbox fixes it.
    enum class InputDevice
    {
        Keyboard,
        PlayStation,
        Xbox,
        Nintendo
    };
    static InputDevice activeDevice();
    // Fixes the device as ZX_PROMPTS does ("keyboard", "playstation",
    // "xbox", "nintendo"; "auto" follows the input again). False if unknown.
    static bool forcePromptDevice(const std::string &name);
    // Changes whenever activeDevice() or the bindings change.
    static uint32_t promptGeneration();
    // The keys and mouse buttons bound to a DualShock 2 button (PadButton)
    // or stick direction (StickDirection), in controls.ini order.
    static std::vector<int> keyBindings(int padButton);
    static std::vector<int> stickKeyBindings(int direction);
    // The physical gamepad button (by position, PadButton numbering) that
    // presses a game button, -1 if none.
    static int gamepadButtonFor(int padButton);

    // A game override says whether the game shows a menu: then the mouse
    // wheel presses up/down and a left click Cross (accept). Off by
    // default.
    static void setMenuMouse(bool menus);
    // A game override says the wheel works the right stick's Y axis (the
    // park editor's camera zoom): a step away from the user pushes the
    // stick up for a few reads, towards the user down. Off by default;
    // setMenuMouse wins.
    static void setWheelStick(bool on);

    // A key or mouse button (host code) went down within the last
    // `seconds` (telling Escape from Enter, both Start by default).
    static bool keyPressedWithin(int code, double seconds);
    // [mouse] mode = look: the mouse turns the camera (else it pushes a stick).
    static bool mouseLook();

    // [hotkeys] (ps2_host_input::Hotkey): how often the shortcut was
    // pressed so far; a game override acts when the count changes.
    // triggerHotkey counts a press as if made (ZX_CONTROL, tests).
    static uint32_t hotkeyPresses(int hotkey);
    static void triggerHotkey(int hotkey);
    // How often the game's reads of the pad on `port` (0 or 1) found the
    // button (ps2_host_input::PadButton) newly down, counting what the game
    // got (after the pad script, ZX_CONTROL and a play recording's replay);
    // a host page acts when the count changes. Any thread.
    static uint32_t buttonPresses(int port, int button);
    // The keys bound to a shortcut (controls.ini [hotkeys]).
    static std::vector<ps2_host_input::Binding> hotkeyKeys(int hotkey);

    // Free input (a game override's photo mode): while on, every port
    // reads an idle pad and the host input goes to takeFreeInput()
    // instead; the cursor is captured while the window has the focus.
    // Turning it off keeps whatever is still held away from the game until
    // released.
    struct FreeInput
    {
        ps2_host_input::HostCodeSet down;    // keys and mouse buttons held (or tapped) since the last take
        ps2_host_input::HostCodeSet pressed; // went down since the last take
        // Mouse motion since the last take, right and down positive: pixels
        // times controls.ini's [mouse] sensitivity, turned over by its
        // invert_x and invert_y = false (at the defaults, the pixels).
        float mouseDx = 0.0f;
        float mouseDy = 0.0f;
        float wheel = 0.0f; // wheel steps since the last take (away from the user positive)
        bool focused = true;
        // The pad as the game would have read it on port 0: the gamepad
        // (buttons by position), ZX_PAD_SCRIPT and ZX_CONTROL.
        uint16_t padButtons = 0u; // held at the last read, bit = PadButton
        uint16_t padPressed = 0u; // went down since the last take
        std::array<float, 4> sticks{}; // RX, RY, LX, LY in -1..1 (right/down positive), last read
    };
    static void setFreeInput(bool on);
    static bool freeInput();
    // A game override's own presses (developer automation, e.g. a start
    // straight into a level): PadScript button masks ORed into port 0's
    // buttons at full pressure, as ZX_CONTROL's (every port with
    // ZX_PAD_PORT1=mirror); 0 releases them.
    static void setOverrideButtons(uint16_t mask);
    static FreeInput takeFreeInput();

    // Main thread, once per host frame after the window polled its events:
    // samples keyboard, mouse, focus and gamepad for the next pad read and
    // applies the cursor capture the input mapping asks for.
    void pollHost();

    // Mouse-look for a game camera hook (game thread, once per camera
    // update): see ps2_host_input::HostInputMapper::cameraFrame.
    static ps2_host_input::LookFrame mouseLookFrame(
        float basePitch,
        ps2_host_input::HostInputMapper::LookContext context = ps2_host_input::HostInputMapper::LookContext::Play);

private:
    const std::atomic<uint64_t> *m_vsyncTick = nullptr;
    const uint8_t *m_rdram = nullptr;
};

#endif
