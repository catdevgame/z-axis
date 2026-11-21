#include "vu1/vu1_record.h"

#include <cstring>
#include <sstream>

namespace ps2vu1rec
{
    static_assert(sizeof(StateImage) == 664u, "StateImage must have no padding");

    namespace
    {
        uint32_t floatBits(float value)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        float bitsFloat(uint32_t bits)
        {
            float value = 0.0f;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        void put(std::vector<uint8_t> &out, const void *data, size_t size)
        {
            const uint8_t *bytes = static_cast<const uint8_t *>(data);
            out.insert(out.end(), bytes, bytes + size);
        }

        template <typename T>
        void putValue(std::vector<uint8_t> &out, const T &value)
        {
            put(out, &value, sizeof(value));
        }

        void putDelta(std::vector<uint8_t> &out, const std::vector<SparseQword> &delta)
        {
            putValue(out, static_cast<uint32_t>(delta.size()));
            for (const SparseQword &q : delta)
            {
                putValue(out, static_cast<uint32_t>(q.index));
                put(out, q.words.data(), 16u);
            }
        }

        std::vector<SparseQword> diffQwords(const uint8_t *before, const uint8_t *after)
        {
            std::vector<SparseQword> delta;
            for (uint32_t index = 0; index < kDataSize / 16u; ++index)
            {
                if (std::memcmp(before + index * 16u, after + index * 16u, 16u) == 0)
                    continue;
                SparseQword q{};
                q.index = static_cast<uint16_t>(index);
                std::memcpy(q.words.data(), after + index * 16u, 16u);
                delta.push_back(q);
            }
            return delta;
        }

        struct Cursor
        {
            const uint8_t *data;
            size_t size;
            size_t pos = 0;
            bool ok = true;

            void read(void *out, size_t count)
            {
                if (!ok || pos + count > size)
                {
                    ok = false;
                    std::memset(out, 0, count);
                    return;
                }
                std::memcpy(out, data + pos, count);
                pos += count;
            }

            template <typename T>
            T value()
            {
                T v{};
                read(&v, sizeof(v));
                return v;
            }

            std::vector<SparseQword> delta()
            {
                const uint32_t count = value<uint32_t>();
                std::vector<SparseQword> result;
                if (count > kDataSize / 16u)
                {
                    ok = false;
                    return result;
                }
                result.resize(count);
                for (SparseQword &q : result)
                {
                    q.index = static_cast<uint16_t>(value<uint32_t>());
                    read(q.words.data(), 16u);
                }
                return result;
            }
        };

        void applyDelta(std::vector<uint8_t> &image, const std::vector<SparseQword> &delta)
        {
            for (const SparseQword &q : delta)
            {
                if (q.index < kDataSize / 16u)
                    std::memcpy(image.data() + q.index * 16u, q.words.data(), 16u);
            }
        }
    }

    StateImage captureState(const VU1State &state)
    {
        StateImage image{};
        for (uint32_t reg = 0; reg < 32u; ++reg)
            for (uint32_t lane = 0; lane < 4u; ++lane)
                image.vf[reg][lane] = floatBits(state.vf[reg][lane]);
        std::memcpy(image.vi, state.vi, sizeof(image.vi));
        for (uint32_t lane = 0; lane < 4u; ++lane)
            image.acc[lane] = floatBits(state.acc[lane]);
        image.q = floatBits(state.q);
        image.p = floatBits(state.p);
        image.i = floatBits(state.i);
        image.r = state.r;
        image.pc = state.pc;
        image.mac = state.mac;
        image.clip = state.clip;
        image.status = state.status;
        image.cycles = state.cycles;
        image.xgkickPackets = state.xgkickPackets;
        image.ebit = state.ebit;
        image.haltAfterDelaySlot = state.haltAfterDelaySlot;
        image.dBitEnabled = state.dBitEnabled;
        image.tBitEnabled = state.tBitEnabled;
        image.stoppedByD = state.stoppedByD;
        image.stoppedByT = state.stoppedByT;
        image.branchPending = state.branchPending;
        image.top = state.top;
        image.itop = state.itop;
        image.branchTarget = state.branchTarget;
        image.branchDelay = state.branchDelay;
        return image;
    }

