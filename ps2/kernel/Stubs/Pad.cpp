#include "Common.h"
#include "Pad.h"
#include "runtime/state_probe.h"
#include "runtime/boot_config.h"

namespace ps2_stubs
{
    namespace
    {
        constexpr uint8_t kPadModeDigital = 0x41;
        constexpr uint8_t kPadModeDualShock = 0x73;
        constexpr uint8_t kPadModeDualShock2Pressure = 0x79;
        constexpr uint8_t kPadAnalogCenter = 0x80;
        constexpr int32_t kPadTypeDigital = 4;
        constexpr int32_t kPadTypeDualShock = 7;
        constexpr int32_t kPadStateDisconnected = 0;
        constexpr int32_t kPadStateExecCmd = 5;
        constexpr int32_t kPadStateStable = 6;
        constexpr size_t kPadPortCount = 2;
        constexpr size_t kPadSlotCount = 1;
        constexpr size_t kPadActuatorBytes = 6;
        // scePadSetActAlign values: actuator 0 is the small motor (on/off),
        // 1 the large one (0-255), 0xFF an unused byte. Default: the
        // DualShock layout games set, in case one never calls SetActAlign.
        constexpr uint8_t kPadActSmall = 0x00;
        constexpr uint8_t kPadActLarge = 0x01;
        constexpr uint8_t kPadActNone = 0xFF;

        constexpr uint16_t kPadBtnSelect = 1u << 0;
        constexpr uint16_t kPadBtnL3 = 1u << 1;
        constexpr uint16_t kPadBtnR3 = 1u << 2;
        constexpr uint16_t kPadBtnStart = 1u << 3;
        constexpr uint16_t kPadBtnUp = 1u << 4;
        constexpr uint16_t kPadBtnRight = 1u << 5;
        constexpr uint16_t kPadBtnDown = 1u << 6;
        constexpr uint16_t kPadBtnLeft = 1u << 7;
        constexpr uint16_t kPadBtnL2 = 1u << 8;
        constexpr uint16_t kPadBtnR2 = 1u << 9;
        constexpr uint16_t kPadBtnL1 = 1u << 10;
        constexpr uint16_t kPadBtnR1 = 1u << 11;
        constexpr uint16_t kPadBtnTriangle = 1u << 12;
        constexpr uint16_t kPadBtnCircle = 1u << 13;
        constexpr uint16_t kPadBtnCross = 1u << 14;
        constexpr uint16_t kPadBtnSquare = 1u << 15;

        struct PadInputState
        {
            uint16_t buttons = 0xFFFF; // active-low
            uint8_t rx = kPadAnalogCenter;
            uint8_t ry = kPadAnalogCenter;
            uint8_t lx = kPadAnalogCenter;
            uint8_t ly = kPadAnalogCenter;
            // Pressure of pressed buttons in DualShock 2 data order (right,
            // left, up, down, triangle, circle, cross, square, L1, R1, L2,
            // R2), from the pad backend; without it a press is full (0xFF).
            bool hasPressure = false;
            uint8_t pressure[12]{};
        };

        struct PadPortState
        {
            bool open = false;
            bool plugged = true; // a pad in the port at the last check (pluggedIn)
            bool analogMode = false;  // real pads power up DIGITAL (CURID=4, mode 0x41)
            bool pressureEnabled = false;
            bool lastUsedOverride = false;
            bool lastUsedBackend = false;
            bool lastReadOk = false;
            uint16_t buttonMask = 0xFFFFu;
            uint32_t dmaAddr = 0u;
            uint32_t reqState = 0u;
            uint32_t transientState = 0u;
            PadInputState lastInput{};
            uint8_t lastData[32]{};
            uint32_t readCount = 0u;
            uint32_t lastReadDataAddr = 0u;
            uint8_t actAlign[kPadActuatorBytes]{kPadActSmall, kPadActLarge, kPadActNone, kPadActNone, kPadActNone, kPadActNone};
            uint8_t motorSmall = 0u;
            uint8_t motorLarge = 0u;

