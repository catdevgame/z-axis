// The bundled disc's check (collection/disc_check.h): SYSTEM.CNF's boot ELF
// and its SHA-256 against the one the code was recompiled from.
#include "collection/disc_check.h"
#include "strings.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ps2x::app
{
    // ---- SHA-256 (FIPS 180-4) ----------------------------------------------

    namespace
    {
        constexpr std::array<uint32_t, 64> kRound = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        constexpr uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    }

    void Sha256::compress(const uint8_t *chunk)
    {
        std::array<uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i)
        {
            const uint8_t *p = chunk + static_cast<size_t>(i) * 4u;
            w[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) | p[3];
        }
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        uint32_t e = m_state[4], f = m_state[5], g = m_state[6], hh = m_state[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + s1 + ch + kRound[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += hh;
    }

    void Sha256::update(const uint8_t *data, size_t size)
    {
        m_length += size;
        if (m_buffered != 0u)
        {
            const size_t take = std::min(size, m_buffer.size() - m_buffered);
            std::memcpy(m_buffer.data() + m_buffered, data, take);
            m_buffered += take;
            data += take;
            size -= take;
            if (m_buffered < m_buffer.size())
                return;
            compress(m_buffer.data());
            m_buffered = 0u;
        }
        for (; size >= 64u; data += 64, size -= 64u)
            compress(data);
        std::memcpy(m_buffer.data(), data, size);
        m_buffered = size;
    }

    std::string Sha256::finishHex()
    {
        const uint64_t bitLength = m_length * 8u;
        uint8_t padding[72] = {0x80u};
        const size_t padBytes = (m_buffered < 56u ? 56u : 120u) - m_buffered;
        update(padding, padBytes);
        uint8_t lengthBytes[8];
        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = static_cast<uint8_t>(bitLength >> ((7 - i) * 8));
        update(lengthBytes, sizeof(lengthBytes));

        static constexpr char kHex[] = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (uint32_t word : m_state)
            for (int i = 3; i >= 0; --i)
            {
                const uint8_t byte = static_cast<uint8_t>(word >> (i * 8));
                out.push_back(kHex[byte >> 4]);
                out.push_back(kHex[byte & 15u]);
            }
        return out;
    }

    std::string sha256Hex(const uint8_t *data, size_t size)
    {
        Sha256 hash;
        hash.update(data, size);
        return hash.finishHex();
    }

    // ---- disc check -----------------------------------------------------------

    std::string bootElfFromSystemCnf(std::string_view systemCnf)
    {
        std::istringstream in{std::string(systemCnf)};
        std::string line;
        while (std::getline(in, line))
        {
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            if (lower(trim(std::string_view(line).substr(0, equals))) != "boot2")
                continue;
            std::string value = trim(std::string_view(line).substr(equals + 1));
            const size_t colon = value.find(':');
            if (colon != std::string::npos)
                value.erase(0, colon + 1);
            while (!value.empty() && (value.front() == '\\' || value.front() == '/'))
                value.erase(value.begin());
            if (const size_t semicolon = value.find(';'); semicolon != std::string::npos)
                value.erase(semicolon);
            return trim(value);
        }
        return {};
    }

    namespace
    {
        bool readWholeFile(const std::filesystem::path &file, size_t limit, std::vector<uint8_t> &bytes)
        {
            std::error_code ec;
            const uintmax_t size = std::filesystem::file_size(file, ec);
            if (ec || size > limit)
                return false;
            std::ifstream in(file, std::ios::binary);
            if (!in)
                return false;
            bytes.resize(static_cast<size_t>(size));
            in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            return in.gcount() == static_cast<std::streamsize>(bytes.size());
        }
    }

    DiscCheck checkDiscFolder(const std::filesystem::path &folder,
                              const std::string &gameTitle,
                              const std::string &expectedElf,
                              const std::string &expectedSha256)
    {
        DiscCheck result;
        std::error_code ec;
        if (folder.empty() || !std::filesystem::is_directory(folder, ec))
        {
            result.status = DiscCheck::Status::Missing;
            result.message = "The game files of " + gameTitle + " are missing (" + folder.string() + ").";
            return result;
        }

        std::vector<uint8_t> cnf;
        if (!readWholeFile(folder / "SYSTEM.CNF", 64u * 1024u, cnf) ||
            (result.bootElf = bootElfFromSystemCnf(std::string(cnf.begin(), cnf.end()))).empty())
        {
            result.status = DiscCheck::Status::NotPs2Disc;
            result.message = "The game files in " + folder.string() + " are incomplete (no SYSTEM.CNF boot entry).";
            return result;
        }

        if (lower(result.bootElf) != lower(expectedElf))
        {
            result.status = DiscCheck::Status::WrongGame;
            result.message = "The game files in " + folder.string() + " are a different game (" + result.bootElf +
                             "). This app needs " + gameTitle + " (" + expectedElf + ").";
            return result;
        }

        std::string elfPath = result.bootElf;
        std::replace(elfPath.begin(), elfPath.end(), '\\', '/');
        if (!readWholeFile(folder / elfPath, 64u * 1024u * 1024u, result.elf))
        {
            result.status = DiscCheck::Status::NotPs2Disc;
            result.message = "The game files in " + folder.string() + " name " + result.bootElf + " but it cannot be read.";
            result.elf.clear();
            return result;
        }

        result.elfSha256 = sha256Hex(result.elf.data(), result.elf.size());
        if (!expectedSha256.empty() && lower(expectedSha256) != result.elfSha256)
        {
            result.status = DiscCheck::Status::WrongVersion;
            result.message = "The game files in " + folder.string() + " hold a different version of " + expectedElf +
                             " than this app was built from (SHA-256 " + result.elfSha256.substr(0, 16) + "...).";
            result.elf.clear();
            return result;
        }

        result.status = DiscCheck::Status::Ok;
        result.message = "OK";
        return result;
    }

}
