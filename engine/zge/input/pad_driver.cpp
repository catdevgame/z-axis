// The PS2 pad driver (zge/input/pads.h; in the ps2Movie.c block,
// translatePadButtons-updatePadVibration): four port records over libpad. Each frame every port
// in use is brought a step further through libpad's mode set-up, or read:
// its buttons as the engine's bits and as levels (DualShock sticks and
// pressures as analog levels), its motors driven. A recording, when one
// runs, takes the buttons from the file instead (pollPads).

#include "pads.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/collision/collision.h"
#include "sce/libgcc.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/input/pad_driver.cpp.h"

namespace zge
{

    namespace
    {
        // libpad (SDK).
        constexpr uint32_t kScePadInit = sym::scePadInit;
        constexpr uint32_t kScePadPortOpen = sym::scePadPortOpen;
        constexpr uint32_t kScePadRead = sym::scePadRead;
        constexpr uint32_t kScePadGetState = sym::scePadGetState;
        constexpr uint32_t kScePadGetReqState = sym::scePadGetReqState;
        constexpr uint32_t kScePadInfoAct = sym::scePadInfoAct;
        constexpr uint32_t kScePadInfoMode = sym::scePadInfoMode;
        constexpr uint32_t kScePadSetMainMode = sym::scePadSetMainMode;
        constexpr uint32_t kScePadSetActDirect = sym::scePadSetActDirect;
        constexpr uint32_t kScePadSetActAlign = sym::scePadSetActAlign;
        constexpr uint32_t kScePadInfoPressMode = sym::scePadInfoPressMode;
        constexpr uint32_t kScePadEnterPressMode = sym::scePadEnterPressMode;

        // libpad's values.
        constexpr int32_t kPadStateFindCTP1 = 2;
        constexpr int32_t kPadStateStable = 6;
        constexpr int32_t kInfoModeCurID = 1;
        constexpr int32_t kInfoModeCurExID = 2;
        constexpr int32_t kInfoModeIdTable = 4;
        constexpr int32_t kPadIdDigital = 4;
        constexpr int32_t kPadIdDualShock = 7;
        constexpr int32_t kReqStateFailed = 1;
        constexpr int32_t kReqStateComplete = 0;
        constexpr int32_t kMainModeAnalog = 1;
        constexpr int32_t kMainModeLock = 3;
        constexpr uint32_t kIdTypeShift = 4; // the ID byte's high nibble: 4 digital, 7 analog

        // The pad recorder (pads.cpp).
        constexpr uint32_t kReadPadRecording = sym::readPadRecording;
        constexpr uint32_t kWritePadRecording = sym::writePadRecording;
        constexpr uint32_t kPadRecordMode = sym::padRecordMode;

        // Stick bytes 4..7 and pressure bytes 8..19 of the read buffer: the
        // level each goes to (g_levelOfReadByte, bytes 0..3 unused).
        inline ee::Global<uint8_t[20], sym::g_levelOfReadByte> g_levelOfReadByte;
        inline ee::Global<ee::Float, sym::g_stickScale> g_stickScale;    // 1 / 127
        inline ee::Global<ee::Float, sym::g_pressureScale> g_pressureScale; // 1 / 255

        constexpr uint32_t kStickBytes = 4;      // read bytes 4..7
        constexpr uint32_t kPressureBytes = 8;   // read bytes 8..19
        constexpr uint32_t kReadBytesEnd = 20;
        constexpr int32_t kStickCentre = 0x7F;
        constexpr uint32_t kStickLevelsMask = 0x7u;            // levels 0..2 (the right stick's y is not marked)
        constexpr uint32_t kPressureLevelsMask = 0x0003CFF0u;  // levels 4..11, 14..17

        PadDriver &driver() { return *g_padDriver; }

        // ZGE2 never keeps the pad's ID (PadPort::id stays 0, so a changed
        // pad is never noticed) nor sees a DualShock gone digital.
        constexpr bool kKeepsPadId = !game::readable::kIsDm2;

        // A button's level: 1 when its bit is set, else 0.
        void setButtonLevel(ee::Ptr<Pad> pad, uint32_t level, bool held)
        {
            pad->levels[level] = ee::Float(held ? 1.0f : 0.0f);
        }