    void applyState(const StateImage &image, VU1State &state)
    {
        for (uint32_t reg = 0; reg < 32u; ++reg)
            for (uint32_t lane = 0; lane < 4u; ++lane)
                state.vf[reg][lane] = bitsFloat(image.vf[reg][lane]);
        std::memcpy(state.vi, image.vi, sizeof(state.vi));
        for (uint32_t lane = 0; lane < 4u; ++lane)
            state.acc[lane] = bitsFloat(image.acc[lane]);
        state.q = bitsFloat(image.q);
        state.p = bitsFloat(image.p);
        state.i = bitsFloat(image.i);
        state.r = image.r;
        state.pc = image.pc;
        state.mac = image.mac;
        state.clip = image.clip;
        state.status = image.status;
        state.cycles = image.cycles;
        state.xgkickPackets = image.xgkickPackets;
        state.ebit = image.ebit != 0u;
        state.haltAfterDelaySlot = image.haltAfterDelaySlot != 0u;
        state.dBitEnabled = image.dBitEnabled != 0u;
        state.tBitEnabled = image.tBitEnabled != 0u;
        state.stoppedByD = image.stoppedByD != 0u;
        state.stoppedByT = image.stoppedByT != 0u;
        state.branchPending = image.branchPending != 0u;
        state.top = image.top;
        state.itop = image.itop;
        state.branchTarget = image.branchTarget;
        state.branchDelay = image.branchDelay;
    }

    bool compareStates(const StateImage &expected, const StateImage &actual, std::string *diff)
    {
        std::ostringstream out;
        bool same = true;
        auto check = [&](const char *name, uint64_t want, uint64_t got)
        {
            if (want == got)
                return;
            same = false;
            if (diff)
                out << " " << name << " want=0x" << std::hex << want << " got=0x" << got << std::dec;
        };
        static const char lanes[] = "xyzw";
        for (uint32_t reg = 0; reg < 32u; ++reg)
            for (uint32_t lane = 0; lane < 4u; ++lane)
            {
                const std::string name = "vf" + std::to_string(reg) + lanes[lane];
                check(name.c_str(), expected.vf[reg][lane], actual.vf[reg][lane]);
            }
        for (uint32_t reg = 0; reg < 16u; ++reg)
        {
            const std::string name = "vi" + std::to_string(reg);
            check(name.c_str(), static_cast<uint32_t>(expected.vi[reg]), static_cast<uint32_t>(actual.vi[reg]));
        }
        for (uint32_t lane = 0; lane < 4u; ++lane)
        {
            const std::string name = std::string("acc") + lanes[lane];
            check(name.c_str(), expected.acc[lane], actual.acc[lane]);
        }
        check("q", expected.q, actual.q);
        check("p", expected.p, actual.p);
        check("i", expected.i, actual.i);
        check("r", expected.r, actual.r);
        check("pc", expected.pc, actual.pc);
        check("mac", expected.mac, actual.mac);
        check("clip", expected.clip, actual.clip);
        check("status", expected.status, actual.status);
        check("cycles", expected.cycles, actual.cycles);
        check("xgkickPackets", expected.xgkickPackets, actual.xgkickPackets);
        check("ebit", expected.ebit, actual.ebit);
        check("haltAfterDelaySlot", expected.haltAfterDelaySlot, actual.haltAfterDelaySlot);
        check("stoppedByD", expected.stoppedByD, actual.stoppedByD);
        check("stoppedByT", expected.stoppedByT, actual.stoppedByT);
        check("branchPending", expected.branchPending, actual.branchPending);
        check("branchTarget", expected.branchTarget, actual.branchTarget);
        check("branchDelay", expected.branchDelay, actual.branchDelay);
        if (diff)
            *diff += out.str();
        return same;
    }

    bool Recorder::open(const std::string &path)
    {
        close();
        m_file = std::fopen(path.c_str(), "wb");
        if (!m_file)
            return false;
        std::fwrite(kMagic, 1, sizeof(kMagic), m_file);
        std::fwrite(&kVersion, sizeof(kVersion), 1, m_file);
        m_lastCode.clear();
        m_lastDataOut.assign(kDataSize, 0u);
        m_codeId = 0;
        m_sequence = 0;
        return true;
    }

    void Recorder::close()
    {
        if (m_file)
        {
            std::fclose(m_file);
            m_file = nullptr;
        }
    }

    void Recorder::beginRun(uint32_t kind, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles,
                            const uint8_t *code, const uint8_t *data, const VU1State &state, bool idle,
                            uint64_t vsync)
    {
        if (!m_file)
            return;
        if (m_lastCode.size() != kCodeSize || std::memcmp(m_lastCode.data(), code, kCodeSize) != 0)
        {
            m_lastCode.assign(code, code + kCodeSize);
            const uint32_t header[2] = {kRecordCode, kCodeSize};
            std::fwrite(header, sizeof(header), 1, m_file);
            std::fwrite(code, 1, kCodeSize, m_file);
            ++m_codeId;
        }
        m_run = {};
        m_run.sequence = m_sequence++;
        m_run.vsync = vsync;
        m_run.kind = kind;
        m_run.startPC = startPC;
        m_run.top = top;
        m_run.itop = itop;
        m_run.maxCycles = maxCycles;
        m_run.codeId = m_codeId - 1u;
        m_run.startedIdle = idle ? 1u : 0u;
        m_run.stateIn = captureState(state);
        m_run.dataInDelta = diffQwords(m_lastDataOut.data(), data);
        m_dataIn.assign(data, data + kDataSize);
        m_inRun = true;
    }

