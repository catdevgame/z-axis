// BMX XXX (SLUS-20415): the view settings, Aggressive Inline's host side
// (zge_view.cpp) at BMX XXX's functions: ZGE4's camera, zgui and the screen
// packet are AI's.
//
// Motion blur (settings.ini [graphics] motion_blur): ZGE4's camera blur
// (drawBlur, the camera's blur switch and colour at AI's offsets), set up by
// the level's "FSMotionBlur" object as AI's is. At the game's own (the
// default) the game's draw runs as it is.
//
// Widescreen (settings.ini [display] widescreen and aspect): the projection's
// horizontal scale, the frustum's half-width tangent at its first angleSin,
// the 2D primitives of the screen packet squeezed back to 4:3 and the HUD at
// the wide picture's sides, as AI's. A level is drawn while a ridden state
// runs (game/bmx/states/game_state.h: the level, the park editor, a saved
// park ridden; not while it loads); the front end, its loading and the
// movies stay 4:3. The HUD at the sides: the widgets of the one-view
// overlay's Overlay groups (UI_overDisp_com: the timer; UI_overDisp_P_full:
// the trick, the balance, the score, the health, the goals) in the left or
// right third of the screen; split screen's keep their places.
//
// Draw distance (settings.ini [gameplay] draw_distance, at boot): the level camera's
// lens (applyCameraLens, every frame) sets the far clip from the level's
// (levelFarClip; nearer in split screen, up to twice as far with a narrow
// field of view) through setCameraFarPlane, which caps it at 1000, and
// moves the fog to end there (moveCameraClipRange). Far and Farthest
// multiply the far clip, and the fog's depth with it, as below. The
// original's: the level's Globals point's FarClip (levelFarClip; 200 for a
// level without, at least 11), as far again with a narrow field of view, and
// the models' own LOD ends. Beyond Original the game's state differs (the
// far clips and LOD ends are its RAM), so BMX XXX's draw distance is Original
// by default and is given at boot (GameSettingsProfile::
// drawDistanceChangesGame).

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_view.cpp.h"
#include "zge_view.h"

#include "ps2_runtime.h"

#include <cstring>

namespace bmx_xxx::view
{
    namespace
    {
        // game/bmx/states/game_state.h and game_state.cpp: the ridden
        // states, and the state machine's phase while a state's frame runs.
        constexpr uint8_t kGameStateLevel = 1, kGameStateParkEditor = 2, kGameStateRidePark = 3;
        constexpr uint8_t kStateRunning = 0;
        constexpr uint32_t kDisplayWidth = 0x0C; // DisplaySetup::width (zge/render/display.h)
        constexpr uint32_t kNodeLods = 0x16C;    // Node::lods (zge/scene/node.h, BMX's)

        uint8_t readByte(const uint8_t *rdram, uint32_t address)
        {
            const uint8_t *p = getConstMemPtr(rdram, address);
            return p ? *p : 0u;
        }

        bool ridden(const uint8_t *rdram, uint32_t &mode)
        {
            mode = readByte(rdram, sym::bmx::g_currentGameState);
            return (mode == kGameStateLevel || mode == kGameStateParkEditor || mode == kGameStateRidePark) &&
                   readByte(rdram, sym::bmx::g_gameStatePhase) == kStateRunning;
        }

        bool hudSideAnchored(const uint8_t *rdram, uint32_t, uint32_t parent, uint32_t grandparent)
        {
            using zge_host::view::widgetNamed;
            return widgetNamed(rdram, parent, "Overlay") &&
                   (widgetNamed(rdram, grandparent, "UI_overDisp_com") || widgetNamed(rdram, grandparent, "UI_overDisp_P_full"));
        }
    }
}

namespace bmx_xxx
{
    bool installView(PS2Runtime &runtime)
    {
        namespace zv = zge_host::view;
        using namespace view;
        bool ok = zv::installMotionBlur(runtime, {"bmx-xxx", sym::bmx::drawBlur});
        ok = zv::installWidescreen(runtime, {"bmx-xxx", "bmx.widescreen.frustum", sym::bmx::updateProjection,
                                             sym::bmx::updateFrustum, sym::bmx::angleSin,
                                             sym::bmx::vif1PacketCloseUnpack, sym::bmx::drawWidget,
                                             sym::bmx::g_screenPrimitives, sym::bmx::g_displaySetupFlags + kDisplayWidth,
                                             nullptr, 0u, &ridden, &hudSideAnchored, &drawnFieldOfView}) && ok;
        ok = zv::installDrawDistance(runtime, {"bmx-xxx", sym::bmx::setCameraFarPlane, 0u, 0u,
                                               sym::bmx::moveCameraClipRange, sym::bmx::draw2DPass,
                                               "bmx.draw_distance.2d_pass", sym::bmx::setLodTable, kNodeLods,
                                               {1.0f, 2.0f, 4.0f}, {1000.0f, 1000.0f, 2500.0f}}) && ok;
        return ok;
    }
}
