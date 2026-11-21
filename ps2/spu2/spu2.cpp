// SPDX-License-Identifier: GPL-3.0+
//
// The SPU2's front end (spu2/spu2.h) around the PCSX2-derived Spu2System.

#include "spu2.h"
#include "spu2_internal.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace zx
{
    Spu2::Spu2(std::span<uint8_t> iopRam)
        : m_system(std::make_unique<spu2::Spu2System>(iopRam))
    {
    }

    Spu2::~Spu2() = default;

    void Spu2::reset()
    {
        m_system->Reset();
    }

    void Spu2::setClock(const uint64_t *iopCycles)
    {
        m_system->Clock = iopCycles;
        m_system->lClocks = m_system->Now();
        for (auto &core : m_system->Cores)
            core.LastClock = m_system->lClocks;
    }

    void Spu2::setOutput(OutputCallback output)
    {
        m_system->Output = std::move(output);
    }

    void Spu2::setTrace(std::function<void(const char *)> trace)
    {
        m_system->Trace = std::move(trace);
        const char *budget = std::getenv("ZX_TRACE_SPU2");
        m_system->TraceBudget = budget ? std::strtol(budget, nullptr, 0) : 0;
    }

    uint16_t Spu2::readRegister(uint32_t physical)
    {
        return m_system->Read(physical);
    }

    void Spu2::writeRegister(uint32_t physical, uint16_t value)
    {
        m_system->TraceEvent("[spu2] w %03x = %04x", physical & 0x7FFu, value);
        m_system->Write(physical, value);
    }

    uint32_t Spu2::readDmaRegister(uint32_t physical)
    {
        spu2::Spu2System &sys = *m_system;
        sys.TimeUpdate(sys.Now());
        const int core = physical >= 0x1F801500u ? 1 : 0;
        const spu2::IopDmaChannel &ch = sys.Dma[core];
        switch (physical & 0xCu)
        {
        case 0x0:
            return ch.MADR;
        case 0x4:
            return ch.BCR;
        case 0x8:
            return ch.CHCR;
        default:
            return ch.TADR;
        }
    }

    void Spu2::writeDmaRegister(uint32_t physical, uint32_t value)
    {
        spu2::Spu2System &sys = *m_system;
        const int core = physical >= 0x1F801500u ? 1 : 0;
        spu2::IopDmaChannel &ch = sys.Dma[core];
        switch (physical & 0xCu)
        {
        case 0x0:
            ch.MADR = value & 0x00FFFFFFu;
            break;
        case 0x4:
            ch.BCR = value;
            break;
        case 0x8:
        {
            // Start on a 0 -> 1 transition of bit 24, so read-modify-write of
            // a busy channel does not restart it.
            const bool wasBusy = (ch.CHCR & 0x01000000u) != 0u;
            ch.CHCR = value;
            if ((value & 0x01000000u) && !wasBusy)
                sys.StartDma(core);
            break;
        }
        default:
            ch.TADR = value & 0x00FFFFFFu;
            break;
        }
    }

    void Spu2::advance()
    {
        m_system->TimeUpdate(m_system->Now());
    }

    void Spu2::advanceTo(uint64_t iopCycle)
    {
        m_system->TimeUpdate(iopCycle);
    }

    uint64_t Spu2::nextEventCycle() const noexcept
    {
        const spu2::Spu2System &sys = *m_system;
        uint64_t next = sys.lClocks + spu2::Spu2System::TickInterval;
        for (const auto &core : sys.Cores)
        {
            if (core.DMAICounter > 0)
                next = std::min<uint64_t>(next, core.LastClock + static_cast<uint64_t>(core.DMAICounter));
        }
        return next;
    }

    bool Spu2::dmaActive() const noexcept
    {
        for (const auto &core : m_system->Cores)
        {
            if (core.DMAICounter > 0)
                return true;
        }
        return false;
    }

    bool Spu2::finishDma()
    {
        spu2::Spu2System &sys = *m_system;
        bool finished = false;
        for (int c = 0; c < 2; ++c)
        {
            spu2::V_Core &core = sys.Cores[c];
            if ((core.AutoDMACtrl & (c + 1)) == (c + 1))
                continue; // ADMA streams follow the mixer
            // Each step lets the remaining transfer time pass (DMA progress
            // is measured from LastClock); a write may continue in pieces.
            for (int step = 0; step < 4096 && core.DMAICounter > 0; ++step)
            {
                finished = true;
                const uint64_t now = sys.Now();
                core.LastClock = now - std::min<uint64_t>(now, static_cast<uint64_t>(core.DMAICounter));
                if (core.LastClock == now)
                    break;
                sys.CheckDMAProgress(c);
            }
        }
        return finished;
    }

    uint32_t Spu2::takeInterrupts() noexcept
    {
        const uint32_t pending = m_system->PendingIrqs;
        m_system->PendingIrqs = 0;
        return pending;
    }

    bool Spu2::hasPendingInterrupts() const noexcept
    {
        return m_system->PendingIrqs != 0u;
    }

    void Spu2::flushOutput()
    {
        m_system->FlushOutput();
    }

    uint64_t Spu2::samplesMixed() const noexcept
    {
        return m_system->SamplesMixed;
    }

    uint16_t Spu2::readSpuRam(uint32_t halfwordAddress) const noexcept
    {
        return static_cast<uint16_t>(m_system->spu2mem[halfwordAddress & 0xFFFFFu]);
    }

    void Spu2::writeSpuRam(uint32_t halfwordAddress, uint16_t value) noexcept
    {
        m_system->spu2M_Write(halfwordAddress, value);
    }

    uint32_t Spu2::activeVoiceMask(int core) const noexcept
    {
        uint32_t mask = 0;
        const auto &c = m_system->Cores[core & 1];
        for (uint32_t v = 0; v < spu2::V_Core::NumVoices; ++v)
        {
            if (c.Voices[v].ADSR.Phase != spu2::V_ADSR::PHASE_STOPPED)
                mask |= 1u << v;
        }
        return mask;
    }

    void Spu2::decodeAdpcmBlock(const uint8_t block[16], int16_t out[28], int32_t &prev1, int32_t &prev2)
    {
        spu2::DecodeAdpcmBlock(block, out, prev1, prev2);
    }
}
