// Based on Blackline Interactive implementation
#include "runtime/memory.h"
#include "runtime/state_probe.h"
#include <cstring>

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    constexpr uint8_t kGifFmtImage = 2u;

    uint32_t pendingGifImageQwc(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            uint64_t tagLo = 0u;
            std::memcpy(&tagLo, data + offset, sizeof(tagLo));
            offset += 16u;

            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;

            uint64_t payloadBytes = 0u;
            if (flg == 0u) // PACKED
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 16ull;
            }
            else if (flg == 1u) // REGLIST, padded to a quadword
            {
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * 8ull;
                payloadBytes = (payloadBytes + 15ull) & ~15ull;
            }
            else if (flg == kGifFmtImage)
            {
                payloadBytes = static_cast<uint64_t>(nloop) * 16ull;
                const uint64_t availableBytes = sizeBytes - offset;
                if (payloadBytes > availableBytes)
                {
                    return static_cast<uint32_t>((payloadBytes - availableBytes) / 16ull);
                }
            }
            else
            {
                return 0u;
            }

            if (payloadBytes > static_cast<uint64_t>(sizeBytes - offset))
                return 0u;
            offset += static_cast<uint32_t>(payloadBytes);
        }

        return 0u;
    }
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;
    if (zx::packets::g_on)
        zx::packets::add(zx::packets::kVif0, data, sizeBytes);

    uint32_t pos = 0;
    while (pos + 4 <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xFFFFu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
        const bool irq = (cmd & 0x80000000u) != 0u;

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if (irq)
            vif0_regs.stat |= (1u << 11);

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif0_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, sizeof(vif0_regs.mask));
            pos += 4u;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.row, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(vif0_regs.col, data + pos, 16u);
            pos += 16u;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            const uint32_t destAddr = static_cast<uint32_t>(imm & 0x1FFu) * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            uint32_t copyBytes = 0u;
            if (m_vu0Code && destAddr < PS2_VU0_CODE_SIZE && mpgBytes > 0u)
            {
                copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU0_CODE_SIZE)
                    copyBytes = PS2_VU0_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu0Code + destAddr, data + pos, copyBytes);
                    markVU0CodeModified();
                }
            }

            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = static_cast<uint8_t>((opcode >> 2) & 0x3u);
            const uint8_t vl = static_cast<uint8_t>(opcode & 0x3u);
            const int components = static_cast<int>(vn) + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3u) ? 4 : 16;
                break;
            default:
                break;
            }
            const int bitsPerVector = (vl == 3u && vn == 3u) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            uint32_t cl = vif0_regs.cycle & 0xFFu;
            uint32_t wl = (vif0_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;
            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }
            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3u) & ~3u;

            if (m_vu0Data && pos + totalBytes <= sizeBytes && vl == 0u)
            {
                uint32_t vuAddr = static_cast<uint32_t>(imm & 0x3FFu);
                if ((imm & 0x8000u) != 0u)
                    vuAddr = (vuAddr + (vif0_regs.tops & 0x3FFu)) & 0x3FFu;
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                    uint32_t destVec = (cl >= wl) ? ((vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu)
                                                  : ((vuAddr + writeIndex) & 0x3FFu);
                    const uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU0_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }
                    if (!sourceAvailable || srcIndex >= sourceVectorCount)
                        continue;
                    const uint8_t *srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                    const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                    for (uint32_t c = 0; c < limit; ++c)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                        lanes[c] = scalar;
                    }
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(m_vu0Data + destOff), _mm_loadu_si128(reinterpret_cast<const __m128i *>(lanes)));
                }
            }
            pos += totalBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            break;
        }
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (sizeBytes == 0u || srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (sizeBytes == 0u)
        return;

    uint32_t pos = 0;

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            std::vector<uint8_t> imagePacket(16u + static_cast<size_t>(chunkQw) * 16u, 0u);
            const uint64_t imageTag =
                static_cast<uint64_t>(chunkQw & 0x7FFFu) |
                ((m_vif1PendingPath2ImageQwc == chunkQw) ? (1ull << 15) : 0ull) |
                (static_cast<uint64_t>(kGifFmtImage) << 58);
            std::memcpy(imagePacket.data(), &imageTag, sizeof(imageTag));
            std::memcpy(imagePacket.data() + 16u, data + pos, static_cast<size_t>(chunkQw) * 16u);
            submitGifPacket(GifPathId::Path2, imagePacket.data(), static_cast<uint32_t>(imagePacket.size()), true, m_vif1PendingPath2DirectHl);

            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
        }

        if (m_vif1StallAfterCommand)
        {
            // The previous VIFcode carried the interrupt bit: stop here until
            // the CPU acknowledges with VIF1_FBRST.STC.
            m_vif1StallAfterCommand = false;
            m_vif1InterruptStalled = true;
            m_vif1StalledStream.assign(data + pos, data + sizeBytes);
            m_pendingIntcCauses.push_back(5u); // INTC VIF1
            return;
        }

        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;
        const bool irq = (cmd & 0x80000000u) != 0u;

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
        {
            vif1_regs.stat |= (1u << 11); // INT
            // ERR.MII masks the interrupt. On the VIF1 worker the EE already
            // cut the stream at the stall (scanVif1Stream).
            if ((vif1_regs.err & 0x1u) == 0u && !onVu1Worker())
                m_vif1StallAfterCommand = true;
        }

        if (opcode == VIF_NOP)
        {
            // A native draw's anchor the EE-side scan took
            // (addVif1NativeAnchor): its GS work goes here.
            if (num == kVif1NativeBegin)
            {
                vif1_regs.code = cmd & ~0x00FF0000u;
                vif1_regs.num = 0;
                runVif1NativeWork(imm);
            }
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIF double-buffer setup. OFFSET clears DBF and resets TOPS to BASE.
            // Do not rewrite BASE from the previous TOPS value.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.tops = vif1_regs.base & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            // BASE only updates the base register. TOPS changes on OFFSET/MSCAL.
            vif1_regs.base = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            // ITOP VIFcode writes pending ITOPS; VU XITOP observes it after MSCAL/MSCNT.
            vif1_regs.itops = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            uint32_t startPC = (uint32_t)imm * 8u;
            // A substituted packet's run (setVif1Substitute): the VIF does
            // what the VIFcode does, VU1 does not run.
            const bool native = num == kVif1NativeSkip || num == kVif1NativeBegin;
            if (native)
            {
                vif1_regs.code = cmd & ~0x00FF0000u;
                vif1_regs.num = 0;
            }

            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (native)
            {
                if (num == kVif1NativeBegin)
                    runVif1NativeWork(imm);
                continue;
            }
            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, runTop, runItop);
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            const bool native = num == kVif1NativeSkip || num == kVif1NativeBegin;
            if (native)
            {
                vif1_regs.code = cmd & ~0x00FF0000u;
                vif1_regs.num = 0;
            }
            const uint32_t runTop = vif1_regs.tops & 0x3FFu;
            const uint32_t runItop = vif1_regs.itops & 0x3FFu;
            vif1_regs.top = runTop;
            vif1_regs.itop = runItop;

            const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
            if (dbf)
                vif1_regs.tops = vif1_regs.base & 0x3FFu;
            else
                vif1_regs.tops = (vif1_regs.base + vif1_regs.ofst) & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF

            if (native)
            {
                if (num == kVif1NativeBegin)
                    runVif1NativeWork(imm);
                continue;
            }
            if (m_vu1MscntCallback)
                m_vu1MscntCallback(runTop, runItop);
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                    markVU1CodeModified();
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                submitGifPacket(GifPathId::Path2, data + pos, qwCount * 16, true, directHl);

                const uint32_t pendingImageQw = pendingGifImageQwc(data + pos, qwCount * 16u);
                if (pendingImageQw != 0u)
                {
                    m_vif1PendingPath2ImageQwc = pendingImageQw;
                    m_vif1PendingPath2DirectHl = directHl;
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;

            // A substituted packet's data (setVif1Substitute, marked in
            // IMMEDIATE bit 13, which UNPACK does not use): nothing reads
            // it, as no microprogram of the packet runs; only a
            // difference-mode unpack's ROW changes would outlive it.
            if ((imm & kVif1NativeUnpackSkip) != 0u && (vif1_regs.mode & 3u) != 2u)
            {
                pos += totalBytes;
                if (pos > sizeBytes)
                    break;
                continue;
            }

            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes && cl >= wl &&
                (vl != 3u || vn == 3u) && !s_vif1GeneralUnpack)
            {
                // Every write cycle reads source data (CL >= WL): the loop
                // below with the mask selectors of each write cycle position
                // worked out once (same values, same ROW updates).
                const uint8_t *src = data + pos;
                const uint32_t mode = vif1_regs.mode & 3u;
                const bool add = (vl != 3u || vn != 3u) && (mode == 1u || mode == 2u);
                const uint32_t lanesGiven = (components == 1 || vl == 3u) ? 4u : static_cast<uint32_t>(components);
                // Per position (0-3, later ones as 3): selector per field
                // (0 data, 1 ROW, 2 COL, 3 protect), and whether it is plain
                // data in every field.
                uint8_t selector[4][4] = {};
                bool plain[4] = {true, true, true, true};
                if (maskEnable)
                {
                    for (uint32_t position = 0; position < 4u; ++position)
                        for (uint32_t field = 0; field < 4u; ++field)
                        {
                            selector[position][field] = static_cast<uint8_t>((vif1_regs.mask >> ((position * 4u + field) * 2u)) & 0x3u);
                            plain[position] = plain[position] && selector[position][field] == 0u;
                        }
                }
                uint32_t block = 0u, cycle = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex, src += bytesPerVector)
                {
                    const uint32_t destVec = (vuAddr + block * cl + cycle) & 0x3FFu;
                    const uint32_t position = cycle > 3u ? 3u : cycle;
                    if (++cycle == wl)
                    {
                        cycle = 0u;
                        ++block;
                    }
                    uint8_t *dst = m_vu1Data + destVec * 16u;
                    uint32_t v[4];
                    if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            std::memcpy(&v[0], src, 4u);
                            v[1] = v[2] = v[3] = v[0];
                        }
                        else
                            std::memcpy(v, src, static_cast<size_t>(components) * 4u);
                    }
                    else if (vl == 1u)
                    {
                        for (int c = 0; c < components; ++c)
                        {
                            uint16_t raw;
                            std::memcpy(&raw, src + c * 2, 2u);
                            v[c] = zeroExtend ? raw : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                        }
                        if (components == 1)
                            v[1] = v[2] = v[3] = v[0];
                    }
                    else if (vl == 2u)
                    {
                        for (int c = 0; c < components; ++c)
                            v[c] = zeroExtend ? src[c] : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(src[c])));
                        if (components == 1)
                            v[1] = v[2] = v[3] = v[0];
                    }
                    else
                    {
                        uint16_t packed;
                        std::memcpy(&packed, src, 2u);
                        v[0] = packed & 0x1Fu;
                        v[1] = (packed >> 5) & 0x1Fu;
                        v[2] = (packed >> 10) & 0x1Fu;
                        v[3] = (packed >> 15) & 0x01u;
                    }
                    if (!add && plain[position])
                    {
                        std::memcpy(dst, v, lanesGiven * 4u);
                        continue;
                    }
                    // Lanes the format does not give keep their memory
                    // value (plus ROW in offset/difference mode).
                    uint32_t lanes[4];
                    std::memcpy(lanes, dst, sizeof(lanes));
                    for (uint32_t field = 0; field < 4u; ++field)
                    {
                        switch (selector[position][field])
                        {
                        case 0u:
                        {
                            uint32_t value = field < lanesGiven ? v[field] : lanes[field];
                            if (add)
                            {
                                value += vif1_regs.row[field];
                                if (mode == 2u)
                                    vif1_regs.row[field] = value;
                            }
                            lanes[field] = value;
                            break;
                        }
                        case 1u:
                            lanes[field] = vif1_regs.row[field];
                            break;
                        case 2u:
                            lanes[field] = vif1_regs.col[position];
                            break;
                        default:
                            break; // write-protect
                        }
                    }
                    std::memcpy(dst, lanes, sizeof(lanes));
                }
            }
            else if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                }
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
}

