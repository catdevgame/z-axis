// The engine's DMA channels (dmaCompletionHandler-vif1Interrupt; the PS2's DMA controller, as the frame, GS and
// texture sends of zge/render/ps2 use it): libdma's channel handles
// kept by number, a send that waits for the channel first and may wait for
// the end, the VIF1 channel's completion interrupt calling a send's
// callback once, and the vblank-start interrupt's handler set up.
//
// A send's flags: the low byte is the channel's CHCR (direction, mode, tag
// transfer, interrupt enable), 0x4 a chain (CHCR written first, then
// sceDmaSend; else sceDmaSendN of qwords), 0x100 wait until it has ended.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/mirror.h"
#include "ee/io.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/render/display.h"
#include "zge/render/ps2/dma.h"
#include "zge/platform/platform.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/ps2/dma.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kDmaChannelsAddress = sym::g_dmaChannelsReady;
        inline ee::Global<DmaChannels, kDmaChannelsAddress> g_dmaChannels;

        constexpr uint32_t kChannelVif1 = 1, kChannelGif = 2;
        constexpr uint32_t kInterruptVif1 = 1u << kChannelVif1, kInterruptGif = 1u << kChannelGif;
        constexpr uint32_t kChannelCount = 10;
        constexpr uint32_t kChcrInterruptEnable = 0x40u; // CHCR.TIE
        constexpr uint32_t kSendChain = 0x4u;
        constexpr uint32_t kSendWait = 0x100u;
        constexpr uint32_t kIntcVblankStart = 5;

        // The DMA controller's view of a source: the scratchpad (its base
        // ..) by its offset with the SPR bit, else the physical address.
        constexpr uint32_t kScratchpadSegment = 0x7u;
        constexpr uint32_t kScratchpadOffsetMask = 0x3FF0u;
        constexpr uint32_t kSourceFromScratchpad = 0x80000000u;

        constexpr uint32_t kSceDmaGetChan = sym::sceDmaGetChan; // (channel) -> its registers
        constexpr uint32_t kSceDmaReset = sym::sceDmaReset;   // (mode)
        constexpr uint32_t kSceDmaSend = sym::sceDmaSend;    // (channel, chain)
        constexpr uint32_t kSceDmaSendN = sym::sceDmaSendN;   // (channel, address, qwords)
        constexpr uint32_t kSceDmaSync = sym::sceDmaSync;    // (channel, 1: block, 0)
        constexpr uint32_t kAddIntcHandler = sym::addIntcHandler; // (cause, handler, next)
        constexpr uint32_t kAddDmacHandler = sym::addDmacHandler; // (channel, handler, next, argument)
        constexpr uint32_t kEnableIntc = sym::EnableIntc;    // (cause)
        constexpr uint32_t kEnableDmac = sym::EnableDmac;    // (channel)
        constexpr uint32_t kDmaCompletionHandler = sym::dmaCompletionHandler;
    }

    namespace
    {
        void runCompletion(uint32_t completion) { ee::call(completion, 0); }
    }

    // dmaCompletionHandler: the VIF1 channel's completion interrupt (AddDmacHandler):
    // the channel's callback, once; interrupts on again. 0.
    uint32_t dmaCompletionHandler(uint32_t channel)
    {
        uint32_t &completion = ee::Ptr<DmaChannels>(kDmaChannelsAddress)->completions[channel];
        if (completion != 0)
        {
            runCompletion(completion);
            completion = 0;
        }
        ee::io::enableInterrupts(); // EI
        return 0;
    }
    READABLE_FUNCTION(dmaCompletionHandler, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        uint32_t sceDmaGetChan(uint32_t channel) { return ee::game<uint32_t>(kSceDmaGetChan, channel); }
        void addDmacHandler(uint32_t channel, uint32_t handler, uint32_t next, uint32_t argument) { ee::game(kAddDmacHandler, channel, handler, next, argument); }
    }

    // initDmaChannels: the ten channels' handles, the GIF channel's interrupt
    // enable cleared, the VIF1 channel's completion handler added and its
    // interrupt enabled.
    void initDmaChannels()
    {
        DmaChannels &dma = *g_dmaChannels;
        dma.ready = 1;
        for (uint32_t channel = 0; channel < kChannelCount; ++channel)
            dma.channels[channel] = sceDmaGetChan(channel);
        const uint32_t gifChcr = dma.channels[kChannelGif];
        ee::io::storeAny<uint32_t>(gifChcr, ee::io::loadAny<uint32_t>(gifChcr) & ~kChcrInterruptEnable);
        dma.setEnabledInterrupts(0);
        // (The handler's argument is the mask just used, passed on as it is;
        // DM2 adds no completion handler.)
        if constexpr (!game::readable::kIsDm2)
            addDmacHandler(kChannelVif1, kDmaCompletionHandler, 0, static_cast<int32_t>(~kChcrInterruptEnable));
        dma.setEnabledInterrupts(dma.enabledInterrupts() | kInterruptVif1);
        resetDmaChannels();
        if constexpr (game::readable::kIsDm2)
            saveGlobalPointer(); // (ZGE2 keeps the global pointer here, ZGE3 in initDisplayState)
    }
    READABLE_FUNCTION(initDmaChannels, Verified);

    // shutDownDmaChannels (no caller). 1.
    uint32_t shutDownDmaChannels()
    {
        if (g_dmaChannels->ready == 0)
            return 1;
        g_dmaChannels->ready = 0;
        return 1;
    }
    READABLE_FUNCTION(shutDownDmaChannels, Draft);

    // dmaSendChain: a chain sent (dmaSend with the chain flag). 1.
    uint32_t dmaSendChain(uint32_t channel, ee::Address chain, uint32_t flags, uint32_t completion)
    {
        return dmaSend(channel, chain, 0, flags | kSendChain, completion);
    }
    READABLE_FUNCTION(dmaSendChain, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sceDmaSync(uint32_t registers, uint32_t value, uint32_t value2) { ee::game(kSceDmaSync, registers, value, value2); }
        void sceDmaSend(uint32_t channel, uint32_t chain) { ee::game(kSceDmaSend, channel, chain); }
        void sceDmaSendN(uint32_t channel, uint32_t address, uint32_t qwords) { ee::game(kSceDmaSendN, channel, address, qwords); }
    }

    // dmaSend: source sent on the channel (flags: see the top) once the
    // channel is free, completion kept for its interrupt. 1.
    uint32_t dmaSend(uint32_t channel, ee::Address source, uint32_t qwords, uint32_t flags, uint32_t completion)
    {
        uint32_t address = 0;
        const uint32_t word = source;
        if (word >> 28 == kScratchpadSegment)
            address = (word & kScratchpadOffsetMask) | kSourceFromScratchpad;
        else
            address = ee::physical(word);
        DmaChannels &dma = *g_dmaChannels;
        const uint32_t registers = dma.channels[channel];
        sceDmaSync(registers, 1, 0);
        const bool chain = (flags & kSendChain) != 0;
        if (chain)
            ee::io::storeAny<uint32_t>(registers, flags & 0xFFu); // CHCR
        dma.completions[channel] = completion;
        if (chain)
            sceDmaSend(registers, address);
        else
            sceDmaSendN(registers, address, qwords);
        if ((flags & kSendWait) != 0)
            sceDmaSync(registers, 1, 0);
        return 1;
    }
    READABLE_FUNCTION(dmaSend, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void sceDmaReset(uint32_t mode) { ee::game(kSceDmaReset, mode); }
        void EnableDmac(uint32_t channel) { ee::game(kEnableDmac, channel); }
        namespace tail
        {
        void sceDmaReset(uint32_t mode) { ee::tailCall(kSceDmaReset, mode); }
        void EnableDmac(uint32_t channel) { ee::tailCall(kEnableDmac, channel); }
        }
    }

    // resetDmaChannels: the DMA controller reset and the channels' completion
    // interrupts enabled again (the GIF's, then the VIF1's: the original's
    // tail call).
    void resetDmaChannels()
    {
        if constexpr (game::readable::kIsDm2) // (ZGE2's ends here, as a tail call: DmaChannels has no interrupt mask there)
        {
            tail::sceDmaReset(1);
            return;
        }
        sceDmaReset(1);
        uint32_t interrupts = g_dmaChannels->enabledInterrupts();
        if ((interrupts & kInterruptGif) != 0)
        {
            EnableDmac(kChannelGif);
            interrupts = g_dmaChannels->enabledInterrupts();
        }
        if ((interrupts & kInterruptVif1) != 0)
        {
            tail::EnableDmac(kChannelVif1); // the original's tail call
            return;
        }
    }
    READABLE_FUNCTION(resetDmaChannels, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        void addIntcHandler(uint32_t cause, uint32_t handler, uint32_t next) { ee::game(kAddIntcHandler, cause, handler, next); }
        namespace tail
        {
        void EnableIntc(uint32_t cause) { ee::tailCall(kEnableIntc, cause); }
        }
    }

    // setVblankStartHandler: handler added last for the vblank-start interrupt and
    // the interrupt enabled (the original's tail call).
    void setVblankStartHandler(uint32_t handler)
    {
        addIntcHandler(kIntcVblankStart, handler, -1);
        tail::EnableIntc(kIntcVblankStart); // the original's tail call
    }
    READABLE_FUNCTION(setVblankStartHandler, Verified);
}
