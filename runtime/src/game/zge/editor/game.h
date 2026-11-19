#pragma once

// What the shared park editor calls, or hands on as a button's handler, in each
// game: each game defines these in zge::editor in its own folder
// (aggressive/level, bmx/editor), its own version of the function (BMX XXX's
// tool buttons play sounds, for one).

#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/editor/edit_records.h"
#include "zge/math/types.h"

#include <cstdint>

namespace zge::editor
{
    uint32_t cellAcceptsPiece(uint32_t address, ee::Ptr<const CellIndex> cell, uint32_t recordAddress);
    void clearParkPieces(uint32_t park);
    void collectSelectedPiece(uint32_t address, uint32_t recordAddress);
    void constructEditTowerObject(uint32_t object);
    void destructEditObject(uint32_t object);
    void editRefused();
    void freeEditUi(uint32_t address);
    int32_t loadAnchor(uint32_t anchor, uint32_t stream);
    uint32_t newWorldObjectInstance(uint32_t hash, uint32_t copyFlags);
    uint32_t objectToolDelete(ee::Ptr<EditUiButton> button);
    int32_t objectToolMoveBack(ee::Ptr<EditUiButton> button);
    int32_t objectToolMoveForward(ee::Ptr<EditUiButton> button);
    int32_t objectToolMoveLeft(ee::Ptr<EditUiButton> button);
    int32_t objectToolMoveRight(ee::Ptr<EditUiButton> button);
    uint32_t objectToolNextPiece(ee::Ptr<EditUiButton> button);
    uint32_t objectToolPlace(ee::Ptr<EditUiButton> button);
    uint32_t objectToolPreviousPiece(ee::Ptr<EditUiButton> button);
    int32_t objectToolRotate(ee::Ptr<EditUiButton> button);
    void placeEditPiece(uint32_t anchor, ee::Ptr<EditObjectPart> object);
    void readEditUiButtons(uint32_t address);
    uint32_t registerEditCoreFunctions();
    void saveAnchor(uint32_t anchor, uint32_t stream);
    int32_t setEditUiPad(uint32_t address, ee::Address pad);
    int32_t setPowerLevel(ee::Float level);
    int32_t startToolMoveBack(ee::Ptr<EditUiButton> button);
    int32_t startToolMoveForward(ee::Ptr<EditUiButton> button);
    int32_t startToolMoveLeft(ee::Ptr<EditUiButton> button);
    int32_t startToolMoveRight(ee::Ptr<EditUiButton> button);
    uint32_t startToolPlace(ee::Ptr<EditUiButton> button);
    int32_t startToolRotate(ee::Ptr<EditUiButton> button);
    int32_t toolButtonPause(ee::Ptr<EditUiButton> button);
    int32_t updateEditFlash(ee::Float seconds);
}
