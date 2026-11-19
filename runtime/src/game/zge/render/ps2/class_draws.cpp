// The class renderers' draws as VU1 takes them (class_draws.h): the strict encoder of zge/render/draw.h's records.

#include "zge/render/ps2/class_draws.h"

#include "zge/render/material.h"
#include "zge/render/renderer.h"
#include "zge/render/texture.h"
#include "zge/render/display.h"
#include "zge/render/shadow.h"
#include "zge/render/vif_codes.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "sce/packet_calls.h"
#include "zge/container/array.h"
#include "zge/math/vec.h"
#include "zge/platform/platform.h"
#include "zge/scene/camera.h"
#include "zge/scene/light.h"
#include "zge/scene/skeleton.h"
#include <bit>
#include "sym/zge/render/ps2/class_draws.cpp.h"

namespace zge::ps2
{
    namespace
    {
        // Four quadwords of rows to the packet.
        template <typename T>
        void addRows(sce::Vif1Packet *packet, const T &rows) { sce::vif1PacketAddQwords(packet, reinterpret_cast<const uint8_t *>(&rows), 4); }

        // A VIF code, a data word and a microprogram call (MSCAL) appended:
        // the library has three copies of the one function, each called at
        // its own symbol.
        void addCode(sce::Vif1Packet *packet, uint32_t code) { sce::vif1PacketAddCode(packet, code); }
        void addWord(sce::Vif1Packet *packet, uint32_t word) { sce::vif1PacketAddWord(packet, word); }
        void addCallCode(sce::Vif1Packet *packet, uint32_t code) { sce::vif1PacketAddCallCode(packet, code); }

        // The view's draw list (g_renderer; the mesh packet is its +0x20): the
        // class renderer whose VU1 state went out last in it (1 rigid and
        // terrain, 2 skinned).
        inline ee::Global<uint32_t, kRendererAddress + offsetof(Renderer, vu1StateOwner)> g_vu1StateOwner;
        constexpr uint32_t kRigidState = 1u, kSkinnedState = 2u;
        // The rigid microprograms' STROW (D_232780): the row the UNPACKs add.
        inline ee::Global<uint32_t[4], sym::g_rigidRow> g_rigidRow;

        // VU1 data: the double buffer's second half at 0x1E0; the node's
        // matrix to the buffer's start (address 0 with FLG: + TOPS), four
        // V4-32 quadwords.
        constexpr uint32_t kDoubleBufferOffset = game::readable::perGame(0x1E0u, 0x200u, 0x1E0u); // (DM2's drawRigid: 0x200)
        constexpr uint32_t kMatrixAddress = vif::kUnpackFromTops | 0u;
        // The microprogram by render type: type + 2 drawn clipped, else the
        // type itself; a prelit rigid part (4) inside the far plane and the
        // sides has its own, 0x1E [inferred: no fog]. 0: nothing to call.
        constexpr uint32_t kClippedProgram = 2u;
        constexpr uint32_t kUnfoggedPrelitProgram = game::readable::perGame(0x1Eu, 0x1Au, 0x1Eu); // (ZGE2's microcode: 0x1A)
        // Terrain: 0x20, or 0x22 clipped.
        constexpr uint32_t kTerrainProgram = 0x20u, kClippedTerrainProgram = 0x22u;

        // The skinned microprograms' VU1 buffers: BASE 0x80, OFFSET 0x1A0.
        constexpr uint32_t kSkinnedBase = game::readable::perGame(0x80u, 0x60u, 0x80u);            // (DM2's drawSkinned: 0x60)
        constexpr uint32_t kSkinnedDoubleBufferOffset = game::readable::perGame(0x1A0u, 0x1D0u, 0x1A0u); // (DM2's: 0x1D0)
        // The skinned program is the render type + 2 (always: no clipped or
        // unlit variants).
        constexpr uint32_t kSkinnedProgramStep = 2u;
    }

