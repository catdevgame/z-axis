#pragma once

// What BMX XXX's game screens call and read outside overlay.c that has no
// body or header in this tree yet: the game's other modules (bmx.c's menu
// machine and texts, action.c's players, game.c's rounds, the rider), each
// called where it is. Every one becomes the plain call or the owner's record
// once its header is here.
//
// Scaffolding (address-call): docs/SCAFFOLDING.md. The records below are
// the owners' as the screens read them, kept in this namespace so that they
// do not stand in the way of the owners' own.

#include "bmx/action/camera_state.h"
#include "bmx/camera/action_camera.h"
#include "bmx/game/npcs.h"
#include "bmx/game/player_riders.h"
#include "bmx/game/round.h"
#include "bmx/profile/players.h"
#include "bmx/profile/records.h"
#include "bmx/replay/replays.h"
#include "bmx/save/game_save.h"
#include "bmx/sound/events.h"
#include "bmx/sound/sound_mode.h"
#include "bmx/states/game_state.h"
#include "bmx/ui/menu.h"
#include "bmx/ui/records_frame.h"
#include "bmx/ui/strings.h"
#include "bmx/ui/ui_nodes.h"
#include "ee/ee.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/scene/objectives.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/overlay/screen_calls.h.h"

namespace game::bmx::calls
{
    using zge::zgui::Node;

    // ---- bmx.c: the menu machine, the UI's nodes, the texts, the game's state
    // The UI's "MsgType": the message waiting for the menu's handler, sent
    // by its script function.
    inline ee::Global<int32_t, sym::g_msgType> g_msgType;
    // The player whose pad has the menus (the UI's "ModalPlayerNum").
    inline ee::Global<int32_t, sym::g_modalPlayerNum> g_modalPlayerNum;
    // The action `name` of a node played; nothing if there is no node.
    // The saved game written to the memory card (the save system's own screens).
    // A string table of the library by name, and one freed.
    // A text copied onto one line, cut with "..." past `limit` characters.
    // The game's state asked for (the front end, the level, the park editor,
    // a movie).
    constexpr uint32_t kGameStateFrontEnd = 0;
    constexpr uint32_t kGameStateParkEditor = 2;
    constexpr uint32_t kGameStateMovie = 4;
    // The sound's mode (Aggressive Inline's setGameMode): 2 as play goes on.
    constexpr uint32_t kSoundModePlay = 2;
    // The play phase the sound follows (DM2's name; the screens only clear it).

    // The levels' table and the saved game's language.
    struct LevelInfo
    {
        ee::Ptr<const char> code;  // +0x00 two letters ("DM")
        ee::Ptr<const char> name;  // +0x04 its files' name
        ee::Ptr<const char> title; // +0x08 ("The Dam")
        uint8_t _0c[4];
    };
    static_assert(sizeof(LevelInfo) == 0x10);
    constexpr uint32_t kLevels = 12;
    inline ee::Ptr<LevelInfo> levelInfo(uint32_t level) { return ee::Ptr<LevelInfo>(sym::g_levels) + level; }
    inline ee::Global<uint8_t, sym::g_currentLevel> g_currentLevel;
    struct SavedGameStart
    {
        uint8_t _00[0x1F];
        uint8_t language;    // +0x1F
        uint8_t soundVolume; // +0x20 the options' two volumes, 0 .. 100
        uint8_t musicVolume; // +0x21
    };
    inline ee::Global<SavedGameStart, sym::g_savedGame> g_savedGame;
    constexpr uint32_t kLanguages = 5;
    inline ee::Global<ee::Ptr<const char>[kLanguages], sym::g_languageCodes> g_languageCodes;

    // (A rider character's record and its record of a level, and the player's
    // character: profile/players.h.)

    // What a player picked (the profile's): the rider character and a name.
    struct PlayerSetup
    {
        uint8_t flags;      // +0x00 kSetupVibration
        uint8_t _01;
        uint8_t cameraType; // +0x02 the player's camera type (the options' "Camera Type")
        uint8_t character;  // +0x03 an index of the characters; 13 and up are the custom riders
        uint8_t _04;
        char name[0x20];   // +0x05
    };
    static_assert(offsetof(PlayerSetup, name) == 0x05 && sizeof(PlayerSetup) == 0x25);
    inline ee::Ptr<PlayerSetup> playerSetup(uint32_t player) { return ee::Ptr<PlayerSetup>(sym::g_playerSetups) + player; }
    constexpr uint8_t kFirstCustomCharacter = 13;
    constexpr uint8_t kSetupVibration = 0x01;
    // The game's table of the characters' names, a row a character.
    struct CharacterNames
    {
        ee::Ptr<const char> first;     // +0x00
        ee::Ptr<const char> last;      // +0x04
        uint8_t _08[4];
        ee::Ptr<const char> boardName; // +0x0C the name the leaderboard lists
        uint8_t _10[4];
    };
    static_assert(sizeof(CharacterNames) == 0x14);
    inline ee::Ptr<CharacterNames> characterNames(uint32_t character)
    {
        return ee::Ptr<CharacterNames>(sym::g_characterNames) + character;
    }

