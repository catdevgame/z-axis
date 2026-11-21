#pragma once

// A scene object's component word, where a level's dynamics object keeps its
// record (object_components.cpp).

#include "ee/ee.h"
#include "ee/plain.h"
#include "zge/scene/scene.h"

#include <cstdint>

namespace game::bmx
{
    // The first word of the scene object's component.
    int32_t objectComponentKind(ee::Ptr<zge::SceneObject> sceneObject);
    // The same word set.
    void setObjectComponentWord(ee::Ptr<zge::SceneObject> sceneObject, ee::Address value);
}
