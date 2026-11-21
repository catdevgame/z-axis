// The screen overlay's and the frame prologue's flags, and the motion blur
// (the initScreenOverlay-addGsRegister2 part of the frame block): FSMotionBlur, the effect
// manager's full-screen effect (zfxmgr), sets a camera's blur; the frame
// prologue draws a blurred camera's last frame over its view, slightly zoomed
// and at half its colour (on the PS2: drawBlur and addMotionBlurSprite,
// ps2/motion_blur.cpp).

#include "zge/memory/regions.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/render/display.h"
#include "zge/render/draw.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/text/cstring.h"
#include "zge/render/fx_manager.h"
#include "zge/render/material.h"
#include "zge/render/primitives.h"
#include "zge/render/renderer.h"
#include "zge/container/cursor_array.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/zge/render/screen_effects.cpp.h"

namespace zge
{
    namespace
    {
        // The screen overlay (drawn by buildFogPlanes when on) and the frame
        // prologue (drawBlur): each on and set-up flag.
        inline ee::Global<uint32_t, sym::g_screenOverlayReady> g_screenOverlayReady;
        inline ee::Global<uint32_t, sym::g_screenOverlayOn> g_screenOverlayOn;
        inline ee::Global<uint32_t, sym::g_framePrologueReady> g_framePrologueReady;
        inline ee::Global<uint32_t, sym::g_framePrologueOn> g_framePrologueOn;

        constexpr uint32_t kRegisterFullScreenEffect = sym::registerFullScreenEffect; // (name, descriptor): the effect manager's
        constexpr uint32_t kMotionBlurName = sym::motionBlurName;           // "FSMotionBlur"
    }

    // initScreenOverlay (initRenderer), shutDownScreenOverlay (shutDownRenderer): the
    // screen overlay on and set up; its set-up flag cleared. Always 1.
    uint32_t initScreenOverlay()
    {
        *g_screenOverlayOn = 1;
        *g_screenOverlayReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initScreenOverlay, Verified);

    uint32_t shutDownScreenOverlay()
    {
        if (*g_screenOverlayReady != 0)
            *g_screenOverlayReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutDownScreenOverlay, Verified);

    // initFramePrologue, shutDownFramePrologue: the same for the frame prologue.
    uint32_t initFramePrologue()
    {
        *g_framePrologueOn = 1;
        *g_framePrologueReady = 1;
        return 1;
    }
    READABLE_FUNCTION(initFramePrologue, Verified);

    uint32_t shutDownFramePrologue()
    {
        if (*g_framePrologueReady != 0)
            *g_framePrologueReady = 0;
        return 1;
    }
    READABLE_FUNCTION(shutDownFramePrologue, Verified);

    // ---- the fog planes -------------------------------------------------------

    namespace
    {
        // The strip of one fog plane, drawn at eight depths: four corners of
        // the viewport with the camera's fog colour (drawPrimitives').
        struct FogVertex
        {
            ee::Float x, y, z; // +0x00
            uint8_t _0c[0x0C];
            uint8_t colour[4]; // +0x18
            uint8_t _1c[0x04];
        };
        static_assert(sizeof(FogVertex) == 0x20);
        constexpr uint32_t kFogVertices = sym::g_fogQuadCorner0X;
        inline ee::Global<FogVertex[4], kFogVertices> g_fogCorners;
        constexpr uint32_t kFogStrip = sym::fogStrip;   // its material first
        constexpr uint32_t kFogStripFlags = 0x02000004u;
        constexpr int kFogPlanes = 8;
        inline ee::Global<uint32_t, sym::g_fogMaterial> g_fogMaterial; // made on the first use
        inline ee::Global<uint32_t, kFogStrip> g_fogStripMaterial;
    }

