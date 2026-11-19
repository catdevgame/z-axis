#pragma once

#include <cstdint>
#include <string>

class PS2Runtime;
struct R5900Context;

namespace aggressive_inline
{
    // TUT/Estate, the GameCube disc's earlier Tutorial, in free skate
    // (PS2X_ESTATE_DIR, made at build time by ps2xGcConvert; PS2X_ESTATE=1
    // turns it on): its level-select entry and its files as the Tutorial's
    // level index 7. See aggressive_inline_estate.cpp.
    void installEstate(PS2Runtime &runtime);
    // Its free skate level-select entry once the menu patch has run, else -1.
    int estateLevelEntry();
    // Estate is the level the Tutorial's files are served for (it was picked
    // in the free skate level select, and the game is not a career).
    bool estateActive();

    // Estate's lessons in the Tutorial's pause list, which the lesson
    // natives (aggressive_inline_restart.cpp) ask while Estate is played:
    // the list's lesson items (0..22, the retail Tutorial's 23, the same
    // topics) are Estate's lessons 1-8, 10-20 and 22-25.
    // The item of the lesson running, -1 if none.
    int estateCurrentLessonItem(const uint8_t *rdram);
    // Ends the lesson running and starts the item's, as Estate's career
    // script's controller would (false if it cannot).
    bool estateJumpToLessonItem(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int item);
    // The completed-lesson column's Items for the 23 lesson items (each
    // "\\n`" done or "\\n~" to do, as restart::tutorialCheckboxItems makes them).
    std::string estateLessonCheckboxes(const uint8_t *rdram);
}
