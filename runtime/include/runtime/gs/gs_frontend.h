#ifndef PS2_GS_FRONTEND_H
#define PS2_GS_FRONTEND_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "runtime/gs/gs_backend.h"

struct GSDebugSnapshot
{
    GSContext ctx[2]{};
    GSPrimReg prim{};
    GSTexaReg texa{};
    GSTexClutReg texclut{};
    uint64_t scanmsk = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t trxdir = 0;
    uint32_t transferX = 0;
    uint32_t transferY = 0;
    uint32_t transferTotalPixels = 0;
    uint32_t transferCopiedPixels = 0;
    uint32_t lastDisplayBaseBytes = 0;
    GSFrameReg preferredDisplaySourceFrame{};
    uint32_t preferredDisplayDestFbp = 0;
    bool hasPreferredDisplaySource = false;
    uint32_t hostPresentationWidth = 0;
    uint32_t hostPresentationHeight = 0;
    uint32_t hostPresentationDisplayFbp = 0;
    uint32_t hostPresentationSourceFbp = 0;
    bool hostPresentationUsedPreferred = false;
    bool hasHostPresentationFrame = false;
    size_t localToHostPendingBytes = 0;
};

enum class GSDebugEventKind : uint8_t
{
    GifTag = 0,
    Register = 1,
    Draw = 2,
    Transfer = 3,
    Present = 4,
};

struct GSDebugHistoryEntry
{
    uint64_t seq = 0;
    uint64_t vsyncTick = 0;
    uint32_t frameIndex = 0;
    GSDebugEventKind kind = GSDebugEventKind::Register;

    uint8_t reg = 0;
    uint64_t regValue = 0;

    uint32_t gifSizeBytes = 0;
    uint32_t gifNloop = 0;
    uint8_t gifFlg = 0;
    uint8_t gifNreg = 0;

    GSPrimReg prim{};
    GSFrameReg frame{};
    GSZbufReg zbuf{};
    GSTex0Reg tex0{};
    GSScissorReg scissor{};
    uint64_t test = 0;
    uint64_t alpha = 0;

    uint32_t vertexCount = 0;
    float xMin = 0.0f;
    float xMax = 0.0f;
    float yMin = 0.0f;
    float yMax = 0.0f;
    double zMin = 0.0;
    double zMax = 0.0;
    uint8_t aMin = 0;
    uint8_t aMax = 0;

    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t trxdir = 0;
    uint32_t transferPixels = 0;

    uint32_t displayFbp = 0;
    uint32_t sourceFbp = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool usedPreferred = false;
};

class GS
{
public:
    GS();
    ~GS() = default;

