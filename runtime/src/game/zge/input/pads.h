#pragma once

// The engine's pads (zinput.c, initPads-padRecordingFrames, zge/input/pads.cpp) and
// the PS2 pad driver under them (in the ps2Movie.c block, translatePadButtons-updatePadVibration,
// zge/input/pad_driver.cpp). The game asks for a Pad (newPad); the driver
// gives it one of four port records (PadPort, two used: the PS2's ports),
// brings each pad up through libpad's mode set-up (DualShock analog mode,
// then pressure-sensitive buttons) and reads it once a frame (pollPads),
// as button bits and as one level a button (0 or 1, or the pressure and the
// sticks' -1..1). A pad's buttons can be recorded to a file and played back
// in place of the pads (the pad recorder, mode PadRecordMode).

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/input/pads.h.h"

namespace zge
{
    struct ZFile; // zge/assets/file.h

    // The engine's button bits (translatePadButtons from libpad's).
    enum PadButton : uint32_t
    {
        kPadUp = 0x0001u,
        kPadDown = 0x0002u,
        kPadLeft = 0x0004u,
        kPadRight = 0x0008u,
        kPadTriangle = 0x0010u,
        kPadSquare = 0x0020u,
        kPadCircle = 0x0040u,
        kPadCross = 0x0080u,
        kPadL1 = 0x0100u,
        kPadR1 = 0x0200u,
        kPadL2 = 0x0400u,
        kPadR2 = 0x0800u,
        kPadL3 = 0x1000u,
        kPadR3 = 0x2000u,
        kPadStart = 0x4000u,
        kPadSelect = 0x8000u,
    };

    // A pad's levels (Pad::levels): the sticks, then one a button.
    enum PadLevel : uint32_t
    {
        kLevelLeftX = 0, // -1 left .. 1 right
        kLevelLeftY = 1, // -1 up .. 1 down
        kLevelRightX = 2,
        kLevelRightY = 3,
        kLevelLeft = 4,
        kLevelRight = 5,
        kLevelUp = 6,
        kLevelDown = 7,
        kLevelTriangle = 8,
        kLevelSquare = 9,
        kLevelCircle = 10,
        kLevelCross = 11,
        // 12, 13: never set
        kLevelL1 = 14,
        kLevelR1 = 15,
        kLevelL2 = 16,
        kLevelR2 = 17,
        kLevelL3 = 18,
        kLevelR3 = 19,
        kLevelStart = 20,
        kLevelSelect = 21,
        kPadLevels = 22,
    };

    constexpr uint32_t kPadConnected = 1u << 0; // Pad::flags

    struct PadPort;

    // A pad as the game has it (newPad; 124-byte records of the pads' pool).
    struct Pad
    {
        uint32_t requestedPort; // +0x00 newPad's argument (the driver gives the ports in turn)
        uint32_t flags;         // +0x04 kPadConnected
        uint32_t buttons;       // +0x08 PadButton bits held
        uint32_t changed;       // +0x0C bits pressed or let go this frame
        uint32_t released;      // +0x10 bits let go this frame
        ee::Float levels[kPadLevels]; // +0x14 PadLevel
        // +0x6C bit n: level n is analog (the sticks 0..2, the pressure
        // levels 4..11 and 14..17); clear: 0 or 1 from the button.
        uint32_t analogLevels;
        uint32_t smallMotor;    // +0x70 setPadVibration's levels (cleared when it ends): 0..255, the small motor on while above 0
        uint32_t bigMotor;      // +0x74 0..65535, the big motor at its high byte
        ee::Ptr<PadPort> port;  // +0x78 its PadPort
    };
    static_assert(offsetof(Pad, levels) == 0x14 && offsetof(Pad, analogLevels) == 0x6C && offsetof(Pad, port) == 0x78 &&
                  sizeof(Pad) == 0x7C);

    // Where the set-up of a port's pad is (PadPort::setup).
    enum PadSetup : uint8_t
    {
        kSetupFindMode = 0,          // read the pad's mode (ID 4 digital, 7 DualShock analog)
        kSetupDigitalModes = 40,     // a digital pad: has it other modes?
        kSetupAnalogMode = 41,       // switch it to analog, locked
        kSetupAnalogModeWait = 42,   // then find its mode again
        kSetupPressureMode = 70,     // a DualShock: can it sense pressure?
        kSetupMotors = 71,           // its motors' alignment
        kSetupMotorsWait = 72,
        kSetupEnterPressure = 76,    // pressure-sensitive buttons on
        kSetupEnterPressureWait = 77,
        kSetupReady = 99,            // read it each frame (any value 78 and up, or not listed)
    };

    // How a port's pad reads (PadPort::mode).
    enum PadMode : uint32_t
    {
        kPadModeOther = 0,
        kPadModeDigital = 1,
        kPadModeAnalog = 2,
        kPadModeAnalogPressure = 3,
    };

    constexpr uint32_t kPadPortInUse = 1u << 0; // PadPort::flags

