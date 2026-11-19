// Game override: Aggressive Inline (USA), SLUS-20327, ELF CRC32 0xA8BE0B5B.
//
// Address bindings here are only valid for this exact ELF build. They cover
// SDK functions the Ghidra/SCE signature pass did not name, plus any
// game-specific behaviour. See docs/BRINGUP.md in the bring-up workspace.
#include "aggressive_inline_native.h"
#include "aggressive_inline_zoo.h"
#include "aggressive_inline_estate.h"
#include "aggressive_inline_panel.h"
#include "aggressive_inline_gsmem.h"
#include "aggressive_inline_devstart.h"
#include "aggressive_inline_ape.h"
#include "aggressive_inline_tuning.h"
#include "aggressive_inline_freeskate.h"
#include "aggressive_inline_careerscripts.h"
#include "aggressive_inline_devtools.h"
#include "aggressive_inline_language.h"
#include "aggressive_inline_disc.h"
#include "aggressive_inline_gameplay.h"
#include "aggressive_inline_movies.h"
#include "aggressive_inline_sound.h"
#include "aggressive_inline_saves.h"
#include "zge_view.h"
#include "zge_sites.h"
#include "zge_xbox_textures.h"
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_texture_replace.h"
#include "runtime/ee_time.h"
#include "../game/zge/host/game_pose.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// Collision on host data (game/zge/native/collision_data.h): AI's level-setup step
// (game/aggressive/level/collision_setup_host.h), declared here as the game's other
// host entry points are (aggressive_inline_native.cpp).
namespace zge::native
{
    struct HostCollisionPart;
    using LevelSetupStep = void (*)(HostCollisionPart &part);
    void setLevelSetupStep(LevelSetupStep step);
}
namespace aggressive
{
    void applyLevelSetupOnHost(zge::native::HostCollisionPart &part);
}

namespace ps2_aggressive_inline
{
    // Settings screen in the game's menus (aggressive_inline_settings.cpp).
    void installSettingsMenus(PS2Runtime &runtime);
}

namespace game::readable
{
    // Hand-written C++ in place of recompiled functions (src/game/ee/readable.h).
    void install(PS2Runtime &runtime);
}

namespace
{
    struct Binding
    {
        uint32_t address;
        const char *handler;
        const char *why;
    };

    constexpr Binding kBindings[] = {
        // libcdvd sceCdDiskReady(mode): binds SIF RPC 0x8000059A (cdvdfsv), which
        // lives in the IOP BIOS and is not provided by iop/. Identified from the
        // SID and the `mode == 8` (non-blocking) check. Callers at 0x116ED0 etc.
        {0x002144A0u, "sceCdDiskReady", "libcdvd DISKREADY RPC"},
        // libmpeg accessors the SCE scan did not name (they sit between
        // sceMpegGetDecodeMode and sceMpegReset). Recompiled, they read
        // decoder-internal state that only the real decoder updates; the
        // MPEG HLE tracks it instead. Without these the movie thread never
        // sees the end of a movie and the intro chain stops after AKA.
        {0x00207D78u, "sceMpegIsEnd", "returns mp->sys->isEnd"},
        {0x00207D88u, "sceMpegIsRefBuffEmpty", "returns mp->sys->refCount == 0"},
    };

