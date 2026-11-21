// Dave Mirra Freestyle BMX 2 (SLUS-20159): the view settings.
//
// Widescreen (settings.ini [display] widescreen and aspect, ZX_WIDESCREEN,
// ZX_ASPECT): Aggressive Inline's host side (zge_view.cpp) at DM2's
// functions, on ZGE2's camera and zgui:
//  - the projection's horizontal scale (Camera::horizontalScale, 1.0 from
//    the display at the camera's set-up, setCameraScales) and the frustum's
//    half-width tangent at its first angleSin, from the width the field of
//    view spans, which is ZGE2's screen rectangle (Camera::screen);
//  - the 2D primitives of the screen packet (g_screenPrimitives, as wide as
//    the display's frame buffer, DisplaySetup::bufferWidth) squeezed back to
//    4:3;
//  - the HUD at the wide picture's sides: the widgets of InGameUI's Overlay
//    and OverlayToggle groups in the left or right third of the screen (the
//    score and the goal at the right; the trick's and the reward's displays
//    stay in the middle), and the HUD that is no widget: the ruler at the
//    right (its ticks and numbers, drawn by overlayRulerFrame; its frame and
//    marker, overlay items) and the replay mark at the top right. The
//    overlay items are moved one by one, each that lies in a side third
//    whole, while the play overlay's group is shown (the screens' items, the
//    pause menu's, stay 4:3; the gauge, the balance bar left of the middle,
//    stays). Split screen's groups (MultiplayerUI) keep their places, as BMX
//    XXX's do.
// A level is drawn in game mode kModeLevel once the rider's physics task runs
// (its frame count is not 0): the front end and loading screens stay 4:3.
// With widescreen never turned on nothing is written.
//
// Draw distance (settings.ini [gameplay] draw_distance, ZX_DRAW_DISTANCE):
// a camera's far clip (Camera::farClip; newSceneRecord sets 200 at creation,
// a level its own, Woodward Camp 187). The projection maps the 16-bit Z from
// the near clip to it and the frustum's far plane is built from it, both
// right after this hook on updateProjection. Far multiplies it by 1.5,
// Farthest by 2.5 (the Z buffer is PSMZ16S, so depth steps grow with the
// square of the range); the game's own value is kept and put back at
// Original. Per camera, every frame, only in a level; the setting comes at
// boot (the far clip is the game's state, so DM2's draw distance is Original
// by default: GameSettingsProfile::drawDistanceChangesGame).
#include "dave_mirra.h"
#include "runtime/play_recording.h"
#include "sym/game_overrides/dave_mirra_view.cpp.h"
#include "zge_view.h"
#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
#include <vector>

namespace dave_mirra::view
{
    namespace
    {
        // ZGE2's camera record (zge/scene/camera.h Camera, DM2's offsets).
        constexpr zge_host::view::WideCamera kCamera{
            0x18,  // screen[2]: the width the field of view spans
            0x44,  // horizontalScale
            0x430, // tanHalfWidth
            0x438, // cotHalfWidth
            0x448, // focalLength
            0x04,  // kind
            0x38,  // fieldOfView
        };
        constexpr uint32_t kCameraFarClip = 0x34;
        constexpr uint32_t kDisplayBufferWidth = 0x1C; // DisplaySetup::bufferWidth (zge/render/display.h, DM2's)
        constexpr uint32_t kModeLevel = 2u;            // g_gameMode (dm2/sound/sound_mode.h kSoundModeLevel)
        constexpr uint8_t kPlayOverlayGroup = 0;       // the overlay items' group of play (dm2/overlay/overlay.cpp)
        constexpr float kDrawDistanceFactor[] = {1.0f, 1.5f, 2.5f};
        constexpr int kRegA0 = 4;

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        bool readFloat(const uint8_t *rdram, uint32_t address, float &out)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            if (!p)
                return false;
            std::memcpy(&out, p, sizeof(out));
            return true;
        }

        void writeFloat(uint8_t *rdram, uint32_t address, float value)
        {
            if (uint8_t *p = getMemPtr(rdram, address))
                std::memcpy(p, &value, sizeof(value));
        }

