#pragma once

// The view settings' host side shared by the games on ZGE4's camera
// (Aggressive Inline, BMX XXX; ZGE2, Dave Mirra BMX 2's, has its own in
// dave_mirra_view.cpp): one body each, with the game's functions and data in
// a table its override fills.
//
// Motion blur (settings.ini [graphics] motion_blur, PS2Runtime::motionBlur:
// 0 off, 1 low, 2 the game's). In a level the game's "FSMotionBlur" object
// gives the camera a switch (zge/scene/camera.h Camera::motionBlurOn) and a
// colour whose alpha is the blur's strength (Camera::motionBlurColour), and
// each frame drawBlur(camera, packet) draws the last frame over the new one
// while the switch is on, with half the alpha as the GS blend factor. Off
// clears the switch and Low halves the alpha for that one call only, so the
// game's own state is never changed and the setting applies at once; at the
// game's own the call is the game's. PS2X_MOTION_BLUR=0|1|2 sets it in the
// runner (checks; the app sets it from its settings), PS2X_TRACE_MOTION_BLUR=1
// logs the packet bytes the draws add.
//
// Widescreen (settings.ini [display] widescreen and aspect, PS2X_WIDESCREEN,
// PS2X_ASPECT): the 3D drawn narrower for a picture the host stretches to the
// window's aspect, the frustum as much wider, the 2D kept at its 4:3 shape and
// the HUD at the wide picture's sides (zge_view.cpp has the whole of it).
//
// Draw distance (the game's settings.ini [gameplay] draw_distance, PS2X_DRAW_DISTANCE:
// 0 the game's, 1 far, 2 farthest): every far clip the game sets times the
// game's factor (the fog with it), and the levels of detail as far out.

#include <cstddef>
#include <cstdint>

class PS2Runtime;

namespace zge_host::view
{
    struct MotionBlur
    {
        const char *game;  // for the log
        uint32_t drawBlur; // drawBlur(camera, packet)
    };

    // True when the hook is in place.
    bool installMotionBlur(PS2Runtime &runtime, const MotionBlur &blur);

    struct Widescreen
    {
        const char *game;          // for the log
        const char *frustumSite;   // the frustum's place, counted (zge_sites.h)
        uint32_t updateProjection; // updateProjection(camera)
        uint32_t updateFrustum;    // updateFrustum(camera): its first angleSin follows the half-width tangent
        uint32_t angleSin;
        uint32_t closeUnpack;      // vif1PacketCloseUnpack(packet)
        uint32_t drawWidget;       // zgui's widget draw (widget)
        uint32_t screenPrimitives; // the 2D packet (zge/scene/camera.cpp g_screenPrimitives)
        uint32_t screenWidth;      // the display's width (float, DisplaySetup::width)
        // Other words that hold the game's 1.0 horizontal scale and get the
        // wide one too (AI's: the PCSX2 patch's), none for a game without.
        const uint32_t *displayXScales;
        size_t displayXScaleCount;
        // A level is drawn (the front end, loading and movies stay 4:3);
        // mode: the game's own state, for PS2X_TRACE_WIDESCREEN=1.
        bool (*inLevel)(const uint8_t *rdram, uint32_t &mode);
        // A drawable HUD widget (its parent and the parent's parent named)
        // that keeps its distance from the wide picture's edge when it is in
        // the left or right third of the screen.
        bool (*sideAnchored)(const uint8_t *rdram, uint32_t widget, uint32_t parent, uint32_t grandparent);
    };

    // True when the hooks are in place.
    bool installWidescreen(PS2Runtime &runtime, const Widescreen &widescreen);

    // A zgui widget's name is `name` (ZGE4's widget: its name's address at
    // +0x14).
    bool widgetNamed(const uint8_t *rdram, uint32_t widget, const char *name);

    struct DrawDistance
    {
        const char *game;           // for the log
        uint32_t setCameraFarPlane; // (camera, far): the cap of 1000, the store and the changed flag
        // The fog: AI sets its start and end (camera, distance), which follow
        // the far clip's ratio; BMX XXX moves the range to end at the far
        // clip each frame (moveCameraClipRange(camera, depth)), whose depth
        // follows it. 0: the game does not set it that way.
        uint32_t setCameraFogStart;
        uint32_t setCameraFogEnd;
        uint32_t moveCameraClipRange;
        // A function whose far clips copy one already set (BMX XXX's 2D pass,
        // draw2DPass: the 2D camera takes the first view camera's), passed as
        // they are while it runs; 0: none.
        uint32_t copiesFarClip;
        const char *copySite;       // its calls' place, counted (zge_sites.h)
        uint32_t setLodTable;       // (node, level): the node's levels of detail
        uint32_t nodeLods;          // Node::lods, the offset of a node's levels of detail
        // Per setting (the game's, far, farthest): the far clip's factor and
        // the furthest it goes.
        float factor[3];
        float maxFarClip[3];
    };

    // True when the hooks are in place.
    bool installDrawDistance(PS2Runtime &runtime, const DrawDistance &distance);
}
