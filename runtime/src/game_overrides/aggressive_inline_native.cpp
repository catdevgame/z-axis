// Aggressive Inline (SLUS-20327): native drawing of level meshes.
//
// Design: observe and substitute. The game's
// renderer runs unchanged; this file watches it and has the VU1 work of the
// meshes it understands replaced by native draws in the GS stream.
//
// 1. Observe (EE thread, game override wrappers that call the originals, so
//    game RAM and EE cycles are what they always were):
//    - FUN_00109EB0(camera), the per-view VU1 globals: the view's world to
//      GS screen matrix (*0x231CC4, uploaded to VU1 1016-1019) and its near
//      plane (camera +0x38).
//    - FUN_0010CE30(node, cullFlags, packet), the renderer of render types 4
//      (rigid prelit) and 0x18 (sky): it opens a DMA CALL tag in the frame's
//      packet (0x27A9D0) to the node's material chain, then its payload
//      (VIF state, the texture request: MARK and an interrupt-bit NOP, the
//      world matrix, the microprogram call). The CALLed chain has, per
//      material, a REF of the material's GS register block (UNPACKed, then
//      XGKICKed by VU1 before the first strip) and a CALL of the part's
//      packet for that material (strips of at most 60 vertices, MSCAL 2 /
//      MSCNT each).
//    - FUN_0010B520(node, cullFlags, packet), the terrain renderer (type
//      0x20, VU1 0x20 / 0x22), with the same packet shape; its materials
//      carry a command list (material +0xE0): the base pass, grass shells
//      (0x4DE), a UV matrix (0x581).
//    - FUN_0010C250(node, cullFlags, packet), the skinned renderer (types 8
//      lit and 0xC prelit: skaters, NPCs; VU1 0x08 / 0x0C): the same packet
//      shape, its payload uploads the bone palette (4 qwords a bone, VU1 0..)
//      and 16 qwords at TOPS (object -> screen, the three lights' directions
//      and colours with the ambient, object -> clip volume), all read from
//      that payload (runtime/ps2_zaxis_mesh.h, SkinnedNodeUpload).
//    - FUN_00109178(part, first, streams), the packet builder: a part rebuilt
//      gets its mesh again, and one rebuilt from changed streams (first = 0)
//      is decoded from its packet from then on.
// 2. Record: the node's part as a native mesh (the triangles VU1 would draw,
//    per material), its world matrix times the view's screen matrix, and the
//    materials' GS blocks. The mesh is made from the GEO data the readable
//    loader read the part from (src/game/zge/native/geo_meshes.h: the same
//    triangles, checked vertex for vertex with ZX_NATIVE_MESHES_CHECK);
//    a part with no file origin, or rebuilt from changed streams, is decoded
//    from its packet (runtime/ps2_zaxis_mesh.h). ZX_NATIVE_MESHES=packet:
//    always decoded.
// 3. Substitute (PS2Memory::setVif1Substitute): where the VIF1 DMA reaches
//    the node's CALL tag, its microprogram runs are not started; in place of
//    the first, the GS gets, for each material in the chain's order, its GS
//    register block (what VU1 would have kicked, read from RAM after the
//    texture interrupt handler patched TEX0) and a native draw of its
//    triangles (GS::drawNative). Order, render targets, depth (GS Z =
//    16 * z' / w' as FTOI4 makes it, 16-bit, the game's GREATER test) and
//    every GS-path draw around them stay as they were.
//
// What the GS draws per vertex is what VU1 0x04 / 0x06 / 0x1E compute:
// [x y z 1] * world * screen, X, Y = x'/w', y'/w', Z = 16 z'/w' (>= 0), STQ =
// (s, t, 1) / w', the colour halved by the packet builder. The GPU clips at
// Z = 2^16, the near plane, as the clipping program (0x06) does at 1.25;
// the CPU renderer gets that program's own triangles (GSNativeClip::NearSplit:
// each split in four at its midpoints, cut where it leaves the clip volume).
// The sky (type 0x18, VU1 0x18) is drawn as the rigid ones but at Z = 1,
// X, Y clamped to the GS window and no clipping: a triangle with a vertex
// beyond the clip volume's near side is left out (GSNativeDraw::dropFlags).
// The skinned programs move each vertex by up to four bones and light it
// (GSNativeSkinning, gs_native_skin_vs), map textures affinely (Q = 1.0)
// and leave out triangles with a vertex outside the guard band or the depth
// range.
// The terrain program is the same with the packed normal (shells move k *
// 0.02 along it), alpha 0x80 (0x80 - 15k for shell k) and its grass state
// (PRIM 0x1C / TEST 0x910C for the base, PRIM 0x5C / TEST 0x940D for the
// shells: alpha test GEQUAL 0x40, failing pixels blended without Z).
//
// Shadows are not substituted: the skater shadow stays the game's own. Per
// view, bucket 4 (FUN_0010F1E8) builds three kinds of packets in 0x27AC80,
// each CALLed from the frame's packet: begin (FUN_0010D388: context 2 on the
// CT16S counter at FBP 144, cleared to 120 by a sprite), per caster the
// volume (FUN_0010EE70: the light, a point 5 units off the caster's bone 1;
// FUN_0010DBC8 the silhouette edges of its shadow mesh, or FUN_0010D6B8 an
// octagon when FUN_00101DF8() is 3 or more; VU1 0x1C draws a fan of flat
// triangles from the light through each edge pushed 1.25 times as far
// again, front faces adding 248 and back faces 8 with COLCLAMP=0, so 16 is
// shadow), then the composite (FUN_0010E150: the low bits cleared under FBMSK
// 0x80808080, a sprite textured with the counter subtracts the node's shadow
// colour (+0x180, halved) where the top bit is set). The GPU renderer draws
// all of that at its internal resolution; its fragmented look came from
// inexact flat colours in the counter, fixed in the backend behind the
// Shadows option (gs_quality.h, GPU_BACKEND.md "Native draws", Shadows).
#include "aggressive_inline_native.h"

#include "ps2_runtime.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_native.h"
#include "runtime/gs/gs_quality.h"
#include "runtime/gs/gs_types.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_zaxis_mesh.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_gs_shots.h"

