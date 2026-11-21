// The box a model's bones span: asked for by what collides with an NPC or a
// traffic car (collideNpcWithRigidBody). The two functions are one code at
// two addresses, for two owners of the scene object.

#include "aggressive/level/object_box.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/quaternion.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/platform/platform.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/level/object_box.cpp.h"

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kMemset = sym::memset;              // (dst, byte, bytes) (libc)

        namespace
        {
            void memset(ee::Ptr<zge::Vec4> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
        }

        // The box around the bones of the model of the scene object in
        // `sceneObject` (a field of `owner`): 0 if it has no scene, or a bone
        // the scene does not know (that leaves the box as memoryFill left it).
        int32_t boneBox(ee::Address owner, ee::Ptr<ee::Ptr<zge::SceneObject>> sceneObject, ee::Ptr<zge::OrientedBox> box)
        {
            ee::Local<zge::Vec4> low;
            ee::Local<zge::Vec4> high;
            ee::Local<zge::Vec4> half;
            ee::Local<zge::Vec4> origin;
            ee::Local<zge::Vec4> point;
            // The original keeps the owner, and later where the high corner is, in
            // its frame (it runs out of registers); they stay there as it leaves them.
            ee::Local<uint32_t> ownerKept;
            ee::Local<ee::Ptr<zge::Vec4>> highKept;
            *ownerKept = owner;
            memset(origin, 0, sizeof(zge::Vec4));
            zge::memoryFill(box, 0, sizeof(zge::OrientedBox));
            const ee::Ptr<zge::Scene> scene = zge::sceneOf(*sceneObject);
            if (!scene)
                return 0;
            const int32_t count = zge::sceneBoneCount(scene);
            const uint32_t bytes = static_cast<uint32_t>(count) * static_cast<uint32_t>(sizeof(uint32_t));
            const ee::Ptr<uint32_t> ids(zge::allocateMemory(bytes));
            zge::memoryFill(ids, 0, bytes);
            zge::sceneBoneIds(scene, ids);
            if (count > 0)
            {
                *highKept = high;
                for (int32_t i = 0; i < count; ++i)
                {
                    const ee::Ptr<zge::SceneBone> bone = zge::findSceneModelBone(scene, ids[i]);
                    if (!bone)
                    {
                        zge::freeMemory(ids);
                        return 0;
                    }
                    zge::bonePoint(scene, bone, origin, point, 0);
                    if (i == 0)
                    {
                        *low = *point;
                        *high = *point;
                    }
                    else
                    {
                        const zge::Vec4 &at = *point;
                        low->x = ee::min(at.x, low->x);
                        high->y = ee::max(at.y, high->y);
                        high->z = ee::max(at.z, high->z);
                        low->y = ee::min(at.y, low->y);
                        high->x = ee::max(at.x, high->x);
                        low->z = ee::min(at.z, low->z);
                    }
                }
            }
            zge::freeMemory(ids);
            *half = *high - *low;
            *half = *half * ee::Float(0.5f);
            box->halfExtents = *half;
            box->centre = *low + *half;
            zge::quaternionCopy(&box->orientation, zge::objectOrientation(*sceneObject));
            return 1;
        }
    }

    // npcBox: the box of an NPC's model.
    int32_t npcBox(ee::Ptr<Npc> npc, ee::Ptr<zge::OrientedBox> box)
    {
        return boneBox(npc, &npc->object, box);
    }
    READABLE_FUNCTION_WAITS(npcBox, Verified);

    // trafficCarBox: the box of a traffic car's model (the scene object is
    // its first word).
    int32_t trafficCarBox(ee::Ptr<TrafficCar> car, ee::Ptr<zge::OrientedBox> box)
    {
        return boneBox(car, &car->object, box);
    }
    READABLE_FUNCTION_WAITS(trafficCarBox, Verified);
}
