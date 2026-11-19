// BMX XXX (SLUS-20415): mouse-look (controls.ini [mouse] mode = look), done
// as Aggressive Inline's and Dave Mirra BMX 2's (aggressive_inline_slus20327.cpp,
// dave_mirra_mouselook.cpp): the follow camera's own wanted eye is turned, so
// the game's easing, its keeping off the rider and out of the walls, and
// everything that follows from the camera stay its own.
//
// Each frame the camera update (updateCameras, game/bmx/camera/update.cpp)
// runs each shown camera's mode (the table of mode functions), then moves its
// view towards what the mode wants: the eye on a spring towards the wanted
// eye eased (ActionCamera::easedEye, eyeVelocity), the look-at a share of the
// way to the wanted one, then kept off the rider and out of the walls, and
// the view aimed from eye to look-at. The follow mode (followCameraMode,
// game/bmx/camera/follow.cpp), behind a riding rider (its second level 0, or
// a swing with a camera type that does not swing), places the wanted eye at
// the camera type's offset in a level space at the rider facing along the
// velocity, and the wanted look-at at the rider.
//
// The hook wraps followCameraMode (it knows the camera by its argument, never
// by a return address). When it has placed player 1's wanted eye that way and
// mouse-look is active (PSPadBackend::mouseLookFrame), it orbits that eye
// about the wanted look-at by the mouse yaw and pitch (any yaw; pitch first,
// so it means the same at every yaw), so the update eases to the mouse-placed
// eye. On frames with mouse motion (look_snap) it also turns the current eye,
// the eased eye and the eye's velocity about the current look-at by that
// frame's motion, so the view follows the mouse at once while the game's
// easing still follows the rider. The rider's eyes (camera type 4: the view
// looks out from the rider), the bail, the view from above, the first-person
// and the swings are left to the game (mouse-look recentres while they run,
// as for every camera that does not ask for it).
// Context: a cut or a snap (ActionCamera::easing) recentres, as AI's snap
// flag does; while the game screen is not play (g_gameScreen: the pause menu
// and the screens from it, the round's and the game's end, a replay), motion
// is dropped and the angle kept. Only player 1's camera is turned: keyboard and mouse are port 0.
// Every other call gets exactly what the original does. Mouse-look is
// inactive until the mouse moves the view, so without mouse motion the game
// runs bit for bit as without the hook. PS2X_MOUSELOOK=0 leaves the camera
// alone; [mouse] mode = stick turns it off from controls.ini (or Settings >
// Controls > Mouse Look). PS2X_TRACE_MOUSELOOK=1 logs what it does.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_mouselook.cpp.h"
#include "zge_sites.h"

