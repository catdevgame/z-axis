// The cameras' records (zge/scene/camera.h lays a Camera out): their pool,
// a camera made with its defaults, its setters and getters, the walk over
// the cameras of a kind, the current camera and the frame's 2D pass.
//
// Original quirk kept: newSceneRecord's second argument is not read.

#include "camera.h"

#include "zge/math/vec.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/math/matrix.h"
#include "zge/memory/pool.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/platform/tasks.h"
#include "zge/platform/platform.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/scene/camera_records.cpp.h"

namespace zge
{
    void setSceneDepthRange(ee::Ptr<Camera> camera, ee::Float first, ee::Float second);
    void setCameraNearClip(ee::Ptr<Camera> camera, ee::Float nearClip);
    void setCameraClears(ee::Ptr<Camera> camera, uint32_t on);
    ee::Float cameraFarClip(ee::Ptr<Camera> camera);

    namespace
    {
        inline ee::Global<uint32_t, sym::g_scenesReady> g_scenesReady;
        inline ee::Global<uint32_t, sym::g_cameraPool> g_cameraPool;
        // The first camera the last walk of a kind found (setCurrentCamera
        // restarts the view count at it).
        inline ee::Global<ee::Ptr<Camera>, sym::g_firstCamera> g_firstCamera;
        inline ee::Global<ee::Ptr<Camera>, sym::g_overlayCamera> g_overlayCamera; // the 2D camera
        inline ee::Global<ee::Ptr<Camera>, sym::g_currentCamera> g_currentCamera;
        inline ee::Global<int32_t, sym::g_currentView> g_currentView;    // the view drawn (split screen: 0, 1)
        // ZGE4's count of the view cameras (kCameraKindView) there are: newSceneRecord and freeSceneRecord keep it,
        // nodeNotDrawn walks that many views.
        inline ee::Global<int32_t, sym::bmx::g_viewCameraCount> g_viewCameraCount;

        // The cameras' defaults the video mode gives (g_cameraDefaults; resetDisplayMode).
        // ZGE2's have a word fewer before the screen's size and none between it and the scales (DM2's
        // newSceneRecord reads its four words at +0x08 .. +0x14; drawProfilePeak the width at +0x08).
        constexpr uint32_t kCameraDefaults = sym::g_cameraDefaults;
        inline ee::Global<int32_t, kCameraDefaults + game::readable::perGame(0x0Cu, 0x08u)> g_screenWidth;
        inline ee::Global<int32_t, kCameraDefaults + game::readable::perGame(0x10u, 0x0Cu)> g_screenHeight;
        inline ee::Global<ee::Float, kCameraDefaults + game::readable::perGame(0x18u, 0x10u)> g_defaultHorizontalScale;
        inline ee::Global<ee::Float, kCameraDefaults + game::readable::perGame(0x1Cu, 0x14u)> g_defaultVerticalScale;

        constexpr uint32_t kCameraRecordsPerBlock = 2;
        // The engine's object type of a camera (Camera::type): ZGE2 numbers it 1.
        constexpr uint8_t kCameraObjectType = game::readable::perGame<uint8_t>(2, 1);
        // The matrices a new camera starts as the identity, from fromWorld on: ZGE2 makes fromWorld alone one
        // (its axes stay 0 until setCameraMatrix); ZGE3 the axes and the two after them too.
        constexpr uint32_t kIdentityMatrices = game::readable::perGame(4u, 1u);
        // ZGE2's new camera has its fog start at 1 (it has no fog end); ZGE3's starts and ends at 0.
        constexpr ee::Float kFogStartDefault = game::readable::perGame(0.0f, 1.0f);
        constexpr uint32_t kCameraFieldOfViewDefault = 0x271C; // 10012 (55 degrees)
        constexpr ee::Float kNearClipDefault = 0.5f;
        constexpr ee::Float kFarClipDefault = 200.0f;
        constexpr ee::Float kFarClipLimit = 1000.0f;
        // The previous viewport swapCameraViewport keeps (a rectangle).
        constexpr uint32_t kPreviousViewport = sym::previousViewport;

