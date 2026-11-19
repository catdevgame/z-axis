#pragma once

// The engine's camera (one per view): projection,
// clip planes, fog and the matrices the renderers upload.
//
// Space conventions (row vectors, zge/math/types.h): camera space has x to
// the right, y down the screen and z into it (forward). The camera's own
// axes in the world are right, down and forward at +0xB0 (camera -> world,
// with the position as the translation row); +0x70 is the inverse (world ->
// camera). The camera update beginCameraFrame runs each frame: the projection
// (updateProjection), the view matrix, the products, then the frustum
// (updateFrustum).

#include "ee/readable_waits.h"
#include "ee/ee.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/types.h"
#include "zge/host/game_view.h"

#include <cstddef>
#include "sym/zge/scene/camera.h.h"

namespace zge
{
    // DM2's bits are AI's one lower: it has no 2D camera (its initScenes
    // makes none), so no overlay bit.
    enum CameraChangedFlags : uint32_t
    {
        kCameraOverlay = 0x01,        // AI: the 2D camera initScenes makes (drawn by the 2D pass)
        kCameraClears = game::readable::perGame(0x02u, 0x01u),        // its frame is cleared (setCameraClears)
        kCameraMatrixChanged = game::readable::perGame(0x04u, 0x02u), // setCameraMatrix
        kCameraFarChanged = game::readable::perGame(0x08u, 0x04u),    // the projection changed (the far clip setter, setCameraFarPlane: only when it differs)
        kCameraFogChanged = game::readable::perGame(0x10u, 0x08u),
    };

    // A camera's kind (Camera::kind): the views (0, drawn by renderFrame)
    // and the 2D camera (2).
    constexpr uint32_t kCameraKindView = 0, kCameraKindOverlay = 2;

    // The frustum's planes and corners, in this order (view space: the near
    // corners at z = near, the far ones at z = far; x and y negative first).
    enum FrustumPlane : uint8_t
    {
        kPlaneNear,
        kPlaneFar,
        kPlaneLeft,
        kPlaneRight,
        kPlaneTop, // y negative (y grows down the screen)
        kPlaneBottom,
        kFrustumPlanes,
    };
    enum FrustumCorner : uint8_t
    {
        kCornerNearTopLeft,     // (-x, -y, near)
        kCornerNearTopRight,    // (+x, -y, near)
        kCornerNearBottomRight, // (+x, +y, near)
        kCornerNearBottomLeft,  // (-x, +y, near)
        kCornerFarTopLeft,      // the same four at far
        kCornerFarTopRight,
        kCornerFarBottomRight,
        kCornerFarBottomLeft,
        kFrustumCorners,
    };

    // A plane as the camera keeps it: the unit normal, pointing into the
    // frustum, in x, y, z; w = the normal's dot product with a point of the
    // plane (a point p is inside when dot(normal, p) >= w).
    using Plane = Vec4;

    // The world frustum as the display holds it (Camera::worldFrustum).
    struct CameraFrustum
    {
        uint32_t planeCount;        // +0x00 6
        uint8_t _04[0x0C];
        Plane planes[kFrustumPlanes];
    };
    static_assert(offsetof(CameraFrustum, planes) == 0x10);