    void init(uint8_t *vram, uint32_t vramSize, struct GSRegisters *privRegs = nullptr);
    void reset();
    void setRasterBackend(std::unique_ptr<GSRasterBackend> backend);
    // setRasterBackend while running: the CLUT buffer goes over to the new
    // backend. False (backend left with the caller) while an image transfer
    // is under way (unless forced); try again later.
    bool switchRasterBackend(std::unique_ptr<GSRasterBackend> &backend, bool force = false);
    // Lets the backend queue draws until local memory is observed through
    // the GS (see GSRasterBackend::SetDeferred). Only for owners that never
    // read the VRAM buffer passed to init() directly while drawing.
    void setDeferredRasterization(bool deferred);

    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    // Texture identities for the backend (GSRasterBackend::NoteTextureObjects),
    // on the thread that feeds the GS, in stream order (PS2Memory::postGsSideband).
    void noteTextureObjects(const std::vector<GSTextureObjectNote> &notes);
    // The GS worker has no more packets queued (GSRasterBackend::WorkerIdle).
    void workerIdle();
    // A native draw (runtime/gs/gs_native.h) at this point of the stream,
    // on the thread that feeds the GS, with the GS state as it is here. The
    // backend draws it (GSRasterBackend::SubmitNative), else its triangles
    // go to the backend as GS primitives, made as the VU1 program it stands
    // for would make them (the CPU renderer, a draw the GPU one declines).
    void drawNative(const GSNativeDraw &draw);
    // A game's fog planes follow in the stream (GSRasterBackend::NoteFogPlanes).
    void noteFogPlanes(const GSFogPlanes &planes);
    // Developers (PS2X_GS_SHOT_PRIMS, ps2_gs_shots.h): every primitive drawn from here on written to `file`, one
    // fixed-size record each (the draw state and the vertices; marked when a native draw's fallback made it), until
    // set to null. On the thread that feeds the GS (a GS sideband).
    void setPrimitiveLog(std::FILE *file);
    // The current backend draws native draws itself (any thread).
    bool nativeDrawsOnBackend() const { return m_nativeBackend.load(std::memory_order_relaxed); }
    struct NativeDrawStats
    {
        uint64_t draws = 0;          // drawn by the backend
        uint64_t fallbackDraws = 0;  // sent as primitives
        uint64_t fallbackTriangles = 0;
    };
    NativeDrawStats takeNativeDrawStats();
    bool processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    void uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes);
    void writeRegister(uint8_t regAddr, uint64_t value);

    const uint8_t *lockDisplaySnapshot(uint32_t &outSize);
    void unlockDisplaySnapshot();
    uint32_t getLastDisplayBaseBytes() const;
    const GSFrameReg &getContextFrame(int index) const
    {
        return m_ctx[(index != 0) ? 1 : 0].frame;
    }
    GSDebugSnapshot getDebugSnapshot() const;
    std::vector<GSDebugHistoryEntry> getDebugHistory() const;
    void clearDebugHistory();
    bool isDebugHistoryPaused() const;
    void setDebugHistoryPaused(bool paused);
    bool getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const;
    // The window's picture, made now from what the display registers show
    // (a single-buffered picture, or with the frame queue off).
    void latchHostPresentationFrame();

    // The frame queue: every game frame's picture, made on the GS worker at
    // its buffer swap (PS2Memory's display change sink), waits until the
    // window shows it, so a host frame that took two refreshes loses none.
    // At most two wait; a third pushes the oldest out (a frame never shown).
    // Off: the window latches the picture itself, as without the queue.
    void setFrameQueue(bool on);
    bool frameQueue() const { return m_frameQueue.load(std::memory_order_relaxed); }
    // GS worker (or the EE without it): the display buffer changed, a game
    // frame is done (counted with the queue off too).
    void capturePresentationFrame();
    struct TakenFrame
    {
        uint64_t frame = 0;  // the game frame's number (framesMade when it was made)
        int64_t madeNs = 0;  // when its picture was made, steady clock ns
        uint32_t waiting = 0; // frames still waiting after it
        uint64_t vsync = 0;  // the guest's vblank tick it was made at
    };
    // Window thread: the oldest waiting frame, not taken (false: none waits).
    bool peekPresentationFrame(TakenFrame &out) const;
    // Window thread: the latched picture's frame (frame 0: none, or latched without the queue).
    TakenFrame shownPresentationFrame() const;
    // Window thread, frame interpolation (runtime/gs/gs_interpolate.h): the picture `t` of the way from the
    // queue's frame `from` to `to`, on the GPU (GSRasterBackend::PresentBetween).
    ps2x::gs_interpolate::Skip presentBetween(uint64_t from, uint64_t to, float t, GSGpuPicture &out);
    // Its counts since the last call, a log line (empty: no backend that interpolates).
    std::string interpolationReport();
    // Window thread: the oldest waiting frame becomes the latched picture.
    // False with none waiting (the latched picture stays).
    bool takePresentationFrame(TakenFrame &out);
    // Window thread: waits until a frame waits, at most `timeout`.
    bool waitForPresentationFrame(std::chrono::microseconds timeout);
    // Game frames made (display buffer changes), and pushed out unshown.
    uint64_t framesMade() const { return m_framesMade.load(std::memory_order_relaxed); }
    uint64_t framesDropped() const { return m_framesDropped.load(std::memory_order_relaxed); }

    // Latches taken again because the display switched while copying.
    uint64_t presentationRetries() const { return m_presentationRetries.load(std::memory_order_relaxed); }
    // The display's change count (PS2Memory::displayChanges: every buffer
    // swap, A, B, A included), read around a picture's copy: one made while
    // the game swapped is taken again. Unset, the display registers' values
    // alone are compared.
    void setDisplayChangeCount(std::function<uint64_t()> count) { m_displayChangeCount = std::move(count); }
    // Pictures kept though the display changed while they were copied, and
    // whether the one shown is such a picture (PS2X_PRESENT_LOG).
    uint64_t presentationsDisturbed() const { return m_presentationsDisturbed.load(std::memory_order_relaxed); }
    bool shownPictureDisturbed() const;
    bool copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp = nullptr,
                                          uint32_t *outSourceFbp = nullptr,
                                          bool *outUsedPreferred = nullptr,
                                          bool *outOnGpu = nullptr) const;
    // The latched frame stayed on the host's GPU device (no pixels to
    // copy): its texture and the frame's size in GS pixels.
    bool latchedHostPresentationGpuPicture(GSGpuPicture &outPicture, uint32_t &outWidth, uint32_t &outHeight) const;
    // That picture read back (screenshots), at the internal resolution.
    bool readLatchedHostPresentationGpuPicture(std::vector<uint8_t> &outPixels, uint32_t &outWidth, uint32_t &outHeight);
    bool clearFramebufferContext(uint32_t contextIndex, uint32_t rgba);
    bool clearActiveFramebuffer(uint32_t rgba);
    uint64_t nativeImageUploadCount() const { return m_nativeImageUploadCount; }
    uint64_t nativePackedGIFPacketCount() const { return m_nativePackedGIFPacketCount; }

    uint32_t consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes);

    void refreshDisplaySnapshot();

    void WriteVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
    uint32_t ReadVram(uint32_t psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y) const;

    // GS input trace recording (gs_record.cpp, format in gs_record.h).
    // Records VRAM + register state, then every GS input call, with a frame
    // marker at each host presentation latch, for vsyncCount latches.
    bool startRecording(const std::string &path, uint32_t vsyncCount);
    void stopRecording();
    bool isRecording() const { return m_recordFile != nullptr; }
    GSRasterBackend *rasterBackend() { return m_backend.get(); }

    // Pictures at a vsync, for comparisons of two runs (PS2X_GS_SHOT_VSYNC, ps2_gs_shots.cpp): the display as the GS
    // stream has it (displayNow, read in a GS sideband on the worker, behind the swaps queued before), then the picture
    // of it once the GS has drawn everything queued before (pictureOfDisplay, in the same sideband), its rows packed
    // to its width; made in a slot the window's pictures are not in, a GPU renderer's read back from it.
    GSPresentationRequest displayNow() const;
    PresentationFrame pictureOfDisplay(const GSPresentationRequest &display);