#include "ps2_runtime.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_session.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace bmx_xxx::mouselook
{
    namespace
    {
        namespace hi = ps2_host_input;

        // An action camera (game/bmx/camera/action_camera.h ActionCamera), as
        // the hook reads it.
        constexpr uint32_t kCameraKind = 0x01;   // 1: a rider's camera
        constexpr uint32_t kCameraType = 0x02;   // the player's camera type
        constexpr uint32_t kCameraEasing = 0x04; // CameraEasing
        constexpr uint32_t kCameraModes = 0x08;  // the mode at each level
        constexpr uint32_t kCameraEye = 0x30, kCameraWantedEye = 0x40, kCameraEasedEye = 0x50;
        constexpr uint32_t kCameraLookAt = 0x60, kCameraWantedLookAt = 0x70, kCameraEyeVelocity = 0x80;
        constexpr uint32_t kCameraRider = 0x170; // riderOf's index (its low byte)
        constexpr uint8_t kRiderCamera = 1;
        constexpr uint8_t kPlayerOneRider = 0;
        // The follow mode's second level (follow.cpp FollowKind) and the
        // camera types that do not swing, and the rider's eyes.
        constexpr uint8_t kFollowRiding = 0, kFollowSwingBehind = 1, kFollowSwingToSide = 2;
        constexpr uint8_t kTypeFarAhead = 1, kTypeNoSwing = 3, kTypeRiderEyes = 4;
        constexpr uint8_t kEasingEase = 0;       // the others put the view there at once
        // game/bmx/overlay/screens.h GameScreen: play (any other: a replay,
        // the pause menu and the screens over the level).
        constexpr uint32_t kScreenPlay = 1;
        constexpr int kRegA0 = 4;
        // The follow mode's space is level: its up is the world's, -y (as DM2's).
        const hi::Vec3 kUp{0.0f, -1.0f, 0.0f};

        struct Hooks
        {
            PS2Runtime::RecompiledFunction followCameraMode = nullptr;
            bool trace = false;
            uint32_t tracedCalls = 0;
        };
        Hooks &hooks() { return ps2x::session::state<Hooks>(); }

        bool readVec(const uint8_t *rdram, uint32_t address, hi::Vec3 &out)
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

        uint8_t readU8(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        uint32_t readU32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        hi::Vec3 sub(const hi::Vec3 &a, const hi::Vec3 &b) { return hi::Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }
        hi::Vec3 add(const hi::Vec3 &a, const hi::Vec3 &b) { return hi::Vec3{a.x + b.x, a.y + b.y, a.z + b.z}; }

        // The follow mode placed the wanted eye behind the riding rider this
        // frame (followCameraMode's dispatch: a swing with a camera type that
        // does not swing rides).
        bool placedBehind(const uint8_t *rdram, uint32_t camera)
        {
            const uint8_t type = readU8(rdram, camera + kCameraType);
            uint8_t kind = readU8(rdram, camera + kCameraModes + 1u);
            if ((kind == kFollowSwingBehind || kind == kFollowSwingToSide) && (type == kTypeFarAhead || type == kTypeNoSwing))
                kind = kFollowRiding;
            return kind == kFollowRiding && type != kTypeRiderEyes;
        }

        // Turns the current camera (eye, eased eye, eye velocity) about the
        // current look-at by this frame's mouse motion.
        void turnCurrentCamera(uint8_t *rdram, uint32_t camera, float yaw, float pitch)
        {
            hi::Vec3 eye, eased, look, velocity;
            if (!readVec(rdram, camera + kCameraEye, eye) || !readVec(rdram, camera + kCameraEasedEye, eased) ||
                !readVec(rdram, camera + kCameraLookAt, look) || !readVec(rdram, camera + kCameraEyeVelocity, velocity))
                return;
            const hi::Quat turn = hi::lookRotation(sub(eye, look), kUp, yaw, pitch);
            const hi::Vec3 newEye = add(look, hi::quatRotate(turn, sub(eye, look)));
            const hi::Vec3 newEased = add(look, hi::quatRotate(turn, sub(eased, look)));
            const hi::Vec3 newVelocity = hi::quatRotate(turn, velocity);
            if (hooks().trace)
                std::fprintf(stderr,
                             "[mouselook] turn yaw=%.2f pitch=%.2f eye=(%.3f %.3f %.3f) -> (%.3f %.3f %.3f) look=(%.3f %.3f %.3f)\n",
                             yaw, pitch, eye.x, eye.y, eye.z, newEye.x, newEye.y, newEye.z, look.x, look.y, look.z);
            writeVec(rdram, camera + kCameraEye, newEye);
            writeVec(rdram, camera + kCameraEasedEye, newEased);
            writeVec(rdram, camera + kCameraEyeVelocity, newVelocity);
        }

        void afterFollow(uint8_t *rdram, uint32_t camera)
        {
            if (readU8(rdram, camera + kCameraKind) != kRiderCamera ||
                readU8(rdram, camera + kCameraRider) != kPlayerOneRider || !placedBehind(rdram, camera))
                return;
            zge_host::sites::seen("bmx.mouselook.follow");
            hi::Vec3 eye, look;
            if (!readVec(rdram, camera + kCameraWantedEye, eye) || !readVec(rdram, camera + kCameraWantedLookAt, look))
                return;
            hi::Vec3 offset = sub(eye, look);
            const float basePitch = hi::viewPitchOfOffset(offset, kUp);
            using Context = hi::HostInputMapper::LookContext;
            const uint32_t screen = readU32(rdram, sym::bmx::g_gameScreen);
            const uint8_t easing = readU8(rdram, camera + kCameraEasing);
            Context context = Context::Play;
            if (easing != kEasingEase)
                context = Context::Cut;
            else if (screen != kScreenPlay)
                context = Context::Paused;
            const hi::LookFrame frame = PSPadBackend::mouseLookFrame(basePitch, context);
            if (frame.snapYaw != 0.0f || frame.snapPitch != 0.0f)
                turnCurrentCamera(rdram, camera, frame.snapYaw, frame.snapPitch);
            if (hooks().trace && (frame.active || (hooks().tracedCalls++ % 60u) == 0u))
                std::fprintf(stderr,
                             "[mouselook] camera=0x%x screen=%u easing=%u ctx=%d base=%.2f yaw=%.2f pitch=%.2f active=%d "
                             "want=(%.3f %.3f %.3f) look=(%.3f %.3f %.3f)\n",
                             camera, screen, easing, static_cast<int>(context), basePitch, frame.yaw, frame.pitch,
                             frame.active ? 1 : 0, eye.x, eye.y, eye.z, look.x, look.y, look.z);
            if (!frame.active)
                return;
            if (!hi::orbitCameraOffset(offset, kUp, frame.yaw, frame.pitch, frame.upLimit, frame.downLimit))
                return;
            writeVec(rdram, camera + kCameraWantedEye, add(look, offset));
        }

        void followCameraModeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t camera = getRegU32(ctx, kRegA0);
            hooks().followCameraMode(rdram, ctx, runtime);
            afterFollow(rdram, camera);
        }
    }
}

namespace bmx_xxx
{
    bool installMouseLook(PS2Runtime &runtime)
    {
        using namespace mouselook;
        if (const char *v = std::getenv("PS2X_MOUSELOOK"); v && v[0] == '0')
            return false;
        Hooks &h = hooks();
        h.followCameraMode = runtime.lookupFunction(sym::bmx::followCameraMode);
        if (!h.followCameraMode || !runtime.replaceFunction(sym::bmx::followCameraMode, &followCameraModeHook))
        {
            std::cerr << "[bmx-xxx] mouse-look hook not installed" << std::endl;
            return false;
        }
        const char *trace = std::getenv("PS2X_TRACE_MOUSELOOK");
        h.trace = trace && trace[0] == '1';
        return true;
    }
}
