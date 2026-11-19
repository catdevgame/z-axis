#pragma once

// The view settings' host side shared by the games (Aggressive Inline, BMX
// XXX; Dave Mirra BMX 2's widescreen, on ZGE2's camera and zgui): one body
// each, with the game's functions and data in a table its override fills.
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
// the HUD at the wide picture's sides (zge_view.cpp has the whole of it). The
// engines differ in where the camera keeps its scale (WideCamera) and in what
// a zgui widget is (WidgetTree); DM2's own HUD that is no widget is drawn by
// functions the table names (SideDraw). A game's field of view setting is
// drawn the same way: the drawn frustum and projection take it, the camera's
// own field of view stays the game's. The game's decisions keep its own view
// (zge/host/game_view.h).
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

    // Where a camera keeps what the widescreen hooks read and change: the
    // width its field of view spans (zge/scene/camera.cpp spannedRectangle:
    // ZGE3's and ZGE4's viewport, ZGE2's screen rectangle), its horizontal
    // scale, the tangent of its horizontal half angle and that tangent's
    // inverse, its focal length; and, for a drawn field of view
    // (Widescreen::drawnFieldOfView), its kind (0 a view camera) and its
    // field of view (a 16-bit angle).
    struct WideCamera
    {
        uint32_t spannedWidth;
        uint32_t horizontalScale;
        uint32_t tanHalfWidth;
        uint32_t cotHalfWidth;
        uint32_t focalLength;
        uint32_t kind;
        uint32_t fieldOfView;
    };
    // ZGE3's and ZGE4's camera (zge/scene/camera.h Camera: viewport[2],
    // horizontalScale, tanHalfWidth, cotHalfWidth, focalLength, kind,
    // fieldOfView).
    inline constexpr WideCamera kZge4Camera{0x30, 0x4C, 0x4D0, 0x4D8, 0x4E8, 0x04, 0x40};

    // What a zgui widget is, for the HUD at the sides.
    enum class WidgetTree : uint8_t
    {
        // ZGE3 and ZGE4 (AI, BMX XXX): a node of zgui's tree, placed in its
        // parent's screen position (zge/zgui/widget.h Node): a widget is
        // moved by its own position, its children with it.
        Nodes,
        // ZGE2 (DM2): a scene object whose record (WidgetRecord) holds its
        // place on the screen, as every object hanging from it does: a
        // widget is moved by what it draws, its children's draws with it.
        SceneObjects,
    };

    // A function whose 2D draws are a HUD element though it is no widget
    // (DM2's ruler and replay mark: overlay rectangles and texts), drawn at a
    // side of the wide picture while it runs: side -1 left, +1 right, 0 each
    // of its primitives at the side whose third it lies in whole (a function
    // that draws many elements), none in the middle. `drawsHud` (none:
    // always) says when its draws are the HUD's.
    struct SideDraw
    {
        uint32_t function;
        int side;
        bool (*drawsHud)(const uint8_t *rdram);
    };

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
        // The field of view a level's view camera is drawn with, in degrees,
        // for the game's own (the game's camera settings: AI's Field of View
        // and Camera Xbox); null, or the game's returned, draws the game's.
        float (*drawnFieldOfView)(const uint8_t *rdram, float gameDegrees) = nullptr;
        // The engine's camera and widgets (ZGE4's unless given).
        WideCamera camera = kZge4Camera;
        WidgetTree widgets = WidgetTree::Nodes;
        // The HUD's draws that are no widget's (none unless given).
        const SideDraw *sideDraws = nullptr;
        size_t sideDrawCount = 0;
    };

    // True when the hooks are in place.
    bool installWidescreen(PS2Runtime &runtime, const Widescreen &widescreen);

    // A zgui widget's name is `name` (a node's name, Node::name; a ZGE2
    // widget object's, its record's WidgetRecord::name).
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
