// The scene draw (drawScene): the world's nodes culled, given their level
// of detail and queued in the draw buckets; the view's culling registers it
// tests against, and a node's tree drawn the same way (the sky's: drawNodeTree)
// (zge/render/node_tree.cpp; the frame block, drawScene-endViewOverlay).
//
// loadViewCulling (beginViewDraws, per view) leaves in VU0 the frustum's
// planes as columns (the four side planes' x, y, z and -w, one plane a lane;
// then the near and far planes' in x and y) and the camera's world -> view
// matrix: they stay there for the view's whole draw. A node's bounding sphere is
// tested against them in four multiply-adds a plane set, then, for a sphere
// across a plane, the eight corners of its model's box against the camera's
// world planes.

#include "draw.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/collision/collision.h"
#include "zge/math/vec.h"
#include "zge/math/vu0_ops.h"
#include "zge/render/part.h"
#include "zge/render/terrain.h"
#include "zge/render/texture.h"
#include "zge/debug/profiler.h"
#include "zge/scene/scene.h"
#include "zge/scene/camera.h"
#include "zge/scene/node.h"
#include "zge/scene/query.h"
#include "zge/scene/visibility_regions.h"
#include "zge/host/game_view.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include "sym/zge/render/node_tree.cpp.h"

namespace zge
{
    namespace
    {
        using zge::vu0::Registers;
        using namespace zge::vu0;

        // Never set: a debug switch that leaves the scene undrawn.
        inline ee::Global<uint32_t, sym::g_sceneDrawOff> g_sceneDrawOff;
        // A region whose mark equals it is out of view (markRegionsOutOfView).
        inline ee::Global<uint32_t, sym::g_outOfViewMark> g_outOfViewMark;

        // A node whose shadow part was queued (bucket 4 holds the casters' parts) is
        // drawn from this bucket.
        constexpr int32_t kCasterBucket = 5;

        // The cull result (the node's cull flags, a draw's): CullFlags, draw.h.

        // The node's record of the current view (through ee::Ptr: the view number is
        // the game's, guest addressing wraps as the original's does).
        NodeView &viewOf(Node &node) { return ee::Ptr<NodeView>(node.views)[*g_currentView]; }

        // A part's flags (ZgePart::flags): the shadow part (the view part's
        // next) is queued when skinned.
        constexpr uint16_t kShadowPartFlag = kPartSkinned;

        // The view's planes as columns, as they go to VU0: the four side planes' x, y,
        // z and -w, one plane a lane; then the near and far planes' in x and y.
        struct alignas(16) PlaneColumns
        {
            ee::Float lane[8][4];
        };

        // The view's culling, which loadViewCulling leaves in VU0 for the whole of the
        // view's draw: the frustum's plane columns and the camera's world -> view
        // matrix. A sphere goes in as centre and radius (w).
        class ViewCulling
        {
        public:
            ViewCulling() = default;
            // On VU0 registers of its own (the game's view beside the drawn one).
            explicit ViewCulling(vu0::Registers::Beside beside) : m_vu(beside) {}

            void load(const PlaneColumns &columns, const Matrix &fromWorld)
            {
                for (int column = 0; column < 8; ++column)
                    m_vu.load(kColumns + column, columns.lane[column]);
                for (int row = 0; row < 4; ++row)
                    m_vu.load(kFromWorld + row, &fromWorld.row(row));
            }

            // A sphere's centre into view space, ready for the plane tests.
            void takeNodeSphere(const Vec4 &sphere)
            {
                m_vu.load(kSphere, &sphere);
                m_vu.transform(kViewCentre, kFromWorld, kFromWorld + 3, kSphere);
                loadRadiusScale();
            }
            void takeWorldSphere(const Vec4 &sphere)
            {
                m_vu.load(kSphere, &sphere);
                loadRadiusScale();
            }
            void storeViewCentre(Vec4 &centre) { m_vu.store(kViewCentre, &centre); }

            // The sphere against the side planes, then the near and far planes, with its
            // radius added (outside a plane) or taken off (across one): any negative.
            bool behindAPlane(bool radiusTakenOff)
            {
                const uint32_t sides = planesNegative(kColumns, kXyzw, radiusTakenOff);
                m_vu.mulAcc(kXyzw, kColumns + 7, 0, kLaneW);
                return ((planesRest(kColumns + 4, kXy, radiusTakenOff) | sides) & kSignFlag) != 0;
            }

            // A node culled as part of a walk that does not cull: its centre in view
            // space, as the walk records it.
            void viewCentreOf(const Vec4 &sphere, Vec4 &centre)
            {
                m_vu.load(kUnculledSphere, &sphere);
                m_vu.transform(kUnculledCentre, kFromWorld, kFromWorld + 3, kUnculledSphere);
                m_vu.store(kUnculledCentre, &centre);
            }

        private:
            static constexpr int kRadiusScale = 1, kSphere = 2, kViewCentre = 3, kColumns = 16, kFromWorld = 24;
            static constexpr int kUnculledSphere = 1, kUnculledCentre = 2;
            static constexpr uint32_t kSignFlag = 0x2u; // a lane came out negative

