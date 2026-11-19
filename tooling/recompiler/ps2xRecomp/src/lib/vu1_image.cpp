#include "ps2recomp/vu1_image.h"
#include "ps2recomp/elf_parser.h"

#include <cstring>
#include <sstream>

namespace ps2recomp
{
    namespace
    {
        // DMA tag ids (the source chain's).
        enum : uint32_t
        {
            TagRefe = 0,
            TagCnt,
            TagNext,
            TagRef,
            TagRefs,
            TagCall,
            TagRet,
            TagEnd
        };
        constexpr uint32_t kMaxTags = 1u << 16;
        constexpr uint32_t kVifNop = 0x00u, kVifMpg = 0x4Au;

        std::string hex(uint32_t value)
        {
            std::ostringstream out;
            out << "0x" << std::hex << std::uppercase << value;
            return out.str();
        }

        // `size` bytes of the ELF's data at `address` (whole words).
        bool readData(const ElfParser &elf, uint32_t address, uint32_t size, std::vector<uint8_t> &out)
        {
            for (uint32_t at = 0; at < size; at += 4u)
            {
                if (!elf.isValidAddress(address + at))
                    return false;
                uint32_t word = 0;
                try
                {
                    word = elf.readWord(address + at);
                }
                catch (const std::exception &)
                {
                    return false; // (a section without data: .bss)
                }
                const size_t end = out.size();
                out.resize(end + 4u);
                std::memcpy(out.data() + end, &word, 4u);
            }
            return true;
        }

        // The bytes the DMAC hands the channel for the source chain at `address`.
        bool chainStream(const ElfParser &elf, uint32_t address, bool tte, std::vector<uint8_t> &out, std::string &error)
        {
            uint32_t tadr = address;
            uint32_t stack[2] = {};
            uint32_t depth = 0;
            for (uint32_t n = 0; n < kMaxTags; ++n)
            {
                std::vector<uint8_t> tag;
                if (!readData(elf, tadr, 16u, tag))
                {
                    error = "the chain's tag at " + hex(tadr) + " is not in the ELF's data";
                    return false;
                }
                uint64_t low = 0, high = 0;
                std::memcpy(&low, tag.data(), 8u);
                std::memcpy(&high, tag.data() + 8u, 8u);
                const uint32_t qwc = static_cast<uint32_t>(low & 0xFFFFu);
                const uint32_t id = static_cast<uint32_t>((low >> 28) & 7u);
                const uint32_t addr = static_cast<uint32_t>((low >> 32) & 0x7FFFFFFFu);
                if (low & (1ull << 31))
                {
                    error = "the tag at " + hex(tadr) + " asks for an interrupt (not in a set-up upload)";
                    return false;
                }
                if (low >> 63)
                {
                    error = "the tag at " + hex(tadr) + " points into the scratchpad";
                    return false;
                }
                const uint32_t after = tadr + 16u;
                uint32_t data = after;
                bool end = false;
                switch (id)
                {
                case TagRefe:
                    data = addr;
                    end = true;
                    break;
                case TagCnt:
                    tadr = after + qwc * 16u;
                    break;
                case TagNext:
                    tadr = addr;
                    break;
                case TagRef:
                case TagRefs:
                    data = addr;
                    tadr = after;
                    break;
                case TagCall:
                    if (depth == 2u)
                    {
                        error = "a third call deep at " + hex(tadr);
                        return false;
                    }
                    stack[depth++] = after + qwc * 16u;
                    tadr = addr;
                    break;
                case TagRet:
                    if (depth != 0u)
                        tadr = stack[--depth];
                    else
                        end = true;
                    break;
                default:
                    end = true;
                    break;
                }
                if (tte)
                {
                    const size_t at = out.size();
                    out.resize(at + 8u);
                    std::memcpy(out.data() + at, &high, 8u);
                }
                if (qwc != 0u && !readData(elf, data, qwc * 16u, out))
                {
                    error = "the data at " + hex(data) + " (" + std::to_string(qwc) + " quadwords) is not in the ELF's data";
                    return false;
                }
                if (end)
                    return true;
            }
            error = "the chain at " + hex(address) + " has no end in " + std::to_string(kMaxTags) + " tags";
            return false;
        }
    }

    bool vu1MicroMemory(const ElfParser &elf, uint32_t chain, bool tte, std::vector<uint8_t> &image, std::string &error)
    {
        std::vector<uint8_t> stream;
        if (!chainStream(elf, chain, tte, stream, error))
            return false;
        image.assign(kVu1MicroMemorySize, 0u);
        size_t at = 0;
        while (at + 4u <= stream.size())
        {
            uint32_t code = 0;
            std::memcpy(&code, stream.data() + at, 4u);
            at += 4u;
            const uint32_t command = (code >> 24) & 0x7Fu, num = (code >> 16) & 0xFFu, immediate = code & 0xFFFFu;
            if (command == kVifMpg)
            {
                const uint32_t size = (num != 0u ? num : 256u) * 8u;
                const uint32_t start = immediate * 8u;
                if (start + size > kVu1MicroMemorySize || at + size > stream.size())
                {
                    error = "the MPG at stream byte " + std::to_string(at - 4u) + " goes past the micro memory or the stream";
                    return false;
                }
                std::memcpy(image.data() + start, stream.data() + at, size);
                at += size;
            }
            else if (command != kVifNop)
            {
                error = "VIF code " + hex(code) + " at stream byte " + std::to_string(at - 4u) +
                        ": an upload is NOPs and MPGs";
                return false;
            }
        }
        return true;
    }

    uint64_t vu1CodeHash(const std::vector<uint8_t> &image)
    {
        uint64_t hash = 0xCBF29CE484222325ull;
        for (uint8_t byte : image)
        {
            hash ^= byte;
            hash *= 0x100000001B3ull;
        }
        return hash;
    }
}
