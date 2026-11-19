// The camera's projection and view frustum (zge/scene/camera.h).

#include "camera.h"

#include "ee/dispatch.h"
#include "ee/io.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vector.h"
#include "zge/math/vec.h"
#include "zge/math/matrix.h"
#include "zge/render/display.h"
#include "zge/render/gs_registers.h"
#include "zge/render/vif_codes.h"
#include "zge/render/renderer.h"
#include "sce/packet_calls.h"
#include "zge/scene/node.h"
#include "zge/render/draw.h"
#include "zge/render/billboards.h"
#include "zge/render/primitives.h"
#include "zge/render/texture.h"
#include "sce/libgcc.h"
#include "zge/math/angle.h"
#include "zge/math/vu0_ops.h"
#include "zge/host/game_view.h"

#include <bit>
#include <cstring>
#include <utility>
#include "sym/zge/scene/camera.cpp.h"

namespace zge
{
    namespace
    {
        using ee::Float;

        // The engine's trigonometry on 16-bit angles (zge/math/angle.h).
        // Called through the game's function table, not game::call: the
        // widescreen override wraps angleSin's entry to widen the frustum
        // (aggressive_inline_slus20327.cpp).
        constexpr uint32_t kAngleSin = sym::angleSin;
        constexpr uint32_t kAngleCos = sym::angleCos;
        constexpr uint32_t kAngleTan = sym::angleTan;  // tanf of an angle
        // libvu0 (the Sony VU0 library linked into the ELF).
        constexpr uint32_t kVu0UnitMatrix = sym::sceVu0UnitMatrix; // sceVu0UnitMatrix(m)
        constexpr uint32_t kVu0MulMatrix = sym::sceVu0MulMatrix;  // sceVu0MulMatrix(out, a, b): out = b * a (row vectors)

        // 1023.5, the guard band's half size in GS
        // pixels: the clip volume reaches this far either side of the
        // viewport's centre (drawSkinned and the other clipping
        // renderers drop what leaves it).
        constexpr Float kGuardBand = std::bit_cast<float>(0x447FE000u);
        // VU1 turns the projected depth into GS Z with FTOI4 (x16); the
        // projection's depth is made / 16 to come out as the display's.
        constexpr Float kOneSixteenth = 0.0625f;

    }

    // ---- the projection ---------------------------------------------------------

    namespace // the game functions below call: ee/plain.h
    {
        void sceVu0UnitMatrix(ee::Ptr<Matrix> out) { ee::game(kVu0UnitMatrix, out); }
        void sceVu0MulMatrix(ee::Ptr<Matrix> out, ee::Ptr<Matrix> left, ee::Ptr<Matrix> right) { ee::game(kVu0MulMatrix, out, left, right); }
    }

    namespace
    {
        // The guard band's half size as ZGE2 writes it: a double (libgcc's soft double, sce/libgcc.h).
        constexpr uint64_t kGuardBandDouble = std::bit_cast<uint64_t>(1023.5);

        // The guard band's half size at the near clip, in view units: near * 1023.5 / focal. ZGE2 works it out
        // in double, ZGE3 in float.
        Float guardBandAtNear(Float nearClip, Float focal)
        {
            if constexpr (game::readable::kIsDm2)
            {
                const uint64_t nearTimesBand = sce::libgcc::dpmul(sce::libgcc::fptodp(nearClip), kGuardBandDouble);
                return sce::libgcc::dptofp(sce::libgcc::dpdiv(nearTimesBand, sce::libgcc::fptodp(focal)));
            }
            else
                return (nearClip * kGuardBand) / focal;
        }
    }

    void updateProjection(ee::Ptr<Camera> camera)
    {
        const Camera &c = *camera;
        const DisplaySetup &display = *g_display;

        // The viewport's centre on the GS's drawing area: the display's
        // centre, moved by the viewport's offset from the screen's centre
        // (a split-screen view's half); vertically halved for field frames.
        // ZGE2 puts every view's centre on the display's (no split-screen offset, no field halving).
        Float centreX, centreY;
        if constexpr (game::readable::kIsDm2)
        {
            centreX = display.centreX;
            centreY = display.centreY;
        }
        else
        {
            const Float viewportCentreX = c.viewport[0] + c.viewport[2] * 0.5f;
            const Float screenCentreX = c.screen[0] + c.screen[2] * 0.5f;
            const Float viewportCentreY = c.viewport[1] + c.viewport[3] * 0.5f;
            const Float screenCentreY = c.screen[1] + c.screen[3] * 0.5f;
            centreX = display.centreX + (viewportCentreX - screenCentreX);
            Float offsetY = viewportCentreY - screenCentreY;
            if (display.flags & kDisplayField)
                offsetY = offsetY * 0.5f;
            centreY = display.centreY + offsetY;
        }

        const Float focal = c.focalLength; // the last frame's (updateFrustum runs after this)
        const Float xScale = c.horizontalScale;
        const Float yScale = c.verticalScale;
        const Float nearClip = c.nearClip;
        const Float farClip = c.farClip;

        // Depth: z / w goes from depthAtNear at the near clip to depthAtFar
        // at the far one, as depthOffset + depthScale / z.
        const Float depthAtFar = display.depthAtFar * kOneSixteenth;
        const Float depthAtNear = display.depthAtNear * kOneSixteenth;
        const Float farTimesNear = farClip * nearClip;
        const Float depthScale = (farTimesNear * (depthAtNear - depthAtFar)) / (farClip - nearClip);
        const Float depthOffset = ((-depthAtNear) * nearClip + depthAtFar * farClip) / (farClip - nearClip);
        // The guard band's half size at the near clip, in view units. ZGE2 works it out again for the clip
        // volume's y scale (the same value).
        const Float guardAtNear = guardBandAtNear(nearClip, focal);
        const Float guardUpAtNear = game::readable::kIsDm2 ? guardBandAtNear(nearClip, focal) : guardAtNear;

        // screenProjection = perspective * viewport. The perspective:
        // (x, y, z, 1) -> (focal x, focal y, 1, z).
        const ee::Ptr<Matrix> screen = &camera->screenProjection;
        sceVu0UnitMatrix(screen);
        screen->m[1][1] = focal;
        screen->m[0][0] = focal;
        screen->m[3][2] = 1.0f;
        screen->m[2][3] = 1.0f;
        screen->m[2][2] = 0.0f;
        screen->m[3][3] = 0.0f;
        ee::Local<Matrix> viewport;
        sceVu0UnitMatrix(ee::Ptr<Matrix>(viewport.address()));
        // The viewport: x and y scaled about the centre, depth as above
        // (w stays z for the GS's perspective divide).
        viewport->m[0][0] = xScale;
        viewport->m[1][1] = yScale;
        viewport->m[2][2] = depthScale;
        viewport->m[3][0] = centreX;
        viewport->m[3][1] = centreY;
        viewport->m[3][2] = depthOffset;
        sceVu0MulMatrix(screen, ee::Ptr<Matrix>(viewport.address()), screen);

        // clipProjection: the guard band's frustum mapped onto the clip
        // volume (-w..w on each axis), as glFrustum(-g, g, -g, g, near,
        // far) with g = guardAtNear.
        const ee::Ptr<Matrix> clip = &camera->clipProjection;
        sceVu0UnitMatrix(clip);
        const Float depth = farClip - nearClip;
        const Float guardWidth = guardAtNear + guardAtNear;
        clip->m[2][3] = 1.0f;
        clip->m[3][3] = 0.0f;
        const Float scale = (nearClip + nearClip) / guardWidth;
        clip->m[3][2] = (farTimesNear * -2.0f) / depth;
        clip->m[2][2] = (farClip + nearClip) / depth;
        clip->m[1][1] = game::readable::kIsDm2 ? (nearClip + nearClip) / (guardUpAtNear + guardUpAtNear) : scale;
        clip->m[0][0] = scale;
    }
    READABLE_FUNCTION(updateProjection, Verified);

