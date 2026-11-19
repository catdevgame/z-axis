#ifndef PS2RECOMP_VU1_IMAGE_H
#define PS2RECOMP_VU1_IMAGE_H

// A game's VU1 micro memory image from its ELF: the code its renderer uploads to VU1 at set-up, which the runtime's
// ahead-of-time compiled microprograms are made from (ps2xVu1Aot, runtime/ps2_vu1_aot_generator.h). As the workshop's
// runtime/tools/vu1_image.py: the DMA chain the renderer sends VIF1 (with CHCR.TTE each tag's upper 64 bits are two
// VIF codes, then the tag's data), walked as the DMAC walks a source chain, and its MPG codes written into an empty
// 16 KiB micro memory. Named by its FNV-1a hash (VU1Interpreter::codeHash), which the generated code registers under.

#include <cstdint>
#include <string>
#include <vector>

namespace ps2recomp
{
    class ElfParser;

    constexpr uint32_t kVu1MicroMemorySize = 0x4000u;

    // The micro memory after the chain at `chain`: false with the reason in `error` (a tag or its data outside the
    // ELF's data, a VIF code other than NOP and MPG, an MPG past the micro memory).
    bool vu1MicroMemory(const ElfParser &elf, uint32_t chain, bool tte, std::vector<uint8_t> &image, std::string &error);

    // FNV-1a, 64 bits.
    uint64_t vu1CodeHash(const std::vector<uint8_t> &image);
}

#endif // PS2RECOMP_VU1_IMAGE_H
