#include "runtime/state_probe.h"
#include "runtime/spot_fork.h"
#include "ee_instructions.h"
#include "machine.h"
#include "runtime/boot_config.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#if defined(ZX_FPU_TRAP)
#include <dlfcn.h>
#endif

namespace
{
    uint32_t readGuestU32(const uint8_t *rdram, uint32_t address)
    {
        const uint32_t physical = address & 0x1FFFFFFCu;
        if (!rdram || physical > PS2_RAM_SIZE - sizeof(uint32_t))
            return 0u;
        uint32_t value = 0u;
        std::memcpy(&value, rdram + physical, sizeof(value));
        return value;
    }

    bool parseNumber(const std::string &text, uint64_t &out)
    {
        if (text.empty())
            return false;
        char *end = nullptr;
        out = std::strtoull(text.c_str(), &end, 0);
        return end && *end == '\0';
    }

    std::vector<std::string> split(const std::string &text, char separator)
    {
        std::vector<std::string> parts;
        size_t start = 0u;
        while (start <= text.size())
        {
            size_t end = text.find(separator, start);
            if (end == std::string::npos)
                end = text.size();
            if (end > start)
                parts.push_back(text.substr(start, end - start));
            start = end + 1u;
        }
        return parts;
    }
}

namespace zx::packets
{
    bool g_on = false;

    namespace
    {
        struct Hash
        {
            uint64_t value = 0xCBF29CE484222325ull;
            uint64_t bytes = 0;

            void word(uint64_t w)
            {
                value = ((value << 5) | (value >> 59)) ^ w;
                value *= 0x9E3779B97F4A7C15ull;
            }
            void add(const void *data, size_t size)
            {
                const uint8_t *p = static_cast<const uint8_t *>(data);
                bytes += size;
                for (; size >= 8u; p += 8, size -= 8u)
                {
                    uint64_t w;
                    std::memcpy(&w, p, 8u);
                    word(w);
                }
                if (size)
                {
                    uint64_t w = 0;
                    std::memcpy(&w, p, size);
                    word(w);
                }
            }
        };

        constexpr const char *kStreamNames[kStreams] = {"vif1", "gif", "vif0", "gs"};
        std::mutex g_mutex;
        Hash g_hash[kStreams];
        Hash g_liveVif1; // kVif1 without VU1 0x3F3's w
        std::FILE *g_out = nullptr;
        std::string g_path;
        uint64_t g_tick = 0, g_dumpFrom = 1, g_dumpTo = 0;
        std::FILE *g_dump[kStreams] = {};

        void closeDumps()
        {
            for (std::FILE *&f : g_dump)
            {
                if (f)
                    std::fclose(f);
                f = nullptr;
            }
        }
    }

    void configureFromEnvironment()
    {
        const char *path = std::getenv("ZX_PACKET_LOG");
        if (!path || !path[0])
            return;
        g_path = path;
        g_out = std::fopen(path, "w");
        if (const char *dump = std::getenv("ZX_PACKET_DUMP"); dump && dump[0])
        {
            unsigned long long from = 1, to = 0;
            if (std::sscanf(dump, "%llu:%llu", &from, &to) == 2)
            {
                g_dumpFrom = from;
                g_dumpTo = to;
            }
        }
        g_on = g_out != nullptr;
    }

    // kVif1's hash without the words no microprogram reads, stack residue the
    // original leaves and a readable function's stack can't hold (rows' w that
    // quaternionToMatrix does not write). The UNPACK V4-32 and its data word:
    // camera->forward.w to VU1 0x3F3 (beginViewDraws), and the w of qwords 7 to 10
    // (the view's rows) in the 14 quadwords to VU1 1 before MSCAL 0x24's chain:
    // the program at 0x1FD0 loads them LQ.xyz. tools27, playfix: zx-vu1check
    // --reads, --poke. And a UV-transform material's block (14 quadwords to
    // TOPS: 8 of GS state, the matrix routine 0x581 with its four rows, the
    // end): the w of the last row (data word 51), animateTextures' stack as
    // orientedBoxContact's VU0 w lane left it; the routine at 0x2C08 loads the
    // rows LQI.xy. Only where data word 32, the routine, is 0x581. playfix2.
    // And BMX XXX's block for the program at 0xC0 (5 quadwords to TOPS, then
    // MSCAL 0x18): the w of its fourth quadword (data word 15), the stack's
    // where the original's drawScreenBatch left a register's upper half; no run
    // of that program reads it (zx-vu1check --chain --poke-range 3 3 3 --poke-top
    // --poke-pc 0xC0 over the A/B input pe-session's VU1 corpora: no run's
    // PATH1, cycles or registers change, its x, y and z change PATH1). bmxbot1.
    struct DeadWords
    {
        uint32_t unpack;
        uint32_t firstDataWord;
        uint32_t count;
        uint32_t stride;
        uint32_t checkWord = 0;  // a data word before the dead one that must hold
        uint32_t checkValue = 0; // checkValue (0: none)
        uint32_t then = 0;       // the VIF code right after the UNPACK's data (0: any)
    };
    constexpr uint32_t kUvTransformRoutine = 0x581u;
    constexpr uint32_t kMscalBmxProgramC0 = 0x14000018u; // MSCAL 0x18: the program at 0xC0
    constexpr DeadWords kDeadWords[] = {{0x6C0103F3u, 3u, 1u, 0u},
                                        {0x6C0E0001u, 27u, 4u, 4u},
                                        {0x6C0E0000u, 51u, 1u, 0u, 32u, kUvTransformRoutine},
                                        {0x6C050000u, 15u, 1u, 0u, 0u, 0u, kMscalBmxProgramC0}};

