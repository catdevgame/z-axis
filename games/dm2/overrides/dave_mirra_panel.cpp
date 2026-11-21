// Dave Mirra Freestyle BMX 2 (SLUS-20159): the Thor's bottom screen
// (its snapshot: runtime/panel_snapshot.h), its state read from DM2's RAM: which level, the
// rider's place, heading, speed and score, whether play is paused. The
// panel's files for DM2's levels are panel_map.py's (`build --game dm2`: each
// level's top-down map, its ProQuest goals and texts, DM2's fonts and trick
// list; the Xbox levels' values give no level). In a ProQuest level the
// goals' states come from the player's character record (games/dm2/game/
// players.h LevelGoals of the session's level): the level file lists twenty
// goals, five challenges of four (those a challenge has not hidden), each
// done, open (the current challenge's, revealed) or not offered yet, its
// objectives met as the goal's count, each objective met (state 4 on) in a
// script word of its own after the counts (the goal's targets on the map,
// panel_map.py's places of its objectives, are done by it). The trick list's rows are listed and
// dimmed as the pause menu's trick list does it, from the character's moves
// it cannot do and those it has landed.
//
// What the game's HUD shows, as it shows it: the turn's clock (the timer
// frame, games/dm2/overlay/frame_timer.cpp: the phase's frames left while
// the turn runs in a timed game, in the seconds its labels write); the run's
// trick lines (frame_trick.cpp: the four name labels the lines go through in
// turn, newest last, their colours taken out, and the points under them,
// games/dm2/game/score.h); the goal the objective label shows
// (frame_labels.cpp g_objectiveState: the goal whose box the rider is in) as
// the active goal. DM2 writes no status line for a goal: its label is the
// goal's text, and the panel counts its objectives met. A goal the next of
// the current challenge an NPC gives (games/dm2/game/npcs.h: npcWaitingFrame's
// rule, its range's first goal not done) and not given yet can be started:
// its giver is that NPC's rider, where it rides now, a tracked object named
// by the goal's text key (panel_maps_dm2.cpp's giver records, by the same
// name). Read-only and
// bounded, as Aggressive Inline's capture: a level half loaded reads as no
// level. The Xbox levels (values 9 on, in Woodward Camp's slot:
// dave_mirra_xboxlevel.cpp) are their folders' files, in levels.txt's order
// (setXboxLevels); their goals' states are the slot's, which holds theirs
// while they run (dave_mirra_xcareer.cpp). Installed by the override
// (dave_mirra.cpp); zx-panelshot --game dm2 runs the same capture on a RAM
// dump.