    // ---- mouse-look (controls.ini [mouse] mode = look) ----------------------
    //
    // The follow camera (FUN_0018E858, camera object at 0x24C9D0) builds the
    // eye it wants each frame: look-at + skater up * height - motion
    // direction * distance, at sp+0x40 ($s0). Then it reads the right stick
    // through the accessor 0x18A260(axis, controller) at 0x18EFB0 (RX) and
    // 0x18F048 (RY) and, for a push past 1e-5, rotates that eye about the
    // look-at ($s1 = camera+0x20): RX * 60 deg about the skater's up axis
    // (player+0xC0), RY * 39.9 deg about its right axis (player+0xB0). Then
    // come the wall/floor collision (0x18D678) and FUN_0018CC50, which eases
    // the camera towards the result: it slerps the camera node's orientation
    // (quaternion x y z w at node+0x20, node at camera+0x0; matrix rows =
    // camera axes in world space, row 2 = view direction) and moves the eye
    // (camera+0x10) part of the way, unless the snap flag camera+0x72 is set.
    //
    // The hook wraps the accessor. Called from the RX site with mouse-look
    // active (PSPadBackend::mouseLookFrame), it orbits the wanted eye by the
    // mouse yaw/pitch itself (any yaw; pitch first, so it means the same at
    // every yaw) and returns 0 for RX and RY, so the game's own stick
    // rotation is skipped and collision and easing work on the mouse-placed
    // eye. On frames with mouse motion it also turns the current camera
    // (eye and node orientation) about the look-at by that frame's motion,
    // so the view follows the mouse at once while the game's easing still
    // follows the skater as before. (The snap flag would also remove the
    // follow lag, and the view would jump when the mouse starts moving in a
    // turn.) Looking up pulls the camera in rather than into the floor
    // (kMaxDrop). Mouse motion while the game is paused or shows a message
    // box (the camera keeps running behind it) is dropped; a scripted camera
    // (0x277488) or a camera cut by the game (snap flag) recentres the view.
    // Every other call (other callers, other camera modes, mouse-look
    // inactive) gets exactly what the original returns, with the original's
    // register effects. Mouse-look is inactive until the mouse moves the
    // view, so without mouse motion the game runs bit for bit as without
    // the hook.
    constexpr uint32_t kStickAccessor = 0x0018A260u;
    // The follow camera's frame (FUN_0018E858, readable updateFollowCamera),
    // whose look around reads the right stick's axes RX (a0 = 2) and RY (3).
    constexpr uint32_t kFollowCamera = 0x0018E858u;
    constexpr uint32_t kAxisRx = 2u, kAxisRy = 3u;
    constexpr uint32_t kCameraEyeFrame = 0x40;  // wanted eye on the camera update's stack
    constexpr uint32_t kCameraNode = 0x00;
    constexpr uint32_t kCameraPlayer = 0x04;
    constexpr uint32_t kCameraEye = 0x10;
    constexpr uint32_t kCameraLookAt = 0x20;
    constexpr uint32_t kNodeRotation = 0x20;
    constexpr uint32_t kPlayerUp = 0xC0;
    constexpr uint32_t kPlayerState = 0x74;
    // Game mode: 2 = play, 3 = paused or a message box (tip, HINT), with the
    // camera update still running behind the menu.
    constexpr uint32_t kGameMode = 0x00276FE4u;
    constexpr uint32_t kModePlay = 2u;
    constexpr uint32_t kModePaused = 3u;
    // 1 while a scripted camera is on (MOVIE LOT fly-thru, Tutorial lesson
    // intros: the follow camera may still run, the scripted one wins).
    constexpr uint32_t kScriptedCamera = 0x00277488u;
    constexpr uint32_t kCameraSnap = 0x72; // set by the game for a camera cut
    // Looking up lowers the camera towards the floor; the game's collision
    // (0x18D678) keeps it about 0.57 below the look-at on flat ground (the
    // look-at is ~1.5 above the feet) and the view then bounces. So the hook
    // pulls the camera in instead, keeping the eye at most this far below
    // the look-at, but no nearer than kMinDistance.
    constexpr float kMaxDrop = 0.45f;
    constexpr float kMinDistance = 1.5f;
    constexpr int kRegA0 = 4, kRegSp = 29;

    struct MouseLook
    {
        PS2Runtime::RecompiledFunction stickAccessor = nullptr;
        PS2Runtime::RecompiledFunction followCamera = nullptr;
        uint32_t followCameraRunning = 0; // updateFollowCamera's camera while it runs, else 0
        bool mouseLookThisFrame = false; // the RX read handed the camera to the mouse
        bool viewTurned = false;         // mouse-look has turned the view (the game's camera is the hidden one since)
        bool traceMouseLook = false;
    };
    MouseLook &mouseLookState() { return ps2x::session::state<MouseLook>(); }

    template <size_t N>
    bool readFloats(uint8_t *rdram, uint32_t address, float (&out)[N])
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

    bool readVec(uint8_t *rdram, uint32_t address, ps2_host_input::Vec3 &out)
    {
        float v[3];
        if (!readFloats(rdram, address, v))
            return false;
        out = ps2_host_input::Vec3{v[0], v[1], v[2]};
        return true;
    }

    bool writeVec(uint8_t *rdram, uint32_t address, const ps2_host_input::Vec3 &in)
    {
        const float v[3] = {in.x, in.y, in.z};
        return writeFloats(rdram, address, v);
    }

    bool readU32(uint8_t *rdram, uint32_t address, uint32_t &out)
    {
        const uint8_t *p = getConstMemPtr(rdram, address);
        if (!p || (address & 3u) != 0u)
            return false;
        std::memcpy(&out, p, sizeof(out));
        return true;
    }

    float dropBelow(const ps2_host_input::Vec3 &offset, const ps2_host_input::Vec3 &up)
    {
        const float ul = std::sqrt(up.x * up.x + up.y * up.y + up.z * up.z);
        if (!(ul > 0.0f))
            return 0.0f;
        return -(offset.x * up.x + offset.y * up.y + offset.z * up.z) / ul;
    }