    // ZX_PACKET_DEAD_W=1 (career_ab.py --game dm2): DM2's particle batches, a V4-32 UNPACK to VU1 0x20 and its
    // continuation at 0xA0 after an STCYCL 4, 4, carry what the stack held in each vertex's w, and no microprogram
    // reads it (zx-vu1check --poke over the 756 images holding one such value: registers, flags, cycles, data and
    // PATH1 all the same). Those lanes are left out of the second hash too.
    constexpr uint32_t kStcycl44 = 0x01000404u, kUnpackV432 = 0x6C000000u;
    bool deadParticleLanes()
    {
        static const bool on = std::getenv("ZX_PACKET_DEAD_W") != nullptr;
        return on;
    }

    // S and T of the 2D batches (a V4-32 UNPACK of a PACKED GIF tag with
    // registers ST, RGBAQ, XYZ2) drawn with zgui's default texture, which no
    // coordinate can change: 8x8 at 4 bits (newUiDefaultMaterial), every texel
    // index 0, every palette entry white, clamped; PRIM keeps TME on.
    // drawBoxWidget draws a piece with no picture that way, and what its
    // vertices' S and T hold is partly the stack's (what a saved register left,
    // 0x00821560) and partly its across / 0. The stream's TEX0_1 (an A+D GIF
    // packet in the same UNPACK form) says which texture a batch has; no other
    // texture of those sizes reached a 2D batch on the gate's inputs.
    class UniformTexcoords
    {
    public:
        void word(uint8_t *at, uint32_t value)
        {
            if (m_vertexWords != 0u)
            {
                if (m_uniform && m_inVertex % 12u < 2u)
                    std::memset(at, 0, 4u);
                ++m_inVertex;
                --m_vertexWords;
            }
            else if (m_registerWords != 0u)
            {
                m_register[m_inVertex++ % 4u] = value;
                if (--m_registerWords % 4u == 0u && m_register[3] == 0u)
                {
                    if ((m_register[2] & 0xFFu) == kTex0)
                    {
                        const uint32_t width = (m_register[0] >> 26) & 0xFu;
                        const uint32_t height = ((m_register[0] >> 30) & 3u) | ((m_register[1] & 3u) << 2);
                        m_zguiDefault = ((m_register[0] >> 20) & 0x3Fu) == kPsmT4 && width == 3u && height == 3u;
                        m_oneTexel = width == 0u && height == 0u;
                    }
                    else if ((m_register[2] & 0xFFu) == kClamp1)
                        m_wrapsInside = (m_register[0] & 3u) < 2u && ((m_register[0] >> 2) & 3u) < 2u;
                    // zgui's 8x8 T4, or a 1x1 texture of any format (BMX XXX's 2D batches' default) wrapped by
                    // REPEAT or CLAMP: one texel, every tap of a sample is it whatever the coordinate and the filter.
                    m_uniform = m_zguiDefault || (m_oneTexel && m_wrapsInside);
                }
            }
            else if (m_tagWord != 0u)
            {
                m_tag[4u - m_tagWord--] = value;
                if (m_tagWord == 0u && m_tag[3] == 0u)
                {
                    m_inVertex = 0u;
                    if (m_tag[2] == 0x512u && (m_tag[1] >> 26) == (3u << 2))
                        m_vertexWords = (m_tag[0] & 0x7FFFu) * 12u;
                    else if (m_tag[2] == 0xEu && (m_tag[1] >> 28) == 1u)
                        m_registerWords = (m_tag[0] & 0x7FFFu) * 4u;
                }
            }
            else if ((value >> 24) == 0x6Cu)
                m_tagWord = 4u;
        }

    private:
        static constexpr uint32_t kTex0 = 0x06u, kClamp1 = 0x08u, kPsmT4 = 0x14u;
        uint32_t m_tag[4] = {}, m_register[4] = {};
        uint32_t m_tagWord = 0, m_vertexWords = 0, m_registerWords = 0, m_inVertex = 0;
        bool m_uniform = false;
        bool m_zguiDefault = false, m_oneTexel = false, m_wrapsInside = true; // (CLAMP_1 0 at reset: REPEAT)
    };