#include "runtime/panel_snapshot.h"
#include "sym/game_overrides/dave_mirra_panel.cpp.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dave_mirra::panel_state
{
    namespace
    {
        // The rider record (games/dm2/rider/rider.h Rider), as far as the panel reads it.
        constexpr uint32_t kRiderScore = 0x1A0;      // its ScoreBlock (rider.h)
        constexpr uint32_t kRiderTurnScore = kRiderScore + 0x000; // score.turnScore (a float)
        constexpr uint32_t kRiderLineCount = kRiderScore + 0x69D;  // score.lineCount: the run's lines
        constexpr uint32_t kRiderForward = 0x1020;   // forward row
        constexpr uint32_t kRiderPosition = 0x1040;  // the physics position (y grows down)
        constexpr uint32_t kRiderSpeed = 0x10D8;     // |velocity|, units a frame
        constexpr uint32_t kRiderBytes = 0x1700;

        // What each player picked (games/dm2/game/players.h PlayerSetup): the
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
        // A goal's objectives (games/dm2/game/goals.cpp objectiveState): four
        // bits each in the goal's two bytes after its flags, the state in the
        // low three; 4 and on: met.
        constexpr uint32_t kGoalObjectives = 4;
        constexpr uint8_t kObjectiveStateMask = 0x7, kObjectiveStateMet = 4;
        // The panel's script words: the goals' counts, then each goal's
        // objectives met (panel_map.py DM2_OBJECTIVE_MET_WORDS).
        constexpr uint32_t kObjectiveMetWords = kChallenges * kChallengeGoals;
        // games/dm2/game/round.h SinglePlayerMode: a ProQuest level's goals.
        constexpr uint8_t kLevelGoalsMode = 0;

        // games/dm2/action/action.h, overlay/overlay.h (dave_mirra_photo.cpp's).
        constexpr uint8_t kGameStateLevel = 1;
        constexpr uint8_t kActionScreen = 1;
        constexpr uint8_t kScreenPause = 1;

        // games/dm2/game/round.h: the modes (multiplayerMode 0 is a
        // single-player game; a free ride and a park test have no clock), the
        // round's phase the clock runs in.
        constexpr uint8_t kSinglePlayer = 0, kFreeRide = 3, kParkTest = 4;
        constexpr uint8_t kTurnRunningPhase = 5;
        constexpr uint32_t kFramesPerSecond = 60;

        // The trick display's widgets (frame_trick.cpp TrickDisplayWidgets,
        // g_trickDisplayWidgets): its four name labels, its score label, the
        // display (0: the layout has none); g_trickNameLabel the label of the
        // run's current line (each new line takes the next label).
        constexpr uint32_t kTrickNameLabels = 4;
        constexpr uint32_t kTrickDisplayNames = 0x00, kTrickDisplayScore = 0x10, kTrickDisplay = 0x14;
        // DM2's zgui (ZGE2) makes a widget a scene object (engine/zge/zgui/
        // widget.h: its kind 4 in the low four bits of its flags) whose record
        // (WidgetRecord) is at the object's +0x3C; a label's (widget_parts.h
        // ListedLabel) keeps its text at +0x64, what its "Text" sets.
        constexpr uint32_t kWidgetObjectBytes = 0x40, kWidgetObjectRecord = 0x3C;
        constexpr uint32_t kObjectKindMask = 0xF, kWidgetObjectKind = 4;
        constexpr uint32_t kListedLabelBytes = 0x7C, kListedLabelText = 0x64;

        // The objective label's state (overlay/overlay.h LabelState,
        // g_objectiveState): the goal shown (challenge * 4 + goal;
        // kOverlayNone: none), and whether its label is up (as the goals'
        // frame last read it from the label's widget).
        constexpr uint32_t kLabelValue = 0x00, kLabelVisible = 0x02;
        constexpr uint8_t kOverlayNone = 0xFF, kLabelValueBits = 0x7F;

        // The level's NPCs (games/dm2/game/npcs.h Npc, g_npcs; a rider of
        // kNoNpc ends the list) and the definition each rides by
        // (NpcDefinition: the challenge whose goals it gives, from which goal
        // to which).
        constexpr uint32_t kNpcs = 4, kNpcBytes = 0x30, kNpcRider = 0x00, kNpcDefinition = 0x28;
        constexpr uint8_t kNoNpc = 0xFF;
        constexpr uint32_t kNpcDefinitionBytes = 0x14;
        constexpr uint32_t kDefinitionChallenge = 0x10, kDefinitionFirstGoal = 0x11, kDefinitionLastGoal = 0x12;
        // A goal's text key (overlay/frame_labels.cpp, the objective label's:
        // "KSTRSP_GOAL_LEV%02d_ROOKIE", "KSTRSP_GOAL_LEV%02d_%s_%d" with the
        // challenge's word), which names its giver.
        constexpr const char *kChallengeWords[kChallenges] = {"ROOKIE", "EASY", "HARD", "XTRA", "EXPERT"};

        // The engine's name hash (zge/text/strings.h nameHash): the ELF hash
        // with ASCII letters upper-cased.
        constexpr uint32_t nameHash(const char *name)
        {
            uint32_t hash = 0;
            for (const char *c = name; *c; ++c)
            {
                uint32_t letter = static_cast<uint8_t>(*c);
                if (letter >= 'a' && letter <= 'z')
                    letter -= 'a' - 'A';
                hash = (hash << 4) + letter;
                const uint32_t high = hash & 0xF0000000u;
                if (high)
                    hash ^= high >> 24;
                hash &= ~high;
            }
            return hash;
        }
        static_assert(nameHash("Text") == 0x00058AD4u); // (as the other games' captures have it)

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
            // A zero-terminated text, as much of it as fits `bytes` and RAM.
            void text(uint32_t address, char *out, size_t bytes) const
            {
                size_t i = 0;
                for (; i + 1 < bytes && ok(address + i, 1); ++i)
                {
                    out[i] = static_cast<char>(bytes_[address + i]);
                    if (!out[i])
                        break;
                }
                out[i] = 0;
            }

        private:
            const uint8_t *bytes_;
            size_t size_;
        };

        // The clock as the timer frame writes it (overlayTimerMessage): 59
        // frames are added, then whole minutes and whole seconds are taken
        // while more than one is left ("1:00" from 3,542 frames, "0:59" at
        // 3,541, "0:00" at the last frame); in seconds.
        float clockSeconds(uint32_t framesLeft)
        {
            // (its loops take whole minutes, then whole seconds, while more than one is left)
            const uint32_t frames = framesLeft + (kFramesPerSecond - 1);
            const uint32_t minutes = (frames - 1) / (60 * kFramesPerSecond);
            const uint32_t seconds = (frames - minutes * 60 * kFramesPerSecond - 1) / kFramesPerSecond;
            return static_cast<float>(minutes * 60 + seconds);
        }

        // A label widget's text, or "".
        void labelText(const Ram &ram, uint32_t label, char *out, size_t bytes)
        {
            out[0] = 0;
            if (!ram.ok(label, kWidgetObjectBytes) || (ram.read<uint32_t>(label) & kObjectKindMask) != kWidgetObjectKind)
                return;
            const uint32_t record = ram.read<uint32_t>(label + kWidgetObjectRecord);
            if (ram.ok(record, kListedLabelBytes))
                ram.text(ram.read<uint32_t>(record + kListedLabelText), out, bytes);
        }

        // A label's text as it reads: its colours ("#RGB{192 192 192}TO")
        // and line breaks taken out, its words one space apart.
        void plainText(char *text)
        {
            char *to = text;
            bool space = true; // (no space at the start)
            for (const char *from = text; *from;)
            {
                if (std::strncmp(from, "#RGB{", 5) == 0)
                {
                    const char *end = std::strchr(from, '}');
                    if (!end)
                        break;
                    from = end + 1;
                    continue;
                }
                const char c = *from++;
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
                {
                    if (!space)
                        *to++ = ' ';
                    space = true;
                    continue;
                }
                *to++ = c;
                space = false;
            }
            if (to > text && to[-1] == ' ')
                --to;
            *to = 0;
        }

        // The run's trick lines as the trick display has them: each line's
        // text is in the name label it took (the current line's is
        // g_trickNameLabel's, the one before it in the label before), so the
        // last four lines are there, the oldest first. The current line's
        // text alone is the current trick; the score label's "%ld points" (""
        // where the game shows no points: gameShowsLinePoints) the pending
        // score.
        void readLine(const Ram &ram, uint32_t rider, panel::Snapshot &out)
        {
            const uint32_t lines = ram.read<uint8_t>(rider + kRiderLineCount);
            const uint32_t widgets = sym::dm2::g_trickDisplayWidgets;
            if (lines == 0 || !ram.ok(ram.read<uint32_t>(widgets + kTrickDisplay), kWidgetObjectBytes))
                return;
            const uint32_t current = ram.read<uint32_t>(sym::dm2::g_trickNameLabel) % kTrickNameLabels;
            const uint32_t shown = std::min(lines, kTrickNameLabels);
            size_t length = 0;
            for (uint32_t back = shown; back-- > 0;)
            {
                char text[panel::kLineBytes];
                const uint32_t label = (current + kTrickNameLabels - back) % kTrickNameLabels;
                labelText(ram, ram.read<uint32_t>(widgets + kTrickDisplayNames + label * 4), text, sizeof(text));
                plainText(text);
                if (!text[0])
                    continue;
                length += static_cast<size_t>(std::snprintf(out.line + length, sizeof(out.line) - length, "%s%s", length ? " " : "", text));
                length = std::min(length, sizeof(out.line) - 1);
                if (back == 0)
                    std::snprintf(out.currentTrick, sizeof(out.currentTrick), "%s", text);
            }
            char points[32];
            labelText(ram, ram.read<uint32_t>(widgets + kTrickDisplayScore), points, sizeof(points));
            out.pendingScore = static_cast<float>(std::strtoul(points, nullptr, 10));
        }

        // The tracked objects sorted by name, as the panel looks them up.
        void sortTracked(panel::Snapshot &out)
        {
            std::sort(out.tracked, out.tracked + out.trackedCount,
                      [](const panel::TrackedObject &a, const panel::TrackedObject &b) { return a.nameHash < b.nameHash; });
        }
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

        // The clock, while the timer frame shows it (overlayTimerShow: the
        // turn running, in a game with a clock); the run's lines and their
        // points under them.
        const uint8_t multiplayer = ram.read<uint8_t>(sym::dm2::g_multiplayerMode);
        const uint8_t single = ram.read<uint8_t>(sym::dm2::g_singlePlayerMode);
        const bool untimed = multiplayer == kSinglePlayer && single >= kFreeRide && single <= kParkTest;
        out.timerSeconds = ram.read<uint8_t>(sym::dm2::g_roundPhase) == kTurnRunningPhase && !untimed
                               ? clockSeconds(ram.read<uint32_t>(sym::dm2::g_phaseFramesLeft))
                               : -1.0f;
        out.pendingScore = 0.0f;
        out.multiplier = 0;
        readLine(ram, rider, out);

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

        // The goal the objective label shows, while it is up.
        const uint8_t labelled = ram.read<uint8_t>(sym::dm2::g_objectiveState + kLabelValue);
        if (labelled != kOverlayNone && ram.read<uint16_t>(sym::dm2::g_objectiveState + kLabelVisible) != 0 &&
            (labelled & kLabelValueBits) < out.objectiveCount)
            out.activeGoal = labelled & kLabelValueBits;

        // The goals the NPCs give: of the current challenge, the first of an
        // NPC's range not done (what it offers whenever it stands); one not
        // given yet can be started, at the NPC's rider.
        if (current >= kChallenges)
            return;
        for (uint32_t npc = 0; npc < kNpcs; ++npc)
        {
            const uint32_t record = sym::dm2::g_npcs + npc * kNpcBytes;
            const uint8_t riderIndex = ram.read<uint8_t>(record + kNpcRider);
            if (riderIndex == kNoNpc)
                break;
            const uint32_t definition = ram.read<uint32_t>(record + kNpcDefinition);
            const uint32_t npcRider = ram.read<uint32_t>(sym::dm2::g_riders + riderIndex * 4u);
            if (!ram.ok(definition, kNpcDefinitionBytes) || !ram.ok(npcRider, kRiderBytes) ||
                ram.read<uint8_t>(definition + kDefinitionChallenge) != current)
                continue;
            uint32_t goal = ram.read<uint8_t>(definition + kDefinitionFirstGoal);
            const uint32_t last = std::min<uint32_t>(ram.read<uint8_t>(definition + kDefinitionLastGoal), kChallengeGoals - 1);
            while (goal <= last && out.objectiveState[current * kChallengeGoals + goal] == panel::kStateComplete)
                ++goal;
            const uint32_t slot = current * kChallengeGoals + goal;
            if (goal > last || out.objectiveState[slot] != panel::kStateInit || out.trackedCount >= panel::kMaxTrackedObjects)
                continue;
            out.objectiveState[slot] = panel::kStateReady;
            char key[48];
            if (current == 0)
                std::snprintf(key, sizeof(key), "KSTRSP_GOAL_LEV%02u_%s", static_cast<unsigned>(gameLevel), kChallengeWords[0]);
            else
                std::snprintf(key, sizeof(key), "KSTRSP_GOAL_LEV%02u_%s_%u", static_cast<unsigned>(gameLevel), kChallengeWords[current], goal);
            panel::TrackedObject &giver = out.tracked[out.trackedCount++];
            giver.nameHash = nameHash(key);
            giver.x = ram.read<float>(npcRider + kRiderPosition);
            giver.y = ram.read<float>(npcRider + kRiderPosition + 4);
            giver.z = ram.read<float>(npcRider + kRiderPosition + 8);
        }
        sortTracked(out);
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
