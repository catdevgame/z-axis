#pragma once

// The engine's DMA channels' state (ps2/dma.cpp): libdma's channel handles kept
// by number, the channels whose completion interrupt is enabled, and each
// channel's send callback.

#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"

#include <cstddef>
#include <cstdint>

namespace zge
{
    // The channels' state (g_dmaChannelsReady, just below the renderer's).
    struct DmaChannels
    {
        uint32_t ready;           // +0x00 1 from initDmaChannels to shutDownDmaChannels
        uint32_t channels[10];    // +0x04 sceDmaGetChan(0 .. 9): each channel's registers
#if !ZX_GAME_DM2
        uint32_t interrupts;      // +0x2C the channels whose completion interrupt is enabled (bit n: channel n)
        uint32_t enabledInterrupts() const { return interrupts; }
        void setEnabledInterrupts(uint32_t mask) { interrupts = mask; }
#else
        // DM2 keeps no mask: its reset enables no channel's interrupt again.
        uint32_t enabledInterrupts() const { return 0; }
        void setEnabledInterrupts(uint32_t) {}
#endif
        uint32_t completions[10]; // +0x30 a send's callback, called with 0 when its channel's transfer ends
    };
    static_assert(offsetof(DmaChannels, completions) == game::readable::perGame(0x30u, 0x2Cu));
}

namespace zge
{
    // (ps2/dma.cpp)
    void initDmaChannels();
    uint32_t dmaSendChain(uint32_t channel, ee::Address chain, uint32_t flags, uint32_t completion);
    uint32_t dmaSend(uint32_t channel, ee::Address source, uint32_t qwords, uint32_t flags, uint32_t completion);
    void resetDmaChannels();
    void setVblankStartHandler(uint32_t handler);
}
