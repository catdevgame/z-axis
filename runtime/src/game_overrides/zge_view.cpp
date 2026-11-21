// The view settings shared by the games (zge_view.h).

#include "zge_view.h"
#include "runtime/play_recording.h"
#include "zge_sites.h"

#include "ps2_runtime.h"
#include "runtime/ps2_session.h"
#include "runtime/ee_time.h"
#include "runtime/ps2_game_pose.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zge_host::view
{
    namespace
    {
        // ZGE4's camera record (zge/scene/camera.h Camera): the blur's switch
        // and its colour's alpha.
        constexpr uint32_t kCameraMotionBlurOn = 0x4EC;
        constexpr uint32_t kCameraMotionBlurAlpha = 0x503; // motionBlurColour's fourth byte
        constexpr int kRegA0 = 4, kRegSp = 29;
        constexpr int kGameBlur = 2, kNoBlur = 0;

        // A VIF1 packet (sce::Vif1Packet): where the next word goes.
        constexpr uint32_t kPacketCurrent = 0x00;
        constexpr int kRegA1 = 5;

        struct State
        {
            PS2Runtime::RecompiledFunction drawBlur = nullptr;
            const char *game = "";
            bool trace = false; // ZX_TRACE_MOTION_BLUR=1: the bytes each level's draws add, every 120 draws
            uint64_t draws[3] = {}, bytes[3] = {};
        };
        State &state() { return zx::session::state<State>(); }

        uint32_t packetAt(const uint8_t *rdram, uint32_t packet)
        {
            uint32_t at = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, packet + kPacketCurrent))
                std::memcpy(&at, p, sizeof(at));
            return at;
        }

        void drawBlurTraced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int level)
        {
            State &st = state();
            const uint32_t packet = getRegU32(ctx, kRegA1);
            const uint32_t before = packetAt(rdram, packet);
            st.drawBlur(rdram, ctx, runtime);
            ++st.draws[level];
            st.bytes[level] += packetAt(rdram, packet) - before;
            if (st.draws[level] % 120u == 0u)
                std::cout << "[" << st.game << "] motion blur " << level << ": " << st.bytes[level] << " packet bytes in "
                          << st.draws[level] << " draws" << std::endl;
        }

        void drawBlur(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int setting = play_recording::read(play_recording::Channel::MotionBlur, runtime->motionBlur());
            const int level = std::clamp(setting, 0, kGameBlur);
            const uint32_t camera = getRegU32(ctx, kRegA0);
            uint8_t *on = level < kGameBlur ? getMemPtr(rdram, camera + kCameraMotionBlurOn) : nullptr;
            uint8_t *alpha = level < kGameBlur ? getMemPtr(rdram, camera + kCameraMotionBlurAlpha) : nullptr;
            if (!on || !alpha)
            {
                if (state().trace)
                    drawBlurTraced(rdram, ctx, runtime, level);
                else
                    state().drawBlur(rdram, ctx, runtime);
                return;
            }
            uint32_t savedOn = 0;
            std::memcpy(&savedOn, on, sizeof(savedOn));
            const uint8_t savedAlpha = *alpha;
            if (level == kNoBlur)
                std::memset(on, 0, sizeof(savedOn));
            else
                *alpha = static_cast<uint8_t>(savedAlpha / 2u);
            if (state().trace)
                drawBlurTraced(rdram, ctx, runtime, level);
            else
                state().drawBlur(rdram, ctx, runtime);
            std::memcpy(on, &savedOn, sizeof(savedOn));
            *alpha = savedAlpha;
        }
    }

    bool installMotionBlur(PS2Runtime &runtime, const MotionBlur &blur)
    {
        state().drawBlur = runtime.lookupFunction(blur.drawBlur);
        state().game = blur.game;
        const char *trace = std::getenv("ZX_TRACE_MOTION_BLUR");
        state().trace = trace && trace[0] == '1';
        // For checks in the runner (the app sets it from its settings): 0, 1 or 2.
        if (const char *level = std::getenv("ZX_MOTION_BLUR"); level && level[0] >= '0' && level[0] <= '2')
            runtime.setMotionBlur(level[0] - '0');
        if (!state().drawBlur || !runtime.replaceFunction(blur.drawBlur, &drawBlur))
        {
            std::cerr << "[" << blur.game << "] motion blur hook not installed" << std::endl;
            return false;
        }
        return true;
    }
}

// ---- widescreen -----------------------------------------------------------
//
// The same changes as the PCSX2 "Widescreen 16:9" patch for Aggressive Inline
// (SLUS-20327, by BloodRaynare, based on the PAL patch by ElHecht), done at
// run time so one binary does both, on the game's camera (ZGE4's,
// zge/scene/camera.h; ZGE2's at its own offsets, WideCamera):
//  - updateProjection(camera) builds the screen projection from the camera's
//    horizontal scale (Camera::horizontalScale, 1.0 from the display at the
//    camera's set-up). The hook wraps it: with widescreen on it first sets
//    the scale (and the game's display words that hold it, AI's) to 4:3 /
//    aspect, 0.75 for 16:9, and back to 1.0 when widescreen is turned off,
//    so the scene is drawn narrower for a picture the host stretches
//    (markWidescreenFrame) and the toggle applies on the next frame.
//  - updateFrustum(camera), each frame after the projection, stores the
//    tangent of the horizontal half angle (Camera::tanHalfWidth: the
//    viewport's half width over the focal length) and its inverse, then calls
//    angleSin first. The frustum is wrapped, so the hook on angleSin knows
//    that call by running inside it and the camera by the frustum's argument
//    (never by a return address, which a readable caller does not give), and
//    rewrites both with the half width divided by the scale: the frustum's
//    corners and planes are as much wider, and objects at the sides are not
//    culled.
//  - A field of view setting (the table's drawnFieldOfView: degrees to draw a
//    view camera with for the game's own) is drawn the same way: at that
//    sine the hook also gives the camera the focal length of the drawn field
//    of view (Camera::focalLength; the frustum reads it next for the
//    vertical tangent, the projection the next frame), so the picture and
//    the frustum are the drawn view's while the camera's field of view, and
//    everything the game works out from it, stays the game's.
// The frustum keeps the game's own planes beside the drawn ones when the two
// differ (zge/scene/camera.cpp updateFrustum, zge/host/game_view.h): what the
// game decides by its view (a node drawn or not) stays what the PS2 decides,
// so a view setting never changes play.
// Only in a level (the game's table says when): the front end, loading and
// movies run a camera too but show 2D screens, which stay 4:3, as does their
// presentation (markWidescreenFrame only for wide frames). With widescreen
// never turned on nothing is written: the game runs bit for bit as without
// the hooks.
//
// 2D (the zgui widgets: the HUD, the menus, message boxes): every screen-space
// primitive is one VIF1 UNPACK V4-32 to VU1 address 8 in the screen packet
// (g_screenPrimitives): a PACKED GIF tag and per vertex ST, RGBAQ and XYZ2,
// with x, y, z as floats in screen pixels (0 .. the display's width; bit 0 of
// x is the ADC flag). The host stretches a wide frame, so each finished 2D
// primitive (at vif1PacketCloseUnpack) has its x squeezed by the scale about
// the screen centre, in place in the packet: the 2D keeps its 4:3 shape,
// centred. Primitives that cover the whole width (fades, the darkened
// background of menus, the letterbox) are left as they are. The 3D path's
// packet is not touched.
// The HUD is also moved to the sides of the wide picture, as PC ports do:
// zgui draws its widget tree with drawWidget, and a widget the game's table
// names as side-anchored, in the left or right third of the screen, is moved
// out by half the width times (1 / scale - 1) (width / 6 for 16:9) while it
// is drawn, so after the squeeze it sits as far from the wide edge as from
// the 4:3 one before. Where the move is made is the engine's:
//  - ZGE3 and ZGE4 (WidgetTree::Nodes): a widget's screen position (Node::
//    screenPosition) is its parent's plus its own (Node::position), so its
//    own position is moved for the draw (its children follow) and put back.
//  - ZGE2 (WidgetTree::SceneObjects, DM2): a widget is a scene object whose
//    record holds where it is on the screen (WidgetRecord::position), as each
//    object hanging from it holds its own; what it and they draw while it is
//    drawn is moved instead: every 2D primitive closed meanwhile is squeezed
//    and then moved out by half the width times (1 - scale), the same place.
//    The game's RAM is not changed.
// HUD elements that are no widget (DM2's ruler and replay mark) are drawn by
// functions the table names (SideDraw); their primitives are moved the same
// way while they run, to their side, or each primitive to the side whose
// third it lies in whole.
namespace zge_host::view
{
    namespace
    {
        // A primitive packet (PrimitivePacketState, a VIF1 packet): the open
        // UNPACK's VIF code (kPacketCurrent, above: where the next word goes).
        constexpr uint32_t kPacketVifCode = 0x0C;
        constexpr uint32_t kVu1PrimAddress = 8u;
        constexpr float kFullWidthMargin = 4.0f;