#include "../game/host/native_draws.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The readable game's meshes made from the GEO data (src/game/zge/native/
// geo_meshes.h): a part's triangles as its packet would decode, from the file
// chunk the readable loader read it from; null for a part with no file origin.
namespace zge::native
{
    std::shared_ptr<const zx::zaxis::RigidMesh> fileMeshForPacketKind(uint32_t partAddress,
                                                                        zx::zaxis::PacketKind kind);
    uint64_t geoParseNanos();
    uint64_t geoStreamsParsed();
}

namespace aggressive_inline
{
    namespace native_draw
    {

        // ---- the game's renderer, as read here ---------------------------
        constexpr uint32_t kPerViewGlobals = 0x00109EB0u;  // FUN_00109EB0(camera)
        constexpr uint32_t kDrawRigid = 0x0010CE30u;       // FUN_0010CE30(node, cullFlags, packet)
        constexpr uint32_t kBuildPartPacket = 0x00109178u; // FUN_00109178(part, ...)
        constexpr uint32_t kDrawTerrain = 0x0010B520u;     // FUN_0010B520(node, cullFlags, packet): type 0x20
        constexpr uint32_t kDrawSkinned = 0x0010C250u;     // FUN_0010C250(node, cullFlags, packet): types 8 / 0xC
        constexpr uint32_t kViewIndex = 0x00273964u;       // D_273964: the view being drawn
        constexpr uint32_t kViewMatrix = 0x00231CBCu;      // *(): world -> view, 4 rows (VU1 1012-1015)
        constexpr uint32_t kScreenMatrix = 0x00231CC4u;    // *(): world -> GS screen, 4 rows (VU1 1016-1019)
        constexpr uint32_t kClipMatrix = 0x00231CCCu;      // *(): world -> clip volume (guard band), 4 rows (VU1 1020-1023)
        constexpr uint32_t kCameraNear = 0x38u;            // camera +0x38: near plane (float)
        // The camera's fog (FUN_0012A140 / FUN_0012A160) and its switch.
        namespace camera
        {
            constexpr uint32_t kOff = 0x04;      // u32: not drawn (no fog planes)
            constexpr uint32_t kFar = 0x3C;      // float: far clip
            constexpr uint32_t kFogStart = 0x5C; // float
            constexpr uint32_t kFogEnd = 0x60;   // float
        }
        constexpr uint32_t kFogPlanes = 0x001110C0u;       // FUN_001110C0(): the view's eight fog planes
        constexpr uint32_t kCurrentCamera = 0x00273960u;   // D_273960: the camera being drawn
        constexpr uint32_t kFogOn = 0x00279214u;           // D_279214: fog planes wanted
        constexpr uint32_t kProjection = 0x00231CC0u;      // *(): view -> GS screen (camera +0x1C0), 4 rows
        constexpr uint32_t kLevelIndex = 0x0024C9B4u;      // the level being played (8: the Zoo)
        constexpr uint32_t kZooIndex = 8u;

        // The scene node (a placed object), as the renderers read it.
        namespace node
        {
            constexpr uint32_t kWorld = 0x50;         // 4x4 floats, rows X, Y, Z, T
            constexpr uint32_t kViewDistance = 0x5C;  // row X's w: FUN_0010B520 puts this view's distance minus radius there
            constexpr uint32_t kMaterialTable = 0x1B0;
            constexpr uint32_t kViewPart = 0x1D0;     // + 0x20 * view: the (LOD) part drawn
            constexpr uint32_t kViewStride = 0x20;
        }
        // Its material table.
        namespace table
        {
            constexpr uint32_t kCount = 0x02;     // u16
            constexpr uint32_t kChains = 0x08;    // [2]: the material chain per packet buffer
            constexpr uint32_t kMaterials = 0x14; // [count]: material objects
        }
        // A material object (MATI at run time).
        namespace material
        {
            constexpr uint32_t kGsBlock = 0x60;       // GIF tag and A+D: CLAMP, TEX1, ALPHA, TEST, MIPTBP1, TEXFLUSH, TEX0
            constexpr uint32_t kCommandCount = 0x6C;  // u32 (the GIF tag's last word): terrain command list length
            constexpr uint32_t kCommands = 0xE0;      // terrain command list, a qword each: routine, parameters
            constexpr uint32_t kGsBlockQwords = 0x160; // u32: what the material chain uploads
        }
        // GS state the terrain program kicks for grass (VU1 0x26F0, 0x2788).
        constexpr uint64_t kGrassBasePrim = 0x1C;          // tri strip, Gouraud, textured
        constexpr uint64_t kGrassBaseTest = 0xFFFF910Cull; // no alpha test, Z GREATER
        constexpr uint64_t kGrassShellPrim = 0x5C;         // and blended
        constexpr uint64_t kGrassShellTest = 0xFFFF940Dull; // alpha GEQUAL 0x40 (failing: frame buffer only), Z GREATER

        enum class Mode
        {
            Auto,
            Native,
            Strict,
            Primitives, // diagnostics: native meshes sent to the GS as primitives
            // The readable class renderers draw natively from their records (zge/host/class_draws.cpp); a draw
            // they do not reach (their recompiled originals running) is drawn here as in Native.
            Records
        };

        struct View
        {
            float view[4][4] = {};   // world -> view (the sphere map's)
            float screen[4][4] = {}; // world -> GS screen
            float clip[4][4] = {};   // world -> clip volume: |x|, |y|, |z| <= w inside the guard band and depth range
            float nearW = 1.25f;
            bool valid = false;
        };

        // A part decoded once (until the game rebuilds its packet).
        struct CachedPart
        {
            uint32_t entries = 0, firstPacket = 0; // the drawn packet's material entries
            std::shared_ptr<const zx::zaxis::RigidMesh> mesh; // null: not drawn natively
            uint64_t lastUse = 0;
        };

