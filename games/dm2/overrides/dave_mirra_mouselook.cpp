// Dave Mirra Freestyle BMX 2 (SLUS-20159): mouse-look (controls.ini [mouse]
// mode = look), done as Aggressive Inline's (aggressive_inline_slus20327.cpp):
// the follow camera's own wanted eye is turned, so the game's easing,
// collision and everything that follows from the camera stay its own.
//
// The follow camera (./src/com/camera.c, followCamera) keeps one state per
// player at 0x2BC070 + i * 0xC0:
//   +0x03 camera mode, +0x04 frames in it, +0x0A cut (1: the next update
//   puts the camera at the wanted one at once), +0x0B flags (1: no minimum
//   distance, 2: no collision),
//   +0x10 eye, +0x20 wanted eye, +0x30 eye chaser, +0x40 look-at,
//   +0x50 wanted look-at, +0x60 eye velocity, +0x70 / +0x74 field of view,
//   +0x80 / +0x90 / +0xA0 the camera's axes (built from eye and look-at).
// Each update the mode's code sets the wanted look-at (the rider's position
// +0x1040 plus offsets) and the wanted eye; for the ride modes the eye is an
// offset (*(sp+0xF8)) placed in a frame at the rider that faces where the
// rider goes, by transformPoint(out = state+0x20, frame, offset) called from
// 0x169A98: every camera type of the Options menu (Wide, Behind, Above;
// state +0x02) comes there in play (seen in Freeride). Two other modes place
// the wanted eye with transformPoint from 0x169190 and 0x16944C; they were
// not seen in play and are left to the game (mouse-look recentres while
// they run, as for every camera that does not ask for it). From there
// all paths meet at 0x169B7C, which eases: the look-at towards the wanted
// one, the chaser towards the wanted eye, the eye on a spring towards the
// chaser (velocity +0x60), then keeps it a minimum distance from the rider,
// pushes it out of the world (sphereQuery ...) and checks the line of sight.
// The camera record at 0x2BC1F0 + i * 0x70 (eye, look-at, axes, field of
// view) then goes to the engine camera (applyCameraState): projection, frustum
// and everything drawn come from that eye and look-at.
//
// The hook wraps transformPoint. Called from the follow site with mouse-look
// active (PSPadBackend::mouseLookFrame), it orbits the wanted eye the game
// just placed about the wanted look-at by the mouse yaw/pitch (any yaw;
// pitch first, so it means the same at every yaw), so the easing, minimum
// distance, collision and line of sight all work on the mouse-placed eye.
// On frames with mouse motion (look_snap) it also turns the current eye,
// the chaser and the eye velocity about the current look-at by that frame's
// motion, so the view follows the mouse at once while the game's easing
// still follows the rider. World y points down, so "up" is (0, -1, 0), the
// up the camera code itself builds its frames with.
// Context: a cut (+0x0A set) recentres, as AI's snap flag does; while an
// overlay is up (the pause menu and the other in-level menus, 0x25BBE4; the
// level and its camera keep running behind them) motion is dropped and the
// angle kept. Only player 1's camera (state 0) is turned:
// keyboard and mouse are port 0. Every other call gets exactly what the
// original does. Mouse-look is inactive until the mouse moves the view, so
// without mouse motion the game runs bit for bit as without the hook.
// ZX_MOUSELOOK=0 leaves the camera alone; [mouse] mode = stick turns it
// off from controls.ini (or Settings > Controls > Mouse Look).
#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_mouselook.cpp.h"
#include "overrides/zge_sites.h"