    struct Camera
    {
        uint8_t type;               // +0x000 2 (the engine's object type of a camera)
        uint8_t _001[0x03];
        uint32_t kind;              // +0x004 kCameraKindView or kCameraKindOverlay
        // +0x008 CameraChangedFlags: bit fields of a 64-bit type in the
        // original, so it reads and writes them as one doubleword with
        // object (LD / SD); the same bytes either way.
        uint32_t changed;
#if PS2X_GAME_DM2
        uint32_t object;            // DM2 +0x00C the scene object it belongs to (ZGE2 keeps no owner or area word)
#else
        uint32_t object;            // +0x00C the scene object it belongs to
        uint32_t owner;             // +0x010 newSceneRecord's fourth argument [inferred: what made it]
        uint32_t area;              // +0x014 the area record the camera is in (setCameraArea, cameraArea)
#endif
        // +0x018 (DM2 +0x010) x, y, width, height of the screen the viewport is part of;
        // the projection puts the viewport's centre as far from the
        // display's centre as it is from this rectangle's (split screen).
        ee::Float screen[4];
        ee::Float viewport[4];      // +0x028 (DM2 +0x020) x, y, width, height
        ee::Float nearClip;         // +0x038 (DM2 +0x030) (1.25 in a level)
        // +0x03C (DM2 +0x034) far clip: capped at 1000 by setCameraFarPlane (the draw distance
        // setting stores up to 2500 past it).
        ee::Float farClip;
        Angle fieldOfView;          // +0x040 (DM2 +0x038) the horizontal angle the viewport's width spans
        uint8_t _042[0x02];
        // +0x044, +0x048 (DM2 +0x03C, +0x040) setSceneDepthRange's two values (0 and 0 when made) [unknown: what reads them].
        ee::Float depthRange[2];
        // +0x04C, +0x050 (DM2 +0x044, +0x048) the screen projection's x and y scales (1.0, the
        // display's at camera set-up; the widescreen hook makes x 0.75).
        ee::Float horizontalScale;
        ee::Float verticalScale;
        uint8_t clearColour[4];     // +0x054 (DM2 +0x04C) R, G, B, A: the frame's clear (the camera's set-up gives it to the display)
        uint8_t fogColour[4];       // +0x058 (DM2 +0x050) R, G, B, A (setCameraFogColour)
        ee::Float fogStart;         // +0x05C (DM2 +0x054) (setCameraFogStart)
        ee::Float fogEnd;           // +0x060 (DM2 +0x058) (setCameraFogEnd)
#if PS2X_GAME_DM2
        uint32_t _05c;              // DM2 +0x05C
#else
        uint8_t _064[0x0C];         // +0x064
#endif
        Matrix fromWorld;           // +0x070 (DM2 +0x060) world -> camera (partInView takes points into view space with it)
        Vec4 right;                 // +0x0B0 (DM2 +0x0A0) the camera's axes in the world (camera -> world)
        Vec4 down;                  // +0x0C0 (DM2 +0x0B0)
        Vec4 forward;               // +0x0D0 (DM2 +0x0C0)
        Vec4 position;              // +0x0E0 (DM2 +0x0D0)
#if !PS2X_GAME_DM2
        uint8_t _0f0[0x80];         // +0x0F0
        uint32_t viewStale;         // +0x170 cleared when the matrix is set [unknown: what sets it]
        uint8_t _174[0x0C];         // +0x174
#endif
        Matrix view;                // +0x180 (DM2 +0x0E0) world -> view
        Matrix screenProjection;    // +0x1C0 (DM2 +0x120) view -> GS screen (updateProjection)
        Matrix viewProjection;      // +0x200 (DM2 +0x160) world -> GS screen (VU1 1016-1019)
        Matrix clipProjection;      // +0x240 (DM2 +0x1A0) view -> clip volume (guard band; updateProjection)
        Matrix clipViewProjection;  // +0x280 (DM2 +0x1E0) world -> clip volume (VU1 1020-1023)
        // +0x2C0 (DM2 +0x220) 1 once lastForward and lastPosition hold a
        // frame's (set by cameraCut, which compares with them only then).
        uint32_t lastKept;
        uint8_t _2c4[0x0C];
        Vec4 lastForward;           // +0x2D0 (DM2 +0x230) the previous frame's (camera cut test, cameraCut)
        Vec4 lastPosition;          // +0x2E0 (DM2 +0x240)
        // The view frustum (updateFrustum, each frame): in view space and
        // in the world.
        uint32_t viewPlaneCount;    // +0x2F0 (DM2 +0x250) 6
        uint8_t _2f4[0x0C];
        // +0x300 (DM2 +0x260) view space (clipSegmentsToPlanes clips segments against them).
        // Their normals come from the half angles, w from the plane's
        // first corner. As compiled, the top and bottom planes get each
        // other's normals, so their w is 2 d sin(half angle) (d = far for
        // the top's corner, near for the bottom's) where it would be 0
        // (the Tutorial: 165.79 and 1.036 at far 200); with widescreen the
        // corners are wider than the side normals' angle, so the left
        // plane's w is not 0 either. The world planes, made from the
        // corners, are right.
        Plane viewPlanes[kFrustumPlanes];
        Vec4 viewCorners[kFrustumCorners];  // +0x360 (DM2 +0x2C0) view space; w is not written
        uint32_t planeCount;        // +0x3E0 (DM2 +0x340) 6
        uint8_t _3e4[0x0C];
        Plane planes[kFrustumPlanes];       // +0x3F0 (DM2 +0x350) the world's
        Vec4 corners[kFrustumCorners];      // +0x450 (DM2 +0x3B0) the world's
        // +0x4D0 (DM2 +0x430) tan of the horizontal and vertical half angles (x and y over
        // z at the viewport's edges), then their inverses.
        ee::Float tanHalfWidth;
        ee::Float tanHalfHeight;    // +0x4D4 (DM2 +0x434)
        ee::Float cotHalfWidth;     // +0x4D8 (DM2 +0x438)
        ee::Float cotHalfHeight;    // +0x4DC (DM2 +0x43C)
        // +0x4E0, +0x4E4 (DM2 +0x440, +0x444) what partInView's sphere test multiplies a
        // radius by against the side planes: sqrt(t * t) + 1 of the tangents
        // above, as compiled (sqrt(t * t + 1), 1 / cos of the half angle, is
        // what a sphere needs; |t| + 1 is never smaller, so the test errs on
        // the side of drawing).
        ee::Float sphereScaleX;
        ee::Float sphereScaleY;
        ee::Float focalLength;      // +0x4E8 (DM2 +0x448) in GS pixels: the viewport's half width / tan(fieldOfView / 2)
#if PS2X_GAME_DM2
        uint32_t _44c;              // DM2 +0x44C, the record's last word (no DM2 code reads it)
        // ZGE2's camera ends here (0x450 bytes, kCameraRecordBytes): it has no motion blur, owner, area or
        // viewStale word (DM2's newSceneRecord and setCameraMatrix write none, and no DM2 code reads past +0x448).
        // They stand past its end for the shared code that names them, in steps and functions DM2 has not got.
        uint32_t owner, area, viewStale;
#endif
        uint32_t motionBlurOn;      // +0x4EC (DM2: none) (setCameraMotionBlur; drawn by drawBlur)
        // +0x4F0 the blur's source rectangle's scale in x and y (0 .. 1; the
        // last frame drawn over the view zoomed by it: addMotionBlurSprite).
        ee::Float motionBlurScale[2];
        ee::Float motionBlurExtra[2]; // +0x4F8 kept clamped to -1 .. 1; no code reads them
        uint8_t motionBlurColour[4]; // +0x500 RGBA; alpha (+0x503) is the tuning table's "Normal Blur"
#if !PS2X_GAME_DM2
        uint8_t _504[0x0C];         // +0x504
#endif