            // (1, 1, 1, 1): VU0's constant register's w, the columns' radius scale.
            void loadRadiusScale() { m_vu.maximumBc(kXyzw, kRadiusScale, 0, 0, kLaneW); }

            uint32_t planesRest(int columns, Dest lanes, bool radiusTakenOff)
            {
                m_vu.maddAcc(lanes, columns, kSphere, kLaneX);
                m_vu.maddAcc(lanes, columns + 1, kSphere, kLaneY);
                m_vu.maddAcc(lanes, columns + 2, kSphere, kLaneZ);
                radiusTakenOff ? m_vu.msubAcc(lanes, kRadiusScale, kSphere, kLaneW) : m_vu.maddAcc(lanes, kRadiusScale, kSphere, kLaneW);
                return m_vu.status();
            }
            uint32_t planesNegative(int columns, Dest lanes, bool radiusTakenOff)
            {
                m_vu.mulAcc(lanes, columns + 3, 0, kLaneW);
                return planesRest(columns, lanes, radiusTakenOff);
            }

            Registers m_vu;
        };

        // The box of the node's model, in the node's space.
        struct ModelBox
        {
            uint8_t header[0x10];
            Vec4 min, max;
        };

        // A box being tested: the corner being made, then the eight in the world.
        struct BoxCorners
        {
            Vec4 corner;
            Vec4 world[8];
        };

        // The box of the node's model against the camera's world planes, its eight
        // corners into the world first: kCullOutside when every corner is outside one
        // plane; kCullInFrontOfFirst when the first of six has them all in front.
        uint32_t boxAgainstPlanes(const Node &node, const Plane *planes, uint32_t planeCount, BoxCorners &box)
        {
            const ModelBox &model = *node.model.cast<ModelBox>();
            box.corner.w = 1.0f;
            for (uint32_t i = 8; i != 0; --i)
            {
                box.corner.x = (i & 1u) ? model.min.x : model.max.x;
                box.corner.y = (i & 2u) ? model.min.y : model.max.y;
                box.corner.z = (i & 4u) ? model.min.z : model.max.z;
                box.world[8 - i] = transformed(node.world, box.corner);
            }

            const auto inFront = [&](const Plane &plane, uint32_t corner) { return ee::Float(0.0f) <= dot(box.world[corner], plane) - plane.w; };
            uint32_t result = 0;
            uint32_t index = 0;
            for (uint32_t planesLeft = planeCount; planesLeft != 0; --planesLeft, ++index)
            {
                const Plane &plane = planes[index];
                uint32_t corner = 0;
                while (corner < 8 && !inFront(plane, corner))
                    ++corner;
                if (corner == 8)
                    return kCullOutside;
                // The first plane, the first corner in front: the other seven too?
                if (planesLeft == 6 && corner == 0)
                {
                    uint32_t next = 1;
                    while (next < 8 && inFront(plane, next))
                        ++next;
                    if (next == 8)
                        result = kCullInFrontOfFirst;
                }
            }
            return result;
        }

        // A node culled: its CullFlags; its sphere's centre in view space recorded
        // unless it is outside the view.
        // `planes`: the frustum's world planes for the box test, null for the camera's own (read only when the
        // sphere crosses a side, as the original reads them).
        uint32_t cullNode(ViewCulling &culling, const Node &node, Vec4 &viewCentre, const Camera &camera, const Plane *planes,
                          uint32_t planeCount, BoxCorners &box, bool *centreTaken = nullptr)
        {
            culling.takeNodeSphere(node.sphereCentre);
            uint32_t result = kCullOutside;
            if (!culling.behindAPlane(false))
            {
                // Inside every plane with the radius taken off: wholly in the view.
                const bool across = culling.behindAPlane(true);
                culling.storeViewCentre(viewCentre);
                if (centreTaken)
                    *centreTaken = true;
                if (!across)
                    result = 0;
                else
                {
                    // Across a plane: the near (as well), the far, else a side.
                    const ee::Float radius = node.radius;
                    const ee::Float depth = viewCentre.z;
                    result = kCullSide | kCullNear;
                    if (!(depth - radius < camera.nearClip))
                    {
                        result = kCullSide;
                        if (camera.farClip < depth + radius)
                            result = kCullFar;
                    }
                }
            }
            if (result & kCullSide)
                result |= planes ? boxAgainstPlanes(node, planes, planeCount, box)
                                 : boxAgainstPlanes(node, camera.planes, camera.planeCount, box);
            return result;
        }
        uint32_t cullNode(ViewCulling &culling, const Node &node, NodeView &view, const Camera &camera, BoxCorners &box,
                          bool *centreTaken = nullptr)
        {
            return cullNode(culling, node, view.viewCentre, camera, nullptr, 0u, box, centreTaken);
        }

