// DM2's park editor: the park level's high-score tables. The game keeps a
// set of tables for each level, 27 entries of 0x14 bytes (the value, a word
// and the holder's name; "MiracleBoy" and "ByTor" hold the ones a new game
// starts with); the set after the eight levels' is the park's. The editor
// keeps a copy of it in its record, and a park's file carries that copy.
#pragma once

#include "park_editor.h"

#include "ee/ee.h"
#include "ee/plain.h"

#include <cstdint>

namespace game::dm2
{
    constexpr uint32_t kParkScoresBytes = sizeof(ParkEditor::scores);

    // The editor's copy taken from the game's tables (two functions of the
    // same code), and the game's tables set from the editor's copy.
    void takeParkScores(ee::Ptr<ParkEditor> editor);
    void takeParkScoresAgain(ee::Ptr<ParkEditor> editor);
    void applyParkScores(ee::Ptr<ParkEditor> editor);

    // The editor's copy read from and written to a park's stream.
    void readParkScores(ee::Ptr<ParkEditor> editor, ee::Address stream);
    void writeParkScores(ee::Ptr<ParkEditor> editor, ee::Address stream);
}