    // ZX_PACKET_PASSTHROUGH=<program>[,<program>...] (career_ab.py --game dm2: 0x1C): microprograms that send the GS
    // their TOPS buffer as it is (DM2's image at 0x1C * 8: XTOP vi1, XGKICK vi1, end; vudis.py). The GIF packets they
    // kick are built in VU1 memory by the TOPS-relative V4-32 UNPACKs (after STCYCL 4, 4) since the MSCAL before, and a
    // PACKED qword's bits its register does not read are no output: ST's fourth word, RGBAQ's upper 24 bits of each
    // word, XYZ2's unused bits and the rest of the GS manual's PACKED formats (the masks of zx-vu1check --chain's
    // packedMask, which proves the lanes it is asked about: tested2). Those bits are left out of the live hash: the
    // stream's chunks since the last MSCAL are held, and at an MSCAL of such a program the GIF packets in the image the
    // UNPACKs built are parsed from TOPS + 0 and the bits masked in the held copies; then the chunks are hashed in order,
    // as they came.
    class Passthrough
    {
    public:
        static const std::vector<uint32_t> &programs()
        {
            static const std::vector<uint32_t> list = []()
            {
                std::vector<uint32_t> out;
                if (const char *text = std::getenv("ZX_PACKET_PASSTHROUGH"))
                    for (const char *p = text; *p;)
                    {
                        char *end = nullptr;
                        const unsigned long v = std::strtoul(p, &end, 0);
                        if (end == p)
                            break;
                        out.push_back(static_cast<uint32_t>(v));
                        p = *end == ',' ? end + 1 : end;
                    }
                return out;
            }();
            return list;
        }
        static bool on() { return !programs().empty(); }

        // The held chunks: each hashed as it came once released.
        std::vector<std::vector<uint8_t>> chunks;

        // A stream word of the current chunk (its index in `chunks.back()`): what it starts or fills.
        void word(uint32_t value, size_t at)
        {
            if (m_unpackWords != 0u)
            {
                const uint32_t index = m_unpackDone++;
                --m_unpackWords;
                const uint32_t qword = (m_unpackAddress + index / 4u) & 0x3FFu;
                m_place[qword * 4u + index % 4u] = {static_cast<uint32_t>(chunks.size() - 1u), static_cast<uint32_t>(at)};
                m_has[qword] |= static_cast<uint8_t>(1u << (index % 4u));
                m_previous = value;
                return;
            }
            if (m_previous == 0x01000404u && (value & 0xFF008000u) == 0x6C008000u) // STCYCL 4, 4; UNPACK V4-32 to TOPS + a
            {
                m_unpackAddress = value & 0x3FFu;
                m_unpackWords = ((value >> 16 & 0xFFu) != 0u ? (value >> 16 & 0xFFu) : 256u) * 4u;
                m_unpackDone = 0;
            }
            else if ((value & 0xFF000000u) == 0x14000000u) // MSCAL
            {
                const uint32_t program = value & 0xFFFFu;
                for (const uint32_t p : programs())
                    if (p == program)
                        maskKicked();
                std::fill(std::begin(m_has), std::end(m_has), uint8_t{0});
            }
            m_previous = value;
        }

    private:
        struct Place
        {
            uint32_t chunk = 0, at = 0;
        };
        uint32_t &wordAt(uint32_t qword, uint32_t w) { return *reinterpret_cast<uint32_t *>(chunks[m_place[qword * 4u + w].chunk].data() + m_place[qword * 4u + w].at); }
        bool whole(uint32_t qword) const { return m_has[qword & 0x3FFu] == 0xFu; }

