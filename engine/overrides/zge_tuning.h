#pragma once

// A developer tuning table for the games on the Z-Axis engine that keep none
// of their own on the PS2 (Dave Mirra BMX 2, BMX XXX): rows of a game's
// globals by the names of the GameCube BMX XXX's TUNING screen, set from
// Mods/tuning.ini at the start, listed in tuning-reference.ini and shown
// live on the TUNING screen (F2) with the developer menu.
//
// tuning.ini (zge_devtools.h tuningFile) sets rows before the game starts;
// with ZX_DEV_MENU=1 tuning-reference.ini beside it lists every row with the
// game's value and the GameCube's range, and F2 shows the screen. With no
// tuning.ini and the developer menu off nothing is read, written or hooked.
//
// The TUNING screen draws as the GameCube's (0x8004F1B4, 0x8004F3D8,
// 0x8004F478): twelve rows at a time, up and down move the cursor (the list
// scrolls with it), left and right change the row's value by its step
// within its range. The lines are the engine's debug text, printed just
// before the strings' task draws them, with the engine's debug drawing on
// for them (the game's frame sets it again from its debug display at the
// next frame's start), without the profiler. The keys also reach the game,
// as on the GameCube's in-game menu. ZX_TUNING_SCREEN=1 (developers) shows
// it from the start, ZX_TUNING_SCREEN_ROW=<n> with the cursor on row n (1
// first).

#include <cstddef>
#include <cstdint>

class PS2Machine;

namespace zge_host::tuning
{
    enum class Kind
    {
        Byte,  // 0 or 1, or a small count
        Half,  // a 16-bit word
        Word,  // a 32-bit count
        Float,
        Angle, // a 16-bit word, 65536 a turn
    };

    struct Row
    {
        const char *name;  // the GameCube's
        uint32_t global;   // the game's global it is (0 in a rider row: the riding rider's record)
        uint32_t field;    // within it
        Kind kind;
        float step, least, most; // the GameCube's
    };

    // A game's table and the game's functions the screen uses.
    struct Table
    {
        const char *game;      // the reference's first line ("BMX XXX")
        const char *about;     // the reference's next lines: what the table is (each line "; ...\n")
        const Row *rows;
        size_t rowCount;
        // The rider abilities: fields of the riding rider's record (riderOf's
        // for rider 0), made from the character's stats at each ride, so the
        // screen's alone, after the table's rows while a rider rides;
        // tuning.ini has no row of them.
        const Row *riderRows;
        size_t riderRowCount;
        uint32_t riderOf;      // the rider record of a rider index
        uint32_t drawText;     // the strings' task (the screen prints before it)
        uint32_t print;        // the debug text's print (x, y, format, the line)
        uint32_t debugEnabled; // the engine's debug drawing on (a word)
    };

    // tuning.ini's rows set, the reference written and the screen hooked, as
    // above (the table lives as long as the session: a game's constant).
    void install(PS2Machine &runtime, const Table &table);
}
