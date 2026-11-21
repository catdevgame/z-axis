#pragma once

// The packaged panel files, written at build time by
// tools/modding/panel_map.py (their contents come from the game's own
// data, so they are never committed).
//
//   file:   "PMAP", u32 version, then chunks {char tag[4], u32 bytes, data}
//           each padded to 4 bytes.
//   <FOLDER>.pmap (a level):
//     NAME  the level's name, zero-terminated
//     MAPI  u32 width, height; f32 worldLeft, worldTop, pixelsPerUnit; u32 turned;
//           then height rows of RLE RGB565: {u16 run, u16 colour}.
//     SCRP  u32 variableBytes, u32 count, {u32 name, description}[count]: the
//           level script the goal set after it is for (version 3; see GoalSet)
//     GOAL  u32 count, GoalRecord[count]  (version 2: the status and activate texts, the target word)
//     TARG  u32 count, TargetRecord[count]
//     GIVE  u32 count, BubbleRecord[count]
//     TEXT  the goals' strings, zero-terminated (the script's own)
//     (SCRP GOAL TARG GIVE TEXT again for each other script the level can run)
//   UI.pmap (the game's fonts and icons):
//     SPEC  u32 count, SpecialTrick[count]
//     TRIK  u32 count, TrickRecord[count]
//     MOVE  u32 count, TrickMove[count]: TRIK's rows by the game's move
//           numbers (a game that keeps its tricks per move: DM2), optional
//     FAMS  u32 count, FamilyName[count]: the families' names, optional
//     FONT  char name[32]; u32 lineHeight, glyphs, pages; pages x {u32 width,
//           height; RGBA}; GlyphRecord[glyphs]
//     ICON  char name[32]; u32 width, height; RGBA