        // A zgui node (ZGE3's and ZGE4's, zge/zgui/widget.h Node).
        constexpr uint32_t kWidgetParent = 0x08;
        constexpr uint32_t kWidgetType = 0x0C; // low 14 bits: 1 = drawable widget
        constexpr uint32_t kWidgetName = 0x14;
        constexpr uint32_t kWidgetPosition = 0x20;
        constexpr uint32_t kWidgetScreenPosition = 0x30;
        // A ZGE2 widget object (zge/scene/scene.h SceneObject, zge/zgui/
        // widget.h WidgetRecord): its kind and shown flag, the attachment it
        // hangs from its parent by (ObjectAttachment::parent at +0), its
        // record, and the record's place and name.
        constexpr uint32_t kObjectFlags = 0x00;
        constexpr uint64_t kObjectKindMask = 0x0Fu;
        constexpr uint64_t kWidgetObjectKind = 4u;
        constexpr uint64_t kWidgetObjectShown = 0x10u;
        constexpr uint32_t kObjectAttachment = 0x30;
        constexpr uint32_t kAttachmentParent = 0x00;
        constexpr uint32_t kObjectWidgetRecord = 0x3C;
        constexpr uint32_t kRecordPosition = 0x00;
        constexpr uint32_t kRecordName = 0x4C;
        constexpr size_t kMaxSideDraws = 4;

        template <size_t N>
        bool readFloats(const uint8_t *rdram, uint32_t address, float (&out)[N])
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            if (!p || getConstMemPtr(rdram, address + N * 4u - 1u) != p + N * 4u - 1u)
                return false;
            std::memcpy(out, p, sizeof(out));
            for (float f : out)
                if (!std::isfinite(f))
                    return false;
            return true;
        }

        template <size_t N>
        bool writeFloats(uint8_t *rdram, uint32_t address, const float (&in)[N])
        {
            uint8_t *p = getMemPtr(rdram, address);
            if (!p || getMemPtr(rdram, address + N * 4u - 1u) != p + N * 4u - 1u)
                return false;
            std::memcpy(p, in, sizeof(in));
            return true;
        }