        bool inLevel(const uint8_t *rdram, uint32_t &mode)
        {
            mode = readWord(rdram, sym::dm2::g_gameMode);
            return mode == kModeLevel && readWord(rdram, sym::dm2::g_physicsTaskFrames) != 0u;
        }

        // The HUD's widgets of one view: those of InGameUI's Overlay and
        // OverlayToggle groups (the trick display and the reward display are
        // groups at the screen's origin, which zge_view moves none of: their
        // widgets are drawn in the middle).
        bool hudSideAnchored(const uint8_t *rdram, uint32_t, uint32_t parent, uint32_t grandparent)
        {
            using zge_host::view::widgetNamed;
            return (widgetNamed(rdram, parent, "Overlay") || widgetNamed(rdram, parent, "OverlayToggle")) &&
                   widgetNamed(rdram, grandparent, "InGameUI");
        }

        // The overlay items drawn are the play overlay's (not a screen's).
        bool playOverlayShown(const uint8_t *rdram)
        {
            const uint8_t *group = getConstMemPtr(rdram, sym::dm2::g_overlayGroupShown);
            return group && *group == kPlayOverlayGroup;
        }

        constexpr int kRight = 1, kEachItem = 0; // SideDraw::side
        constexpr zge_host::view::SideDraw kSideDraws[] = {
            {sym::dm2::overlayRulerFrame, kRight, nullptr},
            {sym::dm2::overlayReplayMarkFrame, kRight, nullptr},
            {sym::dm2::drawOverlayItems, kEachItem, &playOverlayShown},
        };

        // Per camera: the far clip the game set and the one written here.
        struct CameraFar
        {
            uint32_t camera = 0;
            float game = 0.0f;
            float written = 0.0f;
        };

        // One boot's original projection and cameras (made at the boot's
        // first use, the install: zx::boot::PerBoot).
        struct State
        {
            PS2Runtime::RecompiledFunction projection = nullptr;
            std::vector<CameraFar> far;
        };

        State &state()
        {
            static zx::boot::PerBoot<State> ofBoot([] { return State{}; });
            return ofBoot.get();
        }

        void drawDistance(uint8_t *rdram, uint32_t camera, int setting)
        {
            float now = 0.0f;
            if (!readFloat(rdram, camera + kCameraFarClip, now))
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
                writeFloat(rdram, camera + kCameraFarClip, want);
            entry->written = want;
        }

        // updateProjection(camera), around the widescreen's hook.
        void projection(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t mode = 0;
            const bool level = inLevel(rdram, mode);
            drawDistance(rdram, getRegU32(ctx, kRegA0),
                         level ? play_recording::read(play_recording::Channel::DrawDistance, runtime->drawDistance()) : 0);
            state().projection(rdram, ctx, runtime);
        }
    }
}

namespace dave_mirra
{
    bool installView(PS2Runtime &runtime)
    {
        namespace zv = zge_host::view;
        using namespace view;
        zv::Widescreen wide{"dave-mirra", "dm2.widescreen.frustum", sym::dm2::updateProjection, sym::dm2::updateFrustum,
                            sym::dm2::angleSin, sym::dm2::vif1PacketCloseUnpack, sym::dm2::drawWidget,
                            sym::dm2::g_screenPrimitives, sym::dm2::g_displaySetupFlags + kDisplayBufferWidth,
                            nullptr, 0u, &inLevel, &hudSideAnchored};
        wide.camera = kCamera;
        wide.widgets = zv::WidgetTree::SceneObjects;
        wide.sideDraws = kSideDraws;
        wide.sideDrawCount = std::size(kSideDraws);
        const bool ok = zv::installWidescreen(runtime, wide);
        State &st = state();
        st.projection = runtime.lookupFunction(sym::dm2::updateProjection);
        if (!st.projection || !runtime.replaceFunction(sym::dm2::updateProjection, &projection))
        {
            std::cerr << "[dave-mirra] draw distance hook not installed" << std::endl;
            return false;
        }
        return ok;
    }
}