        static void packedMask(uint32_t reg, uint32_t mask[4])
        {
            static const uint32_t kWhole[4] = {~0u, ~0u, ~0u, ~0u};
            static const uint32_t kRgbaq[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
            static const uint32_t kSt[4] = {~0u, ~0u, ~0u, 0u};
            static const uint32_t kUv[4] = {0x3FFFu, 0x3FFFu, 0u, 0u};
            static const uint32_t kXyzf[4] = {0xFFFFu, 0xFFFFu, 0x0FFFFFF0u, 0x8FF0u};
            static const uint32_t kXyz[4] = {0xFFFFu, 0xFFFFu, ~0u, 0x8000u};
            static const uint32_t kFog[4] = {0u, 0u, 0u, 0xFF0u};
            static const uint32_t kAd[4] = {~0u, ~0u, 0xFFu, 0u};
            static const uint32_t kNop[4] = {0u, 0u, 0u, 0u};
            const uint32_t *m = kWhole;
            switch (reg)
            {
            case 0x1: m = kRgbaq; break;
            case 0x2: m = kSt; break;
            case 0x3: m = kUv; break;
            case 0x4: case 0xC: m = kXyzf; break;
            case 0x5: case 0xD: m = kXyz; break;
            case 0xA: m = kFog; break;
            case 0xE: m = kAd; break;
            case 0xF: m = kNop; break;
            default: break;
            }
            std::memcpy(mask, m, sizeof(uint32_t) * 4u);
        }

        // The packets from TOPS + 0, while every qword they take is in the image.
        void maskKicked()
        {
            uint32_t q = 0;
            for (int tags = 0; tags < 1024 && whole(q); ++tags)
            {
                const uint64_t low = static_cast<uint64_t>(wordAt(q, 0)) | static_cast<uint64_t>(wordAt(q, 1)) << 32;
                const uint64_t regs = static_cast<uint64_t>(wordAt(q, 2)) | static_cast<uint64_t>(wordAt(q, 3)) << 32;
                const uint32_t nloop = static_cast<uint32_t>(low & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>(low >> 58) & 3u;
                const uint32_t nreg = (static_cast<uint32_t>(low >> 60) & 15u) != 0u ? static_cast<uint32_t>(low >> 60) & 15u : 16u;
                ++q;
                if (flg != 0u)
                    q += flg == 1u ? (nloop * nreg + 1u) / 2u : nloop;
                else
                    for (uint32_t loop = 0; loop < nloop; ++loop)
                        for (uint32_t r = 0; r < nreg; ++r, ++q)
                        {
                            if (!whole(q))
                                return;
                            uint32_t mask[4];
                            packedMask(static_cast<uint32_t>(regs >> (4u * r)) & 15u, mask);
                            for (uint32_t w = 0; w < 4u; ++w)
                                wordAt(q & 0x3FFu, w) &= mask[w];
                        }
                if (low & 0x8000u) // EOP
                    return;
            }
        }

        Place m_place[0x400 * 4];
        uint8_t m_has[0x400] = {};
        uint32_t m_previous = 0, m_unpackAddress = 0, m_unpackWords = 0, m_unpackDone = 0;
    };

    Passthrough &passthrough()
    {
        static Passthrough p;
        return p;
    }

    // The held chunks hashed in order (at an MSCAL, and at the vsync).
    void releaseHeld()
    {
        Passthrough &p = passthrough();
        for (const std::vector<uint8_t> &chunk : p.chunks)
            g_liveVif1.add(chunk.data(), chunk.size());
        p.chunks.clear();
    }

    // Whether the VIF code right after an UNPACK V4-32's data (num quadwords,
    // 0: 256) at `at` is `then` (0: any). Only within this chunk: a block whose
    // end is in the next one keeps its words.
    bool followedBy(const std::vector<uint8_t> &chunk, size_t at, uint32_t unpack, uint32_t then)
    {
        if (then == 0u)
            return true;
        const uint32_t num = (unpack >> 16 & 0xFFu) != 0u ? (unpack >> 16 & 0xFFu) : 256u;
        const size_t next = at + 4u + num * 16u;
        if (next + 4u > chunk.size())
            return false;
        uint32_t code = 0;
        std::memcpy(&code, chunk.data() + next, 4u);
        return code == then;
    }

    void addLiveVif1(const void *bytes, size_t size)
    {
        static std::vector<uint8_t> copy;
        static uint32_t pending = 0;  // words to the next dead one (a chunk can end before it)
        static uint32_t deadLeft = 0; // that one and those after it
        static uint32_t stride = 0;
        static uint32_t checkLeft = 0, checkValue = 0; // words to the checked one, and what it must hold
        static UniformTexcoords texcoords;
        copy.assign(static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + size);
        const bool deadLanes = deadParticleLanes();
        static uint32_t previous = 0u, laneWords = 0u; // (as `pending`: a chunk can end inside a batch)
        for (size_t at = 0; at + 4u <= size; at += 4u)
        {
            uint32_t word = 0;
            std::memcpy(&word, copy.data() + at, 4u);
            texcoords.word(copy.data() + at, word);
            if (laneWords != 0u)
            {
                if ((--laneWords & 3u) == 0u)
                    std::memset(copy.data() + at, 0, 4u);
                previous = word;
                continue;
            }
            if (deadLanes && previous == kStcycl44 &&
                ((word & 0xFF00FFFFu) == (kUnpackV432 | 0x20u) || (word & 0xFF00FFFFu) == (kUnpackV432 | 0xA0u)))
                laneWords = ((word >> 16 & 0xFFu) != 0u ? (word >> 16 & 0xFFu) : 256u) * 4u;
            previous = word;
            if (pending != 0u)
            {
                if (checkLeft != 0u && --checkLeft == 0u && word != checkValue)
                {
                    pending = 0u; // not the block the rule is for: its words stay
                    deadLeft = 0u;
                    continue;
                }
                if (--pending == 0u)
                {
                    std::memset(copy.data() + at, 0, 4u);
                    if (--deadLeft != 0u)
                        pending = stride;
                }
                continue;
            }
            for (const DeadWords &dead : kDeadWords)
                if ((word & 0xFFFF03FFu) == dead.unpack && followedBy(copy, at, word, dead.then))
                {
                    pending = dead.firstDataWord + 1u;
                    deadLeft = dead.count;
                    stride = dead.stride;
                    checkLeft = dead.checkValue != 0u ? dead.checkWord + 1u : 0u;
                    checkValue = dead.checkValue;
                }
        }
        if (!Passthrough::on())
        {
            g_liveVif1.add(copy.data(), size);
            return;
        }
        // (the passthrough rule: the chunk held until the next MSCAL, its words walked for the UNPACKs and MSCALs)
        Passthrough &p = passthrough();
        p.chunks.push_back(copy);
        for (size_t at = 0; at + 4u <= size; at += 4u)
        {
            uint32_t word = 0;
            std::memcpy(&word, static_cast<const uint8_t *>(bytes) + at, 4u);
            p.word(word, at);
            if ((word & 0xFF000000u) == 0x14000000u && p.chunks.size() > 1u)
            {
                // (an MSCAL: what came before it is decided; this chunk stays, as the next ones may still fill its image)
                std::vector<uint8_t> current = std::move(p.chunks.back());
                p.chunks.pop_back();
                releaseHeld();
                p.chunks.push_back(std::move(current));
            }
        }
    }

    void add(Stream stream, const void *bytes, size_t size)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_hash[stream].add(bytes, size);
        if (stream == kVif1)
            addLiveVif1(bytes, size);
        if (g_tick >= g_dumpFrom && g_tick <= g_dumpTo)
        {
            std::FILE *&file = g_dump[stream];
            if (!file)
                file = std::fopen((g_path + ".t" + std::to_string(g_tick) + "." + kStreamNames[stream]).c_str(), "wb");
            if (file)
                std::fwrite(bytes, 1u, size, file);
        }
    }