            void resetActuators()
            {
                const uint8_t defaults[kPadActuatorBytes]{kPadActSmall, kPadActLarge, kPadActNone, kPadActNone, kPadActNone, kPadActNone};
                std::memcpy(actAlign, defaults, sizeof(actAlign));
                motorSmall = 0u;
                motorLarge = 0u;
            }
        };

        std::mutex g_padOverrideMutex;
        std::mutex g_padStateMutex;
        bool g_padOverrideEnabled = false;
        PadInputState g_padOverrideState{};
        PadPortState g_padPorts[kPadPortCount]{};
        int g_padReadLogCount = 0;

        uint8_t axisToByte(float axis)
        {
            axis = std::clamp(axis, -1.0f, 1.0f);
            const float mapped = (axis + 1.0f) * 127.5f;
            return static_cast<uint8_t>(std::lround(mapped));
        }

        void setButton(PadInputState &state, uint16_t mask, bool pressed)
        {
            if (pressed)
            {
                state.buttons = static_cast<uint16_t>(state.buttons & ~mask);
            }
        }

        // Host keys (GLFW numbering, ps2_host_input) of the last host frame.
        enum : int
        {
            kKeyA = 65,
            kKeyC = 67,
            kKeyD = 68,
            kKeyE = 69,
            kKeyQ = 81,
            kKeyS = 83,
            kKeyV = 86,
            kKeyW = 87,
            kKeyX = 88,
            kKeyZ = 90,
            kKey1 = 49,
            kKey3 = 51,
            kKeyEnter = 257,
            kKeyRight = 262,
            kKeyLeft = 263,
            kKeyDown = 264,
            kKeyUp = 265,
            kKeyLeftControl = 341,
            kKeyRightShift = 344,
            kKeyRightControl = 345,
        };

        // Fallback when the pad backend has no report: the host gamepad and
        // a fixed keyboard layout.
        void applyGamepadState(PadInputState &state)
        {
            if (!ps2_host::windowReady())
            {
                return;
            }

            const ps2_host_input::GamepadState pad = ps2_host::inputSnapshot().gamepad;
            if (!pad.present)
            {
                return;
            }

            namespace hi = ps2_host_input;
            state.lx = axisToByte(pad.axes[hi::kLx]);
            state.ly = axisToByte(pad.axes[hi::kLy]);
            state.rx = axisToByte(pad.axes[hi::kRx]);
            state.ry = axisToByte(pad.axes[hi::kRy]);
            auto down = [&pad](int bit)
            { return (pad.buttons & (1u << bit)) != 0; };
            setButton(state, kPadBtnUp, down(hi::kUp));
            setButton(state, kPadBtnDown, down(hi::kDown));
            setButton(state, kPadBtnLeft, down(hi::kLeft));
            setButton(state, kPadBtnRight, down(hi::kRight));
            setButton(state, kPadBtnCross, down(hi::kCross));
            setButton(state, kPadBtnCircle, down(hi::kCircle));
            setButton(state, kPadBtnSquare, down(hi::kSquare));
            setButton(state, kPadBtnTriangle, down(hi::kTriangle));
            setButton(state, kPadBtnL1, down(hi::kL1));
            setButton(state, kPadBtnR1, down(hi::kR1));
            setButton(state, kPadBtnL2, down(hi::kL2));
            setButton(state, kPadBtnR2, down(hi::kR2));
            setButton(state, kPadBtnL3, down(hi::kL3));
            setButton(state, kPadBtnR3, down(hi::kR3));
            setButton(state, kPadBtnSelect, down(hi::kSelect));
            setButton(state, kPadBtnStart, down(hi::kStart));
        }

