#pragma once

// What the park editor shows of a piece, and its saved views (park_display.cpp).

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "zge/collision/collision.h"
#include "zge/math/matrix.h"
#include "zge/render/part.h"
#include "zge/scene/node.h"
#include "zge/text/strings.h"
#include "zge/assets/geo.h"
#include "zge/scene/scene.h"
#include <cstddef>
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/box.h"
#include "zge/math/types.h"
#include "zge/editor/edit_records.h"

#include <cstdint>

namespace zge::editor
{
    uint32_t editDisplayNode(uint32_t display);
    int32_t freeEditDisplay(uint32_t display);
    void freeEditView(uint32_t view);
    int32_t freeShownDisplay(uint32_t display);
    void hideTowerDisplay();
    void loadEditView(uint32_t view, uint32_t catalog);
    uint32_t newEditDisplay(uint32_t hash, uint32_t copyFlags);
    int32_t newEditView();
    void saveEditView(uint32_t view, uint32_t catalog);
    int32_t setDisplayMatrix(uint32_t display, ee::Ptr<zge::Matrix> matrix);
    void showEditDisplay(uint32_t display, uint32_t shown);
    int32_t stretchTowerDisplay(ee::Ptr<EditDisplay> display, ee::Ptr<zge::Matrix> pieceMatrix, uint32_t height);

    struct EditView
    {
        zge::Vec4 position;    // +0x00
        zge::EulerAngles angles; // +0x10
        uint8_t _16[2];
        uint32_t set;          // +0x18
        uint8_t _1c[4];
    };
    static_assert(sizeof(EditView) == 0x20 && offsetof(EditView, angles) == 0x10 && offsetof(EditView, set) == 0x18);
}
