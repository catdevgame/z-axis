// Dave Mirra Freestyle BMX 2 (SLUS-20159): the view. Widescreen 3D (settings.ini
// [display] widescreen / aspect, PS2X_WIDESCREEN / PS2X_ASPECT), the same
// two changes as Aggressive Inline's widescreen (aggressive_inline_slus20327.cpp)
// on ZGE2's camera layout:
//  - The projection FUN_00101570 (a0 = camera) builds the matrix from the
//    camera's horizontal scale camera+0x44 (1.0, set by FUN_0011DFD0 from
//    0x2073C0 when the display is set up) times its focal length +0x448.
//    The hook sets +0x44 to 4:3 / aspect (0.75 for 16:9) before the
//    projection runs, so the scene is drawn narrower for a picture the host
//    stretches to the wide aspect (markWidescreenFrame), and back to 1.0
//    when widescreen is turned off.
//  - The view frustum (updateFrustum, readable: src/game/zge/scene/camera.cpp)
//    stores the tangent of the horizontal half angle at +0x430 (= camera+0x18
//    * 0.5 / focal +0x448) and its inverse at +0x438, then looks up its first
//    sine (angleSin). The frustum is wrapped (its camera, and that the next
//    sine is its first) and the hook on that sine rewrites both with the half
//    width divided by the scale, so the planes are as much wider and objects
//    at the sides are not culled. (Never by the call's return address: the
//    readable frustum's calls carry none.)
// Draw distance (settings.ini [gameplay] draw_distance, PS2X_DRAW_DISTANCE):
// a camera's far clip is camera+0x34 (FUN_0011DE68 sets 200 at creation; a
// level sets its own, Woodward Camp 187). The projection maps the 16-bit Z
// from near (+0x30) to it and the frustum's far plane is built from it, both
// right after this hook. Far multiplies it by 1.5, Farthest by 2.5 (the Z
// buffer is PSMZ16S, so depth steps grow with the square of the range); the
// game's own value is kept and put back at Original. Per camera, every
// frame; the setting comes at boot (the far clip is the game's state, so
// DM2's draw distance is Original by default: GameSettingsProfile::
// drawDistanceChangesGame).
// Only in a level (game mode 0x259770 = 2 with the level frame counter
// 0x25BCC4 running): the front end and loading screens stay 4:3. With
// widescreen never turned on nothing is written.
//
// 2D in a widescreen level (HUD, pause menu, message boxes): every screen-
// space primitive is one VIF1 UNPACK V4-32 to VU1 address 8 in the packet at
// 0x25CC68 (FUN_00104430 and its siblings 0x1046F8 ... 0x1056C8, as AI's
// 0x27A848): a PACKED GIF tag and per vertex ST, RGBAQ and XYZ2 with x, y as
// floats in screen pixels (0..the width at 0x1FA61C). The host stretches the
// wide frame, so each finished 2D primitive (sceVif1PkCloseUpkCode
// 0x1D88F0) has its x squeezed about the screen centre by the scale: the 2D
// keeps its 4:3 shape, centred. Primitives across the whole width (fades,
// backgrounds) stay as they are.
#include "dave_mirra.h"
#include "runtime/play_recording.h"
#include "zge_sites.h"
#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

namespace dave_mirra::view
{
    constexpr uint32_t kProjection = 0x00101570u;
    constexpr uint32_t kSin = 0x00127870u;
    constexpr uint32_t kFrustum = 0x00101068u; // updateFrustum
    constexpr uint32_t kCameraWidth = 0x18;
    constexpr uint32_t kCameraXScale = 0x44;
    constexpr uint32_t kCameraFar = 0x34;
    constexpr float kDrawDistanceFactor[] = {1.0f, 1.5f, 2.5f};
    constexpr uint32_t kCameraTanX = 0x430;
    constexpr uint32_t kCameraInvTanX = 0x438;
    constexpr uint32_t kCameraFocal = 0x448;
    constexpr uint32_t kGameMode = 0x00259770u;
    constexpr uint32_t kLevelFrame = 0x0025BCC4u;
    constexpr uint32_t kModeLevel = 2u;
    constexpr int kRegA0 = 4, kRegRa = 31;
    constexpr uint32_t kCloseUnpack = 0x001D88F0u;
    constexpr uint32_t kPacket2D = 0x0025CC68u;
    constexpr uint32_t kScreenWidth = 0x001FA61Cu; // float
    constexpr uint32_t kPacketCurrent = 0x00, kPacketVifCode = 0x0C;
    constexpr uint32_t kVu1PrimAddress = 8u;
    constexpr float kFullWidthMargin = 4.0f;