        bool readU32(const uint8_t *rdram, uint32_t address, uint32_t &out)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            if (!p || (address & 3u) != 0u)
                return false;
            std::memcpy(&out, p, sizeof(out));
            return true;
        }

        bool readU64(const uint8_t *rdram, uint32_t address, uint64_t &out)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            if (!p || getConstMemPtr(rdram, address + 7u) != p + 7)
                return false;
            std::memcpy(&out, p, sizeof(out));
            return true;
        }

        // A HUD element being drawn (a ZGE2 widget's draw, a side draw's):
        // the side its 2D primitives are moved to (-1, +1), or each
        // primitive to its own (eachPrimitive), from the call's start to
        // its return. A call can come back to the hook before it has
        // returned (its fiber left at a safe point or a wait: the rest runs
        // later, from the scheduler; the context's stack pointer is then its
        // callee's, not the call's), so the scope is kept open then
        // (unfinished) until a call of the same caller, or of one further
        // out, is drawn (a hook entered with a stack pointer at or above
        // the scope's: the element's own calls are below it), or the next
        // camera frame.
        struct HudScope
        {
            bool open = false;
            bool unfinished = false;
            int side = 0;
            bool eachPrimitive = false;
            uint32_t sp = 0; // the stack pointer the element's call was made with
        };

        // A ZGE3 or ZGE4 widget's move (Node::position, its x) kept while its
        // draw has not returned: a call that came back unfinished (as
        // HudScope's) is undone once a draw of the same caller, or of one
        // further out, is entered, or at the next camera frame; until then
        // the rest of its draw, and its children's, sees the widget moved.
        struct HeldMove
        {
            uint32_t widget = 0;
            float shift = 0.0f;
            uint32_t sp = 0; // the stack pointer the widget's draw was called with
        };

        struct Wide
        {
            Widescreen table{};
            PS2Runtime::RecompiledFunction projection = nullptr;
            PS2Runtime::RecompiledFunction sinCos = nullptr;
            PS2Runtime::RecompiledFunction frustum = nullptr;
            PS2Runtime::RecompiledFunction closeUnpack = nullptr;
            PS2Runtime::RecompiledFunction drawWidget = nullptr;
            uint32_t frustumCamera = 0;   // the frustum being built's camera
            bool frustumFirstSin = false; // its first angleSin call is still to come
            bool wideFrame = false;       // the camera being set up is drawn for widescreen
            bool levelFrame = false;      // the camera being set up is a level's
            bool used = false;            // turned on at least once this run
            bool trace = false;           // ZX_TRACE_WIDESCREEN=1
            // While a ZGE2 widget or a side draw runs: the side its 2D
            // primitives are moved to (HudScope).
            HudScope hud;
            // ZGE3's and ZGE4's widget moves whose draws came back unfinished.
            std::vector<HeldMove> heldMoves;
            PS2Runtime::RecompiledFunction sideDraws[kMaxSideDraws] = {};
            uint32_t traceLastMode = ~0u, traceCalls = 0; // the trace's game mode and camera frames
            uint64_t traceSqueezed = 0, traceMoved = 0;     // 2D primitives squeezed, HUD widgets moved
            uint64_t traceUnfinished = 0;                  // HUD element calls that came back before their return
            std::vector<std::string> traceMovedNames;      // each HUD widget moved, once
            // Other aspects than the patch's 16:9: the scale is 4:3 / aspect
            // (0.75 for 16:9, 0.8333 for 16:10, 0.5714 for 21:9), set each
            // frame from PS2Runtime::wideAspect(). The values written so far
            // are kept so a changed aspect replaces them (the game itself only
            // ever writes 1.0).
            float wideXScale = 0.75f;
            std::vector<float> writtenXScales;
        };
        Wide &wide() { return zx::session::state<Wide>(); }

        float wideXScale(PS2Runtime *runtime)
        {
            return (4.0f / 3.0f) / runtime->wideAspect();
        }

        // Sets the horizontal scale at address to `to` if it holds the game's
        // 1.0 or a scale written here before.
        void setXScale(uint8_t *rdram, uint32_t address, float to)
        {
            Wide &w = wide();
            float value[1];
            if (!readFloats(rdram, address, value) || value[0] == to)
                return;
            if (value[0] == 1.0f || std::find(w.writtenXScales.begin(), w.writtenXScales.end(), value[0]) != w.writtenXScales.end())
            {
                const float out[1] = {to};
                writeFloats(rdram, address, out);
            }
        }

        void endHeldMoves(uint8_t *rdram, uint32_t sp);

        void projectionHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Wide &w = wide();
            w.hud = HudScope{}; // (an element left unfinished is drawn by now)
            endHeldMoves(rdram, ~0u);
            uint32_t mode = 0;
            const bool wideNow = play_recording::read(play_recording::Channel::Widescreen, runtime->widescreen()) && w.table.inLevel(rdram, mode);
            w.wideFrame = wideNow;
            w.levelFrame = w.table.inLevel(rdram, mode);
            if (wideNow)
            {
                w.wideXScale = wideXScale(runtime);
                if (std::find(w.writtenXScales.begin(), w.writtenXScales.end(), w.wideXScale) == w.writtenXScales.end())
                    w.writtenXScales.push_back(w.wideXScale);
            }
            if (w.trace)
            {
                ++w.traceCalls;
                if (mode != w.traceLastMode)
                    std::fprintf(stderr, "[widescreen] game mode %u after %u camera frames\n", mode, w.traceCalls);
                w.traceLastMode = mode;
                if (wideNow && w.traceCalls % 600u == 0u)
                {
                    float tan[1] = {0.0f}, scale[1] = {0.0f};
                    const uint32_t camera = getRegU32(ctx, kRegA0);
                    readFloats(rdram, camera + w.table.camera.tanHalfWidth, tan);
                    readFloats(rdram, camera + w.table.camera.horizontalScale, scale);
                    std::fprintf(stderr,
                                 "[widescreen] camera 0x%x: scale %.4f, half-width tangent %.4f; %llu 2D primitives "
                                 "squeezed, %llu HUD widget draws moved (%llu came back unfinished)\n",
                                 camera, scale[0], tan[0], static_cast<unsigned long long>(w.traceSqueezed),
                                 static_cast<unsigned long long>(w.traceMoved),
                                 static_cast<unsigned long long>(w.traceUnfinished));
                }
            }
            w.used = w.used || wideNow;
            if (w.used)
            {
                const float scale = wideNow ? w.wideXScale : 1.0f;
                for (size_t i = 0; i < w.table.displayXScaleCount; ++i)
                    setXScale(rdram, w.table.displayXScales[i], scale);
                setXScale(rdram, getRegU32(ctx, kRegA0) + w.table.camera.horizontalScale, scale);
            }
            if (wideNow)
                runtime->markWidescreenFrame();
            w.projection(rdram, ctx, runtime);
        }

        // A level's view camera's focal length for the field of view the game's
        // table draws it with (`focal` the game's, which the frustum just
        // stored); the game's own when the table draws the game's.
        float drawnFocalLength(const uint8_t *rdram, uint32_t camera, float focal)
        {
            Wide &w = wide();
            uint32_t kind = 0;
            const uint8_t *angle = getConstMemPtr(rdram, camera + w.table.camera.fieldOfView);
            if (!w.levelFrame || !w.table.drawnFieldOfView || !angle || !readU32(rdram, camera + w.table.camera.kind, kind) || kind != 0u)
                return focal;
            int16_t units = 0;
            std::memcpy(&units, angle, sizeof(units));
            constexpr float kDegreesPerUnit = 360.0f / 65536.0f, kRadiansPerDegree = 3.14159265f / 180.0f;
            const float game = static_cast<float>(static_cast<uint16_t>(units)) * kDegreesPerUnit;
            const float drawn = w.table.drawnFieldOfView(rdram, game);
            if (drawn == game || !(drawn > 0.0f && drawn < 180.0f) || !(game > 0.0f && game < 180.0f))
                return focal;
            return focal * std::tan(game * 0.5f * kRadiansPerDegree) / std::tan(drawn * 0.5f * kRadiansPerDegree);
        }

        void frustumHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Wide &w = wide();
            w.frustumCamera = getRegU32(ctx, kRegA0);
            w.frustumFirstSin = true;
            w.frustum(rdram, ctx, runtime);
            w.frustumFirstSin = false;
        }

        void sinHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // The frustum's first angleSin call (the recompiled one's and the
            // readable one's alike), right after it stored the half-width
            // tangent: the camera is the frustum's argument.
            Wide &w = wide();
            const bool firstInFrustum = w.frustumFirstSin;
            w.frustumFirstSin = false;
            if (firstInFrustum)
                zge_host::sites::seen(w.table.frustumSite);
            if (firstInFrustum && (w.wideFrame || (w.levelFrame && w.table.drawnFieldOfView)))
            {
                const uint32_t camera = w.frustumCamera;
                const WideCamera &layout = w.table.camera;
                float width[1], focal[1];
                if (readFloats(rdram, camera + layout.spannedWidth, width) &&
                    readFloats(rdram, camera + layout.focalLength, focal) && focal[0] != 0.0f)
                {
                    // The drawn field of view: the focal length that spans it.
                    const float drawnFocal = drawnFocalLength(rdram, camera, focal[0]);
                    if (drawnFocal != focal[0])
                    {
                        const float out[1] = {drawnFocal};
                        writeFloats(rdram, camera + layout.focalLength, out);
                    }
                    const float tanX = width[0] * 0.5f / (w.wideFrame ? w.wideXScale : 1.0f) / drawnFocal;
                    if (tanX != 0.0f && (w.wideFrame || drawnFocal != focal[0]))
                    {
                        const float tan[1] = {tanX}, inverse[1] = {1.0f / tanX};
                        writeFloats(rdram, camera + layout.tanHalfWidth, tan);
                        writeFloats(rdram, camera + layout.cotHalfWidth, inverse);
                    }
                }
            }
            w.sinCos(rdram, ctx, runtime);
        }

        // 2D is corrected in this frame: a wide level frame.
        bool wide2D(uint8_t *rdram, PS2Runtime *runtime)
        {
            uint32_t mode = 0;
            return wide().used && play_recording::read(play_recording::Channel::Widescreen, runtime->widescreen()) && wide().table.inLevel(rdram, mode);
        }

        // Squeezes the 2D primitive whose UNPACK is being closed (data from
        // the UNPACK code + 4 to the packet's current pointer).
        void squeeze2DPrimitive(uint8_t *rdram, uint32_t packet)
        {
            uint32_t current = 0, code = 0, unpack = 0;
            float width[1];
            if (!readU32(rdram, packet + kPacketCurrent, current) || !readU32(rdram, packet + kPacketVifCode, code) ||
                code == 0u || !readU32(rdram, code, unpack) || !readFloats(rdram, wide().table.screenWidth, width) ||
                !(width[0] > 0.0f))
                return;
            // UNPACK V4-32 (mask bit ignored) to VU1 address 8 (+TOPS).
            if (((unpack >> 24) & 0x6Fu) != 0x6Cu || (unpack & 0x3FFu) != kVu1PrimAddress)
                return;
            const uint32_t data = code + 4u;
            if (current <= data || ((current - data) & 15u) != 0u)
                return;
            uint64_t tag = 0, regs = 0;
            if (!readU64(rdram, data, tag) || !readU64(rdram, data + 8u, regs))
                return;
            // PACKED GIF tag covering exactly the unpacked qwords.
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
                float x[1];
                if (!readFloats(rdram, vertexX(i), x))
                    return;
                lo = i == 0u ? x[0] : std::min(lo, x[0]);
                hi = i == 0u ? x[0] : std::max(hi, x[0]);
            }
            if (lo <= kFullWidthMargin && hi >= width[0] - kFullWidthMargin)
                return;
            const float centre = width[0] * 0.5f;
            // A HUD element's draw (a ZGE2 widget's, a side draw's): moved
            // out to the wide picture's edge, as far as the 4:3 one was.
            Wide &w = wide();
            const int side = !w.hud.open           ? 0
                             : !w.hud.eachPrimitive ? w.hud.side
                             : hi <= width[0] / 3.0f ? -1 : lo >= width[0] * 2.0f / 3.0f ? 1 : 0;
            const float moved = static_cast<float>(side) * centre * (1.0f - w.wideXScale);
            for (uint32_t i = 0; i < loops; ++i)
            {
                uint8_t *p = getMemPtr(rdram, vertexX(i));
                uint32_t bits = 0, out = 0;
                float x = 0.0f;
                std::memcpy(&bits, p, sizeof(bits));
                std::memcpy(&x, p, sizeof(x));
                float squeezed = centre + (x - centre) * w.wideXScale;
                if (side != 0)
                    squeezed += moved;
                std::memcpy(&out, &squeezed, sizeof(out));
                out = (out & ~1u) | (bits & 1u); // keep the ADC flag
                std::memcpy(p, &out, sizeof(out));
            }
            ++wide().traceSqueezed;
        }

        void closeUnpackHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t packet = wide().table.screenPrimitives;
            if (getRegU32(ctx, kRegA0) == packet && wide2D(rdram, runtime))
                squeeze2DPrimitive(rdram, packet);
            wide().closeUnpack(rdram, ctx, runtime);
        }

        // Where a widget's name is (0: nowhere): a node's own, a ZGE2
        // widget object's in its record.
        uint32_t widgetNameAddress(const uint8_t *rdram, uint32_t widget)
        {
            uint32_t address = 0, record = 0;
            if (wide().table.widgets == WidgetTree::Nodes)
                return readU32(rdram, widget + kWidgetName, address) ? address : 0u;
            if (!readU32(rdram, widget + kObjectWidgetRecord, record) || !record ||
                !readU32(rdram, record + kRecordName, address))
                return 0u;
            return address;
        }

        // ZX_TRACE_WIDESCREEN=1: each HUD widget moved, named once.
        void traceMovedWidget(const uint8_t *rdram, uint32_t widget, uint32_t parent, float moved)
        {
            Wide &w = wide();
            ++w.traceMoved;
            auto nameOf = [&](uint32_t node)
            {
                std::string name;
                if (const uint32_t address = widgetNameAddress(rdram, node))
                    for (uint32_t i = 0; i < 64u; ++i)
                    {
                        const uint8_t *c = getConstMemPtr(rdram, address + i);
                        if (!c || *c == 0u)
                            break;
                        name.push_back(static_cast<char>(*c));
                    }
                return name;
            };
            const std::string name = nameOf(parent) + "/" + nameOf(widget);
            if (std::find(w.traceMovedNames.begin(), w.traceMovedNames.end(), name) != w.traceMovedNames.end())
                return;
            w.traceMovedNames.push_back(name);
            std::fprintf(stderr, "[widescreen] HUD widget %s moved %+.1f\n", name.c_str(), moved);
        }

        // How far the HUD widget is moved while drawn: -/+ half the width
        // times (1 / scale - 1) for a side-anchored widget in the left or
        // right third, else 0.
        float hudAnchorShift(uint8_t *rdram, uint32_t widget)
        {
            uint32_t type = 0, parent = 0, grandparent = 0;
            float width[1], position[1], parentPosition[1];
            if (!readU32(rdram, widget + kWidgetType, type) || (type & 0x3FFFu) != 1u ||
                !readU32(rdram, widget + kWidgetParent, parent) || !parent ||
                !readU32(rdram, parent + kWidgetParent, grandparent) || !grandparent)
                return 0.0f;
            if (!wide().table.sideAnchored(rdram, widget, parent, grandparent) ||
                !readFloats(rdram, wide().table.screenWidth, width) ||
                !readFloats(rdram, widget + kWidgetPosition, position) ||
                !readFloats(rdram, parent + kWidgetScreenPosition, parentPosition))
                return 0.0f;
            // As far from the wide picture's edge as from the 4:3 one: half
            // the width times (1 / scale - 1), width / 6 for 16:9.
            const float x = parentPosition[0] + position[0];
            const float shift = width[0] * 0.5f * (1.0f / wide().wideXScale - 1.0f);
            const float moved = x < width[0] / 3.0f ? -shift : x > width[0] * 2.0f / 3.0f ? shift : 0.0f;
            if (moved != 0.0f && wide().trace)
                traceMovedWidget(rdram, widget, parent, moved);
            return moved;
        }

        // A ZGE2 widget object's side while drawn: -1 or +1 for a shown
        // side-anchored widget whose place is in the left or right third,
        // else 0.
        int objectHudSide(const uint8_t *rdram, uint32_t widget)
        {
            uint64_t flags = 0;
            uint32_t attachment = 0, parent = 0, parentAttachment = 0, grandparent = 0, record = 0;
            float width[1], position[1];
            if (!readU64(rdram, widget + kObjectFlags, flags) ||
                (flags & (kObjectKindMask | kWidgetObjectShown)) != (kWidgetObjectKind | kWidgetObjectShown) ||
                !readU32(rdram, widget + kObjectAttachment, attachment) || !attachment ||
                !readU32(rdram, attachment + kAttachmentParent, parent) || !parent ||
                !readU32(rdram, parent + kObjectAttachment, parentAttachment) || !parentAttachment ||
                !readU32(rdram, parentAttachment + kAttachmentParent, grandparent) || !grandparent)
                return 0;
            if (!wide().table.sideAnchored(rdram, widget, parent, grandparent) ||
                !readFloats(rdram, wide().table.screenWidth, width) ||
                !readU32(rdram, widget + kObjectWidgetRecord, record) || !record ||
                !readFloats(rdram, record + kRecordPosition, position))
                return 0;
            // A widget at the screen's origin places nothing itself: a group,
            // whose children are judged by their own places.
            float place[2];
            if (!readFloats(rdram, record + kRecordPosition, place) || (place[0] == 0.0f && place[1] == 0.0f))
                return 0;
            const float x = position[0];
            const int side = x < width[0] / 3.0f ? -1 : x > width[0] * 2.0f / 3.0f ? 1 : 0;
            if (side != 0 && wide().trace)
                traceMovedWidget(rdram, widget, parent, static_cast<float>(side) * width[0] * 0.5f * (1.0f - wide().wideXScale));
            return side;
        }

        // Whether a HUD element's call entered now is inside the scope open:
        // an unfinished element is over once a call at its own level or
        // further out is made.
        bool insideHudScope(R5900Context *ctx)
        {
            HudScope &hud = wide().hud;
            if (hud.open && hud.unfinished && getRegU32(ctx, kRegSp) >= hud.sp)
                hud = HudScope{};
            return hud.open;
        }

        // Calls an element's draw with its scope open (side 0 with
        // eachPrimitive: each primitive to its own side), and closes it on
        // its return, or leaves it open if it came back unfinished.
        void drawHudElement(PS2Runtime::RecompiledFunction draw, int side, bool eachPrimitive, uint8_t *rdram,
                            R5900Context *ctx, PS2Runtime *runtime)
        {
            Wide &w = wide();
            const uint32_t sp = getRegU32(ctx, kRegSp);
            w.hud = HudScope{true, false, side, eachPrimitive, sp};
            ++w.traceMoved;
            draw(rdram, ctx, runtime);
            // Returned: the stack pointer is the call's again (one that left
            // before its return is at its callee's, below).
            if (getRegU32(ctx, kRegSp) == sp)
                w.hud = HudScope{};
            else
            {
                w.hud.unfinished = true;
                ++w.traceUnfinished;
            }
        }

        // ZGE2's widget draw: a side-anchored widget's draws, and its
        // children's, moved to its side (the squeeze moves each primitive).
        void drawWidgetObjectHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Wide &w = wide();
            const int side = !insideHudScope(ctx) && wide2D(rdram, runtime) ? objectHudSide(rdram, getRegU32(ctx, kRegA0)) : 0;
            if (side != 0)
                drawHudElement(w.drawWidget, side, false, rdram, ctx, runtime);
            else
                w.drawWidget(rdram, ctx, runtime);
        }

        // A side draw (the table's I-th): its 2D primitives moved to its
        // side, or each to its own, while it runs.
        template <size_t I>
        void sideDrawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Wide &w = wide();
            const SideDraw &draw = w.table.sideDraws[I];
            if (!insideHudScope(ctx) && wide2D(rdram, runtime) && (!draw.drawsHud || draw.drawsHud(rdram)))
                drawHudElement(w.sideDraws[I], draw.side, draw.side == 0, rdram, ctx, runtime);
            else
                w.sideDraws[I](rdram, ctx, runtime);
        }
        template <size_t... I>
        constexpr std::array<PS2Runtime::RecompiledFunction, sizeof...(I)> sideDrawHooks(std::index_sequence<I...>)
        {
            return {&sideDrawHook<I>...};
        }
        constexpr auto kSideDrawHooks = sideDrawHooks(std::make_index_sequence<kMaxSideDraws>{});

        // Moves a ZGE3 or ZGE4 widget's x by `shift` (undone as a move, not
        // a restore, in case its draw moved it too).
        void moveWidget(uint8_t *rdram, uint32_t widget, float shift)
        {
            float position[1];
            if (readFloats(rdram, widget + kWidgetPosition, position))
            {
                position[0] += shift;
                writeFloats(rdram, widget + kWidgetPosition, position);
            }
        }

        // The held moves whose draws are over by a call entered with stack
        // pointer sp (~0u: all of them, at a camera frame) undone.
        void endHeldMoves(uint8_t *rdram, uint32_t sp)
        {
            std::vector<HeldMove> &held = wide().heldMoves;
            for (size_t i = held.size(); i-- > 0;)
                if (sp >= held[i].sp)
                {
                    moveWidget(rdram, held[i].widget, -held[i].shift);
                    held.erase(held.begin() + static_cast<std::ptrdiff_t>(i));
                }
        }

        // ZGE3's and ZGE4's widget draw: a side-anchored widget moved while
        // it, and its children, are drawn, then put back; one that comes
        // back unfinished stays moved until it is over (HeldMove).
        void drawWidgetHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Wide &w = wide();
            const uint32_t widget = getRegU32(ctx, kRegA0), sp = getRegU32(ctx, kRegSp);
            endHeldMoves(rdram, sp);
            const float shift = wide2D(rdram, runtime) ? hudAnchorShift(rdram, widget) : 0.0f;
            if (shift != 0.0f)
                moveWidget(rdram, widget, shift);
            w.drawWidget(rdram, ctx, runtime);
            if (shift == 0.0f)
                return;
            // Returned: the stack pointer is the call's again (one that left
            // before its return is at its callee's, below).
            if (getRegU32(ctx, kRegSp) == sp)
                moveWidget(rdram, widget, -shift);
            else
            {
                w.heldMoves.push_back({widget, shift, sp});
                ++w.traceUnfinished;
            }
        }
    }

    bool widgetNamed(const uint8_t *rdram, uint32_t widget, const char *name)
    {
        const uint32_t address = widgetNameAddress(rdram, widget);
        if (!address)
            return false;
        const size_t length = std::strlen(name) + 1u;
        const uint8_t *p = getConstMemPtr(rdram, address);
        return p && getConstMemPtr(rdram, address + static_cast<uint32_t>(length) - 1u) == p + length - 1u &&
               std::memcmp(p, name, length) == 0;
    }

    bool installWidescreen(PS2Runtime &runtime, const Widescreen &table)
    {
        Wide &w = wide();
        w.table = table;
        w.projection = runtime.lookupFunction(table.updateProjection);
        w.sinCos = runtime.lookupFunction(table.angleSin);
        w.frustum = runtime.lookupFunction(table.updateFrustum);
        w.closeUnpack = runtime.lookupFunction(table.closeUnpack);
        w.drawWidget = runtime.lookupFunction(table.drawWidget);
        const char *trace = std::getenv("ZX_TRACE_WIDESCREEN");
        w.trace = trace && trace[0] == '1';
        const PS2Runtime::RecompiledFunction drawWidget =
            table.widgets == WidgetTree::Nodes ? &drawWidgetHook : &drawWidgetObjectHook;
        if (!w.projection || !w.sinCos || !w.frustum || !w.closeUnpack || !w.drawWidget ||
            !runtime.replaceFunction(table.updateProjection, &projectionHook) ||
            !runtime.replaceFunction(table.angleSin, &sinHook) ||
            !runtime.replaceFunction(table.updateFrustum, &frustumHook) ||
            !runtime.replaceFunction(table.closeUnpack, &closeUnpackHook) ||
            !runtime.replaceFunction(table.drawWidget, drawWidget))
        {
            std::cerr << "[" << table.game << "] widescreen hooks not installed" << std::endl;
            return false;
        }
        bool ok = table.sideDrawCount <= kMaxSideDraws;
        for (size_t i = 0; ok && i < table.sideDrawCount; ++i)
        {
            w.sideDraws[i] = runtime.lookupFunction(table.sideDraws[i].function);
            ok = w.sideDraws[i] && runtime.replaceFunction(table.sideDraws[i].function, kSideDrawHooks[i]);
        }
        if (!ok)
            std::cerr << "[" << table.game << "] the HUD's side draw hooks not installed" << std::endl;
        return ok;
    }
}

