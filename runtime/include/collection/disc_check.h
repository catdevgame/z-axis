#pragma once

// The game files bundled with the app, checked: SYSTEM.CNF names the boot
// ELF, whose SHA-256 must be the one the code was recompiled from.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::app
{
    std::string sha256Hex(const uint8_t *data, size_t size);

    // SHA-256 of bytes given piece by piece (a file as it is copied).
    class Sha256
    {
    public:
        void update(const uint8_t *data, size_t size);
        // The digest in lower-case hex; the hash is used up.
        std::string finishHex();

    private:
        void compress(const uint8_t *chunk);
        std::array<uint32_t, 8> m_state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                           0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::array<uint8_t, 64> m_buffer{};
        size_t m_buffered = 0;
        uint64_t m_length = 0;
    };

    // BOOT2 of a SYSTEM.CNF ("cdrom0:\SLUS_203.27;1" -> "SLUS_203.27"), or "".
    std::string bootElfFromSystemCnf(std::string_view systemCnf);

    struct DiscCheck
    {
        enum class Status
        {
            Ok,
            Missing,      // no such folder
            NotPs2Disc,   // no SYSTEM.CNF / BOOT2, or the ELF it names is missing
            WrongGame,    // another game's ELF
            WrongVersion, // right ELF name, different contents
        };
        Status status = Status::Missing;
        std::string message;   // for the user
        std::string bootElf;   // as named in SYSTEM.CNF
        std::string elfSha256; // of the disc's ELF
        std::vector<uint8_t> elf;
    };

    // Checks a folder holding the disc's files (as extracted from the disc
    // image when the app was built): reads SYSTEM.CNF and the boot ELF and
    // compares it with the ELF the code was recompiled from (empty
    // expectedSha256: any ELF with the expected name).
    DiscCheck checkDiscFolder(const std::filesystem::path &folder,
                              const std::string &gameTitle,
                              const std::string &expectedElf,
                              const std::string &expectedSha256);
}
