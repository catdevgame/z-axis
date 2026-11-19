// The renderer (zge/render/renderer.h): set up and shut down (its shadow
// buffers, screen overlay, frame prologue, billboard queue, the frame
// buffers, the default and scene lights' matrices), the frame's end (the
// texture cache moved on, the request queue ended, the statistics kept, the
// frame sent) and a view's packet begun. The frame packets' PS2 form (the
// buffers DMA sends, the fix-ups, the chain's ends) is ps2/frame_packets.cpp.

#include "display.h"
#include "renderer.h"
#include "zge/render/ps2/frame_packets.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "zge/platform/platform.h"
#include "zge/platform/tasks.h"
#include "zge/render/billboards.h"
#include "zge/render/fx_manager.h"
#include "zge/render/primitives.h"
#include "zge/render/shadow.h"
#include "zge/render/texture.h"
#include "zge/scene/camera.h"
#include "zge/debug/profiler.h"
#include "zge/memory/regions.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/render/renderer.cpp.h"

namespace zge
{
    namespace
    {
        constexpr uint32_t kSceVu0NormalLightMatrix = sym::sceVu0NormalLightMatrix;
        constexpr uint32_t kSceVu0LightColorMatrix = sym::sceVu0LightColorMatrix;
        constexpr uint32_t kSceVu0Normalize = sym::sceVu0Normalize;

        // The default lights (ELF data): their directions and colours, the
        // ambient, and the scene lights' start (all one light).
        constexpr uint32_t kDefaultLightDirections[3] = {sym::g_defaultLightDirection0, sym::g_defaultLightDirection1, sym::g_defaultLightDirection2};
        constexpr uint32_t kDefaultLightColours[3] = {sym::g_defaultLightColour0, sym::g_defaultLightColour1, sym::g_defaultLightColour2};
        constexpr uint32_t kDefaultAmbient = sym::defaultAmbient;
        constexpr uint32_t kSceneLightDirections[3] = {sym::g_sceneLightDirection0, sym::g_sceneLightDirection1, sym::g_sceneLightDirection2};
        constexpr uint32_t kSceneLightColour = sym::sceneLightColour, kSceneAmbient = sym::sceneAmbient;
        // The renderer's profile timer's name and group (ELF strings).
        constexpr uint32_t kProfileTimerName = sym::profileTimerName, kProfileTimerGroup = sym::profileTimerGroup;

        // The words from one place in a queue to a later one, as the game's signed sum.
        int32_t wordsBetween(ee::Ptr<uint32_t> from, ee::Ptr<uint32_t> to) { return bytesFrom(from, to) >> 2; }

        // The bytes the frame left free in its buffer.
        uint32_t freePacketBytes(const Renderer &renderer)
        {
            const ee::Ptr<uint8_t> bufferEnd = renderer.meshPacket.bufferStart.cast<uint8_t>() + kFrameBufferBytes;
            return static_cast<uint32_t>(bytesFrom(renderer.meshPacket.current, bufferEnd));
        }
    }

    // ---- the renderer set up and shut down -----------------------------------

    namespace // the game functions below call: ee/plain.h
    {
        void sceVu0NormalLightMatrix(ee::Address m, uint32_t l0, uint32_t l1, uint32_t l2) { ee::game(kSceVu0NormalLightMatrix, m, l0, l1, l2); }
        void sceVu0LightColorMatrix(ee::Address m, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t a) { ee::game(kSceVu0LightColorMatrix, m, c0, c1, c2, a); }
        void sceVu0Normalize(uint32_t out, uint32_t v) { ee::game(kSceVu0Normalize, out, v); }
        namespace tail
        {
        void sceVu0NormalLightMatrix(ee::Address m, uint32_t l0, uint32_t l1, uint32_t l2) { ee::tailCall(kSceVu0NormalLightMatrix, m, l0, l1, l2); }
        }