// ---- draw distance ----------------------------------------------------------
//
// A camera's far clip (Camera::farClip) is set through setCameraFarPlane,
// which caps it at 1000, and its fog starts and ends (Camera::fogStart,
// fogEnd) through setCameraFogStart and setCameraFogEnd. The projection, the
// view frustum's far plane and the fog read them each frame. The hook
// multiplies every far clip by the setting's factor (to at most the game's
// furthest for it) and the camera's fog by the same ratio (the hooked fog
// setters remember the game's values and apply the ratio; a changed ratio
// rescales them), so the fog still ends before the clip. Past the cap of 1000
// the hook stores what setCameraFarPlane would have (the value and the
// camera's changed flag). The projection maps the GS's 16-bit Z from near to
// that far, for every draw alike. A game that moves its fog range to end at
// the far clip each frame (moveCameraClipRange(camera, depth), BMX XXX's) has
// the fog end where the clip went already; the range's depth is multiplied by
// the camera's ratio there, so the fog starts as far before the clip, in
// proportion, as at the game's own. At the game's own nothing is changed.
//
// The levels of detail follow: the scene draw picks a node's part per view
// from its levels of detail (Node::lods, set by setLodTable from the model;
// shared by every node of the model): a count in bits 8-11 of its first word
// and, from +0x18, one pointer per level to a record whose float at +4 is
// that level's end distance; the first level whose end lies beyond the
// node's view depth is drawn, none past the last one. With a further far
// clip the fine parts would switch to the coarse ones, or vanish, at the
// PS2's distances, well inside the new fog. The ends are multiplied by the
// far clip's ratio (the largest any camera has), on every render path, so the
// game's state follows the setting alone: the fine parts are kept as far
// out, in proportion, as on the PS2. Each end is
// written in RAM (the original kept here by address; a value that is no
// longer what was written is not ours any more and is taken afresh), set as
// setLodTable gives a node its table and all again when the ratio changes
// (the far clips the game sets, back to 1 at the game's own). A function whose far clips copy one set already (BMX XXX's 2D pass
// gives the 2D camera the first view camera's) is wrapped, and its calls are
// passed as they are. ZX_LOD_DISTANCE=0 (developers, A/B) keeps the PS2's ends;
// ZX_TRACE_DRAW_DISTANCE=1 logs the cameras' changes.
namespace zge_host::view
{
    namespace
    {
        constexpr uint32_t kCameraFlags = 0x08;     // Camera::changed
        constexpr uint32_t kCameraFar = 0x3C;       // Camera::farClip
        constexpr uint32_t kCameraFogStart = 0x5C;  // Camera::fogStart, fogEnd
        constexpr uint64_t kCameraFarChanged = 0x8;
        constexpr uint64_t kCameraFogChanged = 0x10;
        constexpr float kGameMaxFarClip = 1000.0f;  // setCameraFarPlane's cap
        constexpr int kFpA0 = 12; // f12
        constexpr int kRegRa = 31;

