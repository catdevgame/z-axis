// Dave Mirra Freestyle BMX 2 (SLUS-20159): native drawing of level meshes on
// the GPU renderer, as Aggressive Inline's (aggressive_inline_native.cpp,
// observe and substitute).
//
// DM2's renderer is the same engine one version older (ZGE2): the rigid
// renderer drawRigid(node, cullFlags, packet) does what AI's drawRigid does,
// with other offsets:
//   - the node's part at node +0x17C (AI: per view at +0x1D0), its material
//     table at +0x178 (AI +0x1B0); the part is AI's TGPT layout (+0x50/+0x54
//     the material entries per packet buffer, +0x60 the render type >> 26,
//     +0x64 bit 1 the buffer drawn);
//   - it opens a DMA CALL tag (vif1PacketCall, as AI's) to the
//     table's chain of the drawn buffer (table +8 / +0xC), whose payload is
//     the VIF state (once a frame), the texture request (MARK, an
//     interrupt-bit MSKPATH3), the world matrix (node +0x50, UNPACK at TOPS)
//     and the microprogram call: MSCAL type, or type + 2 with one more qword
//     (a plane) when (cullFlags & 0x22) == 2, or 0x1A for type 4 with
//     (cullFlags & 0xC) == 0, then MSCAL 0;
//   - the chain has, per material, its GS register block (UNPACKed, kicked
//     by MSCAL 0x1C) and a CALL of the part's packet for that material.
// The packet (built by buildPartPacket -> buildRigid for render types 4 and
// 0x14; rebuilt in place for the streams whose dirty bits are set in part
// +0x3E) is AI's prelit layout exactly: chunks of at most 60 vertices, the
// GIF tag UNPACKed at 8, STMASK/STCYCL 1 of 3, UV V4-16 (s, t times 4096,
// z from ROW, w the strip flags: 0x8000 no triangle), colour V4-8 (halved),
// position V3-32, MSCAL 2 / MSCNT a chunk (runtime/zaxis_mesh.h decodes
// it).
//
// DM2's microprograms (one micro memory image, 836c2d1891f4ba03): 4 (0x5F8,
// loop 0x6E0) and 0x1A (0x980) are AI's 4 / 0x1E: [x y z 1] * world (TOPS
// 0-3) * screen (VU1 1016-1019), X, Y = x'/w', y'/w' clamped to 0..4095, Z =
// 16 z'/w' (FTOI4) at least 0, STQ = (s, t, 1) / w', the colour as given.
// 6 (0x1030, loop 0x1120) clips every triangle against the clip volume
// (VU1 1020-1023: |x|, |y|, |z| <= w, FCAND 0x3FFFF), where AI's 6 cuts at
// the near plane only: the native draw is clipped by the GPU at the volume's
// near plane (derived from the clip and screen matrices); X and Y outside
// the guard band and Z beyond the far plane are the GPU's own clipping.
//
// The per-view globals: beginViewDraws UNPACKs at VU1 0x3F7 the camera's
// plane (*g_viewPlanes + 0x10), the screen matrix *g_screenMatrix and the
// clip matrix *g_clipMatrix.
//
// Settings: the render path (ZX_RENDER_PATH, the app's render_path; the
// Native Meshes row). DM2's default (unset, auto) draws natively from the draw
// records (zge/host/class_draws.cpp: the rider and bike too, which this
// override has no skinned form for), with this override drawing on the GPU
// renderer what the records do not reach; native: this override alone
// (developers); strict: every mesh through VU1 (kFollowRenderPath: DM2 follows
// it, since ailevel6). ZX_DM2_NATIVE=1 / 0 forces the override either way,
// 0 the records off too (developers, A/B runs). ZX_TRACE_DM2NATIVE=1 counts;
// ZX_DM2_NATIVE_DUMP=<n> prints n nodes' payloads and chains from a
// level's 200th frame.
#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_native.cpp.h"

#include "machine.h"
#include "gs/gs_frontend.h"
#include "gs/gs_native.h"
#include "gs/gs_types.h"
#include "runtime/memory.h"
#include "runtime/settings_registry.h"
#include "runtime/zaxis_mesh.h"
#include "runtime/boot_config.h"
#include "runtime/gs_shots.h"

