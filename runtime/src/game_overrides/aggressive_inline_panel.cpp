// Aggressive Inline (SLUS-20327): the bottom screen's snapshot read from AI's
// RAM (runtime/panel_snapshot.h), read-only, by EE address (the game's
// globals by AI's symbols: sym/game_overrides/aggressive_inline_panel.cpp.h,
// tools/override_symbols.py), every pointer checked against RAM before it is
// followed, so a level half loaded reads as no level. The fields are the game's own (game/aggressive: skater.h Player,
// hud.h, globals.h, script.h, timers.cpp). Installed by the override
// (aggressive_inline_slus20327.cpp) with the question of which level the
// host serves in the game's folder's place (the Estate in the Tutorial's,
// aggressive_inline_estate.h); ps2xPanelShot and the unit tests run the same
// capture on a RAM image.

#include "aggressive_inline_panel.h"
#include "sym/game_overrides/aggressive_inline_panel.cpp.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace aggressive_inline::panel_state
{
    using namespace panel;

    namespace
    {
        // The game's globals the capture reads are AI's symbols (sym::, the
        // build's table from the work database): g_gameMode, g_topState (the
        // top state: its level, the level's players), g_levelNumber with
        // levelFolders (a folder name a level), g_gameTypeByte, the objectives
        // (g_objectives, g_objectiveCount, g_objectivesLevel), the script's
        // variables (g_vmVariables, g_vmVariablesBytes), the scene objects by
        // name hash (g_levelObjectNames, a zge::SearchTree), g_visibleTimer and
        // the props' list (g_props, level/props.cpp).
        constexpr uint32_t kModePlay = 2u, kModePaused = 3u; // g_gameMode's (1: the front end)
        constexpr uint32_t kTopStateLevel = 0x10u;     // the top state's level
        constexpr uint32_t kLevelPlayer = 0x10u;       // the level's first player (skater.h Player)
        constexpr uint32_t kPlayerBytes = 0x930u;
        constexpr uint32_t kLevelFolderBytes = 0x20u;  // levelFolders: a level's folder name
        constexpr uint32_t kScoreRecordScore = 0x14u;  // the player's score record: the score

        constexpr uint32_t kObjectiveBytes = 0x2Cu;    // g_objectives' records
        constexpr uint32_t kObjectiveState = 0x0Cu;
        constexpr uint32_t kObjectiveName = 0x18u;        // offsets in the script's strings
        constexpr uint32_t kObjectiveDescription = 0x1Cu;
        constexpr uint32_t kPropPosition = 0x40u;        // the rest frame's translation
        constexpr uint32_t kPropKind = 0x58u;
        constexpr uint32_t kPropOwner = 0x60u;
        constexpr uint32_t kPropShown = 0x64u;
        constexpr uint32_t kPropNext = 0x7Cu;
        constexpr uint32_t kTimerTime = 0x08u;
        constexpr uint32_t kTimerRunning = 0x20u;

        constexpr uint32_t kPlayerPosition = 0x090u;
        constexpr uint32_t kPlayerForward = 0x0D0u;
        constexpr uint32_t kPlayerState = 0x074u;
        constexpr uint32_t kPlayerSpeed = 0x1E8u;
        constexpr uint32_t kPlayerSkaterIndex = 0x5E0u;
        constexpr uint32_t kPlayerLevelFlags = 0x5E8u;
        constexpr uint32_t kPlayerSpecialTricks = 0x5F0u;
        constexpr uint32_t kPlayerName = 0x628u;
        constexpr uint32_t kPlayerStats = 0x638u;        // 12 bytes a stat: +1 level, +4 points
        constexpr uint32_t kPlayerSpinsCcw = 0x8B0u;
        constexpr uint32_t kPlayerSpinsCw = 0x8B4u;
        constexpr uint32_t kPlayerHud = 0x8B8u;
        constexpr uint32_t kPlayerScoreRecord = 0x8C4u;  // its record of the score (kScoreRecordScore)
        constexpr uint32_t kPlayerPendingScore = 0x8C8u;
        constexpr uint32_t kPlayerMultiplier = 0x8CCu;
        constexpr uint32_t kPlayerPoints = 0x8D8u;
        constexpr uint32_t kPlayerCurrentObject = 0x8A8u;
        constexpr uint32_t kPlayerHasCurrentObject = 0x8ACu;

        constexpr uint32_t kHudCompletedTricks = 0x14u;  // hud::SkaterHud: the widget of the line's names
        constexpr uint32_t kNodeNextSibling = 0x00u;     // zgui::Node
        constexpr uint32_t kNodeFirstChild = 0x04u;
        constexpr uint32_t kNodeKind = 0x0Cu;
        constexpr uint32_t kNodeNameHash = 0x10u;
        constexpr uint32_t kNodeValue = 0x18u;
        constexpr uint16_t kVariableKind = 2u;
        constexpr uint32_t kTextHash = 0x00058AD4u;      // nameHash("Text")

        constexpr uint32_t kTreeLower = 0x00u, kTreeHigher = 0x04u, kTreeKey = 0x08u, kTreeRecord = 0x0Cu;
        constexpr uint32_t kRecordNode = 0x08u;          // the scene object's record
        constexpr uint32_t kNodePosition = 0x10u;        // the scene node

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
            uint32_t word(uint32_t address) const { return read<uint32_t>(address); }
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

        void readTree(const Ram &ram, Snapshot &out)
        {
            uint32_t stack[64];
            int top = 0;
            uint32_t node = ram.word(sym::g_levelObjectNames);
            uint32_t count = 0;
            uint32_t steps = 0;
            // In order, so `tracked` comes out sorted by hash.
            while ((node || top) && ++steps < kMaxTrackedObjects * 4u)
            {
                while (node && ram.ok(node, 16) && top < 64)
                {
                    stack[top++] = node;
                    node = ram.word(node + kTreeLower);
                }
                if (!top)
                    break;
                node = stack[--top];
                const uint32_t record = ram.word(node + kTreeRecord);
                const uint32_t sceneNode = ram.ok(record, 16) ? ram.word(record + kRecordNode) : 0u;
                if (ram.ok(sceneNode, 0x20) && count < kMaxTrackedObjects)
                {
                    TrackedObject &object = out.tracked[count++];
                    object.nameHash = ram.word(node + kTreeKey);
                    object.x = ram.read<float>(sceneNode + kNodePosition);
                    object.y = ram.read<float>(sceneNode + kNodePosition + 4);
                    object.z = ram.read<float>(sceneNode + kNodePosition + 8);
                }
                node = ram.word(node + kTreeHigher);
            }
            out.trackedCount = count;
        }

        void readProps(const Ram &ram, Snapshot &out)
        {
            uint32_t count = 0;
            uint32_t prop = ram.word(sym::g_props);
            for (uint32_t steps = 0; steps < 2048u && ram.ok(prop, 0x80); ++steps, prop = ram.word(prop + kPropNext))
            {
                if (count >= kMaxCollectables)
                    break;
                const uint32_t kind = ram.word(prop + kPropKind);
                if (kind > 255u)
                    continue;
                Collectable &c = out.collectables[count++];
                c.kind = static_cast<uint8_t>(kind);
                c.shown = ram.word(prop + kPropShown) != 0u;
                c.x = ram.read<float>(prop + kPropPosition);
                c.z = ram.read<float>(prop + kPropPosition + 8);
                c.ownerHash = ram.word(prop + kPropOwner);
            }
            out.collectableCount = count;
        }

        // The HUD's text carries its colours ("#RGB{176 144 24}Tweaked ").
        void stripColours(char *text)
        {
            char *to = text;
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
                *to++ = *from++;
            }
            *to = 0;
        }

        void readLine(const Ram &ram, uint32_t player, Snapshot &out)
        {
            out.line[0] = 0;
            const uint32_t hud = ram.word(player + kPlayerHud);
            const uint32_t widget = ram.ok(hud, 0x18) ? ram.word(hud + kHudCompletedTricks) : 0u;
            uint32_t child = ram.ok(widget, 0x20) ? ram.word(widget + kNodeFirstChild) : 0u;
            for (int i = 0; i < 64 && ram.ok(child, 0x20); ++i, child = ram.word(child + kNodeNextSibling))
            {
                if ((ram.read<uint16_t>(child + kNodeKind) & 0x3FFFu) == kVariableKind && ram.word(child + kNodeNameHash) == kTextHash)
                {
                    const uint32_t value = ram.word(child + kNodeValue);
                    if (ram.ok(value))
                    {
                        ram.text(ram.word(value), out.line, sizeof(out.line));
                        stripColours(out.line);
                        // Before the first trick the widget's text is a space.
                        size_t length = std::strlen(out.line);
                        while (length > 0 && out.line[length - 1] == ' ')
                            out.line[--length] = 0;
                    }
                    return;
                }
            }
        }
    }

    namespace
    {
        const char *(*g_servedLevel)() = nullptr;
    }

    void capture(const uint8_t *guestRam, size_t ramBytes, Snapshot &out, const char *servedLevel)
    {
        const Ram ram(guestRam, ramBytes);
        ++out.sequence;
        out.mode = GameMode::NoLevel;
        out.level = -1;
        out.levelFile[0] = 0;
        out.objectiveCount = 0;
        out.activeGoal = -1;
        out.goalStatus[0] = 0;
        out.goalTargetCount = 0;
        out.scriptWordCount = 0;
        out.scriptVariableBytes = 0;
        out.trackedCount = 0;
        out.collectableCount = 0;

        const uint32_t mode = ram.word(sym::g_gameMode);
        const uint32_t top = ram.word(sym::g_topState);
        const uint32_t level = ram.ok(top, kTopStateLevel + 4) ? ram.word(top + kTopStateLevel) : 0u;
        const uint32_t player = level + kLevelPlayer;
        if ((mode != kModePlay && mode != kModePaused) || !ram.ok(player, kPlayerBytes))
            return;
        const float y = ram.read<float>(player + kPlayerPosition + 4);
        if (y == 20000.0f) // not placed yet
            return;

        out.mode = mode == kModePlay ? GameMode::Play : GameMode::Paused;
        out.level = ram.read<uint8_t>(sym::g_levelNumber);
        if (servedLevel && servedLevel[0])
            std::snprintf(out.levelFile, sizeof(out.levelFile), "%s", servedLevel);
        else
            ram.text(sym::levelFolders + static_cast<uint32_t>(out.level) * kLevelFolderBytes, out.levelFile, sizeof(out.levelFile));
        out.gameType = ram.read<uint8_t>(sym::g_gameTypeByte);
        out.skaterState = ram.read<uint8_t>(player + kPlayerState);
        out.skaterIndex = ram.read<uint8_t>(player + kPlayerSkaterIndex) & 0x1Fu;
        ram.text(player + kPlayerName, out.skaterName, sizeof(out.skaterName));
        out.x = ram.read<float>(player + kPlayerPosition);
        out.y = y;
        out.z = ram.read<float>(player + kPlayerPosition + 8);
        out.forwardX = ram.read<float>(player + kPlayerForward);
        out.forwardZ = ram.read<float>(player + kPlayerForward + 8);
        out.speed = ram.read<float>(player + kPlayerSpeed);

        const uint32_t scoreRecord = ram.word(player + kPlayerScoreRecord);
        out.score = ram.ok(scoreRecord, kScoreRecordScore + 4) ? ram.read<int32_t>(scoreRecord + kScoreRecordScore) : 0;
        out.pendingScore = ram.read<float>(player + kPlayerPendingScore);
        out.multiplier = ram.read<int32_t>(player + kPlayerMultiplier);
        out.points = ram.read<int32_t>(player + kPlayerPoints);
        out.spinsCounterClockwise = ram.read<int32_t>(player + kPlayerSpinsCcw);
        out.spinsClockwise = ram.read<int32_t>(player + kPlayerSpinsCw);
        out.levelFlags = ram.read<uint64_t>(player + kPlayerLevelFlags);
        out.specialTricks = ram.read<uint16_t>(player + kPlayerSpecialTricks);
        for (uint32_t i = 0; i < kStatCount; ++i)
        {
            out.statLevel[i] = ram.read<uint8_t>(player + kPlayerStats + i * 12u + 1u);
            out.statPoints[i] = ram.read<int32_t>(player + kPlayerStats + i * 12u + 4u);
        }
        (void)kPlayerCurrentObject;
        (void)kPlayerHasCurrentObject;

        const uint32_t timer = ram.word(sym::g_visibleTimer);
        out.timerSeconds = (ram.ok(timer, 0x24) && ram.word(timer + kTimerRunning) == 1u) ? ram.read<float>(timer + kTimerTime) : -1.0f;

        readLine(ram, player, out);

        if (ram.word(sym::g_objectivesLevel) == static_cast<uint32_t>(out.level))
        {
            const uint32_t records = ram.word(sym::g_objectives);
            const uint32_t count = std::min(ram.word(sym::g_objectiveCount), kMaxObjectives);
            if (ram.ok(records, count * kObjectiveBytes))
            {
                out.objectiveCount = count;
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t record = records + i * kObjectiveBytes;
                    out.objectiveState[i] = ram.word(record + kObjectiveState);
                    out.objectiveText[i][0] = ram.word(record + kObjectiveName);
                    out.objectiveText[i][1] = ram.word(record + kObjectiveDescription);
                }
            }
        }

        const uint32_t variables = ram.word(sym::g_vmVariables);
        const uint32_t words = std::min(ram.word(sym::g_vmVariablesBytes) / 4u, kMaxScriptWords);
        if (ram.ok(variables, words * 4u))
        {
            out.scriptVariableBytes = ram.word(sym::g_vmVariablesBytes);
            std::memcpy(out.scriptWords, guestRam + variables, words * 4u);
            out.scriptWordCount = words;
        }

        readTree(ram, out);
        readProps(ram, out);
    }

    void capture(const uint8_t *guestRam, size_t ramBytes, Snapshot &out)
    {
        capture(guestRam, ramBytes, out, g_servedLevel ? g_servedLevel() : nullptr);
    }

    void install(const char *(*servedLevel)())
    {
        g_servedLevel = servedLevel;
        panel::setGameCapture(&capture);
    }
}