    void onVsync(uint64_t tick, const R5900Context *ctx)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        closeDumps();
        releaseHeld();
        Hash vf, rest;
        if (ctx)
        {
            vf.add(ctx->vu0_vf, sizeof(ctx->vu0_vf));
            rest.add(&ctx->vu0_acc, sizeof(ctx->vu0_acc));
            rest.add(&ctx->vu0_q, sizeof(ctx->vu0_q));
            rest.add(ctx->vi, sizeof(ctx->vi));
            rest.add(&ctx->vu0_status, sizeof(ctx->vu0_status));
            rest.add(&ctx->vu0_mac_flags, sizeof(ctx->vu0_mac_flags));
            rest.add(&ctx->vu0_clip_flags, sizeof(ctx->vu0_clip_flags));
        }
        std::fprintf(g_out, "P %llu", static_cast<unsigned long long>(g_tick));
        for (Hash &h : g_hash)
        {
            std::fprintf(g_out, " %llu %016llx", static_cast<unsigned long long>(h.bytes),
                         static_cast<unsigned long long>(h.value));
            h = Hash{};
        }
        std::fprintf(g_out, " %016llx %016llx %016llx\n", static_cast<unsigned long long>(vf.value),
                     static_cast<unsigned long long>(rest.value), static_cast<unsigned long long>(g_liveVif1.value));
        g_liveVif1 = Hash{};
        g_tick = tick;
    }
}

bool StateProbe::parseCondition(const std::string &text, Condition &out)
{
    static const struct
    {
        const char *token;
        StateProbe::Op op;
    } kOps[] = {{"==", Op::Eq}, {"!=", Op::Ne}, {">=", Op::Ge},
                {"<=", Op::Le}, {">", Op::Gt}, {"<", Op::Lt}};
    for (const auto &candidate : kOps)
    {
        const size_t at = text.find(candidate.token);
        if (at == std::string::npos || at == 0u)
            continue;
        const std::string lhs = text.substr(0, at);
        const std::string rhs = text.substr(at + std::strlen(candidate.token));
        Condition condition{};
        condition.op = candidate.op;
        if (!parseNumber(rhs, condition.value))
            return false;
        if (lhs == "tick")
        {
            condition.source = Source::Tick;
        }
        else if (lhs == "script")
        {
            condition.source = Source::Script;
        }
        else
        {
            uint64_t address = 0u;
            if (!parseNumber(lhs, address))
                return false;
            condition.address = static_cast<uint32_t>(address);
        }
        out = condition;
        return true;
    }
    return false;
}

namespace
{
    // The pad script's clock, of the boot that published it (-1 in a boot
    // before its first pad read).
    std::atomic<int64_t> s_scriptTime{-1};
    std::atomic<uint64_t> s_scriptTimeBoot{0};
}

void StateProbe::publishScriptTime(int64_t vsyncs)
{
    s_scriptTime.store(vsyncs, std::memory_order_release);
    s_scriptTimeBoot.store(zx::boot::generation(), std::memory_order_release);
}

int64_t StateProbe::scriptTime()
{
    if (s_scriptTimeBoot.load(std::memory_order_acquire) != zx::boot::generation())
        return -1;
    return s_scriptTime.load(std::memory_order_acquire);
}

bool StateProbe::evaluate(const Condition &condition, const uint8_t *rdram, uint64_t tick)
{
    if (condition.source == Source::Script)
    {
        // Signed: the clock is -1 before the first pad read.
        const int64_t now = scriptTime();
        const int64_t value = static_cast<int64_t>(condition.value);
        switch (condition.op)
        {
        case Op::Eq:
            return now == value;
        case Op::Ne:
            return now != value;
        case Op::Ge:
            return now >= value;
        case Op::Le:
            return now <= value;
        case Op::Gt:
            return now > value;
        case Op::Lt:
            return now < value;
        }
        return false;
    }
    const uint64_t lhs = condition.source == Source::Tick ? tick : readGuestU32(rdram, condition.address);
    switch (condition.op)
    {
    case Op::Eq:
        return lhs == condition.value;
    case Op::Ne:
        return lhs != condition.value;
    case Op::Ge:
        return lhs >= condition.value;
    case Op::Le:
        return lhs <= condition.value;
    case Op::Gt:
        return lhs > condition.value;
    case Op::Lt:
        return lhs < condition.value;
    }
    return false;
}

