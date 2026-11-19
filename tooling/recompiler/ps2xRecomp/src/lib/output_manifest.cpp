#include "ps2recomp/output_manifest.h"
#include "ps2recomp_source_id.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ps2recomp
{
    namespace
    {
        // SHA-256 (FIPS 180-4), streamed.
        class Sha256
        {
        public:
            void update(const unsigned char *data, size_t size)
            {
                m_length += size;
                while (size > 0)
                {
                    const size_t take = std::min(size, sizeof(m_block) - m_used);
                    std::copy(data, data + take, m_block + m_used);
                    m_used += take;
                    data += take;
                    size -= take;
                    if (m_used == sizeof(m_block))
                    {
                        compress(m_block);
                        m_used = 0;
                    }
                }
            }

            std::string hex()
            {
                const uint64_t bits = m_length * 8;
                const unsigned char one = 0x80;
                const unsigned char zero = 0;
                update(&one, 1);
                while (m_used != 56)
                    update(&zero, 1);
                unsigned char length[8];
                for (int i = 0; i < 8; ++i)
                    length[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
                update(length, 8);
                static const char digits[] = "0123456789abcdef";
                std::string out;
                for (uint32_t word : m_state)
                    for (int shift = 28; shift >= 0; shift -= 4)
                        out += digits[(word >> shift) & 0xF];
                return out;
            }

        private:
            static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

            void compress(const unsigned char *block)
            {
                static const uint32_t k[64] = {
                    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
                uint32_t w[64];
                for (int i = 0; i < 16; ++i)
                    w[i] = (uint32_t(block[4 * i]) << 24) | (uint32_t(block[4 * i + 1]) << 16) |
                           (uint32_t(block[4 * i + 2]) << 8) | uint32_t(block[4 * i + 3]);
                for (int i = 16; i < 64; ++i)
                {
                    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
                uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
                for (int i = 0; i < 64; ++i)
                {
                    const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
                    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
                    h = g;
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
                m_state[7] += h;
            }

            std::array<uint32_t, 8> m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
            unsigned char m_block[64]{};
            size_t m_used = 0;
            uint64_t m_length = 0;
        };

        std::string hashLine(const char *what, const fs::path &path)
        {
            if (path.empty())
                return std::string(what) + " none\n";
            const std::string hash = sha256FileHex(path);
            return std::string(what) + " " + (hash.empty() ? "unreadable" : hash) + " " + path.filename().string() + "\n";
        }
    }

    std::string sha256Hex(const std::string &bytes)
    {
        Sha256 sha;
        sha.update(reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
        return sha.hex();
    }

    std::string sha256FileHex(const fs::path &path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return "";
        Sha256 sha;
        char buffer[1 << 16];
        while (file)
        {
            file.read(buffer, sizeof(buffer));
            sha.update(reinterpret_cast<const unsigned char *>(buffer), static_cast<size_t>(file.gcount()));
        }
        return file.bad() ? "" : sha.hex();
    }

    std::string recompilerIdentity()
    {
        return std::string(PS2RECOMP_SOURCE_ID) + " " + PS2RECOMP_BUILT_WITH;
    }

    std::string formatOutputManifest(const std::string &recompiler, const OutputManifestInputs &inputs,
                                     std::vector<std::pair<std::string, std::uintmax_t>> files)
    {
        std::sort(files.begin(), files.end());
        files.erase(std::unique(files.begin(), files.end()), files.end());
        std::ostringstream out;
        out << "ps2recomp manifest 1\n";
        out << "recompiler " << recompiler << "\n";
        out << hashLine("config", inputs.configPath);
        out << hashLine("elf", inputs.elfPath);
        out << hashLine("ghidra-map", inputs.ghidraMapPath);
        out << "files " << files.size() << "\n";
        for (const auto &[name, size] : files)
            out << size << " " << name << "\n";
        return out.str();
    }

    bool writeOutputManifest(const fs::path &outputDir, const OutputManifestInputs &inputs,
                             const std::vector<fs::path> &written, std::string &error)
    {
        std::vector<std::pair<std::string, std::uintmax_t>> files;
        files.reserve(written.size());
        for (const fs::path &path : written)
        {
            std::error_code ec;
            const std::uintmax_t size = fs::file_size(path, ec);
            if (ec)
            {
                error = "no size for " + path.string() + ": " + ec.message();
                return false;
            }
            files.emplace_back(path.lexically_relative(outputDir).generic_string(), size);
        }
        const std::string text = formatOutputManifest(recompilerIdentity(), inputs, std::move(files));
        // (binary: the same bytes on every host, LF line ends)
        const fs::path manifest = outputDir / kOutputManifestName;
        std::ofstream file(manifest, std::ios::binary | std::ios::trunc);
        file << text;
        file.close();
        if (!file)
        {
            error = "could not write " + manifest.string();
            return false;
        }
        return true;
    }
}
