#pragma once

#include <array>
#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

// Host keyboard, mouse and gamepad to DualShock 2 mapping.
//
// Everything here is free of window-system calls, so it can be unit tested
// and driven by an injection script: the host glue (pad.cpp, on the
// SDL3 layer in host_sdl.cpp) only fills RawHostInput from the window
// and applies the cursor capture the mapper asks for.
//
// Host input codes are the GLFW key numbers (as raylib used them, < 512;
// host_sdl.cpp maps SDL scancodes to them) and kMouseCodeBase + mouse
// button (left, right, middle, side, extra, forward, back). Names for the
// config file come from hostCodeByName / hostCodeName.
namespace ps2_host_input
{
    constexpr int kKeyCodeCount = 512;
    constexpr int kMouseCodeBase = kKeyCodeCount;
    constexpr int kMouseButtonCount = 7;
    constexpr int kHostCodeCount = kMouseCodeBase + kMouseButtonCount;
    using HostCodeSet = std::bitset<kHostCodeCount>;

    // Returns -1 for an unknown name. Names are case-insensitive.
    int hostCodeByName(const std::string &name);
    // Canonical name of a code ("" if none).
    std::string hostCodeName(int code);
    // How on-screen prompts name it: "Space", "K", "Left Click", "Shift".
    std::string hostCodeLabel(int code);
    inline bool isMouseCode(int code) { return code >= kMouseCodeBase && code < kHostCodeCount; }

    // DualShock 2 buttons, numbered by their bit in the (active-low) pad
    // button word: SELECT = bit 0 ... SQUARE = bit 15.
    enum PadButton : int
    {
        kSelect = 0,
        kL3,
        kR3,
        kStart,
        kUp,
        kRight,
        kDown,
        kLeft,
        kL2,
        kR2,
        kL1,
        kR1,
        kTriangle,
        kCircle,
        kCross,
        kSquare,
        kPadButtonCount
    };
    const char *padButtonName(int button); // config key, e.g. "cross"

    // Stick directions a key can push. Stick byte order in the pad report:
    // RX, RY, LX, LY (as in the scePadRead data from byte 4).
    enum StickDirection : int
    {
        kLeftUp = 0,
        kLeftDown,
        kLeftLeft,
        kLeftRight,
        kRightUp,
        kRightDown,
        kRightLeft,
        kRightRight,
        kStickDirectionCount
    };
    const char *stickDirectionName(int direction); // config key, e.g. "left_up"

    enum StickByte : int
    {
        kRx = 0,
        kRy,
        kLx,
        kLy
    };

    struct Binding
    {
        int code = -1;
        float value = 1.0f; // button pressure or stick deflection, (0, 1]
    };

    enum class MouseMotion
    {
        None,
        RightStick,
        LeftStick
    };

    enum class MouseMode
    {
        Offset,  // motion pushes the stick and it springs back when the mouse stops
        Velocity // deflection follows mouse speed
    };

    // [mouse] mode: what the mouse does with the camera.
    enum class MouseCamera
    {
        Look, // free mouse-look: motion turns the view (yaw/pitch accumulators, read by a game hook)
        Stick // motion pushes a stick (MouseMotion), filtered by MouseMode
    };

    enum class CaptureMode
    {
        Click,  // capture on a click in the window; that click is not sent to the game
        Always, // capture whenever the window has focus (a release key frees it until the next click)
        Never   // never capture; mouse buttons work, motion is still read as deltas
    };

    // PC shortcuts that are not game buttons ([hotkeys] in controls.ini);
    // what they do is up to a game override (PSPadBackend::hotkeyPresses).
    enum Hotkey : int
    {
        kHotkeyRestart = 0,
        kHotkeyHideHud,
        kHotkeyScreenshot,
        kHotkeyPhotoMode,
        kHotkeySwitchGame, // the collection app: back to the game picker
        kHotkeyCount
    };
    const char *hotkeyName(int hotkey); // config key, e.g. "hide_hud"

    // Button prompts ([gamepad] prompts): follow the device in use, or one
    // style always (Steam Input and remappers present another pad type).
    enum class PromptStyle
    {
        Auto,
        Keyboard,
        Xbox,
        PlayStation,
        Nintendo
    };
    const char *promptStyleName(PromptStyle style); // "auto", "keyboard", ...

    // Each game button pressed by its own gamepad button.
    std::array<std::vector<int>, kPadButtonCount> defaultGamepadButtons();