        // ZGE4's own default lights for the nodes of the first draw bucket
        // (ELF data, as the others: three directions, three colours, the
        // ambient): one white light along y.
        constexpr uint32_t kFirstBucketLightDirections[3] = {sym::bmx::g_firstBucketLightDirection0, sym::bmx::g_firstBucketLightDirection1,
                                                             sym::bmx::g_firstBucketLightDirection2};
        constexpr uint32_t kFirstBucketLightColours[3] = {sym::bmx::g_firstBucketLightColour0, sym::bmx::g_firstBucketLightColour1,
                                                          sym::bmx::g_firstBucketLightColour2};
        constexpr uint32_t kFirstBucketAmbient = sym::bmx::firstBucketAmbient;
    }

    // setUpLightMatrices (ZGE4 only; initRenderer's last light step): the
    // default lights' directions made unit (in the ELF's data, in place) and
    // their matrices made, then the first draw bucket's own lights' likewise,
    // then the scene lights' matrices (not normalized), as ZGE3's initRenderer
    // makes them inline.
    inline void setUpLightMatrices()
    {
        Renderer &renderer = *g_renderer;
        for (uint32_t direction : kDefaultLightDirections)
            sceVu0Normalize(direction, direction);
        sceVu0NormalLightMatrix(&renderer.normalLightMatrix, kDefaultLightDirections[0], kDefaultLightDirections[1], kDefaultLightDirections[2]);
        sceVu0LightColorMatrix(&renderer.lightColourMatrix, kDefaultLightColours[0], kDefaultLightColours[1], kDefaultLightColours[2], kDefaultAmbient);
        for (uint32_t direction : kFirstBucketLightDirections)
            sceVu0Normalize(direction, direction);
        sceVu0NormalLightMatrix(&firstBucketNormalLights(renderer), kFirstBucketLightDirections[0], kFirstBucketLightDirections[1],
                                kFirstBucketLightDirections[2]);
        sceVu0LightColorMatrix(&firstBucketLightColours(renderer), kFirstBucketLightColours[0], kFirstBucketLightColours[1],
                               kFirstBucketLightColours[2], kFirstBucketAmbient);
        sceVu0LightColorMatrix(renderer.globalLightColours, kSceneLightColour, kSceneLightColour, kSceneLightColour, kSceneAmbient);
        tail::sceVu0NormalLightMatrix(renderer.globalLightDirections, kSceneLightDirections[0], kSceneLightDirections[1], kSceneLightDirections[2]);
    }
    READABLE_FUNCTION_ONLY(bmx, setUpLightMatrices);

    namespace
    {
        // ZGE2's word that lets parts be queued for drawing, set with the
        // renderer (no other version has one).
        template <typename R>
        void markDrawingOn([[maybe_unused]] R &renderer)
        {
            if constexpr (requires { renderer.drawingOn; })
                renderer.drawingOn = 1;
        }
    }

