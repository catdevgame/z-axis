// BMX XXX's spawn locators (spawn_points.h). BMX XXX's own code (bmx.c).

#include "spawn_points.h"

#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/math/vec.h"
#include "zge/scene/locators.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/bmx/game/spawn_points.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The scene points' kind a spawn locator has: the hash of "Spawn".
        constexpr uint32_t kSpawnKindHash = zge::nameHashOf("Spawn");
        // Farther than any locator.
        inline ee::Global<ee::Float, sym::g_locatorFar> g_locatorFar;

        zge::ScenePoint &locatorPoint(const zge::Locator &locator)
        {
            return *ee::Ptr<zge::ScenePoint>(locator.point);
        }
    }

    // locatorMatrix: every Spawn locator of the name is looked at; a nearer
    // one's rotation and position are kept, and the nearest's go out.
    int32_t locatorMatrix(const ee::Float *point, uint32_t name, zge::Matrix *matrix)
    {
        ee::Local<zge::LocatorList> list;
        ee::Local<zge::Vec4> apart;
        ee::Local<zge::Matrix> nearestMatrix;
        int32_t found = 0;
        zge::findLocators(list.address(), kSpawnKindHash, name);
        ee::Float nearest = *g_locatorFar;
        for (ee::Ptr<zge::Locator> entry = list->head; entry; entry = entry->next)
        {
            zge::ScenePoint &locator = locatorPoint(*entry);
            *apart = *rider::vec4(point) - locator.position;
            const ee::Float distance = zge::squaredLength(*apart);
            if (distance < nearest)
            {
                zge::quaternionToMatrix(&*nearestMatrix, &locatorPoint(*entry).orientation);
                nearestMatrix->translation() = locatorPoint(*entry).position;
                nearest = distance;
            }
        }
        if (nearest == *g_locatorFar)
        {
            zge::matrixIdentity(matrix);
        }
        else
        {
            *matrix = *nearestMatrix;
            found = 1;
        }
        zge::freeLocators(list.address());
        return found;
    }
    READABLE_FUNCTION(locatorMatrix, Verified);

    // addSpawnPoint: a Spawn point of the name, with no tags, at the
    // matrix's position and rotation, added to the level's points.
    void addSpawnPoint(const zge::Matrix *matrix, ee::Ptr<const char> name)
    {
        ee::Local<zge::ScenePoint> point;
        point->kindHash = kSpawnKindHash;
        point->nameHash = zge::nameHash(name.get());
        point->tagBytes = 0;
        point->tags = ee::Ptr<zge::TagRecord>();
        point->position = matrix->translation();
        zge::matrixToQuaternion(&point->orientation, matrix);
        zge::addScenePoint(point);
    }
    READABLE_FUNCTION(addSpawnPoint, Draft);

    // setSpawnPoint: the first Spawn locator of the name takes the matrix's
    // position and rotation; a level with none gets a new one.
    void setSpawnPoint(const zge::Matrix *matrix, ee::Ptr<const char> name)
    {
        ee::Local<zge::LocatorList> list;
        zge::findLocators(list.address(), kSpawnKindHash, zge::nameHash(name.get()));
        if (list->head)
        {
            locatorPoint(*list->head).position = matrix->translation();
            zge::matrixToQuaternion(&locatorPoint(*list->head).orientation, matrix);
        }
        else
        {
            addSpawnPoint(matrix, name);
        }
        zge::freeLocators(list.address());
    }
    READABLE_FUNCTION(setSpawnPoint, Verified);
}