    bool readF32(uint8_t *rdram, uint32_t address, float &out)
    {
        const uint8_t *p = getConstMemPtr(rdram, address);
        if (!p)
            return false;
        std::memcpy(&out, p, sizeof(out));
        return true;
    }

    void writeF32(uint8_t *rdram, uint32_t address, float value)
    {
        if (uint8_t *p = getMemPtr(rdram, address))
            std::memcpy(p, &value, sizeof(value));
    }

    uint32_t readU32(uint8_t *rdram, uint32_t address)
    {
        uint32_t value = 0;
        if (const uint8_t *p = getConstMemPtr(rdram, address))
            std::memcpy(&value, p, sizeof(value));
        return value;
    }

    // Per camera: the far clip the game set and the one written here.
    struct CameraFar
    {
        uint32_t camera = 0;
        float game = 0.0f;
        float written = 0.0f;
    };

    // One boot's originals, widescreen scale and cameras (made at the
    // boot's first use, the install: ps2x::boot::PerBoot).
    struct State
    {
        PS2Runtime::RecompiledFunction closeUnpack = nullptr;
        PS2Runtime::RecompiledFunction projection = nullptr;
        PS2Runtime::RecompiledFunction sin = nullptr;
        PS2Runtime::RecompiledFunction frustum = nullptr;
        uint32_t frustumCamera = 0;   // the frustum's camera while it runs
        bool frustumFirstSine = false; // the frustum's next sine is its first
        bool wideFrame = false;
        bool used = false;
        float scale = 0.75f;
        std::vector<float> written; // scales written here (replaced when the aspect changes)
        std::vector<CameraFar> far;
    };

    State &state()
    {
        static ps2x::boot::PerBoot<State> ofBoot([] { return State{}; });
        return ofBoot.get();
    }

    void drawDistance(uint8_t *rdram, uint32_t camera, int setting)
    {
        float now = 0.0f;
        if (!readF32(rdram, camera + kCameraFar, now))
            return;
        std::vector<CameraFar> &far = state().far;
        CameraFar *entry = nullptr;
        for (CameraFar &e : far)
            if (e.camera == camera)
                entry = &e;
        const float factor = kDrawDistanceFactor[std::clamp(setting, 0, 2)];
        if (!entry)
        {
            if (factor == 1.0f)
                return;
            if (far.size() >= 8u)
                far.erase(far.begin());
            far.push_back(CameraFar{camera, now, now});
            entry = &far.back();
        }
        if (now != entry->written) // the game set a new one
            entry->game = now;
        const float want = entry->game * factor;
        if (want != now && want > 0.0f)
            writeF32(rdram, camera + kCameraFar, want);
        entry->written = want;
    }