        // The game's own view (zge/host/game_view.h), culled beside the drawn
        // one when the view settings widened the drawn frustum: on VU0
        // registers of its own, the guest's kept as the drawn culling leaves
        // them, and with boxes and view centres of its own. A node is marked
        // drawn (kViewDrawn) by this culling and queued by the drawn one.
        class GameCulling
        {
        public:
            explicit GameCulling(const Camera &camera, const host::GameFrustum &frustum)
                : m_camera(camera), m_frustum(frustum), m_culling(vu0::Registers::Beside{})
            {
                PlaneColumns columns{};
                for (int plane = 0; plane < 4; ++plane)
                {
                    columns.lane[0][plane] = frustum.planes[plane].x;
                    columns.lane[1][plane] = frustum.planes[plane].y;
                    columns.lane[2][plane] = frustum.planes[plane].z;
                    columns.lane[3][plane] = -frustum.planes[plane].w;
                }
                for (int plane = 0; plane < 2; ++plane)
                {
                    columns.lane[4][plane] = frustum.planes[4 + plane].x;
                    columns.lane[5][plane] = frustum.planes[4 + plane].y;
                    columns.lane[6][plane] = frustum.planes[4 + plane].z;
                    columns.lane[7][plane] = -frustum.planes[4 + plane].w;
                }
                m_culling.load(columns, frustum.fromWorld);
            }

            // The node's cull flags in the game's view. A node the drawn view
            // left out (its view centre not taken: a drawn view narrower than
            // the game's somewhere) gets its view centre as the game's view
            // takes it, for its level of detail.
            uint32_t cull(const Node &node, NodeView &view, bool drawnCentreTaken)
            {
                vu0::KeptRegisters kept; // (the box test's corners go through the running context's VU0)
                bool taken = false;
                m_centre.z = 0.0f;
                const uint32_t flags = cullNode(m_culling, node, m_centre, m_camera, m_frustum.planes, m_frustum.planeCount, m_box, &taken);
                if (taken && !drawnCentreTaken)
                    view.viewCentre = m_centre;
                return flags;
            }
            // The last culled node's depth in the game's view (its level of detail there).
            ee::Float depth() const { return m_centre.z; }
            // A sector's sphere wholly outside the game's view.
            bool sectorOutside(const Vec4 &sphere)
            {
                m_culling.takeWorldSphere(sphere);
                return m_culling.behindAPlane(false);
            }

        private:
            const Camera &m_camera;
            const host::GameFrustum &m_frustum;
            ViewCulling m_culling;
            BoxCorners m_box;
            Vec4 m_centre;
        };

        // ZGE4's view tests (partInView's, through nodeVisibleInView and sectorInView; zge/scene/camera.cpp) in the
        // game's own view: the same steps on the game frustum's tangents, sphere scales and planes, worked out here
        // (no guest call, so no EE time) with VU0's registers kept as the drawn test left them. Only the outside bit
        // (kCullOutside) is used.
        uint32_t sphereInGameView(const Camera &camera, const host::GameFrustum &game, const Vec4 &sphereCentre, ee::Float radius)
        {
            const Vec4 centre = transformed(game.fromWorld, sphereCentre);
            const ee::Float depth = centre.z;
            const ee::Float front = depth + radius;
            if (front < camera.nearClip)
                return kSphereOutOfView;
            const ee::Float back = depth - radius;
            if (camera.farClip < back)
                return kSphereOutOfView;
            const ee::Float radiusAcross = game.sphereScaleX * radius;
            const ee::Float limitAcross = game.tanHalfWidth * depth + radiusAcross;
            if (limitAcross < ee::abs(centre.x))
                return kSphereOutOfView;
            const ee::Float radiusUp = game.sphereScaleY * radius;
            const ee::Float limitUp = game.tanHalfHeight * depth + radiusUp;
            if (limitUp < ee::abs(centre.y))
                return kSphereOutOfView;
            if (back < camera.nearClip)
                return kSphereCrossesNear | kSphereCrossesSide;
            if (camera.farClip < front)
                return kSphereCrossesFar;
            if (!(limitAcross - (radiusAcross + radius) < ee::abs(centre.x)) && !(limitUp - (radiusUp + radius) < ee::abs(centre.y)))
                return 0;
            return kSphereCrossesSide; // (the near-plane margin bit left out: only the outside bit is read)
        }
        uint32_t boxInGameView(const host::GameFrustum &game, const Box &box, const Matrix &world)
        {
            Vec4 corners[8];
            Vec4 corner;
            corner.w = 1.0f;
            for (uint32_t bits = 8, slot = 0; bits != 0; --bits, ++slot)
            {
                corner.x = (bits & 1u) ? box.low.x : box.high.x;
                corner.y = (bits & 2u) ? box.low.y : box.high.y;
                corner.z = (bits & 4u) ? box.low.z : box.high.z;
                corners[slot] = transformed(world, corner);
            }
            for (uint32_t plane = 0; plane < game.planeCount; ++plane)
            {
                uint32_t inside = 0;
                while (inside < 8 && !(ee::Float(0.0f) <= dot(corners[inside], game.planes[plane]) - game.planes[plane].w))
                    ++inside;
                if (inside == 8)
                    return kCullOutside;
            }
            return 0;
        }
        // ZGE4's nodeVisibleInView's outside bit in the game's view (drawListedNodeTree's: the bounds where the
        // node's origin moves them).
        uint32_t nodeInGameView(const Camera &camera, const host::GameFrustum &game, const Node &node)
        {
            vu0::KeptRegisters kept;
            const uint32_t flags = sphereInGameView(camera, game, node.sphereCentre, node.radius);
            if ((flags & kSphereOutOfView) || !(flags & kSphereCrossesSide))
                return flags & kCullOutside;
            Matrix world = node.world;
            world.translation() = world.translation() + node.origin;
            return boxInGameView(game, const_cast<Node &>(node).modelBounds(), world);
        }
        // sectorInView's outside bit in the game's view.
        uint32_t sectorInGameView(const Camera &camera, const host::GameFrustum &game, const Sector &sector)
        {
            vu0::KeptRegisters kept;
            // (the sphere's radius is the word after its centre: partInView's ViewSphere)
            return sphereInGameView(camera, game, sector.sphere, ee::Ptr<const Vec4>(&sector.sphere).cast<const ViewSphere>()->radius) &
                   kCullOutside;
        }