    namespace
    {
        // requestTexturesWrittenIn: ZGE2 has no requestTextures (ps2/texture_request.cpp): its class renderers write
        // the same request in. Its marker is the FLUSHA itself, placed with the interrupt bit already set (no NOP):
        // a request keeps it (with the far bit when the node is further than 30) and queues the node; a node
        // already asked for (every level, or the far ones and still far) takes the bit off again. Then the next
        // marker and kMarkNoTextures. ZGE2's table flags are bits 3 (far) and 4 (all); its node has one view.
        inline ee::Global<ee::Ptr<ee::Address>, kRendererAddress + offsetof(Renderer, requestQueueEnd)> g_requestQueueEnd;
        inline ee::Global<ee::Ptr<uint32_t>, kRendererAddress + offsetof(Renderer, marker)> g_marker;
        inline ee::Global<uint32_t, kRendererAddress + offsetof(Renderer, markerNeeded)> g_markerNeeded;
        inline ee::Global<uint32_t, sym::g_textureEpoch> g_textureEpoch;
        inline void requestTexturesWrittenIn(sce::Vif1Packet *packet, ee::Ptr<Node> node, ee::Ptr<MaterialTable> materials)
        {
            constexpr uint32_t kRequestedFar = 1u << 3, kRequestedAll = 1u << 4;
            const ee::Float kNearDepth(30.0f);
            const auto placeInterruptMarker = [packet] {
                addCode(packet, vif::mark(kMarkNodeTextures));
                *g_marker = packet->current;
                addCode(packet, vif::flushA() | vif::kInterrupt);
            };
            if (*g_markerNeeded)
            {
                *g_markerNeeded = 0;
                placeInterruptMarker();
            }
            MaterialTable &table = *materials;
            if (table.textureEpoch != *g_textureEpoch)
            {
                table.flags &= ~(kRequestedAll | kRequestedFar);
                table.textureEpoch = *g_textureEpoch;
            }
            const Node &n = *node;
            const ee::Float depth = n.views[0].viewCentre.z - n.radius;
            const uint32_t flags = table.flags;
            const bool asked = (flags & kRequestedAll) || ((flags & kRequestedFar) && !(depth <= kNearDepth));
            if (asked)
                **g_marker &= ~vif::kInterrupt;
            else
            {
                if (kNearDepth < depth)
                {
                    **g_marker |= kMarkerFar;
                    table.flags |= kRequestedFar;
                }
                else
                    table.flags = flags | kRequestedFar | kRequestedAll;
                const ee::Ptr<ee::Address> queueEnd = *g_requestQueueEnd;
                *queueEnd = node;
                *g_requestQueueEnd = queueEnd + 1;
            }
            placeInterruptMarker();
            addCode(packet, vif::mark(kMarkNoTextures));
        }

        // The part's prebuilt packet called as a DMA chain: the material table's chain of the buffer drawn (part
        // buffer flags bit 1).
        void callPartPacket(sce::Vif1Packet *packet, const DrawRecord &record)
        {
            const uint32_t half = (record.part->bufferFlags >> 1) & 1u;
            sce::vif1PacketCall(packet, record.materials->packetChains[half], 0u);
        }

        // Without the draw's VU1 work (the host's form draws it): a DMA tag begun where the part's call would be.
        // The tag the packet had open may be a call (another part's chain, the shadow pass's), whose chain DMA runs
        // only after the data that follows the tag: the draw's state, texture request and anchor then go after that
        // chain, as on the PS2 path (DM2's riders and bikes after the shadow buffer is applied, not under it).
        void beginDrawTag(sce::Vif1Packet *packet) { sce::vif1PacketCnt(packet, 0u); }

        // The class's VU1 state, once per draw list: FLUSHA, STROW of the rigid row, BASE and OFFSET of its
        // double buffer.
        void sendVu1State(sce::Vif1Packet *packet, uint32_t owner, uint32_t base, uint32_t offset)
        {
            if (*g_vu1StateOwner != owner)
            {
                *g_vu1StateOwner = owner;
                addCode(packet, vif::flushA());
                addCode(packet, vif::strow());
                for (uint32_t i = 0; i < 4; ++i)
                    addWord(packet, (*g_rigidRow)[i]);
                addCode(packet, vif::base(base));
                addCode(packet, vif::offset(offset));
            }
        }

        // The texture request in the class's own form: ZGE2's written in.
        void addTextureRequest(sce::Vif1Packet *packet, const DrawRecord &record)
        {
            if constexpr (game::readable::kIsDm2)
                requestTexturesWrittenIn(packet, record.node, record.materials);
            else
                requestTextures(packet, record.node, record.materials);
        }

        // A rigid or terrain draw's start: the part's packet, the VU1 state, the texture request, and the
        // matrix's UNPACK opened (the VU state and the request alone when its VU1 work is the host's).
        void beginNodeDraw(sce::Vif1Packet *packet, const DrawRecord &record, bool vu1Work)
        {
            if (vu1Work)
                callPartPacket(packet, record);
            else
                beginDrawTag(packet);
            sendVu1State(packet, kRigidState, 0u, kDoubleBufferOffset);
            addTextureRequest(packet, record);
            if (vu1Work)
                sce::vif1PacketOpenUnpack(packet, kMatrixAddress, vif::kUnpackV4_32, 4u, 4u);
        }

