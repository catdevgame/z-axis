#pragma once

// The park editor's tower pieces (class "EditTowerObject", edit_tower.cpp).

#include "zge/editor/edit_records.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    // (edit_tower.cpp)
    void freeTowerCollision(ee::Ptr<EditObjectPart> part);
    void setTowerBottom(ee::Ptr<EditTowerObjectPart> tower, ee::Float height);
    void setTowerState(ee::Ptr<EditTowerObjectPart> tower, int32_t corner);
    void setTowerTop(ee::Ptr<EditTowerObjectPart> tower, ee::Float height);
}