        // The flags as the original's bit fields reach them: one doubleword
        // with the word after.
        uint64_t &changedDoubleword() { return *reinterpret_cast<uint64_t *>(&changed); }
        // Camera -> world as a matrix: the axes, then the position.
        Matrix &toWorld() { return *reinterpret_cast<Matrix *>(&right); }
        // The world frustum's fields as one record.
        CameraFrustum &worldFrustum() { return *reinterpret_cast<CameraFrustum *>(&planeCount); }
    };
    static_assert(offsetof(Camera, changed) == 0x08 && offsetof(Camera, kind) == 0x04 && offsetof(Camera, object) == 0x0C &&
                  offsetof(Camera, depthRange) == game::readable::perGame(0x44u, 0x3Cu) &&
                  offsetof(Camera, clearColour) == game::readable::perGame(0x54u, 0x4Cu) && offsetof(Camera, fogColour) == game::readable::perGame(0x58u, 0x50u));
    static_assert(offsetof(Camera, screen) == game::readable::perGame(0x18u, 0x10u) && offsetof(Camera, viewport) == game::readable::perGame(0x28u, 0x20u));
    static_assert(offsetof(Camera, nearClip) == game::readable::perGame(0x38u, 0x30u) && offsetof(Camera, farClip) == game::readable::perGame(0x3Cu, 0x34u));
    static_assert(offsetof(Camera, fieldOfView) == game::readable::perGame(0x40u, 0x38u));
    static_assert(offsetof(Camera, horizontalScale) == game::readable::perGame(0x4Cu, 0x44u) && offsetof(Camera, verticalScale) == game::readable::perGame(0x50u, 0x48u));
    static_assert(offsetof(Camera, fogStart) == game::readable::perGame(0x5Cu, 0x54u) && offsetof(Camera, fogEnd) == game::readable::perGame(0x60u, 0x58u));
    static_assert(offsetof(Camera, fromWorld) == game::readable::perGame(0x70u, 0x60u) && offsetof(Camera, right) == game::readable::perGame(0xB0u, 0xA0u));
    static_assert(offsetof(Camera, down) == game::readable::perGame(0xC0u, 0xB0u));
    static_assert(offsetof(Camera, forward) == game::readable::perGame(0xD0u, 0xC0u) && offsetof(Camera, position) == game::readable::perGame(0xE0u, 0xD0u));
    static_assert(offsetof(Camera, view) == game::readable::perGame(0x180u, 0x0E0u) && offsetof(Camera, screenProjection) == game::readable::perGame(0x1C0u, 0x120u));
    static_assert(offsetof(Camera, clipProjection) == game::readable::perGame(0x240u, 0x1A0u) && offsetof(Camera, clipViewProjection) == game::readable::perGame(0x280u, 0x1E0u));
    static_assert(offsetof(Camera, lastKept) == game::readable::perGame(0x2C0u, 0x220u));
    static_assert(offsetof(Camera, lastForward) == game::readable::perGame(0x2D0u, 0x230u) && offsetof(Camera, lastPosition) == game::readable::perGame(0x2E0u, 0x240u));
    static_assert(offsetof(Camera, viewPlaneCount) == game::readable::perGame(0x2F0u, 0x250u) && offsetof(Camera, viewPlanes) == game::readable::perGame(0x300u, 0x260u));
    static_assert(offsetof(Camera, viewCorners) == game::readable::perGame(0x360u, 0x2C0u) && offsetof(Camera, planeCount) == game::readable::perGame(0x3E0u, 0x340u));
    static_assert(offsetof(Camera, planes) - offsetof(Camera, planeCount) == offsetof(CameraFrustum, planes));
    static_assert(offsetof(Camera, planes) == game::readable::perGame(0x3F0u, 0x350u) && offsetof(Camera, corners) == game::readable::perGame(0x450u, 0x3B0u));
    static_assert(offsetof(Camera, tanHalfWidth) == game::readable::perGame(0x4D0u, 0x430u) && offsetof(Camera, cotHalfHeight) == game::readable::perGame(0x4DCu, 0x43Cu));
    static_assert(offsetof(Camera, sphereScaleX) == game::readable::perGame(0x4E0u, 0x440u) && offsetof(Camera, focalLength) == game::readable::perGame(0x4E8u, 0x448u));
    static_assert(game::readable::perGame(offsetof(Camera, motionBlurOn) == 0x4EC && offsetof(Camera, motionBlurScale) == 0x4F0 &&
                                              offsetof(Camera, motionBlurExtra) == 0x4F8 && offsetof(Camera, motionBlurColour) == 0x500 &&
                                              offsetof(Camera, area) == 0x14 && offsetof(Camera, viewStale) == 0x170 && sizeof(Camera) == 0x510,
                                          offsetof(Camera, owner) == 0x450)); // (DM2: the fields it has not got begin at its record's end)

