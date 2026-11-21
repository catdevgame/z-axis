// The class renderers' draws drawn by the host from their records (class_draws.h; render_path = records).
//
// What a draw is made of, from the records alone (zge/render/draw.h): the node's part's mesh as the GEO data gives it
// (zge/native/geo_meshes.h: the triangles the part's packet would give VU1), the node's world matrix and the view's
// world -> screen, world -> clip and world -> view matrices (the camera's, current while the class renderer runs),
// the camera's near clip; skinned, the bones (each driven by one of the node's poses) and the lights the record
// carries; terrain, the grass shells' depth. Per material with triangles, at the draw's place in the stream: its
// first layer's GS block (read there, as VU1 would kick it after the texture interrupt set TEX0), then its triangles,
// with its effect (zge/render/ps2/material_packet.h: grass shells, UV transforms) as the microprogram applies it.
//
// What each class's microprogram does with a vertex (the native draw's fields, gs/gs_native.h):
// - rigid prelit (AI's VU1 0x04 / 0x06 clipped / 0x1E): [x y z 1] * world * screen, X, Y = x'/w', y'/w', GS Z =
//   16 z'/w' (FTOI4) at least 0, STQ = (s, t, 1) / w', the colour as built (halved); the clipping program cuts at
//   the near plane, and its midpoint splits shade such meshes nearly in perspective (GSNativeClip::NearSplit: the
//   CPU renderer gets its triangles);
// - the sky: the same but at GS Z 1 (behind all but the clear), X and Y clamped to the GS window, no clipping: a
//   triangle with a vertex behind the clip volume's near side is left out;
// - terrain (0x20 / 0x22): as rigid, alpha 0x80, the packed normal moving grass shells;
// - skinned (0x0A lit, 0x0E prelit): each vertex moved by up to four bones of the palette, lit by three directional
//   lights and an ambient (lit), textures affine (Q = 1), no clipping: a triangle with a vertex outside the guard
//   band or the depth range is left out.
//
// The strict encoder still runs for the draw, without its VU1 work (zge/render/ps2/class_draws.h): what the game
// reads back stays as on the PS2 path (the texture request and VU1 state in the mesh packet, terrain's shell depth in
// the node's matrix, the bone list's walk, the VU0 work), and a skinned draw's bones, matrices and lights are the
// ones it computes.

#include "zge/host/class_draws.h"

#include "host/native_draws.h"
#include "zge/native/geo_meshes.h"
#include "zge/native/geo_file.h"
#include "zge/native/host.h"
#include "zge/render/material.h"
#include "zge/render/part.h"
#include "zge/render/renderer.h"
#include "zge/render/shadow.h"
#include "zge/render/gs_registers.h"
#include "zge/render/ps2/class_draws.h"
#include "zge/render/ps2/material_packet.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/skeleton.h"
#include "zge/math/vec.h"
#include "zge/container/array.h"