        void applyKeyboardState(PadInputState &state, bool allowAnalog)
        {
            if (!ps2_host::windowReady())
            {
                return;
            }

            // Keyboard mapping (PS2 -> keys):
            // D-Pad: arrows, Square/Cross/Circle/Triangle: Z/X/C/V
            // L1/R1: Q/E, L2/R2: 1/3, Start/Select: Enter/RightShift
            // L3/R3: LeftCtrl/RightCtrl, Analog left: WASD
            const ps2_host_input::HostCodeSet held = ps2_host::inputSnapshot().held;
            auto key = [&held](int code)
            { return held.test(static_cast<size_t>(code)); };
            setButton(state, kPadBtnUp, key(kKeyUp));
            setButton(state, kPadBtnDown, key(kKeyDown));
            setButton(state, kPadBtnLeft, key(kKeyLeft));
            setButton(state, kPadBtnRight, key(kKeyRight));

            setButton(state, kPadBtnSquare, key(kKeyZ));
            setButton(state, kPadBtnCross, key(kKeyX));
            setButton(state, kPadBtnCircle, key(kKeyC));
            setButton(state, kPadBtnTriangle, key(kKeyV));

            setButton(state, kPadBtnL1, key(kKeyQ));
            setButton(state, kPadBtnR1, key(kKeyE));
            setButton(state, kPadBtnL2, key(kKey1));
            setButton(state, kPadBtnR2, key(kKey3));

            setButton(state, kPadBtnStart, key(kKeyEnter));
            setButton(state, kPadBtnSelect, key(kKeyRightShift));
            setButton(state, kPadBtnL3, key(kKeyLeftControl));
            setButton(state, kPadBtnR3, key(kKeyRightControl));

            if (!allowAnalog)
            {
                return;
            }

            float ax = 0.0f;
            float ay = 0.0f;
            if (key(kKeyD))
                ax += 1.0f;
            if (key(kKeyA))
                ax -= 1.0f;
            if (key(kKeyS))
                ay += 1.0f;
            if (key(kKeyW))
                ay -= 1.0f;

            if (ax != 0.0f || ay != 0.0f)
            {
                state.lx = axisToByte(ax);
                state.ly = axisToByte(ay);
            }
        }

        void resetPadStateLocked()
        {
            for (PadPortState &portState : g_padPorts)
            {
                portState = PadPortState{};
            }
        }

        PadPortState *lookupPadPortStateLocked(int port, int slot)
        {
            if (port < 0 || port >= static_cast<int>(kPadPortCount))
            {
                return nullptr;
            }
            if (slot < 0 || slot >= static_cast<int>(kPadSlotCount))
            {
                return nullptr;
            }
            return &g_padPorts[port];
        }

        void initializePadPortLocked(PadPortState &portState, uint32_t dmaAddr)
        {
            portState.open = true;
            portState.analogMode = false;  // real pads open DIGITAL
            portState.pressureEnabled = false;
            portState.buttonMask = 0xFFFFu;
            portState.dmaAddr = dmaAddr;
            portState.reqState = 0u;
            portState.transientState = 0u;
        }

        // Whether a pad is in the port (PSPadBackend::portConnected: port 1
        // only with player 2's own gamepad, as on a PS2 with one controller
        // the second port is empty). Called before g_padStateMutex is taken.
        bool pluggedIn(int port)
        {
            return port >= 0 && port < static_cast<int>(kPadPortCount) && PSPadBackend::portConnected(port);
        }

        // Records the port's pad state; a pad that went away loses what the
        // game set up (a new one powers up digital, motors off), so the game
        // redoes its setup when one comes back, as with a real pad.
        void notePluggedLocked(PadPortState &portState, int port, bool plugged)
        {
            if (portState.plugged == plugged)
                return;
            portState.plugged = plugged;
            if (plugged)
                return;
            portState.analogMode = false;
            portState.pressureEnabled = false;
            portState.buttonMask = 0xFFFFu;
            portState.reqState = 0u;
            portState.transientState = 0u;
            if (portState.motorSmall != 0u || portState.motorLarge != 0u)
                ps2_host::setRumble(port, 0u, 0u);
            portState.resetActuators();
        }