        // The stack a tree's walk keeps its nodes on, in the caller's frame: a bounded
        // list as the engine's macro lays it out.
        struct alignas(16) TreeStack
        {
            ee::Ptr<Node> nodes[20];
            ee::Ptr<ee::Ptr<Node>> base;  // +0x50
            ee::Ptr<ee::Ptr<Node>> limit; // +0x54
            ee::Ptr<ee::Ptr<Node>> top;   // +0x58 the next free entry
            uint32_t padding;
        };
        static_assert(sizeof(TreeStack) == 0x60 && offsetof(TreeStack, base) == 0x50 && offsetof(TreeStack, top) == 0x58);

        namespace // the game functions below call: ee/plain.h
        {
            // A bucket's add hook: (bucket, node, cull flags).
            void addToBucketHook(DrawBucket &bucket, ee::Ptr<Node> node, uint32_t cullFlags) { ee::call(bucketAddHook(bucket), ee::Ptr<DrawBucket>(&bucket), node, cullFlags); }
        }

        // A walk of a node's tree.
        struct TreeWalk
        {
            ee::Ptr<Node> root;
            uint32_t cullFlags; // the root's, kept for it
            uint32_t gameCullFlags; // the same in the game's view (GameCulling; the drawn's without one)
            bool noCulling;
            int32_t frameStamp;
        };

        // The tree under walk.root queued in the draw buckets, depth first (a node,
        // then its children, then its next sibling; the root's siblings too), each
        // visible node in its view's part: its sphere tested against the view (unless
        // the walk has noCulling, or it is the root: the root keeps walk.cullFlags),
        // its view centre and frame stamp recorded; a shadow caster's shadow part
        // queued (bucket 5).
        void queueTree(const TreeWalk &walk, ViewCulling &culling, GameCulling *game, const Camera &camera, ee::Ptr<TreeStack> stack,
                       BoxCorners &box)
        {
            const ee::Ptr<ee::Ptr<Node>> first = &stack->nodes[0];
            stack->base = first;
            stack->limit = first + 20;
            stack->nodes[0] = ee::Ptr<Node>();
            stack->nodes[1] = walk.root;
            stack->top = first + 1;
            const auto push = [&](ee::Ptr<Node> node) {
                *stack->top = node;
                stack->top = stack->top + 1;
            };

            uint32_t cullFlags = walk.cullFlags;
            uint32_t gameCullFlags = walk.gameCullFlags;
            ee::Ptr<Node> nodeAddress = walk.root;
            if (!nodeAddress)
                return;
            do
            {
                Node &node = *nodeAddress;
                if (node.nextSibling)
                    push(node.nextSibling);
                if (node.firstChild)
                    push(node.firstChild);
                NodeView &view = viewOf(node);
                if (!same(nodeAddress, walk.root))
                {
                    if (!walk.noCulling)
                    {
                        bool centreTaken = false;
                        cullFlags = cullNode(culling, node, view, camera, box, &centreTaken);
                        gameCullFlags = game ? game->cull(node, view, centreTaken) : cullFlags;
                    }
                    else
                        culling.viewCentreOf(node.sphereCentre, view.viewCentre);
                    setViewFrameStamp(view, static_cast<uint32_t>(walk.frameStamp));
                }

                // Queued when it has a part this view, is not culled and is shown.
                const ee::Ptr<ee::Ptr<Node>> popTop = stack->top;
                if (view.part && (cullFlags & kCullOutside) == 0 && (node.flags & kNodeShown) != 0)
                {
                    int32_t bucket = static_cast<int32_t>((node.drawFlags >> 8) & 0xFu) - 1;
                    if (bucket < 0)
                        bucket = nodeDrawBucket(nodeAddress);
                    // The shadow part: the view part's next, when skinned.
                    const ee::Ptr<ZgePart> shadowPart = view.part->next;
                    if (shadowPart && (shadowPart->flags & kShadowPartFlag) != 0 && queuePartDraw(nodeAddress, shadowPart) != 0)
                        bucket = kCasterBucket;
                    DrawBucket &target = bucketAt(static_cast<uint32_t>(bucket));
                    if (bucketAddHook(target) != 0)
                        addToBucketHook(target, nodeAddress, cullFlags);
                    else if (target.hasRoom())
                    {
                        // A plain bucket: a draw at its end while there is room.
                        BucketDraw &draw = *target.end;
                        draw.node = nodeAddress;
                        draw.part = 0;
                        draw.cullFlags = cullFlags;
                        draw.draw = 0;
                        target.end = target.end + 1;
                    }
                    if (!game)
                        view.lodAndFlags |= kViewDrawn;
                }
                // With the game's own view beside: drawn as the game sees it.
                if (game && view.part && (gameCullFlags & kCullOutside) == 0 && (node.flags & kNodeShown) != 0)
                    view.lodAndFlags |= kViewDrawn;
                stack->top = popTop - 1;
                nodeAddress = *stack->top;
            } while (nodeAddress);
        }

