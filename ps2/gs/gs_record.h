#pragma once

// GS input trace ("GS dump") format, written by GS::startRecording and read
// by zx-gsreplay. Everything is little endian.
//
// Header:
//   char[8]  magic "PS2XGSR1"
//   u32      vram size, then that many bytes of GS local memory
//   u64[256] last value written to each GS register address
//   u8[256]  1 if that register was ever written
//   u32      CLUT state size, then that many bytes (backend specific)
//   u64[8]   privileged registers: pmode, smode2, dispfb1, display1,
//            dispfb2, display2, bgcolor, vsyncTick
// Events, until end of file:
//   u8 type, u32 payload size, payload
//
// The trace records inputs at the GS class boundary, so replaying it through
// another GS instance reproduces the same draws without the game.
//
// Native draws (gs/gs_native.h) are recorded as such: a mesh once
// per trace (kNativeMesh, before its first draw), then each draw
// (kNativeDraw); the GS packets the draw itself makes (a backend's CPU
// fallback) are not recorded. Payloads (u32 version 2 first; version 1, without the motion, is read too):
//   kNativeMesh: u64 id, u32 source, u32 vertices, u32 skin records,
//                f32 boundsMin[3], boundsMax[3], GSNativeVertex[vertices],
//                GSNativeSkinWeights[skin records]
//   kNativeDraw: u64 mesh id, u32 first, u32 count, f32 transform[16],
//                f32 nearW, f32 normalOffset, s32 alpha, f32 uv[8],
//                f32 clipTransform[16], u8 clip, perspectiveColour,
//                uvTransform, uvFromNormal, dropFlags, clampXY, affineUV,
//                u8 (bit 0 skinned, bit 1 lit rigid, bit 2 motion); if skinned: u8 lit,
//                f32 lightDir[12], lightColour[16], palette[512]; if lit
//                rigid: f32 lightDir[12], lightColour[16], scale, u8 alpha; if motion
//                (version 2): u64 key, f32 world[16], camera[16]

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

class GS;
struct GSRegisters;
struct GSNativeDraw;
struct GSNativeMesh;

namespace GSRecord
{
    constexpr char kMagic[8] = {'P', 'S', '2', 'X', 'G', 'S', 'R', '1'};

    enum EventType : uint8_t
    {
        kGifPacket = 1,              // processGIFPacket(data)
        kNativePackedGifPacket = 2,  // processNativePackedGIFPacket(data)
        kUploadImageNative = 3,      // u64 bitbltbuf, trxpos, trxreg, trxdir; data
        kWriteRegister = 4,          // u8 reg, u64 value
        kClearFramebufferContext = 5,// u32 context, u32 rgba
        kClearActiveFramebuffer = 6, // u32 rgba
        kFrameMarker = 7,            // u64[8] privileged registers (see header)
        kNativeMesh = 8,             // a native mesh (see header)
        kNativeDraw = 9,             // GS::drawNative of a recorded mesh (see header)
        kFogPlanes = 10,             // GS::noteFogPlanes: i32 count, f32 nearest, step, zA, zB
    };

    // Native draw payloads (see the header comment).
    std::vector<uint8_t> encodeNativeMesh(const GSNativeMesh &mesh);
    std::vector<uint8_t> encodeNativeDraw(const GSNativeDraw &draw);
    std::shared_ptr<GSNativeMesh> decodeNativeMesh(const uint8_t *data, uint32_t size);
    // `draw.mesh` is left for the caller (looked up by the returned id).
    bool decodeNativeDraw(const uint8_t *data, uint32_t size, GSNativeDraw &draw, uint64_t &meshId);

    struct Event
    {
        uint8_t type = 0;
        std::vector<uint8_t> payload;
    };

    class Reader
    {
    public:
        ~Reader();
        // Opens the file and restores the recorded state into gs/privRegs.
        // gs must already be initialised with a VRAM buffer of vram size.
        bool open(const std::string &path, GS &gs, uint8_t *vram, uint32_t vramSize, GSRegisters &privRegs);
        bool next(Event &out);

    private:
        std::FILE *m_file = nullptr;
    };

    // Applies a kFrameMarker payload to the privileged register block.
    void applyFrameMarker(const Event &event, GSRegisters &privRegs);
    // Replays a non-marker event into gs.
    void replayEvent(const Event &event, GS &gs);
}