bool StateProbe::parseDumpSpec(const std::string &spec, std::string *error)
{
    std::vector<DumpTrigger> parsed;
    for (const std::string &entry : split(spec, ','))
    {
        const size_t colon = entry.find(':');
        if (colon == std::string::npos || colon == 0u)
        {
            if (error)
                *error = "missing label in '" + entry + "'";
            return false;
        }
        DumpTrigger trigger;
        trigger.label = entry.substr(0, colon);
        for (const std::string &text : split(entry.substr(colon + 1u), '&'))
        {
            Condition condition{};
            if (!parseCondition(text, condition))
            {
                if (error)
                    *error = "bad condition '" + text + "'";
                return false;
            }
            trigger.conditions.push_back(condition);
        }
        if (trigger.conditions.empty())
        {
            if (error)
                *error = "no condition in '" + entry + "'";
            return false;
        }
        parsed.push_back(std::move(trigger));
    }
    m_triggers = std::move(parsed);
    m_next = 0u;
    return true;
}

bool StateProbe::parseLogSpec(const std::string &spec, std::string *error)
{
    std::vector<uint32_t> addresses;
    for (const std::string &text : split(spec, ','))
    {
        const size_t colon = text.find(':');
        uint64_t address = 0u;
        uint64_t words = 1u;
        if (!parseNumber(text.substr(0, colon), address) ||
            (colon != std::string::npos && (!parseNumber(text.substr(colon + 1u), words) || words == 0u || words > 4096u)))
        {
            if (error)
                *error = "bad address '" + text + "'";
            return false;
        }
        for (uint64_t i = 0; i < words; ++i)
            addresses.push_back(static_cast<uint32_t>(address + 4u * i));
    }
    m_logAddresses = std::move(addresses);
    m_lastLogged.assign(m_logAddresses.size(), 0u);
    m_logPrimed = false;
    return true;
}

void StateProbe::configure(const char *dumpVariable, const char *logVariable)
{
    std::string error;
    if (const char *value = zx::boot::text(dumpVariable); value && value[0])
    {
        if (!parseDumpSpec(value, &error))
            std::fprintf(stderr, "[probe] %s ignored: %s\n", dumpVariable, error.c_str());
    }
    if (const char *value = zx::boot::text(logVariable); value && value[0])
    {
        if (!parseLogSpec(value, &error))
            std::fprintf(stderr, "[probe] %s ignored: %s\n", logVariable, error.c_str());
    }
}

void StateProbe::configureFromEnvironment()
{
    configure("ZX_DUMP_WHEN", "ZX_STATE_LOG");
}

void StateProbe::configurePadReadFromEnvironment()
{
    configure("ZX_PAD_DUMP", "ZX_PAD_LOG");
}

void StateProbe::writeRamDump(const uint8_t *rdram, const std::string &label, uint64_t tick)
{
    const char *prefixEnv = zx::boot::text("ZX_DUMP_PREFIX");
    const std::string path = std::string((prefixEnv && prefixEnv[0]) ? prefixEnv : "zx-dump") + "-" + label + ".ram";
    // The PS2's 32 MB; with ZX_DUMP_EXTENDED=1 all the guest RAM a play recording's checksums cover
    // (PS2_RAM_SIZE: the extended memory above the 32 MB too, where the Zoo's textures and zgui's settings room are).
    const char *extendedEnv = zx::boot::text("ZX_DUMP_EXTENDED");
    const size_t bytes = (extendedEnv && extendedEnv[0] == '1') ? PS2_RAM_SIZE : PS2_LEGACY_RAM_SIZE;
    FILE *file = std::fopen(path.c_str(), "wb");
    const bool ok = file && rdram && std::fwrite(rdram, 1, bytes, file) == bytes;
    if (file)
        std::fclose(file);
    std::fprintf(stderr, "[dump] %s at vsync tick %llu -> %s%s\n", label.c_str(),
                 static_cast<unsigned long long>(tick), path.c_str(), ok ? "" : " FAILED");
}

void StateProbe::writeThreadStacks(const std::string &label, const std::string &text)
{
    const char *prefixEnv = zx::boot::text("ZX_DUMP_PREFIX");
    const std::string path = std::string((prefixEnv && prefixEnv[0]) ? prefixEnv : "zx-dump") + "-" + label + ".sp";
    if (FILE *file = std::fopen(path.c_str(), "w"))
    {
        std::fwrite(text.data(), 1, text.size(), file);
        std::fclose(file);
    }
}

void StateProbe::onVsync(const uint8_t *rdram, uint64_t tick, const DumpWriter &writeDump)
{
    probe(rdram, tick, writeDump, false, "state");
}

void StateProbe::onPadRead(const uint8_t *rdram, uint64_t tick, const DumpWriter &writeDump)
{
    probe(rdram, tick, writeDump, true, "pad-state");
}