        // Whether a sector or node can be seen from the camera's area: with an area and
        // a region, its region is not marked out of view.
        bool regionInView(uint32_t area, uint32_t region)
        {
            return area == 0 || region == 0 || *ee::Ptr<uint32_t>(region + sizeof(uint32_t)) != *g_outOfViewMark;
        }

        // The first level of detail whose distance is past `depth`, -1 for none (at
        // least the first is looked at).
        int32_t levelForDepth(ee::Ptr<CollisionFrame> levels, ee::Float depth)
        {
            for (int32_t level = 0;;)
            {
                if (ee::Float(0.0f) < levels->parts[level]->distance - depth)
                    return level;
                ++level;
                if (level >= ((levels->flags >> 8) & 0xFu))
                    return -1;
            }
        }
    }

    // loadViewCulling (beginViewDraws): the camera's culling into VU0 for
    // drawNodeTree: its six world planes as columns (-w for the distance) and its
    // world -> view matrix. Always 1.
    uint32_t loadViewCulling(ee::Ptr<Camera> camera)
    {
        ee::Local<PlaneColumns> columns;
        const Plane *planes = camera->planes;
        for (int plane = 0; plane < 4; ++plane)
        {
            columns->lane[0][plane] = planes[plane].x;
            columns->lane[1][plane] = planes[plane].y;
            columns->lane[2][plane] = planes[plane].z;
            columns->lane[3][plane] = -planes[plane].w;
        }
        for (int plane = 0; plane < 2; ++plane)
        {
            columns->lane[4][plane] = planes[4 + plane].x;
            columns->lane[5][plane] = planes[4 + plane].y;
            columns->lane[6][plane] = planes[4 + plane].z;
            columns->lane[7][plane] = -planes[4 + plane].w;
        }
        for (int column = 4; column < 8; ++column)
        {
            columns->lane[column][2] = 0.0f;
            columns->lane[column][3] = 0.0f;
        }
        ViewCulling culling;
        culling.load(columns, camera->fromWorld);
        return 1;
    }
    READABLE_FUNCTION(loadViewCulling, Verified);

    namespace
    {
        // The root's flags in the game's view for the next drawNodeTree (ZGE4's listed node gives them; else its
        // flags are the drawn view's).
        std::optional<uint32_t> t_nextRootGameCullFlags;

        // drawNodeTree's walk, with the root's flags in the game's view.
        void queueNodeTreeAs(ee::Ptr<Node> root, uint32_t cullFlags, uint32_t gameCullFlags)
        {
            ee::Local<TreeStack> stack;
            ee::Local<BoxCorners> box;
            const ee::Ptr<Camera> camera(*g_currentCamera);
            TreeWalk walk{};
            walk.root = root;
            walk.cullFlags = cullFlags;
            walk.frameStamp = static_cast<int32_t>((*g_collisionWorld)->frameStamp);
            walk.noCulling = (root->flags & kNodeTreeIsOneUnit) != 0;
            walk.gameCullFlags = gameCullFlags;
            ViewCulling culling;
            std::optional<GameCulling> game;
            if (const host::GameFrustum *frustum = host::gameFrustum(camera))
                game.emplace(*camera, *frustum);
            queueTree(walk, culling, game ? &*game : nullptr, *camera, stack, box);
        }
    }

    // drawNodeTree (drawSky): the tree under `root` queued (queueTree). The
    // second argument is not read (the sky passes -1).
    void drawNodeTree(ee::Ptr<Node> root, uint32_t, uint32_t cullFlags)
    {
        const uint32_t gameCullFlags = t_nextRootGameCullFlags.value_or(cullFlags);
        t_nextRootGameCullFlags.reset();
        queueNodeTreeAs(root, cullFlags, gameCullFlags);
    }
    READABLE_FUNCTION_WAITS(drawNodeTree, Verified);

    namespace // the game functions below call: ee/plain.h
    {
        // The area record a position is in (markRegionsOutOfView: the visibility regions out of view from it marked).
        uint32_t areaAt(const Vec4 &position) { return ee::Address(markRegionsOutOfView(&position.xyz())); }
        // The same for the game's own camera when a hidden one stands elsewhere (zge/host/game_view.h): its position
        // put in the camera's for the call (the regions out of view from where the game's camera is), the drawn one's
        // back after.
        uint32_t areaAtGameCamera(ee::Ptr<Camera> camera, const Vec4 &drawnPosition)
        {
            if (!gameCameraElsewhere(camera))
                return areaAt(drawnPosition);
            const Vec4 drawn = camera->position;
            camera->position = gameCameraPosition(camera);
            const uint32_t area = areaAt(camera->position);
            camera->position = drawn;
            return area;
        }
        // A node's drawCheck: (node, &level of detail, its data), 0 leaves the tree undrawn.
        int32_t askDrawCheck(uint32_t check, ee::Ptr<Node> node, ee::Address level, uint32_t data) { return ee::call<int32_t>(check, node, level, data); }

