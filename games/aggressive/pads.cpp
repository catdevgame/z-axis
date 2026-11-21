// The game's pad state (readable round gamehand3): each frame the engine's
// pads are read and copied into the game's records (skater.h PadState, two
// ports at g_pads), with the game's own adjustments: a snapshot of the
// buttons held (options bit 0x02), from which the next frame's presses can
// be measured (bit 0x01), and buttons 0x80 and 0x4000 held off until they
// are let go (bits 0x04, 0x08: a button that closed a menu does not act in
// play). padAxis reads an analog axis. Below them the pads' set-up, a
// skater's controls reset for a run, the button layouts and the Vibration
// option (Options > Controller).

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "aggressive/globals.h"
#include "aggressive/skater/skater.h"
#include "zge/input/pads.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"
#include "sce/libgcc.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/pads.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kMemcpy = sym::memoryCopy;   // (to, from, bytes)

        constexpr uint32_t kPressedFromSnapshot = 0x01;
        constexpr uint32_t kTakeSnapshot = 0x02;
        constexpr uint32_t kHoldOff80 = 0x04;
        constexpr uint32_t kHoldOff4000 = 0x08;
        constexpr uint32_t kConnected = 0x20;
        constexpr uint32_t kNoSnapshot = 0xFFFFFFFFu;

        ee::Ptr<skater::PadState> padOf(uint32_t port)
        {
            return &(*skater::g_pads)[port];
        }

        // The engine's pad, as the game reads it each frame.
        struct PadDevice
        {
            uint32_t _00;
            uint32_t status;       // +0x04 bit 0: connected
            uint32_t held;         // +0x08
            uint32_t pressed;      // +0x0C
            uint32_t _10;
            ee::Float axes[22];    // +0x14 the analog axes and the rest of the block
            uint32_t extraButtons; // +0x6C
        };
        static_assert(offsetof(PadDevice, axes) == 0x14 && offsetof(PadDevice, extraButtons) == 0x6C);

        // PadState's words the skater header has no name for yet.
        // The block a frame's axes are copied into, and from.
        ee::Ptr<ee::Float> axesOf(skater::PadState &pad) { return &pad.axes[0]; }
        ee::Ptr<ee::Float> axesOf(PadDevice &device) { return &device.axes[0]; }

        // Two of Controls' words cleared at a reset: the one only the reset
        // writes, and trickRelease's timer (a word of 0 is 0.0).
        void clearResetWords(ee::Ptr<skater::Controls> controls)
        {
            controls->clearedAtReset = 0;
            controls->releaseTimer = ee::Float(0.0f);
        }

        // One held-off button (`bit` of options, `button` of the buttons):
        // held when the snapshot was taken and still held, it is let through
        // from now on (the bit cleared); otherwise it is taken out of this
        // frame's held and pressed buttons while held, and the bit cleared
        // once it is up.
        void holdOff(uint32_t port, uint32_t bit, uint32_t button)
        {
            const ee::Ptr<skater::PadState> pad = padOf(port);
            const uint32_t options = pad->options;
            if (!(options & bit))
                return;
            const uint32_t snapshot = pad->heldAtSnapshot;
            if (snapshot != kNoSnapshot && (snapshot & button) != 0 && (pad->held & button) != 0)
            {
                pad->options = options & ~bit;
                return;
            }
            const uint32_t held = pad->held;
            if (held & button)
            {
                pad->held = held & ~button;
                pad->pressed = pad->pressed & ~button;
                return;
            }
            pad->pressed = pad->pressed & ~button;
            pad->options = pad->options & ~bit;
        }
    }

    // updatePads: the pads for the frame. The engine reads them, then each
    // port's record gets its device's buttons (held, pressed), its axes and
    // the rest (0x58 bytes from the device's +0x14) and its +0x6C; then the
    // snapshot (taken with buttons 0x80 / 0x4000 left out when they are
    // held off), the held-off buttons, the presses since the snapshot, and
    // the connected bit. Returns 1.
    uint32_t updatePads()
    {
        zge::readPads();
        for (uint32_t port = 0; port < 2; ++port)
        {
            const ee::Ptr<skater::PadState> pad = padOf(port);
            PadDevice &device = *ee::Ptr<PadDevice>(pad->device);
            pad->held = device.held;
            pad->pressed = device.pressed;
            pad->extraButtons = device.extraButtons;
            zge::memoryCopy(axesOf(*pad), axesOf(device), sizeof(pad->axes));
            if (pad->options & kTakeSnapshot)
            {
                const uint32_t held = pad->held;
                pad->heldAtSnapshot = held;
                uint32_t options = pad->options;
                if (options & kHoldOff80)
                {
                    pad->heldAtSnapshot = held & ~0x80u;
                    options = pad->options;
                }
                if (options & kHoldOff4000)
                {
                    pad->heldAtSnapshot = pad->heldAtSnapshot & ~0x4000u;
                    options = pad->options;
                }
                pad->options = options & ~kTakeSnapshot;
            }
            holdOff(port, kHoldOff80, 0x80);
            holdOff(port, kHoldOff4000, 0x4000);
            uint32_t options = pad->options;
            if (options & kPressedFromSnapshot)
            {
                const uint32_t snapshot = pad->heldAtSnapshot;
                if (snapshot != kNoSnapshot)
                {
                    pad->pressed = snapshot ^ pad->held;
                    pad->heldAtSnapshot = kNoSnapshot;
                    options = pad->options;
                }
                pad->options = options & ~kPressedFromSnapshot;
            }
            if (device.status & 1u)
                pad->options = pad->options | kConnected;
            else
                pad->options = pad->options & ~kConnected;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(updatePads, Verified);

    // padAxis: analog axis `axis` of port `port`'s pad (0 x, 1 y of the
    // left stick; 2, 3 the right stick), -1 .. 1.
    ee::Float padAxis(int32_t axis, uint32_t port)
    {
        return axesOf(*padOf(port & 0xFFu))[axis];
    }
    READABLE_FUNCTION(padAxis, Verified);

    // freeCameraFrameDone: empty in this build; updateFreeCameraMode calls it last,
    // after its debug drawing.
    void freeCameraFrameDone()
    {
    }
    READABLE_FUNCTION(freeCameraFrameDone, Verified);

    // shutdownPlayerPads: empty in this build; shutdownGame calls it. Between the
    // pads' other frame functions and matching initPlayerPads [inferred].
    void shutdownPlayerPads()
    {
    }
    READABLE_FUNCTION(shutdownPlayerPads, Verified);

    // The pads' set-up and the players' controller options (Options >
    // Controller: the button layout and vibration per port, kept in the
    // save's flags g_unlocks).
    namespace
    {
        constexpr uint32_t kPadVibrate = sym::padVibrate;   // (pad, small 0..255, big 0..65000, time, 0) (engine)
        constexpr uint32_t kVibrationOption = 0x10u;    // PadState::options: the pad can vibrate
        constexpr uint32_t kNoButtons = 0xFFFFFFFFu;
        inline ee::Global<ee::Float, sym::g_bigMotorScale> g_bigMotorScale; // 65000

        // The save's flags (g_unlocks): per port the button layout (two bits
        // from 20, port 1's from 22) and vibration (bit 24, port 1's 25).
        constexpr uint32_t kLayoutShift[2] = {20, 22};
        constexpr uint32_t kVibrationShift[2] = {24, 25};

        // Controls::stickFlags' low bits, set by resetControls.
        constexpr uint32_t kControlsVibrate = 0x1u; // the Vibration option is on for the pad
        constexpr uint32_t kControlsReset = 0x2u;   // (set at every reset; no reader found)
    }

    uint32_t padLayoutOption(uint32_t port);
    uint32_t padVibrationOption(uint32_t port);

    // initPlayerPads: both ports' records: the engine's pad, nothing held, no
    // snapshot, the vibration option if the pads have motors; zgui reads
    // each.
    void initPlayerPads()
    {
        for (uint32_t port = 0; port < 2; ++port)
        {
            const ee::Ptr<skater::PadState> pad = padOf(port);
            pad->port = port;
            pad->device = ee::Address(zge::newPad(0));
            pad->held = 0;
            pad->pressed = 0;
            for (ee::Float &axis : pad->axes)
                axis = 0.0f;
            pad->heldAtSnapshot = kNoButtons;
            pad->options = 0;
            if (zge::padCanVibrate(ee::Ptr<zge::Pad>(pad->device)))
                pad->options = pad->options | kVibrationOption;
            zge::zgui::setZguiPad(ee::Ptr<zge::zgui::Pad>(pad->device), port);
        }
    }
    // The engine's pad set-up waits on the IOP.
    READABLE_FUNCTION_WAITS(initPlayerPads, Verified);

    // padDeviceBit: bit `bit` of port `port`'s extra buttons: the debug
    // cameras read them.
    uint32_t padDeviceBit(uint32_t bit, uint32_t port)
    {
        return ((*skater::g_pads)[port & 0xFFu].extraButtons >> (bit & 0x1Fu)) & 1u;
    }
    READABLE_FUNCTION(padDeviceBit, Verified);

    // resetControls: a skater's controls cleared for a run on `port`: no
    // stick, history or timers, the windows closed, the port's button layout
    // and vibration option. Returns the stick flags (as the original leaves them).
    int32_t resetControls(ee::Ptr<skater::Controls> controls, uint32_t port)
    {
        port &= 0xFFu;
        controls->stickX = 0.0f;
        controls->stickY = 0.0f;
        clearResetWords(controls);
        controls->holdHistory = 0;
        zge::memoryFill(&controls->history, 0, 8);
        zge::memoryFill(&controls->historyAge, 0, 0x20);
        zge::memoryFill(&controls->historyHeld, 0, 0x20);
        controls->historyCount = 0;
        controls->historyChanged = 1;
        const uint32_t layout = padLayoutOption(port);
        controls->port = static_cast<uint8_t>(port);
        controls->bowlJumpOutTime = 0.0f;
        controls->stallTime = 0.0f;
        controls->wallRideTime = 0.0f;
        controls->poleGrabTime = 0.0f;
        controls->releasedTime = 0.0f;
        controls->cessSlideTime = 0.0f;
        controls->stickFlags = 0;
        controls->layout = static_cast<uint8_t>(layout);
        controls->window100 = -1;
        controls->trickDelay = -1;
        controls->window200 = -1;
        if (padVibrationOption(port))
            controls->stickFlags = controls->stickFlags | kControlsVibrate;
        controls->stickFlags = controls->stickFlags | kControlsReset;
        return static_cast<int32_t>(controls->stickFlags);
    }
    READABLE_FUNCTION(resetControls, Verified);

    // layoutButtonHeld: whether the layout's button 3 is held on the skater's pad.
    int32_t layoutButtonHeld(ee::Ptr<skater::Controls> controls)
    {
        return skater::padHeld((*skater::g_buttonLayouts)[controls->layout].buttons[3], controls->port);
    }
    READABLE_FUNCTION(layoutButtonHeld, Verified);

    // layoutButton: button `index` of layout `layout`.
    uint32_t layoutButton(uint32_t index, uint32_t layout)
    {
        return (*skater::g_buttonLayouts)[layout].buttons[index];
    }
    READABLE_FUNCTION(layoutButton, Verified);

    // buttonLayoutIndex: which of the layout's buttons `button` is (no bound:
    // the caller passes one of them).
    int32_t buttonLayoutIndex(uint32_t button, uint32_t layout)
    {
        const uint32_t *buttons = (*skater::g_buttonLayouts)[layout].buttons;
        int32_t index = 0;
        while (buttons[index] != button)
            ++index;
        return index;
    }
    READABLE_FUNCTION(buttonLayoutIndex, Verified);

    namespace
    {
        namespace tail
        {
        int32_t padVibrate(uint32_t pad, uint32_t smallMotor, uint32_t bigMotor, uint32_t time, uint32_t tag) { return ee::tailCall<int32_t>(kPadVibrate, pad, smallMotor, bigMotor, time, tag); }
        }
    }

    // vibrate: the skater's pad set vibrating for `time` (the small
    // motor at `small` 0 .. 1, the big one at `big`), if it can and the
    // player's Vibration option is on.
    int32_t vibrate(ee::Ptr<skater::Controls> controls, uint32_t time, ee::Float small, ee::Float big)
    {
        const ee::Ptr<skater::PadState> pad = padOf(controls->port);
        if (!(pad->options & kVibrationOption))
            return 0;
        if (!(controls->stickFlags & kControlsVibrate))
            return 0;
        const uint32_t smallLevel = sce::libgcc::fptoui(ee::toFloat(ee::toInt(small * ee::Float(255.0f))));
        const uint32_t bigLevel = sce::libgcc::fptoui(ee::toFloat(ee::toInt(big * *g_bigMotorScale)));
        return tail::padVibrate(pad->device, smallLevel, bigLevel, time, 0); // the original's tail call
    }
    READABLE_FUNCTION(vibrate, Verified);

    // padLayoutOption: the button layout saved for port 0 or 1 (0 for others).
    uint32_t padLayoutOption(uint32_t port)
    {
        port &= 0xFFu;
        if (port > 1)
            return 0;
        const uint32_t layout = static_cast<uint32_t>(*g_unlocks >> kLayoutShift[port]) & 3u;
        return layout;
    }
    READABLE_FUNCTION(padLayoutOption, Verified);

    // setControllerConfigLayout: the button layout saved for port 0 or 1. Returns
    // the save's flags (1 for other ports).
    uint64_t setControllerConfigLayout(uint32_t port, uint32_t layout)
    {
        port &= 0xFFu;
        if (port > 1)
            return 1;
        const uint64_t flags = (*g_unlocks & ~(3ull << kLayoutShift[port])) | (static_cast<uint64_t>(layout & 3u) << kLayoutShift[port]);
        *g_unlocks = flags;
        return flags;
    }
    READABLE_FUNCTION(setControllerConfigLayout, Verified);

    // padVibrationOption: the Vibration option saved for port 0 or 1 (1 for others).
    uint32_t padVibrationOption(uint32_t port)
    {
        port &= 0xFFu;
        if (port > 1)
            return 1;
        return static_cast<uint32_t>(*g_unlocks >> kVibrationShift[port]) & 1u;
    }
    READABLE_FUNCTION(padVibrationOption, Verified);

    // setControllerConfigVibration: the Vibration option saved for port 0 or 1.
    // Returns the save's flags (1 for other ports).
    uint64_t setControllerConfigVibration(uint32_t port, uint32_t on)
    {
        port &= 0xFFu;
        if (port > 1)
            return 1;
        const uint64_t flags = (*g_unlocks & ~(1ull << kVibrationShift[port])) | (static_cast<uint64_t>(on != 0) << kVibrationShift[port]);
        *g_unlocks = flags;
        return flags;
    }
    READABLE_FUNCTION(setControllerConfigVibration, Verified);
}