        struct State
        {
            PS2Runtime::RecompiledFunction origViewGlobals = nullptr;
            PS2Runtime::RecompiledFunction origDrawRigid = nullptr;
            PS2Runtime::RecompiledFunction origBuildPacket = nullptr;
            PS2Runtime::RecompiledFunction origDrawTerrain = nullptr;
            PS2Runtime::RecompiledFunction origDrawSkinned = nullptr;
            PS2Runtime::RecompiledFunction origFogPlanes = nullptr;
            uint64_t fogNotes = 0;
            bool rigid = true, terrain = true, sky = true, skinned = true; // ZX_NATIVE_CLASSES
            std::atomic<int> mode{static_cast<int>(Mode::Auto)};
            View view;
            std::unordered_map<uint32_t, CachedPart> parts;
            uint64_t clock = 0; // views drawn (FUN_00109EB0 calls)
            uint64_t nextSweep = 0;
            int trace = 0;
            bool zooOnly = false;      // ZX_NATIVE_LEVELS=zoo
            bool linearColour = false; // debugging: ZX_NATIVE_LINEAR_COLOUR=1
            bool skipClipped = false; // debugging: ZX_NATIVE_CLIPPED=0 leaves nodes the clipper draws on VU1
            // Counters (ZX_TRACE_NATIVE).
            uint64_t nodes = 0, registered = 0, undecodable = 0, otherClass = 0, noView = 0, badTag = 0;
            uint64_t skies = 0, sphereMapped = 0, skinnedNodes = 0, badUpload = 0;
            uint64_t lastReport = 0;
            // Meshes from the GEO data (ZX_NATIVE_MESHES=packet: always the
            // packets). A part whose packet was rebuilt from changed streams
            // (FUN_00109178 with a1 = 0) is decoded from then on.
            bool fileMeshes = true;
            std::unordered_set<uint32_t> rebuilt;
            uint64_t fromFile = 0, decoded = 0, fileNanos = 0, decodeNanos = 0;
            // Records: the path booted with (the Native Meshes row goes back to it), and the draws the readable
            // class renderers did not take (drawn here from the packets).
            Mode booted = Mode::Auto;
            uint64_t notFromRecords = 0;
        };

        // The boot's (zx::session): a game booted again hooks its renderer
        // again, with its caches empty.
        State &state() { return zx::session::state<State>(); }

        zx::zaxis::GuestMemory guest(uint8_t *rdram)
        {
            return zx::zaxis::GuestMemory{rdram};
        }

        bool readMatrix(const zx::zaxis::GuestMemory &m, uint32_t address, float out[4][4])
        {
            const uint8_t *p = m.bytes(address, 64);
            if (!p)
                return false;
            std::memcpy(out, p, 64);
            return true;
        }

        // o = a x b (row vectors).
        void multiply(const float a[4][4], const float b[4][4], float o[4][4])
        {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                {
                    float v = 0.0f;
                    for (int k = 0; k < 4; ++k)
                        v += a[r][k] * b[k][c];
                    o[r][c] = v;
                }
        }

        // Native draws now: the mode, and in Auto whether the renderer
        // draws them itself.
        bool active(PS2Runtime *runtime)
        {
            const Mode mode = static_cast<Mode>(state().mode.load(std::memory_order_relaxed));
            if (mode == Mode::Strict)
                return false;
            return mode == Mode::Native || mode == Mode::Primitives || mode == Mode::Records ||
                   runtime->gs().nativeDrawsOnBackend();
        }

        void viewGlobalsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t camera = getRegU32(ctx, 4);
            state().origViewGlobals(rdram, ctx, runtime);
            State &s = state();
            ++s.clock;
            const zx::zaxis::GuestMemory m = guest(rdram);
            uint32_t screen = 0, clip = 0, view = 0;
            s.view.valid = m.u32(kScreenMatrix, screen) && readMatrix(m, screen, s.view.screen) &&
                           m.u32(kClipMatrix, clip) && readMatrix(m, clip, s.view.clip) && m.u32(kViewMatrix, view) &&
                           readMatrix(m, view, s.view.view);
            float nearW = 0.0f;
            if (m.f32(camera + kCameraNear, nearW) && nearW > 0.0f && nearW < 100.0f)
                s.view.nearW = nearW;
        }