    // buildFogPlanes (with the screen overlay on, a view camera): eight planes
    // of fog across the viewport from the far clip (or the fog's end, if
    // nearer) towards the fog's start, an eighth of start to end apart. It
    // returns what it last read or made (the overlay flag, the camera, the
    // material, drawPrimitives' result), as the original does.
    int32_t buildFogPlanes()
    {
        int32_t result = static_cast<int32_t>(*g_screenOverlayOn);
        if (result != 0)
        {
            const ee::Ptr<Camera> current(*g_currentCamera);
            const Camera &camera = *current;
            result = static_cast<int32_t>(*g_currentCamera);
            if (camera.kind != 0)
                return result;
            result = static_cast<int32_t>(*g_fogMaterial);
            if (result == 0)
            {
                const ee::Ptr<Material> material = newMaterial();
                *g_fogMaterial = ee::Address(material);
                zgui::rebuildMaterialRegisters(material, 0, 4, 0);
                result = static_cast<int32_t>(*g_fogMaterial);
                *g_fogStripMaterial = result;
            }
            const ee::Float fogEnd = camera.fogEnd;
            const ee::Float fogStart = camera.fogStart;
            const ee::Float farthest = ee::min(fogEnd, camera.farClip);
            if (farthest <= fogStart)
                return result;
            FogVertex *const corners = *g_fogCorners;
            const ee::Float left = camera.viewport[0], top = camera.viewport[1];
            const ee::Float right = camera.viewport[2] + left;
            ee::Float bottom = camera.viewport[3] + top;
            if constexpr (game::readable::kIsBmx) // (ZGE4's planes end a pixel above the viewport's bottom)
                bottom = bottom - ee::Float(1.0f);
            const ee::Float xs[4] = {right, left, left, right}, ys[4] = {top, top, bottom, bottom};
            for (int corner = 0; corner < 4; ++corner)
            {
                corners[corner].x = xs[corner];
                corners[corner].y = ys[corner];
                std::memcpy(corners[corner].colour, camera.fogColour, sizeof(camera.fogColour));
            }
            const ee::Float step = (fogEnd - fogStart) * ee::Float(0.125f);
            ee::Float depth = farthest - step;
            for (int planesLeft = kFogPlanes - 1;;)
            {
                for (int corner = 0; corner < 4; ++corner)
                    corners[corner].z = depth;
                --planesLeft;
                result = drawPrimitives(kFogStripFlags, kFogStrip, 1, 0);
                if (planesLeft < 0)
                    break;
                depth = depth - step;
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(buildFogPlanes, Verified);

    // ---- the camera's motion blur --------------------------------------------

    // setCameraMotionBlur: a camera's motion blur: on when `colour` (RGBA; 0: none)
    // has an alpha of 8 or more, then the four values kept clamped (the
    // source's scales to 0 .. 1, the other two to -1 .. 1) and the colour
    // copied.
    void setCameraMotionBlur(ee::Ptr<Camera> camera, ee::Ptr<uint8_t> colour, ee::Float scaleX, ee::Float scaleY,
                                 ee::Float extraX, ee::Float extraY)
    {
        if (colour)
        {
            if (colour[3] >= 8)
            {
                const ee::Float one(1.0f);
                const ee::Float zero(0.0f);
                const ee::Float minusOne(-1.0f);
                camera->motionBlurOn = 1;
                camera->motionBlurScale[0] = ee::max(ee::min(scaleX, one), zero);
                camera->motionBlurScale[1] = ee::max(ee::min(scaleY, one), zero);
                camera->motionBlurExtra[0] = ee::max(ee::min(extraX, one), minusOne);
                camera->motionBlurExtra[1] = ee::max(ee::min(extraY, one), minusOne);
                // (LWL / LWR: the colour need not be aligned.)
                uint32_t word;
                std::memcpy(&word, &colour[0], 4);
                std::memcpy(camera->motionBlurColour, &word, 4);
                return;
            }
        }
        camera->motionBlurOn = 0;
    }
    READABLE_FUNCTION(setCameraMotionBlur, Verified);

    namespace
    {
        // A request to the effect (startFullScreenEffect's, setEffectValue's).
        struct MotionBlurRequest
        {
            uint8_t _00[4];
            uint8_t strength;     // +0x04 the colour's alpha
            uint8_t _05[3];
            uint8_t colour[4];    // +0x08 RGBA
            ee::Float scale[2];   // +0x0C setCameraMotionBlur's scales
            ee::Float extra[2];   // +0x14 its last two
            uint32_t camera;      // +0x1C 0: the 2D camera
        };
        static_assert(offsetof(MotionBlurRequest, colour) == 8 && offsetof(MotionBlurRequest, camera) == 0x1C);

        // A running effect (the effect manager's pool record), as the
        // effect's methods use it.
        struct MotionBlurEffect
        {
            uint8_t _00[0x0C];
            uint32_t camera;      // +0x0C the blurred camera
        };
    }

    namespace tail
    {
        // setCameraMotionBlur by address: the effect's methods return what the
        // void function leaves in v0 (the original's tail call).
        uint32_t setCameraMotionBlurKeepingResult(uint32_t camera, ee::Address colour, ee::Float scaleX, ee::Float scaleY, ee::Float extraX,
                                                  ee::Float extraY)
        {
            return ee::tailCall<uint32_t>(sym::setCameraMotionBlur, camera, colour, scaleX, scaleY, extraX, extraY);
        }
    }

    // startMotionBlur (FSMotionBlur's start): the request's camera (or the
    // 2D camera) blurred with its values, the strength as the colour's
    // alpha; kept as the effect's camera. setCameraMotionBlur's result (the
    // original's tail call).
    uint32_t startMotionBlur(ee::Ptr<MotionBlurEffect> effect, ee::Ptr<MotionBlurRequest> request)
    {
        uint32_t camera = request->camera;
        if (camera == 0)
            camera = ee::Address(overlayCamera()); // the request's and effect's camera words
        effect->camera = camera;
        request->colour[3] = request->strength;
        return tail::setCameraMotionBlurKeepingResult(camera, request->colour, request->scale[0], request->scale[1], request->extra[0], request->extra[1]);
    }
    READABLE_FUNCTION(startMotionBlur, Verified);

    // setMotionBlur (FSMotionBlur's set-value method, setEffectValue): the
    // request's camera (or the 2D camera) given the new values: its blur
    // turned off first, then set as at the start. setCameraMotionBlur's
    // result (the original's tail call).
    uint32_t setMotionBlur(ee::Ptr<MotionBlurEffect> effect, ee::Ptr<MotionBlurRequest> request)
    {
        uint32_t camera = request->camera;
        if (camera == 0)
            camera = ee::Address(overlayCamera()); // the request's and effect's camera words
        setCameraMotionBlur(ee::Ptr<Camera>(camera), ee::Ptr<uint8_t>(), ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f));
        effect->camera = camera;
        request->colour[3] = request->strength;
        return tail::setCameraMotionBlurKeepingResult(camera, request->colour, request->scale[0], request->scale[1], request->extra[0], request->extra[1]);
    }
    READABLE_FUNCTION(setMotionBlur, Verified);

    // stopMotionBlur (FSMotionBlur's stop): the effect's camera unblurred
    // (the original's tail call).
    uint32_t stopMotionBlur(ee::Ptr<MotionBlurEffect> effect)
    {
        return tail::setCameraMotionBlurKeepingResult(effect->camera, 0u, ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f), ee::Float(0.0f));
    }
    READABLE_FUNCTION(stopMotionBlur, Verified);

    namespace
    {
        // A full-screen effect's class as registerFullScreenEffect takes it
        // (it keeps a copy: the name's hash written at +0x08, +0x14 cleared,
        // the flags' bits 3-12 cleared, then the hook at +0x18 called if
        // set). Its methods are game functions.
        struct FullScreenEffectClass
        {
            uint8_t _00[8];
            uint64_t hashAndFlags;  // +0x08 the name's hash, the flags word in the upper half
            uint8_t _10[0x10];
            uint32_t start;         // +0x20
            uint8_t _24[8];         // +0x24 two more methods (the lens flare sets +0x28)
            uint32_t stop;          // +0x2C
            uint32_t setValue;      // +0x30
        };
        // Its bytes (the struct rounds up to the doubleword's alignment).
        constexpr uint32_t kEffectClassBytes = 0x34;
        static_assert(offsetof(FullScreenEffectClass, setValue) + sizeof(uint32_t) == kEffectClassBytes);
    }

    namespace // the game functions below call: ee/plain.h
    {
        void registerFullScreenEffect(uint32_t name, ee::Ptr<FullScreenEffectClass> effect) { ee::game(kRegisterFullScreenEffect, name, effect); }
    }

    // registerMotionBlurEffect (initEffectManager): FSMotionBlur registered: a
    // descriptor (52 bytes, cleared twice as compiled) with its start, stop
    // and set-value methods, kEffectHandsBackRun set and kEffectUpdates
    // clear (the flags word written as the doubleword with the name hash). (The original also
    // stores 0 at +0x24, which the second clear has already made 0.)
    void registerMotionBlurEffect()
    {
        // FullScreenEffectFlags in the doubleword's upper half.
        constexpr uint64_t kHandsBackRun = uint64_t(kEffectHandsBackRun) << 32, kUpdates = uint64_t(kEffectUpdates) << 32;
        ee::Local<FullScreenEffectClass> effect;
        memoryFill(effect, 0, kEffectClassBytes);
        effect->hashAndFlags = effect->hashAndFlags & ~kHandsBackRun & ~kUpdates;
        memoryFill(effect, 0, kEffectClassBytes);
        const uint64_t flags = (effect->hashAndFlags | kHandsBackRun) & ~kUpdates;
        effect->stop = sym::stopMotionBlur;
        effect->hashAndFlags = flags;
        effect->start = sym::startMotionBlur;
        effect->setValue = sym::setMotionBlur;
        registerFullScreenEffect(kMotionBlurName, effect);
    }
    READABLE_FUNCTION_WAITS(registerMotionBlurEffect, Verified);

    namespace
    {
        constexpr uint32_t kArrayFirst = sym::arrayFirst; // arrayFirst ((array) -> the first item, or 0)
        constexpr uint32_t kArrayNext = sym::arrayNext; // arrayNext ((array) -> the next item, or 0)
        inline ee::Global<int32_t, sym::g_effectTypeNames> g_effectTypeNames;
        inline ee::Global<int32_t, sym::g_namedEffectClasses> g_namedEffectClasses; // g_namedEffectClasses ((anonymous)::g_namedEffectClasses)
        inline ee::Global<int32_t, sym::g_effectArraysReady> g_effectArraysReady;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_particleTypes> g_particleTypes;
        inline ee::Global<ee::Ptr<PointerArray>, sym::g_particleBehaviours> g_particleBehaviours;
    }

    namespace // the game functions below call: ee/plain.h
    {
        int32_t arrayFirst(uint32_t pointerArray) { return ee::game<int32_t>(kArrayFirst, pointerArray); }
        int32_t arrayNext(uint32_t pointerArray) { return ee::game<int32_t>(kArrayNext, pointerArray); }
    }

    uint32_t effectAllocate(uint32_t bytes)
    {
        return heapAllocate(bytes);
    }
    READABLE_FUNCTION_WAITS(effectAllocate, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        // A particle kind's registration (registerSparkParticles and the like), by address.
        void registerParticleKind(uint32_t registration) { ee::game(registration); }
    }

    // initEffectArrays: the particle types' and behaviours' lists made. ZGE4 registers its three particle kinds here
    // (ZGE3's game registers them).
    int32_t initEffectArrays()
    {
        *g_particleTypes = newCursorArray(0x40);
        *g_particleBehaviours = newCursorArray(0x10);
        *g_effectArraysReady = 1;
        if constexpr (game::readable::kIsBmx)
        {
            registerParticleKind(sym::registerSparkParticles);
            registerParticleKind(sym::registerBillboardParticles);
            registerParticleKind(sym::registerPolyParticles);
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(initEffectArrays, Verified);

}