        // An open port with a pad in it (the setup calls fail on an empty
        // port, as libpad's do while there is no pad).
        PadPortState *lookupPluggedPortLocked(int port, int slot, bool plugged)
        {
            PadPortState *portState = lookupPadPortStateLocked(port, slot);
            if (!portState)
                return nullptr;
            notePluggedLocked(*portState, port, plugged);
            return portState->open && plugged ? portState : nullptr;
        }

        void queueExecCmdStateLocked(PadPortState &portState)
        {
            portState.transientState = static_cast<uint32_t>(kPadStateExecCmd);
        }

        uint8_t pressureValue(const PadInputState &state, const PadPortState &portState, uint16_t mask, int slot)
        {
            if (!portState.pressureEnabled)
            {
                return 0u;
            }
            if ((portState.buttonMask & mask) == 0u)
            {
                return 0u;
            }
            if ((state.buttons & mask) != 0u)
            {
                return 0u;
            }
            return state.hasPressure ? state.pressure[slot] : 0xFFu;
        }

        void fillPadStatus(uint8_t *data, const PadInputState &state, const PadPortState &portState)
        {
            std::memset(data, 0, 32);
            // A DualShock 2 in pressure mode reports 0x79 (analog, 9 extra half-words).
            data[1] = portState.analogMode ? (portState.pressureEnabled ? kPadModeDualShock2Pressure : kPadModeDualShock)
                                           : kPadModeDigital;
            data[2] = static_cast<uint8_t>(state.buttons & 0xFFu);
            data[3] = static_cast<uint8_t>((state.buttons >> 8) & 0xFFu);
            data[4] = state.rx;
            data[5] = state.ry;
            data[6] = state.lx;
            data[7] = state.ly;
            data[8] = pressureValue(state, portState, kPadBtnRight, 0);
            data[9] = pressureValue(state, portState, kPadBtnLeft, 1);
            data[10] = pressureValue(state, portState, kPadBtnUp, 2);
            data[11] = pressureValue(state, portState, kPadBtnDown, 3);
            data[12] = pressureValue(state, portState, kPadBtnTriangle, 4);
            data[13] = pressureValue(state, portState, kPadBtnCircle, 5);
            data[14] = pressureValue(state, portState, kPadBtnCross, 6);
            data[15] = pressureValue(state, portState, kPadBtnSquare, 7);
            // DualShock 2 order: L1, R1, L2, R2 (as in PCSX2's PadDualshock2::Poll).
            data[16] = pressureValue(state, portState, kPadBtnL1, 8);
            data[17] = pressureValue(state, portState, kPadBtnR1, 9);
            data[18] = pressureValue(state, portState, kPadBtnL2, 10);
            data[19] = pressureValue(state, portState, kPadBtnR2, 11);
        }

        bool readPadPortData(int port, int slot, PS2Machine *runtime, uint8_t *outData, uint32_t dataAddr)
        {
            if (!outData)
            {
                return false;
            }

            const bool plugged = pluggedIn(port);
            PadPortState portState;
            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                const PadPortState *sharedPortState = lookupPluggedPortLocked(port, slot, plugged);
                if (!sharedPortState)
                {
                    return false;
                }
                portState = *sharedPortState;
            }

            PadInputState state;
            bool useOverride = false;
            {
                std::lock_guard<std::mutex> lock(g_padOverrideMutex);
                if (g_padOverrideEnabled)
                {
                    state = g_padOverrideState;
                    useOverride = true;
                }
            }

