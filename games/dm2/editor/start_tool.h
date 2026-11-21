// DM2's park editor: the start tool. The park has one start, the cell and
// the heading the rider begins a run at; the tool shows a marker at the
// cursor and another on the cell where the start is placed. DM2's own code
// (it links after the editor's block).
#pragma once

#include "park_editor.h"

#include "ee/ee.h"
#include "zge/math/vec.h"

#include <cstdint>

namespace game::dm2
{
    // No start placed, no marker shown and the cursor's marker unturned.
    void clearStartMarkers(ee::Ptr<ParkEditor> editor);
    // The cursor's marker made, and the placed start's if there is one;
    // both released.
    void newStartMarkers(ee::Ptr<ParkEditor> editor);
    void releaseStartMarkers();
    void freeStartMarkers();
    // The cursor's marker made again where the cursor is.
    void renewStartMarker(ee::Ptr<ParkEditor> editor);
    void moveStartMarkerToCursor(ee::Ptr<ParkEditor> editor);
    // The cursor's marker a quarter turn on.
    void turnStartMarker();

    // 1 unless the cursor's cell holds one of the objects a start cannot
    // stand on, or a rail.
    uint32_t startFitsAtCursor(ee::Ptr<ParkEditor> editor);
    // The start put on the cursor's cell with the marker's heading; taken
    // off the park again.
    void placeStart(ee::Ptr<ParkEditor> editor);
    void removePlacedStart(ee::Ptr<ParkEditor> editor);
    // The game's "PlayerOne" spawn set from the park's start.
    void setParkSpawn(ee::Ptr<ParkEditor> editor);
    // The "Spawn" scene point of a name moved to a matrix's place and turn,
    // or added when the level has none (addSpawnPoint).
    void setSpawnPosition(ee::Ptr<zge::Matrix> matrix, ee::Ptr<const char> name);
    void addSpawnPoint(ee::Ptr<zge::Matrix> matrix, ee::Ptr<const char> name);

    // The start in the park's stream: its cell, its heading and whether it
    // is placed, a byte each.
    void saveParkStart(ee::Ptr<ParkEditor> editor, ee::Address stream);
    void loadParkStart(ee::Ptr<ParkEditor> editor, ee::Address stream);

    // 1 for a point of the park that is at least 15 units inside its edges
    // (x and z; the park is 5 units a cell around the origin).
    uint32_t pointInLevel(ee::Ptr<const zge::Vec4> point);
}