    // ---- the frustum ------------------------------------------------------------

    namespace // the game functions below call: ee/plain.h
    {
        Float angleTan(uint32_t angle) { return ee::game<Float>(kAngleTan, angle); }
        Float angleSin(uint32_t angle) { return ee::game<Float>(kAngleSin, angle); }
        Float angleCos(uint32_t angle) { return ee::game<Float>(kAngleCos, angle); }

        // The rectangle the field of view spans: ZGE2's is the screen rectangle (setCameraViewport's), ZGE3's the
        // viewport (which swapCameraViewport alone can change).
        const Float *spannedRectangle(const Camera &c) { return game::readable::kIsDm2 ? c.screen : c.viewport; }
    }

    namespace
    {
        // The game's own frustum (zge/host/game_view.h): the world planes the
        // frustum below makes, with the game's half-angle tangents where the
        // drawn view has its own (widescreen and a field of view setting
        // change them). The same steps as
        // the drawn one's, on the camera's position and axes, done between
        // VU0's registers kept and put back, so the frustum leaves them as
        // it does without this.
        void keepGameFrustum(ee::Ptr<Camera> camera, Float tanX, Float tanY, const host::GamePose *pose)
        {
            Camera &c = *camera;
            vu0::KeptRegisters kept;
            // The game camera's axes and position (camera -> world).
            Matrix toWorld = c.toWorld();
            if (pose)
                std::memcpy(&toWorld, pose->toWorld, sizeof(toWorld));
            Vec4 viewCorners[kFrustumCorners];
            {
                const Float z = c.nearClip;
                const Float x = z * tanX, y = z * tanY;
                viewCorners[0].x = -x, viewCorners[0].y = -y, viewCorners[0].z = z;
                viewCorners[1].x = x, viewCorners[1].y = -y, viewCorners[1].z = z;
                viewCorners[2].x = x, viewCorners[2].y = y, viewCorners[2].z = z;
                viewCorners[3].x = -x, viewCorners[3].y = y, viewCorners[3].z = z;
            }
            {
                const Float z = c.farClip;
                const Float x = z * tanX, y = z * tanY;
                viewCorners[4].x = -x, viewCorners[4].y = -y, viewCorners[4].z = z;
                viewCorners[5].x = x, viewCorners[5].y = -y, viewCorners[5].z = z;
                viewCorners[6].x = x, viewCorners[6].y = y, viewCorners[6].z = z;
                viewCorners[7].x = -x, viewCorners[7].y = y, viewCorners[7].z = z;
            }
            Vec4 corners[kFrustumCorners];
            for (int i = 0; i < kFrustumCorners; ++i)
            {
                corners[i] = rotated(toWorld, viewCorners[i]);
                corners[i] += toWorld.row(3);
            }
            static_assert(sizeof(host::GameFrustum::planes) == sizeof(Plane) * kFrustumPlanes);
            host::GameFrustum frustum{};
            frustum.fromWorld = c.fromWorld;
            frustum.position = toWorld.row(3);
            frustum.elsewhere = pose != nullptr;
            if (pose)
                std::memcpy(&frustum.fromWorld, pose->fromWorld, sizeof(frustum.fromWorld));
            frustum.tanHalfWidth = tanX;
            frustum.tanHalfHeight = tanY;
            frustum.sphereScaleX = ee::sqrt(tanX * tanX) + 1.0f;
            frustum.sphereScaleY = ee::sqrt(tanY * tanY) + 1.0f;
            frustum.planeCount = c.viewPlaneCount;
            const FrustumPlaneCorners *planeCorners = *g_frustumPlaneCorners;
            for (uint32_t i = 0; i < frustum.planeCount; ++i)
            {
                const FrustumPlaneCorners &each = planeCorners[i];
                const Vec4 &a = corners[each.corner[0]];
                const Vec4 alongFirst = corners[each.corner[1]] - a;
                const Vec4 alongSecond = corners[each.corner[2]] - corners[each.corner[1]];
                frustum.planes[i] = unit(cross(alongFirst, alongSecond));
                frustum.planes[i].w = dot(frustum.planes[i], a);
            }
            host::keepGameFrustum(camera, frustum);
        }
    }

    bool gameCameraElsewhere(ee::Ptr<const Camera> camera)
    {
        const host::GameFrustum *frustum = host::gameFrustum(camera);
        return frustum && frustum->elsewhere;
    }

    Vec4 gameCameraPosition(ee::Ptr<const Camera> camera)
    {
        const host::GameFrustum *frustum = host::gameFrustum(camera);
        return frustum && frustum->elsewhere ? frustum->position : camera->position;
    }