        bool readFloat(const uint8_t *rdram, uint32_t address, float &out)
        {
            uint32_t bits = 0;
            if (!readU32(rdram, address, bits))
                return false;
            std::memcpy(&out, &bits, sizeof(out));
            return true;
        }

        void writeFloat(uint8_t *rdram, uint32_t address, float value)
        {
            const float one[1] = {value};
            writeFloats(rdram, address, one);
        }

        // Per camera: the fog the game set (through the two setters) and the
        // ratio the hook applies to it.
        struct CameraFog
        {
            uint32_t camera = 0;
            float fog[2] = {0.0f, 0.0f};
            bool known[2] = {false, false};
            float ratio = 1.0f;
        };
        struct LodEnd
        {
            float original = 0.0f;
            float written = 0.0f;
        };
        // The cameras that set a far clip lately (the last 16 calls): the LOD
        // ratio is that of the one reaching furthest (the level's view; menu
        // and overlay cameras do not reach as far, and a camera that stops
        // being updated drops out).
        struct LodCamera
        {
            uint32_t camera = 0;
            float far = 0.0f;
            float ratio = 1.0f;
            uint64_t tick = 0;
        };
        struct Distance
        {
            DrawDistance table{};
            PS2Runtime::RecompiledFunction setFarClip = nullptr;
            PS2Runtime::RecompiledFunction setFogStart = nullptr;
            PS2Runtime::RecompiledFunction setFogEnd = nullptr;
            PS2Runtime::RecompiledFunction moveClipRange = nullptr;
            PS2Runtime::RecompiledFunction copiesFarClip = nullptr;
            bool inCopy = false; // copiesFarClip is running
            PS2Runtime::RecompiledFunction setLodTable = nullptr;
            std::vector<CameraFog> cameraFog;
            std::unordered_map<uint32_t, LodEnd> lodEnds; // address of an LOD record's end distance
            float lodRatio = 1.0f;
            bool lodFollow = true;
            LodCamera lodCameras[8];
            uint64_t lodTick = 0;
            bool trace = false;
        };
        Distance &distance() { return zx::session::state<Distance>(); }