    // ---- action.c: the players, and what the level is doing
    inline ee::Global<uint8_t, sym::g_playerCount> g_playerCount;
    // What is being played (0 .. 6): 4 is a park's test from the editor.
    inline ee::Global<uint8_t, sym::g_sessionKind> g_sessionKind;
    constexpr uint8_t kSessionParkTest = 4;
    inline ee::Global<ee::Ptr<ActionCamera>, sym::g_gameCamera> g_gameCamera;
    // What the level shows (cameraState's value; the names are read from
    // the setter's body: the overlay's parts it shows for each).
    constexpr uint32_t kCameraStatePlay = 0;
    constexpr uint32_t kCameraStateScreen = 1;
    constexpr uint32_t kCameraStateReplay = 2;
    constexpr uint32_t kCameraStateReplayDemo = 4;

    // ---- game.c: the round
    constexpr uint32_t kTurnStartPhase = 2;
    constexpr uint32_t kTurnRunningPhase = 3;
    constexpr uint32_t kTurnOverPhase = 4;
    // The round's flags (roundFlags).
    constexpr uint32_t kGameEndsFlag = 0x01;  // this round is the game's last
    constexpr uint32_t kRoundDoneFlag = 0x02; // every player has had the round's turn
    // The game modes the screens tell apart, by the instructions each has
    // ("$KSTRSP_INST_LEV", "_COMP_LEV", "_SESSION_LEV", "_FREE"); the modes
    // from 4 up are the multiplayer games.
    constexpr uint8_t kGoalsMode = 0;
    constexpr uint8_t kSessionMode = 2;
    constexpr uint8_t kFreeMode = 3; // a park's test is played in it
    constexpr uint32_t kTieFlag = 0x08;       // the game's lead is shared
    // The player who won a round.
    // A mode's own record kind, by mode.
    inline ee::Ptr<uint8_t> modeRecordSlots() { return ee::Ptr<uint8_t>(sym::g_modeRecordSlots); }
    // The texts game.c writes for a game's end (named from their strings:
    // "$KSTR_SERIES_WIN_%d" / "$KSTR_SERIES_TIE", and the players' letters).
    inline void seriesStandingText(ee::Address out) { ee::game(sym::seriesStandingText, out); }
    inline void wordGameStandingText(ee::Address out) { ee::game(sym::wordGameStandingText, out); }
    // The player whose pad runs the replay shown.
    inline ee::Global<uint8_t, sym::g_replayPadPlayer> g_replayPadPlayer;
    // How the game was started (1 .. 3: the game over screen offers no save).
    inline ee::Global<uint8_t, sym::g_startUpKind> g_startUpKind;

    // A distance (metres) as feet and inches.
    // A text copied with line breaks put in once a line has `width` characters.

    // The riders met in a level (game.c's NPCs): what one is, as its
    // screen shows it.
    struct NpcInfo
    {
        ee::Ptr<const char> name;          // +0x00 its character's name
        uint8_t character;                 // +0x04
        uint8_t _05[3];
        ee::Ptr<const char> objectiveName; // +0x08 the objective it challenges the player to
        uint8_t goal;                      // +0x0C that objective's goal number
        uint8_t meetings;                  // +0x0D how often the player has met it
        uint8_t rider;                     // +0x0E
        uint8_t _0f;
    };
    static_assert(sizeof(NpcInfo) == 0x10);
    // What the game knows of an NPC copied out (the camera and the objectives call it npcFacts too).
    inline void npcFacts(uint32_t npc, ee::Address out) { ee::game(sym::npcFacts, npc, out); }
    constexpr uint32_t kNpcWaiting = 5; // it waits to ride

    // ---- the objectives (the engine's, and objectives.c's goals)
    // visit(objective, data) for every objective while it answers non-zero.
    inline void forEachObjective(uint32_t visit, ee::Address data) { zge::forEachObjectiveJump(visit, data); }
    // A character's record of one level (profile/players.h's LevelGoals)
    // has a byte of marks for each of the level's objectives, by its place
    // among them:
    constexpr uint8_t kGoalSaved = 0x01;  // the objective is a goal
    constexpr uint8_t kGoalGap = 0x02;    // a gap
    constexpr uint8_t kGoalActive = 0x04; // it was activated
    constexpr uint8_t kGoalDone = 0x08;   // completed
    constexpr uint8_t kGoalDoneThisTurn = 0x10;
    // A reward of the game's reward tables.
    struct Reward
    {
        uint16_t kind;  // +0x00 0: none
        uint16_t value; // +0x02
    };
    static_assert(sizeof(Reward) == 0x04);
    // A reward's text for a player written to `out`; 0 when it has none.
    inline uint32_t rewardText(uint32_t player, ee::Ptr<Reward> reward, ee::Address out)
    {
        return ee::game<uint32_t>(sym::rewardText, player, reward, out);
    }
    // The objective a name hash names (the objectives' goalNameHash gives a
    // player's goal's).
    inline ee::Ptr<zge::Objective> objectiveByName(uint32_t name) { return zge::objectiveByName(name); }
    inline ee::Global<uint8_t, sym::g_currentPlayer> g_currentPlayer;
    // The game's own strings (the level's are set while a screen reads them).
    inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_gameStrings> g_gameStrings;
    inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_levelStrings> g_levelStrings;
    // A sound event with its first two words, and what it answers.
    // A sound event with nothing more said of it.
}