        // The microprogram call, then the program from 0 (the original's tail call).
        void callProgram(sce::Vif1Packet *packet, uint32_t program)
        {
            addCallCode(packet, vif::mscal(program));
            ee::tailCall(sym::vif1PacketAddCallCode, packet, vif::mscal(0)); // the original's tail call
        }
    }

    namespace
    {
        // ZGE2's clipped rigid draw sends one clip plane after the node's rows: the view frustum's first plane in
        // the node's space (its normal by the inverse world's rotation, its distance less the normal's dot with the
        // node's position), on VU0 as the original's.
        inline ee::Global<ee::Ptr<CameraFrustum>, sym::g_viewPlanes> g_viewPlanes; // the camera's world frustum
        inline void addClipPlaneInNodeSpace(sce::Vif1Packet *packet, ee::Ptr<Node> node, bool vu1Work)
        {
            ee::Local<Vec4> plane;
            ee::Local<ee::Ptr<Vec4>> planeSlot; // (the original keeps the plane's address in its frame)
            *planeSlot = &(*g_viewPlanes)->planes[0];
            const Vec4 &viewPlane = **planeSlot;
            plane = zge::rotated(node->inverseWorld, viewPlane);
            plane->w = viewPlane.w - zge::dot(viewPlane, node->world.translation());
            if (vu1Work)
                sce::vif1PacketAddQword(packet, std::bit_cast<ee::Qword>(*plane));
        }
    }

    namespace
    {
        // sceVu0MulMatrix(out, a, b): out = b * a (row vectors).
        constexpr uint32_t kVu0MulMatrix = sym::sceVu0MulMatrix;
    }

    namespace // the game functions below call: ee/plain.h
    {
        void sceVu0MulMatrix(ee::Ptr<Matrix> out, ee::Address left, ee::Ptr<Matrix> right) { ee::game(kVu0MulMatrix, out, left, right); }
    }

    namespace
    {
        // ZGE4's rigid lights, after the node's matrix: the directions turned by the node's world (their first
        // row's w then the word 1) and the colours; none: one zero quadword.
        void addRigidLights(sce::Vif1Packet *packet, ee::Ptr<Node> node, const DrawLights &lights, bool vu1Work,
                            RigidLightsUpload *toHost)
        {
            if (!lights.directions)
            {
                if (vu1Work)
                    sce::vif1PacketAddQword(packet, ee::Qword{});
                return;
            }
            ee::Local<Matrix> world;
            ee::Local<Matrix> directions;
            affineMatrixCopy(world, &node->world);
            sceVu0MulMatrix(directions, lights.directions, world);
            directions->m[0][3] = ee::Float::fromBits(1u);
            if (toHost)
            {
                toHost->directions = *directions;
                toHost->colours = *lights.colours;
            }
            if (!vu1Work)
                return;
            addRows(packet, *directions);
            addRows(packet, *lights.colours);
        }
    }

    void encodeRigidDraw(sce::Vif1Packet *packet, const RigidDraw &draw, bool vu1Work, RigidLightsUpload *toHost)
    {
        const DrawRecord &record = draw.record;
        beginNodeDraw(packet, record, vu1Work);
        if (vu1Work)
            addRows(packet, record.node->world);
        uint32_t program = static_cast<uint32_t>(record.type);
        if (record.clipped())
        {
            if constexpr (game::readable::kIsDm2)
                addClipPlaneInNodeSpace(packet, record.node, vu1Work);
            program += kClippedProgram;
        }
        else if (!draw.lights.directions)
        {
            if (record.insideFarAndSides() && record.type == RenderType::RigidPrelit)
                program = kUnfoggedPrelitProgram;
        }
        if constexpr (game::readable::kIsBmx)
            addRigidLights(packet, record.node, draw.lights, vu1Work, toHost);
        if (!vu1Work)
            return;
        sce::vif1PacketCloseUnpack(packet);
        if (!program)
            return;
        if constexpr (game::readable::kIsBmx)
        {
            // ZGE4 calls the second MSCAL and returns (its frame still holds the saves): no tail call.
            addCallCode(packet, vif::mscal(program));
            addCallCode(packet, vif::mscal(0));
        }
        else
            callProgram(packet, program);
    }

    void encodeTerrainDraw(sce::Vif1Packet *packet, const TerrainDraw &draw, bool vu1Work)
    {
        const DrawRecord &record = draw.record;
        beginNodeDraw(packet, record, vu1Work);
        // The shells' depth rides in the matrix's spare lane (row X's w).
        Node &n = *record.node;
        n.world.m[0][3] = draw.shellDepth;
        if (!vu1Work)
            return;
        addRows(packet, n.world);
        sce::vif1PacketCloseUnpack(packet);
        callProgram(packet, record.clipped() ? kClippedTerrainProgram : kTerrainProgram);
    }

