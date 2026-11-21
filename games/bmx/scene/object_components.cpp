// BMX XXX's objectComponent (ZGE4's zobj.c; ZGE3's is zge/scene/objects.cpp): the answer for the object last asked
// about kept (a cache of one, which spawnLevelObjects and removeLevelObjects empty), and the look-up itself with the
// scene's kind and the level object's node read inline, a particle emitter's own tags and an object's own table.

#include "ee/abi.h"
#include "ee/address_compare.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/level_objects.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/scene/object_components.cpp.h"

namespace game::bmx
{
    namespace
    {
        // ZGE4's particle emitter objects (object kind 4).
        constexpr uint64_t kObjectParticleEmitterKind = 4;
        inline ee::Global<ee::Ptr<zge::SceneObject>, sym::g_componentCacheObject> g_componentCacheObject;
        inline ee::Global<ee::Ptr<zge::TagRecord>, sym::g_componentCacheRecord> g_componentCacheRecord;
    }

    // findObjectComponent (cachedObjectComponent's look-up): the first record of the tag table the object has: its
    // level object's node's for an object with a scene (0 for no node), the emitter's tags themselves for a
    // particle emitter object, else the table an object with no scene keeps; 0 for no table.
    ee::Ptr<zge::TagRecord> findObjectComponent(ee::Ptr<zge::SceneObject> object)
    {
        const ee::Ptr<zge::LevelObject> record(zge::sceneObjectOwner(object));
        const ee::Ptr<zge::Scene> scene = zge::sceneOf(object);
        ee::Ptr<zge::TagTableHeader> tags;
        if (scene && static_cast<uint8_t>(scene->flags) != 0)
        {
            const ee::Ptr<zge::Node> node = record->node;
            if (!node)
                return {};
            tags = ee::Ptr<zge::TagTableHeader>(node->tags);
        }
        else if ((object->flags & zge::kObjectKindMask) == kObjectParticleEmitterKind)
            return zge::emitterTagsOf(object)->tags;
        else
            tags = object->tags;
        if (!tags)
            return {};
        return zge::tagRecordAt(tags, 0u);
    }
    READABLE_FUNCTION(findObjectComponent, Verified);

    // cachedObjectComponent: BMX XXX's objectComponent: findObjectComponent's answer, kept for the object last
    // asked about.
    ee::Ptr<zge::TagRecord> cachedObjectComponent(ee::Ptr<zge::SceneObject> object)
    {
        if (!ee::same(*g_componentCacheObject, object))
        {
            *g_componentCacheObject = object;
            *g_componentCacheRecord = findObjectComponent(object);
        }
        return *g_componentCacheRecord;
    }
    READABLE_FUNCTION(cachedObjectComponent, Verified);
}
