// BMX XXX (SLUS-20415): the Thor's bottom screen (its snapshot: runtime/panel_snapshot.h),
// its state read from BMX XXX's RAM: which level, the rider's place, heading,
// speed and score, whether play is paused, the level's challenges and the
// moves landed. The panel's files for BMX XXX's levels are the setup's
// (collection/extras/panel_maps_bmx.cpp, after panel_map.py's `build --game
// bmx`: each level's top-down map, its challenges and their
// texts from its OBJCTV.TXT, the givers from its DAT and the executable's
// NPC table, BMX XXX's fonts and
// trick list). The level is a row of the game's level table
// (games/bmx/profile/players.h LevelInfo, g_levels): the panel's file is
// named by the row's folder, read here from RAM.
//
// In a session with objectives (games/bmx/objectives: the challenge levels)
// the goals are the level's objectives as the game's CHALLENGES list has them
// (overlay/screen_objectives.cpp): the engine's objective pool walked the
// oldest first (the file's order, which is the panel file's), a set's child
// left out. Each goal's state is its objective's (engine/zge/scene/
// objectives.h Objective: complete, else running from active on, else ready
// when every prerequisite is complete, else locked; a REPEATABLE one taken
// again after its completion, which the game set back to ready, runs with
// its done mark kept: objectiveDoneMarks); its count and target (a
// script word pair a goal) are those its status line carries, the line
// found as the game finds it (statusCount: the first running member's
// line, else the objective's own "<STRINGID>_STATUS" format from the
// level's string table, with its type's two values);
// the goal the game's objective display shows the player (overlay/
// displays.cpp g_objectiveDisplay) is the active goal, and the display's
// widget's "Status" text is the status line as the game formats it. The
// active goal's and every running goal's targets are read live: the
// objective tree walked through its sets, a COLLECT member's items (scene
// objects, each with its collected bit and its level object's name, by
// which the panel file tells a checkpoint from a pickup), a PROXIMITY,
// TRANSFER or GRIND member's bounds; an ordered set's members past the
// current one come as later. A goal's giver is tracked where it is now, by
// the goal's name (the panel file's givers, from the DAT's people, and the
// level's NPC riders' challenges, which only the game places): a path man
// that walks a path with the objective, an NPC rider with its challenge.
//
// What the game's HUD shows, as it shows it: the trick display's lines
// (overlay/displays.cpp trickDisplayMessage: the current trick line's lines
// in the four name labels they go through in turn, newest last, their
// colours taken out) and the points under them; the challenge's clock.
// The trick list's rows are dimmed as the pause menu's trick list does it,
// from the saved game's moves landed (a bit a trick, the modifiers' from bit
// 100; games/bmx/ui/trick_list.cpp).
// Read-only and bounded, as DM2's capture (dave_mirra_panel.cpp): a level
// half loaded reads as no level. Installed by the override (bmx_xxx.cpp);
// zx-panelshot --game bmx runs the same capture on a RAM dump.

