#ifndef PS2_VU1_RECORD_H
#define PS2_VU1_RECORD_H

// VU1 run corpus: every microprogram run (MSCAL / MSCNT) with its complete
// inputs and outputs, so runs can be replayed offline and compared bit for bit
// (zx-vu1check, docs/VU1_CORPUS.md).
//
// File layout: "PS2XVU1R" magic, u32 version, then records of
// {u32 type, u32 payload size, payload}:
//   kRecordCode  16 KiB micro memory (written whenever it changed since the
//                previous run)
//   kRecordRun   one run, see Vu1RecordedRun
// Data memory is stored as sparse qword lists: data-in as the qwords that
// differ from the previous run's data-out, data-out as the qwords that differ
// from data-in.

#include "vu1/vu1_interpreter.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ps2vu1rec
{
    constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'V', 'U', '1', 'R'};
    constexpr uint32_t kVersion = 1u;
    constexpr uint32_t kRecordCode = 1u;
    constexpr uint32_t kRecordRun = 2u;
    constexpr uint32_t kCodeSize = 0x4000u;
    constexpr uint32_t kDataSize = 0x4000u;

    // Architectural state that carries between runs (no pipeline state:
    // runs that end flush their pipelines, see VU1Interpreter::run).
    struct StateImage
    {
        uint32_t vf[32][4];
        int32_t vi[16];
        uint32_t acc[4];
        uint32_t q, p, i, r, pc, mac, clip, status;
        uint64_t cycles;
        uint64_t xgkickPackets;
        uint8_t ebit, haltAfterDelaySlot, dBitEnabled, tBitEnabled;
        uint8_t stoppedByD, stoppedByT, branchPending, pad0;
        uint32_t top, itop, branchTarget, branchDelay;
    };

    StateImage captureState(const VU1State &state);
    void applyState(const StateImage &image, VU1State &state);
    // Field-by-field comparison; appends a description of each difference.
    bool compareStates(const StateImage &expected, const StateImage &actual, std::string *diff);

    struct SparseQword
    {
        uint16_t index;
        std::array<uint32_t, 4> words;
    };

    struct Vu1RecordedRun
    {
        uint64_t sequence = 0;
        uint64_t vsync = 0;
        uint32_t kind = 0; // 0 MSCAL, 1 MSCNT
        uint32_t startPC = 0;
        uint32_t top = 0;
        uint32_t itop = 0;
        uint32_t maxCycles = 0;
        uint32_t codeId = 0;      // index of the code record in use
        uint32_t startedIdle = 1; // interpreter had no pending pipeline work
        StateImage stateIn{};
        StateImage stateOut{};
        std::vector<SparseQword> dataInDelta;  // vs previous run's data-out
        std::vector<SparseQword> dataOutDelta; // vs this run's data-in
        std::vector<std::vector<uint8_t>> packets; // PATH1 packets, in order
    };

    // Recorder used by the runtime (ZX_VU1_RECORD=<file>).
    class Recorder
    {
    public:
        bool open(const std::string &path);
        bool isOpen() const { return m_file != nullptr; }
        void close();

        // Call before and after VU1Interpreter::execute/resume.
        void beginRun(uint32_t kind, uint32_t startPC, uint32_t top, uint32_t itop, uint32_t maxCycles,
                      const uint8_t *code, const uint8_t *data, const VU1State &state, bool idle, uint64_t vsync);
        void addPacket(const uint8_t *data, uint32_t size);
        void endRun(const uint8_t *data, const VU1State &state);

        uint64_t runCount() const { return m_sequence; }

    private:
        FILE *m_file = nullptr;
        std::vector<uint8_t> m_lastCode;
        std::vector<uint8_t> m_lastDataOut;
        std::vector<uint8_t> m_dataIn;
        uint32_t m_codeId = 0;
        uint64_t m_sequence = 0;
        Vu1RecordedRun m_run;
        bool m_inRun = false;
    };

    // Reader used by the checker.
    class Reader
    {
    public:
        bool open(const std::string &path);
        // Returns false at the end of the file. code/dataIn/dataOut are full
        // 16 KiB images reconstructed from the deltas.
        bool next(Vu1RecordedRun &run, std::vector<uint8_t> &code,
                  std::vector<uint8_t> &dataIn, std::vector<uint8_t> &dataOut);
        ~Reader();

    private:
        FILE *m_file = nullptr;
        std::vector<std::vector<uint8_t>> m_codes;
        std::vector<uint8_t> m_lastDataOut = std::vector<uint8_t>(kDataSize, 0u);
    };
}

#endif