        // An unsigned word as a float the way the compiled code converts it
        // (halved and doubled when the top bit is set).
        ee::Float unsignedToFloat(uint32_t value)
        {
            if (static_cast<int32_t>(value) >= 0)
                return ee::toFloat(static_cast<int32_t>(value));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((value & 1u) | value >> 1));
            return half + half;
        }
    }

    // translatePadButtons: libpad's button bits (set when held) as the engine's
    // (PadButton).
    uint32_t translatePadButtons(uint32_t raw)
    {
        uint32_t buttons = (raw >> 12) & 0x1u;     // up
        if (raw & 0x4000u) buttons |= kPadDown;
        if (raw & 0x8000u) buttons |= kPadLeft;
        if (raw & 0x2000u) buttons |= kPadRight;
        if (raw & 0x0800u) buttons |= kPadStart;
        if (raw & 0x0100u) buttons |= kPadSelect;
        buttons |= raw & kPadTriangle;              // the same bit in both
        if (raw & 0x0040u) buttons |= kPadCross;
        if (raw & 0x0080u) buttons |= kPadSquare;
        if (raw & 0x0020u) buttons |= kPadCircle;
        if (raw & 0x0004u) buttons |= kPadL1;
        if (raw & 0x0001u) buttons |= kPadL2;
        if (raw & 0x0008u) buttons |= kPadR1;
        if (raw & 0x0002u) buttons |= kPadR2;
        if (raw & 0x0200u) buttons |= kPadL3;
        if (raw & 0x0400u) buttons |= kPadR3;
        return buttons;
    }
    READABLE_FUNCTION(translatePadButtons, Verified);

    namespace
    {
        struct LevelBit
        {
            uint32_t level;
            uint32_t bit;
        };
        constexpr LevelBit kLevelBits[16] = {
            {kLevelUp, kPadUp}, {kLevelDown, kPadDown}, {kLevelLeft, kPadLeft}, {kLevelRight, kPadRight},
            {kLevelTriangle, kPadTriangle}, {kLevelSquare, kPadSquare}, {kLevelCircle, kPadCircle}, {kLevelCross, kPadCross},
            {kLevelL1, kPadL1}, {kLevelL2, kPadL2}, {kLevelL3, kPadL3}, {kLevelR1, kPadR1},
            {kLevelR2, kPadR2}, {kLevelR3, kPadR3}, {kLevelStart, kPadStart}, {kLevelSelect, kPadSelect},
        };
    }

    // padButtonBits: a port's pad's buttons as bits again, from its levels
    // (a button with a level above 0 is held).
    uint32_t padButtonBits(ee::Ptr<PadPort> port)
    {
        const ee::Ptr<Pad> pad = port->pad;
        uint32_t buttons = 0;
        for (uint32_t i = 0; i < 16; ++i)
        {
            if (ee::Float(0.0f) < pad->levels[kLevelBits[i].level])
                buttons |= kLevelBits[i].bit;
        }
        return buttons;
    }
    READABLE_FUNCTION(padButtonBits, Verified);

    // setPadButtonLevels: a port's pad's button levels from button bits (a
    // recording's: 1 for each bit set, 0 for the rest; the sticks kept).
    void setPadButtonLevels(ee::Ptr<PadPort> port, uint32_t buttons)
    {
        const ee::Ptr<Pad> pad = port->pad;
        for (uint32_t i = 0; i < 16; ++i)
        {
            const bool held = (buttons & kLevelBits[i].bit) != 0;
            setButtonLevel(pad, kLevelBits[i].level, held);
        }
    }
    READABLE_FUNCTION(setPadButtonLevels, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        int32_t scePadGetState(uint32_t port, uint32_t slot) { return ee::game<int32_t>(kScePadGetState, port, slot); }
        int32_t scePadInfoMode(uint32_t port, uint32_t slot, uint32_t term, uint32_t offset) { return ee::game<int32_t>(kScePadInfoMode, port, slot, term, offset); }
        int32_t scePadGetReqState(uint32_t port, uint32_t slot) { return ee::game<int32_t>(kScePadGetReqState, port, slot); }
        int32_t scePadSetMainMode(uint32_t port, uint32_t slot, uint32_t mainModeAnalog, uint32_t mainModeLock) { return ee::game<int32_t>(kScePadSetMainMode, port, slot, mainModeAnalog, mainModeLock); }
        int32_t scePadInfoPressMode(uint32_t port, uint32_t slot) { return ee::game<int32_t>(kScePadInfoPressMode, port, slot); }
        int32_t scePadInfoAct(uint32_t port, uint32_t slot, uint32_t actuator, uint32_t term) { return ee::game<int32_t>(kScePadInfoAct, port, slot, actuator, term); }
        int32_t scePadSetActAlign(uint32_t port, uint32_t slot, ee::Ptr<uint8_t> motorAlign, uint32_t unusedSlots) { return ee::game<int32_t>(kScePadSetActAlign, port, slot, motorAlign, unusedSlots); }
        int32_t scePadEnterPressMode(uint32_t port, uint32_t slot) { return ee::game<int32_t>(kScePadEnterPressMode, port, slot); }
        int32_t scePadRead(uint32_t port, uint32_t slot, ee::Ptr<uint8_t> buffer) { return ee::game<int32_t>(kScePadRead, port, slot, buffer); }
    }

    // updatePad: one frame of a port. libpad's state first: no pad (or an
    // error) clears the record and the pad (no longer connected). Otherwise
    // the set-up goes a step further (PadSetup): the pad's mode found, a
    // digital pad switched to analog where it can be, a DualShock's motors
    // aligned and its pressure-sensitive buttons turned on; once ready, the
    // pad is read: its ID checked (a changed one starts the set-up again;
    // ZGE2 keeps no ID), its buttons as bits (rawButtons) and levels, the
    // sticks (beyond a dead zone of 32) and the pressures. A connected pad's
    // motors are driven last (updatePadVibration).
    void updatePad(ee::Ptr<PadPort> port)
    {
        const ee::Ptr<Pad> pad = port->pad;
        const int32_t state = scePadGetState(port->port, port->slot);

        const auto infoMode = [&](int32_t term, int32_t offset) {
            return scePadInfoMode(port->port, port->slot, term, offset);
        };
        const auto requestState = [&]() { return scePadGetReqState(port->port, port->slot); };
        const bool connected = state == kPadStateStable || state == kPadStateFindCTP1;

        // libpad's states 0 (no pad), 1 (finding it) and 7 (an error): the
        // original's jump table. ZGE2 takes state 0 alone for a pad gone (in
        // 1 and 7 its set-up goes on).
        const bool gone = game::readable::kIsDm2 ? state == 0
                                                 : static_cast<uint32_t>(state) < 8u && (state <= 1 || state == 7);
        if (gone)
        {
            port->setup = kSetupFindMode;
            for (uint32_t level = 0; level < kPadLevels; ++level)
                if (level != 12 && level != 13)
                    pad->levels[level] = ee::Float(0.0f);
            pad->flags = pad->flags & ~kPadConnected;
            return;
        }

        switch (port->setup)
        {
        case kSetupFindMode:
        {
            if (!connected)
                break;
            uint32_t modeId = static_cast<uint32_t>(infoMode(kInfoModeCurID, 0));
            if (modeId == 0)
                break;
            const int32_t extendedId = infoMode(kInfoModeCurExID, 0);
            if (extendedId >= 1)
                modeId = static_cast<uint32_t>(extendedId);
            pad->flags = pad->flags | kPadConnected;
            port->analog = 0;
            port->pressure = 0;
            if (modeId == kPadIdDigital)
            {
                port->setup = kSetupDigitalModes;
                port->mode = kPadModeDigital;
            }
            else if (modeId == kPadIdDualShock)
            {
                port->analog = 1;
                port->setup = kSetupPressureMode;
                port->mode = kPadModeAnalog;
            }
            else
            {
                port->mode = kPadModeOther;
                port->setup = kSetupReady;
            }
            break;
        }

        case kSetupDigitalModes: // a digital pad: has it other modes?
            if (infoMode(kInfoModeIdTable, -1) == 0)
            {
                port->setup = kSetupReady;
                break;
            }
            port->setup = static_cast<uint8_t>(port->setup + 1);
            [[fallthrough]];

        case kSetupAnalogMode: // switch it to analog
            if (scePadSetMainMode(port->port, port->slot, kMainModeAnalog, kMainModeLock) == 1)
                port->setup = static_cast<uint8_t>(port->setup + 1);
            break;

        case kSetupAnalogModeWait: // the mode switch: failed, retry; done, find the mode again
            if (requestState() == kReqStateFailed)
            {
                port->setup = static_cast<uint8_t>(port->setup - 1);
                // (ZGE2 asks again after a failure, as its other waits do)
                if constexpr (!game::readable::kIsDm2)
                    break;
            }
            if (requestState() == kReqStateComplete)
                port->setup = kSetupFindMode;
            break;

        case kSetupPressureMode: // a DualShock: pressure-sensitive buttons?
            if (scePadInfoPressMode(port->port, port->slot) == 1)
                port->setup = static_cast<uint8_t>(port->setup + 1);
            else
                port->setup = kSetupReady;
            break;

        case kSetupMotors: // the motors' alignment: [0] the small motor, [1] the big one, the rest none
            if (scePadInfoAct(port->port, port->slot, -1, 0) == 0)
                port->setup = kSetupReady; // no motors: the step below makes it 100, still ready
            port->motorAlign[0] = 0;
            port->motorAlign[1] = 1;
            for (uint32_t i = 2; i < 6; ++i)
                port->motorAlign[i] = 0xFF;
            if (scePadSetActAlign(port->port, port->slot, port->motorAlign, 0xFF) != 0)
                port->setup = static_cast<uint8_t>(port->setup + 1);
            break;

        case kSetupMotorsWait: // the alignment: failed, retry; done, pressure on
            if (requestState() == kReqStateFailed)
                port->setup = static_cast<uint8_t>(port->setup - 1);
            if (requestState() == kReqStateComplete)
                port->setup = kSetupEnterPressure;
            break;

        case kSetupEnterPressure: // pressure-sensitive buttons on
            if (scePadEnterPressMode(port->port, port->slot) == 1)
                port->setup = static_cast<uint8_t>(port->setup + 1);
            break;

        case kSetupEnterPressureWait: // pressure: failed, retry; done, ready
            if (requestState() == kReqStateFailed)
                port->setup = static_cast<uint8_t>(port->setup - 1);
            if (requestState() == kReqStateComplete)
            {
                port->pressure = 1;
                port->setup = kSetupReady;
                port->mode = kPadModeAnalogPressure;
            }
            break;

        default: // ready: read the pad
        {
            if (!connected)
                break;
            const uint8_t *data = driver().readBuffer;
            const int32_t read = scePadRead(port->port, port->slot, driver().readBuffer);
            const uint8_t lastId = port->id;
            const bool dataGood = read > 0 && data[0] == 0;
            if (lastId != 0 && lastId != data[1])
            {
                // Another pad (or mode): set up again.
                port->id = 0;
                port->setup = kSetupFindMode;
                break;
            }
            const uint8_t id = data[1];
            if constexpr (kKeepsPadId)
                port->id = id;
            if (!dataGood)
            {
                driver().rawButtons = 0;
                memoryFill(ee::Ptr<ee::Float>(pad->levels), 0, static_cast<uint32_t>(sizeof(pad->levels)));
                break;
            }
            if (kKeepsPadId && port->analog != 0 && (id & 0xF0u) >> kIdTypeShift == kPadIdDigital)
            {
                // A DualShock gone digital (its analog button): set up again.
                port->id = 0;
                port->setup = kSetupFindMode;
                break;
            }
            const uint32_t raw = ((static_cast<uint32_t>(data[2]) << 8) | data[3]) ^ 0xFFFFu;
            driver().rawButtons = raw;
            const uint32_t buttons = translatePadButtons(raw);
            setButtonLevel(pad, kLevelStart, (buttons & kPadStart) != 0);
            setButtonLevel(pad, kLevelSelect, (buttons & kPadSelect) != 0);
            setButtonLevel(pad, kLevelR3, (buttons & kPadR3) != 0);
            setButtonLevel(pad, kLevelL3, (buttons & kPadL3) != 0);

            if (port->analog != 0)
            {
                // The sticks: beyond the dead zone, (byte - 127) / 127 (at
                // most 1), else 0.
                const ee::Float scale = *g_stickScale;
                for (uint32_t byte = kStickBytes; byte < kPressureBytes; ++byte)
                {
                    const ee::Float value = ee::toFloat(static_cast<int32_t>(data[byte]) - kStickCentre);
                    const uint32_t level = (*g_levelOfReadByte)[byte];
                    if (ee::Float(32.0f) < ee::abs(value))
                        pad->levels[level] = ee::min(value * scale, ee::Float(1.0f));
                    else
                        pad->levels[level] = ee::Float(0.0f);
                }
                pad->analogLevels = pad->analogLevels | kStickLevelsMask;
            }
            else
            {
                // A digital pad: the left stick from the direction buttons.
                if (buttons & kPadLeft)
                    pad->levels[kLevelLeftX] = ee::Float(-1.0f);
                else
                    setButtonLevel(pad, kLevelLeftX, (buttons & kPadRight) != 0);
                if (buttons & kPadUp)
                    pad->levels[kLevelLeftY] = ee::Float(-1.0f);
                else
                    setButtonLevel(pad, kLevelLeftY, (buttons & kPadDown) != 0);
                pad->levels[kLevelRightX] = ee::Float(0.0f);
                pad->analogLevels = pad->analogLevels & ~kStickLevelsMask;
            }

            if (port->pressure != 0)
            {
                // The pressures: byte / 255.
                const ee::Float scale = *g_pressureScale;
                for (uint32_t byte = kPressureBytes; byte < kReadBytesEnd; ++byte)
                    pad->levels[(*g_levelOfReadByte)[byte]] = ee::toFloat(static_cast<int32_t>(data[byte])) * scale;
                pad->analogLevels = pad->analogLevels | kPressureLevelsMask;
                break;
            }
            constexpr uint32_t kPressureOrder[12] = {kLevelLeft, kLevelRight, kLevelUp, kLevelDown, kLevelTriangle, kLevelSquare,
                                                     kLevelCircle, kLevelCross, kLevelL1, kLevelR1, kLevelL2, kLevelR2};
            constexpr uint32_t kPressureOrderBits[12] = {kPadLeft, kPadRight, kPadUp, kPadDown, kPadTriangle, kPadSquare,
                                                         kPadCircle, kPadCross, kPadL1, kPadR1, kPadL2, kPadR2};
            for (uint32_t i = 0; i < 12; ++i)
                setButtonLevel(pad, kPressureOrder[i], (buttons & kPressureOrderBits[i]) != 0);
            pad->analogLevels = pad->analogLevels & ~kPressureLevelsMask;
            break;
        }
        }

        if ((pad->flags & kPadConnected) == 0)
            return;
        if constexpr (game::readable::kIsDm2) // (ZGE2 ends in the motors, as a tail call)
            game::tail(updatePadVibration, port);
        else
            updatePadVibration(port);
    }
    READABLE_FUNCTION(updatePad, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void scePadInit(uint32_t value) { ee::game(kScePadInit, value); }
        void scePadPortOpen(uint32_t port, uint32_t slot, ee::Ptr<uint8_t> dmaBuffer) { ee::game(kScePadPortOpen, port, slot, dmaBuffer); }
    }

    // openPadPorts: libpad started and ports 0 and 1 opened. 1.
    uint32_t openPadPorts()
    {
        scePadInit(0);
        scePadPortOpen(0, 0, driver().dmaBuffers[0]);
        scePadPortOpen(1, 0, driver().dmaBuffers[1]);
        return 1;
    }
    READABLE_FUNCTION(openPadPorts, Verified);

    // clearPadRecords: every port in use cleared (through its pad). 1.
    uint32_t clearPadRecords()
    {
        int32_t left = static_cast<int32_t>(kPadPorts);
        uint32_t entry = kPadDriverAddress;
        for (;;)
        {
            const ee::Ptr<PadPort> port(entry);
            if (port->flags & kPadPortInUse)
                clearPadState(port->pad);
            --left;
            entry += static_cast<uint32_t>(sizeof(PadPort));
            if (left > 0)
                continue;
            return 1;
        }
    }
    READABLE_FUNCTION(clearPadRecords, Verified);

    // findPadRecord: a free port record for a pad (the first; libpad port 0
    // when it is the only one in use, else 1), or 0. Bug kept: with all four
    // in use the search ends just past them (on the raw buttons and the read
    // buffer) and hands that out: the game's none-free test can never hold.
    // The requested port is not read.
    ee::Ptr<PadPort> findPadRecord(uint32_t requestedPort, ee::Ptr<Pad> pad)
    {
        (void)requestedPort;
        ee::Ptr<PadPort> port(driver().ports);
        for (uint32_t i = 0; i < kPadPorts && (port->flags & kPadPortInUse) != 0; ++i)
            ++port;

        uint32_t inUse = 0;
        for (const PadPort &other : driver().ports)
            inUse += other.flags & kPadPortInUse;

        port->flags = port->flags | kPadPortInUse;
        port->port = inUse != 0 ? 1u : 0u;
        port->pad = pad;
        port->slot = 0;
        port->id = 0;
        port->pressure = 0;
        pad->flags = pad->flags & ~kPadConnected;
        return port;
    }
    READABLE_FUNCTION(findPadRecord, Verified);

    // clearPadState: a pad's port record cleared (free again). 1.
    uint32_t clearPadState(ee::Ptr<Pad> pad)
    {
        memoryFill(pad->port, 0, static_cast<uint32_t>(sizeof(PadPort)));
        return 1;
    }
    READABLE_FUNCTION(clearPadState, Verified);

    // releasePadRecord: a pad let go of for now: its port's set-up starts again
    // and it reads as not connected (the record stays in use). 1.
    uint32_t releasePadRecord(ee::Ptr<Pad> pad)
    {
        ee::Ptr<PadPort>(pad->port)->setup = kSetupFindMode;
        pad->flags = pad->flags & ~kPadConnected;
        return 1;
    }
    READABLE_FUNCTION(releasePadRecord, Verified);

    namespace
    {
        // ZGE4 (BMX XXX) records the sticks too: two bytes after each frame's
        // buttons, the stick levels (-1 .. 1) a nibble each in sevenths
        // (0 .. 14), set back into the levels as it records and as it plays.
        constexpr uint32_t kRecordedStickBytes = 2;
        constexpr uint32_t kStickSteps = 7;

        void setStickLevels(ee::Ptr<Pad> pad, const ee::Local<uint8_t[kRecordedStickBytes]> &sticks)
        {
            const ee::Float one(1.0f);
            const ee::Float step(1.0f / kStickSteps);
            const auto level = [&](uint8_t nibble) { return ee::toFloat(static_cast<int32_t>(nibble)) * step - one; };
            pad->levels[kLevelLeftX] = level(sticks[0] & 0xFu);
            pad->levels[kLevelLeftY] = level(sticks[0] >> 4);
            pad->levels[kLevelRightX] = level(sticks[1] & 0xFu);
            pad->levels[kLevelRightY] = level(sticks[1] >> 4);
        }

        // Bug kept: both bytes get the right stick's nibbles (the left
        // stick's are worked out and lost), so a recording plays the right
        // stick on both.
        void recordStickLevels(ee::Ptr<Pad> pad)
        {
            ee::Local<uint8_t[kRecordedStickBytes]> sticks;
            const ee::Float one(1.0f);
            const ee::Float steps(static_cast<float>(kStickSteps));
            const auto nibble = [&](uint32_t level) { return static_cast<uint32_t>(ee::toInt((pad->levels[level] + one) * steps)); };
            [[maybe_unused]] const uint32_t leftX = nibble(kLevelLeftX);
            [[maybe_unused]] const uint32_t leftY = nibble(kLevelLeftY);
            const uint32_t rightX = nibble(kLevelRightX);
            const uint32_t rightY = nibble(kLevelRightY);
            const uint8_t right = static_cast<uint8_t>(rightX | rightY << 4);
            sticks[1] = right;
            sticks[0] = right;
            writePadRecording(sticks, kRecordedStickBytes);
            setStickLevels(pad, sticks);
        }

        void playStickLevels(ee::Ptr<Pad> pad)
        {
            ee::Local<uint8_t[kRecordedStickBytes]> sticks;
            readPadRecording(sticks, kRecordedStickBytes);
            setStickLevels(pad, sticks);
        }
    }

    // pollPads: every port in use for the frame: updated and its buttons
    // taken (from the levels), then, when a recording runs, written to it or
    // replaced by the recorded ones (levels set from them); the pad's held,
    // changed and released bits. 1.
    uint32_t pollPads()
    {
        ee::Local<uint32_t> buttons;
        int32_t left = static_cast<int32_t>(kPadPorts);
        uint32_t entry = kPadDriverAddress;
        for (;;)
        {
            const ee::Ptr<PadPort> port(entry);
            if (port->flags & kPadPortInUse)
            {
                const ee::Ptr<Pad> pad = port->pad;
                updatePad(port);
                *buttons = padButtonBits(port);
                const uint32_t mode = padRecordMode();
                if (mode == kPadsRecording)
                {
                    writePadRecording(buttons, 4);
                    setPadButtonLevels(port, *buttons);
                    if constexpr (game::readable::kIsBmx)
                        recordStickLevels(pad);
                }
                else if (mode == kPadsPlayback)
                {
                    readPadRecording(buttons, 4);
                    setPadButtonLevels(port, *buttons);
                    if constexpr (game::readable::kIsBmx)
                        playStickLevels(pad);
                }
                const uint32_t now = *buttons;
                const uint32_t before = pad->buttons;
                const uint32_t changed = now ^ before;
                pad->changed = changed;
                pad->released = before & changed;
                pad->buttons = now;
            }
            --left;
            entry += static_cast<uint32_t>(sizeof(PadPort));
            if (left > 0)
                continue;
            return 1;
        }
    }
    READABLE_FUNCTION_WAITS(pollPads, Verified);

    // padMode (no caller but the dead padModeOf): a pad's port's
    // PadMode.
    uint32_t padMode(ee::Ptr<Pad> pad)
    {
        return ee::Ptr<PadPort>(pad->port)->mode;
    }
    READABLE_FUNCTION(padMode, Verified);

    // padsHaveVibration: the pads can vibrate (on the PS2, always). 1.
    uint32_t padsHaveVibration()
    {
        return 1;
    }
    READABLE_FUNCTION(padsHaveVibration, Verified);

    // setPadVibration: a pad's motors started (the small one on at a level
    // 0..255, the big one at 0..65535), both falling to nothing over `time`
    // milliseconds; both 0 stops them. Returns a leftover of no meaning (0 when
    // stopped; else from the last unsigned conversion: the time's low bit,
    // or the halved value of a negative time or big level).
    int32_t setPadVibration(ee::Ptr<Pad> pad, uint32_t smallMotor, uint32_t bigMotor, uint32_t time, uint32_t tag)
    {
        const ee::Ptr<PadPort> port(pad->port);
        const uint32_t vibrating = smallMotor != 0 || bigMotor != 0 ? 1u : 0u;
        port->vibrating = vibrating;
        if (!vibrating)
            return 0;
        port->vibrationTag = tag;
        port->vibrationStart = collisionClock();
        port->vibrationTime = time;

        // The conversions' blocks (each a branch on the sign).
        const auto converted = [](uint32_t value) {
            const ee::Float f = unsignedToFloat(value);
            return f;
        };
        const ee::Float small = converted(smallMotor);
        const ee::Float smallTime = converted(time);
        port->smallMotorFall = small / smallTime;
        const ee::Float big = converted(bigMotor);
        const ee::Float bigTime = converted(time);
        port->bigMotorFall = big / bigTime;
        if constexpr (game::readable::kIsDm2)
        {
            // ZGE2 keeps each level as it comes back from its float, and
            // answers the big one's.
            pad->smallMotor = sce::libgcc::fptoui(small);
            pad->bigMotor = sce::libgcc::fptoui(big);
            return static_cast<int32_t>(pad->bigMotor);
        }
        pad->bigMotor = bigMotor;
        pad->smallMotor = smallMotor;

        if (static_cast<int32_t>(time) < 0)
            return static_cast<int32_t>((time & 1u) | time >> 1);
        if (static_cast<int32_t>(bigMotor) < 0)
            return static_cast<int32_t>((bigMotor & 1u) | bigMotor >> 1);
        return static_cast<int32_t>(time & 1u);
    }

    // startPadVibration: ZGE4's setPadVibration: the motors always started (a
    // level of 0 does not stop them; that motor only gets no fall), each level
    // falling to nothing over `time` milliseconds. Returns a leftover of no
    // meaning, as ZGE3's: with both levels 0 the clock; else from the last
    // unsigned conversion (the time's low bit, or its halved value when
    // negative; with the big level set, the big level's unless the time is
    // negative).
    int32_t startPadVibration(ee::Ptr<Pad> pad, uint32_t smallMotor, uint32_t bigMotor, uint32_t time, uint32_t tag)
    {
        const auto leftoverOf = [](uint32_t value) {
            return static_cast<int32_t>(value) < 0 ? (value & 1u) | value >> 1 : value & 1u;
        };
        const ee::Ptr<PadPort> port(pad->port);
        port->vibrating = 1;
        port->vibrationTag = tag;
        const uint32_t now = collisionClock();
        port->vibrationStart = now;
        port->vibrationTime = time;
        const ee::Float noFall(0.0f);
        port->smallMotorFall = smallMotor != 0 ? unsignedToFloat(smallMotor) / unsignedToFloat(time) : noFall;
        port->bigMotorFall = bigMotor != 0 ? unsignedToFloat(bigMotor) / unsignedToFloat(time) : noFall;
        uint32_t leftover = now;
        if (bigMotor != 0)
            leftover = static_cast<int32_t>(time) < 0 ? leftoverOf(time) : leftoverOf(bigMotor);
        else if (smallMotor != 0)
            leftover = leftoverOf(time);
        pad->bigMotor = bigMotor;
        pad->smallMotor = smallMotor;
        return static_cast<int32_t>(leftover);
    }
    READABLE_FUNCTION_OWN(setPadVibration, Verified, bmx, startPadVibration);

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void scePadSetActDirect(uint32_t port, uint32_t slot, ee::Ptr<uint8_t> motors) { ee::tailCall(kScePadSetActDirect, port, slot, motors); }
        }
    }

    // updatePadVibration: a vibrating port's motors for the frame: each level as
    // set less its fall times the milliseconds since (not below 0); the small
    // motor on while its level is above 0, the big one at its level's high
    // byte. Both down to 0 ends it (the pad's levels cleared). An analog
    // pad gets the motors' bytes (scePadSetActDirect, the original's tail
    // call).
    void updatePadVibration(ee::Ptr<PadPort> port)
    {
        if (port->vibrating == 0)
            return;
        const int32_t elapsed = static_cast<int32_t>(collisionClock() - port->vibrationStart);
        const ee::Ptr<Pad> pad = port->pad;

        const ee::Float elapsedTime = unsignedToFloat(static_cast<uint32_t>(elapsed));
        const int32_t smallLevel = ee::toInt(unsignedToFloat(pad->smallMotor) - elapsedTime * port->smallMotorFall);
        const int32_t bigLevel = ee::toInt(unsignedToFloat(pad->bigMotor) - elapsedTime * port->bigMotorFall);
        const int32_t smallNow = smallLevel < 0 ? 0 : smallLevel;
        const int32_t bigNow = bigLevel < 0 ? 0 : bigLevel;

        port->motors[0] = smallNow != 0 ? 1 : 0;
        port->motors[1] = static_cast<uint8_t>(bigNow >> 8);
        if ((smallNow | bigNow) == 0)
        {
            port->vibrating = 0;
            pad->smallMotor = 0;
            pad->bigMotor = 0;
        }
        if (port->analog != 0)
            tail::scePadSetActDirect(port->port, port->slot, port->motors);
    }
    READABLE_FUNCTION(updatePadVibration, Verified);
}