        // drawScene's frame. Its layout and every store stay as the original's:
        // drawSparks reads a leftover of this stack (see there), so a field moved or a
        // write dropped here changes a streak's length on screen.
        struct SceneFrame
        {
            int32_t chosenLevel;           // +0x000 the level of detail, handed to a drawCheck
            uint8_t padding[0x0C];
            BoxCorners listedBox;          // +0x010 a listed node's
            TreeStack stack;               // +0x0A0
            BoxCorners treeBox;            // +0x100 a node of a tree's
            uint32_t area;                 // +0x190 the area the camera is in
            uint32_t frameStamp;           // +0x194
            ee::Ptr<TreeStack> stackPlace; // +0x198
            ee::Ptr<Sector> nextSector;    // +0x19C
            int32_t sectorsLeft;           // +0x1A0
        };
        static_assert(offsetof(SceneFrame, stack) == 0xA0 && offsetof(SceneFrame, treeBox) == 0x100 &&
                      offsetof(SceneFrame, area) == 0x190 && offsetof(SceneFrame, sectorsLeft) == 0x1A0);

        // A listed node of a sector drawn, unless this view has seen it this frame: culled,
        // given the level of detail for its depth, and its tree queued.
        void drawListedNode(ee::Ptr<SceneFrame> frame, ViewCulling &culling, GameCulling *game, const Camera &camera,
                            ee::Ptr<Node> nodeAddress)
        {
            Node &node = *nodeAddress;
            NodeView &view = viewOf(node);
            if (viewFrameStamp(view) == frame->frameStamp)
                return;
            setViewFrameStamp(view, frame->frameStamp);
            view.lodAndFlags &= ~1u;
            const int32_t currentLevel = (view.lodAndFlags >> 1) & 0xFu;

            uint32_t cullFlags;
            uint32_t gameCullFlags; // (the game's view beside: in it or not)
            const auto cull = [&] {
                bool centreTaken = false;
                cullFlags = cullNode(culling, node, view, camera, frame->listedBox, &centreTaken);
                gameCullFlags = game ? game->cull(node, view, centreTaken) : cullFlags;
            };
            const bool oneUnit = (node.flags & kNodeTreeIsOneUnit) != 0;
            if (oneUnit)
            {
                // The tree as one: culled as a whole, the level of detail set only when
                // in view. (Hidden, it is not even tested.)
                if ((node.flags & kNodeShown) == 0 || !regionInView(frame->area, node.region))
                    return;
                cull();
                if (cullFlags & gameCullFlags & kCullOutside)
                    return;
            }
            else
            {
                if (!regionInView(frame->area, node.region))
                    return;
                cull();
                if ((node.flags & kNodeShown) == 0)
                    cullFlags = gameCullFlags = kCullOutside;
            }
            frame->chosenLevel = levelForDepth(node.lods, view.viewCentre.z);
            // (past its last level of detail in the game's view: not drawn there)
            const int32_t gameLevel = game ? levelForDepth(node.lods, game->depth()) : frame->chosenLevel;
            if (gameLevel < 0)
                gameCullFlags = kCullOutside;
            if (frame->chosenLevel < 0)
            {
                if (oneUnit && gameLevel < 0)
                    return;
                cullFlags = kCullOutside;
            }
            else if (frame->chosenLevel != currentLevel)
                setTreeLevelOfDetail(nodeAddress, static_cast<uint8_t>(frame->chosenLevel));

            if (node.drawCheck != 0 && askDrawCheck(node.drawCheck, nodeAddress, &frame->chosenLevel, node.drawCheckData) == 0)
                return;
            TreeWalk walk{};
            walk.root = nodeAddress;
            walk.frameStamp = static_cast<int32_t>((*g_collisionWorld)->frameStamp);
            walk.noCulling = oneUnit;
            walk.cullFlags = cullFlags;
            walk.gameCullFlags = gameCullFlags;
            queueTree(walk, culling, game, camera, frame->stackPlace, frame->treeBox);
        }

        // The list's next node, 0 at its end (the list's own cursor).
        ee::Ptr<Node> nextListedNode(NodeList &nodes)
        {
            const int32_t cursor = nodes.cursor;
            if (cursor >= 0)
            {
                const int32_t next = cursor + 1;
                nodes.cursor = next;
                if (next < static_cast<int32_t>(nodes.count))
                    return nodes.items[next];
            }
            nodes.cursor = -1;
            return ee::Ptr<Node>();
        }
    }