    // The driver's record of a port (four at padDriverAddress; findPadRecord hands
    // them out).
    struct PadPort
    {
        uint32_t flags;          // +0x00 kPadPortInUse
        ee::Ptr<Pad> pad;        // +0x04 its Pad
        uint32_t port;           // +0x08 libpad's port (0, then 1 for every later pad)
        uint32_t slot;           // +0x0C 0
        uint8_t id;              // +0x10 the pad's ID byte as last read (0: none yet; ZGE2 never stores it)
        uint8_t setup;           // +0x11 PadSetup
        uint8_t pressure;        // +0x12 pressure-sensitive buttons on
        uint8_t analog;          // +0x13 a DualShock in analog mode (sticks read, motors driven)
        uint32_t mode;           // +0x14 PadMode
        uint32_t vibrating;      // +0x18
        uint32_t vibrationStart; // +0x1C the game clock (milliseconds) when it began
        uint32_t vibrationTime;  // +0x20 milliseconds until the motors stop
        ee::Float smallMotorFall; // +0x24 the small motor's level lost a millisecond (level / time)
        ee::Float bigMotorFall;  // +0x28
        uint32_t vibrationTag;   // +0x2C setPadVibration's last argument [unknown: nothing reads it]
        uint8_t _30[8];
        uint8_t motors[6];       // +0x38 scePadSetActDirect's: [0] the small motor on, [1] the big one's level
        uint8_t motorAlign[6];   // +0x3E scePadSetActAlign's
    };
    static_assert(offsetof(PadPort, mode) == 0x14 && offsetof(PadPort, vibrationTag) == 0x2C &&
                  offsetof(PadPort, motors) == 0x38 && offsetof(PadPort, motorAlign) == 0x3E && sizeof(PadPort) == 0x44);

    constexpr uint32_t kPadPorts = 4;

    // The driver's state (padDriverAddress).
    struct PadDriver
    {
        PadPort ports[kPadPorts];  // +0x000
        uint32_t rawButtons;       // +0x110 libpad's button bits, set when held, as last read (0: no data)
        uint8_t readBuffer[32];    // +0x114 scePadRead's: [0] 0 when the data is good, [1] the ID, [2..3] the
                                   //        buttons (clear when held), [4..7] the sticks, [8..19] the pressures
        uint8_t _134[0xC];
        uint8_t dmaBuffers[2][0x100]; // +0x140 scePadPortOpen's, ports 0 and 1
    };
    static_assert(offsetof(PadDriver, rawButtons) == 0x110 && offsetof(PadDriver, readBuffer) == 0x114 &&
                  offsetof(PadDriver, dmaBuffers) == 0x140 && sizeof(PadDriver) == 0x340);

    constexpr uint32_t kPadDriverAddress = sym::padDriverAddress;
    inline ee::Global<PadDriver, kPadDriverAddress> g_padDriver;

    // The pad recorder's modes (PadRecorder::mode).
    enum PadRecordMode : uint32_t
    {
        kPadsLive = 0,
        kPadsRecording = 1, // each frame's buttons written to the file
        kPadsPlayback = 2,  // each frame's buttons read from it instead
    };

    constexpr uint32_t kPadRecordingBufferBytes = 0x10000u;

    // The engine's pads and the pad recorder (g_padsReady).
    struct PadRecorder
    {
        uint32_t ready;          // +0x00
        uint32_t padPool;        // +0x04 the Pads (two a block; freed: freePad)
        uint32_t closeRequested; // +0x08 readPads closes the recording first [unknown: nothing sets it]
        uint32_t buffer;         // +0x0C 64 KB heap block
        ee::Ptr<ZFile> file;     // +0x10 the file it reads or writes (none: not recording)
        uint32_t mode;           // +0x14 PadRecordMode
        uint32_t filled;         // +0x18 playback: the bytes the last read put in the buffer
        uint32_t position;       // +0x1C the buffer's bytes used (written, or played back)
        uint32_t frames;         // +0x20 records written or played back since it began
    };
    static_assert(offsetof(PadRecorder, mode) == 0x14 && sizeof(PadRecorder) == 0x24);

    constexpr uint32_t kPadRecorderAddress = sym::g_padsReady;
    inline ee::Global<PadRecorder, kPadRecorderAddress> g_padRecorder;

    // pad_driver.cpp
    uint32_t translatePadButtons(uint32_t raw);
    uint32_t padButtonBits(ee::Ptr<PadPort> port);
    void setPadButtonLevels(ee::Ptr<PadPort> port, uint32_t buttons);
    void updatePad(ee::Ptr<PadPort> port);
    uint32_t openPadPorts();
    uint32_t clearPadRecords();
    ee::Ptr<PadPort> findPadRecord(uint32_t requestedPort, ee::Ptr<Pad> pad);
    uint32_t clearPadState(ee::Ptr<Pad> pad);
    uint32_t releasePadRecord(ee::Ptr<Pad> pad);
    uint32_t pollPads();
    uint32_t padMode(ee::Ptr<Pad> pad);
    uint32_t padsHaveVibration();
    int32_t setPadVibration(ee::Ptr<Pad> pad, uint32_t smallMotor, uint32_t bigMotor, uint32_t time, uint32_t tag);
    void updatePadVibration(ee::Ptr<PadPort> port);

    // pads.cpp
    uint32_t initPads();
    uint32_t shutDownPads();
    ee::Ptr<Pad> newPad(uint32_t requestedPort);
    uint32_t freePad(ee::Ptr<Pad> pad);
    uint32_t releasePad(ee::Ptr<Pad> pad);
    uint32_t readPads();
    ee::Ptr<Pad> padAt(int32_t index);
    uint32_t padModeOf(ee::Ptr<Pad> pad);
    uint32_t padCanVibrate(ee::Ptr<Pad> pad);
    int32_t padVibrate(ee::Ptr<Pad> pad, uint32_t smallMotor, uint32_t bigMotor, uint32_t time, uint32_t tag);
    uint32_t openPadRecording(uint32_t path, uint32_t mode);
    void readPadRecording(ee::Address dst, uint32_t bytes);
    void writePadRecording(ee::Address src, uint32_t bytes);
    uint32_t closePadRecording();
    uint32_t padRecordMode();
    uint32_t padRecordingFrames();
}