    // The gamepad layouts the Controls page offers (Gamepad Layout, the
    // [gamepad] bindings in one go). PS2 Original: each game button on its
    // own gamepad button, the defaults. Modern Triggers: L1 / R1 on the
    // triggers and L2 / R2 on the shoulder buttons, every other button where
    // it was; Aggressive Inline's Xbox release lays its controller out so
    // (Rotate on the triggers, Cess slide on the shoulders), and its
    // Tutorial's pictures of the controller show it.
    enum class GamepadLayout
    {
        Ps2Original,
        ModernTriggers,
        Count,
    };
    std::array<std::vector<int>, kPadButtonCount> gamepadLayoutButtons(GamepadLayout layout);
    // The layout `buttons` are, or Count for the player's own (Custom).
    GamepadLayout gamepadLayoutOf(const std::array<std::vector<int>, kPadButtonCount> &buttons);

    struct HostInputConfig
    {
        std::array<std::vector<Binding>, kPadButtonCount> buttons{};
        std::array<std::vector<Binding>, kStickDirectionCount> stick{};
        std::vector<Binding> stickModifier;  // hold for partial key deflection ("walk")
        float stickModifierAmount = 0.5f;

        MouseMotion mouseMotion = MouseMotion::RightStick;
        MouseMode mouseMode = MouseMode::Offset;
        float mouseSensitivityX = 1.0f; // 1.0: 200 pixels push the stick from centre to full
        float mouseSensitivityY = 1.0f;
        bool mouseInvertX = false;
        bool mouseInvertY = true; // mouse up = look up (the camera stick raises the view when pushed up)
        float mouseDecay = 0.85f;    // fraction of the deflection kept per vsync while the mouse is still
        float mouseDeadzone = 0.27f; // smallest deflection sent for any motion (the game ignores |x| < ~0.26)
        CaptureMode capture = CaptureMode::Click;
        std::vector<Binding> releaseKeys;

        // Mouse-look (mouseCamera == Look). Only the game hook that asks for
        // cameraFrame() uses it; without such a hook the mouse does nothing.
        MouseCamera mouseCamera = MouseCamera::Look;
        float lookDegreesPerPixel = 0.2f; // times sensitivity_x / sensitivity_y
        float lookUpLimit = 15.0f;        // degrees above horizontal the view may look
        float lookDownLimit = 75.0f;      // degrees below horizontal
        float lookAutoRecentre = 0.0f;    // seconds without mouse motion before the view recentres; 0 = never
        bool lookSnap = true;             // camera follows mouse motion at once (the game's easing only for its own motion)
        std::vector<Binding> recentreKeys;

        bool gamepadEnabled = true;
        float gamepadStickThreshold = 0.2f; // a gamepad stick past this wins over keys/mouse
        // [gamepad] <button> = <physical buttons>: which gamepad buttons
        // (named by position, gamepadButtonName) press each game button.
        // By default each presses its own (south = cross, ...).
        std::array<std::vector<int>, kPadButtonCount> gamepadButtons = defaultGamepadButtons();
        PromptStyle prompts = PromptStyle::Auto;

        // [hotkeys]: keys per shortcut, and gamepad buttons (by position)
        // held together for it (<name>_pad = L3+R3).
        std::array<std::vector<Binding>, kHotkeyCount> hotkeys{};
        std::array<std::vector<int>, kHotkeyCount> hotkeyPads{};

        // Aggressive Inline's defaults (gameControls("ai")).
        static HostInputConfig defaults();
        // A game's defaults: its documented controls.ini (GameControls).
        static HostInputConfig defaults(const std::string &iniText);
        // Aggressive Inline's documented default controls.ini (parses to defaults()).
        static std::string defaultIniText();
        // Applies an ini file on top of the current values: a key present in
        // the file replaces that binding list (empty value = unbound), absent
        // keys keep their value. Unknown sections, keys and names are reported
        // through warnings and skipped.
        void applyIni(const std::string &text, std::vector<std::string> *warnings = nullptr);
    };

    // A game's controls (game_controls.cpp): its controls.ini as first
    // written, with the keys and buttons put where that game's actions are
    // and their names, and the names of its actions on the Controls rows.
    // Each game has its own controls.ini (the collection: games/<id>/), so
    // rebinding one game leaves the others alone.
    struct GameControls
    {
        std::string id;    // "ai", "dm2", "bmx" (the collection's game ids)
        std::string title; // "Dave Mirra Freestyle BMX 2": the file's header, its runner's config folder
        std::string defaultIni;
        // controls.ini key ("cross", "left_up", "modifier", "recentre",
        // "screenshot") -> what it does in the game ("Pedal"), in the order
        // the rows show them. Keys left out get no row.
        std::vector<std::pair<std::string, std::string>> labels;
    };
    // By id; Aggressive Inline's for an unknown one.
    const GameControls &gameControls(const std::string &id);
    // Every game's, in the collection's order.
    std::vector<const GameControls *> allGameControls();