    // drawScene (the view's draw): the frame's stamp, the area the camera
    // is in, the sky, the moved nodes brought up to date (the first frame:
    // every mesh of the world), then every sector of the world in view: each
    // of its nodes not yet seen this frame and this view culled, given the
    // level of detail for its depth, and its tree queued; last the queued
    // nodes drawn. Always 1.
    uint32_t drawScene()
    {
        ee::Local<SceneFrame> frame;
        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        if (!world)
            return 1;
        const ee::Ptr<Camera> camera(*g_currentCamera);

        frame->frameStamp = world->frameStamp + 1;
        world->frameStamp = frame->frameStamp;
        resetDrawBuckets();
        frame->area = areaAtGameCamera(camera, camera->position);
        flushTexturesOnAreaExit(frame->area);
        setCameraArea(ee::Ptr<Camera>(*g_currentCamera), frame->area);
        if (*g_sceneDrawOff != 0)
            return 1;
        drawSky();
        updateMovedNodes();
        if (frame->frameStamp == 1)
            forEachWorldMesh(sym::updateMovedNodeVisitor, 0);

        ViewCulling culling;
        std::optional<GameCulling> game;
        if (const host::GameFrustum *frustum = host::gameFrustum(camera))
            game.emplace(*camera, *frustum);
        ee::Ptr<Sector> sector = world->cell;
        frame->stackPlace = &frame->stack;
        int32_t left = world->cells;
        do
        {
            NodeList &list = *sector->nodes;
            ee::Ptr<Node> node;
            if (list.count != 0)
            {
                list.cursor = 0;
                node = list.items[0];
            }
            else
                list.cursor = -1;
            frame->sectorsLeft = left - 1;
            if (node && !regionInView(frame->area, sector->region))
                sector = sector + 1;
            else
            {
                frame->nextSector = sector + 1;
                if (node)
                {
                    culling.takeWorldSphere(sector->sphere);
                    // (in the drawn view, or with the game's beside in that)
                    if (!culling.behindAPlane(false) || (game && !game->sectorOutside(sector->sphere)))
                        for (; node; node = nextListedNode(*sector->nodes))
                            drawListedNode(frame, culling, game ? &*game : nullptr, *camera, node);
                }
                sector = frame->nextSector;
            }
            left = frame->sectorsLeft;
        } while (left != 0);
        drawQueuedNodes();
        return 1;
    }

    namespace
    {
        inline ee::Global<uint32_t, sym::dm2::g_sceneProfileTimer> g_sceneProfileTimer; // ZGE2's "SCENE" profile bar
    }

    // drawSceneBySector: ZGE2's drawScene, timed by its "SCENE" profile bar (made on the first draw): the world's
    // frame stamp up and the draw buckets reset, the area the camera is in (from the camera's cached matrix's
    // translation) kept in the world, the moved nodes brought up to date, the sky, then every sector of the world
    // through forEachWorldSector with drawSectorNodes, last the queued nodes drawn. Always 1. ZGE3 keeps the area in
    // the draw's frame, walks the sectors and their nodes inline, and has g_sceneDrawOff and the first frame's
    // forEachWorldMesh, which ZGE2 has not.
    uint32_t drawSceneBySector()
    {
        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        if (!world)
            return 1;
        if (*g_sceneProfileTimer == 0)
            *g_sceneProfileTimer = newProfileBar(0, 0, sym::dm2::sceneTimerColour, sym::dm2::sceneTimerName);
        startProfileBar(*g_sceneProfileTimer);
        world->frameStamp += 1;
        resetDrawBuckets();
        keepCameraAreaInWorld(*world, areaAt(cachedMatrix(ee::Ptr<Camera>(*g_currentCamera), 1)->translation()));
        updateMovedNodes();
        drawSky();
        forEachWorldSector(SectorVisit(sym::dm2::drawSectorNodes), 0);
        drawQueuedNodes();
        stopProfileBar(*g_sceneProfileTimer);
        return 1;
    }
    namespace
    {
        // ZGE4 keeps a view's level of detail in bits 28-31 of NodeView::lodAndFlags (ZGE3: bits 1-4).
        constexpr uint32_t kZge4ViewLevelShift = 28;
        int32_t zge4ViewLevel(const NodeView &view) { return static_cast<int32_t>((view.lodAndFlags >> kZge4ViewLevelShift) & 0xFu); }

        // ZGE4's draw of a sector's listed node, unless this view has seen it this frame: tested against the view by
        // updatedNodeVisibleInView, its position put in the camera's view space (the view's centre), given the level of
        // detail for that depth, asked its draw check, and its tree drawn by drawNodeTree with its cull flags. A tree
        // that is one unit is drawn only when shown, in view and at a level of detail; another tree is drawn with the
        // flags "outside" when it is hidden or past its last level.
        void drawListedNodeTree(ee::Ptr<Node> nodeAddress, uint32_t frameStamp, uint32_t area, const Matrix &fromWorld,
                                ee::Local<int32_t> &level, ee::Local<ee::Float> &distancePast)
        {
            // The game's own view beside the drawn one (zge/host/game_view.h): the node in it or not.
            const ee::Ptr<Camera> camera(*g_currentCamera);
            const host::GameFrustum *game = host::gameFrustum(camera);
            const auto inGame = [&](uint32_t drawnFlags) {
                return game ? nodeInGameView(*camera, *game, *nodeAddress) : drawnFlags;
            };
            Node &node = *nodeAddress;
            NodeView &view = viewOf(node);
            if (viewFrameStamp(view) == frameStamp)
                return;
            setViewFrameStamp(view, frameStamp);
            view.lodAndFlags &= ~kViewDrawn;

            uint32_t cullFlags, gameCullFlags;
            if ((node.flags & kNodeTreeIsOneUnit) != 0)
            {
                if ((node.flags & kNodeShown) == 0 || !regionInView(area, node.region))
                    return;
                cullFlags = updatedNodeVisibleInView(nodeAddress);
                gameCullFlags = inGame(cullFlags);
                if (cullFlags & gameCullFlags & kCullOutside)
                    return;
                view.viewCentre = transformed(fromWorld, node.world.translation());
                *level = frameLevelForDistance(node.lods, &view.viewCentre.xyz(), distancePast);
                if (*level < 0)
                    return;
                if (*level != zge4ViewLevel(view))
                    setTreeLevelOfDetail(nodeAddress, static_cast<uint8_t>(*level));
            }
            else
            {
                if (!regionInView(area, node.region))
                    return;
                cullFlags = updatedNodeVisibleInView(nodeAddress);
                gameCullFlags = inGame(cullFlags);
                if ((node.flags & kNodeShown) == 0)
                    cullFlags = gameCullFlags = kCullOutside;
                view.viewCentre = transformed(fromWorld, node.world.translation());
                *level = frameLevelForDistance(node.lods, &view.viewCentre.xyz(), distancePast);
                if (*level < 0)
                    cullFlags = gameCullFlags = kCullOutside;
                else if (*level != zge4ViewLevel(view))
                    setTreeLevelOfDetail(nodeAddress, static_cast<uint8_t>(*level));
            }
            if (node.drawCheck != 0 && askDrawCheck(node.drawCheck, nodeAddress, level, node.drawCheckData) == 0)
                return;
            t_nextRootGameCullFlags = gameCullFlags;
            drawNodeTree(nodeAddress, ~0u, cullFlags); // (its second argument is not read: -1, as the sky's)
        }
    }

