#pragma once

#include <cstdint>
#include <string>
#include <vector>

// PS2X_CONTROL=<dir>: drive a running game from outside (pad input on every
// port, like PS2X_PAD_SCRIPT), like PINE for
// PCSX2. Commands are appended as lines to <dir>/cmd and run one after the
// other on the EE thread at VBlank; results are appended to <dir>/out.
// tools/nctl.py (main repo) is the client.
//
//   press <BTN>[+<BTN>...] [hold=6] [gap=6]   hold buttons for `hold` vsyncs, then
//                                             release for `gap` vsyncs
//   hold <BTN>[+...] <vsyncs>                 hold for n vsyncs (no gap)
//   down <BTN>[+...] / up <BTN>[+...]         press / release until changed
//   stick <LX|LY|RX|RY> <-1..1> [vsyncs]      hold an axis (0 vsyncs: until changed;
//                                             value 0 recentres)
//   wait <vsyncs>
//   until <addr><op><value>[&...] [timeout]   wait for guest u32 conditions
//                                             (StateProbe syntax); reports ok/timeout
//   peek <addr> [words]                       hex u32 words
//   peekf <addr> [words]                      floats
//   poke <addr> <value> / or <addr> <value>   u32 write / OR
//   shot <file.png>                           presented picture (next host frame)
//   dump <file>                               32 MB EE RAM image
//   renderer <cpu|gpu> [scale=1]              switch the GS renderer (next host frame)
//   settings                                  open the settings, as Cmd+, does
//   set <key> <value> / get <key>             a setting of ps2x::settings ("ok <value>");
//                                             set on an action row runs it
//   prompts <device>                          button prompts for keyboard, playstation,
//                                             xbox or nintendo; auto: follow the input
//   gsdump <file.gsr> [frames=2]              GS input trace from the next host frame
//                                             (ps2xGsReplay; as PS2X_GS_DUMP)
//   ftrace start / ftrace stop <file>         frame timeline trace (runtime/ps2_frame_trace.h)
//   echo <text>                               marker: written when reached
//
// Buttons are PS2X_PAD_SCRIPT names. Every command writes "<id> ..." to out
// when it completes (id = line number from 1), so a client can wait for it.
namespace ps2_debug_control
{
    bool enabled();
    // Overrides PS2X_CONTROL and resets the command state (tests; "" disables).
    void setDirectory(const std::string &dir);
    // EE thread, every VBlank (before the guest sees it).
    void onVsync(uint8_t *rdram, uint64_t tick);
    // Pad read: buttons to OR in (PadScript masks), stick overrides.
    uint16_t buttons();
    // Writes active axes (RX, RY, LX, LY) into sticks; returns a mask of them.
    unsigned sticks(uint8_t *sticks);
    // Host frame thread: the next pending screenshot path, or "".
    std::string takePendingShot();
    void shotDone(const std::string &path, bool ok);
    // Host frame thread: a pending "renderer" request ("gpu 3", "cpu 1"), or "".
    std::string takePendingRenderer();
    // Host frame thread: a pending "gsdump" request ("" if none).
    std::string takePendingGsDump(uint32_t &frames);

    // Parsing helper, exposed for tests: splits "A+B" into a PadScript mask.
    uint16_t parseButtons(const std::string &spec);
}