    void updateFrustum(ee::Ptr<Camera> camera)
    {
        Camera &c = *camera;
        // The focal length that makes the field of view span the
        // viewport's width, and the half-angle tangents.
        const Angle halfWidthAngle = static_cast<Angle>(c.fieldOfView / 2);
        // ZGE2 halves the angle for the tangent by a shift (rounding down), ZGE3 by division (towards zero).
        const Angle tanAngle = game::readable::kIsDm2 ? static_cast<Angle>(c.fieldOfView >> 1) : halfWidthAngle;
        const Float tanHalfAngle = angleTan(tanAngle);
        const Float halfWidth = spannedRectangle(c)[2] * 0.5f;
        c.viewPlanes[kPlaneNear].x = 0.0f;
        c.viewPlanes[kPlaneNear].y = 0.0f;
        c.viewPlanes[kPlaneNear].z = 1.0f;
        c.viewPlanes[kPlaneFar].x = 0.0f;
        c.viewPlanes[kPlaneFar].y = 0.0f;
        c.viewPlanes[kPlaneFar].z = -1.0f;
        const Float focal = halfWidth / tanHalfAngle;
        const Float tanHalfWidth = halfWidth / focal;
        c.focalLength = focal;
        c.tanHalfWidth = tanHalfWidth;
        c.cotHalfWidth = 1.0f / tanHalfWidth;

        // The side planes' normals from the horizontal half angle; the
        // widescreen override changes tanHalfWidth during the first call
        // (so it is read again below, as the original does).
        const Float sinX = angleSin(halfWidthAngle);
        const Float cosX = angleCos(halfWidthAngle);
        const Float halfHeight = spannedRectangle(c)[3] * 0.5f;
        c.viewPlanes[kPlaneRight].z = sinX;
        c.viewPlanes[kPlaneLeft].z = sinX;
        c.viewPlanes[kPlaneLeft].y = 0.0f;
        c.viewPlanes[kPlaneRight].y = 0.0f;
        const Float tanHalfHeight = halfHeight / c.focalLength;
        c.viewPlanes[kPlaneLeft].x = cosX;
        c.viewPlanes[kPlaneRight].x = -cosX;
        c.tanHalfHeight = tanHalfHeight;
        c.cotHalfHeight = 1.0f / tanHalfHeight;

        // The vertical half angle, from its tangent, for the top and
        // bottom planes' normals.
        const Angle halfHeightAngle = angleAtan(tanHalfHeight);
        const Float sinY = angleSin(halfHeightAngle);
        const Float cosY = angleCos(halfHeightAngle);
        const Float tanX = c.tanHalfWidth;
        c.viewPlanes[kPlaneBottom].y = cosY;
        c.viewPlanes[kPlaneBottom].z = sinY;
        c.viewPlanes[kPlaneTop].x = 0.0f;
        c.viewPlanes[kPlaneTop].y = -cosY;
        c.viewPlanes[kPlaneTop].z = sinY;
        c.viewPlanes[kPlaneBottom].x = 0.0f;
        c.sphereScaleX = ee::sqrt(tanX * tanX) + 1.0f;
        const Float tanY = c.tanHalfHeight;
        c.sphereScaleY = ee::sqrt(tanY * tanY) + 1.0f;

        // The corners in view space: the viewport's edges at the near and
        // the far clip (w is left as it was).
        {
            const Float z = c.nearClip;
            const Float x = z * tanX, y = z * tanY;
            Vec4 *nearCorners = &c.viewCorners[kCornerNearTopLeft];
            nearCorners[0].x = -x, nearCorners[0].y = -y, nearCorners[0].z = z;
            nearCorners[1].x = x, nearCorners[1].y = -y, nearCorners[1].z = z;
            nearCorners[2].x = x, nearCorners[2].y = y, nearCorners[2].z = z;
            nearCorners[3].x = -x, nearCorners[3].y = y, nearCorners[3].z = z;
        }
        {
            const Float z = c.farClip;
            const Float x = z * c.tanHalfWidth, y = z * c.tanHalfHeight;
            Vec4 *farCorners = &c.viewCorners[kCornerFarTopLeft];
            farCorners[0].x = -x, farCorners[0].y = -y, farCorners[0].z = z;
            farCorners[1].x = x, farCorners[1].y = -y, farCorners[1].z = z;
            farCorners[2].x = x, farCorners[2].y = y, farCorners[2].z = z;
            farCorners[3].x = -x, farCorners[3].y = y, farCorners[3].z = z;
        }
        c.viewPlaneCount = kFrustumPlanes;

        // Each view plane's distance: its normal's dot product with the
        // plane's first corner.
        const FrustumPlaneCorners *planeCorners = *g_frustumPlaneCorners;
        for (uint32_t i = 0; i < c.viewPlaneCount; ++i)
            c.viewPlanes[i].w = dot(c.viewPlanes[i], c.viewCorners[planeCorners[i].corner[0]]);

        // The corners into the world: camera -> world (the camera's axes,
        // then its position). A rotated corner's w is what the last rotation
        // left, here or before this function, and stays in the world corner.
        for (int i = 0; i < kFrustumCorners; ++i)
        {
            c.corners[i] = rotated(c.toWorld(), c.viewCorners[i]);
            c.corners[i] += c.position;
        }

        // The world's planes from the world's corners: the unit normal of
        // (b - a) x (third - b), w its dot product with a.
        c.planeCount = c.viewPlaneCount;
        for (uint32_t i = 0; i < c.planeCount; ++i)
        {
            const FrustumPlaneCorners &corners = planeCorners[i];
            const Vec4 &a = c.corners[corners.corner[0]];
            const Vec4 &b = c.corners[corners.corner[1]];
            const Vec4 &third = c.corners[corners.corner[2]];
            const Vec4 alongFirst = b - a;
            const Vec4 alongSecond = third - b;
            c.planes[i] = cross(alongFirst, alongSecond);
            c.planes[i] = unit(c.planes[i]);
            c.planes[i].w = dot(c.planes[i], a);
        }

        // The drawn view is not the game's (the view settings' override changed
        // the tangents or the focal length at the first sine: widescreen, a
        // field of view setting): the game's own frustum kept for what the
        // game decides by it.
        const Float gameTanHalfHeight = halfHeight / focal;
        host::GamePose pose;
        const bool elsewhere = host::takeGamePose(camera, pose); // (a hidden camera's, kept this frame)
        if (c.tanHalfWidth != tanHalfWidth || c.tanHalfHeight != gameTanHalfHeight || elsewhere)
            keepGameFrustum(camera, tanHalfWidth, gameTanHalfHeight, elsewhere ? &pose : nullptr);
        else
            host::dropGameFrustum(camera);
    }
    READABLE_FUNCTION_VU0(updateFrustum, Verified);

    namespace
    {
        // vif1PacketAddQwords for data of any type: `count` quadwords from it.
        template <typename T>
        void addQwords(sce::Vif1Packet *packet, const T *data, int32_t count)
        {
            sce::vif1PacketAddQwords(packet, reinterpret_cast<const uint8_t *>(data), count);
        }

        // The A+D GIF tag (one register) the scissor goes in.
        constexpr ee::Ptr<const ee::Qword> kScissorGifTag(sym::scissorGifTag);
        // A display's draw environment (sceGsDrawEnv1): each register's
        // value, then its A+D address.
        struct DrawEnvironment
        {
            uint64_t frame, frameRegister;
            uint64_t zbuf, zbufRegister;
            uint64_t xyOffset, xyOffsetRegister;
            uint64_t scissor, scissorRegister;
        };
        static_assert(offsetof(DrawEnvironment, scissor) == 0x30);
        // The display's two draw environments and which one the frame is
        // drawn with (g_display +0x64).
        inline ee::Global<uint32_t, sym::g_drawEnvironment> g_drawEnvironment;
        inline ee::Global<DrawEnvironment, sym::g_drawEnvironment0> g_drawEnvironment0;
        inline ee::Global<DrawEnvironment, sym::g_drawEnvironment1> g_drawEnvironment1;
        const DrawEnvironment &drawEnvironment(bool second) { return second ? *g_drawEnvironment1 : *g_drawEnvironment0; }
        constexpr uint64_t kScissorFields =
            gs::Scissor::kLeft.mask() | gs::Scissor::kRight.mask() | gs::Scissor::kTop.mask() | gs::Scissor::kBottom.mask();
    }