        void buildPacketHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t part = getRegU32(ctx, 4);
            const bool firstBuild = getRegU32(ctx, 5) != 0; // (part, 1, all) new; (part, 0, dirty streams) rebuilt
            state().origBuildPacket(rdram, ctx, runtime);
            State &s = state();
            s.parts.erase(part);
            if (firstBuild)
                s.rebuilt.erase(part);
            else
                s.rebuilt.insert(part);
        }

        // The part's mesh (of the packet `entries` it is drawn with), decoded
        // now if new or changed.
        std::shared_ptr<const zx::zaxis::RigidMesh> meshOf(const zx::zaxis::GuestMemory &m, uint32_t part, uint32_t entries,
                                                    zx::zaxis::PacketKind kind)
        {
            State &s = state();
            CachedPart now;
            now.entries = entries;
            if (!m.u32(entries, now.firstPacket))
                return nullptr;
            auto it = s.parts.find(part);
            if (it != s.parts.end() && it->second.entries == now.entries && it->second.firstPacket == now.firstPacket)
            {
                it->second.lastUse = s.clock;
                return it->second.mesh;
            }
            const bool skinned = kind == zx::zaxis::PacketKind::Skinned || kind == zx::zaxis::PacketKind::SkinnedPrelit;
            const auto started = std::chrono::steady_clock::now();
            auto nanosSince = [&]
            {
                return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                 std::chrono::steady_clock::now() - started)
                                                 .count());
            };
            // The mesh made from the GEO data, while the part's streams are
            // the file's (the same triangles: ZX_NATIVE_MESHES_CHECK); the
            // packet's decode otherwise.
            uint8_t bones = 0;
            if (s.fileMeshes && !s.rebuilt.count(part) && m.u8(part + zx::zaxis::part::kBoneCount, bones) &&
                (skinned || bones == 0))
                now.mesh = zge::native::fileMeshForPacketKind(part, kind);
            if (now.mesh)
            {
                ++s.fromFile;
                s.fileNanos += nanosSince();
            }
            else
            {
                auto mesh = std::make_shared<zx::zaxis::RigidMesh>();
                std::string why;
                if (skinned ? zx::zaxis::decodeSkinnedPart(m, part, *mesh, &why, kind)
                            : zx::zaxis::decodeRigidPart(m, part, *mesh, &why, kind))
                    now.mesh = mesh;
                else if (s.trace)
                    std::cout << "[native] part 0x" << std::hex << part << std::dec << " stays on VU1: " << why
                              << std::endl;
                ++s.decoded;
                s.decodeNanos += nanosSince();
            }
            now.lastUse = s.clock;
            CachedPart &slot = s.parts[part];
            slot = now;
            return slot.mesh;
        }

        // Parts not drawn for a while (another level's) are let go.
        void sweepParts()
        {
            State &s = state();
            if (s.clock < s.nextSweep)
                return;
            s.nextSweep = s.clock + 600;
            for (auto it = s.parts.begin(); it != s.parts.end();)
                it = s.clock - it->second.lastUse > 7200 ? s.parts.erase(it) : std::next(it); // a minute or two
        }

        void report(PS2Runtime *runtime)
        {
            State &s = state();
            if (!s.trace || s.nodes - s.lastReport < 20000)
                return;
            s.lastReport = s.nodes;
            const PS2Memory::Vif1SubstituteStats v = runtime->memory().vif1SubstituteStats();
            size_t meshes = 0, bytes = 0;
            for (const auto &kv : s.parts)
                if (kv.second.mesh)
                {
                    ++meshes;
                    bytes += kv.second.mesh->mesh->vertices.size() * sizeof(GSNativeVertex);
                }
            std::cout << "[native] nodes " << s.nodes << " registered " << s.registered << " other class " << s.otherClass
                      << " undecodable " << s.undecodable << " no view " << s.noView << " bad tag " << s.badTag
                      << " sky " << s.skies << " sphere-mapped " << s.sphereMapped << " skinned " << s.skinnedNodes
                      << " bad upload " << s.badUpload
                      << " | packets " << v.packets << " declined " << v.declined << " stale " << v.mismatched
                      << " runs skipped " << v.runsSkipped << " unpacks skipped " << v.unpacksSkipped
                      << " posted " << v.drawsPosted << " lost " << v.drawsLost
                      << " | meshes " << meshes << " (" << (bytes >> 20) << " MB)"
                      << " | from the GEO data " << s.fromFile << " (" << s.fileNanos / 1000 << " us), decoded "
                      << s.decoded << " (" << s.decodeNanos / 1000 << " us); GEO data read at load "
                      << zge::native::geoStreamsParsed() << " streams (" << zge::native::geoParseNanos() / 1000
                      << " us) | records: not taken " << s.notFromRecords << std::endl;
        }

        // The node renderers this file draws natively.
        enum class Renderer
        {
            RigidPrelit, // FUN_0010CE30: render types 4 and 0x18 (sky)
            Terrain,     // FUN_0010B520: render type 0x20 (ground, grass)
            Skinned,     // FUN_0010C250: render types 8 (lit) and 0xC (prelit): skaters, NPCs
        };

        // A material of a node as its microprogram draws it.
        struct Item
        {
            uint32_t block = 0;          // material +0x60: GIF tag and A+D
            uint32_t first = 0, count = 0; // its triangles in the mesh
            // Terrain command list (material +0xE0, count at +0x6C).
            bool grass = false;          // routine 0x4DE: base and shells
            int shells = 0;              // drawn this time
            int alphaStep = 0;           // shell alpha 0x80 - step * k
            float shellStep = 0.0f;      // shell offset along the normal, per shell
            // UV routines in the list's order: 0x581 (a UV matrix: its rows
            // R0..R3 at `at`, read when drawn) and 0x592 (sphere map).
            struct UvOp
            {
                uint32_t routine = 0, at = 0;
            };
            std::vector<UvOp> uvOps;
        };

        // A UV map: S, T = i0 * r[0] + i1 * r[1] + i2 * r[2] + r[3], (i0, i1,
        // i2) the vertex's (s, t, 1) or, fromNormal, its normal / 32768.
        struct UvMap
        {
            float r[4][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
            bool fromNormal = false;
            bool any = false;

            // Routine 0x581 (VU1 0x2C08): uv' = uv.x * R0 + uv.y * R1 + uv.z
            // * R2 + R3, xy of each; the UV qword's z is 1.0.
            void matrix(const float m[4][4])
            {
                float o[4][2];
                for (int k = 0; k < 4; ++k)
                    for (int c = 0; c < 2; ++c)
                        o[k][c] = r[k][0] * m[0][c] + r[k][1] * m[1][c] + (k == 3 ? m[2][c] + m[3][c] : 0.0f);
                if (!fromNormal)
                {
                    // (s, t, 1): row 2 is a constant too.
                    for (int c = 0; c < 2; ++c)
                    {
                        o[3][c] += o[2][c];
                        o[2][c] = 0.0f;
                    }
                }
                std::memcpy(r, o, sizeof(r));
                any = true;
            }
            // Routine 0x592 (VU1 0x2C90): uv = 0.5 + 0.5 * (n * W * V).xy,
            // n the packed normal / 32768 (ITOF15), W the node's rotation
            // (VU1 1000-1002), V the view matrix's rows 0-2 (1012-1014).
            void sphere(const float world[4][4], const float view[4][4])
            {
                for (int k = 0; k < 3; ++k)
                    for (int c = 0; c < 2; ++c)
                        r[k][c] = 0.5f * (world[k][0] * view[0][c] + world[k][1] * view[1][c] + world[k][2] * view[2][c]);
                r[3][0] = r[3][1] = 0.5f;
                fromNormal = true;
                any = true;
            }
        };

        // The GS register block of a material: its first GIF packet (A+D),
        // which is what the microprogram kicks.
        bool gsBlockBytes(const zx::zaxis::GuestMemory &m, uint32_t block, uint32_t limitQwords, std::vector<uint8_t> &out)
        {
            uint64_t tag = 0;
            const uint8_t *t = m.bytes(block, 16);
            if (!t)
                return false;
            std::memcpy(&tag, t, 8);
            const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
            const uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu) ? static_cast<uint32_t>((tag >> 60) & 0xFu) : 16u;
            const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 3u);
            if (flg != 0u)
                return false;
            const uint32_t qwords = std::min<uint32_t>(1u + nloop * nreg, limitQwords);
            const uint8_t *p = m.bytes(block, qwords * 16u);
            if (!p)
                return false;
            out.assign(p, p + qwords * 16u);
            return true;
        }

        // A terrain material's command list: what the microprogram runs for
        // each of its chunks. False for a routine not drawn here.
        bool readCommands(const zx::zaxis::GuestMemory &m, uint32_t mat, float distance, Item &it)
        {
            uint32_t ncmd = 0;
            if (!m.u32(mat + material::kCommandCount, ncmd) || ncmd > 32u)
                return false;
            for (uint32_t i = 0; i < ncmd; ++i)
            {
                const uint32_t at = mat + material::kCommands + i * 16u;
                uint32_t routine = 0;
                if (!m.u32(at, routine))
                    return false;
                if (routine == 0x4D4u) // draw (terminator)
                    return true;
                if (routine == 0x5AFu) // no-op
                    continue;
                if (routine == 0x581u) // UV matrix: the next four qwords
                {
                    it.uvOps.push_back({routine, at + 16u});
                    i += 4u;
                    continue;
                }
                if (routine == 0x592u) // sphere-mapped UVs from the normals
                {
                    it.uvOps.push_back({routine, 0u});
                    continue;
                }
                if (routine == 0x4DEu) // grass: base, shells, end of chunk
                {
                    int32_t count = 0, alphaStep = 0;
                    float step = 0.0f;
                    if (!m.u32(at + 4u, reinterpret_cast<uint32_t &>(count)) ||
                        !m.u32(at + 8u, reinterpret_cast<uint32_t &>(alphaStep)) || !m.f32(at + 12u, step))
                        return false;
                    it.grass = true;
                    it.alphaStep = alphaStep;
                    it.shellStep = step;
                    // 6 - floor(clamp(d - 6, 0, 100)), at least one: the
                    // loop test reads the count before the subtraction
                    // (VU1 0x27D0/0x27D8, a branch hazard).
                    if (count > 0)
                    {
                        const float k = std::floor(std::min(std::max(distance - 6.0f, 0.0f), 100.0f));
                        it.shells = std::max(1, count - static_cast<int>(k));
                    }
                    return true;
                }
                return false;
            }
            return true;
        }

        void registerNode(Renderer renderer, PS2Runtime::RecompiledFunction original, uint8_t *rdram, R5900Context *ctx,
                          PS2Runtime *runtime)
        {
            State &s = state();
            // The arguments (the original clobbers the registers).
            const uint32_t nodeAddr = getRegU32(ctx, 4);
            const uint32_t cull = getRegU32(ctx, 5);
            const uint32_t packet = getRegU32(ctx, 6);
            const zx::zaxis::GuestMemory m = guest(rdram);
            uint32_t cursor = 0;
            m.u32(packet, cursor);
            // FUN_00210D78 pads the packet to a qword and opens the CALL tag there.
            const uint32_t tagAddress = (cursor + 15u) & ~15u;

            const uint64_t recordDraws = game::host::native_draws::draws();
            original(rdram, ctx, runtime);

            ++s.nodes;
            PS2Memory &memory = runtime->memory();
            // Nothing registered here stays from an earlier frame.
            memory.clearVif1Substitute(tagAddress);
            if (!active(runtime))
                return;
            // Records: the readable renderer took the draw (drawn from its record, or left to its VU1 work).
            if (static_cast<Mode>(s.mode.load(std::memory_order_relaxed)) == Mode::Records)
            {
                if (game::host::native_draws::draws() != recordDraws)
                    return;
                ++s.notFromRecords;
            }
            const bool terrain = renderer == Renderer::Terrain;
            const bool skinnedRenderer = renderer == Renderer::Skinned;
            if (terrain ? !s.terrain : skinnedRenderer && !s.skinned)
                return;
            // Every level (ZX_NATIVE_LEVELS=zoo: the Zoo only).
            uint32_t level = 0;
            if (s.zooOnly && (!m.u32(kLevelIndex, level) || level != kZooIndex))
                return;
            report(runtime);
            sweepParts();

            uint32_t view = 0, part = 0, tableAddr = 0, typeWord = 0, buffers = 0;
            if (!m.u32(kViewIndex, view) || view > 3u || !m.u32(nodeAddr + node::kViewPart + view * node::kViewStride, part) ||
                !m.u32(nodeAddr + node::kMaterialTable, tableAddr) || !m.u32(part + zx::zaxis::part::kRenderType, typeWord) ||
                !m.u32(part + zx::zaxis::part::kBuffers, buffers))
                return;
            const uint32_t type = typeWord >> 26;
            // FUN_0010CE30 draws rigid parts and the sky (VU1 0x18 / 0x1A).
            const bool sky = renderer == Renderer::RigidPrelit && type == static_cast<uint32_t>(zx::zaxis::RenderType::Sky);
            const bool skinnedType = type == static_cast<uint32_t>(zx::zaxis::RenderType::Skinned) ||
                                     type == static_cast<uint32_t>(zx::zaxis::RenderType::SkinnedPrelit);
            const bool expected = terrain          ? type == static_cast<uint32_t>(zx::zaxis::RenderType::Lit)
                                  : skinnedRenderer ? skinnedType
                                                    : type == static_cast<uint32_t>(zx::zaxis::RenderType::RigidPrelit) || sky;
            if (!expected)
            {
                ++s.otherClass;
                return;
            }
            if (sky ? !s.sky : renderer == Renderer::RigidPrelit && !s.rigid)
                return;
            if (!s.view.valid)
            {
                ++s.noView;
                return;
            }
            // The tag the renderer opened: CALL of the drawn buffer's chain.
            const uint32_t drawn = (buffers >> 1) & 1u;
            uint32_t chain = 0, tagLo = 0, tagHi = 0, entries = 0;
            uint16_t count = 0;
            if (!m.u32(tableAddr + table::kChains + drawn * 4u, chain) || !m.u32(tagAddress, tagLo) ||
                !m.u32(tagAddress + 4u, tagHi) || ((tagLo >> 28) & 7u) != 5u || (tagHi & 0x7FFFFFFFu) != (chain & 0x7FFFFFFFu) ||
                !m.u16(tableAddr + table::kCount, count) ||
                !m.u32(part + zx::zaxis::part::kPacketEntries + drawn * 4u, entries))
            {
                ++s.badTag;
                return;
            }
            // Skinned: the packet's layout follows the part's normals stream
            // (FUN_0010BFE0); the bones, matrices and lights are read where
            // VIF1 reaches the node (begin below): the recompiled renderer
            // may come back here before its bone loop is done (a yield),
            // with the rest of the payload written later.
            zx::zaxis::PacketKind kind = terrain ? zx::zaxis::PacketKind::Terrain : zx::zaxis::PacketKind::RigidPrelit;
            const bool lit = type == static_cast<uint32_t>(zx::zaxis::RenderType::Skinned);
            if (skinnedRenderer)
            {
                uint32_t normals = 0;
                if (!m.u32(part + zx::zaxis::part::kNormals, normals))
                    return;
                kind = normals ? zx::zaxis::PacketKind::Skinned : zx::zaxis::PacketKind::SkinnedPrelit;
                if (lit != (kind == zx::zaxis::PacketKind::Skinned))
                {
                    ++s.badUpload;
                    return;
                }
            }
            std::shared_ptr<const zx::zaxis::RigidMesh> mesh = meshOf(m, part, entries, kind);
            if (!mesh)
            {
                ++s.undecodable;
                return;
            }
            // Terrain: this view's distance to the node's sphere (FUN_0010B520
            // has just written it, row 0's w of the uploaded matrix).
            float distance = 0.0f;
            if (terrain && !m.f32(nodeAddr + node::kViewDistance, distance))
                return;

            // What VU1 would draw: per material with a packet, in the chain's
            // order, its GS block and its triangles.
            std::vector<Item> items;
            items.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t meshPacket = 0, mat = 0;
                if (!m.u32(entries + i * zx::zaxis::kPacketEntryBytes, meshPacket))
                    return;
                if (meshPacket == 0u)
                    continue;
                if (!m.u32(tableAddr + table::kMaterials + i * 4u, mat))
                    return;
                Item it;
                it.block = mat + material::kGsBlock;
                if (i < mesh->runs.size())
                {
                    it.first = mesh->runs[i].first;
                    it.count = mesh->runs[i].count;
                }
                if (terrain && !readCommands(m, mat, distance, it))
                {
                    ++s.undecodable;
                    return;
                }
                items.push_back(it);
            }

            // world (w column 0, 0, 0, 1, as the microprogram loads it) x
            // screen, Z column times 16 (FTOI4: the GS's Z value).
            float world[4][4];
            if (!readMatrix(m, nodeAddr + node::kWorld, world))
                return;
            world[0][3] = world[1][3] = world[2][3] = 0.0f;
            world[3][3] = 1.0f;
            GSNativeDraw base;
            base.mesh = std::shared_ptr<const GSNativeMesh>(mesh, mesh->mesh.get());
            multiply(world, s.view.screen, base.transform);
            // Frame interpolation: the node in this view, its world and the view's camera.
            base.motion.key = gsNativeMotionKey(nodeAddr, view);
            std::memcpy(base.motion.world, world, sizeof(world));
            std::memcpy(base.motion.camera, s.view.view, sizeof(base.motion.camera));
            for (int r = 0; r < 4; ++r)
                base.transform[r][2] *= 16.0f;
            base.clip = (cull & 0x22u) == 2u ? GSNativeClip::Near : GSNativeClip::None;
            if (base.clip == GSNativeClip::Near && renderer == Renderer::RigidPrelit)
            {
                // The rigid clipping program (VU1 0x06): against the clip
                // volume, cut at the near plane, split at the midpoints.
                base.clip = GSNativeClip::NearSplit;
                multiply(world, s.view.clip, base.clipTransform);
            }
            if (skinnedRenderer)
            {
                // The skinned programs (VU1 0x158 / 0x1E0, 0x5A0 / 0x5F0):
                // the node's own object -> screen matrix (the skinned
                // position is in object space; set in begin), Z times 16
                // (FTOI4), STQ = (s, t, 1.0) (affine), Gouraud colours; no
                // clipping: a triangle with a vertex outside the guard band
                // or the depth range (CLIPw.xyz of the clip matrix, any
                // flag) is not drawn.
                base.dropFlags = 0x3Fu;
                base.affineUV = true;
                base.clip = GSNativeClip::None;
                ++s.skinnedNodes;
            }
            if (sky)
            {
                // The sky program (VU1 0x2288 / 0x23C0): X, Y as the rigid
                // ones but clamped to 0..4095, Z the integer 1 (behind all
                // but the clear), STQ in perspective, no clipping: a
                // triangle with a vertex behind the clip volume's near side
                // (-z of world x clip, w at least 0) is not drawn.
                for (int r = 0; r < 4; ++r)
                    base.transform[r][2] = base.transform[r][3];
                multiply(world, s.view.clip, base.clipTransform);
                base.dropFlags = 0x20u;
                base.clampXY = true;
                base.clip = GSNativeClip::None;
                ++s.skies;
            }
            if (base.clip != GSNativeClip::None && s.skipClipped)
                return;
            // The clipping programs' midpoint splits make such meshes'
            // colours (nearly) perspective; the others are linear on screen.
            base.perspectiveColour = base.clip != GSNativeClip::None && !s.linearColour;
            // Terrain UV routines: the sphere map's matrix is this node's
            // and view's.
            float sphere[4][2] = {};
            for (const Item &it : items)
                for (const Item::UvOp &op : it.uvOps)
                    if (op.routine == 0x592u)
                    {
                        ++s.sphereMapped;
                        UvMap u;
                        u.sphere(world, s.view.view);
                        std::memcpy(sphere, u.r, sizeof(sphere));
                    }
            base.asPrimitives = static_cast<Mode>(s.mode.load(std::memory_order_relaxed)) == Mode::Primitives;
            base.nearW = s.view.nearW;
            if (terrain)
                base.alpha = 0x80; // the terrain program's base alpha

            PS2Memory::Vif1Substitute sub;
            sub.tag = (static_cast<uint64_t>(tagHi) << 32) | tagLo;
            const uint32_t skinnedProgram = skinnedRenderer ? type + 2u : 0u;
            sub.begin = [runtime, base = std::move(base), items = std::move(items), sphere, skinnedProgram, lit,
                         tagAddress]() mutable -> std::function<void()>
            {
                // EE thread, where VIF1 reaches the node: the GS blocks and
                // UV matrices as the DMA reads them (TEX0 patched by the
                // texture handler).
                const zx::zaxis::GuestMemory m{runtime->memory().getRDRAM()};
                if (skinnedProgram)
                {
                    // The node's upload, complete now: the CALL tag's
                    // payload up to its first microprogram run.
                    auto upload = std::make_unique<zx::zaxis::SkinnedNodeUpload>();
                    std::string why;
                    if (!zx::zaxis::readSkinnedUpload(m, tagAddress + 8u, tagAddress + 8u + 0x2000u, *upload, &why) ||
                        upload->program != skinnedProgram)
                    {
                        State &s = state();
                        if (s.trace && s.badUpload < 8)
                            std::cout << "[native] skinned packet 0x" << std::hex << tagAddress << " program 0x"
                                      << upload->program << std::dec << " stays on VU1: " << (why.empty() ? "program" : why)
                                      << std::endl;
                        ++s.badUpload;
                        return {};
                    }
                    auto skin = std::make_shared<GSNativeSkinning>();
                    std::memcpy(skin->palette, upload->palette, sizeof(skin->palette));
                    skin->lit = lit;
                    std::memcpy(skin->lightDir, upload->lightDir, sizeof(skin->lightDir));
                    std::memcpy(skin->lightColour, upload->lightColour, sizeof(skin->lightColour));
                    std::memcpy(base.transform, upload->screen, sizeof(base.transform));
                    for (int r = 0; r < 4; ++r)
                        base.transform[r][2] *= 16.0f;
                    std::memcpy(base.clipTransform, upload->clip, sizeof(base.clipTransform));
                    // Character Lighting: Xbox (the GPU renderer's option; the
                    // CPU renderer's primitives keep VU1's lights).
                    if (lit && zx::gs_quality::xboxLighting() && runtime->gs().nativeDrawsOnBackend())
                        zx::zaxis::xboxCharacterLights(*skin);
                    base.skin = std::move(skin);
                }
                struct Step
                {
                    Item item;
                    std::vector<uint8_t> block;
                    UvMap uv;
                };
                std::vector<Step> steps(items.size());
                for (size_t i = 0; i < items.size(); ++i)
                {
                    Step &st = steps[i];
                    st.item = items[i];
                    if (!gsBlockBytes(m, st.item.block, 16u, st.block))
                        return {};
                    for (const Item::UvOp &op : st.item.uvOps)
                    {
                        if (op.routine == 0x592u)
                        {
                            std::memcpy(st.uv.r, sphere, sizeof(sphere));
                            st.uv.fromNormal = st.uv.any = true;
                            continue;
                        }
                        float r[4][4];
                        if (!readMatrix(m, op.at, r))
                            return {};
                        st.uv.matrix(r);
                    }
                }
                return [runtime, base, steps = std::move(steps)]()
                {
                    GS &gs = runtime->gs();
                    for (const Step &st : steps)
                    {
                        gs.processGIFPacket(st.block.data(), static_cast<uint32_t>(st.block.size()));
                        GSNativeDraw d = base;
                        d.first = st.item.first;
                        d.count = st.item.count;
                        if (st.uv.any)
                        {
                            d.uvTransform = true;
                            d.uvFromNormal = st.uv.fromNormal;
                            std::memcpy(d.uv, st.uv.r, sizeof(d.uv));
                        }
                        if (!st.item.grass)
                        {
                            if (d.count >= 3u)
                                gs.drawNative(d);
                            continue;
                        }
                        // Grass (VU1 0x26F0): the base opaque with the Z test,
                        // then the shells blended with an alpha test.
                        gs.writeRegister(GS_REG_PRIM, kGrassBasePrim);
                        gs.writeRegister(GS_REG_TEST_1, kGrassBaseTest);
                        if (d.count >= 3u)
                            gs.drawNative(d);
                        gs.writeRegister(GS_REG_PRIM, kGrassShellPrim);
                        gs.writeRegister(GS_REG_TEST_1, kGrassShellTest);
                        if (d.count < 3u)
                            continue;
                        for (int k = 1; k <= st.item.shells; ++k)
                        {
                            GSNativeDraw shell = d;
                            shell.alpha = std::max(0, 0x80 - st.item.alphaStep * k);
                            shell.normalOffset = st.item.shellStep * static_cast<float>(k);
                            gs.drawNative(shell);
                        }
                    }
                };
            };
            memory.setVif1Substitute(tagAddress, std::move(sub));
            ++s.registered;
        }

        // FUN_001110C0: the fog of the view being drawn, as eight flat planes
        // of the fog colour over the view's viewport (into the 2D packet,
        // FUN_00106BB0 with flags 0x2000004: VU1 0x10, GS Z from
        // FUN_00104830, Z test GREATER, blended (Cs - Cd) As + Cd, no Z
        // write), each where the scene lies behind it: n planes give F + (S -
        // F)(1 - As/128)^n, eight even steps between fog start and end.
        // The GPU renderer is told where they are (GSFogPlanes, in stream
        // order before them), so it can draw that fog per pixel from the
        // depth buffer (gs_quality Fog: smooth) and anti-alias the view's 3D
        // picture before the HUD. The original runs; RAM is only read.
        void fogPlanesHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &s = state();
            GSFogPlanes planes;
            bool drawn = false;
            if (active(runtime) && (zx::gs_quality::smoothFog() || zx::gs_quality::fxaa()))
            {
                const zx::zaxis::GuestMemory m = guest(rdram);
                uint32_t cam = 0, on = 0, off = 0, projection = 0;
                float start = 0.0f, end = 0.0f, far = 0.0f;
                float p[4][4];
                if (m.u32(kCurrentCamera, cam) && m.u32(kFogOn, on) && on != 0u && m.u32(cam + camera::kOff, off) &&
                    off == 0u && m.f32(cam + camera::kFogStart, start) && m.f32(cam + camera::kFogEnd, end) &&
                    m.f32(cam + camera::kFar, far) && m.u32(kProjection, projection) && readMatrix(m, projection, p))
                {
                    // As the function: the farthest plane one step in from
                    // min(end, far), each next one a step nearer, eight.
                    const float top = end < far ? end : far;
                    const float step = (end - start) * 0.125f;
                    // A projection (w' = z, z' = M22 z + M32): GS Z = 16 z'/w'.
                    if (start < top && step > 0.0f && p[2][3] == 1.0f && p[3][3] == 0.0f && p[0][3] == 0.0f &&
                        p[1][3] == 0.0f)
                    {
                        planes.count = 8;
                        planes.step = step;
                        planes.nearest = top - 8.0f * step;
                        planes.zA = 16.0f * p[3][2];
                        planes.zB = 16.0f * p[2][2];
                        drawn = planes.nearest > 0.0f;
                    }
                }
            }
            s.origFogPlanes(rdram, ctx, runtime);
            if (!drawn)
                return;
            ++s.fogNotes;
            if (s.trace && (s.fogNotes & 1023u) == 1u)
                std::cout << "[native] fog planes from " << planes.nearest << " every " << planes.step << " (Z = "
                          << planes.zA << " / w + " << planes.zB << ")" << std::endl;
            runtime->memory().postGsSideband([runtime, planes]() { runtime->gs().noteFogPlanes(planes); });
        }

        void drawRigidHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            registerNode(Renderer::RigidPrelit, state().origDrawRigid, rdram, ctx, runtime);
        }

        void drawTerrainHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            registerNode(Renderer::Terrain, state().origDrawTerrain, rdram, ctx, runtime);
        }

        void drawSkinnedHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            registerNode(Renderer::Skinned, state().origDrawSkinned, rdram, ctx, runtime);
        }

        Mode modeFromText(const char *v)
        {
            if (!v || !v[0] || std::strcmp(v, "auto") == 0)
                return Mode::Auto;
            if (std::strcmp(v, "strict") == 0 || std::strcmp(v, "0") == 0)
                return Mode::Strict;
            if (std::strcmp(v, "records") == 0)
                return Mode::Records;
            return Mode::Native;
        }
    }

    using namespace native_draw;

    void installNativeDraw(PS2Runtime &runtime)
    {
        State &s = state();
        s.booted = modeFromText(zx::boot::text("ZX_RENDER_PATH"));
        s.mode.store(static_cast<int>(s.booted));
        // The rigid clipping program's form: VU1 0x06 at 0xF70; the skinned builder sends the stream's four bones
        // and puts a submesh's strips at its own material entry; the Xbox release's character lights replace the
        // default ones with Character Lighting: Xbox.
        game::host::native_draws::attach(runtime, s.booted == Mode::Records, {.clipped = GSNativeClip::NearSplit},
                                         {.xboxLights = true});
        zx::gs_shots::install(runtime); // (ZX_GS_SHOT_VSYNC, developers)
        if (const char *v = std::getenv("ZX_NATIVE_MESHES"))
            s.fileMeshes = std::strcmp(v, "packet") != 0;
        if (const char *v = std::getenv("ZX_TRACE_NATIVE"))
            s.trace = std::atoi(v);
        if (const char *v = std::getenv("ZX_NATIVE_LEVELS"))
            s.zooOnly = std::strcmp(v, "zoo") == 0;
        if (const char *v = std::getenv("ZX_NATIVE_LINEAR_COLOUR"))
            s.linearColour = v[0] == '1';
        if (const char *v = std::getenv("ZX_NATIVE_CLIPPED"))
            s.skipClipped = v[0] == '0';
        s.origViewGlobals = runtime.lookupFunction(kPerViewGlobals);
        s.origDrawRigid = runtime.lookupFunction(kDrawRigid);
        s.origBuildPacket = runtime.lookupFunction(kBuildPartPacket);
        s.origDrawTerrain = runtime.lookupFunction(kDrawTerrain);
        s.origDrawSkinned = runtime.lookupFunction(kDrawSkinned);
        s.origFogPlanes = runtime.lookupFunction(kFogPlanes);
        if (const char *v = std::getenv("ZX_NATIVE_CLASSES"))
        {
            // "4", "0x20", "4,0x20" (render types), "none".
            const std::string list = std::string(",") + v + ",";
            s.rigid = list.find(",4,") != std::string::npos;
            s.terrain = list.find(",0x20,") != std::string::npos || list.find(",32,") != std::string::npos;
            s.sky = list.find(",0x18,") != std::string::npos || list.find(",24,") != std::string::npos;
            s.skinned = list.find(",8,") != std::string::npos || list.find(",0xC,") != std::string::npos ||
                        list.find(",0xc,") != std::string::npos || list.find(",12,") != std::string::npos;
        }
        if (!s.origViewGlobals || !s.origDrawRigid || !s.origBuildPacket || !s.origDrawTerrain || !s.origDrawSkinned ||
            !s.origFogPlanes || !runtime.replaceFunction(kFogPlanes, &fogPlanesHook) ||
            !runtime.replaceFunction(kPerViewGlobals, &viewGlobalsHook) ||
            !runtime.replaceFunction(kBuildPartPacket, &buildPacketHook) ||
            !runtime.replaceFunction(kDrawRigid, &drawRigidHook) ||
            !runtime.replaceFunction(kDrawTerrain, &drawTerrainHook) ||
            !runtime.replaceFunction(kDrawSkinned, &drawSkinnedHook))
        {
            s.origDrawRigid = nullptr;
            std::cerr << "[native] renderer hooks not installed: every mesh through VU1" << std::endl;
            return;
        }
        // The app adds the row with settings.ini behind it; the developer
        // runner gets one without (ZX_CONTROL "set display.native_meshes 0").
        if (!zx::settings::has("display.native_meshes"))
            zx::session::addSetting(nativeDrawSetting(nullptr));
        // The picture quality options (runtime/gs/gs_quality.h): the
        // environment over settings.ini; rows for the developer runner.
        zx::gs_quality::applyEnvironment();
        zx::gs_quality::addSettings(nullptr);
        const Mode mode = static_cast<Mode>(s.mode.load());
        std::cout << "[native] render path "
                  << (mode == Mode::Strict    ? "strict"
                      : mode == Mode::Native  ? "native"
                      : mode == Mode::Records ? "records (native draws from the draw records)"
                                              : "auto (native on the GPU renderer)")
                  << std::endl;
    }

    zx::settings::Setting nativeDrawSetting(std::function<void(bool)> save)
    {
        zx::settings::Setting setting;
        setting.key = "display.native_meshes";
        setting.label = "Native Meshes";
        setting.help = "Levels drawn by the GPU.";
        if (std::getenv("ZX_NATIVE_DEBUG"))
        {
            // 2: native meshes as GS primitives (GS::drawNative's own path).
            setting.kind = zx::settings::Kind::Choice;
            setting.choices = {"Off", "On", "As GS Primitives"};
            setting.maxValue = 2;
        }
        setting.current = []()
        {
            const Mode m = static_cast<Mode>(state().mode.load());
            return m == Mode::Strict ? 0 : m == Mode::Primitives ? 2 : 1;
        };
        setting.apply = [save](int value)
        {
            // On: the path booted with (records stays records), else auto.
            const Mode booted = state().booted == Mode::Records ? Mode::Records : Mode::Auto;
            const Mode mode = value == 2 ? Mode::Primitives : value ? booted : Mode::Strict;
            state().mode.store(static_cast<int>(mode));
            game::host::native_draws::setRecords(mode == Mode::Records);
            if (save)
                save(value != 0);
        };
        return setting;
    }
}