    // Turns the current camera (eye and node orientation) about the look-at
    // by this frame's mouse motion.
    void turnCurrentCamera(uint8_t *rdram, uint32_t camera, const ps2_host_input::Vec3 &look,
                           const ps2_host_input::Vec3 &up, float yaw, float pitch)
    {
        uint32_t node = 0;
        ps2_host_input::Vec3 eye;
        float q[4];
        if (!readU32(rdram, camera + kCameraNode, node) || !node || !readVec(rdram, camera + kCameraEye, eye) ||
            !readFloats(rdram, node + kNodeRotation, q))
            return;
        const ps2_host_input::Vec3 offset{eye.x - look.x, eye.y - look.y, eye.z - look.z};
        const ps2_host_input::Quat turn = ps2_host_input::lookRotation(offset, up, yaw, pitch);
        ps2_host_input::Vec3 turned = ps2_host_input::quatRotate(turn, offset);
        ps2_host_input::limitCameraDrop(turned, up, kMaxDrop, kMinDistance, dropBelow(offset, up));
        const ps2_host_input::Quat orientation =
            ps2_host_input::quatMultiply(turn, ps2_host_input::Quat{q[0], q[1], q[2], q[3]});
        const float out[4] = {orientation.x, orientation.y, orientation.z, orientation.w};
        if (mouseLookState().traceMouseLook)
            std::fprintf(stderr,
                         "[mouselook] turn node=0x%x eye=(%.3f %.3f %.3f) look=(%.3f %.3f %.3f) q=(%.4f %.4f %.4f %.4f) "
                         "-> eye=(%.3f %.3f %.3f) q=(%.4f %.4f %.4f %.4f)\n",
                         node, eye.x, eye.y, eye.z, look.x, look.y, look.z, q[0], q[1], q[2], q[3], look.x + turned.x,
                         look.y + turned.y, look.z + turned.z, out[0], out[1], out[2], out[3]);
        writeVec(rdram, camera + kCameraEye, ps2_host_input::Vec3{look.x + turned.x, look.y + turned.y, look.z + turned.z});
        writeFloats(rdram, node + kNodeRotation, out);
    }

    // RX site: returns true when mouse-look placed the eye this frame.
    bool mouseLookAtRxSite(uint8_t *rdram, R5900Context *ctx)
    {
        // The follow camera's (updateFollowCamera, running: its camera) look
        // around: the wanted eye is its frame's local (the original's and
        // the readable one's at the same place), the look-at the camera's.
        const uint32_t camera = mouseLookState().followCameraRunning;
        const uint32_t eyeAddr = getRegU32(ctx, kRegSp) + kCameraEyeFrame;
        const uint32_t lookAddr = camera + kCameraLookAt;
        if (!camera)
            return false;
        uint32_t player = 0;
        ps2_host_input::Vec3 eye, look, up;
        if (!readU32(rdram, camera + kCameraPlayer, player) || !player || !readVec(rdram, eyeAddr, eye) ||
            !readVec(rdram, lookAddr, look) || !readVec(rdram, player + kPlayerUp, up))
            return false;
        ps2_host_input::Vec3 offset{eye.x - look.x, eye.y - look.y, eye.z - look.z};
        const float basePitch = ps2_host_input::viewPitchOfOffset(offset, up);
        using Context = ps2_host_input::HostInputMapper::LookContext;
        uint32_t mode = 0, scripted = 0;
        const uint8_t *snapFlag = getConstMemPtr(rdram, camera + kCameraSnap);
        Context context = Context::Play;
        if ((readU32(rdram, kScriptedCamera, scripted) && scripted != 0u) || (snapFlag && *snapFlag != 0u))
            context = Context::Cut;
        else if (!readU32(rdram, kGameMode, mode) || mode != kModePlay)
            context = Context::Paused;
        const ps2_host_input::LookFrame frame = PSPadBackend::mouseLookFrame(basePitch, context);
        if (frame.snapYaw != 0.0f || frame.snapPitch != 0.0f)
        {
            turnCurrentCamera(rdram, camera, look, up, frame.snapYaw, frame.snapPitch);
            mouseLookState().viewTurned = true;
        }
        if (!frame.active)
            return false;
        mouseLookState().viewTurned = true;
        const float baseDrop = dropBelow(offset, up);
        if (!ps2_host_input::orbitCameraOffset(offset, up, frame.yaw, frame.pitch, frame.upLimit, frame.downLimit))
            return false;
        ps2_host_input::limitCameraDrop(offset, up, kMaxDrop, kMinDistance, baseDrop);
        if (!writeVec(rdram, eyeAddr, ps2_host_input::Vec3{look.x + offset.x, look.y + offset.y, look.z + offset.z}))
            return false;
        if (mouseLookState().traceMouseLook)
        {
            const uint8_t *state = getConstMemPtr(rdram, player + kPlayerState);
            std::fprintf(stderr,
                         "[mouselook] player=0x%x state=%u up=(%.3f %.3f %.3f) base=%.2f yaw=%.2f pitch=%.2f "
                         "eye=(%.3f %.3f %.3f) turn=(%.2f %.2f)\n",
                         player, state ? *state : 0u, up.x, up.y, up.z, basePitch, frame.yaw, frame.pitch,
                         look.x + offset.x, look.y + offset.y, look.z + offset.z, frame.snapYaw, frame.snapPitch);
        }
        return true;
    }

