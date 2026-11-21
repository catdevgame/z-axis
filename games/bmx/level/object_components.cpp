// A scene object's component word, where a level's dynamics object keeps its
// record (object_components.h): BMX XXX's own code (its game block), word for word
// Aggressive Inline's (games/aggressive/level/object_components.cpp).

#include "object_components.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/bmx/level/object_components.cpp.h"

namespace game::bmx
{
    using namespace zge;

    // objectComponentKind, setObjectComponentWord: the first word of the
    // scene object's component (a level's dynamics object keeps its record
    // there). BMX XXX's setTagWord gives no answer (nothing reads one), so
    // neither does the setter.
    int32_t objectComponentKind(ee::Ptr<SceneObject> sceneObject)
    {
        const ee::Ptr<TagRecord> component = objectComponent(sceneObject);
        return tagWord(component, 8, 0);
    }
    READABLE_FUNCTION(objectComponentKind, Verified);

    void setObjectComponentWord(ee::Ptr<SceneObject> sceneObject, ee::Address value)
    {
        const ee::Ptr<TagRecord> component = objectComponent(sceneObject);
        game::tail(setTagWord, component, 8, 0, value);
    }
    READABLE_FUNCTION(setObjectComponentWord, Verified);
}