    // setViewportScissor: a DIRECT block in `packet` that sets SCISSOR_1 to the
    // camera's viewport (in field lines: y halved), or with no camera to the
    // scissor of the draw environment the frame uses; FLUSHA on both sides.
    void setViewportScissor(sce::Vif1Packet *packet, ee::Ptr<Camera> camera)
    {
        ee::Local<ee::Qword> tag;
        *tag = *kScissorGifTag;
        sce::vif1PacketCnt(packet, 0u);
        sce::vif1PacketAddCode(packet, vif::flushA());
        sce::vif1PacketAlign(packet, 2u, 3u);
        sce::vif1PacketOpenDirect(packet, 0u);
        sce::vif1PacketOpenGifTag(packet, *tag);

        uint64_t scissor = drawEnvironment(*g_drawEnvironment != 0).scissor & kScissorFields;
        if (camera)
        {
            // Towards zero, the y's halved as ints (towards zero too).
            const Float *viewport = camera->viewport;
            const int32_t x0 = ee::toInt(viewport[0]);
            const int32_t y0 = ee::toInt(viewport[1]) / 2;
            const int32_t x1 = ee::toInt(viewport[0] + viewport[2]) - 1;
            const int32_t y1 = ee::toInt(viewport[1] + viewport[3]) / 2 - 1;
            const auto field = [](gs::Field f, int32_t value) { return gs::withField(0, f, static_cast<uint32_t>(value)); };
            scissor = field(gs::Scissor::kLeft, x0) | field(gs::Scissor::kRight, x1) | field(gs::Scissor::kTop, y0) |
                      field(gs::Scissor::kBottom, y1);
        }
        sce::vif1PacketAddGsAd(packet, gs::kScissor1, scissor);
        sce::vif1PacketCloseGifTag(packet);
        sce::vif1PacketCloseDirect(packet);
        sce::vif1PacketAddCode(packet, vif::flushA());
    }
    READABLE_FUNCTION(setViewportScissor, Verified);

    namespace
    {
        // render/ps2/primitive_packets.cpp's PrimitivePacket, as far as a frame's
        // start needs it: the world one is also the VIF1 packet the frame's
        // VU1 set-up goes in (beginViewDraws's end).
        struct PrimitivePacketState
        {
            sce::Vif1Packet vif;      // +0x00
            uint8_t _18[0x20];
            int32_t texture;          // +0x38 the texture bound, -1 for none
            uint8_t _3c[0x0C];
            uint32_t startProgram;    // +0x48 the next run is an MSCAL
            uint32_t _4c;
        };
        static_assert(sizeof(PrimitivePacketState) == 0x50 && offsetof(PrimitivePacketState, texture) == 0x38);
        inline ee::Global<PrimitivePacketState, sym::g_worldPrimitives> g_worldPrimitives;
        inline ee::Global<PrimitivePacketState, sym::g_screenPrimitives> g_screenPrimitives;
        inline ee::Global<uint32_t[4], sym::strowWords> g_strowWords;
        // The camera's world frustum (Camera::worldFrustum, set by
        // beginCameraFrame): its first plane here, five of the six in
        // beginViewDraws.
        inline ee::Global<ee::Ptr<CameraFrustum>, sym::g_viewPlanes> g_viewFrustum;
        constexpr uint32_t kViewBufferOffset = 0x1E0u; // the double buffer's second half
    }

    namespace // the game functions below call: ee/plain.h
    {
        namespace tail
        {
        void vif1PacketAddCallCode(ee::Ptr<sce::Vif1Packet> packet, uint32_t code) { ee::tailCall(sym::vif1PacketAddCallCode, packet, code); }
        }
    }

    // beginFramePacket: the frame's VU1 set-up packet: the camera's scissor,
    // the STROW row, the double buffer's BASE and OFFSET, the world -> GS
    // screen and world -> clip matrices and a quadword of the display's
    // block (relative to TOPS), and the microprogram's calls.
    void beginFramePacket(ee::Ptr<Camera> camera)
    {
        for (PrimitivePacketState *primitives : {&*g_worldPrimitives, &*g_screenPrimitives})
        {
            primitives->texture = -1;
            primitives->startProgram = 0;
        }
        sce::Vif1Packet *packet = &g_worldPrimitives->vif;
        setViewportScissor(packet, camera);
        sce::vif1PacketCnt(packet, 0u);
        sce::vif1PacketAddCode(packet, vif::flushA());
        sce::vif1PacketAddCode(packet, vif::strow());
        for (uint32_t word = 0; word < 4; ++word)
            sce::vif1PacketAddWord(packet, (*g_strowWords)[word]);
        sce::vif1PacketAddCode(packet, vif::base(0));
        sce::vif1PacketAddCode(packet, vif::offset(kViewBufferOffset));
        sce::vif1PacketOpenUnpack(packet, vif::kUnpackFromTops | 0u, vif::kUnpackV4_32, 4u, 4u);
        addQwords(packet, g_screenMatrix->get(), 4);
        addQwords(packet, g_clipMatrix->get(), 4);
        sce::vif1PacketAddQword(packet, std::bit_cast<ee::Qword>((*g_viewFrustum)->planes[0]));
        sce::vif1PacketCloseUnpack(packet);
        sce::vif1PacketAddCallCode(packet, vif::mscal(0x12u));
        tail::vif1PacketAddCallCode(packet, vif::mscal(0));
    }
    READABLE_FUNCTION(beginFramePacket, Verified);

    namespace
    {
        // The view's draw list (g_renderer, render/ps2/texture_request.cpp): the
        // camera (+0x60), the class renderer whose VU1 state went out last
        // (+0x84), "place a marker first" (+0x88); its mesh packet at +0x20.
        inline ee::Global<ee::Ptr<Camera>, sym::viewCamera> g_viewCamera;
        inline ee::Global<uint32_t, kRendererAddress + offsetof(Renderer, vu1StateOwner)> g_vu1StateOwner;
        inline ee::Global<uint32_t, kRendererAddress + offsetof(Renderer, markerNeeded)> g_markerNeeded;
        inline ee::Global<sce::Vif1Packet, kRendererAddress + offsetof(Renderer, meshPacket)> g_viewMeshPacket;
        constexpr uint64_t kZbufMask = gs::Zbuf::kZmsk.mask();
        constexpr ee::Ptr<const uint64_t> kGsData(sym::gsData);  // four A+D pairs

        void unpackTo(sce::Vif1Packet *packet, uint32_t vuAddress)
        {
            sce::vif1PacketOpenUnpack(packet, vuAddress, vif::kUnpackV4_32, 4u, 4u);
        }
    }