uint32_t PS2Memory::vif1UnpackBytes(uint32_t cmd, uint32_t cycle)
{
    const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
    const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xFFu);
    const uint8_t vn = (opcode >> 2) & 0x3u;
    const uint8_t vl = opcode & 0x3u;
    const int components = vn + 1;
    int bitsPerComponent = 32;
    switch (vl)
    {
    case 1:
        bitsPerComponent = 16;
        break;
    case 2:
        bitsPerComponent = 8;
        break;
    case 3:
        bitsPerComponent = (vn == 3) ? 4 : 16;
        break;
    default:
        break;
    }
    const int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
    const uint32_t bytesPerVector = static_cast<uint32_t>((bitsPerVector + 7) / 8);
    const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
    uint32_t cl = cycle & 0xFFu;
    uint32_t wl = (cycle >> 8) & 0xFFu;
    if (cl == 0u)
        cl = 1u;
    if (wl == 0u)
        wl = 1u;
    uint32_t sourceVectorCount = writeVectorCount;
    if (cl < wl)
    {
        const uint32_t fullBlocks = writeVectorCount / wl;
        uint32_t remainder = writeVectorCount % wl;
        if (remainder > cl)
            remainder = cl;
        sourceVectorCount = fullBlocks * cl + remainder;
    }
    return (sourceVectorCount * bytesPerVector + 3u) & ~3u;
}