void StateProbe::probe(const uint8_t *rdram, uint64_t tick, const DumpWriter &writeDump, bool everyCall, const char *tag)
{
    if (!m_logAddresses.empty())
    {
        bool changed = everyCall || !m_logPrimed;
        for (size_t i = 0; i < m_logAddresses.size(); ++i)
        {
            const uint32_t value = readGuestU32(rdram, m_logAddresses[i]);
            changed = changed || value != m_lastLogged[i];
            m_lastLogged[i] = value;
        }
        m_logPrimed = true;
        if (changed)
        {
            // One write, and a copy in ZX_STATE_LOG_FILE when set: other
            // threads' log text gets spliced into stderr's lines (a line built
            // by pieces always, a whole one when theirs are built by pieces),
            // and only the file's are sure to be whole.
            std::string line = "[" + std::string(tag) + "] tick=" + std::to_string(tick);
            char field[32];
            for (size_t i = 0; i < m_logAddresses.size(); ++i)
            {
                std::snprintf(field, sizeof(field), " %x=%x", m_logAddresses[i], m_lastLogged[i]);
                line += field;
            }
            line += '\n';
            // Each boot its own file (the one before closed).
            using File = std::unique_ptr<std::FILE, int (*)(std::FILE *)>;
            static zx::boot::PerBoot<File> fileOfBoot([]() -> File
            {
                const char *path = zx::boot::text("ZX_STATE_LOG_FILE");
                std::FILE *opened = (path && path[0]) ? std::fopen(path, "w") : nullptr;
                if (opened)
                    std::setvbuf(opened, nullptr, _IOLBF, 1u << 16);
                return File(opened, [](std::FILE *f) { return std::fclose(f); });
            });
            // A child forked at a spot (runtime/spot_fork.h): its own file, by its environment.
            static uint32_t fileEpoch = 0;
            if (fileEpoch != zx::spot::forkEpoch())
            {
                fileEpoch = zx::spot::forkEpoch();
                const char *path = zx::boot::text("ZX_STATE_LOG_FILE");
                std::FILE *opened = (path && path[0]) ? std::fopen(path, "w") : nullptr;
                if (opened)
                    std::setvbuf(opened, nullptr, _IOLBF, 1u << 16);
                fileOfBoot.get() = File(opened, [](std::FILE *f) { return f ? std::fclose(f) : 0; });
            }
            std::FILE *const file = fileOfBoot.get().get();
            std::fwrite(line.data(), 1, line.size(), stderr);
            if (file)
            {
                // Whole lines in the file as they are written, on every
                // platform: MSVC's _IOLBF is full buffering, and a boot's file
                // stays open after its boot until the next one logs.
                std::fwrite(line.data(), 1, line.size(), file);
                std::fflush(file);
            }
        }
    }

    if (m_next >= m_triggers.size())
        return;
    const DumpTrigger &trigger = m_triggers[m_next];
    for (const Condition &condition : trigger.conditions)
    {
        if (!evaluate(condition, rdram, tick))
            return;
    }
    ++m_next;
    if (writeDump)
        writeDump(trigger.label, tick);
}

#if defined(ZX_FPU_TRAP)
// See PS2_FTRAP in ee_instructions.h. ZX_FPU_TRAP_FROM=<pad script vsync> starts reporting there
// (default 0), ZX_FPU_TRAP_LIMIT=<n> stops after n reports (default 200).
__attribute__((noinline)) void zx_fpu_trap_report(const char *op, uint32_t a, uint32_t b, uint32_t r)
{
    static const int64_t from = []()
    {
        const char *value = std::getenv("ZX_FPU_TRAP_FROM");
        return value ? std::strtoll(value, nullptr, 0) : 0;
    }();
    static long remaining = []()
    {
        const char *value = std::getenv("ZX_FPU_TRAP_LIMIT");
        return value ? std::strtol(value, nullptr, 0) : 200;
    }();
    const int64_t now = StateProbe::scriptTime();
    if (now < from || remaining <= 0)
        return;
    --remaining;
    void *where = __builtin_return_address(0);
    Dl_info info{};
    const char *name = dladdr(where, &info) && info.dli_sname ? info.dli_sname : "?";
    const long offset = info.dli_saddr ? static_cast<long>(static_cast<char *>(where) - static_cast<char *>(info.dli_saddr)) : 0;
    std::fprintf(stderr, "[fpu-trap] script=%lld %s %08x %08x -> %08x in %s+0x%lx\n", static_cast<long long>(now), op, a, b, r, name, offset);
}
#endif