    // beginViewDraws: a view's draw list started for `camera`: the camera's
    // scissor, then VU1's constants for the view at 0x3ED-0x3FF (ZBUF_1
    // with and without the Z mask, four A+D pairs, five quadwords of the
    // display's block, the camera's forward axis, the view's three
    // matrices at 1012-1023),
    // then the frame's VU1 set-up. Returns 1.
    int32_t beginViewDraws(ee::Ptr<Camera> camera)
    {
        ee::Local<Plane[5]> pickedPlanes;
        loadViewCulling(camera);
        *g_viewCamera = camera;
        *g_markerNeeded = 1;
        *g_vu1StateOwner = 0;
        sce::Vif1Packet *packet = &*g_viewMeshPacket;
        setViewportScissor(packet, camera);
        sce::vif1PacketCnt(packet, 0u);

        const uint64_t zbuf = ee::io::load<uint64_t>(sym::g_scratchZbuf);
        unpackTo(packet, 0x3EDu);
        sce::vif1PacketAddGsAd(packet, gs::kZbuf1, zbuf);
        sce::vif1PacketAddGsAd(packet, gs::kZbuf1, zbuf | kZbufMask);
        sce::vif1PacketCloseUnpack(packet);

        unpackTo(packet, 0x3EFu);
        sce::vif1PacketAddGsData(packet, kGsData, 4u);
        sce::vif1PacketCloseUnpack(packet);

        // The camera's world planes but the far one.
        const ee::Ptr<CameraFrustum> frustum = *g_viewFrustum;
        constexpr uint32_t kPicked[5] = {0, 2, 3, 4, 5};
        for (uint32_t i = 0; i < 5; ++i)
            pickedPlanes[i] = frustum->planes[kPicked[i]];
        unpackTo(packet, 0x3F2u);
        addQwords(packet, &pickedPlanes[0], 5);
        sce::vif1PacketCloseUnpack(packet);

        unpackTo(packet, 0x3F3u);
        addQwords(packet, &camera->forward, 1);
        sce::vif1PacketCloseUnpack(packet);

        const std::pair<uint32_t, ee::Ptr<Matrix>> matrices[3] = {
            {0x3F4u, *g_viewMatrix}, {0x3F8u, *g_screenMatrix}, {0x3FCu, *g_clipMatrix}};
        for (const auto &[vuAddress, matrix] : matrices)
        {
            unpackTo(packet, vuAddress);
            addQwords(packet, matrix.get(), 4);
            sce::vif1PacketCloseUnpack(packet);
        }
        sce::vif1PacketAlign(packet, 2u, 0u);
        if constexpr (game::readable::kIsBmx)
            beginViewBillboardsNoop(); // (ZGE4's billboard queue: nothing to do for a view)
        beginFramePacket(camera);
        return 1;
    }

    // beginViewConstants: ZGE2's beginViewDraws (its renderFrame calls it for
    // each camera set current, and reads no camera word): a marker due and
    // no VU1 state sent, the view's counts cleared, then VU1's constants for
    // the view at 0x3F7-0x3FF in a CNT tag (the world frustum's first plane,
    // the world -> GS screen and world -> clip matrices), aligned, and the
    // screen primitives' viewport. No culling, scissor, Z buffer, GS data or
    // view matrix as ZGE3's, and no frame packet of the world primitives. 1.
    int32_t beginViewConstants(ee::Ptr<Camera>)
    {
        *g_markerNeeded = 1;
        *g_vu1StateOwner = 0;
        for (auto &viewCounts : g_frameCounts->view)
            for (uint32_t &count : viewCounts)
                count = 0;
        sce::Vif1Packet *packet = &*g_viewMeshPacket;
        sce::vif1PacketCnt(packet, 0u);
        unpackTo(packet, 0x3F7u);
        addQwords(packet, &(*g_viewFrustum)->planes[0], 1);
        addQwords(packet, g_screenMatrix->get(), 4);
        addQwords(packet, g_clipMatrix->get(), 4);
        sce::vif1PacketCloseUnpack(packet);
        sce::vif1PacketAlign(packet, 2u, 0u);
        addViewportPacket();
        return 1;
    }
    READABLE_FUNCTION_OWN(beginViewDraws, Verified, dm2, beginViewConstants);

    namespace
    {
        // The camera being set up.
        inline ee::Global<ee::Ptr<Camera>, sym::g_settingUpCamera> g_settingUpCamera;
        constexpr float kMinimumNearClip = 1.25f;
        // g_display's words for the view, beside the three matrices of node.h.
        inline ee::Global<ee::Ptr<Camera>, sym::g_debugFontCamera> g_displayCamera;
        inline ee::Global<ee::Ptr<Matrix>, sym::g_screenProjection> g_screenProjection;
        inline ee::Global<ee::Ptr<Matrix>, sym::viewClipProjection> g_clipProjection;
        inline ee::Global<ee::Ptr<Vec4>, sym::g_viewMatrix2> g_viewAxes;
        inline ee::Global<ee::Ptr<Matrix>, sym::viewFromWorldWord> g_viewFromWorld;
        inline ee::Global<Float, sym::viewNearClip> g_viewNearClip;
        inline ee::Global<Float, sym::viewFarClip> g_viewFarClip;
        inline ee::Global<Float, sym::viewFocalLength> g_viewFocalLength;
        inline ee::Global<ee::Ptr<uint32_t>, sym::viewPlaneCount> g_viewPlaneCount;
        // In the scratchpad, the other's FRAME beside the frame's FRAME, ZBUF
        // and XYOFFSET (those three go through ee::io, the scratchpad's I/O).
        inline ee::Global<uint64_t, sym::g_scratchOtherFrame> g_scratchOtherFrame;
    }

    namespace
    {
        // cameraCut's limits: a move of more than 7 units (squared), or a
        // turn whose cosine's size is under this (about 20 degrees).
        constexpr ee::Float kCutMoveSquared = ee::Float(49.0f);
        constexpr ee::Float kCutTurnCosine = ee::Float(0.93969f);
        // DM2's turn limit is the double 0.93969 (its C compares the float
        // with a double constant), its bits as libgcc's soft double takes them.
        constexpr uint64_t kCutTurnCosineDouble = std::bit_cast<uint64_t>(0.93969);
    }

    namespace // the game functions below call: ee/plain.h
    {
        // libgcc's soft double (sce/libgcc.cpp): (double) value, and compare (< 0, 0, > 0).
    }

