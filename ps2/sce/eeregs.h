#pragma once

// The EE's own hardware registers, by the names of Sony's eeregs.h. These are
// the machine's, the same in every game; ee/io.h reads and writes them.

#include <cstdint>

namespace sce
{
    constexpr uint32_t D_CTRL = 0x1000E000u; // the DMA controller's control
    constexpr uint32_t D_STAT = 0x1000E010u; // its status: writing a channel's bit clears its interrupt
    constexpr uint32_t SPR = 0x70000000u;    // the scratchpad
    constexpr uint32_t D3_CHCR = 0x1000B000u; // IPU from memory's DMA channel: its control
    constexpr uint32_t D4_CHCR = 0x1000B400u; // IPU to memory's
    constexpr uint32_t D_ENABLER = 0x1000F520u; // reads the channels' master enable
    constexpr uint32_t D_ENABLEW = 0x1000F590u; // writes it
    constexpr uint32_t VIF1_ERR = 0x10003C20u;  // which VIF1 errors stop it
    constexpr uint32_t VIF1_FBRST = 0x10003C10u; // resets it
    constexpr uint32_t VIF1_MARK = 0x10003C30u;  // the last MARK code's value
    constexpr uint32_t VIF1_CODE = 0x10003C80u;  // the code VIF1 last read (an interrupting one's immediate)
    constexpr uint32_t VIF0_ERR = 0x10003820u;   // which VIF0 errors stop it
    constexpr uint32_t GIF_MODE = 0x10003010u;   // the GIF's mode (bit 2, IMT: intermittent PATH3)
    constexpr uint32_t GIF_STAT = 0x10003020u;   // the GIF's status
    constexpr uint32_t D2_CHCR = 0x1000A000u;    // the GIF's DMA channel: its control
    constexpr uint32_t D2_QWC = 0x1000A020u;     // its quadword count
    constexpr uint32_t D2_TADR = 0x1000A030u;    // its tag address
}
