// A scene object's component word, where a level's dynamics object keeps its
// record (object_components.h): DM2's own code (its game block), word for word
// Aggressive Inline's (games/aggressive/level/object_components.cpp).

#include "object_components.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/dm2/level/object_components.cpp.h"

namespace game::dm2
{
    using namespace zge;

    // objectComponentKind, setObjectComponentWord: the first word of the
    // scene object's component (a level's dynamics object keeps its record
    // there).
    int32_t objectComponentKind(ee::Ptr<SceneObject> sceneObject)
    {
        const ee::Ptr<TagRecord> component = objectComponent(sceneObject);
        return tagWord(component, 8, 0);
    }
    READABLE_FUNCTION(objectComponentKind, Verified);

    int32_t setObjectComponentWord(ee::Ptr<SceneObject> sceneObject, ee::Address value)
    {
        const ee::Ptr<TagRecord> component = objectComponent(sceneObject);
        return game::tail(setTagWord, component, 8, 0, value);
    }
    READABLE_FUNCTION(setObjectComponentWord, Verified);
}