    // cameraCut (beginCameraFrame): whether the first camera jumped since the
    // last frame: moved more than 7 units, or its forward turned by more
    // than about 20 degrees (a turn to nearly straight back does not count:
    // the cosine's size is tested). Its forward and position are then kept
    // for the next frame. 1 for a cut; 0 for none, on the first frame, or
    // with no camera. DM2 measures the move with its FPU helper and tests
    // the turn in double precision; AI and BMX do both in float.
    int32_t cameraCut()
    {
        ee::Local<Vec4> moved;
        const ee::Ptr<Camera> camera = zge::firstCameraOfKind(kCameraKindView);
        if (!camera)
            return 0;
        int32_t cut = 0;
        if (camera->lastKept)
        {
            *moved = camera->lastPosition - camera->position;
            if constexpr (game::readable::kIsDm2)
            {
                if (kCutMoveSquared < lengthSquared(&moved->xyz())) // (ZGE2's own function; zge/math/vector.h)
                    cut = 1;
                if (sce::libgcc::dpcmp(sce::libgcc::fptodp(ee::abs(dot(camera->lastForward, camera->forward))), kCutTurnCosineDouble) < 0)
                    cut = 1;
            }
            else
            {
                if (kCutMoveSquared < squaredLength(*moved))
                    cut = 1;
                if (ee::abs(dot(camera->lastForward, camera->forward)) < kCutTurnCosine)
                    cut = 1;
            }
        }
        camera->lastKept = 1;
        camera->lastForward = camera->forward;
        camera->lastPosition = camera->position;
        return cut;
    }
    READABLE_FUNCTION(cameraCut, Verified);

    // beginCameraFrame: a camera made current for the frame: its near clip at
    // least 1.25, the change flags cleared, a cut noted, its projections,
    // view matrix and products, the frustum; the display's view words and
    // the scratchpad's GS state from it and the draw environment; then the
    // view's draw list (beginViewDraws). Returns 1. ZGE4 works the frustum
    // out before the projections and keeps only the ZBUF in the scratchpad.
    int32_t beginCameraFrame(ee::Ptr<Camera> camera)
    {
        *g_settingUpCamera = camera;
        Camera &c = *camera;
        uint32_t changed = c.changed;
        if (changed & kCameraFogChanged)
        {
            changed &= ~uint32_t{kCameraFogChanged};
            c.changed = changed;
        }
        const uint32_t kind = c.kind;
        c.changed = changed & ~uint32_t{kCameraFarChanged};
        c.nearClip = ee::max(c.nearClip, Float(kMinimumNearClip));
        // A view's clear colour goes to the display for the frame's first view only.
        if (kind == kCameraKindView && *g_currentView == 0)
            setClearColour(c.clearColour);
        c.changed = c.changed & ~uint32_t{kCameraMatrixChanged};
        if (cameraCut() != 0)
        {
            if constexpr (game::readable::kIsBmx) // (ZGE4 calls the flush through its thunk)
                requestTextureCacheFlushThunk();
            else
                requestTextureCacheFlush();
        }
        // ZGE4 works the frustum out before the projections, ZGE3 after the view's products.
        if constexpr (game::readable::kIsBmx)
            updateFrustum(camera);
        updateProjection(camera);
        affineMatrixCopy(&c.view, &c.fromWorld);
        sceVu0MulMatrix(&c.viewProjection, &c.screenProjection, &c.view);
        sceVu0MulMatrix(&c.clipViewProjection, &c.clipProjection, &c.view);
        if constexpr (!game::readable::kIsBmx)
            updateFrustum(camera);

        *g_viewAxes = &c.right;
        *g_viewMatrix = &c.view;
        *g_screenProjection = &c.screenProjection;
        *g_screenMatrix = &c.viewProjection;
        *g_clipProjection = &c.clipProjection;
        *g_clipMatrix = &c.clipViewProjection;
        *g_viewFromWorld = &c.fromWorld;
        *g_displayCamera = camera;
        *g_viewNearClip = c.nearClip;
        *g_viewFarClip = c.farClip;
        *g_viewPlaneCount = &c.viewPlaneCount;
        *g_viewFocalLength = c.focalLength;
        *g_viewFrustum = &c.worldFrustum();
        const bool second = *g_drawEnvironment != 0;
        const DrawEnvironment &drawn = drawEnvironment(second);
        if constexpr (game::readable::kIsBmx) // ZGE4 keeps the drawn buffer's ZBUF alone in the scratchpad
            ee::io::store<uint64_t>(sym::g_scratchZbuf, drawn.zbuf);
        else
        {
            ee::io::store<uint64_t>(sym::g_scratchFrame, drawn.frame);
            ee::io::store<uint64_t>(sym::g_scratchZbuf, drawn.zbuf);
            ee::io::store<uint64_t>(sym::g_scratchXyOffset, drawn.xyOffset);
            *g_scratchOtherFrame = drawEnvironment(!second).frame;
        }
        beginViewDraws(camera);
        return 1;
    }

    // beginChangedCameraFrame: ZGE2's beginCameraFrame: a camera made current for the frame, its work done for
    // what changed alone: a projection change (kCameraFarChanged) holds its near clip at 1.25 or more and gives
    // the display its clear colour; a matrix change notes a cut and remakes the projections, the view matrix and
    // its products and the frustum. Then the display's view words, and the draw environment's FRAME, TEST and
    // ZBUF in the scratchpad. What changed in ZGE3: the near clip, projections and frustum every frame, the clear
    // colour for the first view only, the camera kept as the one being set up, the other buffer's FRAME and
    // XYOFFSET in the scratchpad in place of TEST, and the view's draw list started (beginViewDraws). Returns 1.
    int32_t beginChangedCameraFrame(ee::Ptr<Camera> camera)
    {
        Camera &c = *camera;
        if (c.changed & kCameraFogChanged)
            c.changed &= ~uint32_t{kCameraFogChanged};
        if (c.changed & kCameraFarChanged)
        {
            c.changed &= ~uint32_t{kCameraFarChanged};
            c.nearClip = ee::max(c.nearClip, Float(kMinimumNearClip));
            setClearColour(c.clearColour);
        }
        if (c.changed & kCameraMatrixChanged)
        {
            c.changed &= ~uint32_t{kCameraMatrixChanged};
            if (cameraCut() != 0)
                requestTextureCacheFlush();
            updateProjection(camera);
            affineMatrixCopy(&c.view, &c.fromWorld);
            sceVu0MulMatrix(&c.viewProjection, &c.screenProjection, &c.view);
            sceVu0MulMatrix(&c.clipViewProjection, &c.clipProjection, &c.view);
            updateFrustum(camera);
        }

        *g_viewMatrix = &c.view;
        *g_screenProjection = &c.screenProjection;
        *g_screenMatrix = &c.viewProjection;
        *g_clipProjection = &c.clipProjection;
        *g_clipMatrix = &c.clipViewProjection;
        *g_viewAxes = &c.right;
        *g_viewFromWorld = &c.fromWorld;
        *g_displayCamera = camera;
        *g_viewNearClip = c.nearClip;
        *g_viewFarClip = c.farClip;
        *g_viewFocalLength = c.focalLength;
        *g_viewPlaneCount = &c.viewPlaneCount;
        *g_viewFrustum = &c.worldFrustum();
        const GsDrawEnvironment1 &drawn = g_display->buffers.draw[g_display->drawBuffer != 0 ? 1 : 0].draw1;
        ee::io::store<uint64_t>(sym::g_scratchFrame, drawn.frame.value);
        ee::io::store<uint64_t>(sym::dm2::g_scratchTest, drawn.test.value);
        ee::io::store<uint64_t>(sym::g_scratchZbuf, drawn.zbuf.value);
        return 1;
    }
    READABLE_FUNCTION_OWN(beginCameraFrame, Verified, dm2, beginChangedCameraFrame);