    // Physical gamepad buttons, identified by the game button they press by
    // default (their PadButton): SOUTH = kCross, EAST = kCircle, WEST =
    // kSquare, NORTH = kTriangle, L1, R1, L2, R2, START, SELECT (Back), L3,
    // R3, DPAD_UP, ... Names are case-insensitive; -1 if unknown.
    int gamepadButtonByName(const std::string &name);
    std::string gamepadButtonName(int physical);

    // "SPACE, K, C*0.5": bindings as controls.ini writes them.
    std::string formatBindings(const std::vector<Binding> &bindings);
    // Sets `key = value` in [section] of an ini text, keeping everything
    // else (comments, order, alignment of the other lines): the first such
    // line is rewritten (its comment after the value is kept), else the
    // line is added at the end of the section, else the section is added.
    std::string setIniValue(const std::string &text, const std::string &section, const std::string &key,
                            const std::string &value);

    // Gamepad as read from the host (already in DualShock 2 terms).
    struct GamepadState
    {
        bool present = false;
        uint16_t buttons = 0u;               // pressed, bit = PadButton
        std::array<float, 4> axes{};          // RX, RY, LX, LY in -1..1 (right/down positive)
    };

    struct RawHostInput
    {
        HostCodeSet down;    // keys and mouse buttons held (including taps since the last sample)
        HostCodeSet pressed; // went down since the last sample
        float mouseDx = 0.0f; // pixels since the last sample
        float mouseDy = 0.0f;
        float wheel = 0.0f; // wheel steps since the last sample (away from the user positive)
        bool focused = true;
        GamepadState gamepad;
    };

    struct PadReport
    {
        uint16_t buttons = 0u; // pressed, bit = PadButton
        std::array<uint8_t, 4> sticks{0x80, 0x80, 0x80, 0x80}; // RX, RY, LX, LY
        std::array<uint8_t, kPadButtonCount> pressure{};       // per PadButton, 0 when released
    };

    // The gamepad part of a pad report, on top of what `report` already has
    // (keys and mouse for port 0, nothing for port 1's own gamepad):
    // [gamepad] button remaps press at full pressure, a stick pushed past
    // stick_threshold replaces that stick. Nothing if the gamepad is off
    // (enabled = false) or absent. Returns whether the right stick was
    // pushed (mouse-look gives the camera back to the game then).
    bool applyGamepad(const HostInputConfig &config, const GamepadState &pad, PadReport &report);

    // Which connected gamepad is which player. Slot 0 is player 1 (pad port
    // 0, with keyboard and mouse), slot 1 player 2 (port 1, split screen);
    // further pads wait as spares. The occupied slots are always 0..n-1.
    //  - A pad that connects takes the next slot (the first pad connected is
    //    player 1, the second player 2).
    //  - A pad keeps its slot while it stays connected.
    //  - A pad that goes away from slot 0 or 1 is replaced by the first
    //    spare if there is one; otherwise the pads after it move up, so
    //    when player 1's pad goes and one pad remains, that pad becomes
    //    player 1 (one player keeps a working gamepad). A pad that comes
    //    back later takes the next free slot.
    //  - swapPlayers() exchanges players 1 and 2 (Settings > Controls).
    // Ids are the host's (SDL joystick instance ids; 0 is never one).
    class GamepadSlots
    {
    public:
        static constexpr int kPlayerSlots = 2;
        // The slot of a newly connected pad (its slot if already known).
        int connect(uint32_t id);
        // False if the id was not known.
        bool disconnect(uint32_t id);
        void swapPlayers();
        void clear() { m_order.clear(); }
        int slotOf(uint32_t id) const; // -1 if unknown
        uint32_t idAt(int slot) const; // 0 if the slot is free
        int count() const { return static_cast<int>(m_order.size()); }
        const std::vector<uint32_t> &order() const { return m_order; }

    private:
        std::vector<uint32_t> m_order; // by slot
    };