    namespace
    {
        void addMatrixQwords(sce::Vif1Packet *packet, const Matrix &matrix)
        {
            for (uint32_t row = 0; row < 4; ++row)
                sce::vif1PacketAddQword(packet, reinterpret_cast<const ee::Qword &>(matrix.row(row)));
        }
        ee::Ptr<BonePose> bonePosesOf(const Node &node) { return node.bonePoses.cast<BonePose>(); }
    }

    // VU1 gets the node's bones (each pose with the bone's bind offset taken out of its translation), then the
    // world -> screen and world -> clip matrices with the lights between them.
    void encodeSkinnedDraw(sce::Vif1Packet *packet, const SkinnedDraw &draw, SkinnedUpload *toHost)
    {
        ee::Local<Matrix> product;
        ee::Local<Matrix> world;
        ee::Local<Matrix> pose;
        ee::Local<Vec4> rotated;
        ee::Local<Matrix> lit;
        ee::Local<ee::Ptr<Matrix>> worldSlot;
        ee::Local<ee::Ptr<ee::Ptr<PacketMaterial>>> tablesSlot;
        const DrawRecord &record = draw.record;
        Node &n = *record.node;
        const ee::Ptr<ZgePart> part = record.part;
        // Both stay in the frame, as the original keeps them.
        *worldSlot = &n.world;
        *tablesSlot = &part->materialTables[0];
        if (!toHost)
            callPartPacket(packet, record);
        else
            beginDrawTag(packet);
        sendVu1State(packet, kSkinnedState, kSkinnedBase, kSkinnedDoubleBufferOffset);
        requestTextures(packet, record.node, record.materials);
        addCode(packet, vif::flushA());

        // The bones, to VU1 0 on.
        const ee::Ptr<BonePose> poses = bonePosesOf(n);
        if (!toHost)
            sce::vif1PacketOpenUnpack(packet, 0u, vif::kUnpackV4_32, 4u, 4u);
        BoneList &bones = *ee::Ptr<BoneList>(part->bones);
        ee::Ptr<Bone> bone;
        if (bones.count != 0u)
        {
            bones.cursor = 0;
            bone = bones.items[0];
        }
        else
            bones.cursor = -1;
        while (bone)
        {
            // Its pose (poses count from 1), with the translation less the bind offset
            // turned by the pose; the rows' w 0, 0, 0, 1.
            pose = poses[bone->poseIndex - 1u].matrix;
            rotated = zge::rotated(pose, bone->offset);
            pose->translation() -= *rotated;
            pose->m[0][3] = ee::Float(0.0f);
            pose->m[3][3] = ee::Float(1.0f);
            pose->m[2][3] = ee::Float(0.0f);
            pose->m[1][3] = ee::Float(0.0f);
            if (toHost)
                toHost->bones.push_back(*pose);
            else
                addRows(packet, *pose);

            // The list's next bone.
            int32_t cursor = bones.cursor;
            bone = ee::Ptr<Bone>();
            bool inList = false;
            if (cursor >= 0)
            {
                bones.cursor = ++cursor;
                inList = cursor < static_cast<int32_t>(bones.count);
                if (inList)
                    bone = bones.items[cursor];
            }
            if (!inList)
                bones.cursor = -1;
        }
        if (!toHost)
            sce::vif1PacketCloseUnpack(packet);

        // The matrices and lights, to VU1 0x8000 (+ TOPS).
        affineMatrixCopy(world, &n.world);
        if (!toHost)
            sce::vif1PacketOpenUnpack(packet, kMatrixAddress, vif::kUnpackV4_32, 4u, 4u);
        sceVu0MulMatrix(product, draw.screen, world);
        if (toHost)
            toHost->screen = *product;
        else
            addMatrixQwords(packet, product);
        if (draw.lights.turnedByWorld)
        {
            sceVu0MulMatrix(lit, draw.lights.directions, world);
            if (toHost)
                toHost->lightDirections = *lit;
            else
                addRows(packet, *lit);
        }
        else if (toHost)
            toHost->lightDirections = *draw.lights.directions;
        else
            addRows(packet, *draw.lights.directions);
        if (toHost)
            toHost->lightColours = *draw.lights.colours;
        else
            addRows(packet, *draw.lights.colours);
        sceVu0MulMatrix(product, draw.clip, world);
        if (toHost)
        {
            toHost->clip = *product;
            return;
        }
        addMatrixQwords(packet, product);
        const uint32_t program = static_cast<uint32_t>(record.type) + kSkinnedProgramStep;
        sce::vif1PacketCloseUnpack(packet);
        if (program)
        {
            addCallCode(packet, vif::mscal(program));
            addCallCode(packet, vif::mscal(0));
        }
    }