        CameraFog &cameraFog(uint32_t camera)
        {
            std::vector<CameraFog> &all = distance().cameraFog;
            for (CameraFog &entry : all)
                if (entry.camera == camera)
                    return entry;
            if (all.size() >= 8u)
                all.erase(all.begin());
            all.push_back(CameraFog{});
            all.back().camera = camera;
            return all.back();
        }

        // The game sets a fog distance: remembered, and set times the camera's ratio.
        void fogHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int which)
        {
            CameraFog &entry = cameraFog(getRegU32(ctx, kRegA0));
            entry.fog[which] = ctx->f[kFpA0];
            entry.known[which] = true;
            ctx->f[kFpA0] *= entry.ratio;
            (which == 0 ? distance().setFogStart : distance().setFogEnd)(rdram, ctx, runtime);
        }

        void fogStartHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            fogHook(rdram, ctx, runtime, 0);
        }

        void fogEndHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            fogHook(rdram, ctx, runtime, 1);
        }

        // The game moves the fog to end at the far clip (which has the ratio
        // already) and start `depth` before it: the depth times the camera's ratio.
        void moveClipRangeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            for (const CameraFog &e : distance().cameraFog)
                if (e.camera == getRegU32(ctx, kRegA0))
                    ctx->f[kFpA0] *= e.ratio;
            distance().moveClipRange(rdram, ctx, runtime);
        }

        float lodScaled(float end, float ratio)
        {
            return (std::isfinite(end) && end > 0.0f && end < 1.0e6f) ? end * ratio : end;
        }

        void lodTableScale(uint8_t *rdram, uint32_t table)
        {
            Distance &d = distance();
            uint32_t head = 0;
            if (!table || !readU32(rdram, table, head))
                return;
            const uint32_t count = (head >> 8) & 0xFu;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t record = 0;
                float end = 0.0f;
                if (!readU32(rdram, table + 0x18u + 4u * i, record) || !record || !readFloat(rdram, record + 4u, end))
                    continue;
                const uint32_t at = record + 4u;
                auto it = d.lodEnds.find(at);
                if (it != d.lodEnds.end() && it->second.written == end)
                    continue; // ours, at the current ratio
                LodEnd e;
                e.original = end;
                e.written = lodScaled(end, d.lodRatio);
                if (e.written != end)
                    writeFloat(rdram, at, e.written);
                d.lodEnds[at] = e;
            }
        }

        void setLodTableHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t node = getRegU32(ctx, kRegA0);
            distance().setLodTable(rdram, ctx, runtime);
            uint32_t table = 0;
            if (readU32(rdram, node + distance().table.nodeLods, table))
                lodTableScale(rdram, table);
        }

        // The ratio the LOD ends should have now: the largest far clip ratio,
        // whichever renderer draws (the game's state follows the draw
        // distance alone). Changed: every end again. A recording made while
        // the ends followed it only with native draws keeps whether they
        // did (the channel); a recording now keeps that they do.
        void lodRatioUpdate(uint8_t *rdram, float farRatio)
        {
            Distance &d = distance();
            const bool follows = play_recording::read(play_recording::Channel::NativeDraws, true);
            const float want = d.lodFollow && follows ? farRatio : 1.0f;
            if (want == d.lodRatio)
                return;
            d.lodRatio = want;
            size_t moved = 0;
            for (auto it = d.lodEnds.begin(); it != d.lodEnds.end();)
            {
                float now = 0.0f;
                if (!readFloat(rdram, it->first, now) || now != it->second.written)
                {
                    it = d.lodEnds.erase(it);
                    continue;
                }
                it->second.written = lodScaled(it->second.original, want);
                writeFloat(rdram, it->first, it->second.written);
                ++moved;
                ++it;
            }
            if (d.trace)
                std::fprintf(stderr, "[draw-distance] LOD ends x%.2f (%zu)\n", want, moved);
        }

        void lodRatioFromCamera(uint8_t *rdram, uint32_t camera, float far, float ratio)
        {
            Distance &d = distance();
            ++d.lodTick;
            LodCamera *slot = nullptr;
            for (LodCamera &c : d.lodCameras)
                if (c.camera == camera)
                    slot = &c;
            if (!slot)
            {
                slot = &d.lodCameras[0];
                for (LodCamera &c : d.lodCameras)
                    if (c.tick < slot->tick)
                        slot = &c;
            }
            *slot = LodCamera{camera, far, ratio, d.lodTick};
            const LodCamera *best = slot;
            for (const LodCamera &c : d.lodCameras)
                if (c.camera && c.tick + 16u > d.lodTick && c.far > best->far)
                    best = &c;
            lodRatioUpdate(rdram, best->ratio);
        }

        // The copier: its far clips are another camera's, set already.
        void copiesFarClipHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Distance &d = distance();
            const bool outer = d.inCopy;
            d.inCopy = true;
            d.copiesFarClip(rdram, ctx, runtime);
            d.inCopy = outer;
        }

        void farClipHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Distance &d = distance();
            if (d.inCopy)
            {
                zge_host::sites::seen(d.table.copySite);
                d.setFarClip(rdram, ctx, runtime);
                return;
            }
            const int setting = play_recording::read(play_recording::Channel::DrawDistance, runtime->drawDistance());
            const int level = std::clamp(setting, 0, 2);
            const uint32_t camera = getRegU32(ctx, kRegA0);
            const float far = ctx->f[kFpA0];
            const float maxFar = d.table.maxFarClip[level];
            float ratio = 1.0f;
            float beyond = 0.0f; // a far clip past the game's cap, stored here
            if (level != 0 && far > 0.0f && far < maxFar)
            {
                const float scaled = std::min(far * d.table.factor[level], maxFar);
                ratio = scaled / far;
                ctx->f[kFpA0] = std::min(scaled, kGameMaxFarClip);
                if (scaled > kGameMaxFarClip)
                    beyond = scaled;
            }
            if (beyond > 0.0f)
            {
                // setCameraFarPlane without its cap: the value, and the
                // camera's changed flag set if it changed, cleared if not;
                // then returned for it, as its jr ra would: the caller goes on
                // only when pc is its return address (else the dispatcher runs
                // setCameraFarPlane again, for ever).
                ctx->pc = getRegU32(ctx, kRegRa);
                uint8_t *flagsPtr = getMemPtr(rdram, camera + kCameraFlags);
                uint8_t *farPtr = getMemPtr(rdram, camera + kCameraFar);
                if (!flagsPtr || !farPtr)
                    return;
                float stored = 0.0f;
                std::memcpy(&stored, farPtr, sizeof(stored));
                std::memcpy(farPtr, &beyond, sizeof(beyond));
                uint64_t flags = 0;
                std::memcpy(&flags, flagsPtr, sizeof(flags));
                flags = (flags & ~kCameraFarChanged) | (stored != beyond ? kCameraFarChanged : 0u);
                std::memcpy(flagsPtr, &flags, sizeof(flags));
            }
            else
                d.setFarClip(rdram, ctx, runtime);
            // The fog follows when the ratio changes (also back to the game's own).
            CameraFog *entry = nullptr;
            for (CameraFog &e : d.cameraFog)
                if (e.camera == camera)
                    entry = &e;
            lodRatioFromCamera(rdram, camera, far * ratio, ratio);
            if (!entry && ratio == 1.0f)
                return;
            if (!entry)
                entry = &cameraFog(camera);
            if (entry->ratio == ratio)
                return;
            entry->ratio = ratio;
            if (!entry->known[0] || !entry->known[1])
            {
                if (d.trace)
                    std::fprintf(stderr, "[draw-distance] camera 0x%x: far %.0f -> %.0f\n", camera, far, far * ratio);
                return;
            }
            const float want[2] = {entry->fog[0] * ratio, entry->fog[1] * ratio};
            uint8_t *flagsPtr = getMemPtr(rdram, camera + kCameraFlags);
            if (!flagsPtr || !writeFloats(rdram, camera + kCameraFogStart, want))
                return;
            uint64_t flags = 0;
            std::memcpy(&flags, flagsPtr, sizeof(flags));
            flags |= kCameraFogChanged;
            std::memcpy(flagsPtr, &flags, sizeof(flags));
            if (d.trace)
                std::fprintf(stderr, "[draw-distance] camera 0x%x: far %.0f -> %.0f, fog %.0f-%.0f\n", camera, far,
                             far * ratio, want[0], want[1]);
        }
    }

    bool installDrawDistance(PS2Runtime &runtime, const DrawDistance &table)
    {
        Distance &d = distance();
        d.table = table;
        d.setFarClip = runtime.lookupFunction(table.setCameraFarPlane);
        d.setFogStart = table.setCameraFogStart ? runtime.lookupFunction(table.setCameraFogStart) : nullptr;
        d.setFogEnd = table.setCameraFogEnd ? runtime.lookupFunction(table.setCameraFogEnd) : nullptr;
        d.moveClipRange = table.moveCameraClipRange ? runtime.lookupFunction(table.moveCameraClipRange) : nullptr;
        d.copiesFarClip = table.copiesFarClip ? runtime.lookupFunction(table.copiesFarClip) : nullptr;
        d.setLodTable = runtime.lookupFunction(table.setLodTable);
        bool ok = true;
        if (table.copiesFarClip && (!d.copiesFarClip || !runtime.replaceFunction(table.copiesFarClip, &copiesFarClipHook)))
        {
            std::cerr << "[" << table.game << "] the far clip copier's hook not installed" << std::endl;
            ok = false;
        }
        if (!d.setLodTable || !runtime.replaceFunction(table.setLodTable, &setLodTableHook))
        {
            std::cerr << "[" << table.game << "] LOD distance hook not installed" << std::endl;
            ok = false;
        }
        const bool fogSetters = table.setCameraFogStart && table.setCameraFogEnd;
        if (!d.setFarClip || (fogSetters && (!d.setFogStart || !d.setFogEnd)) ||
            (table.moveCameraClipRange && !d.moveClipRange) ||
            !runtime.replaceFunction(table.setCameraFarPlane, &farClipHook) ||
            (fogSetters && (!runtime.replaceFunction(table.setCameraFogStart, &fogStartHook) ||
                            !runtime.replaceFunction(table.setCameraFogEnd, &fogEndHook))) ||
            (table.moveCameraClipRange && !runtime.replaceFunction(table.moveCameraClipRange, &moveClipRangeHook)))
        {
            std::cerr << "[" << table.game << "] draw distance hooks not installed" << std::endl;
            ok = false;
        }
        const char *trace = std::getenv("ZX_TRACE_DRAW_DISTANCE");
        d.trace = trace && trace[0] == '1';
        const char *lodDistance = std::getenv("ZX_LOD_DISTANCE");
        d.lodFollow = !(lodDistance && lodDistance[0] == '0');
        return ok;
    }
}