    // ---- the hidden follow camera ---------------------------------------------
    //
    // Camera Distance and Height change the follow camera's tuning, and
    // mouse-look turns it: the camera moves, and with it what the game
    // decides by its view (the regions out of view from the camera's area:
    // traffic stands still there; the placed effects' range; what is drawn).
    // So with either in use a hidden follow camera runs first each frame:
    // its own state (the FollowCamera record, the camera object's place and
    // the camera record, kept here between frames), the game's own tuning,
    // the pad's stick and no mouse-look, outside the game's EE time
    // (ee_time.h) and with every byte it writes put back (the records, the
    // EE's registers, the stack below it). Its pose is the game's own camera
    // (zge/host/game_pose.h keepGamePose, after the camera object's own update): the engine's culling, regions and
    // the effects' range go by it, while the player's camera runs after it
    // as before, with its wall collision, and is drawn. A change made to the
    // records between frames (a cut, a mode, a scripted eye) is made to the
    // hidden camera's too. Off (and nothing kept) while neither is in use.
    constexpr uint32_t kFollowCameraBytes = 0x80;
    constexpr uint32_t kFollowCameraDistance = 0x3C; // FollowCamera::distance
    constexpr uint32_t kSceneObjectBytes = 0x50, kSceneObjectScene = 0x38;
    constexpr uint32_t kSceneObjectPlace = 0x10; // position, then orientation
    // The camera object's update (updateObjectCamera: the camera record set from the object's place), which the
    // frame's stale objects run after the camera's update: the hidden camera's pose is the one it leaves.
    constexpr uint32_t kUpdateObjectCamera = 0x0015DEA8u;
    constexpr uint32_t kCameraRecordBytes = 0x510;
    constexpr uint32_t kCameraFromWorld = 0x70, kCameraToWorld = 0xB0; // fromWorld; right .. position
    constexpr uint32_t kHiddenStackBytes = 0x4000; // below the caller's sp
    constexpr int kRegSpHidden = 29;

    struct HiddenCamera
    {
        // Each kept block: the hidden camera's, and the player's as its run left it (to see what changed since).
        struct Block
        {
            uint32_t address = 0, size = 0;
            std::vector<uint8_t> hidden, playerAfter;
        };
        Block follow, object, camera;
        bool valid = false;
    };
    struct HiddenCameras
    {
        std::unordered_map<uint32_t, HiddenCamera> byFollowCamera;
        PS2Runtime::RecompiledFunction updateObjectCamera = nullptr;
        bool running = false; // the hidden camera is running
    };
    HiddenCameras &hiddenCameras() { return ps2x::session::state<HiddenCameras>(); }

    bool copyOut(const uint8_t *rdram, uint32_t address, uint32_t size, std::vector<uint8_t> &out)
    {
        const uint8_t *p = getConstMemPtr(rdram, address);
        if (!p || getConstMemPtr(rdram, address + size - 1u) != p + size - 1u)
            return false;
        out.assign(p, p + size);
        return true;
    }
    void copyIn(uint8_t *rdram, uint32_t address, const std::vector<uint8_t> &in)
    {
        if (uint8_t *p = getMemPtr(rdram, address))
            std::memcpy(p, in.data(), in.size());
    }

    // The block's hidden bytes with every byte the game changed since the
    // player's camera ran (`now` against playerAfter) taken from now.
    void takeChanges(HiddenCamera::Block &block, const std::vector<uint8_t> &now)
    {
        for (size_t i = 0; i < now.size(); ++i)
            if (now[i] != block.playerAfter[i])
                block.hidden[i] = now[i];
    }

