#pragma once

// A game's state as the bottom screen reads it (the collection's panel,
// collection/panel.h): one frame's snapshot, plain data copied out
// of guest RAM by the game's own capture. The contract between a game and
// the panel: each game's override fills a Snapshot from its own RAM layout
// (game_overrides/aggressive_inline_panel.cpp, dave_mirra_panel.cpp,
// bmx_xxx_panel.cpp) and installs its capture here; the panel draws from
// the snapshot and never sees the game. The folder of the running game's
// panel files travels the same way.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace panel
{
    constexpr uint32_t kMaxObjectives = 64; // (Snapshot::objectiveDoneMarks: a bit each)
    constexpr uint32_t kMaxScriptWords = 2048;
    constexpr uint32_t kMaxTrackedObjects = 512;
    constexpr uint32_t kMaxCollectables = 256;
    constexpr uint32_t kMaxGoalTargets = 256;
    constexpr uint32_t kGoalStatusBytes = 96;

    // A goal's state (Snapshot::objectiveState). Aggressive Inline's capture
    // copies the script's state name hash (any other value is open); the
    // other games' captures use these four.
    constexpr uint32_t kStateInit = 0x0004E2E4u;     // nameHash("init"): not offered yet (locked)
    constexpr uint32_t kStateComplete = 0x04250E05u; // nameHash("complete"): done
    constexpr uint32_t kStateOpen = 1u;              // running
    constexpr uint32_t kStateReady = 2u;             // can be started now (what it waited for is done): its giver shows

    // What a goal's target is for, and when.
    enum class TargetRole : uint8_t
    {
        Pickup,  // something to get (a can, a hooker, a switch)
        DropOff, // where to bring what was picked up (a checkpoint)
        Place,   // somewhere to be or ride (a gap's faces, a car, a pool)
    };
    enum class TargetPhase : uint8_t
    {
        Now,   // the game asks for it now
        Later, // comes after the current step (an ordered set's next member)
        Done,  // got, reached, cleared
    };
    // A goal's target as the game has it this frame (a live position, its
    // own done mark): a goal with any of these shows them in place of the
    // level file's targets.
    struct GoalTarget
    {
        int32_t goal;       // the goal's slot (objectiveState's index)
        uint32_t nameHash;  // the object's name, hashed (0 for a place)
        float x, z;
        TargetRole role;
        TargetPhase phase;
        uint8_t _0e[2];
    };
    constexpr uint32_t kStatCount = 7;
    constexpr uint32_t kLineBytes = 256;
    constexpr uint32_t kMoveBitBytes = 0x18; // room for BMX XXX's 186 moves (DM2 has 158)

    enum class GameMode : uint8_t
    {
        NoLevel, // the front end, a loading screen
        Play,
        Paused,
    };

    // A scene object where the game has it now (cars and NPCs move).
    struct TrackedObject
    {
        uint32_t nameHash;
        float x, y, z;
    };

    // Something to pick up, from the game's prop list (level/props.cpp). `kind`
    // as the game numbers its prop definitions (initPropDefinitions): 0 .. 2
    // juice, 3 and 49 skates, 4 .. 10 stat points, 11 .. 18 keys, 19 .. 26
    // locks, 50 .. 56 the skaters' heads (special tricks), the rest the
    // levels' own collectables. BMX XXX's level pickups (games/bmx/game/
    // pickups.cpp PickupKind) from kBmxPickupKinds: health, weapon, score,
    // bike part, sniper.
    constexpr uint8_t kBmxPickupKinds = 0x80;
    struct Collectable
    {
        uint8_t kind;
        uint8_t shown;      // the game's +0x64: 0 once taken or not offered
        float x, z;
        uint32_t ownerHash; // +0x60: the script object told when it is taken (a key's goal), or 0
    };

    // One game frame's state, copied out of guest RAM. Plain data: nothing
    // in it points into the game.
    struct Snapshot
    {
        uint32_t sequence = 0;     // grows by one per capture
        int32_t level = -1;        // the level index, -1 with GameMode::NoLevel
        char levelFile[16] = {};   // the panel file's name: the game's level folder, or what the host serves there
        GameMode mode = GameMode::NoLevel;
        uint8_t gameType = 0;      // aggressive::GameType (1 career, 2 free skate, 3 timed, 4 split)
        uint8_t skaterState = 0;   // skater::State
        uint8_t skaterIndex = 0;
        char skaterName[16] = {};

        float x = 0.0f, y = 0.0f, z = 0.0f; // Player +0x90
        float forwardX = 0.0f, forwardZ = 1.0f; // the skater's heading on the ground plane
        float speed = 0.0f;

        int32_t score = 0;
        float pendingScore = 0.0f;  // the line's trick score, banked on landing
        int32_t multiplier = 0;     // the line's multiplier; 0: the game keeps none (DM2)
        int32_t points = 0;         // a challenge's points counter
        float timerSeconds = -1.0f; // the HUD's countdown; -1 while it shows none
        int32_t spinsCounterClockwise = 0, spinsClockwise = 0; // half turns this jump
        char line[kLineBytes] = {}; // the line's trick names as the HUD has them
        char currentTrick[32] = {};

        uint64_t levelFlags = 0;    // Player +0x5E8: bit (level + 9): the locked area's key held
        uint16_t specialTricks = 0; // Player +0x5F0
        uint8_t statLevel[kStatCount] = {}; // Jump, Speed, Spin, Grind, Manual, Fakie, Wallride
        int32_t statPoints[kStatCount] = {};

        // A game that keeps its tricks per move (DM2): a bit a move by its
        // number (UiFile::moves), the moves the character cannot do and
        // those it has landed.
        bool hasMoves = false;
        uint8_t movesOff[kMoveBitBytes] = {};
        uint8_t movesLanded[kMoveBitBytes] = {};

        uint32_t objectiveCount = 0;
        uint32_t objectiveState[kMaxObjectives] = {}; // the state's name hash (+0x0C of the game's record)
        // A bit a goal (objectiveState's index): done once, as the save
        // keeps it. Set beside kStateComplete, and on a goal that runs again
        // after it was done (BMX XXX's repeatable jobs: kStateOpen, its
        // targets shown, still counted done). 0 from a capture without.
        uint64_t objectiveDoneMarks = 0;
        int32_t activeGoal = -1;                      // the goal the game's own display shows the player, -1 none
        char goalStatus[kGoalStatusBytes] = {};       // its status line as the game formats it ("Hookers delivered: 1 of 5"), "" none

        uint32_t goalTargetCount = 0;
        GoalTarget goalTargets[kMaxGoalTargets] = {}; // grouped by goal

        uint32_t scriptWordCount = 0;
        uint32_t scriptWords[kMaxScriptWords] = {};   // the level script's variables, raw
        // Which script is loaded (AI: the PS2 disc's or the Xbox release's,
        // whose variables lie elsewhere): its variables' bytes, and each
        // objective's name and description as offsets in its strings
        // (+0x18, +0x1C of the game's record). 0: not known (the level
        // file's first goal set is used).
        uint32_t scriptVariableBytes = 0;
        uint32_t objectiveText[kMaxObjectives][2] = {};

        uint32_t trackedCount = 0;
        TrackedObject tracked[kMaxTrackedObjects] = {}; // sorted by nameHash

        uint32_t collectableCount = 0;
        Collectable collectables[kMaxCollectables] = {};
    };

    // Reads one snapshot from the guest's main RAM (ramBytes, 32 MB; EE
    // addresses as in the game) with the running game's capture. Read-only
    // and bounded: a level that is loading gives GameMode::NoLevel, never a
    // wild read. Call it at a frame boundary on the game thread. With no
    // capture installed (no game booted yet), GameMode::NoLevel.
    void captureSnapshot(const uint8_t *guestRam, size_t ramBytes, Snapshot &out);

    // A game's capture: its override installs it as the game starts
    // (game_overrides/aggressive_inline_panel.cpp, dave_mirra_panel.cpp,
    // bmx_xxx_panel.cpp), the app clears it before a boot. Null: none.
    using GameCapture = void (*)(const uint8_t *guestRam, size_t ramBytes, Snapshot &out);
    void setGameCapture(GameCapture capture);

    // The folder of the running game's panel files (its bundled files'
    // `panel`: Resources/games/<id>/panel in the collection), set by the app
    // as it boots a game, with setGameCapture(nullptr) before the game's
    // override installs its own capture. Read by the painter's thread to
    // follow the game that runs; "" until a game boots. `fallback`: the
    // folder a file is read from when `folder` has none of it, or has it in
    // another version (the bundled files under the setup's). Thread-safe.
    void setGameFiles(const std::string &folder, const std::string &fallback = {});
    std::string gameFiles();
    std::string gameFilesFallback();
    // A level file the running boot serves in place of the folder's
    // <name>.pmap: Custom Freeplay's map, made at its boot from the files
    // the boot reads (collection/maps.h), under the slot's panel name. Set
    // by the app before the boot, cleared (name "") after it; the painter's
    // thread asks for it as it loads a level. Thread-safe.
    void setServedLevelFile(const std::string &name, std::shared_ptr<const std::vector<uint8_t>> bytes);
    // The bytes served under `name`, null for none.
    std::shared_ptr<const std::vector<uint8_t>> servedLevelFile(const std::string &name);

    // ---- Clean Top Screen ------------------------------------------------------
    // The game's HUD pieces a bottom screen shows (its live screen's cards,
    // collection/panel.h), which the top screen leaves out while the player
    // asks for it (settings.ini [display] clean_top_screen). A piece is left
    // out while
    //  - the setting is on (the app says so: setCleanTopScreen),
    //  - a bottom screen is up and its screen shows the piece whenever the
    //    game has it (the painter, a dual-screen handheld's second display
    //    or a computer's Map Window, says so after each picture:
    //    setBottomScreenShows; not a reference such as the goal list), and
    //  - the game's capture gives the bottom screen the piece
    //    (captureSnapshot notes it): a clock or a goal in a capture of the
    //    last second (the second covers the top's widget fading out once
    //    the game's own is over), a line in any capture since the game
    //    booted (a line's top widget shows its spin before the line has a
    //    trick, the bottom screen's from the first trick: the top's line is
    //    the bottom's once the game's capture has given a line at all; a
    //    capture that reads none keeps the top's).
    // Each game's override asks hiddenOnTop for each of its HUD widgets as
    // zgui draws them (aggressive_inline_gameplay.cpp,
    // dave_mirra_gameplay.cpp, bmx_xxx_gameplay.cpp). Drawing only: a widget
    // left out is not drawn, and nothing of the game's own changes.
    enum class HudPiece : uint8_t
    {
        Clock,     // the clock of a timed goal or turn (Snapshot::timerSeconds)
        TrickLine, // the line's tricks and their points (Snapshot::line, pendingScore)
        Goal,      // the goal the game's own display shows: its name and status (Snapshot::activeGoal)
        Count
    };
    using HudPieces = uint32_t; // a bit a HudPiece
    constexpr HudPieces hudPiece(HudPiece piece) { return HudPieces{1} << static_cast<uint32_t>(piece); }

    enum class CleanTopScreen : uint8_t
    {
        Off,    // the top screen whole (the default)
        On,     // the pieces a bottom screen shows left out, while one is up
        Always, // every piece left out, a bottom screen or not: test runs (logiccheck, shots)
    };
    // The app, from the setting; ZX_CLEAN_TOP_SCREEN=0|1|always says it
    // instead (read at the first ask, and then the app's setting is not
    // applied). Any thread.
    void setCleanTopScreen(CleanTopScreen mode);
    CleanTopScreen cleanTopScreen();
    // The painter, after each picture: the pieces its screen shows (0: none,
    // a blank screen or one of its references). Any thread.
    void setBottomScreenShows(HudPieces pieces);
    // Whether a bottom screen is up: a picture painted in the last second.
    bool bottomScreenUp();
    // A game's HUD draw: whether the top screen leaves `piece` out now. Any thread.
    bool hiddenOnTop(HudPiece piece);
}