#include "host/native_draws.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dave_mirra
{
    namespace native_draw
    {

        // The hooked functions: beginViewDraws() (the view's VU1 globals), drawRigid(node, cullFlags, packet),
        // buildPartPacket(node, part, new, dirty streams). The view's matrices, each a pointer to 4 rows:
        // *g_screenMatrix world -> GS screen (VU1 1016-1019), *g_clipMatrix world -> clip volume (VU1 1020-1023),
        // *g_viewMatrix world -> view (frame interpolation's camera).
        constexpr uint32_t kSkyType = 0x14u; // part +0x60 >> 26 of the sky (VU1 0x14 / 0x16)

        namespace node
        {
            constexpr uint32_t kWorld = 0x50;
            constexpr uint32_t kTable = 0x178;
            constexpr uint32_t kPart = 0x17C;
        }
        namespace table
        {
            constexpr uint32_t kChains = 0x08; // [2]: the material chain per packet buffer
        }

        // Whether DM2 follows the render path (auto: native on the GPU
        // renderer) when ZX_DM2_NATIVE is not set. On since ailevel6's
        // measurements (BRINGUP "AI levels in DM2, round 6"): Windows'
        // VIF1 and GS workers 8-11x less busy, game logic and the ride checks
        // unchanged.
        constexpr bool kFollowRenderPath = true;

        struct View
        {
            float screen[4][4] = {};
            float clip[4][4] = {};
            float view[4][4] = {}; // world -> view (frame interpolation)
            bool haveView = false;
            float nearW = 0.0f, farW = 0.0f;
            bool valid = false;
        };

        struct CachedPart
        {
            uint32_t entries = 0, firstPacket = 0;
            std::shared_ptr<const zx::zaxis::RigidMesh> mesh;
            uint64_t lastUse = 0;
        };

        struct State
        {
            PS2Machine::RecompiledFunction origBeginViewDraws = nullptr;
            PS2Machine::RecompiledFunction origDrawRigid = nullptr;
            PS2Machine::RecompiledFunction origBuildPacket = nullptr;
            std::atomic<bool> on{false};
            bool records = false;        // render_path = records (installNative)
            uint64_t notFromRecords = 0; // draws the readable renderer did not take (drawn here)
            View view;
            std::unordered_map<uint32_t, CachedPart> parts;
            uint64_t clock = 0, nextSweep = 0;
            int trace = 0;
            int dump = 0;
            uint64_t skies = 0, nodes = 0, registered = 0, undecodable = 0, otherType = 0, noView = 0, badTag = 0, clipped = 0;
            uint64_t rebuilds = 0, decoded = 0, decodeNanos = 0, badChain = 0;
            uint64_t lastReport = 0;
            bool addedSetting = false; // the Native Meshes row is this boot's
        };

        // One boot's hooks, switch and mesh cache: made at the boot's first
        // use (the install), so every boot installs its hooks into its own
        // function table and caches the meshes of its own levels
        // (zx::boot::PerBoot).
        State &state()
        {
            static zx::boot::PerBoot<std::unique_ptr<State>> ofBoot([] { return std::make_unique<State>(); });
            return *ofBoot.get();
        }

        // The run stopped: the Native Meshes row this boot added goes with it
        // (the next game booted adds its own).
        void removeSetting(PS2Machine &)
        {
            State &s = state();
            if (s.addedSetting)
                zx::settings::remove("display.native_meshes");
            s.addedSetting = false;
        }

        bool readMatrix(const zx::zaxis::GuestMemory &m, uint32_t address, float out[4][4])
        {
            const uint8_t *p = m.bytes(address, 64);
            if (!p)
                return false;
            std::memcpy(out, p, 64);
            return true;
        }

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

        bool active(PS2Machine *runtime)
        {
            return state().on.load(std::memory_order_relaxed) &&
                   (state().records || runtime->gs().nativeDrawsOnBackend());
        }

        void beginViewDrawsHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            state().origBeginViewDraws(rdram, ctx, runtime);
            State &s = state();
            ++s.clock;
            const zx::zaxis::GuestMemory m{rdram};
            uint32_t screen = 0, clip = 0;
            s.view.valid = m.u32(sym::dm2::g_screenMatrix, screen) && readMatrix(m, screen, s.view.screen) &&
                           m.u32(sym::dm2::g_clipMatrix, clip) && readMatrix(m, clip, s.view.clip);
            uint32_t view = 0;
            s.view.haveView = m.u32(sym::dm2::g_viewMatrix, view) && readMatrix(m, view, s.view.view);
            if (s.view.valid && !zx::zaxis::clipDepthRange(s.view.screen, s.view.clip, s.view.nearW, s.view.farW))
                s.view.nearW = s.view.farW = 0.0f; // clipped nodes stay on VU1
            if (s.trace && (s.clock % 3600u) == 1u)
                std::cout << "[dm2native] view: near w " << s.view.nearW << ", far w " << s.view.farW << std::endl;
        }

        void buildPacketHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            const uint32_t part = getRegU32(ctx, 5);
            state().origBuildPacket(rdram, ctx, runtime);
            State &s = state();
            if (s.parts.erase(part))
                ++s.rebuilds;
        }

        std::shared_ptr<const zx::zaxis::RigidMesh> meshOf(const zx::zaxis::GuestMemory &m, uint32_t part, uint32_t entries)
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
            const auto started = std::chrono::steady_clock::now();
            auto mesh = std::make_shared<zx::zaxis::RigidMesh>();
            std::string why;
            if (zx::zaxis::decodeRigidPart(m, part, *mesh, &why, zx::zaxis::PacketKind::RigidPrelit))
                now.mesh = mesh;
            else if (s.trace)
                std::cout << "[dm2native] part 0x" << std::hex << part << std::dec << " stays on VU1: " << why << std::endl;
            ++s.decoded;
            s.decodeNanos += static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count());
            now.lastUse = s.clock;
            CachedPart &slot = s.parts[part];
            slot = now;
            return slot.mesh;
        }

        void sweepParts()
        {
            State &s = state();
            if (s.clock < s.nextSweep)
                return;
            s.nextSweep = s.clock + 600;
            for (auto it = s.parts.begin(); it != s.parts.end();)
                it = s.clock - it->second.lastUse > 7200 ? s.parts.erase(it) : std::next(it);
        }

        void report(PS2Machine *runtime)
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
            std::cout << "[dm2native] nodes " << s.nodes << " registered " << s.registered << " clipped " << s.clipped << " sky " << s.skies
                      << " other type " << s.otherType << " undecodable " << s.undecodable << " no view " << s.noView
                      << " bad tag " << s.badTag << " bad chain " << s.badChain << " | packets " << v.packets
                      << " declined " << v.declined << " stale " << v.mismatched << " runs skipped " << v.runsSkipped
                      << " unpacks skipped " << v.unpacksSkipped << " posted " << v.drawsPosted << " lost " << v.drawsLost
                      << " | meshes " << meshes << " (" << (bytes >> 20) << " MB), decoded " << s.decoded << " ("
                      << s.decodeNanos / 1000 << " us), rebuilt " << s.rebuilds << std::endl;
        }

        // One material as the chain draws it: its GS block (what MSCAL 0x1C
        // kicks: the UNPACKed GIF packet) and the packet CALLed after it.
        struct Item
        {
            std::vector<uint8_t> block;
            uint32_t packet = 0;
        };

        // The node's material chain as the DMA reads it now: per CALL of a
        // packet, the last GS block UNPACKed before it. False for a chain of
        // another form.
        bool readChain(const zx::zaxis::GuestMemory &m, uint32_t chain, std::vector<Item> &out, std::string *why, bool dump)
        {
            auto fail = [why](const char *r)
            {
                if (why)
                    *why = r;
                return false;
            };
            std::vector<uint8_t> block;
            size_t skip = 0;      // bytes of the current VIFcode's data still to come
            bool capture = false; // those bytes are the GS block's
            uint32_t address = chain;
            for (int budget = 1024; budget > 0; --budget)
            {
                const uint8_t *tag = m.bytes(address, 16);
                if (!tag)
                    return fail("chain not in memory");
                uint64_t lo = 0;
                std::memcpy(&lo, tag, 8);
                const uint32_t qwc = static_cast<uint32_t>(lo & 0xFFFFu);
                const uint32_t id = static_cast<uint32_t>((lo >> 28) & 7u);
                const uint32_t addr = static_cast<uint32_t>((lo >> 32) & 0x7FFFFFF0u);
                uint32_t data = address + 16u, next = data + qwc * 16u;
                bool end = false, call = false;
                switch (id)
                {
                case 1: break;              // cnt
                case 2: next = addr; break; // next
                case 3:                     // ref
                case 4:                     // refs
                    data = addr;
                    next = address + 16u;
                    break;
                case 5: call = true; break; // call: its data, then the packet
                case 6:                     // ret
                case 7: end = true; break;  // end
                default: return fail("chain has other DMA tags");
                }
                if (dump)
                {
                    uint32_t v0 = 0, v1 = 0;
                    std::memcpy(&v0, tag + 8, 4);
                    std::memcpy(&v1, tag + 12, 4);
                    std::printf("[dm2native]   tag 0x%08x id %u qwc %u addr 0x%08x vif %08x %08x\n", address, id, qwc,
                                addr, v0, v1);
                }
                // The VIFcodes: the tag's upper 64 bits, then its data (read
                // in place; a VIFcode's data may continue past the tag).
                const uint8_t *data8 = nullptr;
                if (qwc && !(data8 = m.bytes(data, qwc * 16u)))
                    return fail("chain data not in memory");
                const uint8_t *segments[2] = {tag + 8, data8};
                const size_t sizes[2] = {8u, qwc * 16u};
                for (int seg = 0; seg < 2; ++seg)
                {
                    const uint8_t *p = segments[seg];
                    size_t pos = 0;
                    const size_t size = sizes[seg];
                    while (pos < size)
                    {
                        if (skip)
                        {
                            const size_t n = std::min(skip, size - pos);
                            if (capture)
                                block.insert(block.end(), p + pos, p + pos + n);
                            skip -= n;
                            pos += n;
                            continue;
                        }
                        capture = false;
                        if (pos + 4u > size)
                            return fail("chain VIFcode cut");
                        uint32_t cmd = 0;
                        std::memcpy(&cmd, p + pos, 4);
                        pos += 4;
                        const uint8_t op = static_cast<uint8_t>((cmd >> 24) & 0x7Fu);
                        const uint32_t num = (cmd >> 16) & 0xFFu;
                        if (dump)
                            std::printf("[dm2native]     vif %08x\n", cmd);
                        if ((op & 0x60u) == 0x60u)
                        {
                            // UNPACK V4-32 of the GS block (what MSCAL 0x1C kicks).
                            if (((op >> 2) & 3u) != 3u || (op & 3u) != 0u)
                                return fail("chain has other unpacks");
                            skip = (num ? num : 256u) * 16u;
                            capture = true;
                            block.clear();
                            continue;
                        }
                        switch (op)
                        {
                        case 0x20: skip = 4; break;             // STMASK
                        case 0x30: case 0x31: skip = 16; break; // STROW, STCOL
                        case 0x4A: return fail("chain has MPG");
                        case 0x50: case 0x51: return fail("chain has DIRECT");
                        default: break;
                        }
                    }
                }
                if (call)
                {
                    if (block.empty() || skip)
                        return fail("packet before any GS block");
                    out.push_back({block, addr});
                    address = next; // the packet returns here
                    continue;
                }
                if (end)
                    return true;
                address = next;
            }
            return fail("chain too long");
        }

        void dumpNode(const zx::zaxis::GuestMemory &m, uint32_t nodeAddr, uint32_t cull, uint32_t part, uint32_t type,
                      uint32_t tagAddress, uint32_t chain)
        {
            std::printf("[dm2native] node 0x%08x cull 0x%x part 0x%08x type 0x%x tag 0x%08x chain 0x%08x\n", nodeAddr, cull,
                        part, type, tagAddress, chain);
            // The payload: the CALL tag's upper words and what follows, up to
            // 48 words.
            for (uint32_t i = 0; i < 48; ++i)
            {
                uint32_t w = 0;
                if (!m.u32(tagAddress + 8u + i * 4u, w))
                    break;
                std::printf("[dm2native]   payload +%02x %08x\n", 8u + i * 4u, w);
            }
            std::vector<Item> items;
            std::string why;
            if (!readChain(m, chain, items, &why, true))
                std::printf("[dm2native]   chain: %s\n", why.c_str());
            for (const Item &it : items)
            {
                std::printf("[dm2native]   item packet 0x%08x block %zu bytes:", it.packet, it.block.size());
                for (size_t k = 0; k + 8u <= it.block.size(); k += 8u)
                {
                    uint64_t q = 0;
                    std::memcpy(&q, &it.block[k], 8);
                    std::printf(" %016llx", static_cast<unsigned long long>(q));
                }
                std::printf("\n");
            }
            // The first material packet's first VIFcodes.
            if (!items.empty())
            {
                for (uint32_t i = 0; i < 24; ++i)
                {
                    uint32_t w = 0;
                    if (!m.u32(items[0].packet + i * 4u, w))
                        break;
                    std::printf("[dm2native]   packet +%02x %08x\n", i * 4u, w);
                }
            }
            std::fflush(stdout);
        }

        void drawRigidHook(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime)
        {
            State &s = state();
            const uint32_t nodeAddr = getRegU32(ctx, 4);
            const uint32_t cull = getRegU32(ctx, 5);
            const uint32_t packet = getRegU32(ctx, 6);
            const zx::zaxis::GuestMemory m{rdram};
            uint32_t cursor = 0;
            m.u32(packet, cursor);
            const uint32_t tagAddress = (cursor + 15u) & ~15u; // vif1PacketCall opens the CALL tag there

            const uint64_t recordDraws = game::host::native_draws::draws();
            s.origDrawRigid(rdram, ctx, runtime);

            ++s.nodes;
            PS2Memory &memory = runtime->memory();
            memory.clearVif1Substitute(tagAddress);
            if (!active(runtime) && s.dump <= 0)
                return;
            report(runtime);
            sweepParts();

            uint32_t part = 0, tableAddr = 0, typeWord = 0, buffers = 0;
            if (!m.u32(nodeAddr + node::kPart, part) || !m.u32(nodeAddr + node::kTable, tableAddr) ||
                !m.u32(part + zx::zaxis::part::kRenderType, typeWord) || !m.u32(part + zx::zaxis::part::kBuffers, buffers))
                return;
            const uint32_t type = typeWord >> 26;
            const uint32_t drawn = (buffers >> 1) & 1u;
            uint32_t chain = 0, tagLo = 0, tagHi = 0, entries = 0;
            const bool tagOk = m.u32(tableAddr + table::kChains + drawn * 4u, chain) && m.u32(tagAddress, tagLo) &&
                               m.u32(tagAddress + 4u, tagHi) && ((tagLo >> 28) & 7u) == 5u &&
                               (tagHi & 0x7FFFFFFFu) == (chain & 0x7FFFFFFFu) &&
                               m.u32(part + zx::zaxis::part::kPacketEntries + drawn * 4u, entries);
            uint32_t levelFrame = 0;
            if (s.dump > 0 && tagOk && m.u32(sym::dm2::g_physicsTaskFrames, levelFrame) && levelFrame >= 200u) // a level's 200th frame on
            {
                --s.dump;
                dumpNode(m, nodeAddr, cull, part, type, tagAddress, chain);
            }
            if (!active(runtime))
                return;
            // Records (render_path = records): the readable renderer took the draw (from its record, or its VU1
            // work); one it did not reach is drawn from the packets here.
            if (game::host::native_draws::recordsWanted())
            {
                if (game::host::native_draws::draws() != recordDraws)
                    return;
                ++s.notFromRecords;
            }
            // Type 0x14 is DM2's sky (VU1 0x14 / 0x16 at 0xC88: AI's 0x18).
            const bool sky = type == kSkyType;
            if (type != static_cast<uint32_t>(zx::zaxis::RenderType::RigidPrelit) && !sky)
            {
                ++s.otherType;
                return;
            }
            if (!s.view.valid)
            {
                ++s.noView;
                return;
            }
            if (!tagOk)
            {
                ++s.badTag;
                return;
            }
            std::shared_ptr<const zx::zaxis::RigidMesh> mesh = meshOf(m, part, entries);
            if (!mesh)
            {
                ++s.undecodable;
                return;
            }
            // Material k's packet -> its run of the mesh.
            uint8_t materials = 0;
            if (!m.u8(part + zx::zaxis::part::kMaterialCount, materials))
                return;
            std::vector<std::pair<uint32_t, zx::zaxis::RigidMesh::Run>> runs;
            runs.reserve(materials);
            for (uint32_t k = 0; k < materials && k < mesh->runs.size(); ++k)
            {
                uint32_t p = 0;
                if (!m.u32(entries + k * zx::zaxis::kPacketEntryBytes, p))
                    return;
                if (p)
                    runs.push_back({p, mesh->runs[k]});
            }

            float world[4][4];
            if (!readMatrix(m, nodeAddr + node::kWorld, world))
                return;
            world[0][3] = world[1][3] = world[2][3] = 0.0f;
            world[3][3] = 1.0f;
            GSNativeDraw base;
            base.mesh = std::shared_ptr<const GSNativeMesh>(mesh, mesh->mesh.get());
            multiply(world, s.view.screen, base.transform);
            if (s.view.haveView)
            {
                // Frame interpolation: the node (ZGE2 has one view), its world and the view's camera.
                base.motion.key = gsNativeMotionKey(nodeAddr, 0u);
                std::memcpy(base.motion.world, world, sizeof(world));
                std::memcpy(base.motion.camera, s.view.view, sizeof(base.motion.camera));
            }
            for (int r = 0; r < 4; ++r)
                base.transform[r][2] *= 16.0f;
            const bool clipped = (cull & 0x22u) == 2u;
            if (sky)
            {
                // The sky program: X, Y as the rigid ones but clamped to
                // 0..4095, Z the integer 1, STQ in perspective, no clipping:
                // a triangle with a vertex behind the clip volume's near side
                // (-z of world x clip, w at least 0; FCAND 0x020820) is not
                // drawn. The same for its clipping entry (0x16).
                for (int r = 0; r < 4; ++r)
                    base.transform[r][2] = base.transform[r][3];
                multiply(world, s.view.clip, base.clipTransform);
                base.dropFlags = 0x20u;
                base.clampXY = true;
                base.clip = GSNativeClip::None;
                ++s.skies;
            }
            else if (clipped)
            {
                ++s.clipped;
                if (!(s.view.nearW > 0.0f))
                    return;
                // The clipping program (0x06 at 0x1030): against the clip
                // volume, cut at the near plane, split at the midpoints.
                base.clip = GSNativeClip::NearSplitAndFar;
                base.nearW = s.view.nearW;
                multiply(world, s.view.clip, base.clipTransform);
            }
            base.perspectiveColour = clipped && !sky;

            PS2Memory::Vif1Substitute sub;
            sub.tag = (static_cast<uint64_t>(tagHi) << 32) | tagLo;
            sub.begin = [runtime, base = std::move(base), runs = std::move(runs), chain]() -> std::function<void()>
            {
                // EE thread, where VIF1 reaches the node: the chain's GS
                // blocks as the DMA reads them (TEX0 patched by the texture
                // interrupt handler).
                const zx::zaxis::GuestMemory m{runtime->memory().getRDRAM()};
                std::vector<Item> items;
                std::string why;
                if (!readChain(m, chain, items, &why, false))
                {
                    State &s = state();
                    if (s.trace && s.badChain < 8)
                        std::cout << "[dm2native] chain 0x" << std::hex << chain << std::dec << " stays on VU1: " << why
                                  << std::endl;
                    ++s.badChain;
                    return {};
                }
                struct Step
                {
                    std::vector<uint8_t> block;
                    zx::zaxis::RigidMesh::Run run;
                };
                std::vector<Step> steps;
                steps.reserve(items.size());
                for (Item &it : items)
                {
                    const auto r = std::find_if(runs.begin(), runs.end(),
                                                [&](const std::pair<uint32_t, zx::zaxis::RigidMesh::Run> &x)
                                                { return x.first == it.packet; });
                    if (r == runs.end())
                    {
                        ++state().badChain;
                        return {};
                    }
                    steps.push_back({std::move(it.block), r->second});
                }
                return [runtime, base, steps = std::move(steps)]()
                {
                    GS &gs = runtime->gs();
                    for (const Step &st : steps)
                    {
                        gs.processGIFPacket(st.block.data(), static_cast<uint32_t>(st.block.size()));
                        GSNativeDraw d = base;
                        d.first = st.run.first;
                        d.count = st.run.count;
                        if (d.count >= 3u)
                            gs.drawNative(d);
                    }
                };
            };
            memory.setVif1Substitute(tagAddress, std::move(sub));
            ++s.registered;
        }
    }

    bool installNative(PS2Machine &runtime)
    {
        namespace nd = native_draw;
        nd::State &s = nd::state();
        {
            // ZX_DM2_NATIVE over the render path; strict: through VU1.
            const char *forced = std::getenv("ZX_DM2_NATIVE");
            const char *path = zx::boot::text("ZX_RENDER_PATH");
            const bool strict = path && (std::strcmp(path, "strict") == 0 || std::strcmp(path, "0") == 0);
            const bool overrideAlone = path && std::strcmp(path, "native") == 0;
            const bool records = !strict && !overrideAlone;
            if (forced && forced[0])
                s.on = forced[0] == '1';
            else
                s.on = nd::kFollowRenderPath && !strict;
            // records (the default): the readable class renderers draw from their records (zge/host/class_draws.cpp),
            // with either renderer; what they do not reach is drawn here as on the GPU renderer.
            s.records = records && !(forced && forced[0] == '0');
            // The rigid clipping program's form: VU1 0x06 at 0x1030; the skinned builder (ZGE2's buildSkinned)
            // drops a vertex's light bones and sends three, fills the material table in the submeshes' order and
            // sends the UVs as floats; its lit program (VU1 0x118) lights the alpha too.
            game::host::native_draws::attach(runtime, s.records, {.clipped = GSNativeClip::NearSplitAndFar},
                                             {.weights = zx::zaxis::SkinnedDrawForm::Weights::LightBonesDropped,
                                              .materialsInGroupOrder = true,
                                              .uvsAsFloats = true,
                                              .alphaFromLights = true},
                                             /*followInterpolation: its riders are native only from their records*/ s.on);
            zx::gs_shots::install(runtime); // (ZX_GS_SHOT_VSYNC, developers)
        }
        if (const char *v = std::getenv("ZX_TRACE_DM2NATIVE"))
            s.trace = std::atoi(v);
        if (const char *v = std::getenv("ZX_DM2_NATIVE_DUMP"))
            s.dump = std::atoi(v);
        s.origBeginViewDraws = runtime.lookupFunction(sym::dm2::beginViewDraws);
        s.origDrawRigid = runtime.lookupFunction(sym::dm2::drawRigid);
        s.origBuildPacket = runtime.lookupFunction(sym::dm2::buildPartPacket);
        if (!s.origBeginViewDraws || !s.origDrawRigid || !s.origBuildPacket ||
            !runtime.replaceFunction(sym::dm2::beginViewDraws, &nd::beginViewDrawsHook) ||
            !runtime.replaceFunction(sym::dm2::buildPartPacket, &nd::buildPacketHook) ||
            !runtime.replaceFunction(sym::dm2::drawRigid, &nd::drawRigidHook))
        {
            s.origDrawRigid = nullptr;
            std::cerr << "[dm2native] renderer hooks not installed: every mesh through VU1" << std::endl;
            return false;
        }
        // The Native Meshes row (Aggressive Inline's key), unless the app
        // added one; switched while the game runs.
        if (!zx::settings::has("display.native_meshes"))
        {
            zx::settings::Setting setting;
            setting.key = "display.native_meshes";
            setting.label = "Native Meshes";
            setting.help = "Levels drawn by the GPU.";
            setting.current = []() { return nd::state().on.load() ? 1 : 0; };
            setting.apply = [](int value)
            {
                nd::state().on.store(value != 0);
                game::host::native_draws::setRecords(value != 0 && nd::state().records);
                game::host::native_draws::setMeshesOn(value != 0);
            };
            zx::settings::add(setting);
            s.addedSetting = true;
            runtime.addStoppedHook(&nd::removeSetting);
        }
        std::cout << "[dm2native] level meshes "
                  << (s.records ? "native from the draw records" : s.on ? "native on the GPU renderer" : "through VU1")
                  << std::endl;
        return true;
    }
}