    // What a pad port reads. Port 0: keyboard and mouse (controls.ini) plus
    // the gamepad in slot 0 if any; always connected. Port 1: the gamepad
    // in slot 1 alone when there is one, else no pad at all (connected
    // false: the game sees an empty port, as on a PS2 with one controller;
    // a copy of port 0 made games that read both ports, like DM2's cheat
    // sniffer, see every press twice). mirrorPort1 (ZX_PAD_PORT1=mirror,
    // developer runs that drive split screen with one input) gives the old
    // routing: port 1 without its own pad copies port 0's last report.
    // Gamepads off: no slot for any port. Ports past 1 (no multitap): none.
    struct PortSource
    {
        bool connected = false;
        bool keyboardMouse = false;
        int gamepadSlot = -1; // -1: no gamepad
        bool mirrorPort0 = false;
    };
    PortSource routePort(int port, int gamepadCount, bool gamepadEnabled, bool mirrorPort1 = false);

    // Mouse-look state for one camera update (HostInputMapper::cameraFrame).
    // yaw: degrees the view is turned right of the game's own camera
    // direction (-180..180]; pitch: degrees it is tilted up from it.
    struct LookFrame
    {
        bool active = false; // false: leave the game's camera alone
        float yaw = 0.0f;
        float pitch = 0.0f;
        // What the mouse added this frame, when the current camera should
        // follow at once (look_snap): turn it by this before the game eases
        // it towards the new angle.
        float snapYaw = 0.0f;
        float snapPitch = 0.0f;
        float upLimit = 15.0f; // view pitch limits (HostInputConfig::lookUpLimit / lookDownLimit)
        float downLimit = 75.0f;
    };

    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    // Orbits a camera offset (eye - look-at) about the target: the view
    // direction (-offset) turns right by yawDeg about `up` (right-handed,
    // so "right" is forward x up) and tilts up by pitchDeg, with the view
    // pitch kept within [-downLimit, upLimit] degrees of the plane normal to
    // `up`. The distance is kept. Returns false (offset unchanged) for a
    // degenerate offset or up vector.
    bool orbitCameraOffset(Vec3 &offset, const Vec3 &up, float yawDeg, float pitchDeg, float upLimit,
                           float downLimit);
    // View pitch of a camera offset in degrees (looking down < 0).
    float viewPitchOfOffset(const Vec3 &offset, const Vec3 &up);
    // Pulls a camera offset in along itself so the eye is at most maxDrop
    // below the target (along up), the way a camera that meets the floor
    // when looking up moves closer: never shorter than minLength, and never
    // limiting a drop the offset already had (allowedDrop). Returns true if
    // it changed the offset.
    bool limitCameraDrop(Vec3 &offset, const Vec3 &up, float maxDrop, float minLength, float allowedDrop);