#include "runtime/panel_snapshot.h"
#include "sym/game_overrides/bmx_xxx_panel.cpp.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bmx_xxx::panel_state
{
    namespace
    {
        // The rider record (games/bmx/rider/rider.h Rider), as far as the panel reads it.
        constexpr uint32_t kRiderScore = 0x1C0;      // its ScoreBlock (rider.h)
        constexpr uint32_t kRiderTurnScore = kRiderScore + 0x0000;  // score.turnScore (a float)
        constexpr uint32_t kRiderLineCount = kRiderScore + 0x1A21;  // score.lineCount: the current trick line's lines
        constexpr uint32_t kRiderObjectives = 0x2588; // objectives.h RiderObjectives: before the wheels' contacts
        constexpr uint32_t kRiderTimedObjective = kRiderObjectives + 0x0C; // the objective with a time limit running (0: none)
        constexpr uint32_t kRiderTicksLeft = kRiderObjectives + 0x10;      // its time left, in frames
        constexpr uint32_t kRiderForward = 0x2630;   // forward row
        constexpr uint32_t kRiderPosition = 0x2650;  // the physics position (y grows down)
        constexpr uint32_t kRiderSpeed = 0x26F8;     // |velocity|, units a frame
        constexpr uint32_t kRiderReadBytes = 0x2700; // the record's first bytes, which hold what is read here
        constexpr float kFramesASecond = 60.0f;

        // The level table (players.h LevelInfo, g_levels): a row a level, its
        // folder's name at +4; the disc's levels are the first eight rows.
        constexpr uint32_t kLevelCount = 12;
        constexpr uint32_t kLevelInfoBytes = 0x10;
        constexpr uint32_t kLevelInfoName = 0x04;

        // What each player picked (games/bmx/profile/player_setup.h
        // PlayerSetup): the character record it rides.
        constexpr uint32_t kMaxPlayers = 2;
        constexpr uint32_t kSetupBytes = 0x25;
        constexpr uint32_t kSetupCharacter = 0x03;
        // The saved game (players.h SavedGame): the moves landed, then the
        // characters' records (RiderCharacter), each with its goals of the
        // twelve levels (LevelGoals: the marks at +6, one an objective).
        constexpr uint32_t kSavedMovesLanded = 0x07;
        constexpr uint32_t kSavedMovesBytes = 0x18;
        constexpr uint32_t kSavedCharacters = 0x24;
        constexpr uint32_t kCharacterCount = 17;
        constexpr uint32_t kCharacterBytes = 0x1FF4;
        constexpr uint32_t kCharacterLevels = 0x78E;
        constexpr uint32_t kLevelGoalsBytes = 0x206;
        constexpr uint32_t kLevelGoalsMarks = 0x06;
        constexpr uint32_t kLevelObjectives = 0x200;
        // A mark's bits (games/bmx/objectives/objectives.h GoalMark): the
        // objective was completed.
        constexpr uint8_t kGoalDone = 0x08;
        // Only a session of kind 0 has objectives (objectives.cpp g_sessionKind).
        constexpr uint8_t kObjectivesSession = 0;

        // The engine's objectives (engine/zge/scene/objectives.h
        // ObjectiveSystem, g_objectives): a pool of Objective records
        // (engine/zge/memory/pool.h Pool: its used list's links, the newest
        // first, a record 16 bytes after its link), the oldest first from
        // the list's tail by each link's previous.
        constexpr uint32_t kObjectiveSystemPool = 0x00;
        constexpr uint32_t kPoolUsedTail = 0x04;
        constexpr uint32_t kPoolLinkPrevious = 0x04;
        constexpr uint32_t kPoolLinkBytes = 0x10;
        // An Objective (0xE70 bytes): its flags (a set's child has no line
        // in the game's list), its name's hash, what it waits for, its
        // state, its bounds, its type and the type's own record.
        constexpr uint32_t kObjectiveBytes = 0xE70;
        constexpr uint32_t kObjectiveFlags = 0x00;
        constexpr uint32_t kObjectiveNameHash = 0x24;
        constexpr uint32_t kObjectivePrerequisites = 0xE28;
        constexpr uint32_t kObjectivePrerequisiteCount = 4;
        constexpr uint32_t kObjectiveState = 0xE38;
        constexpr uint32_t kObjectiveBoundsMin = 0xE40; // a Vec4
        constexpr uint32_t kObjectiveBoundsMax = 0xE50;
        constexpr uint32_t kObjectiveType = 0xE60;
        constexpr uint32_t kObjectiveData = 0xE64;
        constexpr uint32_t kObjectiveRepeatable = 1u << 2; // REPEATABLE: offered again once complete
        constexpr uint32_t kObjectiveIsChild = 1u << 10;
        // ObjectiveState: active, in progress, done, complete.
        constexpr uint32_t kObjectiveActive = 2, kObjectiveDone = 4, kObjectiveComplete = 5;
        // A registered type (ObjectiveType): its name's hash at +0x20.
        constexpr uint32_t kTypeNameHash = 0x20;
        constexpr uint32_t kTypeCollect = 0x084109C4u;   // nameHash("COLLECT")
        constexpr uint32_t kTypeSet = 0x000057A4u;       // nameHash("SET")
        constexpr uint32_t kTypeProximity = 0x04CE4BE9u; // nameHash("PROXIMITY")
        constexpr uint32_t kTypeTransfer = 0x06637F32u;  // nameHash("TRANSFER")
        constexpr uint32_t kTypeGrind = 0x004C6E24u;     // nameHash("GRIND")
        constexpr uint32_t kTypePoints = 0x0553E393u;    // nameHash("POINTS")
        constexpr uint32_t kTypeTrick = 0x00596D7Bu;     // nameHash("TRICK")
        constexpr uint32_t kTypeRecord = 0x05698464u;    // nameHash("RECORD")
        // An Objective's STRINGID: its texts' keys begin with it, its status
        // text's is "<STRINGID>_STATUS" (the types' status key format, made in
        // a 0x40-byte key).
        constexpr uint32_t kObjectiveStringId = 0x2C;
        constexpr const char *kStatusKeySuffix = "_STATUS";
        constexpr size_t kStatusKeyBytes = 0x40;
        // A SET record (objectives/set_type.cpp SetObjective).
        constexpr uint32_t kSetOrder = 0x00, kSetCount = 0x08, kSetRequired = 0x0C, kSetDone = 0x10;
        constexpr uint32_t kSetMembers = 0x18, kSetMemberFlags = 0x98, kSetMembersMost = 32, kSetBytes = 0xB8;
        // A member's flags: it has a status text, it is a set (set_type.cpp
        // kMemberHasStatus, kMemberIsSet). The game sets the status flags
        // whenever the lookup answers, and stringTableText answers "" for a
        // missing text, so every set and member has them: what decides is
        // whether the text is empty.
        constexpr uint8_t kMemberHasStatus = 0x1, kMemberIsSet = 0x2;
        constexpr uint32_t kSetOrdered = 2, kSetOrderedStrict = 3;
        // A COLLECT record (objectives/collect_type.cpp CollectObjective).
        constexpr uint32_t kCollectItemCount = 0x02, kCollectRequired = 0x03, kCollectCollected = 0x04;
        constexpr uint32_t kCollectCollectedBits = 0x05, kCollectItems = 0x10, kCollectItemsMost = 64, kCollectBytes = 0x110;
        // A POINTS record (points_type.cpp PointsObjective): MIN_POINTS, the points scored.
        constexpr uint32_t kPointsMinimum = 0x00, kPointsScored = 0x04, kPointsBytes = 0x0C;
        // A GRIND record (grind_type.cpp GrindObjective): NUMEDGES, how many are done.
        constexpr uint32_t kGrindTagCount = 0x02, kGrindDone = 0x03, kGrindBytes = 0x138;
        // The level's string table (zge/text/strings.h StringTable, g_levelStrings: the one
        // the game's status lines are made from): its entries' count, then each entry's key
        // (a name's hash) and its text's offset from the table (0: no text).
        constexpr uint32_t kStringTableCount = 0x04, kStringTableEntries = 0x08, kStringEntryBytes = 0x08;
        constexpr uint32_t kStringEntryOffset = 0x04, kStringTableMost = 0x10000;
        constexpr size_t kStatusBytes = 0x100; // the game's status line buffer (displays.cpp objectiveDisplayFrame)
        // A scene object (engine/zge/scene/scene.h SceneObject): its position,
        // and its owner, a record of the level objects' pool (engine/zge/scene/
        // objects.cpp LevelObject, 0x14 bytes each from levelObjectsBegin)
        // whose name node (LevelObjectName, 0x10 bytes, the hash at +8) is
        // the record's by index after the pool.
        constexpr uint32_t kSceneObjectPosition = 0x10, kSceneObjectOwner = 0x40, kSceneObjectBytes = 0x50;
        constexpr uint32_t kLevelObjects = 0x200, kLevelObjectBytes = 0x14, kLevelObjectNameBytes = 0x10, kLevelObjectNameHash = 0x08;
        constexpr uint32_t kTargetWalkDepth = 8;

        // The overlay's objective display (overlay/displays.cpp
        // ObjectiveDisplayState, a player each): the goal shown (its place
        // among the level's objectives; kNoGoal none) and its Objective. Its
        // widget is the objective display's (displays.h kOverlayObjective, a
        // row of g_overlayDisplays: OverlayDisplay, the players' widgets at
        // +0x14) where the "Status" widget's "Text" variable holds the
        // status line (zge/zgui/widget.h Node: siblings, children, the kind
        // in the low four bits of +0x0C, the name hash at +0x10, a variable's
        // value at +0x18: a text's holder).
        constexpr uint32_t kObjectiveDisplayBytes = 0x18, kObjectiveDisplayGoal = 0x04, kObjectiveDisplayObjective = 0x08;
        constexpr uint32_t kNoGoal = 0xFFFFFFFFu;
        constexpr uint32_t kOverlayObjective = 12, kOverlayDisplayBytes = 0x24, kOverlayDisplayPlayers = 0x14, kOverlayPlayerBytes = 0x08;
        constexpr uint32_t kOverlayPlayerWidget = 0x04;
        constexpr uint32_t kNodeNextSibling = 0x00, kNodeFirstChild = 0x04, kNodeKind = 0x0C, kNodeNameHash = 0x10, kNodeValue = 0x18;
        constexpr uint32_t kNodeBytes = 0x40;
        constexpr uint32_t kNodeWidget = 1, kNodeVariable = 2;
        constexpr uint32_t kStatusHash = 0x058869A3u; // nameHash("Status")
        constexpr uint32_t kTextHash = 0x00058AD4u;   // nameHash("Text")
        constexpr uint32_t kNodeChildrenMost = 64;

        // The trick display's state (overlay/displays.cpp TrickDisplayState,
        // g_trickDisplay, a player each): the name label the newest line took
        // (each new line takes the next of the four, in turn), the four name
        // labels ("LabelName00" ..) and the score label ("LabelScore"), whose
        // "Text" trickDisplayMessage sets.
        constexpr uint32_t kTrickDisplayBytes = 0x18, kTrickDisplayLine = 0x00, kTrickDisplayLabels = 0x04, kTrickDisplayScore = 0x14;
        constexpr uint32_t kTrickNameLabels = 4;
        constexpr uint32_t kPlayerOne = 0; // (the displays' and riders' first)

        // The level's NPC riders (games/bmx/game/npcs.h Npc, g_npcs; a rider
        // of kNoNpc ends the list), each with its record (NpcRecord: the
        // objective it challenges the player to, "" none) and its rider's
        // slot (rider.h riderOf, g_riders).
        constexpr uint32_t kNpcs = 3, kNpcBytes = 0x24, kNpcRider = 0x00, kNpcRecord = 0x18;
        constexpr uint8_t kNoNpc = 0xFF;
        constexpr uint32_t kNpcRecordObjective = 0x0C, kNpcRecordBytes = 0x2C, kNpcObjectiveBytes = 0x20;
        constexpr uint32_t kRiderSlots = 16;

        // The level's path men (games/bmx/pathman/pathman.h PathManRecord, a
        // record each from the pool g_pathManRecords, the pool's used list
        // walked by each link's next): its follower (PathManTravel, 0 until it
        // is put on a path), the objective it gives ("" none) and that
        // objective (0: the level has none of the name). A follower of
        // either kind (zge/scene/follower.h Follower, fast_paths.h
        // FastFollower) holds at +0x20 the place movePathMan puts the path
        // man at as it walks.
        constexpr uint32_t kPoolUsedHead = 0x00, kPoolLinkNext = 0x00, kPathMenMost = 512;
        constexpr uint32_t kPathManFollower = 0x04, kPathManObjectiveName = 0x88, kPathManObjective = 0xC8, kPathManRecordBytes = 0xCC;
        constexpr uint32_t kPathManObjectiveNameBytes = 0x40;
        constexpr uint32_t kFollowerPosition = 0x20;

        // The level's pickups (level/pickups.cpp, the level object type
        // "Pickup") that the game sets up itself as a turn starts
        // (game/pickups.cpp setUpLevelPickup: those whose node's name, after
        // its first '_', begins with one of g_pickupKindNames' kinds; the
        // other pickups are objectives' items). Each is a record of the level
        // objects' pool (LevelObject: its scene object, its node, its type,
        // whose name hash is "Pickup"'s); its name is its node's (ZGE4's Node
        // +0x128, objectLevelNodeName); its state the first byte of its own
        // tag record (objectComponent: the node's tag table for an object
        // with a scene of a type, else the object's own; ZGE4's table keeps
        // its records' address at +0x0C), there to take while enabling or
        // idle (before activating: taken, disabled).
        constexpr uint32_t kLevelObjectObject = 0x08, kLevelObjectNode = 0x0C, kLevelObjectType = 0x10;
        constexpr uint32_t kTypeNameHash0 = 0x00; // EntityType: its name's hash first
        constexpr uint32_t kTypePickup = 0x054D80A0u; // nameHash("Pickup")
        constexpr uint32_t kSceneObjectScene = 0x38, kSceneObjectTags = 0x3C;
        constexpr uint32_t kObjectKind = 0x1F, kParticleEmitterKind = 4;
        constexpr uint32_t kNodeName = 0x128, kNodeNameBytes = 0x20, kNodeTags = 0x150, kLevelNodeBytes = 0x1F0;
        constexpr uint32_t kTagTableRecords = 0x0C;
        constexpr uint8_t kPickupActivating = 2;
        constexpr uint32_t kPickupKinds = 5; // health, weapon, score, bike part, sniper (game/pickups.cpp PickupKind)
        constexpr uint32_t kPickupKindNameBytes = 8; // PickupKindName: the kind's capitals, its length

        // games/bmx/states/game_state.h: the level being ridden, its phase
        // running (bmx_xxx_photo.cpp's); games/bmx/overlay/screens.h GameScreen:
        // the pause menu and every screen from it (the trick list, the
        // objectives, the exit confirmation) are kScreenPause and up.
        constexpr uint8_t kGameStateLevel = 1, kStateRunning = 0;
        constexpr uint32_t kScreenPause = 7;

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

        // A level folder's name as the table has it ("CHL_DAM"): capitals,
        // digits and underscores, at least one character.
        bool isFolderName(const char *name)
        {
            if (!name[0])
                return false;
            for (const char *c = name; *c; ++c)
                if (!((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_'))
                    return false;
            return true;
        }

        // A scene object's name, hashed, through its level object; 0 without one.
        uint32_t sceneObjectNameHash(const Ram &ram, uint32_t object)
        {
            const uint32_t owner = ram.read<uint32_t>(object + kSceneObjectOwner);
            const uint32_t begin = sym::bmx::levelObjectsBegin;
            if (owner < begin || owner >= begin + kLevelObjects * kLevelObjectBytes || (owner - begin) % kLevelObjectBytes)
                return 0;
            const uint32_t name = begin + kLevelObjects * kLevelObjectBytes + (owner - begin) / kLevelObjectBytes * kLevelObjectNameBytes;
            return ram.read<uint32_t>(name + kLevelObjectNameHash);
        }

        uint32_t objectiveTypeHash(const Ram &ram, uint32_t objective)
        {
            const uint32_t type = ram.read<uint32_t>(objective + kObjectiveType);
            return ram.ok(type, kTypeNameHash + 4) ? ram.read<uint32_t>(type + kTypeNameHash) : 0u;
        }

        bool objectiveFinished(const Ram &ram, uint32_t objective)
        {
            return ram.read<uint32_t>(objective + kObjectiveState) >= kObjectiveDone;
        }

        bool objectiveRunning(const Ram &ram, uint32_t objective)
        {
            const uint32_t state = ram.read<uint32_t>(objective + kObjectiveState);
            return state >= kObjectiveActive && state < kObjectiveDone;
        }

        // objectiveReadyToActivate's rule: not active yet, every prerequisite complete.
        bool objectiveReady(const Ram &ram, uint32_t objective)
        {
            if (ram.read<uint32_t>(objective + kObjectiveState) >= kObjectiveActive)
                return false;
            for (uint32_t i = 0; i < kObjectivePrerequisiteCount; ++i)
            {
                const uint32_t prerequisite = ram.read<uint32_t>(objective + kObjectivePrerequisites + i * 4);
                if (prerequisite && !(ram.ok(prerequisite, kObjectiveBytes) &&
                                      ram.read<uint32_t>(prerequisite + kObjectiveState) == kObjectiveComplete))
                    return false;
            }
            return true;
        }

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
        static_assert(nameHash("Pickup") == kTypePickup);
        static_assert(nameHash("COLLECT") == kTypeCollect && nameHash("SET") == kTypeSet && nameHash("POINTS") == kTypePoints &&
                      nameHash("TRICK") == kTypeTrick && nameHash("RECORD") == kTypeRecord && nameHash("GRIND") == kTypeGrind &&
                      nameHash("Status") == kStatusHash && nameHash("Text") == kTextHash);

        // An objective's status format (its type's statusText: the level
        // table's "<STRINGID>_STATUS"), "" when the table has none, as
        // stringTableText answers.
        void statusFormat(const Ram &ram, uint32_t objective, char *out, size_t bytes)
        {
            out[0] = 0;
            char key[kStatusKeyBytes];
            ram.text(objective + kObjectiveStringId, key, sizeof(key) - std::strlen(kStatusKeySuffix));
            std::strcat(key, kStatusKeySuffix);
            const uint32_t hash = nameHash(key);
            const uint32_t table = ram.read<uint32_t>(sym::bmx::g_levelStrings);
            const uint32_t entries = ram.read<uint32_t>(table + kStringTableCount);
            if (!ram.ok(table, kStringTableEntries) || entries > kStringTableMost ||
                !ram.ok(table + kStringTableEntries, entries * kStringEntryBytes))
                return;
            for (uint32_t i = 0; i < entries; ++i) // first match wins, as the game's search
            {
                const uint32_t entry = table + kStringTableEntries + i * kStringEntryBytes;
                if (ram.read<uint32_t>(entry) != hash)
                    continue;
                const uint32_t offset = ram.read<uint32_t>(entry + kStringEntryOffset);
                if (offset)
                    ram.text(table + offset, out, bytes);
                return;
            }
        }

        // The count a status format writes. A format with two integer
        // conversions and no other ("%d of %d", in every language) writes the
        // type's two values, the count and its target. A format with no
        // conversion is the line as it stands: a count when the line has two
        // whole numbers, the first at most the second (the delivery jobs'
        // steps: "Hookers delivered: 2 of 5", "Abgelieferte Aliens: 2 von 5").
        // Any other format (a time, a record: "%3.1f seconds") carries none.
        bool formatCount(const char *format, uint32_t first, uint32_t second, uint32_t &count, uint32_t &target)
        {
            int integers = 0, others = 0;
            for (const char *c = format; *c; ++c)
            {
                if (*c != '%')
                    continue;
                ++c;
                while (*c && std::strchr("-+ #0123456789.lh", *c))
                    ++c;
                if (!*c)
                    break;
                if (*c == 'd' || *c == 'i' || *c == 'u')
                    ++integers;
                else if (*c != '%')
                    ++others;
            }
            if (integers == 2 && others == 0)
            {
                count = first, target = second;
                return true;
            }
            if (integers != 0 || others != 0)
                return false;
            uint32_t numbers[2] = {}, found = 0;
            for (const char *c = format; *c;)
            {
                if (*c < '0' || *c > '9')
                {
                    ++c;
                    continue;
                }
                uint32_t value = 0;
                for (; *c >= '0' && *c <= '9'; ++c)
                    value = value * 10 + static_cast<uint32_t>(*c - '0');
                if (found == 2)
                    return false;
                numbers[found++] = value;
            }
            if (found != 2 || numbers[1] == 0 || numbers[0] > numbers[1])
                return false;
            count = numbers[0], target = numbers[1];
            return true;
        }

        // What the objective's status line (objectiveStatusText, the line the
        // game's display shows for its goal) is made of: whether it has one,
        // and the count and target it carries, if any. A SET's is its first
        // running member's that has one (set_type.cpp setStatusText), else its
        // own format with how many members are done and are required; a
        // COLLECT's its own with those collected and required, a POINTS' the
        // points scored and MIN_POINTS, a GRIND's the tags done and NUMEDGES;
        // a TRICK's and a RECORD's are times and records, no count. The other
        // types have no line.
        bool statusCount(const Ram &ram, uint32_t objective, uint32_t depth, bool &counted, uint32_t &count, uint32_t &target)
        {
            if (depth > kTargetWalkDepth || !ram.ok(objective, kObjectiveBytes))
                return false;
            const uint32_t type = objectiveTypeHash(ram, objective);
            const uint32_t data = ram.read<uint32_t>(objective + kObjectiveData);
            uint32_t first = 0, second = 0;
            if (type == kTypeSet && ram.ok(data, kSetBytes))
            {
                const int32_t members = ram.read<int32_t>(data + kSetCount);
                for (int32_t i = 0; i < members && i < static_cast<int32_t>(kSetMembersMost); ++i)
                {
                    const uint32_t member = ram.read<uint32_t>(data + kSetMembers + static_cast<uint32_t>(i) * 4);
                    const uint8_t flags = ram.read<uint8_t>(data + kSetMemberFlags + static_cast<uint32_t>(i));
                    if (!ram.ok(member, kObjectiveBytes) || !objectiveRunning(ram, member) || !(flags & (kMemberIsSet | kMemberHasStatus)))
                        continue;
                    if (statusCount(ram, member, depth + 1, counted, count, target))
                        return true;
                }
                first = static_cast<uint32_t>(ram.read<int32_t>(data + kSetDone));
                second = static_cast<uint32_t>(ram.read<int32_t>(data + kSetRequired));
            }
            else if (type == kTypeCollect && ram.ok(data, kCollectBytes))
                first = ram.read<uint8_t>(data + kCollectCollected), second = ram.read<uint8_t>(data + kCollectRequired);
            else if (type == kTypePoints && ram.ok(data, kPointsBytes))
                first = ram.read<uint32_t>(data + kPointsScored), second = ram.read<uint32_t>(data + kPointsMinimum);
            else if (type == kTypeGrind && ram.ok(data, kGrindBytes))
                first = ram.read<uint8_t>(data + kGrindDone), second = ram.read<uint8_t>(data + kGrindTagCount);
            else if (type != kTypeTrick && type != kTypeRecord)
                return false;
            char format[kStatusBytes];
            statusFormat(ram, objective, format, sizeof(format));
            if (!format[0])
                return false;
            counted = (type != kTypeTrick && type != kTypeRecord) && formatCount(format, first, second, count, target);
            return true;
        }

        // A goal's count and target: those its status line carries.
        bool goalCounts(const Ram &ram, uint32_t objective, uint32_t &count, uint32_t &target)
        {
            bool counted = false;
            return statusCount(ram, objective, 0, counted, count, target) && counted;
        }

        // At the level's origin nothing stands: an object not placed yet, or
        // taken out of the level (a delivered hooker), or empty bounds.
        bool placed(float x, float z) { return x * x + z * z > 0.25f; }

        class Targets
        {
        public:
            Targets(const Ram &ram, panel::Snapshot &out) : ram_(ram), out_(out) {}

            // The goal's targets as the game has them now, through its sets.
            void add(int32_t goal, uint32_t objective, panel::TargetPhase phase, uint32_t depth)
            {
                if (depth > kTargetWalkDepth || !ram_.ok(objective, kObjectiveBytes))
                    return;
                const uint32_t type = objectiveTypeHash(ram_, objective);
                const uint32_t data = ram_.read<uint32_t>(objective + kObjectiveData);
                if (type == kTypeSet && ram_.ok(data, kSetBytes))
                {
                    const uint32_t order = ram_.read<uint32_t>(data + kSetOrder);
                    const bool ordered = order == kSetOrdered || order == kSetOrderedStrict;
                    const int32_t count = ram_.read<int32_t>(data + kSetCount);
                    for (int32_t i = 0; i < count && i < static_cast<int32_t>(kSetMembersMost); ++i)
                    {
                        const uint32_t member = ram_.read<uint32_t>(data + kSetMembers + static_cast<uint32_t>(i) * 4);
                        if (!ram_.ok(member, kObjectiveBytes))
                            continue;
                        panel::TargetPhase memberPhase = phase;
                        if (objectiveFinished(ram_, member))
                            memberPhase = panel::TargetPhase::Done;
                        else if (!objectiveRunning(ram_, member))
                            memberPhase = ordered ? panel::TargetPhase::Later : phase;
                        add(goal, member, memberPhase, depth + 1);
                    }
                }
                else if (type == kTypeCollect && ram_.ok(data, kCollectBytes))
                {
                    const uint32_t items = ram_.read<uint8_t>(data + kCollectItemCount);
                    for (uint32_t i = 0; i < items && i < kCollectItemsMost; ++i)
                    {
                        const uint32_t item = ram_.read<uint32_t>(data + kCollectItems + i * 4);
                        if (!ram_.ok(item, kSceneObjectBytes))
                            continue;
                        const bool got = (ram_.read<uint8_t>(data + kCollectCollectedBits + i / 8) >> (i % 8)) & 1u;
                        const float x = ram_.read<float>(item + kSceneObjectPosition), z = ram_.read<float>(item + kSceneObjectPosition + 8);
                        // (the level file's target of the same name says whether it is a drop-off: its role wins)
                        if (placed(x, z))
                            put(goal, sceneObjectNameHash(ram_, item), x, z, panel::TargetRole::Pickup, got ? panel::TargetPhase::Done : phase);
                    }
                }
                else if (type == kTypeProximity || type == kTypeTransfer || type == kTypeGrind)
                {
                    const float x0 = ram_.read<float>(objective + kObjectiveBoundsMin), x1 = ram_.read<float>(objective + kObjectiveBoundsMax);
                    const float z0 = ram_.read<float>(objective + kObjectiveBoundsMin + 8), z1 = ram_.read<float>(objective + kObjectiveBoundsMax + 8);
                    const float x = (x0 + x1) * 0.5f, z = (z0 + z1) * 0.5f;
                    if (x == x && z == z && placed(x, z))
                        put(goal, ram_.read<uint32_t>(objective + kObjectiveNameHash), x, z, panel::TargetRole::Place,
                            objectiveFinished(ram_, objective) ? panel::TargetPhase::Done : phase);
                }
            }

        private:
            // One target, once: the same object named by two steps keeps the earliest phase.
            void put(int32_t goal, uint32_t nameHash, float x, float z, panel::TargetRole role, panel::TargetPhase phase)
            {
                for (uint32_t i = 0; i < out_.goalTargetCount; ++i)
                {
                    panel::GoalTarget &t = out_.goalTargets[i];
                    if (t.goal == goal && t.nameHash == nameHash && t.x == x && t.z == z)
                    {
                        if (static_cast<uint8_t>(phase) < static_cast<uint8_t>(t.phase))
                            t.phase = phase;
                        return;
                    }
                }
                if (out_.goalTargetCount >= panel::kMaxGoalTargets)
                    return;
                out_.goalTargets[out_.goalTargetCount++] = {goal, nameHash, x, z, role, phase, {0, 0}};
            }

            const Ram &ram_;
            panel::Snapshot &out_;
        };

        // A widget's "Text" variable's text (what setVariable(widget, "Text", ..)
        // last set), "" when it has none.
        void widgetText(const Ram &ram, uint32_t widget, char *out, size_t bytes)
        {
            out[0] = 0;
            if (!ram.ok(widget, kNodeBytes))
                return;
            uint32_t child = ram.read<uint32_t>(widget + kNodeFirstChild);
            for (uint32_t i = 0; i < kNodeChildrenMost && ram.ok(child, kNodeBytes); ++i, child = ram.read<uint32_t>(child + kNodeNextSibling))
                if ((ram.read<uint32_t>(child + kNodeKind) & 0xFu) == kNodeVariable && ram.read<uint32_t>(child + kNodeNameHash) == kTextHash)
                {
                    const uint32_t holder = ram.read<uint32_t>(child + kNodeValue);
                    if (ram.ok(holder))
                        ram.text(ram.read<uint32_t>(holder), out, bytes);
                    return;
                }
        }

        // The "Text" of the "Status" widget below `widget`, as the overlay's
        // objective display sets it every frame (objectiveDisplayFrame).
        void readStatusText(const Ram &ram, uint32_t widget, char *out, size_t bytes)
        {
            out[0] = 0;
            uint32_t stack[32];
            int top = 0;
            if (ram.ok(widget, kNodeBytes))
                stack[top++] = ram.read<uint32_t>(widget + kNodeFirstChild);
            for (uint32_t steps = 0; top > 0 && steps < 512; ++steps)
            {
                const uint32_t node = stack[--top];
                if (!ram.ok(node, kNodeBytes))
                    continue;
                const uint32_t kind = ram.read<uint32_t>(node + kNodeKind) & 0xFu;
                if (kind == kNodeWidget && ram.read<uint32_t>(node + kNodeNameHash) == kStatusHash)
                {
                    widgetText(ram, node, out, bytes);
                    return;
                }
                const uint32_t sibling = ram.read<uint32_t>(node + kNodeNextSibling);
                if (sibling && top < 32)
                    stack[top++] = sibling;
                const uint32_t child = ram.read<uint32_t>(node + kNodeFirstChild);
                if (kind == kNodeWidget && child && top < 32)
                    stack[top++] = child;
            }
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

        // The rider's current trick line as the trick display has it: each
        // of the line's lines (score.lines, a trick and its modifiers each)
        // is in the name label it took, the newest in the display's `line`,
        // the one before it in the label before, so the last four are there,
        // the oldest first; from the second on the game writes them after a
        // grey "TO". The newest one's text alone is the current trick; the
        // score label's "%ld points" ("" in the modes that score no points)
        // the pending score, read whether or not the HUD shows the label
        // then (its animations show and hide it: many a one-trick line's
        // points are never shown).
        void readLine(const Ram &ram, uint32_t rider, uint32_t player, panel::Snapshot &out)
        {
            const uint32_t lines = ram.read<uint8_t>(rider + kRiderLineCount);
            const uint32_t display = sym::bmx::g_trickDisplay + player * kTrickDisplayBytes;
            if (lines == 0)
                return;
            const uint32_t newest = ram.read<uint8_t>(display + kTrickDisplayLine) % kTrickNameLabels;
            const uint32_t shown = std::min(lines, kTrickNameLabels);
            size_t length = 0;
            for (uint32_t back = shown; back-- > 0;)
            {
                char text[panel::kLineBytes];
                const uint32_t label = (newest + kTrickNameLabels - back) % kTrickNameLabels;
                widgetText(ram, ram.read<uint32_t>(display + kTrickDisplayLabels + label * 4), text, sizeof(text));
                plainText(text);
                if (!text[0])
                    continue;
                length += static_cast<size_t>(std::snprintf(out.line + length, sizeof(out.line) - length, "%s%s", length ? " " : "", text));
                length = std::min(length, sizeof(out.line) - 1);
                if (back == 0)
                    std::snprintf(out.currentTrick, sizeof(out.currentTrick), "%s", text);
            }
            char points[32];
            widgetText(ram, ram.read<uint32_t>(display + kTrickDisplayScore), points, sizeof(points));
            out.pendingScore = static_cast<float>(std::strtoul(points, nullptr, 10));
        }

        // The pickup's kind as setUpLevelPickup finds it (entityKindName: its
        // node's name after the first '_', in capitals, against the kinds'
        // names), kPickupKinds for none.
        uint32_t pickupKind(const Ram &ram, const char *name)
        {
            const char *after = std::strchr(name, '_');
            if (!after)
                return kPickupKinds;
            ++after;
            for (uint32_t kind = 0; kind < kPickupKinds; ++kind)
            {
                const uint32_t row = sym::bmx::g_pickupKindNames + kind * kPickupKindNameBytes;
                char kindName[16];
                ram.text(ram.read<uint32_t>(row), kindName, sizeof(kindName));
                const size_t length = std::min<size_t>(ram.read<uint32_t>(row + 4), std::strlen(kindName));
                size_t i = 0;
                while (i < length && after[i] && std::toupper(static_cast<unsigned char>(after[i])) == kindName[i])
                    ++i;
                if (length && i == length)
                    return kind;
            }
            return kPickupKinds;
        }

        // The pickup's first tag record (objectComponent), 0 for none.
        uint32_t pickupTags(const Ram &ram, uint32_t object, uint32_t node)
        {
            const uint32_t scene = ram.read<uint32_t>(object + kSceneObjectScene);
            uint32_t table = 0;
            if (ram.ok(scene, 1) && ram.read<uint8_t>(scene) != 0)
                table = ram.ok(node, kLevelNodeBytes) ? ram.read<uint32_t>(node + kNodeTags) : 0u;
            else if ((ram.read<uint32_t>(object) & kObjectKind) != kParticleEmitterKind)
                table = ram.read<uint32_t>(object + kSceneObjectTags);
            return ram.ok(table, kTagTableRecords + 4) ? ram.read<uint32_t>(table + kTagTableRecords) : 0u;
        }

        // What lies about: the level's own pickups, a Collectable each (its
        // kind BMX XXX's, panel_snapshot.h), shown while there to take.
        void readItems(const Ram &ram, panel::Snapshot &out)
        {
            for (uint32_t i = 0; i < kLevelObjects && out.collectableCount < panel::kMaxCollectables; ++i)
            {
                const uint32_t record = sym::bmx::levelObjectsBegin + i * kLevelObjectBytes;
                const uint32_t object = ram.read<uint32_t>(record + kLevelObjectObject);
                const uint32_t type = ram.read<uint32_t>(record + kLevelObjectType);
                const uint32_t node = ram.read<uint32_t>(record + kLevelObjectNode);
                if (!ram.ok(object, kSceneObjectBytes) || !ram.ok(type) || ram.read<uint32_t>(type + kTypeNameHash0) != kTypePickup ||
                    !ram.ok(node, kLevelNodeBytes))
                    continue;
                char name[kNodeNameBytes + 1];
                ram.text(node + kNodeName, name, sizeof(name));
                const uint32_t kind = pickupKind(ram, name);
                const uint32_t tags = pickupTags(ram, object, node);
                if (kind >= kPickupKinds || !ram.ok(tags, 1))
                    continue;
                panel::Collectable &item = out.collectables[out.collectableCount++];
                item.kind = static_cast<uint8_t>(panel::kBmxPickupKinds + kind);
                item.shown = ram.read<uint8_t>(tags) < kPickupActivating ? 1 : 0;
                item.x = ram.read<float>(object + kSceneObjectPosition);
                item.z = ram.read<float>(object + kSceneObjectPosition + 8);
                item.ownerHash = 0;
            }
        }

        // A tracked object at a world place.
        void putTracked(panel::Snapshot &out, uint32_t nameHash, const Ram &ram, uint32_t position)
        {
            if (out.trackedCount >= panel::kMaxTrackedObjects)
                return;
            panel::TrackedObject &object = out.tracked[out.trackedCount++];
            object.nameHash = nameHash;
            object.x = ram.read<float>(position);
            object.y = ram.read<float>(position + 4);
            object.z = ram.read<float>(position + 8);
        }

        // The givers where they are now, each by the name of the objective
        // it gives (the name a goal's giver has in the panel file): the path
        // men that give one of the level's objectives and have walked their
        // path, at the place in their follower (a fast follower's room
        // movePathMan fills as it moves the path man, a path follower's point
        // on its path; a fast follower is made cleared, so one whose path man
        // has only stood holds the origin, and so has none: the file's place,
        // the level's, is where those stand), and the NPC riders with a
        // challenge, where their rider is. A name more than one path man
        // gives has no one place: those keep the file's.
        void readGivers(const Ram &ram, panel::Snapshot &out)
        {
            const uint32_t pool = ram.read<uint32_t>(sym::bmx::g_pathManRecords);
            uint32_t link = ram.ok(pool, kPoolUsedHead + 4) ? ram.read<uint32_t>(pool + kPoolUsedHead) : 0u;
            const uint32_t firstPathMan = out.trackedCount;
            for (uint32_t steps = 0; link && ram.ok(link, kPoolLinkBytes + kPathManRecordBytes) && steps < kPathMenMost;
                 ++steps, link = ram.read<uint32_t>(link + kPoolLinkNext))
            {
                const uint32_t record = link + kPoolLinkBytes;
                const uint32_t follower = ram.read<uint32_t>(record + kPathManFollower);
                if (!ram.ok(ram.read<uint32_t>(record + kPathManObjective), kObjectiveBytes) || !ram.ok(follower + kFollowerPosition, 12))
                    continue;
                char name[kPathManObjectiveNameBytes];
                ram.text(record + kPathManObjectiveName, name, sizeof(name));
                if (name[0])
                    putTracked(out, nameHash(name), ram, follower + kFollowerPosition);
            }
            // (each name's givers counted over the path men as read, before any is moved down over another)
            std::array<uint32_t, panel::kMaxTrackedObjects> carriers{};
            for (uint32_t i = firstPathMan; i < out.trackedCount; ++i)
                for (uint32_t j = firstPathMan; j < out.trackedCount; ++j)
                    carriers[i] += out.tracked[j].nameHash == out.tracked[i].nameHash ? 1u : 0u;
            uint32_t kept = firstPathMan;
            for (uint32_t i = firstPathMan; i < out.trackedCount; ++i)
                if (carriers[i] == 1 && placed(out.tracked[i].x, out.tracked[i].z))
                    out.tracked[kept++] = out.tracked[i];
            out.trackedCount = kept;

            for (uint32_t npc = 0; npc < kNpcs; ++npc)
            {
                const uint32_t at = sym::bmx::g_npcs + npc * kNpcBytes;
                const uint8_t riderIndex = ram.read<uint8_t>(at + kNpcRider);
                if (riderIndex == kNoNpc)
                    break;
                const uint32_t record = ram.read<uint32_t>(at + kNpcRecord);
                const uint32_t npcRider = riderIndex < kRiderSlots ? ram.read<uint32_t>(sym::bmx::g_riders + riderIndex * 4u) : 0u;
                if (!ram.ok(record, kNpcRecordBytes) || !ram.ok(npcRider, kRiderReadBytes))
                    continue;
                char name[kNpcObjectiveBytes + 1];
                ram.text(record + kNpcRecordObjective, name, sizeof(name)); // (at most the record's 0x20 bytes)
                if (name[0])
                    putTracked(out, nameHash(name), ram, npcRider + kRiderPosition);
            }
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
        out.objectiveDoneMarks = 0;
        out.goalStatus[0] = 0;
        out.goalTargetCount = 0;
        out.hasMoves = false;
        out.scriptWordCount = 0;
        out.trackedCount = 0;
        out.collectableCount = 0;
        out.line[0] = 0;
        out.currentTrick[0] = 0;

        if (ram.read<uint8_t>(sym::bmx::g_currentGameState) != kGameStateLevel ||
            ram.read<uint8_t>(sym::bmx::g_gameStatePhase) != kStateRunning ||
            ram.read<uint32_t>(sym::bmx::g_physicsTaskFrames) == 0u)
            return;
        const uint32_t rider = ram.read<uint32_t>(sym::bmx::g_riders); // player 1's
        const uint8_t level = ram.read<uint8_t>(sym::bmx::g_currentLevel);
        if (level >= kLevelCount || !ram.ok(rider, kRiderReadBytes))
            return;
        char folder[sizeof(out.levelFile)];
        ram.text(ram.read<uint32_t>(sym::bmx::g_levels + level * kLevelInfoBytes + kLevelInfoName), folder, sizeof(folder));
        if (!isFolderName(folder))
            return;

        const bool paused = ram.read<uint32_t>(sym::bmx::g_gameScreen) >= kScreenPause;
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
        out.multiplier = 0; // (BMX XXX keeps none: a line's points are its tricks' sum)
        // The clock of the challenge with a time limit the rider is on.
        out.timerSeconds = ram.read<uint32_t>(rider + kRiderTimedObjective) != 0u
                               ? static_cast<float>(ram.read<uint32_t>(rider + kRiderTicksLeft)) / kFramesASecond
                               : -1.0f;
        // Player 1's trick line, on player 1's trick display.
        readLine(ram, rider, kPlayerOne, out);

        // The moves ever landed are the saved game's, whoever rides.
        static_assert(kSavedMovesBytes <= panel::kMoveBitBytes);
        for (uint32_t i = 0; i < panel::kMoveBitBytes; ++i)
        {
            out.movesOff[i] = 0;
            out.movesLanded[i] = i < kSavedMovesBytes ? ram.read<uint8_t>(sym::bmx::g_savedGame + kSavedMovesLanded + i) : 0;
        }
        out.hasMoves = true;
        readItems(ram, out);

        // The current player's character, and its goals of this level.
        if (ram.read<uint8_t>(sym::bmx::g_sessionKind) != kObjectivesSession)
            return;
        const uint8_t player = ram.read<uint8_t>(sym::bmx::g_currentPlayer);
        if (player >= kMaxPlayers)
            return;
        const uint8_t character = ram.read<uint8_t>(sym::bmx::g_playerSetups + player * kSetupBytes + kSetupCharacter);
        if (character >= kCharacterCount)
            return;
        const uint32_t goals = sym::bmx::g_savedGame + kSavedCharacters + character * kCharacterBytes + kCharacterLevels +
                               level * kLevelGoalsBytes;
        if (!ram.ok(goals, kLevelGoalsBytes))
            return;
        // The goal the game's display shows the player, by its place among the level's objectives.
        const uint32_t display = sym::bmx::g_objectiveDisplay + player * kObjectiveDisplayBytes;
        const uint32_t shownPlace = ram.read<uint32_t>(display + kObjectiveDisplayGoal);
        const uint32_t shownObjective = ram.read<uint32_t>(display + kObjectiveDisplayObjective);
        // The level's objectives, the oldest first (the file's order), as the
        // game's list walks them: each one's place counts, a set's child has
        // no line.
        const uint32_t pool = ram.read<uint32_t>(sym::bmx::g_objectives + kObjectiveSystemPool);
        if (!ram.ok(pool, kPoolUsedTail + 4))
            return;
        Targets targets(ram, out);
        uint32_t slot = 0;
        uint32_t link = ram.read<uint32_t>(pool + kPoolUsedTail);
        for (uint32_t place = 0; link && ram.ok(link, kPoolLinkBytes + kObjectiveBytes) && place < kLevelObjectives &&
                                 slot < panel::kMaxObjectives && slot * 2 + 1 < panel::kMaxScriptWords;
             ++place, link = ram.read<uint32_t>(link + kPoolLinkPrevious))
        {
            const uint32_t objective = link + kPoolLinkBytes;
            const uint32_t flags = ram.read<uint32_t>(objective + kObjectiveFlags);
            if (flags & kObjectiveIsChild)
                continue;
            const uint32_t state = ram.read<uint32_t>(objective + kObjectiveState);
            const bool marked = state == kObjectiveComplete || (ram.read<uint8_t>(goals + kLevelGoalsMarks + place) & kGoalDone);
            const bool underway = state >= kObjectiveActive && state != kObjectiveComplete;
            // A repeatable goal is back at ready the frame it completes
            // (resetObjective): taken again, it runs with its done mark kept.
            const bool repeating = marked && underway && (flags & kObjectiveRepeatable);
            const bool done = marked && !repeating;
            out.objectiveState[slot] = done                             ? panel::kStateComplete
                                       : underway                       ? panel::kStateOpen
                                       : objectiveReady(ram, objective) ? panel::kStateReady
                                                                        : panel::kStateInit;
            if (marked)
                out.objectiveDoneMarks |= uint64_t{1} << slot;
            uint32_t count = 0, target = 0;
            const bool counted = goalCounts(ram, objective, count, target);
            out.scriptWords[slot * 2] = counted ? count : 0xFFFFFFFFu;
            out.scriptWords[slot * 2 + 1] = counted ? target : 0u;
            if (shownPlace != kNoGoal && place == shownPlace && shownObjective == objective)
                out.activeGoal = static_cast<int32_t>(slot);
            if (!done && underway)
                targets.add(static_cast<int32_t>(slot), objective, panel::TargetPhase::Now, 0);
            ++slot;
        }
        out.objectiveCount = slot;
        out.scriptWordCount = slot * 2;
        readGivers(ram, out);
        if (out.activeGoal >= 0)
        {
            const uint32_t widgets = sym::bmx::g_overlayDisplays + kOverlayObjective * kOverlayDisplayBytes + kOverlayDisplayPlayers;
            readStatusText(ram, ram.read<uint32_t>(widgets + player * kOverlayPlayerBytes + kOverlayPlayerWidget), out.goalStatus,
                           sizeof(out.goalStatus));
        }
    }

    void install()
    {
        panel::setGameCapture(&capture);
    }
}