    void Recorder::addPacket(const uint8_t *data, uint32_t size)
    {
        if (!m_file || !m_inRun)
            return;
        m_run.packets.emplace_back(data, data + size);
    }

    void Recorder::endRun(const uint8_t *data, const VU1State &state)
    {
        if (!m_file || !m_inRun)
            return;
        m_inRun = false;
        m_run.stateOut = captureState(state);
        m_run.dataOutDelta = diffQwords(m_dataIn.data(), data);
        m_lastDataOut.assign(data, data + kDataSize);

        std::vector<uint8_t> payload;
        putValue(payload, m_run.sequence);
        putValue(payload, m_run.vsync);
        putValue(payload, m_run.kind);
        putValue(payload, m_run.startPC);
        putValue(payload, m_run.top);
        putValue(payload, m_run.itop);
        putValue(payload, m_run.maxCycles);
        putValue(payload, m_run.codeId);
        putValue(payload, m_run.startedIdle);
        putValue(payload, m_run.stateIn);
        putValue(payload, m_run.stateOut);
        putDelta(payload, m_run.dataInDelta);
        putDelta(payload, m_run.dataOutDelta);
        putValue(payload, static_cast<uint32_t>(m_run.packets.size()));
        for (const std::vector<uint8_t> &packet : m_run.packets)
        {
            putValue(payload, static_cast<uint32_t>(packet.size()));
            put(payload, packet.data(), packet.size());
        }
        const uint32_t header[2] = {kRecordRun, static_cast<uint32_t>(payload.size())};
        std::fwrite(header, sizeof(header), 1, m_file);
        std::fwrite(payload.data(), 1, payload.size(), m_file);
    }

    bool Reader::open(const std::string &path)
    {
        m_file = std::fopen(path.c_str(), "rb");
        if (!m_file)
            return false;
        char magic[8]{};
        uint32_t version = 0;
        if (std::fread(magic, 1, sizeof(magic), m_file) != sizeof(magic) ||
            std::memcmp(magic, kMagic, sizeof(magic)) != 0 ||
            std::fread(&version, sizeof(version), 1, m_file) != 1 || version != kVersion)
        {
            std::fclose(m_file);
            m_file = nullptr;
            return false;
        }
        return true;
    }

    Reader::~Reader()
    {
        if (m_file)
            std::fclose(m_file);
    }

    bool Reader::next(Vu1RecordedRun &run, std::vector<uint8_t> &code,
                      std::vector<uint8_t> &dataIn, std::vector<uint8_t> &dataOut)
    {
        if (!m_file)
            return false;
        for (;;)
        {
            uint32_t header[2]{};
            if (std::fread(header, sizeof(header), 1, m_file) != 1)
                return false;
            std::vector<uint8_t> payload(header[1]);
            if (header[1] != 0u && std::fread(payload.data(), 1, payload.size(), m_file) != payload.size())
                return false;
            if (header[0] == kRecordCode)
            {
                m_codes.push_back(std::move(payload));
                continue;
            }
            if (header[0] != kRecordRun)
                continue;

            Cursor c{payload.data(), payload.size()};
            run = {};
            run.sequence = c.value<uint64_t>();
            run.vsync = c.value<uint64_t>();
            run.kind = c.value<uint32_t>();
            run.startPC = c.value<uint32_t>();
            run.top = c.value<uint32_t>();
            run.itop = c.value<uint32_t>();
            run.maxCycles = c.value<uint32_t>();
            run.codeId = c.value<uint32_t>();
            run.startedIdle = c.value<uint32_t>();
            run.stateIn = c.value<StateImage>();
            run.stateOut = c.value<StateImage>();
            run.dataInDelta = c.delta();
            run.dataOutDelta = c.delta();
            const uint32_t packetCount = c.value<uint32_t>();
            for (uint32_t index = 0; index < packetCount && c.ok; ++index)
            {
                const uint32_t size = c.value<uint32_t>();
                if (size > payload.size())
                {
                    c.ok = false;
                    break;
                }
                std::vector<uint8_t> packet(size);
                c.read(packet.data(), size);
                run.packets.push_back(std::move(packet));
            }
            if (!c.ok || run.codeId >= m_codes.size())
                return false;

            code = m_codes[run.codeId];
            dataIn = m_lastDataOut;
            applyDelta(dataIn, run.dataInDelta);
            dataOut = dataIn;
            applyDelta(dataOut, run.dataOutDelta);
            m_lastDataOut = dataOut;
            return true;
        }
    }
}
