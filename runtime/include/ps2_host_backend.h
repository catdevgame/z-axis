#pragma once

// Host platform layer (SDL3): the window, presentation through SDL_GPU
// (Metal on macOS, D3D12/Vulkan elsewhere), keyboard, mouse, gamepads and
// the audio device. SDL types stay out of this header (it is included by
// the kernel stubs); ps2_host_sdl.h has the SDL-typed hooks for the debug UI.
//
// Everything runs on the window (main) thread, except audioDevice(),
// inputSnapshot() and what says "any thread".

#include "runtime/ps2_host_input.h"

#include <cstdint>
#include <string>
#include <vector>

struct SDL_GPUTexture;

namespace ps2_host
{
    struct WindowOptions
    {
        int width = 640;
        int height = 448;
        // Retina: a swapchain in pixels instead of points.
        bool highDpi = false;
        // No window, no GPU device, nothing presented: a run that shows
        // nothing (PS2X_GS_NULL, PS2X_HEADLESS) works where no swapchain can
        // exist, such as an SSH session on Windows.
        bool headless = false;
    };

    // Window, GPU device, gamepads and the audio device. False if there is
    // no window and headless was not asked for (the audio device is optional).
    bool initialize(const char *title, const WindowOptions &options);
    void shutdown();
    // Up: initialize succeeded and no shutdown since (a second initialize
    // while up keeps it as it is).
    bool initialized();
    // The host input is up: a window, or a headless run (windowFocused() is
    // true there, the window sizes are 0).
    bool windowReady();

    // Handles the window system's events; once per host frame, before the
    // input of the frame is read.
    void pumpEvents();
    // Close button, Cmd+Q, Alt+F4, or requestQuit().
    bool quitRequested();
    // Ends the run as the close button does (any thread): the main loop
    // stops the game and runs the shutdown hooks (autosave).
    void requestQuit();
    // Leaves the running game for the app's menu (any thread): the game's
    // own quit item, File > Switch Game, the switch_game key. The main loop
    // stops the game as for a window close (PS2Runtime::run returns, the
    // shutdown hooks run), but the app then shows its menu instead of
    // quitting. Where the app has no menu (setMenuAvailable not called: a
    // single-game app, the runner) it is requestQuit().
    void requestExitToMenu();
    // A requestExitToMenu since the last clearExitToMenu (any thread).
    bool exitToMenuRequested();
    void clearExitToMenu();
    // The app has a menu to come back to (the collection's shell).
    void setMenuAvailable(bool available);
    bool menuAvailable();
    // The window's title (UTF-8); nothing without a window.
    void setWindowTitle(const char *title);
    bool windowFocused();
    bool windowMinimized();
    bool windowFullscreen();
    void setWindowFullscreen(bool fullscreen);
    // In points (window coordinates).
    void windowSize(int &width, int &height);

    // Keys and mouse buttons as ps2_host_input codes (the GLFW numbering
    // controls.ini names): held now, or pressed since the previous
    // pumpEvents (also when released again within the frame).
    bool keyDown(int code);
    bool keyPressed(int code);

    struct InputFrame
    {
        ps2_host_input::HostCodeSet held;
        ps2_host_input::HostCodeSet pressed;
        // Mouse motion since the previous pumpEvents, in points (relative
        // motion while the cursor is captured).
        float mouseDx = 0.0f;
        float mouseDy = 0.0f;
        // Cursor position in points (window coordinates; meaningless while
        // the cursor is captured) and wheel steps since the previous
        // pumpEvents (positive = away from the user).
        float mouseX = 0.0f;
        float mouseY = 0.0f;
        float wheel = 0.0f;
        bool mouseMoved = false; // the cursor moved since the previous pumpEvents
        bool touched = false;    // a finger touched or moved since the previous pumpEvents (the mouse
                                 // events SDL makes of it come too; the menu's hint hides on touch)
        bool focused = true;
        // Files and folders dropped on the window since the previous
        // pumpEvents (UTF-8 paths), in the order dropped.
        std::vector<std::string> dropped;
    };
    const InputFrame &input();

