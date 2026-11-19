#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace GSMem
{
    class TexturePageCache
    {
    public:
        static constexpr uint32_t kPageSize = 8192u;

        void Invalidate() noexcept
        {
            m_pageBase = UINT32_MAX;
        }

        // byteAddress is the wrapped, swizzled VRAM address. The returned
        // pointer is valid only until the next miss or invalidation.
        const uint8_t* Resolve(const uint8_t* vram, uint32_t byteAddress) noexcept
        {
            const uint32_t pageBase = byteAddress & ~(kPageSize - 1u);
            if (m_pageBase != pageBase)
            {
                std::memcpy(m_bytes.data(), vram + pageBase, kPageSize);
                m_pageBase = pageBase;
            }
            return m_bytes.data() + (byteAddress & (kPageSize - 1u));
        }

        bool HasPage() const noexcept { return m_pageBase != UINT32_MAX; }
        uint32_t PageBase() const noexcept { return m_pageBase; }
        const uint8_t* Data() const noexcept { return m_bytes.data(); }

        // Takes a fresh copy of the page at pageBase (byte address).
        void Load(const uint8_t* vram, uint32_t pageBase) noexcept
        {
            m_pageBase = pageBase & ~(kPageSize - 1u);
            std::memcpy(m_bytes.data(), vram + m_pageBase, kPageSize);
        }

    private:
        alignas(64) std::array<uint8_t, kPageSize> m_bytes{};
        uint32_t m_pageBase = UINT32_MAX;
    };
}
