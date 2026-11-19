// Dave Mirra Freestyle BMX 2 (SLUS-20159): the Thor's bottom screen
// (its snapshot: runtime/panel_snapshot.h), its state read from DM2's RAM: which level, the
// rider's place, heading, speed and score, whether play is paused. The
// panel's files for DM2's levels are panel_map.py's (`build --game dm2`: each
// level's top-down map, its ProQuest goals and texts, DM2's fonts and trick
// list; the Xbox levels' values give no level). In a ProQuest level the
// goals' states come from the player's character record (game/dm2/game/
// players.h LevelGoals of the session's level): the level file lists twenty
// goals, five challenges of four (those a challenge has not hidden), each
// done, open (the current challenge's, revealed) or not offered yet, its
// objectives met as the goal's count, each objective met (state 4 on) in a
// script word of its own after the counts (the goal's targets on the map,
// panel_map.py's places of its objectives, are done by it). The trick list's rows are listed and
// dimmed as the pause menu's trick list does it, from the character's moves
// it cannot do and those it has landed. Read-only and
// bounded, as Aggressive Inline's capture: a level half loaded reads as no
// level. The Xbox levels (values 9 on, in Woodward Camp's slot:
// dave_mirra_xboxlevel.cpp) are their folders' files, in levels.txt's order
// (setXboxLevels); their goals' states are the slot's, which holds theirs
// while they run (dave_mirra_xcareer.cpp). Installed by the override
// (dave_mirra.cpp); ps2xPanelShot --game dm2 runs the same capture on a RAM
// dump.