    void projection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool inLevel = readU32(rdram, kGameMode) == kModeLevel && readU32(rdram, kLevelFrame) != 0u;
        const bool wide = play_recording::read(play_recording::Channel::Widescreen, runtime->widescreen()) && inLevel;
        State &st = state();
        st.wideFrame = wide;
        if (wide)
        {
            st.scale = (4.0f / 3.0f) / runtime->wideAspect();
            if (std::find(st.written.begin(), st.written.end(), st.scale) == st.written.end())
                st.written.push_back(st.scale);
        }
        st.used = st.used || wide;
        if (st.used)
        {
            const uint32_t field = getRegU32(ctx, kRegA0) + kCameraXScale;
            const float want = wide ? st.scale : 1.0f;
            float now = 0.0f;
            if (readF32(rdram, field, now) && now != want &&
                (now == 1.0f || std::find(st.written.begin(), st.written.end(), now) != st.written.end()))
                writeF32(rdram, field, want);
        }
        if (wide)
            runtime->markWidescreenFrame();
        drawDistance(rdram, getRegU32(ctx, kRegA0),
                     inLevel ? play_recording::read(play_recording::Channel::DrawDistance, runtime->drawDistance()) : 0);
        st.projection(rdram, ctx, runtime);
    }

    bool wide2D(uint8_t *rdram, PS2Runtime *runtime)
    {
        return state().used && play_recording::read(play_recording::Channel::Widescreen, runtime->widescreen()) && readU32(rdram, kGameMode) == kModeLevel &&
               readU32(rdram, kLevelFrame) != 0u;
    }

    // The 2D primitive whose UNPACK is being closed: data from the UNPACK
    // code + 4 to the packet's current pointer.
    void squeeze2DPrimitive(uint8_t *rdram)
    {
        const uint32_t current = readU32(rdram, kPacket2D + kPacketCurrent);
        const uint32_t code = readU32(rdram, kPacket2D + kPacketVifCode);
        float width = 0.0f;
        if (code == 0u || !readF32(rdram, kScreenWidth, width) || !(width > 0.0f))
            return;
        const uint32_t unpack = readU32(rdram, code);
        if (((unpack >> 24) & 0x6Fu) != 0x6Cu || (unpack & 0x3FFu) != kVu1PrimAddress)
            return;
        const uint32_t data = code + 4u;
        if (current <= data || ((current - data) & 15u) != 0u)
            return;
        const uint64_t tag = readU32(rdram, data) | (static_cast<uint64_t>(readU32(rdram, data + 4u)) << 32);
        const uint64_t regs = readU32(rdram, data + 8u) | (static_cast<uint64_t>(readU32(rdram, data + 12u)) << 32);
        const uint32_t loops = static_cast<uint32_t>(tag & 0x7FFFu);
        const uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
        if (nreg == 0u || loops == 0u || 1u + loops * nreg != (current - data) / 16u || ((tag >> 58) & 3u) != 0u)
            return;
        uint32_t xyz = nreg;
        for (uint32_t r = 0; r < nreg; ++r)
        {
            const uint32_t reg = static_cast<uint32_t>((regs >> (4u * r)) & 0xFu);
            if (reg == 4u || reg == 5u) // XYZF2, XYZ2
                xyz = r;
        }
        if (xyz == nreg)
            return;
        auto vertexX = [&](uint32_t i) { return data + 16u * (1u + i * nreg + xyz); };
        float lo = 0.0f, hi = 0.0f;
        for (uint32_t i = 0; i < loops; ++i)
        {
            float x = 0.0f;
            if (!readF32(rdram, vertexX(i), x))
                return;
            lo = i == 0u ? x : std::min(lo, x);
            hi = i == 0u ? x : std::max(hi, x);
        }
        if (lo <= kFullWidthMargin && hi >= width - kFullWidthMargin)
            return;
        const float centre = width * 0.5f;
        for (uint32_t i = 0; i < loops; ++i)
        {
            uint8_t *p = getMemPtr(rdram, vertexX(i));
            if (!p)
                return;
            uint32_t bits = 0, out = 0;
            float x = 0.0f;
            std::memcpy(&bits, p, sizeof(bits));
            std::memcpy(&x, p, sizeof(x));
            const float squeezed = centre + (x - centre) * state().scale;
            std::memcpy(&out, &squeezed, sizeof(out));
            out = (out & ~1u) | (bits & 1u); // keep the ADC flag
            std::memcpy(p, &out, sizeof(out));
        }
    }

    void closeUnpack(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, kRegA0) == kPacket2D && wide2D(rdram, runtime))
            squeeze2DPrimitive(rdram);
        state().closeUnpack(rdram, ctx, runtime);
    }

    void frustum(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &st = state();
        st.frustumCamera = getRegU32(ctx, kRegA0);
        st.frustumFirstSine = true;
        st.frustum(rdram, ctx, runtime);
        st.frustumFirstSine = false;
    }

    void sine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        State &st = state();
        const bool first = st.frustumFirstSine;
        st.frustumFirstSine = false;
        if (first)
            zge_host::sites::seen("dm2.widescreen.frustum");
        if (st.wideFrame && first)
        {
            const uint32_t camera = st.frustumCamera;
            float width = 0.0f, focal = 0.0f;
            if (readF32(rdram, camera + kCameraWidth, width) && readF32(rdram, camera + kCameraFocal, focal) &&
                focal != 0.0f)
            {
                const float tanX = width * 0.5f / st.scale / focal;
                if (tanX != 0.0f)
                {
                    writeF32(rdram, camera + kCameraTanX, tanX);
                    writeF32(rdram, camera + kCameraInvTanX, 1.0f / tanX);
                }
            }
        }
        st.sin(rdram, ctx, runtime);
    }
}

namespace dave_mirra
{
    bool installView(PS2Runtime &runtime)
    {
        using namespace view;
        State &st = state();
        st.projection = runtime.lookupFunction(kProjection);
        st.sin = runtime.lookupFunction(kSin);
        st.closeUnpack = runtime.lookupFunction(kCloseUnpack);
        st.frustum = runtime.lookupFunction(kFrustum);
        if (!st.projection || !st.sin || !st.closeUnpack || !st.frustum || !runtime.replaceFunction(kProjection, &projection) ||
            !runtime.replaceFunction(kSin, &sine) || !runtime.replaceFunction(kCloseUnpack, &closeUnpack) ||
            !runtime.replaceFunction(kFrustum, &frustum))
        {
            std::cerr << "[dave-mirra] widescreen / draw distance hooks not installed" << std::endl;
            return false;
        }
        return true;
    }
}