    void runHiddenCamera(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t followCamera)
    {
        HiddenCamera &h = hiddenCameras().byFollowCamera[followCamera];
        uint32_t object = 0, camera = 0;
        if (!readU32(rdram, followCamera + kCameraNode, object) || !object || !readU32(rdram, object + kSceneObjectScene, camera) ||
            !camera)
            return;
        HiddenCamera::Block *blocks[3] = {&h.follow, &h.object, &h.camera};
        const uint32_t addresses[3] = {followCamera, object, camera};
        const uint32_t sizes[3] = {kFollowCameraBytes, kSceneObjectBytes, kCameraRecordBytes};
        std::vector<uint8_t> player[3];
        for (int i = 0; i < 3; ++i)
            if (!copyOut(rdram, addresses[i], sizes[i], player[i]))
                return;
        const bool same = h.valid && h.object.address == object && h.camera.address == camera;
        for (int i = 0; i < 3; ++i)
        {
            HiddenCamera::Block &b = *blocks[i];
            if (!same)
            {
                // From the player's camera as it is now (both are the game's at a level's start).
                b.address = addresses[i], b.size = sizes[i];
                b.hidden = player[i];
                b.playerAfter = player[i];
            }
            else
                takeChanges(b, player[i]);
        }
        if (!same)
        {
            // The camera's set-up (initFollowCamera) gave it the table's Camera / Distance: the hidden one starts at
            // the game's own.
            float game = 0.0f, now = 0.0f, distance = 0.0f;
            aggressive_inline::cameraDistance(rdram, game, now);
            std::memcpy(&distance, h.follow.hidden.data() + kFollowCameraDistance, sizeof(distance));
            if (now != 0.0f && distance == now)
                std::memcpy(h.follow.hidden.data() + kFollowCameraDistance, &game, sizeof(game));
        }
        h.valid = true;

        // The hidden run, everything it writes put back after.
        const R5900Context keptContext = *ctx;
        const uint32_t sp = getRegU32(ctx, kRegSpHidden);
        std::vector<uint8_t> keptStack;
        const bool stackKept = copyOut(rdram, sp - kHiddenStackBytes, kHiddenStackBytes, keptStack);
        for (int i = 0; i < 3; ++i)
            copyIn(rdram, addresses[i], blocks[i]->hidden);
        aggressive_inline::holdGameCameraTuning(rdram, true);
        {
            ee_time::Held held;
            MouseLook &m = mouseLookState();
            const uint32_t outer = m.followCameraRunning;
            m.followCameraRunning = 0; // (the stick's own look-around: no mouse-look)
            hiddenCameras().running = true;
            m.followCamera(rdram, ctx, runtime);
            // (the camera object's update after it, as the frame's stale objects run it: the record from its place)
            SET_GPR_U32(ctx, kRegA0, object);
            if (hiddenCameras().updateObjectCamera)
                hiddenCameras().updateObjectCamera(rdram, ctx, runtime);
            hiddenCameras().running = false;
            m.followCameraRunning = outer;
        }
        aggressive_inline::holdGameCameraTuning(rdram, false);
        for (int i = 0; i < 3; ++i)
            copyOut(rdram, addresses[i], sizes[i], blocks[i]->hidden);
        // Its pose: the game's own camera this frame (taken by the camera's frustum).
        zge::host::GamePose pose{};
        std::memcpy(&pose.toWorld, h.camera.hidden.data() + kCameraToWorld, sizeof(pose.toWorld));
        std::memcpy(&pose.fromWorld, h.camera.hidden.data() + kCameraFromWorld, sizeof(pose.fromWorld));
        zge::host::keepGamePose(camera, pose);
        for (int i = 0; i < 3; ++i)
            copyIn(rdram, addresses[i], player[i]);
        if (stackKept)
            copyIn(rdram, sp - kHiddenStackBytes, keptStack);
        *ctx = keptContext;
    }