    // initRenderer: the renderer set up: its state cleared, the shadow
    // buffers, the screen overlay and the frame prologue made (0 if one
    // fails), the VU1 microprograms sent, DMA tag mismatches masked, the
    // texture uploads' handlers, the two frame buffers, the first frame's
    // packet, the default and scene lights' matrices and the fix-up lists.
    uint32_t initRenderer()
    {
        memoryFill(kRendererAddress, 0, kRendererBytes);
        Renderer &renderer = *g_renderer;
        renderer.ready = 1;
        markDrawingOn(renderer);
        if (allocateShadowBuffers() == 0)
            return 0;
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no frame prologue or screen overlay)
        {
            if (initFramePrologue() == 0)
                return 0;
            if (initScreenOverlay() == 0)
                return 0;
        }
        if constexpr (game::readable::kIsBmx) // (ZGE4's billboard queue)
        {
            if (initBillboardQueue() == 0)
                return 0;
        }
        ps2::loadMicroprograms();
        initUploadBuffers();
        for (uint32_t &buffer : renderer.frameBuffers)
            buffer = allocateIn(kFrameBufferBytes, 0, 1);
        if constexpr (!game::readable::kIsBmx) // (ZGE4: beginFramePackets takes the frame's packet)
            flipMeshPacket();
        if constexpr (!game::readable::kIsDm2) // (ZGE2 keeps no such statistics)
        {
            renderer.leastQueueWords = 0x300;
            renderer.leastPacketBytes = kFrameBufferBytes;
        }
        if constexpr (game::readable::kIsBmx) // ZGE4 normalizes the directions and has a second default pair (setUpLightMatrices)
            setUpLightMatrices();
        else
        {
            sceVu0NormalLightMatrix(&renderer.normalLightMatrix, kDefaultLightDirections[0], kDefaultLightDirections[1], kDefaultLightDirections[2]);
            sceVu0LightColorMatrix(&renderer.lightColourMatrix, kDefaultLightColours[0], kDefaultLightColours[1], kDefaultLightColours[2], kDefaultAmbient);
            sceVu0LightColorMatrix(renderer.globalLightColours, kSceneLightColour, kSceneLightColour, kSceneLightColour, kSceneAmbient);
            if constexpr (!game::readable::kIsDm2) // (ZGE2: the scene lights' colours only)
                sceVu0NormalLightMatrix(renderer.globalLightDirections, kSceneLightDirections[0], kSceneLightDirections[1], kSceneLightDirections[2]);
        }
        if constexpr (!game::readable::kIsDm2) // (ZGE2 has no frame fix-ups)
            newFrameFixups();
        return 1;
    }
    READABLE_FUNCTION_WAITS(initRenderer, Verified);

    // shutDownRenderer: the renderer shut down (as it was set up, the other way
    // round; 0 if a shut-down fails). 1 when it was not set up.
    uint32_t shutDownRenderer()
    {
        Renderer &renderer = *g_renderer;
        if (renderer.ready != 0)
        {
            if constexpr (game::readable::kIsBmx) // (ZGE4's billboard queue first)
            {
                if (freeBillboardQueue() == 0)
                    return 0;
            }
            if constexpr (!game::readable::kIsDm2) // (ZGE2's frees no screen overlay, frame prologue, fix-ups or profile bar)
            {
                if (shutDownScreenOverlay() == 0 || shutDownFramePrologue() == 0)
                    return 0;
            }
            if (freeShadowBuffers() == 0)
                return 0;
            if constexpr (!game::readable::kIsDm2)
                freeFrameFixups();
            freeUploadBuffers();
            if constexpr (!game::readable::kIsDm2)
            {
                const uint32_t timer = renderer.profileTimer;
                if (timer != 0)
                    freeProfileBar(timer);
            }
            for (uint32_t &buffer : renderer.frameBuffers)
            {
                freeIn(buffer, 0);
                buffer = 0;
            }
            renderer.ready = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(shutDownRenderer, Verified);

    // ---- a frame -------------------------------------------------------------

    // finishFrame: the frame built ends (when one was begun): the texture
    // cache moved on, the request queue ended (0) and the frame's request
    // queue, the fewest queue words and free buffer bytes kept, the
    // statistics cleared, the next frame's packet taken, the primitive
    // buffers flipped, the frame's profile timer (made the first time)
    // restarted, and the frame sent; then the primitives' buffers reset and
    // the deferred releases done.
    void finishFrame()
    {
        Renderer &renderer = *g_renderer;
        if (renderer.frameBuffer != 0)
        {
            advanceTextureCache();
            ee::Ptr<uint32_t> ended = renderer.requestQueueEnd;
            if constexpr (!game::readable::kIsBmx) // (ZGE4 leaves the queue's end as it is)
            {
                *ended = 0;
                ended = ended + 1;
                renderer.requestQueueEnd = ended;
            }
            const ee::Ptr<uint32_t> queue = frameRequestQueue(renderer);
            const uint32_t leastWords = renderer.leastQueueWords;
            const uint32_t freeBytes = freePacketBytes(renderer);
            const uint32_t leastBytes = renderer.leastPacketBytes;
            const uint32_t usedWords = static_cast<uint32_t>(wordsBetween(queue, ended));
            renderer.requestQueueRead = queue;
            renderer.leastQueueWords = leastWords < usedWords ? leastWords : usedWords;
            renderer.leastPacketBytes = leastBytes < freeBytes ? leastBytes : freeBytes;
            renderer.materialsSeen = 0;
            renderer.interrupts = 0;
            renderer.texelBytes = 0;
            renderer.clutBytes = 0;
            if constexpr (!game::readable::kIsBmx) // (ZGE4: beginFramePackets)
                flipMeshPacket();
            flipBufferSet();
            if (renderer.profileTimer == 0)
            {
                const uint32_t timer = newProfileBar(0x00010000u, 0, kProfileTimerName, kProfileTimerGroup);
                renderer.profileTimer = timer;
                setProfileBarScale(timer, ee::Float(1.0f));
            }
            endProfileBarFrame(renderer.profileTimer);
            startProfileBar(renderer.profileTimer);
            sendFramePacket(1);
        }
        if constexpr (!game::readable::kIsBmx) // (ZGE4: beginFramePackets)
            resetPrimitiveBuffers();
        releaseDeferred();
    }

    // closeAndFinishFrame: ZGE2's finishFrame, which closes the frame's mesh
    // packet itself (ZGE3's renderFrame does, closeFramePacket): when a frame
    // was begun, the packet ended with NEXT to the screen primitives' packets
    // (END when there are none), aligned and its tag closed (no shadow packet,
    // no FLUSHA); then as ZGE3's, the texture cache moved on, the request
    // queue ended (0) and read from the frame's start, the next frame's packet
    // taken, the primitive buffers flipped and the frame sent, but with no
    // least words or bytes kept and no profile timer; the frame's two counts
    // cleared after the send. Then the primitives' buffers reset and the
    // deferred releases done. 1.
    int32_t closeAndFinishFrame()
    {
        Renderer &renderer = *g_renderer;
        if (renderer.frameBuffer != 0)
        {
            ps2::closeMeshPacket(closeScreenPrimitivePackets());
            advanceTextureCache();
            const ee::Ptr<uint32_t> ended = renderer.requestQueueEnd;
            *ended = 0;
            renderer.requestQueueEnd = ended + 1;
            renderer.requestQueueRead = frameRequestQueue(renderer);
            flipMeshPacket();
            flipBufferSet();
            sendFramePacket(1);
            FrameCounts &counts = *g_frameCounts;
            counts.frame[1] = 0;
            counts.frame[0] = 0;
        }
        resetPrimitiveBuffers();
        releaseDeferred();
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(finishFrame, Verified, dm2, closeAndFinishFrame);

    // summedFrameCounts (ZGE2 only): the frame counts, their totals made the
    // sums of the view's two groups (playFrame's debug line takes the record).
    inline ee::Ptr<FrameCounts> summedFrameCounts()
    {
        FrameCounts &counts = *g_frameCounts;
        for (uint32_t i = 0; i < 3; ++i)
            counts.totals[i] = counts.view[0][i] + counts.view[1][i];
        return ee::Ptr<FrameCounts>(sym::dm2::g_frameCounts);
    }
    READABLE_FUNCTION_ONLY(dm2, summedFrameCounts);

    // endViewDraws (ZGE2 only): a camera's view ended (renderFrame, after its
    // stages 1 and 2): the request queue's end marked (0), as ZGE3's
    // closeFramePacket does once a frame, then the texture cache's camera
    // hook. 1.
    inline int32_t endViewDraws()
    {
        Renderer &renderer = *g_renderer;
        const ee::Ptr<uint32_t> queueEnd = renderer.requestQueueEnd;
        *queueEnd = 0;
        renderer.requestQueueEnd = queueEnd + 1;
        textureCacheCameraHook();
        return 1;
    }
    READABLE_FUNCTION_ONLY(dm2, endViewDraws);

    // beginViewPacket: a view's packet begun (endCameraSetup): the screen
    // overlay drawn, the frame prologue, FLUSHA, and the views' two hooks.
    void beginViewPacket()
    {
        Renderer &renderer = *g_renderer;
        renderer.vu1StateOwner = 0;
        buildFogPlanes();
        drawBlur(ee::Ptr<Camera>(renderer.framePrologue), &renderer.meshPacket);
        ps2::waitForFramePrologue();
        textureCacheCameraHook();
        if constexpr (game::readable::kIsBmx) // (ZGE4's queued billboards, after the scene)
            drawQueuedBillboards();
        endViewOverlay();
    }
    READABLE_FUNCTION_WAITS(beginViewPacket, Verified);
}