// ---- a camera's hidden twin (zge_view.h CameraTwin) -------------------------
namespace zge_host::view
{
    namespace
    {
        constexpr uint32_t kTwinStackBytes = 0x4000; // below the caller's sp
        constexpr uint32_t kRecordFromWorld = 0x70, kRecordToWorld = 0xB0;

        bool copyOut(const uint8_t *rdram, uint32_t address, uint32_t size, std::vector<uint8_t> &out)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            if (!p || size == 0u || getConstMemPtr(rdram, address + size - 1u) != p + size - 1u)
                return false;
            out.assign(p, p + size);
            return true;
        }
        void copyIn(uint8_t *rdram, uint32_t address, const std::vector<uint8_t> &in)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, in.data(), in.size());
        }
    }

    bool CameraTwin::track(const uint8_t *rdram, const std::vector<std::pair<uint32_t, uint32_t>> &blocks)
    {
        bool same = m_valid && m_blocks.size() == blocks.size();
        for (size_t i = 0; same && i < blocks.size(); ++i)
            same = m_blocks[i].address == blocks[i].first && m_blocks[i].size == blocks[i].second;
        std::vector<uint8_t> now;
        if (!same)
        {
            m_blocks.assign(blocks.size(), Block{});
            for (size_t i = 0; i < blocks.size(); ++i)
            {
                Block &b = m_blocks[i];
                b.address = blocks[i].first, b.size = blocks[i].second;
                if (!copyOut(rdram, b.address, b.size, b.hidden))
                {
                    reset();
                    return false;
                }
                b.playerAfter = b.hidden;
            }
            m_valid = true;
            return true;
        }
        for (Block &b : m_blocks)
        {
            if (!copyOut(rdram, b.address, b.size, now))
                continue;
            for (size_t i = 0; i < now.size(); ++i)
                if (now[i] != b.playerAfter[i])
                    b.hidden[i] = now[i];
        }
        return false;
    }

    void CameraTwin::run(uint8_t *rdram, R5900Context *ctx, const std::function<void()> &update)
    {
        if (!m_valid)
            return;
        std::vector<std::vector<uint8_t>> player(m_blocks.size());
        for (size_t i = 0; i < m_blocks.size(); ++i)
            if (!copyOut(rdram, m_blocks[i].address, m_blocks[i].size, player[i]))
                return;
        const R5900Context keptContext = *ctx;
        const uint32_t sp = getRegU32(ctx, kRegSp);
        std::vector<uint8_t> keptStack;
        const bool stackKept = copyOut(rdram, sp - kTwinStackBytes, kTwinStackBytes, keptStack);
        for (const Block &b : m_blocks)
            copyIn(rdram, b.address, b.hidden);
        {
            ee_time::Held held;
            update();
        }
        for (Block &b : m_blocks)
            copyOut(rdram, b.address, b.size, b.hidden);
        for (size_t i = 0; i < m_blocks.size(); ++i)
            copyIn(rdram, m_blocks[i].address, player[i]);
        if (stackKept)
            copyIn(rdram, sp - kTwinStackBytes, keptStack);
        *ctx = keptContext;
    }

    void CameraTwin::notePlayer(const uint8_t *rdram)
    {
        for (Block &b : m_blocks)
            copyOut(rdram, b.address, b.size, b.playerAfter);
    }

    void CameraTwin::notePlayerWrites(const uint8_t *rdram, uint32_t address, const std::vector<uint8_t> &before)
    {
        for (Block &b : m_blocks)
        {
            std::vector<uint8_t> now;
            if (b.address != address || before.size() != b.size || !copyOut(rdram, b.address, b.size, now))
                continue;
            for (size_t i = 0; i < now.size(); ++i)
                if (now[i] != before[i])
                    b.playerAfter[i] = now[i];
        }
    }

    std::vector<uint8_t> CameraTwin::bytesNow(const uint8_t *rdram, uint32_t address) const
    {
        std::vector<uint8_t> now;
        for (const Block &b : m_blocks)
            if (b.address == address && copyOut(rdram, b.address, b.size, now))
                return now;
        return {};
    }

    const CameraTwin::Block *CameraTwin::blockAt(uint32_t address) const
    {
        for (const Block &b : m_blocks)
            if (b.address == address)
                return &b;
        return nullptr;
    }

    void keepTwinPose(uint32_t cameraRecord, const std::vector<uint8_t> &record)
    {
        zx::game_pose::Pose pose{};
        if (record.size() < kRecordToWorld + sizeof(pose.toWorld))
            return;
        std::memcpy(&pose.toWorld, record.data() + kRecordToWorld, sizeof(pose.toWorld));
        std::memcpy(&pose.fromWorld, record.data() + kRecordFromWorld, sizeof(pose.fromWorld));
        zx::game_pose::keep(cameraRecord, pose);
    }

    void noteTwinPlayer(const uint8_t *rdram, uint32_t cameraRecord, uint32_t cameraObject)
    {
        float place[2][4];
        const uint8_t *p = getConstMemPtr(rdram, cameraObject + 0x10u); // SceneObject position, then orientation
        if (!p)
            return;
        std::memcpy(place, p, sizeof(place));
        zx::game_pose::notePlayer(cameraRecord, place);
    }

    void dropTwinPose(uint32_t cameraRecord)
    {
        zx::game_pose::drop(cameraRecord);
    }
}