#include "collection/panel_view.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace panel
{
    constexpr uint32_t kFileVersion = 3;

    // One goal, as the level script's objective table and handlers have it.
    struct GoalRecord
    {
        uint32_t title;         // +0x00 in TEXT
        uint32_t text;          // +0x04 in TEXT
        uint32_t ownerHash;     // +0x08 the object whose goal it is (a giver, a helper)
        int32_t counterWord;    // +0x0C the script word that counts, -1 none
        int32_t counterTarget;  // +0x10 the count that completes it
        uint32_t firstTarget;   // +0x14 in TARG
        uint32_t targetCount;   // +0x18
        uint8_t type;           // +0x1C 0 other, 1 points, 4 wall ride, 5 grind, 6 combo, 7 transfer, 8 lesson
        uint8_t tier;           // +0x1D 0 .. 2; 3 hidden: listed only once done
        uint8_t lockedArea;     // +0x1E shown only with the level's key
        uint8_t points;         // +0x1F the pause menu's points
        float anchorX;          // +0x20 where it is when no target is placed
        float anchorZ;          // +0x24 NaN: anywhere on the level
        uint8_t scoreKind;      // +0x28 0 none, 1 the skater's score, 2 the line's, 3 a challenge's points, 4 the score with the line's
        uint8_t _29[3];
        int32_t scoreTarget;    // +0x2C
        uint32_t status;        // +0x30 in TEXT: the game's own status line with two %d (the count, the target), "" none
        int32_t targetWord;     // +0x34 the script word that holds the count that completes it, -1: counterTarget
        uint32_t activateText;  // +0x38 in TEXT: what starts it, shown while it can be started ("Go talk to Homeless Jones."), "" none
    };
    static_assert(sizeof(GoalRecord) == 0x3C);

    struct TargetRecord
    {
        uint32_t nameHash;   // +0x00 the object, by scene name
        float x;             // +0x04 where the level file places it; a live position wins
        float z;             // +0x08
        int32_t doneWord;    // +0x0C the script word that says it is done, -1 none
        uint32_t doneValue;  // +0x10 the value it has then
        uint32_t role;       // +0x14 TargetRole, from the level's own naming (a checkpoint is a drop-off)
    };
    static_assert(sizeof(TargetRecord) == 0x18);

    // An NPC's challenge bubble: it shows while a script word has a value,
    // or, with no word (-1), while the goal whose ownerHash is its name is
    // ready to start (kStateReady).
    struct BubbleRecord
    {
        uint32_t nameHash;
        float x, z;
        int32_t stateWord;
        uint32_t stateValue;
    };
    static_assert(sizeof(BubbleRecord) == 0x14);

    // A row of the pause menu's special trick list.
    struct SpecialTrick
    {
        uint32_t bit;      // of Snapshot::specialTricks
        char name[48];
        char buttons[16];  // in fnt_ps2buttons' glyphs
    };
    static_assert(sizeof(SpecialTrick) == 68);

    // A trick of the game's tables, with the stick directions that ask for it.
    enum class TrickFamily : uint8_t
    {
        Grab,
        Flip,
        Combo,   // a grab and a flip
        Grind,
        Other,   // hand plants, manuals
        Special, // found, not given: unlockBit
        Count,
    };
    struct TrickRecord
    {
        TrickFamily family;
        uint8_t unlockBit;    // of Snapshot::specialTricks; 0: always there
        uint8_t inputCount;
        uint8_t button;       // fnt_ps2buttons glyph after the directions; 0 none
        uint8_t inputs[4];    // directions, oldest first: 1 up, then clockwise
        char name[40];
    };
    static_assert(sizeof(TrickRecord) == 48);

    // A row of TRIK as a game that keeps its tricks per move has it (DM2's
    // character record): the move's bit in Snapshot::movesOff and
    // movesLanded, and whether the row is listed only while the character can
    // do the move (a signature move); a move not landed yet is drawn dim.
    struct TrickMove
    {
        uint8_t move;
        uint8_t whileAble;
        uint16_t _02;
    };
    static_assert(sizeof(TrickMove) == 4);
    // A family's name on the tricks pane ("AIR", "GRINDS"), as the game calls it.
    struct FamilyName
    {
        char name[16];
    };
    static_assert(sizeof(FamilyName) == 16);
    struct GlyphRecord
    {
        uint8_t character;
        uint8_t page;
        uint16_t _02;
        uint16_t x, y, width, height;
    };
    static_assert(sizeof(GlyphRecord) == 12);

    // A level file as loaded: the map as RGB565, a mip per halving.
    struct MapLevel
    {
        int width = 0, height = 0;
        std::vector<uint16_t> pixels;
    };

    // A level's goals as one of its scripts has them. Aggressive Inline runs
    // a level's PS2 disc script or the Xbox release's (the career scripts
    // setting, and a saved career keeps the script it was played with),
    // and their script words lie in different places: a set says which
    // script it is for by what the game has loaded (SCRP): the script's
    // variables' bytes and each objective's name and description offsets.
    // A set without SCRP is for whatever script runs (DM2, BMX XXX).
    struct GoalSet
    {
        bool keyed = false;
        uint32_t variableBytes = 0;
        std::vector<uint32_t> objectiveTexts; // name, description for each objective
        std::vector<GoalRecord> goals;
        std::vector<TargetRecord> targets;
        std::vector<BubbleRecord> bubbles;
        std::vector<char> text;
    };

    struct LevelFile
    {
        std::string name;
        MapTransform transform;
        std::vector<MapLevel> mips;
        std::vector<GoalSet> goalSets;
        // The set in use (useGoalSet), the first one at load.
        int goalSet = -1;
        std::vector<GoalRecord> goals;
        std::vector<TargetRecord> targets;
        std::vector<BubbleRecord> bubbles;
        std::vector<char> text;

        const char *string(uint32_t offset) const { return offset < text.size() ? text.data() + offset : ""; }
    };

    // The goal set for the script the game has loaded (`variableBytes`, the
    // objectives' name and description offsets, two words each): an
    // unkeyed set, else the keyed one that matches, else -1 (none of the
    // file's scripts).
    int matchingGoalSet(const LevelFile &level, uint32_t variableBytes, const uint32_t *objectiveTexts, uint32_t objectiveCount);
    // Puts a set in use. -1: the first set's titles and texts alone, with
    // nothing read from script words (counts, places done, givers) and no
    // score kind: the loaded script is none the file knows, so its words
    // are not where the file's are.
    void useGoalSet(LevelFile &level, int set);

    struct Image
    {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    struct Font
    {
        int lineHeight = 0;
        std::vector<Image> pages;
        GlyphRecord glyphs[256] = {};
        bool has[256] = {};
    };

    struct UiFile
    {
        std::map<std::string, Font> fonts;
        std::map<std::string, Image> icons;
        std::vector<SpecialTrick> specialTricks;
        std::vector<TrickRecord> tricks;
        std::vector<TrickMove> moves;          // empty, or one a trick
        std::vector<FamilyName> familyNames;   // empty, or one a family
    };

    // From bytes in memory; false on a file that is not whole.
    bool parseLevelFile(const uint8_t *data, size_t bytes, LevelFile &out);
    bool parseUiFile(const uint8_t *data, size_t bytes, UiFile &out);
    bool loadLevelFile(const std::string &path, LevelFile &out);
    bool loadUiFile(const std::string &path, UiFile &out);
}