            bool usedBackend = false;
            if (!useOverride)
            {
                uint8_t backendData[32]{};
                if (runtime && runtime->padBackend().readState(port, slot, backendData, sizeof(backendData)))
                {
                    state.buttons = static_cast<uint16_t>(backendData[2] | (backendData[3] << 8));
                    state.rx = backendData[4];
                    state.ry = backendData[5];
                    state.lx = backendData[6];
                    state.ly = backendData[7];
                    state.hasPressure = true;
                    std::memcpy(state.pressure, backendData + 8, sizeof(state.pressure));
                    usedBackend = true;
                }
                else
                {
                    applyGamepadState(state);
                    applyKeyboardState(state, portState.analogMode);
                }
            }

            fillPadStatus(outData, state, portState);

            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                if (PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot))
                {
                    sharedPortState->lastInput = state;
                    std::memcpy(sharedPortState->lastData, outData, sizeof(sharedPortState->lastData));
                    sharedPortState->lastUsedOverride = useOverride;
                    sharedPortState->lastUsedBackend = usedBackend;
                    sharedPortState->lastReadOk = true;
                    sharedPortState->lastReadDataAddr = dataAddr;
                    ++sharedPortState->readCount;
                }
            }

            return true;
        }
    }

    void PadSyncCallback(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void scePadEnd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadEnterPressMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)runtime;
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const bool plugged = pluggedIn(port);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPluggedPortLocked(port, static_cast<int>(getRegU32(ctx, 5)), plugged);
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->pressureEnabled = true;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadExitPressMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)runtime;
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const bool plugged = pluggedIn(port);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPluggedPortLocked(port, static_cast<int>(getRegU32(ctx, 5)), plugged);
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->pressureEnabled = false;
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadGetButtonMask(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint16_t mask = portState ? portState->buttonMask : 0xFFFFu;
        setReturnS32(ctx, static_cast<int32_t>(mask));
    }

    void scePadGetDmaStr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint32_t dmaAddr = portState ? portState->dmaAddr : getRegU32(ctx, 6);
        setReturnU32(ctx, dmaAddr);
    }

    void scePadGetFrameCount(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        static std::atomic<uint32_t> frameCount{0};
        setReturnU32(ctx, frameCount++);
    }

    void scePadGetModVersion(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Arbitrary non-zero module version.
        setReturnS32(ctx, 0x0200);
    }

    void scePadGetPortMax(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 2);
    }

    void scePadGetReqState(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, static_cast<int32_t>(portState ? portState->reqState : 0u));
    }

    void scePadGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Most games use one slot unless multitap is active.
        setReturnS32(ctx, 1);
    }

    void scePadGetState(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        // An empty port (port 1 without player 2's gamepad) reads
        // DISCONNECTED, as on a PS2: games that poll both ports (DM2's
        // cheat sniffer, AI's controller count) see one pad.
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const bool plugged = pluggedIn(port);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPluggedPortLocked(port, static_cast<int>(getRegU32(ctx, 5)), plugged);
        int32_t state = kPadStateDisconnected;
        if (portState)
        {
            if (portState->transientState != 0u)
            {
                state = static_cast<int32_t>(portState->transientState);
                portState->transientState = 0u;
            }
            else
            {
                state = kPadStateStable;
            }
        }
        setReturnS32(ctx, state);
    }

    void scePadInfoAct(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        const int32_t act = static_cast<int32_t>(getRegU32(ctx, 6));
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const bool plugged = pluggedIn(port);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPluggedPortLocked(port, static_cast<int>(getRegU32(ctx, 5)), plugged);
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (act < 0)
        {
            setReturnS32(ctx, 2); // small + large motors
            return;
        }
        setReturnS32(ctx, (act < 2) ? 1 : 0);
    }

    void scePadInfoComb(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        // No combined modes reported.
        setReturnS32(ctx, 0);
    }

    void scePadInfoMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;

        const int32_t infoMode = static_cast<int32_t>(getRegU32(ctx, 6)); // a2
        const int32_t index = static_cast<int32_t>(getRegU32(ctx, 7));    // a3
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const bool plugged = pluggedIn(port);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPluggedPortLocked(port, static_cast<int>(getRegU32(ctx, 5)), plugged);
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const int32_t currentId = portState->analogMode ? kPadTypeDualShock : kPadTypeDigital;
        switch (infoMode)
        {
        case 1: // PAD_MODECURID
            setReturnS32(ctx, currentId);
            return;
        case 2: // PAD_MODECUREXID
            setReturnS32(ctx, currentId);
            return;
        case 3: // PAD_MODECUROFFS
            setReturnS32(ctx, 0);
            return;
        case 4: // PAD_MODETABLE
            if (index == -1)
            {
                setReturnS32(ctx, 1); // one available mode
            }
            else if (index == 0)
            {
                setReturnS32(ctx, currentId);
            }
            else
            {
                setReturnS32(ctx, 0);
            }
            return;
        default:
            setReturnS32(ctx, 0);
            return;
        }
    }

    void scePadInfoPressMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const bool plugged = pluggedIn(port);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPluggedPortLocked(port, static_cast<int>(getRegU32(ctx, 5)), plugged);
        setReturnS32(ctx, portState ? 1 : 0);
    }

    void scePadInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadInit2(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        scePadInit(rdram, ctx, runtime);
    }

    void scePadPortClose(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = false;
        portState->pressureEnabled = false;
        portState->reqState = 0u;
        portState->transientState = 0u;
        if (portState->motorSmall != 0u || portState->motorLarge != 0u)
            ps2_host::setRumble(static_cast<int>(portState - g_padPorts), 0u, 0u);
        portState->motorSmall = 0u;
        portState->motorLarge = 0u;
        setReturnS32(ctx, 1);
    }

    void scePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)runtime;
        const uint32_t dmaAddr = getRegU32(ctx, 6);
        uint8_t *dmaStr = getMemPtr(rdram, dmaAddr);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || (dmaAddr != 0u && !dmaStr))
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = true;
        portState->resetActuators();
        portState->analogMode = false;     // real pads open DIGITAL
        portState->pressureEnabled = false;
        portState->buttonMask = 0xFFFFu;
        portState->dmaAddr = dmaAddr;
        portState->reqState = 0u;
        portState->transientState = 0u;
        if (dmaStr)
        {
            ps2TraceGuestRangeWrite(rdram, dmaAddr, 32u, "scePadPortOpen", ctx);
            std::memset(dmaStr, 0, 32);
        }
        setReturnS32(ctx, 1);
    }

    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const int slot = static_cast<int>(getRegU32(ctx, 5));
        const uint32_t dataAddr = getRegU32(ctx, 6);
        uint8_t *data = getMemPtr(rdram, dataAddr);
        if (!data)
        {
            setReturnS32(ctx, 0);
            return;
        }

        ps2TraceGuestRangeWrite(rdram, dataAddr, 32u, "scePadRead", ctx);
        if (!readPadPortData(port, slot, runtime, data, dataAddr))
        {
            // No pad (or the port not open): libpad's failed read, nothing
            // pressed.
            std::memset(data, 0, 32);
            data[0] = 0xFFu;
            data[2] = 0xFFu;
            data[3] = 0xFFu;
            setReturnS32(ctx, 0);
            return;
        }

        if (port == 0 && slot == 0)
        {
            // ZX_PAD_TRACE=<file>: the buttons and sticks the game read, a
            // line "<vsync tick> <buttons> <rx> <ry> <lx> <ly>" (hex, the
            // pad's bytes) at each change: tools/checks/career_ab.py makes a pad
            // script of a closed-loop route's run, for an A/B tick by tick.
            static std::FILE *const padTrace = []() -> std::FILE *
            {
                const char *path = std::getenv("ZX_PAD_TRACE");
                return path && path[0] ? std::fopen(path, "w") : nullptr;
            }();
            if (padTrace && runtime)
            {
                static uint8_t last[6] = {};
                if (std::memcmp(last, data + 2, sizeof(last)) != 0)
                {
                    std::memcpy(last, data + 2, sizeof(last));
                    std::fprintf(padTrace, "%llu %02x%02x %02x %02x %02x %02x\n",
                                 static_cast<unsigned long long>(runtime->memory().gs().vsyncTick.load(std::memory_order_acquire)),
                                 data[2], data[3], data[4], data[5], data[6], data[7]);
                    std::fflush(padTrace);
                }
            }
            // ZX_PAD_LOG / ZX_PAD_DUMP (runtime/state_probe.h).
            static zx::boot::PerBoot<StateProbe> padProbeOfBoot([]()
            {
                StateProbe configured;
                configured.configurePadReadFromEnvironment();
                return configured;
            });
            StateProbe &padProbe = padProbeOfBoot.get();
            if (padProbe.enabled() && runtime)
            {
                const uint64_t tick = runtime->memory().gs().vsyncTick.load(std::memory_order_acquire);
                padProbe.onPadRead(rdram, tick, [rdram](const std::string &label, uint64_t dumpTick)
                                   { StateProbe::writeRamDump(rdram, label, dumpTick); });
            }
        }

        PS2_IF_AGRESSIVE_LOGS({
            if (g_padReadLogCount < 48)
            {
                const ps2_host::InputSnapshot host = ps2_host::inputSnapshot();
                const bool enterPressed = host.held.test(kKeyEnter);
                const bool gamepadStartPressed =
                    host.gamepad.present && (host.gamepad.buttons & (1u << ps2_host_input::kStart)) != 0;
                const bool startPressed = (data[2] != 0xFFu || data[3] != 0xFFu ||
                                           enterPressed || gamepadStartPressed);
                if (startPressed)
                {
                    const uint32_t guestButtons =
                        (static_cast<uint32_t>(static_cast<uint8_t>(data[2] ^ 0xFFu)) << 8) |
                        static_cast<uint32_t>(static_cast<uint8_t>(data[3] ^ 0xFFu));
                    std::printf("[padread] port=%d slot=%d data2=0x%02x data3=0x%02x guestButtons=0x%04x enter=%d gamepadStart=%d\n",
                                port, slot, data[2], data[3], guestButtons,
                                enterPressed ? 1 : 0, gamepadStartPressed ? 1 : 0);
                    ++g_padReadLogCount;
                }
            }
        });

        setReturnS32(ctx, 1);
    }

    void scePadReqIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = (state == 0) ? "COMPLETE" : "BUSY";
        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    void scePadSetActAlign(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)runtime;
        const uint8_t *align = getConstMemPtr(rdram, getRegU32(ctx, 6));
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const bool plugged = pluggedIn(port);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPluggedPortLocked(port, static_cast<int>(getRegU32(ctx, 5)), plugged);
        if (!portState || !align)
        {
            setReturnS32(ctx, 0);
            return;
        }
        std::memcpy(portState->actAlign, align, kPadActuatorBytes);
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    // Vibration: the motor values go to the port's host gamepad (port n
    // drives the gamepad in slot n, ps2_host::setRumble; port 1 vibrates
    // nothing while it has no gamepad of its own). The game only sends
    // non-zero values when its own Vibration option is on.
    void scePadSetActDirect(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)runtime;
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const uint8_t *data = getConstMemPtr(rdram, getRegU32(ctx, 6));
        const bool plugged = pluggedIn(port);
        uint8_t small = 0u;
        uint8_t large = 0u;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            PadPortState *portState = lookupPluggedPortLocked(port, static_cast<int>(getRegU32(ctx, 5)), plugged);
            if (!portState || !data)
            {
                setReturnS32(ctx, 0);
                return;
            }
            for (size_t i = 0; i < kPadActuatorBytes; ++i)
            {
                if (portState->actAlign[i] == kPadActSmall)
                    small = data[i] != 0u ? 1u : 0u;
                else if (portState->actAlign[i] == kPadActLarge)
                    large = data[i];
            }
            static const bool trace = std::getenv("ZX_TRACE_RUMBLE") != nullptr;
            if (trace && (portState->motorSmall != small || portState->motorLarge != large))
                std::printf("[rumble] port=%d small=%u large=%u\n", port, small, large);
            portState->motorSmall = small;
            portState->motorLarge = large;
        }
        ps2_host::setRumble(port, static_cast<uint16_t>(large * 257u), small ? 0xFFFFu : 0u);
        setReturnS32(ctx, 1);
    }

    void scePadSetButtonInfo(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->buttonMask = static_cast<uint16_t>(getRegU32(ctx, 6));
            portState->reqState = 0u;
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetMainMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const bool plugged = pluggedIn(port);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPluggedPortLocked(port, static_cast<int>(getRegU32(ctx, 5)), plugged);
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->analogMode = (getRegU32(ctx, 6) != 0u);
        portState->reqState = 0u;
        queueExecCmdStateLocked(*portState);
        setReturnS32(ctx, 1);
    }

    void scePadSetReqState(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->reqState = static_cast<uint32_t>(getRegU32(ctx, 6) ? 1u : 0u);
            queueExecCmdStateLocked(*portState);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetVrefParam(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetWarningLevel(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void scePadStateIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = "UNKNOWN";
        if (state == 6)
        {
            text = "STABLE";
        }
        else if (state == 1)
        {
            text = "FINDPAD";
        }
        else if (state == 5)
        {
            text = "EXECCMD";
        }
        else if (state == 0)
        {
            text = "DISCONNECTED";
        }

        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    PadDebugSnapshot getPadDebugSnapshot()
    {
        PadDebugSnapshot snapshot{};
        {
            std::lock_guard<std::mutex> lock(g_padOverrideMutex);
            snapshot.overrideEnabled = g_padOverrideEnabled;
            snapshot.overrideButtons = g_padOverrideState.buttons;
            snapshot.overrideRx = g_padOverrideState.rx;
            snapshot.overrideRy = g_padOverrideState.ry;
            snapshot.overrideLx = g_padOverrideState.lx;
            snapshot.overrideLy = g_padOverrideState.ly;
        }

        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            snapshot.readLogCount = g_padReadLogCount;
            for (size_t port = 0; port < kPadDebugPortCount; ++port)
            {
                for (size_t slot = 0; slot < kPadDebugSlotCount; ++slot)
                {
                    const PadPortState &src = g_padPorts[port];
                    PadDebugPortSnapshot &dst = snapshot.ports[port][slot];
                    dst.open = src.open;
                    dst.analogMode = src.analogMode;
                    dst.pressureEnabled = src.pressureEnabled;
                    dst.lastUsedOverride = src.lastUsedOverride;
                    dst.lastUsedBackend = src.lastUsedBackend;
                    dst.lastReadOk = src.lastReadOk;
                    dst.buttonMask = src.buttonMask;
                    dst.lastButtons = src.lastInput.buttons;
                    dst.dmaAddr = src.dmaAddr;
                    dst.reqState = src.reqState;
                    dst.readCount = src.readCount;
                    dst.lastReadDataAddr = src.lastReadDataAddr;
                    dst.motorSmall = src.motorSmall;
                    dst.motorLarge = src.motorLarge;
                    dst.rx = src.lastInput.rx;
                    dst.ry = src.lastInput.ry;
                    dst.lx = src.lastInput.lx;
                    dst.ly = src.lastInput.ly;
                    std::memcpy(dst.lastData, src.lastData, sizeof(dst.lastData));
                }
            }
        }
        return snapshot;
    }

    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = true;
        g_padOverrideState.buttons = buttons;
        g_padOverrideState.lx = lx;
        g_padOverrideState.ly = ly;
        g_padOverrideState.rx = rx;
        g_padOverrideState.ry = ry;
    }

    void clearPadOverrideState()
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = false;
        g_padOverrideState = PadInputState{};
    }
}