    // Unit quaternion, Hamilton convention: rotates v to q v q*.
    struct Quat
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 1.0f;
    };
    Quat quatMultiply(const Quat &a, const Quat &b); // a after b
    Vec3 quatRotate(const Quat &q, const Vec3 &v);
    // The rigid rotation that turns a camera with offset `offset` (eye -
    // target) right by yawDeg and up by pitchDeg, in the sense of
    // orbitCameraOffset: tilt about the horizontal axis normal to the offset,
    // then turn about `up`. Identity for a degenerate offset.
    Quat lookRotation(const Vec3 &offset, const Vec3 &up, float yawDeg, float pitchDeg);

    // Stick byte for a deflection in -1..1, as ZX_PAD_SCRIPT and the
    // gamepad path compute it (centre 0x80).
    uint8_t stickByte(float value);
    // Pressure byte for a pressed button, 1..255.
    uint8_t pressureByte(float value);

    class HostInputMapper
    {
    public:
        HostInputMapper() : HostInputMapper(HostInputConfig::defaults()) {}
        explicit HostInputMapper(const HostInputConfig &config) : m_config(config) {}

        const HostInputConfig &config() const { return m_config; }
        void setConfig(const HostInputConfig &config) { m_config = config; }

        // One pad sample. dtVsyncs is the emulated time since the previous
        // sample (mouse filter timing). Also runs the cursor capture state.
        PadReport map(const RawHostInput &raw, double dtVsyncs);

        bool captured() const { return m_captured; }
        // The window system released the cursor on its own.
        void forceRelease() { m_captured = false; }
        // Mouse stick deflection before the dead zone mapping (tests).
        float mouseStickX() const { return m_mouseX; }
        float mouseStickY() const { return m_mouseY; }

        // What the game is doing when its camera asks for mouse-look.
        enum class LookContext
        {
            Play,   // the mouse turns the view
            Paused, // paused or a message box: motion dropped, the angle kept
            Cut     // scripted camera or a camera cut: motion dropped, recentred
        };

        // Mouse-look, from the game's camera update (once per camera frame,
        // between pad reads). Takes the mouse motion of the last pad read
        // into the yaw/pitch accumulators. basePitch is the view pitch the
        // game's camera wants this frame (degrees, from viewPitchOfOffset);
        // pitch is limited so basePitch + pitch stays within the configured
        // limits. Motion is only taken while the camera runs: a pad read
        // with no camera frame since the previous one (pause menu, scripted
        // camera, loading) drops its motion, and after kLookResetReads such
        // reads the view recentres. Paused (the camera still running behind
        // a menu): the motion is dropped too and the view keeps its angle;
        // that time does not count towards auto_recentre. Cut: recentred.
        LookFrame cameraFrame(float basePitch, LookContext context = LookContext::Play);
        float lookYaw() const { return m_lookYaw; }
        float lookPitch() const { return m_lookPitch; }
        static constexpr int kLookResetReads = 30;

    private:
        void updateCapture(const RawHostInput &raw);
        float filterMouse(float &state, float delta, float sensitivity, double dtVsyncs) const;

        HostInputConfig m_config;
        bool m_captured = false;
        bool m_lastFocused = true;
        bool m_releasedByKey = false; // capture = always: stay released until a click
        HostCodeSet m_swallowed; // mouse buttons held since the capturing click
        float m_mouseX = 0.0f;
        float m_mouseY = 0.0f;

        // Mouse-look (game thread).
        float m_lookYaw = 0.0f;
        float m_lookPitch = 0.0f;
        float m_lookPendingX = 0.0f; // degrees from the last pad read, not yet taken by a camera frame
        float m_lookPendingY = 0.0f;
        bool m_lookRecentre = false;   // recentre key pressed since the last camera frame
        bool m_lookPadStick = false;   // gamepad right stick pushed at the last pad read
        bool m_cameraSinceRead = false;
        int m_readsWithoutCamera = 0;
        double m_lookIdleVsyncs = 0.0;
    };

    // ZX_HOST_INPUT_SCRIPT: scripted host keyboard/mouse/focus events that
    // go through the same mapping as real input. Comma-separated entries:
    //   <t>:KEY=<name>[:<dur>]       hold a key or mouse button (default 0.25 s)
    //   <t>:MOUSE=<dx>/<dy>[:<dur>]  relative mouse motion in pixels: at t, or
    //                                spread evenly over dur
    //   <t>:FOCUS=0|1                window loses / gains focus
    //   <t>:WHEEL=<steps>            mouse wheel steps (away from the user positive)
    // Times and durations are seconds or vsyncs with a "v" suffix, on the
    // ZX_PAD_SCRIPT clock (vsyncs from the first pad read; SYNC/WAIT
    // anchors of a pad script shift it too).
    class HostInputScript
    {
    public:
        struct KeyHold
        {
            double start = 0.0; // vsyncs
            double end = 0.0;
            int code = -1;
        };
        struct Motion
        {
            double start = 0.0;
            double duration = 0.0; // 0: all at start
            float dx = 0.0f;
            float dy = 0.0f;
        };
        struct Focus
        {
            double time = 0.0;
            bool focused = true;
        };
        struct Wheel
        {
            double time = 0.0;
            float steps = 0.0f;
        };

        void parse(const std::string &spec, std::vector<std::string> *warnings = nullptr);
        bool empty() const { return m_keys.empty() && m_motions.empty() && m_focus.empty() && m_wheel.empty(); }

        // Input for the script interval (from, to]: keys held at `to`, keys
        // pressed in the interval, motion in the interval, focus at `to`.
        // Use from < 0 on the first sample.
        RawHostInput sample(double from, double to) const;

        const std::vector<KeyHold> &keys() const { return m_keys; }
        const std::vector<Motion> &motions() const { return m_motions; }
        const std::vector<Focus> &focus() const { return m_focus; }

    private:
        std::vector<KeyHold> m_keys;
        std::vector<Motion> m_motions;
        std::vector<Focus> m_focus;
        std::vector<Wheel> m_wheel;
    };

    // Per-user config directory for an app name:
    // macOS ~/Library/Application Support/<app>, Windows %APPDATA%\<app>,
    // otherwise $XDG_CONFIG_HOME/<app> or ~/.config/<app>.
    // Empty if the home directory is unknown.
    std::string userConfigDir(const std::string &appName);
}
