// The rider's skinned parts for ghost riding (rider_ghost.h).

#include "zge/host/rider_ghost.h"

#include "zge/render/part.h"
#include "zge/render/shadow.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"
#include "zge/scene/skeleton.h"

#include <cstdint>
#include <utility>

namespace zge::host
{
    namespace
    {
        // The deepest a rider's model hangs below its node.
        constexpr int kModelDepth = 16;

        // The node is a skinned model's of one of the rider's objects (its scene's node), or under one.
        bool isRiders(ee::Ptr<Node> node)
        {
            size_t count = 0;
            const uint32_t *objects = zx::ghost_rider::riderObjects(count);
            if (count == 0u)
                return false;
            ee::Ptr<Node> models[zx::ghost_rider::kMaxRiderObjects];
            for (size_t i = 0; i < count; ++i)
            {
                const ee::Ptr<Scene> scene = ee::Ptr<SceneObject>(objects[i])->scene;
                models[i] = scene ? scene->node : ee::Ptr<Node>();
            }
            for (int depth = 0; node && depth < kModelDepth; ++depth, node = node->parent)
                for (size_t i = 0; i < count; ++i)
                    if (models[i] && node == models[i])
                        return true;
            return false;
        }

        // The part's pose as the skinned encoder sends it (zge/render/ps2/class_draws.cpp encodeSkinnedDraw): per
        // bone of the part's list, the pose of the node's that drives it, its translation less the bind position
        // turned by the pose. Read without the encoder's walk (which moves the list's cursor).
        zx::ghost_rider::PartPose partPose(const Node &node, const ZgePart &part)
        {
            zx::ghost_rider::PartPose pose;
            pose.vertices = part.vertexCount;
            for (int r = 0; r < 4; ++r)
            {
                for (int c = 0; c < 3; ++c)
                    pose.world[r][c] = node.world.m[r][c].raw();
                pose.world[r][3] = r == 3 ? 1.0f : 0.0f;
            }
            const BoneList &bones = *ee::Ptr<BoneList>(part.bones);
            const ee::Ptr<BonePose> poses = node.bonePoses.cast<BonePose>();
            pose.bones = bones.count;
            pose.palette.reserve(size_t{bones.count} * 12u);
            for (uint32_t k = 0; k < bones.count; ++k)
            {
                const Bone &bone = *bones.items[k];
                const Matrix &m = poses[bone.poseIndex - 1u].matrix;
                const float offset[3] = {bone.offset.x.raw(), bone.offset.y.raw(), bone.offset.z.raw()};
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 3; ++c)
                        pose.palette.push_back(m.m[r][c].raw());
                for (int c = 0; c < 3; ++c)
                    pose.palette.push_back(m.m[3][c].raw() -
                                           (offset[0] * m.m[0][c].raw() + offset[1] * m.m[1][c].raw() + offset[2] * m.m[2][c].raw()));
            }
            return pose;
        }
    }

    std::shared_ptr<const zx::ghost_rider::Ghost> noteRiderDraw(const SkinnedDraw &draw)
    {
        if (!zx::ghost_rider::active())
            return nullptr;
        const DrawRecord &record = draw.record;
        if (!record.node || !record.part || !isRiders(record.node) || !record.part->bones)
            return nullptr;
        return zx::ghost_rider::noteRiderPart(record.node.address, record.part.address, partPose(*record.node, *record.part),
                                              draw.lights.turnedByWorld);
    }
}