    // Gamepads. Every connected gamepad stays open, in player order (slots,
    // see ps2_host_input::GamepadSlots: slot 0 is player 1 on pad port 0,
    // slot 1 player 2 on port 1). They keep working while the window is in
    // the background. Mappings: SDL's own database, then gamecontrollerdb.txt
    // next to the executable (Contents/Resources in the app), the
    // SDL_GAMECONTROLLERCONFIG environment variable (SDL reads it itself)
    // and loadGamepadMappings (the one next to controls.ini).

    // Whose face-button labels a gamepad has (for on-screen prompts).
    enum class GamepadStyle
    {
        None,        // no gamepad
        PlayStation, // cross, circle, square, triangle
        Xbox,        // A, B, X, Y (also unknown and generic gamepads)
        Nintendo     // B, A, Y, X by position
    };
    const char *gamepadStyleName(GamepadStyle style); // "PlayStation", "Xbox", "Nintendo", "None"

    struct GamepadInfo
    {
        int slot = -1;
        uint32_t id = 0;   // SDL joystick instance id
        std::string name;  // as SDL names it (UTF-8)
        std::string type;  // SDL's gamepad type ("xbox360", "ps5", "unknown", ...)
        GamepadStyle style = GamepadStyle::Xbox;
        bool rumble = false; // the driver says it can rumble
        bool mapped = false; // SDL has a mapping for it
    };
    // Window thread. In slot order.
    std::vector<GamepadInfo> gamepads();
    int gamepadCount();
    // Changes whenever a pad connects, goes away, is remapped or changes
    // slot (any thread).
    uint32_t gamepadGeneration();
    // The gamepad in a slot, in the DualShock 2 layout (not present if the
    // slot is free). Window thread.
    ps2_host_input::GamepadState gamepadState(int slot);
    // The first one (slot 0; index -1: none).
    ps2_host_input::GamepadState gamepad(int &index, std::string &name);
    // The style of the gamepad in a slot (None if the slot is free).
    GamepadStyle gamepadStyle(int slot = 0);
    // Player 1 and 2 exchange gamepads (window thread).
    void swapGamepadPlayers();
    // [gamepad] enabled: off stops and ignores every gamepad's motors
    // (the input side is up to the caller). Any thread.
    void setGamepadsEnabled(bool enabled);
    // Adds the SDL mappings of a gamecontrollerdb.txt (the community
    // format, one controller per line) if the file exists; connected pads
    // it covers are remapped at once. Returns how many were added (-1: no
    // file or unreadable). Window thread.
    int loadGamepadMappings(const std::string &path);
    // Writes every connected gamepad's details to the log.
    void logGamepads();

    // Thread-safe copy of the last pumped frame's keys and gamepad (slot 0).
    struct InputSnapshot
    {
        ps2_host_input::HostCodeSet held;
        ps2_host_input::GamepadState gamepad;
    };
    InputSnapshot inputSnapshot();

    // Gamepad vibration (any thread) for a pad port's gamepad (port n
    // drives the pad in slot n; nothing if there is none): low = large
    // (left) motor, high = small (right) motor, 0-0xFFFF. A request lasts
    // ~250 ms unless repeated, so the motors stop when the guest stops
    // updating them (pause, stall). PS2X_RUMBLE=0 turns it off.
    void setRumble(int port, uint16_t low, uint16_t high);
    // Settings > Controls > Test Rumble: each connected pad in turn runs
    // its large (low-frequency) motor, then its small one, through the same
    // per-slot request and refresh path as the game's (the game's own
    // requests wait meanwhile). Window thread.
    void startRumbleTest();

    // Relative mouse mode: hidden cursor kept in the window.
    void setMouseCaptured(bool captured);