#include "ee/held.h"
#include "ee/readable.h"
#include "sce/packet_calls.h"
#include "runtime/session.h"
#include "runtime/zaxis_mesh.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace zge::host
{
    namespace
    {
        namespace draws = game::host::native_draws;
        using Count = draws::Count;

        // Parts whose packets were built again from changed streams (notePartPacketBuilt): each one's mesh decoded
        // from the packet last built (null until a draw asks), and whether that was decoded since. One boot's.
        struct RebuiltPart
        {
            std::shared_ptr<const zx::zaxis::RigidMesh> mesh;
            bool decoded = false;
        };
        struct Parts
        {
            std::unordered_map<uint32_t, RebuiltPart> streamsChanged;
        };
        Parts &parts() { return zx::session::state<Parts>(); }

        // ---- the view and the node, as floats --------------------------------------------------------------

        struct Rows
        {
            float m[4][4] = {};
        };
        Rows rowsOf(const Matrix &matrix)
        {
            Rows r;
            for (int row = 0; row < 4; ++row)
                for (int c = 0; c < 4; ++c)
                    r.m[row][c] = matrix.m[row][c].raw();
            return r;
        }
        // o = a x b (row vectors: a's transform, then b's).
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
        // The node's world as the microprograms load it: the w column 0, 0, 0, 1.
        Rows affineWorld(const Node &node)
        {
            Rows world = rowsOf(node.world);
            world.m[0][3] = world.m[1][3] = world.m[2][3] = 0.0f;
            world.m[3][3] = 1.0f;
            return world;
        }
        // Where the clipping programs cut, as the view's depth: the clip volume's near side (|z| <= w of world x clip:
        // ZGE2's VU1 0x06 clips against the whole volume, ZGE3's and ZGE4's at its near side, the camera's near clip,
        // 1.25 in a level); the far side and the guard band are the GPU's own clipping. The camera's near clip when
        // the matrices are not of that form.
        float nearClipOfView(const float screen[4][4])
        {
            const Rows clip = rowsOf(**g_clipMatrix);
            float nearW = 0.0f, farW = 0.0f;
            if (zx::zaxis::clipDepthRange(screen, clip.m, nearW, farW) && nearW > 0.0f)
                return nearW;
            const ee::Ptr<Camera> camera(*g_currentCamera);
            const float near = camera ? camera->nearClip.raw() : 0.0f;
            return near > 0.0f && near < 100.0f ? near : 1.25f;
        }

        // ---- the mesh ---------------------------------------------------------------------------------------

        // The mesh of a part whose packet was built again from changed streams (the effects that animate a mesh: DM2's
        // water): its GEO data is not what VU1 draws, the packet just built is. Decoded once per build, as VU1 gets
        // it (the drawn half of the part's double buffer); a skinned packet is not decoded (null: counted).
        std::shared_ptr<const zx::zaxis::RigidMesh> rebuiltMeshOf(const DrawRecord &record, zx::zaxis::PacketKind kind,
                                                           RebuiltPart &rebuilt)
        {
            const bool skinned = kind == zx::zaxis::PacketKind::Skinned || kind == zx::zaxis::PacketKind::SkinnedPrelit;
            if (!rebuilt.decoded && !skinned)
            {
                rebuilt.decoded = true;
                auto mesh = std::make_shared<zx::zaxis::RigidMesh>();
                std::string why;
                if (zx::zaxis::decodeRigidPart(zx::zaxis::GuestMemory{native::gameRam()}, record.part.address, *mesh, &why, kind))
                    rebuilt.mesh = std::move(mesh);
                else
                {
                    rebuilt.mesh.reset();
                    draws::noteNoMesh(record.part.address, static_cast<uint32_t>(record.type), "its rebuilt packet");
                }
            }
            if (!rebuilt.mesh || skinned)
                draws::count(Count::StreamsChanged);
            return skinned ? nullptr : rebuilt.mesh;
        }

        // The part's mesh from the GEO data for the packet kind its renderer draws (null: none, counted).
        std::shared_ptr<const zx::zaxis::RigidMesh> meshOf(const DrawRecord &record, zx::zaxis::PacketKind kind)
        {
            const ZgePart &part = *record.part;
            const auto rebuilt = parts().streamsChanged.find(record.part.address);
            if (rebuilt != parts().streamsChanged.end())
                return rebuiltMeshOf(record, kind, rebuilt->second);
            const bool skinned = kind == zx::zaxis::PacketKind::Skinned || kind == zx::zaxis::PacketKind::SkinnedPrelit;
            std::shared_ptr<const zx::zaxis::RigidMesh> mesh;
            if (skinned || part.boneCount == 0)
                mesh = native::fileMeshForPacketKind(record.part.address, kind, draws::skinnedDraw());
            // A mesh whose submeshes are not the part's materials is not what VU1 draws from the part's packet.
            const bool runsDiffer = mesh && mesh->runs.size() != part.materialCount;
            if (runsDiffer)
                mesh = nullptr;
            if (!mesh)
            {
                draws::count(Count::NoMesh);
                draws::noteNoMesh(record.part.address, static_cast<uint32_t>(record.type),
                                  !(skinned || part.boneCount == 0) ? "a rigid part with bones"
                                  : runsDiffer                      ? "its runs are not the part's materials"
                                  : native::filePartFor(record.part.address) ? "its file part made no mesh"
                                                                             : "no file part");
            }
            return mesh;
        }

        // ---- a part whose streams the game made -----------------------------------------------------------

        // A UV and a normal as the streams hold them (ZGE3's and ZGE4's layouts).
        struct UvRecord
        {
            int16_t s, t; // 4096 = 1.0
        };
        struct NormalRecord
        {
            ee::Float x, y, z;
        };
        static_assert(sizeof(UvRecord) == 4 && sizeof(NormalRecord) == 12);

        // A vertex's record of a stream, through the stream's index table when it has one.
        template <typename T>
        const T &vertexRecord(ee::Ptr<uint8_t> field, uint32_t vertex)
        {
            const ZgeStream &stream = ZgeStream::of(field);
            uint32_t entry = vertex;
            if (stream.indices)
                entry = (stream.wideIndices & 1u) ? stream.indices.cast<uint16_t>()[vertex] : stream.indices[vertex];
            return streamRecords<T>(field)[entry];
        }

        // The part's streams as they are, in the GEO part's form: what its packet was just built from (only BMX
        // XXX's game makes a part's streams, in ZGE4's layout; its strip groups stay after the build).
        std::shared_ptr<const native::GeoFilePart> streamsOf(const ZgePart &part)
        {
            auto out = std::make_shared<native::GeoFilePart>();
            native::GeoFilePart &p = *out;
            p.flags = part.flags;
            p.vertexCount = part.vertexCount;
            p.faceCount = part.faceCount;
            p.groupCount = part.groupCount;
            p.boneCount = part.boneCount;
            p.materialCount = part.materialCount;
            const uint32_t n = part.vertexCount;
            if (part.positions)
            {
                const ee::Ptr<Float4> positions = streamRecords<Float4>(part.positions);
                p.positions.resize(n * 3u);
                for (uint32_t v = 0; v < n; ++v)
                {
                    p.positions[v * 3u] = positions[v].x.raw();
                    p.positions[v * 3u + 1u] = positions[v].y.raw();
                    p.positions[v * 3u + 2u] = positions[v].z.raw();
                }
            }
            if (part.uvs)
            {
                p.hasUvs = true;
                p.uvs.resize(n * 2u);
                for (uint32_t v = 0; v < n; ++v)
                {
                    const UvRecord &uv = vertexRecord<UvRecord>(part.uvs, v);
                    p.uvs[v * 2u] = uv.s;
                    p.uvs[v * 2u + 1u] = uv.t;
                }
            }
            if (part.colours)
            {
                p.hasColours = true;
                p.colours.resize(n * 4u);
                for (uint32_t v = 0; v < n; ++v)
                    std::memcpy(&p.colours[v * 4u], &vertexRecord<VertexColour>(part.colours, v), 4);
            }
            if (part.normals)
            {
                p.hasNormals = true;
                p.normalStride = sizeof(float) * 3u;
                p.normalBytes.resize(n * p.normalStride);
                for (uint32_t v = 0; v < n; ++v)
                {
                    const NormalRecord &normal = vertexRecord<NormalRecord>(part.normals, v);
                    const float xyz[3] = {normal.x.raw(), normal.y.raw(), normal.z.raw()};
                    std::memcpy(&p.normalBytes[v * p.normalStride], xyz, sizeof(xyz));
                }
            }
            if (part.skinWeights)
            {
                const ee::Ptr<uint8_t> weights = streamRecords<uint8_t>(part.skinWeights);
                p.skinWeights.assign(weights.get(), weights.get() + n * 8u);
            }
            if (part.groups)
            {
                ee::Ptr<StripGroup> group = streamRecords<StripGroup>(part.groups);
                for (uint32_t g = 0; g < part.groupCount; ++g, group = StripGroup::after(group))
                {
                    native::GeoGroup &to = p.groups.emplace_back();
                    to.mode = static_cast<uint8_t>(group->submeshMode & 3u);
                    to.material = static_cast<uint8_t>(group->submeshMode >> 8);
                    to.entries.assign(group->indices, group->indices + group->count);
                }
            }
            return out;
        }

        // ---- the materials ----------------------------------------------------------------------------------

        // A material's GS block (its first layer's GIF tag and A+D registers) as VU1 kicks it, read where VIF1
        // reaches the draw; at most 16 quadwords (the tag and seven registers in every material).
        bool gsBlockOf(ee::Ptr<Material> material, std::vector<uint8_t> &out)
        {
            const MaterialLayer &layer = materialLayer(material, 0);
            const uint64_t tag = layer.gifTag[0];
            if ((tag & gs::kGifTagFormat) != 0)
                return false; // not PACKED
            const uint32_t loops = static_cast<uint32_t>(tag & gs::kGifTagLoops);
            const uint32_t registers = static_cast<uint32_t>(tag >> 60) ? static_cast<uint32_t>(tag >> 60) : 16u;
            const uint32_t quadwords = std::min<uint32_t>(1u + loops * registers, 16u);
            static_assert(sizeof(MaterialLayer) >= offsetof(MaterialLayer, gifTag) + 8 * 16);
            if (offsetof(MaterialLayer, gifTag) + quadwords * 16u > sizeof(MaterialLayer))
                return false;
            out.resize(quadwords * 16u);
            std::memcpy(out.data(), layer.gifTag, out.size());
            return true;
        }

        // A UV map: S, T = i0 * r[0] + i1 * r[1] + i2 * r[2] + r[3], (i0, i1, i2) the vertex's (s, t, 1) or, from
        // the normal, its normal / 32768.
        struct UvMap
        {
            float r[4][2] = {{1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};
            bool fromNormal = false;
            bool any = false;

            // A texture matrix (VU1's matrix routine): uv' = uv.x * R0 + uv.y * R1 + uv.z * R2 + R3, xy of each;
            // the UV's z is 1.0.
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
            // The sphere map: uv = 0.5 + 0.5 * (n * W * V).xy, n the packed normal / 32768, W the node's rotation,
            // V the view matrix's rows 0-2.
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

        // The grass shells VU1 draws at the shells' depth: 6 - floor(clamp(depth - 6, 0, 100)), at least one (its
        // loop reads the count before the subtraction: VU1 0x27D0 / 0x27D8, a branch hazard).
        int grassShellsAt(int32_t shells, float depth)
        {
            if (shells <= 0)
                return 0;
            const float k = std::floor(std::min(std::max(depth - 6.0f, 0.0f), 100.0f));
            return std::max(1, shells - static_cast<int>(k));
        }

        // The terrain program's GS words for grass (VU1 0x26F0, 0x2788): the base a textured Gouraud tri strip
        // with no alpha test, the shells blended with alpha GEQUAL 0x40 (failing pixels: the frame buffer only),
        // both Z GREATER (the upper words as the program leaves them).
        constexpr uint64_t kGrassBasePrim = gs::kTriangleStrip | gs::kGouraud | gs::kTextured;
        constexpr uint64_t kGrassBaseTest = 0xFFFF910Cull;
        constexpr uint64_t kGrassShellPrim = kGrassBasePrim | gs::kBlended;
        constexpr uint64_t kGrassShellTest = 0xFFFF940Dull;

        // A material of the draw: where its triangles are in the mesh, what it draws them with.
        struct Item
        {
            ee::Ptr<Material> material;
            uint32_t first = 0, count = 0;
            bool effect = false; // its routines run (the terrain program's; the rigid and skinned ones run none)
            int shells = 0;      // grass shells drawn this time (0: no grass)
        };

        // The draw's materials with triangles, in the table's order (terrain: with their effects); false for an
        // effect this does not draw.
        bool itemsOf(const DrawRecord &record, const zx::zaxis::RigidMesh &mesh, bool effects, float shellDepth,
                     std::vector<Item> &out)
        {
            const MaterialTable &table = *record.materials;
            for (uint32_t i = 0; i < table.count && i < mesh.runs.size(); ++i)
            {
                if (mesh.runs[i].count == 0)
                    continue;
                Item item;
                item.material = table.materials[i];
                item.first = mesh.runs[i].first;
                item.count = mesh.runs[i].count;
                item.effect = effects;
                MaterialEffect effect;
                if (effects && !materialEffectOf(item.material, effect))
                {
                    draws::count(Count::Effect);
                    return false;
                }
                if (effect.grass)
                    item.shells = grassShellsAt(effect.shells, shellDepth);
                out.push_back(item);
            }
            return true;
        }

        // ---- the draw at its place in the stream ------------------------------------------------------------

        // The encoder without the VU1 work begins the draw's DMA tag where the original called the part's packet:
        // the held-register table (ee/held.h placeAs) takes that call for the part's call, so the class renderer's
        // next callees get the registers the original holds there, and their frames hold what they hold on the
        // strict path. Once per game, before its first draw from a record.
        void placeDrawTagAsPartCall()
        {
            static const bool placed = []
            {
                ee::held::placeAs(static_cast<uint32_t>(game::readable::runningGame()), sym::vif1PacketCnt,
                                  sym::vif1PacketCall);
                return true;
            }();
            (void)placed;
        }


        // The draw's anchor appended: when VIF1 reaches it, each material's GS block and UV transforms read as
        // they are then, and the GS work of its triangles returned.
        void addAnchor(sce::Vif1Packet *packet, const GSNativeDraw &base, std::vector<Item> items, const float sphere[4][2])
        {
            struct Step
            {
                Item item;
                std::vector<uint8_t> block;
                UvMap uv;
                MaterialEffect effect;
            };
            float sphereRows[4][2];
            std::memcpy(sphereRows, sphere, sizeof(sphereRows));
            const uint32_t code = draws::anchorCode(
                [base, items = std::move(items), sphereRows]() -> draws::Work
                {
                    std::vector<Step> steps(items.size());
                    for (size_t i = 0; i < items.size(); ++i)
                    {
                        Step &st = steps[i];
                        st.item = items[i];
                        if (!gsBlockOf(st.item.material, st.block) ||
                            (st.item.effect && !materialEffectOf(st.item.material, st.effect)))
                        {
                            draws::countDeclined();
                            return {};
                        }
                        for (uint32_t k = 0; k < st.effect.uvTransformCount; ++k)
                        {
                            const MaterialEffect::UvTransform &t = st.effect.uvTransforms[k];
                            if (t.sphereMap)
                            {
                                std::memcpy(st.uv.r, sphereRows, sizeof(sphereRows));
                                st.uv.fromNormal = st.uv.any = true;
                            }
                            else
                                st.uv.matrix(t.rows);
                        }
                    }
                    return [base, steps = std::move(steps)]()
                    {
                        for (const Step &st : steps)
                        {
                            draws::gifPacket(st.block.data(), static_cast<uint32_t>(st.block.size()));
                            GSNativeDraw d = base;
                            d.first = st.item.first;
                            d.count = st.item.count;
                            if (st.uv.any)
                            {
                                d.uvTransform = true;
                                d.uvFromNormal = st.uv.fromNormal;
                                std::memcpy(d.uv, st.uv.r, sizeof(d.uv));
                            }
                            if (!st.effect.grass)
                            {
                                if (d.count >= 3u)
                                    draws::draw(d);
                                continue;
                            }
                            // Grass: the base opaque with the Z test, then the shells blended with the alpha test.
                            draws::setRegister(gs::kPrim, kGrassBasePrim);
                            draws::setRegister(gs::kTest1, kGrassBaseTest);
                            if (d.count >= 3u)
                                draws::draw(d);
                            draws::setRegister(gs::kPrim, kGrassShellPrim);
                            draws::setRegister(gs::kTest1, kGrassShellTest);
                            if (d.count < 3u)
                                continue;
                            for (int k = 1; k <= st.item.shells; ++k)
                            {
                                GSNativeDraw shell = d;
                                shell.alpha = std::max(0, 0x80 - st.effect.alphaStep * k);
                                shell.normalOffset = st.effect.shellStep * static_cast<float>(k);
                                draws::draw(shell);
                            }
                        }
                    };
                });
            sce::vif1PacketAddCode(packet, code);
        }

        // A rigid, sky or terrain part's native draw: its mesh, materials, transform and clipping; false (counted) for
        // one this does not make.
        struct PartDraw
        {
            GSNativeDraw base;
            std::vector<Item> items;
            UvMap sphere;
            Count drawn = Count::Rigid;
        };
        bool preparePart(const DrawRecord &record, zx::zaxis::PacketKind kind, bool terrain, float shellDepth, PartDraw &out)
        {
            const std::shared_ptr<const zx::zaxis::RigidMesh> mesh = meshOf(record, kind);
            if (!mesh || !itemsOf(record, *mesh, terrain, shellDepth, out.items))
                return false;
            const bool sky = record.type == RenderType::Sky;
            const Rows world = affineWorld(*record.node);
            const Rows screen = rowsOf(**g_screenMatrix);
            GSNativeDraw &base = out.base;
            base.mesh = std::shared_ptr<const GSNativeMesh>(mesh, mesh->mesh.get());
            multiply(world.m, screen.m, base.transform);
            for (int r = 0; r < 4; ++r)
                base.transform[r][2] *= 16.0f; // FTOI4: the GS's Z
            // Frame interpolation: the node in this view, its world and the view's camera.
            base.motion.key = gsNativeMotionKey(record.node.address, record.view);
            std::memcpy(base.motion.world, world.m, sizeof(base.motion.world));
            std::memcpy(base.motion.camera, rowsOf(**g_viewMatrix).m, sizeof(base.motion.camera));
            base.clip = record.clipped() ? GSNativeClip::Near : GSNativeClip::None;
            if (record.clipped() && !terrain)
            {
                // The rigid clipping program: against the clip volume, cut at the near plane, split at the midpoints
                // (each game's form: native_draws::rigidDraw).
                base.clip = draws::rigidDraw().clipped;
                multiply(world.m, rowsOf(**g_clipMatrix).m, base.clipTransform);
            }
            if (sky)
            {
                for (int r = 0; r < 4; ++r)
                    base.transform[r][2] = base.transform[r][3]; // Z = 1
                const Rows clip = rowsOf(**g_clipMatrix);
                multiply(world.m, clip.m, base.clipTransform);
                base.dropFlags = 0x20u; // -z: behind the clip volume's near side
                base.clampXY = true;
                base.clip = GSNativeClip::None;
            }
            base.perspectiveColour = base.clip != GSNativeClip::None;
            base.nearW = nearClipOfView(screen.m);
            if (terrain)
            {
                base.alpha = 0x80; // the terrain program's alpha
                out.sphere.sphere(world.m, rowsOf(**g_viewMatrix).m); // (the sphere map's rows: this node's and view's)
            }
            out.drawn = terrain ? Count::Terrain : sky ? Count::Sky : Count::Rigid;
            return true;
        }
    }

    bool drawFromRecord(sce::Vif1Packet *packet, const RigidDraw &draw)
    {
        if (!draws::fromRecords())
            return false;
        placeDrawTagAsPartCall();
        zx::zaxis::PacketKind kind = zx::zaxis::PacketKind::RigidPrelit;
        packetKindOf(*draw.record.part, draw.record.type, kind);
        if (draw.lights.directions && kind != zx::zaxis::PacketKind::RigidNormals)
        {
            draws::count(Count::Lights); // lights for a part whose packet has no normals
            return false;
        }
        PartDraw part;
        if (!preparePart(draw.record, kind, false, 0.0f, part))
            return false;
        // ZGE4's lit rigid parts (a node with lights of its own): the program lights each vertex from its normal
        // with the node's lights, the directions turned into the node's space (GSNativeLights).
        ps2::RigidLightsUpload upload;
        ps2::encodeRigidDraw(packet, draw, false, draw.lights.directions ? &upload : nullptr);
        if (draw.lights.directions)
        {
            auto lights = std::make_shared<GSNativeLights>();
            const Rows directions = rowsOf(upload.directions), colours = rowsOf(upload.colours);
            for (int r = 0; r < 3; ++r)
                std::memcpy(lights->lightDir[r], directions.m[r], sizeof(lights->lightDir[r]));
            std::memcpy(lights->lightColour, colours.m, sizeof(lights->lightColour));
            part.base.lights = std::move(lights);
        }
        addAnchor(packet, part.base, std::move(part.items), part.sphere.r);
        draws::count(part.drawn);
        return true;
    }

    bool drawFromRecord(sce::Vif1Packet *packet, const TerrainDraw &draw)
    {
        if (!draws::fromRecords())
            return false;
        placeDrawTagAsPartCall();
        PartDraw part;
        if (!preparePart(draw.record, zx::zaxis::PacketKind::Terrain, true, draw.shellDepth.raw(), part))
            return false;
        ps2::encodeTerrainDraw(packet, draw, false);
        addAnchor(packet, part.base, std::move(part.items), part.sphere.r);
        draws::count(part.drawn);
        return true;
    }

    bool drawFromRecord(sce::Vif1Packet *packet, const SkinnedDraw &draw, SkinnedEncoder encode)
    {
        if (!draws::fromRecords())
            return false;
        placeDrawTagAsPartCall();
        const DrawRecord &record = draw.record;
        const ZgePart &part = *record.part;
        // The packet's layout follows the part's normals stream: lit (type 8) has normals, prelit (0xC) colours.
        const bool lit = record.type == RenderType::Skinned;
        const zx::zaxis::PacketKind kind = part.normals ? zx::zaxis::PacketKind::Skinned : zx::zaxis::PacketKind::SkinnedPrelit;
        const int32_t bones = ee::Ptr<PointerArray>(part.bones)->count;
        if (lit != (kind == zx::zaxis::PacketKind::Skinned) ||
            bones * 4 > static_cast<int32_t>(GSNativeSkinning::kPaletteRows))
        {
            draws::count(Count::Kind);
            return false;
        }
        const std::shared_ptr<const zx::zaxis::RigidMesh> mesh = meshOf(record, kind);
        std::vector<Item> items;
        if (!mesh || !itemsOf(record, *mesh, false, 0.0f, items))
            return false;

        // The encoder's own upload: the bones' rows, the matrices and the lights, from its VU0 work.
        ps2::SkinnedUpload upload;
        encode(packet, draw, &upload);
        auto skin = std::make_shared<GSNativeSkinning>();
        for (size_t b = 0; b < upload.bones.size() && b * 4 < static_cast<size_t>(GSNativeSkinning::kPaletteRows); ++b)
        {
            const Rows rows = rowsOf(upload.bones[b]);
            std::memcpy(skin->palette[b * 4], rows.m, sizeof(rows.m));
        }
        skin->lit = lit;
        skin->alphaFromLights = lit && draws::skinnedDraw().alphaFromLights;
        std::memcpy(skin->lightDir, rowsOf(upload.lightDirections).m, sizeof(skin->lightDir));
        std::memcpy(skin->lightColour, rowsOf(upload.lightColours).m, sizeof(skin->lightColour));
        if (lit && draws::xboxCharacterLighting())
            zx::zaxis::xboxCharacterLights(*skin);

        GSNativeDraw base;
        base.mesh = std::shared_ptr<const GSNativeMesh>(mesh, mesh->mesh.get());
        std::memcpy(base.transform, rowsOf(upload.screen).m, sizeof(base.transform));
        for (int r = 0; r < 4; ++r)
            base.transform[r][2] *= 16.0f; // FTOI4
        std::memcpy(base.clipTransform, rowsOf(upload.clip).m, sizeof(base.clipTransform));
        base.dropFlags = 0x3Fu; // outside the guard band or the depth range
        base.affineUV = true;
        base.clip = GSNativeClip::None;
        base.nearW = nearClipOfView(rowsOf(*draw.screen).m);
        base.skin = std::move(skin);
        base.motion.key = gsNativeMotionKey(record.node.address, record.view);
        std::memcpy(base.motion.world, affineWorld(*record.node).m, sizeof(base.motion.world));
        std::memcpy(base.motion.camera, rowsOf(**g_viewMatrix).m, sizeof(base.motion.camera));
        const float noSphere[4][2] = {};
        addAnchor(packet, base, std::move(items), noSphere);
        draws::count(Count::Skinned);
        return true;
    }

    const zx::zaxis::SkinnedDrawForm &skinnedDrawForm() { return draws::skinnedDraw(); }

    bool packetKindOf(const ZgePart &part, RenderType type, zx::zaxis::PacketKind &kind)
    {
        switch (type)
        {
        case RenderType::RigidPrelit:
        case RenderType::Sky:
            kind = draws::rigidDraw().normalsForColours && part.normals ? zx::zaxis::PacketKind::RigidNormals
                                                                         : zx::zaxis::PacketKind::RigidPrelit;
            return true;
        case RenderType::Terrain:
            kind = zx::zaxis::PacketKind::Terrain;
            return true;
        case RenderType::Skinned:
            kind = zx::zaxis::PacketKind::Skinned;
            return true;
        case RenderType::SkinnedPrelit:
            kind = zx::zaxis::PacketKind::SkinnedPrelit;
            return true;
        default:
            return false;
        }
    }

    void notePartPacketBuilt(ee::Ptr<ZgePart> part, bool whole, uint32_t streams)
    {
        // A part whose streams the game made: its mesh is made from them as they are now, whichever were rebuilt.
        const bool made = native::hostExtrasOn() && native::partStreamsMade(part.address);
        if (made && (draws::fromRecords() || native::meshCheckEnabled()))
            native::notePartStreams(part.address, streamsOf(*part));
        if (!draws::fromRecords())
            return;
        if (whole || made)
        {
            parts().streamsChanged.erase(part.address);
            return;
        }
        const auto [it, first] = parts().streamsChanged.try_emplace(part.address);
        it->second = RebuiltPart{}; // decoded from the new packet at its next draw
        if (first)
            draws::noteStreamsChanged(part.address, static_cast<uint32_t>(renderTypeOf(*part)), streams);
    }
}