        constexpr uint32_t kCreatePool = sym::createPool;     // (record bytes, records a block, free callback) -> a pool
        constexpr uint32_t kDestroyPool = sym::destroyPool;
        constexpr uint32_t kPoolLinkOf = sym::poolLinkOf;     // (record) -> its list link
        constexpr uint32_t kFreeSceneRecord = sym::freeSceneRecord;
        constexpr uint32_t kClearCurrentCamera = sym::clearCurrentCamera;
        // Not in the sweep (recompiled code): (camera, far) -> the old far
        // clip: capped at 1000, the projection marked changed when it
        // differs; (camera, depth) -> the old fog start / end, the fog changed.
        constexpr uint32_t kSetCameraFogStart = sym::setCameraFogStart;
        constexpr uint32_t kSetCameraFogEnd = sym::setCameraFogEnd;

        ee::Ptr<Pool> cameraPool() { return ee::Ptr<Pool>(*g_cameraPool); }
        uint32_t &changedOf(ee::Ptr<Camera> camera) { return camera->changed; }

        // The rectangles and colours come and go by unaligned loads, from
        // anywhere.
        void copyFrom(void *to, ee::Ptr<const uint8_t> from, uint32_t bytes) { std::memcpy(to, from.get(), bytes); }
        void copyTo(ee::Ptr<uint8_t> to, const void *from, uint32_t bytes) { std::memcpy(to.get(), from, bytes); }

        // Four quadword moves: the low four bits of the address ignored.
        void loadAxes(Vec4 *to, ee::Ptr<Matrix> matrix)
        {
            for (uint32_t row = 0; row < 4; ++row)
                to[row] = quadwordOf(ee::Ptr<Vec4>(&matrix->row(row)));
        }

        struct ScreenRectangle
        {
            ee::Float x, y, width, height;
        };

        // An unsigned screen size as a float (CVT.S.W of half and the low
        // bit, doubled, when the top bit is set).
        ee::Float unsignedToFloat(int32_t value)
        {
            if (value >= 0)
                return ee::toFloat(value);
            const ee::Float half = ee::toFloat(static_cast<int32_t>((static_cast<uint32_t>(value) & 1u) |
                                                                    (static_cast<uint32_t>(value) >> 1)));
            return half + half;
        }
    }

    // ---- the pool -----------------------------------------------------------------

    // initScenes: the cameras' pool (0x510-byte records, two a block; ZGE2's 0x450) and
    // the 2D camera (ZGE2 has none). 0 if the platform's hook fails.
    uint32_t initScenes()
    {
        *g_cameraPool = createPool(kCameraRecordBytes, kCameraRecordsPerBlock, kFreeSceneRecord);
        if (initScenesHook() == 0)
            return 0;
        *g_scenesReady = 1;
        if constexpr (game::readable::kIsDm2)
            return 1;
        const ee::Ptr<Camera> overlay = newSceneRecord(kCameraKindOverlay, 0, 0 READABLE_PARAM_EXCEPT(dm2, 0));
        *g_overlayCamera = overlay;
        if (overlay)
            overlay->changed |= kCameraOverlay;
        return 1;
    }
    READABLE_FUNCTION_WAITS(initScenes, Verified);