#if defined(ZX_FLOAT_CENSUS)
// See PS2_CENSUS_* in ee_instructions.h (devdep1). Each EE FPU / VU0 macro-mode operation is sorted by what
// the R5900 does differently from the host's IEEE arithmetic, so the count says how often each difference bites
// on a real route. Counted on the EE thread without locks (the other threads do no guest float work); printed at
// exit. The classes, per operation:
//   total        every call
//   daz          an operand with exponent 0 and a mantissa (a host denormal): the EE reads it as zero (DAZ)
//   chop         the IEEE round-to-nearest result differs from the round-toward-zero one (RTZ)
//   align        ADD/SUB only: the R5900 adder's operand alignment (ps2_fpu_align_addends) gives a result unlike
//                the IEEE round-toward-zero sum of the same operands (no host rounding mode has this)
//   special      an exponent-255 operand or result, an overflow saturated to +-0x7FFFFFFF, a result flushed to zero,
//                x/0 (the slow path: no host mode has these either)
//   saturate     CVT.W.S of |x| >= 2^31 (the host's conversion is undefined there)
namespace
{
    struct FloatCensusRow
    {
        const char *name;
        uint64_t total = 0, daz = 0, chop = 0, align = 0, special = 0, saturate = 0;
    };
    FloatCensusRow g_census[] = {{"ADD.S"}, {"SUB.S"}, {"MUL.S"}, {"DIV.S"}, {"SQRT.S"}, {"CVT.W.S"}, {"CVT.S.W"},
                                 {"VADD lane"}, {"VSUB lane"}, {"VMUL lane"}};
    void printFloatCensus()
    {
        std::fprintf(stderr, "[float-census] %-10s %14s %12s %12s %12s %12s %12s\n", "op", "total", "daz", "chop(RTZ)",
                     "align", "special", "saturate");
        for (const FloatCensusRow &row : g_census)
            std::fprintf(stderr, "[float-census] %-10s %14llu %12llu %12llu %12llu %12llu %12llu\n", row.name,
                         static_cast<unsigned long long>(row.total), static_cast<unsigned long long>(row.daz),
                         static_cast<unsigned long long>(row.chop), static_cast<unsigned long long>(row.align),
                         static_cast<unsigned long long>(row.special), static_cast<unsigned long long>(row.saturate));
    }
    bool isDenormalBits(uint32_t b) { return (b & PS2_FLOAT_EXP_MASK) == 0u && (b & 0x7FFFFFu) != 0u; }
    bool isExp255(uint32_t b) { return (b & PS2_FLOAT_EXP_MASK) == PS2_FLOAT_EXP_MASK; }
    // An exponent-255 operand, an overflow (Inf or exponent 255 on the host), or a result below the normal range
    // (flushed to zero on the EE). An exact zero result is an ordinary result, not a special case.
    bool isSpecialCase(uint32_t a, uint32_t b, float rn)
    {
        const uint32_t rb = std::bit_cast<uint32_t>(rn);
        return isExp255(a) || isExp255(b) || isExp255(rb) || (!ps2_fnormal(rn) && (rb & 0x7FFFFFFFu) != 0u);
    }
}

void zx_float_census(int kind, uint32_t a, uint32_t b, uint32_t r)
{
    static const bool registered = []
    {
        std::atexit(printFloatCensus);
        // A run stopped at its tick limit gets SIGTERM (run-game.sh): print, then leave.
        std::signal(SIGTERM, [](int) { printFloatCensus(); std::_Exit(0); });
        return true;
    }();
    (void)registered;
    FloatCensusRow &row = g_census[kind];
    ++row.total;
    const float fa = std::bit_cast<float>(a), fb = std::bit_cast<float>(b);
    switch (kind)
    {
    case kCensusAdd:
    case kCensusSub:
    {
        if (isDenormalBits(a) || isDenormalBits(b))
            ++row.daz;
        const float da = ps2_daz(fa), db = kind == kCensusAdd ? ps2_daz(fb) : -ps2_daz(fb);
        const float rn = da + db; // IEEE round to nearest, full precision
        if (isSpecialCase(a, b, rn))
        {
            ++row.special;
            break;
        }
        const float rtz = ps2_chop_sum(da, db, rn); // IEEE round toward zero of the same operands
        if (std::bit_cast<uint32_t>(rtz) != std::bit_cast<uint32_t>(rn))
            ++row.chop;
        if (std::bit_cast<uint32_t>(rtz) != r)
            ++row.align;
        break;
    }
    case kCensusMul:
    {
        if (isDenormalBits(a) || isDenormalBits(b))
            ++row.daz;
        const float da = ps2_daz(fa), db = ps2_daz(fb);
        const float rn = da * db;
        if (isSpecialCase(a, b, rn))
        {
            ++row.special;
            break;
        }
        if (std::bit_cast<uint32_t>(rn) != r)
            ++row.chop;
        break;
    }
    case kCensusDiv:
    case kCensusSqrt:
    {
        if (isDenormalBits(a) || isDenormalBits(b))
            ++row.daz;
        const float rn = kind == kCensusDiv ? fa / fb : std::sqrt(fa);
        if (std::bit_cast<uint32_t>(rn) != r)
            ++row.special; // x/0, an exponent-255 operand, sqrt of a negative, a flushed result
        break;
    }
    case kCensusCvtWS:
        if ((a & PS2_FLOAT_EXP_MASK) > 0x4E800000u)
            ++row.saturate;
        if (isDenormalBits(a))
            ++row.daz;
        break;
    case kCensusCvtSW:
        if (std::bit_cast<uint32_t>(static_cast<float>(static_cast<int32_t>(a))) != r)
            ++row.chop;
        break;
    default: // the VU0 lanes: the host result against the lane's
    {
        if (isDenormalBits(a) || isDenormalBits(b))
            ++row.daz;
        const float da = ps2_daz(fa), db = ps2_daz(fb);
        const float rn = kind == kCensusVecAdd ? da + db : kind == kCensusVecSub ? da - db : da * db;
        if (isSpecialCase(a, b, rn))
            ++row.special;
        else if (std::bit_cast<uint32_t>(rn) != r)
            ++row.chop;
        break;
    }
    }
}
#endif