// The framing of processVIF1Data and the register effects of its VIFcodes,
// without unpacking or running VU1: where an interrupt-bit VIFcode stops the
// stream, and the VIF1 registers and PATH3 mask the EE will read. Runs on
// the EE thread on what will be queued for the VIF1 worker, on m_vif1Shadow.
uint32_t PS2Memory::scanVif1Stream(const uint8_t *data, uint32_t sizeBytes, bool &stalls, bool *incomplete,
                                   std::vector<uint32_t> *mscals)
{
    VIFRegisters &r = m_vif1Shadow;
    stalls = false;
    if (incomplete)
        *incomplete = false;
    uint32_t pos = 0;
    // Where the current VIFcode starts and what it changed before its data
    // was found to be cut off (incomplete mode undoes it).
    uint32_t cmdStart = 0;
    uint32_t savedCode = 0, savedNum = 0, savedStat = 0;
    bool savedStallAfter = false;
    auto cutOff = [&]() -> uint32_t
    {
        r.code = savedCode;
        r.num = savedNum;
        r.stat = savedStat;
        m_vif1ScanStallAfter = savedStallAfter;
        *incomplete = true;
        return cmdStart;
    };
    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1ScanPendingImageQwc != 0u)
        {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
                break;
            const uint32_t chunkQw = std::min<uint32_t>(m_vif1ScanPendingImageQwc, availableQw);
            pos += chunkQw * 16u;
            m_vif1ScanPendingImageQwc -= chunkQw;
            continue;
        }
        if (m_vif1ScanStallAfter)
        {
            m_vif1ScanStallAfter = false;
            stalls = true;
            return pos;
        }

        cmdStart = pos;
        savedCode = r.code;
        savedNum = r.num;
        savedStat = r.stat;
        savedStallAfter = m_vif1ScanStallAfter;
        uint32_t cmd;
        std::memcpy(&cmd, data + pos, 4);
        pos += 4;
        const uint8_t opcode = (cmd >> 24) & 0x7F;
        const uint16_t imm = cmd & 0xFFFF;
        const uint8_t num = (cmd >> 16) & 0xFF;
        r.code = cmd;
        r.num = num;
        if ((cmd & 0x80000000u) != 0u)
        {
            r.stat |= (1u << 11); // INT
            if ((vif1_regs.err & 0x1u) == 0u) // ERR is only written by the EE
                m_vif1ScanStallAfter = true;
        }

        if (opcode == VIF_NOP)
        {
            if (num == kVif1NativeAnchor && mscals)
                mscals->push_back(cmdStart); // a native draw's anchor (addVif1NativeAnchor)
        }
        else if (opcode == VIF_STCYCL)
            r.cycle = imm;
        else if (opcode == VIF_OFFSET)
        {
            r.ofst = imm & 0x3FFu;
            r.tops = r.base & 0x3FFu;
            r.stat &= ~(1u << 7);
        }
        else if (opcode == VIF_BASE)
            r.base = imm & 0x3FFu;
        else if (opcode == VIF_ITOP)
            r.itops = imm & 0x3FFu;
        else if (opcode == VIF_STMOD)
            r.mode = imm & 3u;
        else if (opcode == VIF_MSKPATH3)
            m_path3MaskedShadow = (imm & 0x8000u) != 0u;
        else if (opcode == VIF_MARK)
        {
            r.mark = imm;
            r.stat |= (1u << 6);
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF || opcode == VIF_MSCNT)
        {
            if (mscals)
                mscals->push_back(cmdStart);
            r.top = r.tops & 0x3FFu;
            r.itop = r.itops & 0x3FFu;
            const bool dbf = (r.stat & (1u << 7)) != 0u;
            r.tops = dbf ? (r.base & 0x3FFu) : ((r.base + r.ofst) & 0x3FFu);
            r.stat ^= (1u << 7);
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
            {
                if (incomplete)
                    return cutOff();
                break;
            }
            std::memcpy(&r.mask, data + pos, sizeof(r.mask));
            pos += 4;
        }
        else if (opcode == VIF_STROW || opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
            {
                if (incomplete)
                    return cutOff();
                break;
            }
            std::memcpy(opcode == VIF_STROW ? r.row : r.col, data + pos, 16);
            pos += 16;
        }
        else if (opcode == VIF_MPG)
        {
            pos += ((num == 0u) ? 256u : static_cast<uint32_t>(num)) * 8u;
            if (pos > sizeBytes)
            {
                if (incomplete)
                    return cutOff();
                break;
            }
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (truncated && incomplete)
                return cutOff();
            if (qwCount > availableQw)
                qwCount = availableQw;
            if (qwCount > 0)
            {
                const uint32_t pendingImageQw = pendingGifImageQwc(data + pos, qwCount * 16u);
                if (pendingImageQw != 0u)
                    m_vif1ScanPendingImageQwc = pendingImageQw;
            }
            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
        }
        else if ((opcode & 0x60) == 0x60)
        {
            if (mscals)
                mscals->push_back(cmdStart); // (native draws: the data of a substituted packet's)
            // Mode 2 (difference) unpacks also change ROW, which the EE does
            // not read back (ROW reads see the registers it wrote).
            pos += vif1UnpackBytes(cmd, r.cycle);
            if (pos > sizeBytes)
            {
                if (incomplete)
                    return cutOff();
                break;
            }
        }
    }
    if (incomplete && pos < sizeBytes)
    {
        // Less than a VIFcode left.
        *incomplete = true;
        return pos;
    }
    return sizeBytes;
}