private:
    void recordEventUnlocked(uint8_t type, const void *a, uint32_t aBytes, const void *b = nullptr, uint32_t bBytes = 0u);
    void recordFrameMarkerUnlocked();
    void snapshotVRAM();
    void writeRegisterUnlocked(uint8_t regAddr, uint64_t value);
    void writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi);
    void uploadImageNativeUnlocked(uint64_t bitbltbuf,
                                   uint64_t trxpos,
                                   uint64_t trxreg,
                                   uint64_t trxdir,
                                   const uint8_t *data,
                                   uint32_t sizeBytes);
    void vertexKick(bool drawing);

    void recordDebugEventUnlocked(GSDebugHistoryEntry entry);
    GSDebugHistoryEntry makeDebugEventUnlocked(GSDebugEventKind kind) const;
    void recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg);
    void recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value);
    void recordDrawDebugEventUnlocked(int vertexCount);
    void recordTransferDebugEventUnlocked();
    void recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred);

    void processImageData(const uint8_t *data, uint32_t sizeBytes);
    bool tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes);
    GSDrawState drawState() const;
    GSPrimitiveBatch buildDrawBatch(int vertexCount) const;
    // vertexKick's primitive, built in place: the kick's vertices, and the
    // state copied in only when the cache below was rebuilt.
    const GSPrimitiveBatch &kickBatch(int vertexCount);
    const GSDrawState &cachedDrawState() const;
    // buildDrawBatch's cache of the state: stale after any non-vertex register write.
    mutable GSDrawState m_drawState{};
    mutable bool m_drawStateDirty = true;
    mutable uint64_t m_drawStateBuilds = 0; // times m_drawState was built
    GSPrimitiveBatch m_kickBatch{};
    uint64_t m_kickBatchState = ~0ull; // the build of m_drawState in m_kickBatch
    mutable uint64_t m_drawStateChecks = 0, m_drawStateMismatches = 0;
    void drawNativeTriangles(const GSNativeDraw &draw, const GSPrimitiveBatch &base);
    std::atomic<bool> m_nativeBackend{false};
    NativeDrawStats m_nativeStats;
    void updatePreferredDisplaySourceForDraw(const GSPrimitiveBatch &batch);
    GSPresentationRequest buildPresentationRequestUnlocked() const;


    GSContext &activeContext();

    uint8_t *m_localMemoryStorage = nullptr;
    uint32_t m_localMemorySize = 0u;
    struct GSRegisters *m_privRegs = nullptr;
    mutable std::recursive_mutex m_stateMutex;
    // Threads other than the GS worker blocked on m_stateMutex (the window
    // thread's presentation latch): the worker lets them in before its next
    // GIF packet instead of taking the lock straight back (see lockForHost).
    mutable std::atomic<int> m_stateWaiters{0};
    std::unique_lock<std::recursive_mutex> lockForHost() const;
    void letHostIn() const;
    mutable std::mutex m_backendLifetimeMutex;
    std::atomic<uint64_t> m_presentationRetries{0};
    mutable std::mutex m_presentationMutex;

    GSContext m_ctx[2];
    GSPrimReg m_prim{};
    GSPrimReg m_primRegister{};
    GSPrimReg m_prmodeRegister{};

    uint8_t m_curR = 0x80, m_curG = 0x80, m_curB = 0x80, m_curA = 0x80;
    float m_curQ = 1.0f;
    float m_curS = 0.0f, m_curT = 0.0f;
    uint16_t m_curU = 0, m_curV = 0;
    uint8_t m_curFog = 0;
    uint8_t m_fogR = 0, m_fogG = 0, m_fogB = 0;

    bool m_prmodecont = true;
    bool m_pabe = false;
    uint64_t m_scanmsk = 0;
    uint64_t m_dimx = 0;
    uint64_t m_dthe = 0;
    uint64_t m_colclamp = 0;
    GSTexaReg m_texa{0u, false, 0u};
    GSTexClutReg m_texclut{0u, 0u, 0u};

    GSBitBltBuf m_bitbltbuf{};
    GSTrxPos m_trxpos{};
    GSTrxReg m_trxreg{};
    uint32_t m_trxdir = 3;


    static constexpr int kMaxVerts = 6;
    GSVertex m_vtxQueue[kMaxVerts];
    int m_vtxCount = 0;
    int m_vtxIndex = 0;

    std::vector<uint8_t> m_displaySnapshot;
    std::mutex m_snapshotMutex;
    uint32_t m_lastDisplayBaseBytes = 0;
    GSFrameReg m_preferredDisplaySourceFrame{};
    uint32_t m_preferredDisplayDestFbp = 0;
    bool m_hasPreferredDisplaySource = false;
    // The pictures for the window (kPresentationSlots, m_presentationMutex):
    // the one latched (shown), the game frames waiting, one being made.
    struct PresentationPicture
    {
        std::vector<uint8_t> pixels; // kHostFrameWidth wide, RGBA8 (none on the GPU)
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t displayFbp = 0;
        uint32_t sourceFbp = 0;
        bool usedPreferred = false;
        bool onGpu = false;
        GSGpuPicture gpu{}; // onGpu: the backend's texture for the slot, asked when made
        bool has = false;
        bool disturbed = false; // the display changed while it was copied
        uint64_t frame = 0;
        int64_t madeNs = 0;
        uint64_t vsync = 0; // the guest's vblank tick it was made at
    };
    static constexpr uint32_t kMaxWaitingFrames = 2;
    static constexpr uint32_t kNoSlot = ~0u;
    std::array<PresentationPicture, kPresentationSlots> m_pictures{};
    uint32_t m_shownSlot = 0;
    std::array<uint32_t, kMaxWaitingFrames + 1> m_waiting{}; // oldest first
    uint32_t m_waitingCount = 0;
    uint32_t m_makingSlot = kNoSlot;
    std::mutex m_makeMutex; // one picture made at a time
    std::condition_variable m_frameWaiting;
    std::atomic<bool> m_frameQueue{false};
    std::atomic<uint64_t> m_framesMade{0};
    std::atomic<uint64_t> m_framesDropped{0};
    std::function<uint64_t()> m_displayChangeCount;
    std::atomic<uint64_t> m_presentationsDisturbed{0};
    uint32_t freeSlotLocked() const;
    // Makes the picture the display registers show into `slot` (m_makeMutex held).
    // frame: the frame queue's number for it (GSPresentationRequest::frame), 0 for a latched picture.
    void makePresentationPicture(uint32_t slot, PresentationPicture &out, uint64_t frame = 0);
    void clearPresentationPicturesLocked(bool gpuOnly);
    uint64_t m_nativeImageUploadCount = 0;
    uint64_t m_nativePackedGIFPacketCount = 0;

    static constexpr size_t kDebugHistoryCapacity = 512;
    std::array<GSDebugHistoryEntry, kDebugHistoryCapacity> m_debugHistory{};
    size_t m_debugHistoryWrite = 0;
    size_t m_debugHistoryCount = 0;
    uint64_t m_debugNextSeq = 1;
    uint32_t m_debugFrameIndex = 0;
    uint64_t m_debugLastVsyncTick = UINT64_MAX;
    bool m_debugHistoryPaused = true;

    std::unique_ptr<GSRasterBackend> m_backend;
    bool m_deferredRasterization = false;

    std::FILE *m_recordFile = nullptr;
    uint32_t m_recordVsyncsLeft = 0u;
    std::FILE *m_primitiveLog = nullptr;
    uint32_t m_nativeFallbackSource = 0u; // the mesh's source while a native draw's fallback draws (0: none)
    void logPrimitive(const GSPrimitiveBatch &batch);
    std::unordered_set<uint64_t> m_recordedNativeMeshes; // GSNativeMesh::id written to this trace
    std::array<uint64_t, 256> m_shadowRegs{};
    std::array<uint8_t, 256> m_shadowValid{};
};

#endif