    // One presented frame: the game picture is the top-left width x height
    // of an RGBA8 image (texWidth x texHeight, stride bytes per row), or of
    // gpuTexture, a texture on gpuDevice() (the GPU GS renderer's picture,
    // at its internal resolution), shown at the given aspect: the largest
    // fit with black bars, or whole multiples of 480 lines with
    // integerScale. Waits for the display's vsync (see setVsync).
    struct Frame
    {
        SDL_GPUTexture *gpuTexture = nullptr;
        // Mipmap levels of gpuTexture: a window much smaller than the
        // picture is filled from the level nearest above its size.
        uint32_t gpuLevels = 1;
        const uint8_t *rgba = nullptr;
        uint32_t stride = 0;
        uint32_t texWidth = 0;
        uint32_t texHeight = 0;
        bool changed = true; // pixels differ from the last presented ones
        uint32_t width = 0;
        uint32_t height = 0;
        float aspect = 4.0f / 3.0f;
        bool integerScale = false;
        bool linearFilter = false;
    };
    void present(const Frame &frame);
    // Waits for the display's next refresh (the swapchain picture present()
    // draws next is taken now), so the game picture chosen after it is the
    // newest one: present() then shows it at the refresh after, a refresh
    // sooner than a picture chosen before the wait. Window thread; nothing
    // without a window.
    void waitForRefresh();
    // A run without a window (WindowOptions::headless).
    bool headless();

    bool audioReady();
    uint32_t audioDevice(); // SDL_AudioDeviceID, 0 if none
    // SDL's audio driver ("coreaudio", "wasapi", ...; "dummy" when the sound is off: ZAXIS_AUDIO=off or an unpaced
    // run), "" if none.
    const char *audioDriver();
    // Master volume of everything played (0 = silent, 1 = as mixed). Any thread.
    void setAudioGain(float gain);

    // Presentation waits for the display's vsync (the default), or not
    // (frames shown as soon as they are ready, if the GPU driver can).
    void setVsync(bool vsync);
    bool vsync();
    // Variable refresh (G-Sync, FreeSync, adaptive sync, ProMotion): the
    // window shows each game frame when it is ready, and the display
    // follows; off, the window shows the newest frame at each refresh of a
    // fixed rate. Only for a display with variable refresh (on another, a
    // game frame is shown at the next refresh after it is ready, with no
    // phase lock). ZAXIS_VRR=1 at start, or the app's setting.
    void setVariableRefresh(bool on);
    bool variableRefresh();

    // Writes the top-left width x height of an RGBA8 image as an opaque
    // PNG, scaled (bilinear) to outWidth x outHeight if those are not 0.
    bool writePng(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride,
                  uint32_t outWidth = 0, uint32_t outHeight = 0);

    // ---- the map window --------------------------------------------------------
    // A second resizable window beside the game's (the collection's Map
    // Window: the bottom screen, panel/panel.h, on a desktop). It shows one
    // RGBA picture fitted with its aspect kept, and gives back its pointer.
    // Window thread. Nothing in a headless run (openMapWindow says no).
    struct MapWindowRect
    {
        int x = -1; // window coordinates; -1: where the system puts it
        int y = -1;
        int width = 0;
        int height = 0;
    };
    bool openMapWindow(const char *title, const MapWindowRect &rect);
    void closeMapWindow();
    bool mapWindowOpen();
    // Where it is now.
    MapWindowRect mapWindowRect();
    // Shows the picture (RGBA8, stride in bytes).
    void presentMap(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride);
    // The pointer in that window since the last call, in the pixels of the
    // picture last presented (the left button down, moved while down, up;
    // the wheel's turn), and whether its close button was pressed since
    // the last call (the window is closed by then).
    struct MapPointerEvent
    {
        enum Kind
        {
            Down,
            Move,
            Up,
            Wheel,
        } kind = Down;
        float x = 0.0f;
        float y = 0.0f;
        float wheel = 0.0f; // Wheel: up positive
    };
    std::vector<MapPointerEvent> takeMapEvents();
    bool takeMapWindowClosed();
}