    // drawSceneByNodeTrees: ZGE4's drawScene, timed by its "SCENE" profile bar (made on the first draw): the world's
    // frame stamp up (the camera's world -> view matrix taken for the whole draw), the draw buckets reset, the area the
    // camera is in, the sky and the moved nodes as ZGE3's; then every sector of the world whose region is in view and
    // that sectorInView finds in the view: each of its listed nodes drawn by drawListedNodeTree; last the queued nodes
    // drawn and the bar stopped. Always 1. ZGE3 culls the sectors and nodes against VU0's planes and walks the trees
    // inline, returns at once under g_sceneDrawOff (ZGE4 still draws the queue), and brings every world mesh up to date
    // on the first frame, which ZGE4 does not here.
    uint32_t drawSceneByNodeTrees()
    {
        constexpr ee::Global<uint32_t, sym::bmx::g_sceneProfileTimer> sceneProfileTimer;
        constexpr uint32_t kSceneBarDisplay = 4u; // newProfileBar's display flags' mask (ZGE2's scene bar has 0)
        ee::Local<int32_t> level;          // the listed node's level of detail, handed to its draw check
        ee::Local<ee::Float> distancePast; // frameLevelForDistance's

        const ee::Ptr<CollisionWorld> world = *g_collisionWorld;
        if (!world)
            return 1;
        const uint32_t frameStamp = world->frameStamp + 1;
        world->frameStamp = frameStamp;
        const ee::Ptr<Matrix> fromWorld = cachedMatrix(ee::Ptr<Camera>(*g_currentCamera), 0);
        if (*sceneProfileTimer == 0)
            *sceneProfileTimer = newProfileBar(kSceneBarDisplay, 0, sym::bmx::sceneTimerColour, sym::bmx::sceneTimerName);
        startProfileBar(*sceneProfileTimer);
        resetDrawBuckets();
        const ee::Ptr<Camera> currentCamera(*g_currentCamera);
        const uint32_t area = areaAtGameCamera(currentCamera, cachedMatrix(currentCamera, 1)->translation());
        flushTexturesOnAreaExit(area);
        setCameraArea(ee::Ptr<Camera>(*g_currentCamera), area);
        if (*g_sceneDrawOff == 0)
        {
            drawSky();
            updateMovedNodes();
            ee::Ptr<Sector> sector = world->cell;
            uint32_t left = static_cast<uint32_t>(world->cellCount[0]) * static_cast<uint32_t>(world->cellCount[1]) *
                            static_cast<uint32_t>(world->cellCount[2]);
            do
            {
                NodeList &list = *sector->nodes;
                ee::Ptr<Node> node;
                if (list.count != 0)
                {
                    list.cursor = 0;
                    node = list.items[0];
                }
                else
                    list.cursor = -1;
                // (in the drawn view, or with the game's beside in that)
                const auto inView = [&] {
                    if ((sectorInView(sector) & kCullOutside) == 0)
                        return true;
                    const ee::Ptr<Camera> camera(*g_currentCamera);
                    const host::GameFrustum *game = host::gameFrustum(camera);
                    return game && sectorInGameView(*camera, *game, *sector) == 0;
                };
                if (node && regionInView(area, sector->region) && inView())
                    for (; node; node = nextListedNode(*sector->nodes))
                        drawListedNodeTree(node, frameStamp, area, *fromWorld, level, distancePast);
                sector = sector + 1;
            } while (--left != 0);
        }
        drawQueuedNodes();
        stopProfileBar(*sceneProfileTimer);
        return 1;
    }
    READABLE_FUNCTION_WAITS_OWN(drawScene, Verified, dm2, drawSceneBySector, bmx, drawSceneByNodeTrees);
}