    namespace
    {
        inline ee::Global<int32_t, sym::g_drawFrustumFirstCall> g_drawFrustumFirstCall;
        inline ee::Global<int32_t, sym::g_sceneRecordCount> g_sceneRecordCount;
        inline ee::Global<int32_t, sym::g_settingUpCamera> g_settingUpCameraWord;
    }

    uint32_t initScenesHook()
    {
        return 1;
    }
    READABLE_FUNCTION(initScenesHook, Verified);

    uint32_t sceneHook()
    {
        return 1;
    }
    READABLE_FUNCTION(sceneHook, Verified);

    uint32_t countSceneRecord()
    {
        *g_sceneRecordCount = *g_sceneRecordCount + 1;
        return 1;
    }
    READABLE_FUNCTION(countSceneRecord, Verified);

    uint32_t uncountSceneRecord()
    {
        *g_sceneRecordCount = *g_sceneRecordCount - 1;
        return 1;
    }
    READABLE_FUNCTION(uncountSceneRecord, Verified);

    uint32_t sceneRecordCount()
    {
        return *g_sceneRecordCount;
    }
    READABLE_FUNCTION(sceneRecordCount, Verified);

    // endCameraSetup: the view's packet begun and the camera's set-up over. 1. ZGE2's is empty: it begins no view
    // packet here and keeps no set-up word.
    uint32_t endCameraSetup()
    {
        if constexpr (!game::readable::kIsDm2)
        {
            zge::beginViewPacket();
            *g_settingUpCameraWord = 0;
        }
        return 1;
    }
    READABLE_FUNCTION_WAITS(endCameraSetup, Verified);

    // Its whole body is empty; closeCameraPackets calls it each camera.
    uint32_t textureCacheCameraHook()
    {
        return game::readable::perGame(0u, 1u); // (ZGE2's says 1)
    }
    READABLE_FUNCTION(textureCacheCameraHook, Verified);

    // ---- view tests no level reaches --------------------------------------------

    namespace
    {
        // (ViewSphere and partInView's answer, ViewSphereFlags: camera.h)
        constexpr ee::Float kNearPlaneMargin = 4.0f;

        // partClipFlags' answer.
        enum BoxClipFlags : uint32_t
        {
            kBoxOutOfView = 1u << 0,
            kBoxInsideFirstPlane = 1u << 5, // every corner on the inside of the first plane (a camera's near plane)
        };
        constexpr uint32_t kBoxCorners = 8;

        inline ee::Global<int32_t, sym::g_screenOffsetX> g_screenOffsetX; // the screen's place in GS coordinates (whole pixels)
        inline ee::Global<int32_t, sym::g_screenOffsetY> g_screenOffsetY;
        constexpr ee::Float kPerSubpixel = 0.0625f;  // GS x: 1/16 pixel
        constexpr ee::Float kPerFieldRow = 0.125f;   // GS y: 1/16 pixel, two rows a field row

        void sceVu0RotTransPers(ee::Ptr<int32_t> out, ee::Ptr<Matrix> matrix, ee::Ptr<Vec4> point, uint32_t mode)
        {
            ee::game(sym::sceVu0RotTransPers, out, matrix, point, mode);
        }
    }

    // partInView: where a bounding sphere lies against the camera's view
    // (ViewSphereFlags): its centre taken into camera space, out of view
    // when wholly before the near plane, past the far plane or outside a
    // side; else which planes it crosses (0: wholly inside).
    uint32_t partInView(ee::Ptr<Camera> camera, ee::Ptr<ViewSphere> sphere)
    {
        ee::Local<Vec4> centre;
        if constexpr (game::readable::kIsDm2) // (ZGE2 calls its point transform)
            transformPoint(centre, &camera->fromWorld, &sphere->centre);
        else
            *centre = transformed(camera->fromWorld, sphere->centre);
        const ee::Float radius = sphere->radius;
        const ee::Float depth = centre->z;
        const ee::Float nearClip = camera->nearClip;
        const ee::Float front = depth + radius;
        if (front < nearClip)
            return kSphereOutOfView;
        const ee::Float back = depth - radius;
        const ee::Float farClip = camera->farClip;
        if (farClip < back)
            return kSphereOutOfView;
        // The sides: the view's half width at the sphere's depth, widened
        // for the radius.
        const ee::Float radiusAcross = camera->sphereScaleX * radius;
        const ee::Float limitAcross = camera->tanHalfWidth * depth + radiusAcross;
        centre->x = ee::abs(centre->x);
        if (limitAcross < centre->x)
            return kSphereOutOfView;
        const ee::Float radiusUp = camera->sphereScaleY * radius;
        const ee::Float limitUp = camera->tanHalfHeight * depth + radiusUp;
        centre->y = ee::abs(centre->y);
        if (limitUp < centre->y)
            return kSphereOutOfView;
        if (back < nearClip)
            return kSphereCrossesNear | kSphereCrossesSide;
        if (farClip < front)
            return kSphereCrossesFar;
        const uint32_t nearNear = back < nearClip + kNearPlaneMargin ? kSphereNearNearPlane : 0u;
        if (!(limitAcross - (radiusAcross + radius) < centre->x) && !(limitUp - (radiusUp + radius) < centre->y))
            return 0;
        return nearNear | kSphereCrossesSide;
    }
    READABLE_FUNCTION(partInView, Draft);

    // partClipFlags: a box (in the world, or through `world` when given)
    // against the camera's world frustum (BoxClipFlags): out of view when
    // all eight corners are outside one plane.
    uint32_t partClipFlags(ee::Ptr<Camera> camera, ee::Ptr<const Box> box, ee::Ptr<const Matrix> world)
    {
        ee::Local<Vec4> corner;
        ee::Local<Vec4[kBoxCorners]> corners;
        // The corners in the order the original makes them, from 8 down: bit
        // 0 picks low x over high, bit 1 low y, bit 2 low z.
        const auto pick = [&](uint32_t bits, uint32_t axis) {
            const Vec4 &from = (bits >> axis & 1u) ? box->low : box->high;
            return axis == 0 ? from.x : axis == 1 ? from.y : from.z;
        };
        if (world)
        {
            corner->w = 1.0f;
            for (uint32_t bits = kBoxCorners, slot = 0; bits != 0; --bits, ++slot)
            {
                corner->x = pick(bits, 0);
                corner->y = pick(bits, 1);
                corner->z = pick(bits, 2);
                if constexpr (game::readable::kIsDm2) // (ZGE2 calls its point transform)
                    transformPoint(&corners[slot], world, corner);
                else
                    corners[slot] = transformed(*world, *corner);
            }
        }
        else
        {
            for (uint32_t bits = kBoxCorners, slot = 0; bits != 0; --bits, ++slot)
            {
                corners[slot].x = pick(bits, 0);
                corners[slot].y = pick(bits, 1);
                corners[slot].z = pick(bits, 2);
                corners[slot].w = 1.0f;
            }
        }
        uint32_t flags = 0;
        uint32_t planesLeft = camera->planeCount;
        for (ee::Ptr<Plane> plane = &camera->planes[0];; plane = plane + 1)
        {
            uint32_t first = 0; // the first corner on the inside
            while (first < kBoxCorners && !(ee::Float(0.0f) <= dot(corners[first], *plane) - plane->w))
                ++first;
            if (first == kBoxCorners)
                return kBoxOutOfView;
            // With six planes left (the first plane of a camera's six), a box
            // whose first corner is inside and all the rest too is marked.
            if (planesLeft == kFrustumPlanes && first == 0)
            {
                uint32_t next = 1;
                while (next < kBoxCorners && !(dot(corners[next], *plane) - plane->w < ee::Float(0.0f)))
                    ++next;
                if (next == kBoxCorners)
                    flags |= kBoxInsideFirstPlane;
            }
            if (--planesLeft == 0)
                return flags;
        }
    }
    READABLE_FUNCTION(partClipFlags, Draft);

