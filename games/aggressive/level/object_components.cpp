// A scene object's component word, where a level's dynamics object keeps its
// record (dynamics.h): moved from zge/scene/objects.cpp, where they sat as
// engine code; the blocks say they are AI's own (layer2).

#include "aggressive/level/dynamics.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/assets/tag_table.h"
#include "zge/scene/scene.h"

#include <cstdint>
#include "sym/aggressive/level/object_components.cpp.h"

namespace aggressive::level
{
    using namespace zge;

    // objectComponentKind, setObjectComponentWord: the first word of the
    // scene object's component (a level's dynamics object keeps its record
    // there). DM2 (PS2) has both word for word.
    int32_t objectComponentKind(uint32_t sceneObject)
    {
        const ee::Ptr<TagRecord> component = objectComponent(ee::Ptr<SceneObject>(sceneObject));
        return tagWord(component, 8, 0);
    }
    READABLE_FUNCTION(objectComponentKind, Verified);

    int32_t setObjectComponentWord(uint32_t sceneObject, ee::Address value)
    {
        const ee::Ptr<TagRecord> component = objectComponent(ee::Ptr<SceneObject>(sceneObject));
        return game::tail(setTagWord, component, 8, 0, value);
    }
    READABLE_FUNCTION(setObjectComponentWord, Verified);
}