    // The camera object's update (after the player's camera ran, however its
    // run went: a thread switch can leave the follow camera's frame to the
    // scheduler, past its hook): what the player's camera left, against which
    // the next frame finds the changes made in between.
    void updateObjectCameraHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        HiddenCameras &hc = hiddenCameras();
        const uint32_t object = getRegU32(ctx, kRegA0);
        hc.updateObjectCamera(rdram, ctx, runtime);
        if (hc.running)
            return;
        for (auto &[followCamera, h] : hc.byFollowCamera)
            if (h.valid && h.object.address == object)
                for (HiddenCamera::Block *b : {&h.follow, &h.object, &h.camera})
                    copyOut(rdram, b->address, b->size, b->playerAfter);
    }

    void dropHiddenCamera(uint32_t followCamera)
    {
        auto found = hiddenCameras().byFollowCamera.find(followCamera);
        if (found == hiddenCameras().byFollowCamera.end())
            return;
        if (found->second.valid)
            zge::host::dropGamePose(found->second.camera.address);
        hiddenCameras().byFollowCamera.erase(found);
    }

    // The follow camera's frame: the stick reads inside it are its look
    // around's (lookAround: RX, then RY). With Camera Distance, Height or
    // mouse-look in use, the hidden camera runs first.
    void followCameraHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        MouseLook &m = mouseLookState();
        const uint32_t followCamera = getRegU32(ctx, kRegA0);
        const bool hidden = aggressive_inline::cameraTuningChanged() || m.viewTurned;
        if (hidden)
            runHiddenCamera(rdram, ctx, runtime, followCamera);
        else
            dropHiddenCamera(followCamera);
        const uint32_t outer = m.followCameraRunning;
        m.followCameraRunning = followCamera;
        m.followCamera(rdram, ctx, runtime);
        m.followCameraRunning = outer;
    }

    void cameraStickAccessor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        MouseLook &m = mouseLookState();
        const uint32_t axis = getRegU32(ctx, kRegA0);
        m.stickAccessor(rdram, ctx, runtime);
        if (!m.followCameraRunning)
            return;
        if (axis == kAxisRx)
        {
            zge_host::sites::seen("ai.mouselook.rx");
            m.mouseLookThisFrame = mouseLookAtRxSite(rdram, ctx);
            if (m.mouseLookThisFrame)
                ctx->f[0] = 0.0f;
        }
        else if (axis == kAxisRy)
        {
            zge_host::sites::seen("ai.mouselook.ry");
            if (m.mouseLookThisFrame)
            {
                m.mouseLookThisFrame = false;
                ctx->f[0] = 0.0f;
            }
        }
    }

    void installMouseLook(PS2Runtime &runtime)
    {
        mouseLookState().stickAccessor = runtime.lookupFunction(kStickAccessor);
        mouseLookState().followCamera = runtime.lookupFunction(kFollowCamera);
        hiddenCameras().updateObjectCamera = runtime.lookupFunction(kUpdateObjectCamera);
        if (!mouseLookState().stickAccessor || !mouseLookState().followCamera || !hiddenCameras().updateObjectCamera ||
            !runtime.replaceFunction(kStickAccessor, &cameraStickAccessor) ||
            !runtime.replaceFunction(kFollowCamera, &followCameraHook) ||
            !runtime.replaceFunction(kUpdateObjectCamera, &updateObjectCameraHook))
        {
            std::cerr << "[aggressive-inline] mouse-look hook not installed" << std::endl;
            return;
        }
        const char *trace = std::getenv("PS2X_TRACE_MOUSELOOK");
        mouseLookState().traceMouseLook = trace && trace[0] == '1';
    }

    // ---- widescreen (settings.ini [display] widescreen, View > Widescreen) --
    //
    // The host side shared with BMX XXX (zge_view.cpp: the projection's scale,
    // the frustum, the 2D squeezed, the HUD at the sides) at AI's functions:
    // the projection FUN_00101BB0, the frustum FUN_00101680 and its first
    // sin/cos call 0x136CB0, sceVif1PkCloseUpkCode 0x210F28 on the 2D packet
    // 0x27A848 (0..512 wide, the width at 0x23190C) and zgui's widget draw
    // FUN_00171CA8. AI's own:
    //  - the PCSX2 patch also changes the 1.0 the display setup (FUN_00100FB8,
    //    0x101130) stores at 0x231914 and 0x231948, copied to 0x23FD78, which
    //    FUN_0012A020 gives every camera as camera+0x4C: those get the scale
    //    too;
    //  - a level is game mode 2 (play) or 3 (paused or a message box), the
    //    front end (1) and loading (0) stay 4:3;
    //  - the HUD at the sides: a direct child of InGameUI/Overlay (JuiceMeter,
    //    Score) or InGameUI/OverlayToggle (xpIndicator, Timer, SecondaryScore,
    //    CrashMessage; TrickList and the balance meters are in the middle) or
    //    the game-over score (PauseMenus/Death/Score).
    // PS2X_TRACE_WIDESCREEN=1 logs the game mode's changes.
    constexpr uint32_t kProjection = 0x00101BB0u;
    constexpr uint32_t kSinCos = 0x00136CB0u;
    constexpr uint32_t kFrustum = 0x00101680u;
    constexpr uint32_t kDisplayXScales[] = {0x00231914u, 0x00231948u, 0x0023FD78u};
    constexpr uint32_t kCloseUnpack = 0x00210F28u;
    constexpr uint32_t kGuiDraw = 0x00171CA8u;
    constexpr uint32_t kPacket2D = 0x0027A848u;
    constexpr uint32_t kScreenWidth = 0x0023190Cu; // float

    bool widescreenLevel(const uint8_t *rdram, uint32_t &mode)
    {
        const uint8_t *p = getConstMemPtr(rdram, kGameMode);
        if (!p)
            return false;
        std::memcpy(&mode, p, sizeof(mode));
        return mode == kModePlay || mode == kModePaused;
    }

    bool hudSideAnchored(const uint8_t *rdram, uint32_t widget, uint32_t parent, uint32_t grandparent)
    {
        using zge_host::view::widgetNamed;
        const bool hud = (widgetNamed(rdram, parent, "Overlay") || widgetNamed(rdram, parent, "OverlayToggle")) &&
                         widgetNamed(rdram, grandparent, "InGameUI");
        const bool gameOverScore = widgetNamed(rdram, widget, "Score") && widgetNamed(rdram, parent, "Death") &&
                                   widgetNamed(rdram, grandparent, "PauseMenus");
        return hud || gameOverScore;
    }

    void installWidescreen(PS2Runtime &runtime)
    {
        zge_host::view::installWidescreen(
            runtime, {"aggressive-inline", "ai.widescreen.frustum", kProjection, kFrustum, kSinCos, kCloseUnpack, kGuiDraw,
                      kPacket2D, kScreenWidth, kDisplayXScales, std::size(kDisplayXScales), &widescreenLevel,
                      &hudSideAnchored, &aggressive_inline::drawnFieldOfView});
    }

    // ---- motion blur (settings.ini [display] motion_blur) -----------------
    //
    // In a level the camera blends the previous frame over the new one: the
    // "FSMotionBlur" object FUN_001901C0 makes at level start (none in split
    // screen) gives the camera, through FUN_00111B28, a switch at
    // camera+0x4EC and a colour at camera+0x500 whose alpha (+0x503) is the
    // tuning table's Camera "Normal Blur" (65; below 8 it is off). Nothing
    // else sets it (FUN_0018FCA8 resets it to Normal Blur). Each frame
    // FUN_001116E0(camera, packet) draws the blend if the switch is on, with
    // half the alpha as the GS blend factor (65: 32/128, a quarter of the
    // previous frame). The hook changes the alpha (Low: half) or the switch
    // (Off) only for that call, so the game's state is never changed and the
    // setting applies at once.
    // The hook is the host side AI shares with BMX XXX (zge_view.cpp).
    constexpr uint32_t kBlurDraw = 0x001116E0u;

    // ---- draw distance (settings.ini [gameplay] draw_distance) ------------
    //
    // The host side shared with BMX XXX (zge_view.cpp) at AI's camera
    // setters: the far clip FUN_00129D68 (which caps it at 1000) and the fog's
    // start and end FUN_0012A140, FUN_0012A160, and the levels of detail's
    // FUN_001324F8 (a node's table at +0x174), read by the terrain traversal
    // FUN_0010F6D0. FUN_001901C0 sets them up when a level starts, from the
    // level's "Globals" (Movie Lot: 1000, fog 250-600) or, in split screen,
    // from the tuning table's MP Far Clip entries (Boardwalk, Civic Center,
    // Movie Lot 100, Museum 150) with fog 65-90, and keeps the far clip in
    // the tuning table's "Far Clip"; the camera update FUN_0018FA00 sets the
    // far clip from there every frame. Every camera starts at 200
    // (newSceneRecord), which only the level camera sets again: Far makes
    // the 2D camera's 500 all session, and a new camera's until the level
    // camera sets its own (the game's state, so the setting comes with the
    // boot: drawDistanceChangesGame). Far (x2.5) keeps the game's cap of
    // 1000 (the level camera in single player stays as it is); Farthest
    // (x10) goes up to 2500, past the cap: split screen 100 -> 1000 (fog
    // 650-900), single player 1000 -> 2500 (Movie Lot fog 250-600 ->
    // 625-1500; one Z step is then w^2 / 81900 units deep: 49 at 2000).
    constexpr uint32_t kSetFarClip = 0x00129D68u;
    constexpr uint32_t kSetFogStart = 0x0012A140u;
    constexpr uint32_t kSetFogEnd = 0x0012A160u;
    constexpr uint32_t kSetLodTable = 0x001324F8u;
    constexpr uint32_t kNodeLodTable = 0x174;

    void installViewSettings(PS2Runtime &runtime)
    {
        zge_host::view::installDrawDistance(runtime, {"aggressive-inline", kSetFarClip, kSetFogStart, kSetFogEnd, 0u, 0u,
                                                      nullptr, kSetLodTable, kNodeLodTable, {1.0f, 2.5f, 10.0f},
                                                      {1000.0f, 1000.0f, 2500.0f}});
        zge_host::view::installMotionBlur(runtime, {"aggressive-inline", kBlurDraw});
    }

    // The level loading screen: game mode 2 (level) while the level frame
    // counter is still 0 (it counts from the first level frame; the fly-in
    // and message boxes come after it). Silent, so the scheduler runs it
    // unpaced (EeScheduler::updateFastForward).
    constexpr uint32_t kLevelFrame = 0x00231CACu;

    bool levelLoadingScreen(const uint8_t *rdram)
    {
        uint32_t mode = 0;
        uint32_t frame = 0;
        std::memcpy(&mode, rdram + kGameMode, sizeof(mode));
        std::memcpy(&frame, rdram + kLevelFrame, sizeof(frame));
        return mode == kModePlay && frame == 0u;
    }

    void applyAggressiveInline(PS2Runtime &runtime)
    {
        // The modules below keep their state in this boot's session
        // (runtime/ps2_session.h) and take back what they registered with
        // the host when the game is left; the disc patches and the pad
        // state they set go here.
        ps2x::session::onEnd([]()
                             {
                                 ps2x::disc_patch::removePatches("ai");
                                 // Every texture replacement is this game's (the Xbox
                                 // packs, converted pictures, movie frames).
                                 ps2x::texture_replace::clear();
                                 ps2x::texture_replace::setEnabled(true);
                                 PSPadBackend::setOverrideButtons(0u);
                                 PSPadBackend::setFreeInput(false);
                                 PSPadBackend::setMenuMouse(false);
                                 PSPadBackend::setWheelStick(false);
                             });
        // First, so that the hooks below wrap the readable functions.
        game::readable::install(runtime);
        for (const Binding &binding : kBindings)
        {
            if (!ps2_game_overrides::bindAddressHandler(runtime, binding.address, binding.handler))
            {
                std::cerr << "[aggressive-inline] failed to bind 0x" << std::hex << binding.address
                          << std::dec << " -> " << binding.handler << std::endl;
            }
        }
        // Host side only: AI's controls (its defaults and action names,
        // ps2_game_controls.cpp); controls.ini lives in the user's
        // "Aggressive Inline" config folder (runtime/ps2_host_input.h).
        PSPadBackend::setGame("ai");
        installMouseLook(runtime);
        installWidescreen(runtime);
        installViewSettings(runtime);
        aggressive_inline::language::install(runtime);
        aggressive_inline::installDevTools(runtime);
        ps2_aggressive_inline::installSettingsMenus(runtime);
        aggressive_inline::installGameplay(runtime);
        aggressive_inline::installNativeSaves(runtime);
        aggressive_inline::installZoo(runtime);
        aggressive_inline::installEstate(runtime);
        // The bottom screen reads AI's state (aggressive_inline_panel.cpp); the
        // Estate is served in the Tutorial's folder's place.
        aggressive_inline::panel_state::install(+[]() -> const char * { return aggressive_inline::estateActive() ? "ESTATE" : nullptr; });
        aggressive_inline::installDevStart(runtime);
        aggressive_inline::installApe(runtime);
        aggressive_inline::installXboxTuning(runtime);
        aggressive_inline::installXboxFreeSkate(runtime);
        aggressive_inline::installXboxCareerScripts(runtime);
        runtime.setLoadingScreenProbe(&levelLoadingScreen);
        // Pad and memory card are HLE on the EE (scePad*, sceMc*): the game
        // never binds PADMAN's or MCSERV's RPC servers, and no other module
        // imports from these.
        // With the native disc layer nothing talks to ZCDDVD either, and
        // with native movies nothing to SDRDRV (it only played movie sound).
        std::vector<std::string> skipped{"SIO2MAN.IRX", "PADMAN.IRX", "MCMAN.IRX", "MCSERV.IRX"};
        zge_host::xbox_textures::install(runtime, game::Game::Ai);
        aggressive_inline::installGsTextureAudit(runtime); // developers, PS2X_GSMEM_AUDIT=1
        aggressive_inline::installNativeDraw(runtime);
        // Collision on host data: the host parts get the words AI's level setup writes at load.
        zge::native::setLevelSetupStep(&aggressive::applyLevelSetupOnHost);
        if (aggressive_inline::installNativeDisc(runtime))
            skipped.push_back("ZCDDVD.IRX");
        if (aggressive_inline::installNativeMovies(runtime))
            skipped.push_back("SDRDRV.IRX");
        // With the native ZAudio driver nothing talks to ZAUDIO, and LIBSD
        // only served ZAUDIO and SDRDRV.
        if (aggressive_inline::installNativeSound(runtime))
        {
            skipped.push_back("ZAUDIO.IRX");
            if (std::find(skipped.begin(), skipped.end(), "SDRDRV.IRX") != skipped.end())
                skipped.push_back("LIBSD.IRX");
        }
        // Every IRX the game loads skipped: the IOP has nothing to run
        // (PS2X_IOP=1 keeps it running anyway).
        const bool iopIdle = skipped.size() == 8u;
        runtime.setSkippedIopModules(std::move(skipped));
        const char *iop = std::getenv("PS2X_IOP");
        if (iopIdle && !(iop && iop[0] == '1'))
        {
            runtime.setIopStopped(true);
            std::cout << "[aggressive-inline] the IOP is not run: nothing the game uses is on it (PS2X_IOP=1 runs it)" << std::endl;
        }
        std::cout << "[aggressive-inline] override applied (" << std::size(kBindings) << " bindings)" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Aggressive Inline (USA)", "SLUS_203.27", 0x00100008u, 0xA8BE0B5Bu, &applyAggressiveInline);