    // As ZGE3's, with the texture request written in, the bones walked with arrayFirst and arrayNext and each pose
    // copied with copyRotationAndTranslation (ZGE3 writes both in), the directions always turned by the node's
    // world and each matrix sent a quadword at a time. The part also stays in the frame. (Inline outside DM2's
    // instance, as its renderer's own body is: ZGE3 and ZGE4 have no copyRotationAndTranslation.)
    READABLE_OWN_INLINE(dm2) void encodeSkinnedDrawInDefaultLights(sce::Vif1Packet *packet, const SkinnedDraw &draw, SkinnedUpload *toHost)
    {
        ee::Local<Matrix> product;
        ee::Local<Matrix> world;
        ee::Local<Matrix> pose;
        ee::Local<Vec4> rotated;
        ee::Local<Matrix> lit;
        ee::Local<ee::Ptr<Matrix>> worldSlot;
        ee::Local<ee::Ptr<ZgePart>> partSlot;
        ee::Local<ee::Ptr<ee::Ptr<PacketMaterial>>> tablesSlot;
        const DrawRecord &record = draw.record;
        Node &n = *record.node;
        *worldSlot = &n.world;
        *partSlot = record.part;
        *tablesSlot = &(*partSlot)->materialTables[0];
        if (!toHost)
            callPartPacket(packet, record);
        else
            beginDrawTag(packet);
        sendVu1State(packet, kSkinnedState, kSkinnedBase, kSkinnedDoubleBufferOffset);
        requestTexturesWrittenIn(packet, record.node, record.materials);
        addCode(packet, vif::flushA());

        // The bones, to VU1 0 on: each pose's rotation and translation, the
        // translation less the bind offset turned by the pose, the rows' w 0,
        // 0, 0, 1.
        const ee::Ptr<BonePose> poses = bonePosesOf(n);
        if (!toHost)
            sce::vif1PacketOpenUnpack(packet, 0u, vif::kUnpackV4_32, 4u, 4u);
        for (ee::Ptr<Bone> bone(arrayFirst(ee::Ptr<PointerArray>((*partSlot)->bones))); bone;
             bone = ee::Ptr<Bone>(arrayNext(ee::Ptr<PointerArray>((*partSlot)->bones))))
        {
            copyRotationAndTranslation(ee::addressOf(&*pose), ee::addressOf(&poses[bone->poseIndex - 1u].matrix));
            rotated = zge::rotated(pose, bone->offset);
            pose->translation() -= *rotated;
            pose->m[0][3] = ee::Float(0.0f);
            pose->m[3][3] = ee::Float(1.0f);
            pose->m[2][3] = ee::Float(0.0f);
            pose->m[1][3] = ee::Float(0.0f);
            if (toHost)
                toHost->bones.push_back(*pose);
            else
                addRows(packet, *pose);
        }
        if (!toHost)
            sce::vif1PacketCloseUnpack(packet);

        // The matrices and lights, to VU1 0x8000 (+ TOPS).
        affineMatrixCopy(world, *worldSlot);
        if (!toHost)
            sce::vif1PacketOpenUnpack(packet, kMatrixAddress, vif::kUnpackV4_32, 4u, 4u);
        sceVu0MulMatrix(product, draw.screen, world);
        if (toHost)
            toHost->screen = *product;
        else
            addMatrixQwords(packet, product);
        sceVu0MulMatrix(lit, draw.lights.directions, world);
        const ee::Ptr<Vec4> colours = draw.lights.colours.cast<Vec4>();
        if (toHost)
        {
            toHost->lightDirections = *lit;
            toHost->lightColours = *colours.cast<Matrix>();
        }
        else
        {
            addMatrixQwords(packet, lit);
            for (uint32_t row = 0; row < 4; ++row)
                sce::vif1PacketAddQword(packet, reinterpret_cast<const ee::Qword &>(colours[row]));
        }
        sceVu0MulMatrix(product, draw.clip, world);
        if (toHost)
        {
            toHost->clip = *product;
            return;
        }
        addMatrixQwords(packet, product);
        const uint32_t program = static_cast<uint32_t>(record.type) + kSkinnedProgramStep;
        sce::vif1PacketCloseUnpack(packet);
        if (program)
        {
            addCallCode(packet, vif::mscal(program));
            addCallCode(packet, vif::mscal(0));
        }
    }
}