#include "runtime/panel_snapshot.h"
#include "sym/game_overrides/dave_mirra_panel.cpp.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace dave_mirra::panel_state
{
    namespace
    {
        // The rider record (game/dm2/rider/rider.h Rider), as far as the panel reads it.
        constexpr uint32_t kRiderTurnScore = 0x1A0;  // score.turnScore (a float)
        constexpr uint32_t kRiderForward = 0x1020;   // forward row
        constexpr uint32_t kRiderPosition = 0x1040;  // the physics position (y grows down)
        constexpr uint32_t kRiderSpeed = 0x10D8;     // |velocity|, units a frame
        constexpr uint32_t kRiderBytes = 0x1700;

        // What each player picked (game/dm2/game/players.h PlayerSetup): the
        // character record it rides.
        constexpr uint32_t kSetupBytes = 0x11;
        constexpr uint32_t kSetupCharacter = 0x03;
        // A character's record (players.h RiderCharacter): its goals of each
        // of the nine levels, the moves it cannot do and those it has landed.
        constexpr uint32_t kCharacterBytes = 0x3C8;
        constexpr uint32_t kCharacterCount = 21;
        constexpr uint32_t kCharacterLevels = 0x13C;
        constexpr uint32_t kCharacterMovesOff = 0x3A0;
        constexpr uint32_t kCharacterMovesLanded = 0x3B4;
        constexpr uint32_t kCharacterMoveBytes = 0x14; // each: a bit a move, 0x48 tricks then 0x56 modifiers
        static_assert(kCharacterMoveBytes <= panel::kMoveBitBytes);
        // A level's goals (players.h LevelGoals): five challenges of four
        // goals of three bytes (flags first), then which challenge is next.
        constexpr uint32_t kLevelGoalsBytes = 0x44;
        constexpr uint32_t kLevelGoalsChallenge = 0x3D;
        constexpr uint32_t kChallenges = 5, kChallengeGoals = 4, kGoalBytes = 3, kChallengeBytes = 0x0C;
        constexpr uint32_t kGameLevels = 8; // with goals (8: the park editor's park)
        constexpr uint8_t kGoalRevealed = 0x80, kGoalDone = 0x40, kGoalObjectivesMet = 0x1F;
        // A goal's objectives (game/dm2/game/goals.cpp objectiveState): four
        // bits each in the goal's two bytes after its flags, the state in the
        // low three; 4 and on: met.
        constexpr uint32_t kGoalObjectives = 4;
        constexpr uint8_t kObjectiveStateMask = 0x7, kObjectiveStateMet = 4;
        // The panel's script words: the goals' counts, then each goal's
        // objectives met (panel_map.py DM2_OBJECTIVE_MET_WORDS).
        constexpr uint32_t kObjectiveMetWords = kChallenges * kChallengeGoals;
        // game/dm2/game/round.h SinglePlayerMode: a ProQuest level's goals.
        constexpr uint8_t kLevelGoalsMode = 0;

        // game/dm2/action/action.h, overlay/overlay.h (dave_mirra_photo.cpp's).
        constexpr uint8_t kGameStateLevel = 1;
        constexpr uint8_t kActionScreen = 1;
        constexpr uint8_t kScreenPause = 1;

        // The level select's folders by menu value (g_currentLevel, Root.CurrentLevel;
        // panel_map.py DM2_LEVELS); 8 is the park editor's park.
        constexpr const char *kLevelFolders[] = {"WOODWARD", "TRAINYAR", "SWAMPYTR", "COMMERCI",
                                                 "GALLOON",  "CLOVERLE", "DEVILSPE", "AIRPORT"};
        constexpr int32_t kLevelCount = static_cast<int32_t>(sizeof(kLevelFolders) / sizeof(kLevelFolders[0]));
        // The Xbox levels' values (dave_mirra_xboxlevel.cpp) and folders.
        constexpr int32_t kFirstXboxLevel = 9;
        constexpr size_t kXboxLevelsMost = 8, kFolderBytes = 16;
        char g_xboxFolders[kXboxLevelsMost][kFolderBytes] = {};
        int32_t g_xboxLevelCount = 0;

        const char *levelFolder(int32_t level)
        {
            if (level >= 0 && level < kLevelCount)
                return kLevelFolders[level];
            if (level >= kFirstXboxLevel && level < kFirstXboxLevel + g_xboxLevelCount)
                return g_xboxFolders[level - kFirstXboxLevel];
            return nullptr;
        }

        // RAM below 1 MB is the EE kernel's: no game data there.
        constexpr uint32_t kGameRamStart = 1u << 20;

        class Ram
        {
        public:
            Ram(const uint8_t *bytes, size_t size) : bytes_(bytes), size_(size) {}
            bool ok(uint32_t address, uint32_t length = 4) const
            {
                return address >= kGameRamStart && address + length <= size_ && address + length > address;
            }
            template <typename T> T read(uint32_t address) const
            {
                T value{};
                if (ok(address, sizeof(T)))
                    std::memcpy(&value, bytes_ + address, sizeof(T));
                return value;
            }

        private:
            const uint8_t *bytes_;
            size_t size_;
        };
    }

    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out)
    {
        const Ram ram(guestRam, ramBytes);
        ++out.sequence;
        out.mode = panel::GameMode::NoLevel;
        out.level = -1;
        out.levelFile[0] = 0;
        out.objectiveCount = 0;
        out.activeGoal = -1;
        out.goalStatus[0] = 0;
        out.goalTargetCount = 0;
        out.hasMoves = false;
        out.scriptWordCount = 0;
        out.trackedCount = 0;
        out.collectableCount = 0;
        out.line[0] = 0;
        out.currentTrick[0] = 0;

        if (ram.read<uint8_t>(sym::dm2::g_currentGameState) != kGameStateLevel ||
            ram.read<uint32_t>(sym::dm2::g_physicsTaskFrames) == 0u)
            return;
        const uint32_t rider = ram.read<uint32_t>(sym::dm2::g_riders); // player 1's
        const int32_t level = ram.read<int32_t>(sym::dm2::g_currentLevel);
        const char *folder = levelFolder(level);
        if (!ram.ok(rider, kRiderBytes) || !folder)
            return;

        const bool paused = ram.read<uint8_t>(sym::dm2::g_actionMode) == kActionScreen &&
                            ram.read<uint8_t>(sym::dm2::g_gameScreen) == kScreenPause;
        out.mode = paused ? panel::GameMode::Paused : panel::GameMode::Play;
        out.level = level;
        std::snprintf(out.levelFile, sizeof(out.levelFile), "%s", folder);
        out.x = ram.read<float>(rider + kRiderPosition);
        out.y = ram.read<float>(rider + kRiderPosition + 4);
        out.z = ram.read<float>(rider + kRiderPosition + 8);
        out.forwardX = ram.read<float>(rider + kRiderForward);
        out.forwardZ = ram.read<float>(rider + kRiderForward + 8);
        out.speed = ram.read<float>(rider + kRiderSpeed);
        out.score = static_cast<int32_t>(ram.read<float>(rider + kRiderTurnScore));
        out.pendingScore = 0.0f;
        out.timerSeconds = -1.0f;

        // The character player 1 rides: its moves, and in a ProQuest level its goals there.
        const uint8_t index = ram.read<uint8_t>(sym::dm2::g_playerSetups + kSetupCharacter);
        const uint32_t character = sym::dm2::g_riderCharacters + index * kCharacterBytes;
        if (index >= kCharacterCount || !ram.ok(character, kCharacterBytes))
            return;
        for (uint32_t i = 0; i < panel::kMoveBitBytes; ++i)
        {
            out.movesOff[i] = i < kCharacterMoveBytes ? ram.read<uint8_t>(character + kCharacterMovesOff + i) : 0;
            out.movesLanded[i] = i < kCharacterMoveBytes ? ram.read<uint8_t>(character + kCharacterMovesLanded + i) : 0;
        }
        out.hasMoves = true;
        const uint8_t gameLevel = ram.read<uint8_t>(sym::dm2::g_sessionLevel);
        if (ram.read<uint8_t>(sym::dm2::g_sessionSinglePlayerMode) != kLevelGoalsMode || gameLevel >= kGameLevels)
            return;
        const uint32_t goals = character + kCharacterLevels + gameLevel * kLevelGoalsBytes;
        const uint8_t current = ram.read<uint8_t>(goals + kLevelGoalsChallenge);
        for (uint32_t c = 0; c < kChallenges; ++c)
            for (uint32_t g = 0; g < kChallengeGoals; ++g)
            {
                const uint32_t slot = c * kChallengeGoals + g;
                const uint8_t flags = ram.read<uint8_t>(goals + c * kChallengeBytes + g * kGoalBytes);
                out.objectiveState[slot] = (flags & kGoalDone)                          ? panel::kStateComplete
                                           : c == current && (flags & kGoalRevealed) ? panel::kStateOpen
                                                                                     : panel::kStateInit;
                out.scriptWords[slot] = flags & kGoalObjectivesMet;
                for (uint32_t o = 0; o < kGoalObjectives; ++o)
                {
                    const uint8_t part = ram.read<uint8_t>(goals + c * kChallengeBytes + g * kGoalBytes + 1 + o / 2);
                    const uint32_t state = (part >> ((o & 1u) * 4u)) & kObjectiveStateMask;
                    out.scriptWords[kObjectiveMetWords + slot * kGoalObjectives + o] = state >= kObjectiveStateMet ? 1u : 0u;
                }
            }
        out.objectiveCount = kChallenges * kChallengeGoals;
        out.scriptWordCount = kObjectiveMetWords + kChallenges * kChallengeGoals * kGoalObjectives;
    }

    void setXboxLevels(const char *const *folders, size_t count)
    {
        g_xboxLevelCount = 0;
        for (size_t i = 0; i < count && i < kXboxLevelsMost; ++i)
        {
            std::snprintf(g_xboxFolders[i], kFolderBytes, "%s", folders[i]);
            ++g_xboxLevelCount;
        }
    }

    void install()
    {
        panel::setGameCapture(&capture);
    }
}