    // projectToScreen: a camera-space point (w set to 1) through the
    // camera's screen projection, into screen pixels (x, y; z and w 0).
    // A point before the near plane gives (0, 0, 0, 1) and 0; else 1.
    uint32_t projectToScreen(ee::Ptr<Camera> camera, ee::Ptr<Vec4> point, ee::Ptr<Vec4> screen)
    {
        ee::Local<int32_t[4]> gs;
        if (point->z < camera->nearClip)
        {
            aligned(screen) = kZero;
            return 0;
        }
        point->w = 1.0f;
        sceVu0RotTransPers(gs, &camera->screenProjection, point, 0);
        screen->x = ee::toFloat(gs[0]) * kPerSubpixel - ee::toFloat(*g_screenOffsetX);
        const ee::Float offsetY = ee::toFloat(*g_screenOffsetY);
        screen->w = 0.0f;
        screen->z = 0.0f;
        screen->y = ee::toFloat(gs[1]) * kPerFieldRow - offsetY;
        return 1;
    }
    READABLE_FUNCTION(projectToScreen, Draft);

    // The camera's matrices cameraTransform hands out.
    enum CameraTransform : uint32_t
    {
        kTransformScreenProjection = 0, // view -> GS screen
        kTransformView = 1,             // world -> view
        kTransformViewProjection = 2,   // world -> GS screen
    };

    // cameraTransform (was displayBufferBase): one of the camera's
    // matrices (CameraTransform), or 0.
    ee::Ptr<Matrix> cameraTransform(ee::Ptr<Camera> camera, uint32_t which)
    {
        switch (which)
        {
        case kTransformView:
            return &camera->view;
        case kTransformScreenProjection:
            return &camera->screenProjection;
        case kTransformViewProjection:
            return &camera->viewProjection;
        default:
            return ee::Ptr<Matrix>();
        }
    }
    READABLE_FUNCTION(cameraTransform, Draft);

    // ---- debug drawing ----------------------------------------------------------

    namespace
    {
        namespace tail
        {
        // render/ps2/primitive_packets.cpp: quads as a texture word and four
        // vertex pointers each, from the first quad's texture.
        }

        // A frustum corner as the primitive API takes a vertex
        // (zge/render/ps2/primitive_packets.cpp's PrimitiveVertex): the corner's
        // whole quadword is copied in, so its w lands in the word the API
        // does not read.
        struct FrustumVertex
        {
            Vec4 position;    // +0x00 view space
            ee::Float u, v;   // +0x10 0: no texture
            uint8_t rgba[4];  // +0x18 0x80 is full intensity
            uint32_t unused;  // +0x1C
        };
        static_assert(sizeof(FrustumVertex) == 0x20 && offsetof(FrustumVertex, u) == 0x10 &&
                      offsetof(FrustumVertex, rgba) == 0x18);

        // A frustum face as the primitive API takes a quad: a texture (0, the
        // slot's default) and its four corners.
        struct FrustumQuad
        {
            uint32_t texture;                   // +0x00
            ee::Ptr<FrustumVertex> corners[4];  // +0x04
        };
        static_assert(sizeof(FrustumQuad) == 0x14);

        // The frustum's faces and corners, filled on drawFrustum's first call.
        inline ee::Global<FrustumQuad[kFrustumPlanes], sym::g_frustumFaces> g_frustumFaces;
        inline ee::Global<FrustumVertex[kFrustumCorners], sym::g_frustumPoints> g_frustumPoints;

        // The near corners tinted blue, the far ones red, half transparent.
        constexpr uint8_t kNearCornerColour[4] = {0x80, 0x80, 0xC0, 0x80};
        constexpr uint8_t kFarCornerColour[4] = {0xC0, 0x80, 0x80, 0x80};

        // The primitive API's flags: in the world (through the current view
        // matrix, no matrix of the quads' own: kPrimitiveViewRelative) with the
        // slot's second default texture (kPrimitiveSecondSlot).
        constexpr uint32_t kFrustumQuadFlags = (1u << 24) | (1u << 1);
    }

    // drawFrustum (debug, no caller in AI): the camera's view frustum
    // drawn as its six faces, from the corners in view space. The faces and
    // the corners' colours are set up once, on the first call.
    int32_t drawFrustum(ee::Ptr<Camera> camera)
    {
        FrustumQuad(&faces)[kFrustumPlanes] = *g_frustumFaces;
        FrustumVertex(&vertices)[kFrustumCorners] = *g_frustumPoints;
        if (*g_drawFrustumFirstCall != 0)
        {
            *g_drawFrustumFirstCall = 0;
            for (uint32_t face = 0; face < kFrustumPlanes; ++face)
            {
                const FrustumPlaneCorners &corners = (*g_frustumPlaneCorners)[face];
                for (uint32_t i = 0; i < 4; ++i)
                    faces[face].corners[i] = &vertices[corners.corner[i]];
                faces[face].texture = 0;
            }
            for (uint32_t corner = 0; corner < kFrustumCorners; ++corner)
            {
                FrustumVertex &vertex = vertices[corner];
                vertex.u = 0.0f;
                vertex.v = 0.0f;
                const uint8_t(&colour)[4] = corner < kCornerFarTopLeft ? kNearCornerColour : kFarCornerColour;
                for (uint32_t channel = 0; channel < 4; ++channel)
                    vertex.rgba[channel] = colour[channel];
            }
        }
        // Every call: this camera's corners into the vertices.
        for (uint32_t corner = 0; corner < kFrustumCorners; ++corner)
            vertices[corner].position = camera->viewCorners[corner];
        return game::tail(drawPrimitiveQuads, kFrustumQuadFlags, &faces[0].texture, static_cast<int32_t>(camera->viewPlaneCount), 0);
    }
    READABLE_FUNCTION_WAITS(drawFrustum, Draft);
}