    // shutDownScenes: the 2D camera freed and the pool destroyed. 0 if the
    // platform's hook fails.
    uint32_t shutDownScenes()
    {
        if (*g_scenesReady != 0)
        {
            if constexpr (!game::readable::kIsDm2) // (ZGE2 has no 2D camera)
            {
                const ee::Ptr<Camera> overlay = *g_overlayCamera;
                if (overlay)
                {
                    freeSceneRecord(overlay);
                    *g_overlayCamera = {};
                }
            }
            destroyPool(cameraPool());
            if (sceneHook() == 0)
                return 0;
            *g_scenesReady = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownScenes, Verified);

    namespace
    {
        // Scaffolding (unnamed-field): ZGE4's setter of bit 0x20 of a camera's changed flags (the word's bit 0);
        // newSceneRecord is its only caller and nothing found reads the bit yet, so it has no name: by address.
        inline void setCameraFlag20(ee::Ptr<Camera> camera, uint32_t on) { ee::game<void>(sym::bmx::setCameraFlag20, camera, on); }
    }

    // newSceneRecord: a camera of a kind for a scene object, from the pool,
    // cleared: identity matrices, the whole screen as its viewport, the
    // defaults' scales, a 55-degree field of view, no fog range, near 0.5,
    // far 200, clearing its frame; counted (ZGE4: its flag 0x20 set too, a view
    // camera counted as one). 0 when the pool is empty. ZGE2 keeps no owner:
    // its newSceneRecord takes three words.
    ee::Ptr<Camera> newSceneRecord(uint32_t kind, uint32_t unused, ee::Address object READABLE_PARAM_EXCEPT(dm2, uint32_t owner = 0))
    {
        (void)unused;
        ee::Local<ScreenRectangle> wholeScreen;
        const ee::Ptr<Camera> self(poolAlloc(cameraPool()));
        if (!self)
            return {};
        memoryFill(self, 0, kCameraRecordBytes);
        self->kind = kind;
        self->type = kCameraObjectType;
        self->object = object;
        wholeScreen->x = ee::toFloat(0);
        wholeScreen->y = ee::toFloat(0);
        wholeScreen->width = ee::toFloat(*g_screenWidth);
        wholeScreen->height = ee::toFloat(*g_screenHeight);
        // fromWorld, the axes as one matrix, and the two after them (ZGE2: fromWorld).
        const ee::Ptr<Matrix> matrices = &self->fromWorld;
        for (uint32_t i = 0; i < kIdentityMatrices; ++i)
            matrixIdentity(matrices + i);
        setCameraViewport(self, wholeScreen);
        setSceneDepthRange(self, ee::Float(0.0f), ee::Float(0.0f));
        setCameraScales(self, *g_defaultHorizontalScale, *g_defaultVerticalScale);
        setCameraFieldOfView(self, kCameraFieldOfViewDefault);
        setCameraFogStart(self, kFogStartDefault);
        if constexpr (!game::readable::kIsDm2)
            setCameraFogEnd(self, ee::Float(0.0f));
        setCameraNearClip(self, kNearClipDefault);
        setCameraFarPlane(self, kFarClipDefault);
        setCameraClears(self, 1);
        if constexpr (game::readable::kIsBmx)
            setCameraFlag20(self, 1);
        if constexpr (!game::readable::kIsDm2)
            self->owner = owner;
        countSceneRecord();
        // ZGE4 counts its view cameras (freeSceneRecord takes them off).
        if constexpr (game::readable::kIsBmx)
            if (self->kind == kCameraKindView)
                *g_viewCameraCount += 1;
        return self;
    }
    READABLE_FUNCTION_WAITS(newSceneRecord, Verified);

    // freeSceneRecord: a camera uncounted and back to the pool (ZGE4: a view
    // camera off its count too). 1.
    uint32_t freeSceneRecord(ee::Address camera)
    {
        if constexpr (game::readable::kIsBmx)
            if (ee::Ptr<Camera>(camera)->kind == kCameraKindView)
                *g_viewCameraCount -= 1;
        uncountSceneRecord();
        poolFree(cameraPool(), camera);
        return 1;
    }
    READABLE_FUNCTION(freeSceneRecord, Verified);

    // ---- setters and getters -------------------------------------------------------

    // setCameraFieldOfView: the field of view set (the projection changed); the old one.
    uint32_t setCameraFieldOfView(ee::Ptr<Camera> camera, uint32_t fieldOfView)
    {
        const int32_t old = camera->fieldOfView;
        // ZGE4 leaves the camera as it is when the field of view is the one it has (no far-plane change marked).
        if constexpr (game::readable::kIsBmx)
            if (old == static_cast<Angle>(fieldOfView))
                return static_cast<uint32_t>(old);
        const uint32_t changed = changedOf(camera) | kCameraFarChanged;
        camera->fieldOfView = static_cast<Angle>(fieldOfView);
        changedOf(camera) = changed;
        return static_cast<uint32_t>(old);
    }
    READABLE_FUNCTION(setCameraFieldOfView, Verified);

    // cameraFieldOfView (no caller): the field of view.
    uint32_t cameraFieldOfView(ee::Ptr<Camera> camera)
    {
        return static_cast<uint32_t>(static_cast<int32_t>(camera->fieldOfView));
    }
    READABLE_FUNCTION(cameraFieldOfView, Verified);

    // setCameraNearClip: the near clip set; the projection marked changed only
    // when it differs.
    void setCameraNearClip(ee::Ptr<Camera> camera, ee::Float nearClip)
    {
        // ZGE2 marks it changed whatever the near clip was.
        if constexpr (game::readable::kIsDm2)
        {
            camera->changedDoubleword() |= kCameraFarChanged;
            camera->nearClip = nearClip;
            return;
        }
        const bool same = camera->nearClip == nearClip;
        camera->nearClip = nearClip;
        changedOf(camera) = (changedOf(camera) & ~uint32_t{kCameraFarChanged}) |
                            (same ? 0u : uint32_t{kCameraFarChanged});
    }
    READABLE_FUNCTION(setCameraNearClip, Verified);

    // setCameraFarPlane: the far clip set, at most 1000; the projection marked
    // changed only when it differs. Returns the flags doubleword.
    uint64_t setCameraFarPlane(ee::Ptr<Camera> camera, ee::Float farClip)
    {
        uint64_t &flags = camera->changedDoubleword();
        // ZGE2 (DM2) takes any distance and always marks the far clip changed; ZGE3 caps it at 1000 and marks it
        // only when it differs.
        if constexpr (game::readable::kIsDm2)
        {
            flags |= kCameraFarChanged;
            camera->farClip = farClip;
            return flags;
        }
        if (ee::Float(1000.0f) < farClip)
            farClip = ee::Float(1000.0f);
        const bool same = camera->farClip == farClip;
        camera->farClip = farClip;
        flags = (flags & ~uint64_t{kCameraFarChanged}) | (same ? 0u : uint64_t{kCameraFarChanged});
        return flags;
    }
    READABLE_FUNCTION(setCameraFarPlane, Verified);

    // cameraNearClip: the near clip.
    ee::Float cameraNearClip(ee::Ptr<Camera> camera)
    {
        return camera->nearClip;
    }
    READABLE_FUNCTION(cameraNearClip, Verified);

    // cameraFarClip: the far clip.
    ee::Float cameraFarClip(ee::Ptr<Camera> camera)
    {
        return camera->farClip;
    }
    READABLE_FUNCTION(cameraFarClip, Verified);

    // setCameraMatrix: the camera's axes and position (camera -> world) and
    // their rigid inverse set.
    void setCameraMatrix(ee::Ptr<Camera> camera, ee::Ptr<Matrix> matrix)
    {
        if constexpr (game::readable::kIsDm2) // ZGE2 copies the rotation and translation alone (no row's w)
            copyMatrixRotationAndTranslation(&camera->toWorld(), matrix);
        else
            loadAxes(&camera->right, matrix);
        matrixInvertRigid(&camera->fromWorld, matrix);
        const uint32_t changed = changedOf(camera);
        if constexpr (!game::readable::kIsDm2) // (DM2 has no such word)
            camera->viewStale = 0;
        changedOf(camera) = changed | kCameraMatrixChanged;
    }
    READABLE_FUNCTION(setCameraMatrix, Verified);

    // setCameraViewport: the camera's screen rectangle (x, y, width, height) and
    // its viewport set to it; the projection changed. 8 (the flag's value
    // left as its result).
    uint32_t setCameraViewport(ee::Ptr<Camera> camera, ee::Address rectangle)
    {
        copyFrom(camera->screen, ee::Ptr<const uint8_t>(rectangle), sizeof(camera->screen));
        swapCameraViewport(camera, rectangle);
        changedOf(camera) |= kCameraFarChanged;
        return kCameraFarChanged;
    }
    READABLE_FUNCTION(setCameraViewport, Verified);

    // sceneViewport: the camera's screen rectangle copied out.
    void sceneViewport(ee::Ptr<Camera> camera, ee::Address rectangle)
    {
        copyTo(ee::Ptr<uint8_t>(rectangle), camera->screen, sizeof(camera->screen));
    }
    READABLE_FUNCTION(sceneViewport, Verified);

    // cameraViewport (ZGE4 only): the camera's viewport rectangle copied out, as sceneViewport copies its screen
    // rectangle (BMX XXX's resetCameraViewports reads it).
    inline void cameraViewport(ee::Ptr<Camera> camera, ee::Address rectangle)
    {
        copyTo(ee::Ptr<uint8_t>(rectangle), camera->viewport, sizeof(camera->viewport));
    }
    READABLE_FUNCTION_ONLY(bmx, cameraViewport);

    // cameraObject: the scene object the camera belongs to.
    uint32_t cameraObject(ee::Ptr<Camera> camera)
    {
        return camera->object;
    }
    READABLE_FUNCTION(cameraObject, Verified);

    // swapCameraViewport: the viewport set (the old one kept at previousViewport); the
    // projection changed. Where the old one is.
    uint32_t swapCameraViewport(ee::Ptr<Camera> camera, ee::Address rectangle)
    {
        copyTo(ee::Ptr<uint8_t>(kPreviousViewport), camera->viewport, sizeof(camera->viewport));
        copyFrom(camera->viewport, ee::Ptr<const uint8_t>(rectangle), sizeof(camera->viewport));
        changedOf(camera) |= kCameraFarChanged;
        return kPreviousViewport;
    }
    READABLE_FUNCTION(swapCameraViewport, Verified);

    // setCameraScales: the screen projection's scales.
    void setCameraScales(ee::Ptr<Camera> camera, ee::Float horizontalScale, ee::Float verticalScale)
    {
        camera->verticalScale = verticalScale;
        camera->horizontalScale = horizontalScale;
    }
    READABLE_FUNCTION(setCameraScales, Verified);

    // setSceneDepthRange: the two depth range values set; the projection changed.
    void setSceneDepthRange(ee::Ptr<Camera> camera, ee::Float first, ee::Float second)
    {
        const uint32_t changed = changedOf(camera);
        camera->depthRange[0] = first;
        camera->depthRange[1] = second;
        changedOf(camera) = changed | kCameraFarChanged;
    }
    READABLE_FUNCTION(setSceneDepthRange, Verified);

    // setCameraClears: whether the camera's frame is cleared; the projection changed.
    void setCameraClears(ee::Ptr<Camera> camera, uint32_t on)
    {
        changedOf(camera) = (changedOf(camera) & ~uint32_t{kCameraClears}) |
                            ((on & 1u) != 0 ? uint32_t{kCameraClears} : 0u) | kCameraFarChanged;
    }
    READABLE_FUNCTION(setCameraClears, Verified);

    // setCameraClearColour: the clear colour (R, G, B, A at colour); the projection changed.
    void setCameraClearColour(ee::Ptr<Camera> camera, uint32_t colour)
    {
        copyFrom(camera->clearColour, ee::Ptr<const uint8_t>(colour), sizeof(camera->clearColour));
        changedOf(camera) |= kCameraFarChanged;
    }
    READABLE_FUNCTION(setCameraClearColour, Verified);

    // setCameraFogColour: the fog colour (R, G, B, A at colour); the fog changed.
    void setCameraFogColour(ee::Ptr<Camera> camera, uint32_t colour)
    {
        copyFrom(camera->fogColour, ee::Ptr<const uint8_t>(colour), sizeof(camera->fogColour));
        changedOf(camera) |= kCameraFogChanged;
    }
    READABLE_FUNCTION(setCameraFogColour, Verified);

    // setCameraFogStart, setCameraFogEnd: where the fog starts and ends. Each returns
    // the flags doubleword.
    uint64_t setCameraFogStart(ee::Ptr<Camera> camera, ee::Float start)
    {
        const uint64_t changed = camera->changedDoubleword() | kCameraFogChanged;
        camera->fogStart = start;
        camera->changedDoubleword() = changed;
        return changed;
    }
    READABLE_FUNCTION(setCameraFogStart, Verified);

    uint64_t setCameraFogEnd(ee::Ptr<Camera> camera, ee::Float end)
    {
        const uint64_t changed = camera->changedDoubleword() | kCameraFogChanged;
        camera->fogEnd = end;
        camera->changedDoubleword() = changed;
        return changed;
    }
    READABLE_FUNCTION(setCameraFogEnd, Verified);

    // moveCameraClipRange (no caller): the fog range moved to end at the far clip
    // and start depth before it; the fog changed. The old range's length.
    ee::Float moveCameraClipRange(ee::Ptr<Camera> camera, ee::Float depth)
    {
        const ee::Float length = camera->fogEnd - camera->fogStart;
        const ee::Float far = cameraFarClip(camera);
        const ee::Float start = far - depth;
        setCameraFogEnd(camera, far);
        setCameraFogStart(camera, start);
        changedOf(camera) |= kCameraFogChanged;
        return length;
    }
    READABLE_FUNCTION(moveCameraClipRange, Verified);

    // ---- the cameras of a kind ----------------------------------------------------------

    // firstCameraOfKind: the first camera of a kind (kept as the first camera),
    // or 0.
    ee::Ptr<Camera> firstCameraOfKind(uint32_t kind)
    {
        const ee::Ptr<Pool> pool = cameraPool();
        if (!pool)
            return {};
        for (ee::Ptr<ListNode> link = firstPoolLink(pool); link; link = nextPoolLink(link))
        {
            const ee::Ptr<Camera> camera = poolRecordAs<Camera>(link);
            if (camera->kind == kind)
            {
                if constexpr (!game::readable::kIsDm2) // (ZGE2 counts no views)
                    *g_firstCamera = camera;
                return camera;
            }
        }
        return {};
    }
    READABLE_FUNCTION(firstCameraOfKind, Verified);

    // nextCameraOfKind: the next camera of the same kind after camera, or 0.
    ee::Ptr<Camera> nextCameraOfKind(ee::Ptr<Camera> camera)
    {
        const uint32_t kind = camera->kind;
        for (ee::Ptr<ListNode> link = nextPoolLink(ee::Ptr<ListNode>(poolLinkOf(camera))); link; link = nextPoolLink(link))
        {
            const ee::Ptr<Camera> next = poolRecordAs<Camera>(link);
            if (next->kind == kind)
                return next;
        }
        return {};
    }
    READABLE_FUNCTION(nextCameraOfKind, Verified);

    // ---- the current camera -----------------------------------------------------------

    // setCurrentCamera: a camera made current (the view count restarting at the
    // first camera) and its frame begun. 1.
    uint32_t setCurrentCamera(ee::Ptr<Camera> camera)
    {
        // ZGE2 counts no views: the camera is made current alone.
        if constexpr (game::readable::kIsDm2)
            *g_currentCamera = camera;
        else
        {
            if (camera == *g_firstCamera)
                *g_currentView = -1;
            const int32_t view = *g_currentView;
            *g_currentCamera = camera;
            *g_currentView = view + 1;
        }
        beginCameraFrame(camera);
        return 1;
    }
    READABLE_FUNCTION(setCurrentCamera, Verified);

    // clearCurrentCamera: the current camera's set-up ended and none current. 1.
    uint32_t clearCurrentCamera()
    {
        endCameraSetup();
        *g_currentCamera = {};
        return 1;
    }
    READABLE_FUNCTION_WAITS(clearCurrentCamera, Verified);

    // draw2DPass: the frame's 2D pass: the 2D camera given the whole
    // screen (in ZGE4 the first view camera's clip planes too) and made current (view 0), render stages 4, 3
    // and 5, then no camera.
    void draw2DPass()
    {
        ee::Local<ScreenRectangle> wholeScreen;
        wholeScreen->x = ee::Float(0.0f);
        wholeScreen->y = ee::Float(0.0f);
        wholeScreen->width = unsignedToFloat(*g_screenWidth);
        wholeScreen->height = unsignedToFloat(*g_screenHeight);
        if constexpr (game::readable::kIsBmx) // ZGE4's 2D camera takes the first view camera's clip planes
            if (const ee::Ptr<Camera> view = firstCameraOfKind(kCameraKindView))
            {
                setCameraFarPlane(*g_overlayCamera, cameraFarClip(view));
                setCameraNearClip(*g_overlayCamera, cameraNearClip(view));
            }
        setCameraViewport(*g_overlayCamera, wholeScreen);
        *g_currentView = 0;
        setCurrentCamera(*g_overlayCamera);
        runRenderStage(4);
        runRenderStage(3);
        runRenderStage(5);
        clearCurrentCamera();
    }
    READABLE_FUNCTION_WAITS(draw2DPass, Verified);

    // overlayCamera (the motion blur's, for a request without a camera): the
    // 2D camera.
    ee::Ptr<Camera> overlayCamera()
    {
        return *g_overlayCamera;
    }
    READABLE_FUNCTION(overlayCamera, Verified);

    // cameraArea, setCameraArea: the area the camera is in.
    uint32_t cameraArea(ee::Ptr<Camera> camera)
    {
        return camera->area;
    }
    READABLE_FUNCTION(cameraArea, Verified);

    void setCameraArea(ee::Ptr<Camera> camera, uint32_t area)
    {
        camera->area = area;
    }
    READABLE_FUNCTION(setCameraArea, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_litoRecords> g_litoRecords;
        constexpr uint32_t kLitoRecords = sym::g_litoRecords;
        inline ee::Global<ee::Ptr<Pool>, sym::g_litoChunkPool> g_litoChunkPool;
        inline ee::Global<int32_t, sym::g_litoChunksReady> g_litoChunksReady;
    }

    uint32_t freeLitoRecord(uint32_t litoRecord);

    uint32_t freeLitoRecords()
    {
        int32_t litoRecords = *g_litoRecords;
        if (litoRecords != 0)
        {
            for (;;)
            {
                freeLitoRecord(litoRecords);
                litoRecords = *g_litoRecords;
                if (litoRecords != 0)
                    continue;
                return 1;
            }
        }
        return 1;
    }

    // freeLitoRecordsToHeap: ZGE2's freeLitoRecords: the first record given
    // back to the heap (heapFree) while there is one. What changed in ZGE3:
    // the records come from a pool (g_litoChunkPool) and leave the list first
    // (freeLitoRecord).
    uint32_t freeLitoRecordsToHeap()
    {
        for (;;)
        {
            const int32_t litoRecord = *g_litoRecords;
            if (litoRecord == 0)
                return 1;
            heapFree(static_cast<uint32_t>(litoRecord));
        }
    }
    READABLE_FUNCTION_OWN(freeLitoRecords, Verified, dm2, freeLitoRecordsToHeap);

    uint32_t freeLitoRecord(uint32_t litoRecord)
    {
        listRemove(ee::Ptr<List>(static_cast<uint32_t>(kLitoRecords)), ee::Ptr<ListNode>(litoRecord));
        poolFree(*g_litoChunkPool, litoRecord);
        return 1;
    }
    READABLE_FUNCTION(freeLitoRecord, Verified);
}
