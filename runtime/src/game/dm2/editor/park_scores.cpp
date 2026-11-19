// DM2's park editor: the park level's high-score tables (park_scores.h).
// DM2's own code (its editor block).

#include "park_scores.h"
#include "edit_core.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/editor/park_scores.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The game's tables for the park level.
        constexpr ee::Ptr<uint8_t> kParkLevelScores{sym::g_parkLevelScores};

        ee::Ptr<uint8_t> editorScores(ee::Ptr<ParkEditor> editor)
        {
            return ee::Ptr<uint8_t>(editor->scores);
        }

        // Scaffolding (address-call): the park stream's byte reader and
        // writer (the editor core) have no readable body yet.
    }

    void takeParkScores(ee::Ptr<ParkEditor> editor)
    {
        zge::memoryCopy(editorScores(editor), kParkLevelScores, kParkScoresBytes);
    }
    READABLE_FUNCTION(takeParkScores, Verified);

    void readParkScores(ee::Ptr<ParkEditor> editor, ee::Address stream)
    {
        game::tail(parkStreamRead, stream, editorScores(editor), kParkScoresBytes);
    }
    READABLE_FUNCTION(readParkScores, Verified);

    void writeParkScores(ee::Ptr<ParkEditor> editor, ee::Address stream)
    {
        game::tail(parkStreamWrite, stream, editorScores(editor), kParkScoresBytes);
    }
    READABLE_FUNCTION(writeParkScores, Verified);

    void applyParkScores(ee::Ptr<ParkEditor> editor)
    {
        zge::memoryCopy(kParkLevelScores, editorScores(editor), kParkScoresBytes);
    }
    READABLE_FUNCTION(applyParkScores, Verified);

    void takeParkScoresAgain(ee::Ptr<ParkEditor> editor)
    {
        zge::memoryCopy(editorScores(editor), kParkLevelScores, kParkScoresBytes);
    }
    READABLE_FUNCTION(takeParkScoresAgain, Verified);
}