    // The bytes of a camera record (the pool's records, newSceneRecord's clear): ZGE2's ends after focalLength.
    constexpr uint32_t kCameraRecordBytes = game::readable::perGame(static_cast<uint32_t>(sizeof(Camera)), 0x450u);

    // The corners that make each plane (in FrustumPlane order),
    // listed going round it: the first three give its normal, (b - a) x
    // (c - b), and a its distance. The fourth is not read.
    struct FrustumPlaneCorners
    {
        uint8_t corner[4];
    };
    static_assert(sizeof(FrustumPlaneCorners) == 4);
    inline ee::Global<FrustumPlaneCorners[kFrustumPlanes], sym::g_frustumPlaneCorners> g_frustumPlaneCorners;

    // updateProjection: the camera's two projections from its viewport, clip
    // distances and focal length (the last frame's, from updateFrustum):
    // screenProjection (view -> GS screen pixels and depth) and
    // clipProjection (view -> the clip volume of the guard band).
    void updateProjection(ee::Ptr<Camera> camera);
    // setCameraArea (camera_records.cpp): the area record the camera is in.
    void setCameraArea(ee::Ptr<Camera> camera, uint32_t area);

    // updateFrustum: the focal length and half-angle tangents from the
    // field of view and the viewport, then the view frustum: its planes and
    // corners in view space and in the world.
    void updateFrustum(ee::Ptr<Camera> camera);

    // camera_records.cpp: cameraNearClip the camera's near clip; overlayCamera
    // the 2D camera.
    ee::Float cameraNearClip(ee::Ptr<Camera> camera);
    ee::Ptr<Camera> overlayCamera();
}

namespace sce { struct Vif1Packet; } // (sce/packet_records.h)

namespace zge
{
    struct Box;         // (zge/math/box.h)
    struct SceneObject; // (zge/scene/scene.h)

    // (camera.cpp)
    void setViewportScissor(sce::Vif1Packet *packet, ee::Ptr<Camera> camera);
    int32_t beginCameraFrame(ee::Ptr<Camera> camera);
    uint32_t initScenesHook();
    uint32_t sceneHook();
    uint32_t countSceneRecord();
    uint32_t uncountSceneRecord();
    uint32_t sceneRecordCount();
    uint32_t endCameraSetup();
    int32_t beginViewDraws(ee::Ptr<Camera> camera);
    uint32_t textureCacheCameraHook();
    uint32_t partClipFlags(ee::Ptr<Camera> camera, ee::Ptr<const Box> box, ee::Ptr<const Matrix> world);
}