#include "machine.h"
#include "runtime/boot_config.h"
#include "runtime/host_input.h"
#include "runtime/pad.h"
#include "runtime/state_probe.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace dave_mirra::mouselook
{
    namespace hi = ps2_host_input;

    constexpr uint32_t kTransform = sym::dm2::transformPoint; // out(a0) = vector(a2) placed by the 4x4 rows at a1
    // The follow camera (followCamera in games/dm2/camera/
    // follow_camera.cpp), wrapped: its ride modes' wanted eye is the
    // transform it makes into player 1's wanted eye while it runs (known by
    // that and the call's out argument, never by a return address or the
    // caller's registers, which a readable caller does not keep).
    constexpr uint32_t kFollowCamera = sym::dm2::followCamera;
    constexpr uint32_t kStates = sym::dm2::g_playerCameras; // player 1 (each player 0xC0 bytes on)
    constexpr uint32_t kStateMode = 0x03, kStateCut = 0x0A, kStateFlags = 0x0B;
    constexpr uint32_t kStateEye = 0x10, kStateWantedEye = 0x20, kStateChaser = 0x30;
    constexpr uint32_t kStateLook = 0x40, kStateWantedLook = 0x50, kStateVelocity = 0x60;
    // The overlay over the level (showGameScreen sets it; 0 = none, 1 = the
    // pause menu, others the round-over and goal menus). The level and its
    // camera keep running behind an overlay.
    constexpr uint32_t kOverlay = sym::dm2::g_gameScreen;
    // The camera record the engine camera is set from (applyCameraState):
    // 0x2BC1F0 + i * 0x70, eye at +0x10, look-at at +0x20 (trace only).
    constexpr uint32_t kRecord = sym::dm2::g_cameraStates, kRecordEye = 0x10, kRecordLook = 0x20;
    constexpr int kRegA0 = 4;
    const hi::Vec3 kUp{0.0f, -1.0f, 0.0f};

    // One boot's original and trace switch (made at the boot's first use,
    // the install: zx::boot::PerBoot).
    struct Hooks
    {
        PS2Machine::RecompiledFunction transform = nullptr;
        PS2Machine::RecompiledFunction followCamera = nullptr;
        bool inFollowCamera = false;
        bool trace = false;
        uint32_t tracedCalls = 0;
    };

    Hooks &hooks()
    {
        static zx::boot::PerBoot<Hooks> ofBoot([] { return Hooks{}; });
        return ofBoot.get();
    }

    bool readVec(uint8_t *rdram, uint32_t address, hi::Vec3 &out)
    {
        const uint8_t *p = getConstMemPtr(rdram, address);
        if (!p || getConstMemPtr(rdram, address + 11u) != p + 11u)
            return false;
        float v[3];
        std::memcpy(v, p, sizeof(v));
        for (float f : v)
            if (!std::isfinite(f))
                return false;
        out = hi::Vec3{v[0], v[1], v[2]};
        return true;
    }

    bool writeVec(uint8_t *rdram, uint32_t address, const hi::Vec3 &in)
    {
        uint8_t *p = getMemPtr(rdram, address);
        if (!p || getMemPtr(rdram, address + 11u) != p + 11u)
            return false;
        const float v[3] = {in.x, in.y, in.z};
        std::memcpy(p, v, sizeof(v));
        return true;
    }

    uint8_t readU8(uint8_t *rdram, uint32_t address)
    {
        const uint8_t *p = getConstMemPtr(rdram, address);
        return p ? *p : 0u;
    }

    hi::Vec3 sub(const hi::Vec3 &a, const hi::Vec3 &b) { return hi::Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
    hi::Vec3 add(const hi::Vec3 &a, const hi::Vec3 &b) { return hi::Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }

    // Turns the current camera (eye, chaser, eye velocity) about the
    // current look-at by this frame's mouse motion.
    void turnCurrentCamera(uint8_t *rdram, uint32_t state, float yaw, float pitch)
    {
        hi::Vec3 eye, chaser, look, velocity;
        if (!readVec(rdram, state + kStateEye, eye) || !readVec(rdram, state + kStateChaser, chaser) ||
            !readVec(rdram, state + kStateLook, look) || !readVec(rdram, state + kStateVelocity, velocity))
            return;
        const hi::Quat turn = hi::lookRotation(sub(eye, look), kUp, yaw, pitch);
        const hi::Vec3 newEye = add(look, hi::quatRotate(turn, sub(eye, look)));
        const hi::Vec3 newChaser = add(look, hi::quatRotate(turn, sub(chaser, look)));
        const hi::Vec3 newVelocity = hi::quatRotate(turn, velocity);
        if (hooks().trace)
            std::fprintf(stderr,
                         "[mouselook] turn yaw=%.2f pitch=%.2f eye=(%.3f %.3f %.3f) -> (%.3f %.3f %.3f) look=(%.3f %.3f %.3f)\n",
                         yaw, pitch, eye.x, eye.y, eye.z, newEye.x, newEye.y, newEye.z, look.x, look.y, look.z);
        writeVec(rdram, state + kStateEye, newEye);
        writeVec(rdram, state + kStateChaser, newChaser);
        writeVec(rdram, state + kStateVelocity, newVelocity);
    }

    void atFollowSite(uint8_t *rdram, R5900Context *ctx)
    {
        const uint32_t out = getRegU32(ctx, kRegA0);
        const uint32_t state = kStates;
        // Only player 1's camera: its wanted eye.
        if (out != state + kStateWantedEye)
            return;
        zge_host::sites::seen("dm2.mouselook.follow");
        hi::Vec3 eye, look;
        if (!readVec(rdram, state + kStateWantedEye, eye) || !readVec(rdram, state + kStateWantedLook, look))
            return;
        hi::Vec3 offset = sub(eye, look);
        const float basePitch = hi::viewPitchOfOffset(offset, kUp);
        using Context = hi::HostInputMapper::LookContext;
        Context context = Context::Play;
        const uint8_t overlay = readU8(rdram, kOverlay);
        if (readU8(rdram, state + kStateCut) != 0u)
            context = Context::Cut;
        else if (overlay != 0u)
            context = Context::Paused;
        const hi::LookFrame frame = PSPadBackend::mouseLookFrame(basePitch, context);
        if (frame.snapYaw != 0.0f || frame.snapPitch != 0.0f)
            turnCurrentCamera(rdram, state, frame.snapYaw, frame.snapPitch);
        if (hooks().trace)
        {
            // The record holds the camera of the previous update (what is
            // drawn now).
            hi::Vec3 recEye, recLook;
            if ((frame.active || (hooks().tracedCalls++ % 60u) == 0u) && readVec(rdram, kRecord + kRecordEye, recEye) &&
                readVec(rdram, kRecord + kRecordLook, recLook))
                std::fprintf(stderr,
                             "[mouselook] script=%lldv mode=%u cut=%u flags=0x%x overlay=%u ctx=%d base=%.2f yaw=%.2f "
                             "pitch=%.2f active=%d want=(%.3f %.3f %.3f) look=(%.3f %.3f %.3f) "
                             "drawn=(%.3f %.3f %.3f) (%.3f %.3f %.3f)\n",
                             static_cast<long long>(StateProbe::scriptTime()),
                             readU8(rdram, state + kStateMode), readU8(rdram, state + kStateCut),
                             readU8(rdram, state + kStateFlags), overlay, static_cast<int>(context), basePitch,
                             frame.yaw, frame.pitch, frame.active ? 1 : 0, eye.x, eye.y, eye.z, look.x, look.y,
                             look.z, recEye.x, recEye.y, recEye.z, recLook.x, recLook.y, recLook.z);
        }
        if (!frame.active)
            return;
        if (!hi::orbitCameraOffset(offset, kUp, frame.yaw, frame.pitch, frame.upLimit, frame.downLimit))
            return;
        writeVec(rdram, state + kStateWantedEye, add(look, offset));
    }

    void followCameraHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        hooks().inFollowCamera = true;
        hooks().followCamera(rdram, ctx, runtime);
        hooks().inFollowCamera = false;
    }

    void transform(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
    {
        hooks().transform(rdram, ctx, runtime);
        if (hooks().inFollowCamera)
            atFollowSite(rdram, ctx);
    }
}

namespace dave_mirra
{
    bool installMouseLook(PS2Machine &runtime)
    {
        using namespace mouselook;
        Hooks &h = hooks();
        if (const char *v = std::getenv("ZX_MOUSELOOK"); v && v[0] == '0')
            return false;
        h.transform = runtime.lookupFunction(kTransform);
        h.followCamera = runtime.lookupFunction(kFollowCamera);
        if (!h.transform || !h.followCamera || !runtime.replaceFunction(kTransform, &transform) ||
            !runtime.replaceFunction(kFollowCamera, &followCameraHook))
        {
            std::cerr << "[dave-mirra] mouse-look hook not installed" << std::endl;
            return false;
        }
        const char *trace = std::getenv("ZX_TRACE_MOUSELOOK");
        h.trace = trace && trace[0] == '1';
        return true;
    }
}