namespace zge
{
    // (camera_objects.cpp)
    uint32_t setFov(ee::Ptr<SceneObject> object, uint32_t fieldOfView);
    ee::Ptr<SceneObject> newCamera();
    ee::Ptr<SceneObject> newSceneCameraWith(uint32_t owner);
}

namespace zge
{
    // (camera_records.cpp)
    uint32_t initScenes();
    uint32_t shutDownScenes();
    ee::Ptr<Camera> newSceneRecord(uint32_t kind, uint32_t unused, ee::Address object READABLE_PARAM_EXCEPT(dm2, uint32_t owner)); // (ZGE2's takes three words)
    uint32_t freeSceneRecord(ee::Address camera);
    uint32_t setCameraFieldOfView(ee::Ptr<Camera> camera, uint32_t fieldOfView);
    uint64_t setCameraFarPlane(ee::Ptr<Camera> camera, ee::Float farClip);
    void setCameraMatrix(ee::Ptr<Camera> camera, ee::Ptr<Matrix> matrix);

    // The game's own camera stands elsewhere than the drawn one this frame (a hidden camera's pose,
    // zge/host/game_view.h; from the camera's frustum on).
    bool gameCameraElsewhere(ee::Ptr<const Camera> camera);
    // Where the game's own camera is: what the game measures from its eye (the area the scene walk stamps the
    // regions out of view from, the placed effects' range).
    Vec4 gameCameraPosition(ee::Ptr<const Camera> camera);
    uint32_t setCameraViewport(ee::Ptr<Camera> camera, ee::Address rectangle);
    void sceneViewport(ee::Ptr<Camera> camera, ee::Address rectangle);
    // cameraViewport (ZGE4 only, camera_records.cpp): the camera's viewport rectangle copied out.
    void cameraViewport(ee::Ptr<Camera> camera, ee::Address rectangle);
    uint32_t cameraObject(ee::Ptr<Camera> camera);
    uint32_t swapCameraViewport(ee::Ptr<Camera> camera, ee::Address rectangle);
    void setCameraScales(ee::Ptr<Camera> camera, ee::Float horizontalScale, ee::Float verticalScale);
    void setCameraClears(ee::Ptr<Camera> camera, uint32_t on);
    void setSceneDepthRange(ee::Ptr<Camera> camera, ee::Float first, ee::Float second);
    void setCameraClearColour(ee::Ptr<Camera> camera, uint32_t colour);
    // setCameraClears: whether the camera's frame is cleared (camera_records.cpp).
    void setCameraClears(ee::Ptr<Camera> camera, uint32_t on);
    void setCameraFogColour(ee::Ptr<Camera> camera, uint32_t colour);
    uint64_t setCameraFogStart(ee::Ptr<Camera> camera, ee::Float start);
    uint64_t setCameraFogEnd(ee::Ptr<Camera> camera, ee::Float end);
    // The fog range moved to end at the far clip and start `depth` before it (camera_records.cpp).
    ee::Float moveCameraClipRange(ee::Ptr<Camera> camera, ee::Float depth);
    ee::Ptr<Camera> firstCameraOfKind(uint32_t kind);
    ee::Ptr<Camera> nextCameraOfKind(ee::Ptr<Camera> camera);
    uint32_t setCurrentCamera(ee::Ptr<Camera> camera);
    uint32_t clearCurrentCamera();
    void draw2DPass();
    uint32_t cameraArea(ee::Ptr<Camera> camera);

    // A bounding sphere as the parts and nodes keep it (and ZGE4's billboard effects).
    struct ViewSphere
    {
        Vec4 centre;      // +0x00
        ee::Float radius; // +0x10
        uint32_t _14, _18, _1c;
    };
    static_assert(sizeof(ViewSphere) == 0x20);

    // partInView's answer.
    enum ViewSphereFlags : uint32_t
    {
        kSphereOutOfView = 1u << 0,
        kSphereCrossesNear = 1u << 1, // with kSphereCrossesSide's bit: 10
        kSphereCrossesFar = 1u << 2,
        kSphereCrossesSide = 1u << 3,
        kSphereNearNearPlane = 1u << 4, // within 4 units of the near plane (only with kSphereCrossesSide)
    };
    // partInView (camera.cpp): where the sphere lies against the camera's view (ViewSphereFlags; 0: wholly inside).
    uint32_t partInView(ee::Ptr<Camera> camera, ee::Ptr<ViewSphere> sphere);
    uint32_t freeLitoRecords();
}
