// A level's challenges: the script data's objectives (chunks "Objective",
// "BonusObjective", "GapObjective", "GrindSequence"), their saved state, and
// what the skater's gaps and grind sequences do to them.
//
// Objectives and bonus objectives go through states by the hash of a name:
// init, active, wait (for the skater's crash or landing), complete,
// waitSuccess, waitFail.

#include "aggressive/level/objectives.h"
#include "aggressive/career.h"
#include "aggressive/globals.h"
#include "aggressive/level/level_object.h"
#include "aggressive/level/pause_parts.h"
#include "aggressive/script/script.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/skater.h"
#include "aggressive/hud/hud.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/scene/node.h"
#include "zge/scene/scene.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/objectives.cpp.h"

namespace aggressive::level
{
    namespace
    {
        using parts::skaterHandle;
        using pause::Objective;
        using skater::skaterProfile;

        constexpr uint32_t kHiddenChallenge = 3; // Objective::kind: not listed, and worth no goal
        constexpr uint32_t kBonusKindGap = 8;    // BonusObjective::kind

        using pause::BonusObjective;

        // A gap: over it from either end, then the other end, makes the
        // bonus objective. The ends are objects by name and a surface (0:
        // any).
        struct GapGoal
        {
            uint32_t bonus;        // +0x00 the bonus objective's index
            uint32_t startObject;  // +0x04
            uint32_t startSurface; // +0x08
            uint32_t endObject;    // +0x0C
            uint32_t endSurface;   // +0x10
        };
        static_assert(sizeof(GapGoal) == 0x14);

        // A grind sequence: up to five rails in order, by object and surface
        // (0: any). A rail named kNoRail ends the sequence early.
        constexpr uint32_t kGrindGoalSteps = 5;
        struct GrindStep
        {
            uint32_t rail;    // +0x00 the object's name hash
            uint32_t surface; // +0x04
        };
        struct GrindGoal
        {
            uint32_t bonus;                  // +0x00 the bonus objective's index
            GrindStep steps[kGrindGoalSteps]; // +0x04
        };
        static_assert(sizeof(GrindGoal) == 0x2C);
        constexpr uint32_t kNoRail = 0x00053A0Cu; // the name hash of "NULL"

        // nameHash at compile time.
        constexpr uint32_t textHash(const char *text)
        {
            uint32_t hash = 0;
            for (; *text; ++text)
            {
                hash = (hash << 4) + static_cast<uint8_t>(*text);
                const uint32_t high = hash & 0xF0000000u;
                if (high)
                    hash ^= high >> 24;
                hash &= ~high;
            }
            return hash;
        }
        static_assert(textHash("NOTAG") == 5458007u);

        // What the script data holds where a goal names no surface: loading
        // turns it into 0, any. The number is the hash of "NOTAG"; that text
        // is in no build, so the name is the likeliest of its five-letter
        // readings, not a proven one.
        constexpr uint32_t kDataAnySurface = textHash("NOTAG");

        // A gap the skater jumped into: a heap record, listed from
        // g_gapsUnderWay, with the end still to come.
        struct GapUnderWay
        {
            uint32_t bonus;      // +0x00 the bonus objective's index
            uint32_t endObject;  // +0x04
            uint32_t endSurface; // +0x08
            ee::Ptr<GapUnderWay> next; // +0x0C
        };
        static_assert(sizeof(GapUnderWay) == 0x10);

        // A grind sequence begun: a heap record, listed from g_sequencesUnderWay.
        struct SequenceUnderWay
        {
            uint32_t goal; // +0x00 its index in g_grindGoals
            uint32_t step; // +0x04 the rail it wants next
            uint32_t done; // +0x08 1 once the last rail is ground
            ee::Ptr<SequenceUnderWay> next;     // +0x0C
            ee::Ptr<SequenceUnderWay> previous; // +0x10
        };
        static_assert(sizeof(SequenceUnderWay) == 0x14);

        // A chunk of the level's script data; its records follow.
        struct ScriptChunk
        {
            uint32_t hash;        // +0x00 the name of what it holds
            uint32_t count;       // +0x04
            uint32_t recordBytes; // +0x08
        };
        static_assert(sizeof(ScriptChunk) == 12);

        // A row of the experience table: what a challenge gives, by stat.
        struct ChallengeReward
        {
            int32_t experience[kStatCount];
        };
        static_assert(sizeof(ChallengeReward) == 28);

        // Where the skater was when a jump or line ended: a crash message's data.
        struct alignas(16) CrashSite
        {
            uint32_t object;  // the object's name hash, 0 with none
            uint32_t surface;
        };

        // soundEvent's events.
        constexpr uint32_t kSoundChallengeComplete = 63, kSoundGapComplete = 64, kSoundBonusComplete = 65, kSoundCareerUnlock = 66;

        constexpr int32_t kLevelMuseum = 6; // opens no next level
        constexpr uint64_t kLootPowerSkates = 0x100u;
        constexpr uint32_t kTricksPowerSkates = 0x500u;
        constexpr uint64_t kUnlockSkaterBits = 0xFFFFFu; // the unlock word's low 20 bits
        constexpr ee::Float kUnlockMessageSeconds = ee::Float(3.5f);

        constexpr uint32_t kTextInit = sym::g_textInit2, kTextActive = sym::g_textActive, kTextWait = sym::g_textWait, kTextComplete = sym::g_textComplete2;
        constexpr uint32_t kTextWaitSuccess = sym::g_textWaitSuccess, kTextWaitFail = sym::g_textWaitFail, kTextObjective = sym::g_textObjective;
        constexpr uint32_t kTextBonusObjective = sym::g_textBonusObjective, kTextGapObjective = sym::g_textGapObjective, kTextGrindSequence = sym::g_textGrindSequence;
        constexpr uint32_t kTextNewLevelUnlocked = sym::g_textNewLevelUnlocked; // "New Level Unlocked!"
        constexpr uint32_t kTextNewEditorPieces = sym::g_textNewEditorPiecesUnlocked;
        constexpr uint32_t kTextCinematicUnlocked = sym::g_textCinematicUnlocked;
        constexpr uint32_t kTextPowerSkatesUnlocked = sym::g_textPowerSkatesUnlocked;
        constexpr uint32_t kTextSecretCharacterUnlocked = sym::g_textSecretCharacterUnlocked;

        inline ee::Global<int32_t, sym::g_levelNumber> g_levelNumber;
        inline ee::Global<uint32_t, sym::g_hiddenObjectiveCount> g_hiddenObjectiveCount;
        inline ee::Global<uint32_t, sym::g_shownObjectiveCount> g_shownObjectiveCount;
        inline ee::Global<uint32_t, sym::g_bonusObjectives> g_bonusObjectives;
        inline ee::Global<uint32_t, sym::g_bonusObjectiveCount> g_bonusObjectiveCount;
        inline ee::Global<uint32_t, sym::g_gapGoals> g_gapGoals;
        inline ee::Global<uint32_t, sym::g_gapGoalCount> g_gapGoalCount;
        inline ee::Global<uint32_t, sym::g_grindGoals> g_grindGoals;
        inline ee::Global<uint32_t, sym::g_grindGoalCount> g_grindGoalCount;
        inline ee::Global<int32_t, sym::g_objectivesLevel> g_objectivesLevel;
        inline ee::Global<uint32_t, sym::g_objectiveStateBytes> g_objectiveStateBytes;
        inline ee::Global<uint32_t[8], sym::g_stateHashes> g_stateHashes; // by ObjectiveState
        inline ee::Global<uint32_t, sym::g_gapObjectiveHash> g_gapObjectiveHash;
        inline ee::Global<uint32_t, sym::g_grindSequenceHash> g_grindSequenceHash;
        inline ee::Global<ee::Ptr<GapUnderWay>, sym::g_gapsUnderWay> g_gapsUnderWay;
        inline ee::Global<ee::Ptr<SequenceUnderWay>, sym::g_sequencesUnderWay> g_sequencesUnderWay;
        inline ee::Global<uint32_t, sym::g_nextLevelPoints> g_nextLevelPoints;
        inline ee::Global<uint32_t, sym::g_editorPiecePoints> g_editorPiecePoints;
        inline ee::Global<uint16_t, sym::g_unlockedSkaters2> g_unlockedLevels;
        inline ee::Global<uint16_t, sym::g_mediaUnlocks> g_unlockedMedia;
        inline ee::Global<uint16_t, sym::g_lockedSecretPieces> g_unlockedEditorPieces;
        inline ee::Global<uint16_t, sym::g_mediaWatched> g_secretCharacterLevels; // by level: its secret character is won
        inline ee::Global<uint64_t, sym::g_unlockedSkaters> g_unlockWords;
        inline ee::Global<ee::Ptr<SavedObjective>[kProfileLevels], sym::levelsSaved> g_levelsSaved; // each level's objective states
        inline ee::Global<uint32_t[kCareerLevelCount], sym::careerLevels> g_careerLevels;           // the levels in order
        inline ee::Global<uint32_t[kProfileLevels], sym::nextLevelOf> g_nextLevelOf;                // the level each opens
        inline ee::Global<uint32_t[kProfileLevels], sym::secretCharacterOf> g_secretCharacterOf;    // by level

        enum ObjectiveState : uint32_t
        {
            kStateInit = 0,
            kStateActive = 1,
            kStateWait = 2,
            kStateComplete = 3,
            kStateWaitSuccess = 4,
            kStateWaitFail = 5,
            kChunkObjective = 6,
            kChunkBonusObjective = 7,
        };
        uint32_t stateHash(ObjectiveState state)
        {
            return (*g_stateHashes)[state];
        }

        ee::Ptr<Objective> objective(uint32_t index)
        {
            return ee::Ptr<Objective>(*g_objectives) + index;
        }
        ee::Ptr<BonusObjective> bonusObjective(uint32_t index)
        {
            return ee::Ptr<BonusObjective>(*g_bonusObjectives) + index;
        }
        ee::Ptr<GapGoal> gapGoal(uint32_t index)
        {
            return ee::Ptr<GapGoal>(*g_gapGoals) + index;
        }
        ee::Ptr<GrindGoal> grindGoal(uint32_t index)
        {
            return ee::Ptr<GrindGoal>(*g_grindGoals) + index;
        }
        ee::Ptr<SavedObjective> &levelSave(int32_t level)
        {
            return (*g_levelsSaved)[level];
        }
        ee::Ptr<skater::Player> playerOne()
        {
            return ee::Ptr<skater::Player>(skaterHandle(0));
        }

        // The next rail of a sequence or the far end of a gap is met when the
        // object is the one named and its surface is, or none is named.
        bool sameEnd(uint32_t wantedObject, uint32_t wantedSurface, uint32_t object, uint32_t surface)
        {
            return wantedObject == object && (wantedSurface == 0 || wantedSurface == surface);
        }

        uint32_t levelBit(int32_t level)
        {
            return 1u << (static_cast<uint32_t>(level) & 0x1Fu);
        }
        void addLevel(uint16_t &levels, int32_t level)
        {
            levels = static_cast<uint16_t>(levels | levelBit(level));
        }

        // An unsigned number as a float: past the sign bit it is halved with
        // the low bit kept, converted and doubled.
        ee::Float unsignedToFloat(uint32_t value)
        {
            if (static_cast<int32_t>(value) >= 0)
                return ee::toFloat(static_cast<int32_t>(value));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((value & 1u) | (value >> 1)));
            return half + half;
        }

        // Frees a list of records under way, each one's link read first.
        template <typename T>
        void freeList(ee::Ptr<T> record)
        {
            while (record)
            {
                const ee::Ptr<T> next = record->next;
                zge::heapFree(record);
                record = next;
            }
        }
    }

    // At the game's start: nothing loaded, the levels' states freed, the state
    // and chunk names hashed, the script load's callback set.
    void initObjectives()
    {
        *g_objectivesLevel = kProfileLevels;
        *g_objectives = 0;
        *g_objectiveCount = 0;
        *g_hiddenObjectiveCount = 0;
        *g_shownObjectiveCount = 0;
        *g_objectiveStateBytes = 0;
        *g_bonusObjectives = 0;
        *g_bonusObjectiveCount = 0;
        *g_gapGoalCount = 0;
        *g_gapGoals = 0;
        *g_gapsUnderWay = ee::Ptr<GapUnderWay>();
        *g_grindGoalCount = 0;
        *g_grindGoals = 0;
        *g_sequencesUnderWay = ee::Ptr<SequenceUnderWay>();
        for (uint32_t level = 0; level < kProfileLevels; ++level)
        {
            if (levelSave(level))
                zge::heapFree(levelSave(level));
            levelSave(level) = ee::Ptr<SavedObjective>();
        }
        script::scriptSetLoadCallback(sym::loadLevelObjectives);
        const auto hash = [](uint32_t name) {
            return static_cast<uint32_t>(zge::nameHash(ee::Ptr<const char>(name)));
        };
        (*g_stateHashes)[kStateInit] = hash(kTextInit);
        (*g_stateHashes)[kStateActive] = hash(kTextActive);
        (*g_stateHashes)[kStateWait] = hash(kTextWait);
        (*g_stateHashes)[kStateComplete] = hash(kTextComplete);
        (*g_stateHashes)[kStateWaitSuccess] = hash(kTextWaitSuccess);
        (*g_stateHashes)[kStateWaitFail] = hash(kTextWaitFail);
        (*g_stateHashes)[kChunkObjective] = hash(kTextObjective);
        (*g_stateHashes)[kChunkBonusObjective] = hash(kTextBonusObjective);
        *g_gapObjectiveHash = hash(kTextGapObjective);
        *g_grindSequenceHash = hash(kTextGrindSequence);
    }
    READABLE_FUNCTION_WAITS(initObjectives, Verified);

    // The saved state back on the lists, when it is this level's.
    void applySavedObjectives()
    {
        const int32_t level = *g_objectivesLevel;
        if (level != *g_levelNumber || !levelSave(level))
            return;
        ee::Ptr<SavedObjective> saved = levelSave(level);
        for (uint32_t i = 0; i < *g_objectiveCount; ++i, ++saved)
        {
            objective(i)->state = saved->state;
            objective(i)->done = saved->done;
        }
        for (uint32_t i = 0; i < *g_bonusObjectiveCount; ++i, ++saved)
        {
            bonusObjective(i)->state = saved->state;
            bonusObjective(i)->done = saved->done;
        }
    }
    READABLE_FUNCTION(applySavedObjectives, Verified);

    // The lists' state into this level's block, allocated once.
    void storeObjectives()
    {
        const int32_t level = *g_objectivesLevel;
        if (!levelSave(level))
            levelSave(level) = ee::Ptr<SavedObjective>(zge::heapAllocate(*g_objectiveStateBytes));
        ee::Ptr<SavedObjective> saved = levelSave(level);
        for (uint32_t i = 0; i < *g_objectiveCount; ++i, ++saved)
        {
            saved->state = objective(i)->state;
            saved->done = objective(i)->done;
        }
        for (uint32_t i = 0; i < *g_bonusObjectiveCount; ++i, ++saved)
        {
            saved->state = bonusObjective(i)->state;
            saved->done = bonusObjective(i)->done;
        }
    }
    READABLE_FUNCTION_WAITS(storeObjectives, Verified);

    // A level's restore: the block copied back and applied.
    void loadObjectiveState(ee::Address saved)
    {
        const int32_t level = *g_objectivesLevel;
        if (!levelSave(level))
            levelSave(level) = ee::Ptr<SavedObjective>(zge::heapAllocate(*g_objectiveStateBytes));
        zge::memoryCopy(levelSave(level), saved, *g_objectiveStateBytes);
        game::tail(applySavedObjectives);
    }
    READABLE_FUNCTION_WAITS(loadObjectiveState, Verified);

    // A level's save: stored, its size out, and the block.
    ee::Ptr<SavedObjective> saveObjectiveState(ee::Ptr<uint32_t> bytes)
    {
        storeObjectives();
        *bytes = *g_objectiveStateBytes;
        return levelSave(*g_objectivesLevel);
    }
    READABLE_FUNCTION_WAITS(saveObjectiveState, Verified);

    void endGapsUnderWay(uint32_t object, uint32_t surface);

    void endGrindSequences();

    // The skater's jump or line ended, landed or crashed. Every objective and
    // bonus objective waiting is told by a message to its script object, then
    // the gaps under way end where it was and the grind sequences end.
    void objectiveCrash(ee::Ptr<skater::Player> player, uint32_t landed)
    {
        ee::Local<zge::EngineMessage> message;
        ee::Local<CrashSite> site;
        site->object = player->object ? zge::nodeNameHash(ee::Ptr<zge::Node>(player->object)) : 0;
        site->surface = player->object ? skater::surfaceKind(player->objectNode, player->surface) : 0;

        const auto tellIfWaiting = [&](uint32_t state, ee::Ptr<uint32_t> objectName) {
            if (state != stateHash(kStateWait))
                return;
            const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(objectName, 1);
            if (!found)
                return;
            message->target = *found;
            message->sender = 0;
            message->type = stateHash(landed ? kStateWaitSuccess : kStateWaitFail);
            message->data = site.address();
            message->flags = zge::kMessageNow;
            zge::sendEngineMessage(message);
        };
        for (uint32_t i = 0; i < *g_objectiveCount; ++i)
            tellIfWaiting(objective(i)->state, &objective(i)->objectName);
        for (uint32_t i = 0; i < *g_bonusObjectiveCount; ++i)
            tellIfWaiting(bonusObjective(i)->state, &bonusObjective(i)->objectName);

        endGapsUnderWay(site->object, site->surface);
        endGrindSequences();
    }
    READABLE_FUNCTION_WAITS(objectiveCrash, Verified);

    // A bonus objective's state set; complete: its experience by stat and
    // its sound.
    void awardObjective(uint32_t index, uint32_t state)
    {
        const ee::Ptr<skater::Player> player = playerOne();
        const ee::Ptr<BonusObjective> bonus = bonusObjective(index);
        if (state == stateHash(kStateComplete))
        {
            for (uint32_t stat = 0; stat < kStatCount; ++stat)
                if (bonus->experience[stat] != 0)
                    skater::awardExperience(player, stat, bonus->experience[stat]);
            if (bonus->done == 0)
                bonus->done = 1;
            soundEvent(bonus->kind == kBonusKindGap ? kSoundGapComplete : kSoundBonusComplete, 1, 0, 0, 0);
        }
        bonus->state = state;
    }
    READABLE_FUNCTION_WAITS(awardObjective, Verified);

    // An objective's text on the overlay.
    int32_t showObjectiveText(uint32_t index)
    {
        return game::tail(aggressive::hud::showOverlayText, script::scriptString(objective(index)->description), 0, ee::Float(6.0f));
    }
    READABLE_FUNCTION_WAITS(showObjectiveText, Verified);

    // An objective's title.
    ee::Ptr<char> objectiveTitle(uint32_t index)
    {
        return game::tail(script::scriptString, objective(index)->name);
    }
    READABLE_FUNCTION(objectiveTitle, Verified);

    uint32_t currentObjectives() { return *g_objectives; }
    READABLE_FUNCTION(currentObjectives, Verified);
    uint32_t objectiveCount() { return *g_objectiveCount; }
    READABLE_FUNCTION(objectiveCount, Verified);
    uint32_t objectivesComplete() { return *g_shownObjectiveCount; }
    READABLE_FUNCTION(objectivesComplete, Verified);
    uint32_t bonusObjectives() { return *g_bonusObjectives; }
    READABLE_FUNCTION(bonusObjectives, Verified);
    uint32_t bonusObjectiveCount() { return *g_bonusObjectiveCount; }
    READABLE_FUNCTION(bonusObjectiveCount, Verified);

    // The skater took off from (object, surface): the gaps under way are
    // forgotten, and each gap with an end here is under way, with the other
    // end still to come and its bonus objective active.
    void goalJump(uint32_t object, uint32_t surface)
    {
        freeList(*g_gapsUnderWay);
        *g_gapsUnderWay = ee::Ptr<GapUnderWay>();

        ee::Ptr<GapUnderWay> last;
        for (uint32_t i = 0; i < *g_gapGoalCount; ++i)
        {
            const ee::Ptr<GapGoal> gap = gapGoal(i);
            const bool atStart = sameEnd(gap->startObject, gap->startSurface, object, surface);
            if (!atStart && !sameEnd(gap->endObject, gap->endSurface, object, surface))
                continue;

            const ee::Ptr<GapUnderWay> added(zge::heapAllocate(sizeof(GapUnderWay)));
            if (!*g_gapsUnderWay)
                *g_gapsUnderWay = added;
            else
                last->next = added;
            last = added;

            added->bonus = gap->bonus;
            added->endObject = atStart ? gap->endObject : gap->startObject;
            added->endSurface = atStart ? gap->endSurface : gap->startSurface;
            added->next = ee::Ptr<GapUnderWay>();
            bonusObjective(gap->bonus)->state = stateHash(kStateActive);
        }
    }
    READABLE_FUNCTION_WAITS(goalJump, Verified);

    // The skater came over (object, surface): a gap under way with its end
    // there and still active shows its name and worth as a trick and waits
    // for the landing.
    void tellGoalsOver(ee::Ptr<skater::Player> player, uint32_t object, uint32_t surface)
    {
        for (ee::Ptr<GapUnderWay> gap = *g_gapsUnderWay; gap; gap = gap->next)
        {
            if (!sameEnd(gap->endObject, gap->endSurface, object, surface))
                continue;
            const ee::Ptr<BonusObjective> bonus = bonusObjective(gap->bonus);
            if (bonus->state != stateHash(kStateActive))
                continue;
            constexpr uint32_t kNoMultiplierBonus = 0;
            const ee::Ptr<char> text = script::scriptString(bonus->name);
            aggressive::beginEndAndQueueObject(player, skater::newTextObject(text, kNoMultiplierBonus, unsignedToFloat(bonus->points)));
            bonus->state = stateHash(kStateWait);
        }
    }
    READABLE_FUNCTION_WAITS(tellGoalsOver, Verified);

    // The jump ended at (object, surface): each gap under way is complete if
    // this was its far end, else back to init; all are freed.
    void endGapsUnderWay(uint32_t object, uint32_t surface)
    {
        for (ee::Ptr<GapUnderWay> gap = *g_gapsUnderWay; gap; gap = gap->next)
        {
            if (sameEnd(gap->endObject, gap->endSurface, object, surface))
                awardObjective(gap->bonus, stateHash(kStateComplete));
            else
                bonusObjective(gap->bonus)->state = stateHash(kStateInit);
        }
        freeList(*g_gapsUnderWay);
        *g_gapsUnderWay = ee::Ptr<GapUnderWay>();
    }
    READABLE_FUNCTION_WAITS(endGapsUnderWay, Verified);

    // A grind on (rail, surface): sequences that start here are under way;
    // each under way that wanted this rail moves on, done after its last, and
    // one that wanted another rail is taken out of the list.
    //
    // Two bugs of the original are kept. `last` starts null on every call, so
    // a sequence that starts while others are already under way links itself
    // into low memory and never joins the list. And the list's first record
    // has no previous: taking it out writes its link into low memory too, and
    // leaves the list's head on the freed record.
    void goalGrind(uint32_t rail, uint32_t surface)
    {
        ee::Ptr<SequenceUnderWay> last;
        for (uint32_t i = 0; i < *g_grindGoalCount; ++i)
        {
            const GrindStep &first = grindGoal(i)->steps[0];
            if (!sameEnd(first.rail, first.surface, rail, surface))
                continue;

            const ee::Ptr<SequenceUnderWay> added(zge::heapAllocate(sizeof(SequenceUnderWay)));
            if (!*g_sequencesUnderWay)
            {
                *g_sequencesUnderWay = added;
                added->previous = ee::Ptr<SequenceUnderWay>();
            }
            else
            {
                last->next = added;
                added->previous = last;
            }
            last = added;

            added->goal = i;
            added->step = 0;
            added->done = 0;
            added->next = ee::Ptr<SequenceUnderWay>();
            bonusObjective(grindGoal(i)->bonus)->state = stateHash(kStateActive);
        }

        for (ee::Ptr<SequenceUnderWay> sequence = *g_sequencesUnderWay; sequence; sequence = sequence->next)
        {
            if (sequence->done)
                continue;
            const ee::Ptr<GrindGoal> goal = grindGoal(sequence->goal);
            const uint32_t step = sequence->step;
            if (sameEnd(goal->steps[step].rail, goal->steps[step].surface, rail, surface))
            {
                sequence->step = step + 1;
                if (step + 1 == kGrindGoalSteps || goal->steps[step + 1].rail == kNoRail)
                    sequence->done = 1;
            }
            else
            {
                const ee::Ptr<SequenceUnderWay> previous = sequence->previous;
                previous->next = sequence->next;
                zge::heapFree(sequence);
                sequence = previous;
            }
        }
    }
    READABLE_FUNCTION_WAITS(goalGrind, Verified);

    // The line ended: a sequence done completes its bonus objective, the
    // others go back to init; all are freed.
    void endGrindSequences()
    {
        for (ee::Ptr<SequenceUnderWay> sequence = *g_sequencesUnderWay; sequence; sequence = sequence->next)
        {
            const uint32_t bonus = grindGoal(sequence->goal)->bonus;
            if (sequence->done)
                awardObjective(bonus, stateHash(kStateComplete));
            else
                bonusObjective(bonus)->state = stateHash(kStateInit);
        }
        freeList(*g_sequencesUnderWay);
        *g_sequencesUnderWay = ee::Ptr<SequenceUnderWay>();
    }
    READABLE_FUNCTION_WAITS(endGrindSequences, Verified);

    // The script's load callback: the data's chunks walked, the lists set to
    // the records in place, the surfaces the data leaves open cleared. The
    // size of the state a level saves.
    int32_t loadLevelObjectives()
    {
        ee::Local<uint32_t> dataBytes;
        *g_objectivesLevel = *g_levelNumber;
        // The data walked by its words, as the level keeps its records (the script data a byte blob).
        uint32_t chunkAt = ee::Address(aggressive::script::scriptData(dataBytes));
        if (chunkAt == 0)
            return 0;

        for (uint32_t read = 0; chunkAt != 0 && read < *dataBytes;)
        {
            const ee::Ptr<ScriptChunk> chunk(chunkAt);
            const uint32_t records = chunkAt + sizeof(ScriptChunk);
            bool known = true;
            if (chunk->hash == stateHash(kChunkObjective))
            {
                *g_objectives = records;
                *g_objectiveCount = chunk->count;
                for (uint32_t i = 0; i < chunk->count; ++i)
                    if (objective(i)->kind == kHiddenChallenge)
                        ++*g_hiddenObjectiveCount;
                *g_shownObjectiveCount = chunk->count - *g_hiddenObjectiveCount;
            }
            else if (chunk->hash == stateHash(kChunkBonusObjective))
            {
                *g_bonusObjectives = records;
                *g_bonusObjectiveCount = chunk->count;
            }
            else if (chunk->hash == *g_gapObjectiveHash)
            {
                *g_gapGoals = records;
                *g_gapGoalCount = chunk->count;
                const auto clear = [](uint32_t &surface) {
                    if (surface == kDataAnySurface)
                        surface = 0;
                };
                for (uint32_t i = 0; i < chunk->count; ++i)
                {
                    clear(gapGoal(i)->startSurface);
                    clear(gapGoal(i)->endSurface);
                }
            }
            else if (chunk->hash == *g_grindSequenceHash)
            {
                *g_grindGoals = records;
                *g_grindGoalCount = chunk->count;
                for (uint32_t i = 0; i < chunk->count; ++i)
                    for (GrindStep &step : grindGoal(i)->steps)
                        if (step.surface == kDataAnySurface)
                            step.surface = 0;
            }
            else
                known = false;

            // A bug kept: a chunk of any other kind is stepped over without
            // its header, so the next one is read that many bytes early,
            // though `read` counts the header as passed.
            const uint32_t recordBytes = chunk->recordBytes * chunk->count;
            read += sizeof(ScriptChunk) + recordBytes;
            chunkAt = (known ? records : chunkAt) + recordBytes;
        }

        const uint32_t bytes = (*g_bonusObjectiveCount + *g_objectiveCount) * sizeof(SavedObjective);
        *g_objectiveStateBytes = bytes;
        return static_cast<int32_t>(bytes);
    }
    READABLE_FUNCTION(loadLevelObjectives, Verified);

    namespace
    {
        bool everyCareerCinematicUnlocked(ee::Ptr<SkaterProfile> profile)
        {
            for (const uint32_t level : *g_careerLevels)
                if ((profile->levels[level].rewards & kRewardCinematic) == 0)
                    return false;
            return true;
        }
        bool everyObjectiveDone()
        {
            for (uint32_t i = 0; i < *g_objectiveCount; ++i)
                if (objective(i)->done == 0)
                    return false;
            return true;
        }
    }

    // A challenge was just completed (not in the Tutorial): what its points
    // and count unlock: the next level, the editor's pieces, the level's
    // cinematic; all the career's cinematics: the power skates and every stat
    // up; every objective done: the secret character.
    void checkCareerProgress(ee::Ptr<skater::Player> player)
    {
        if (*g_levelNumber == static_cast<int32_t>(kLevelTutorial))
            return;
        const ee::Ptr<SkaterProfile> profile = skaterProfile(player);
        const int32_t level = *g_objectivesLevel;
        LevelRecord &record = profile->levels[level];
        const uint32_t points = record.challengePoints;
        bool told = false;
        const auto unlocked = [&](uint32_t text) {
            hud::showMessage(text, kUnlockMessageSeconds);
            told = true;
        };

        if (points >= *g_nextLevelPoints && (record.rewards & kRewardNextLevel) == 0)
        {
            record.rewards |= kRewardNextLevel;
            if (level != kLevelMuseum)
            {
                const uint32_t next = (*g_nextLevelOf)[level];
                profile->experienceMultiplier = static_cast<uint8_t>(profile->experienceMultiplier + 1);
                profile->bytes |= static_cast<uint32_t>(uint64_t(1) << (next & 0x3Fu)); // this skater's opened levels
                addLevel(*g_unlockedLevels, static_cast<int32_t>(next));
                unlocked(kTextNewLevelUnlocked);
            }
        }
        if (points >= *g_editorPiecePoints && (record.rewards & kRewardEditorPieces) == 0)
        {
            record.rewards |= kRewardEditorPieces;
            addLevel(*g_unlockedEditorPieces, level);
            unlocked(kTextNewEditorPieces);
            addLevel(profile->editorPieceLevels, level);
        }

        if (static_cast<uint32_t>(record.goalsDone) >= *g_shownObjectiveCount)
        {
            if ((record.rewards & kRewardCinematic) == 0)
            {
                record.rewards |= kRewardCinematic;
                addLevel(*g_unlockedMedia, level);
                unlocked(kTextCinematicUnlocked);
            }
            if (everyCareerCinematicUnlocked(profile))
            {
                profile->lootFlags |= kLootPowerSkates;
                unlocked(kTextPowerSkatesUnlocked);
                for (uint32_t stat = 0; stat < kStatCount; ++stat)
                    skater::levelUpAttribute(player, stat);
                profile->unlockedTricks = static_cast<uint16_t>(profile->unlockedTricks | kTricksPowerSkates);
            }
        }

        if (everyObjectiveDone() && (record.rewards & kRewardSecretCharacter) == 0)
        {
            record.rewards |= kRewardSecretCharacter;
            *g_unlockWords |= levelBit(static_cast<int32_t>((*g_secretCharacterOf)[level])) & kUnlockSkaterBits;
            unlocked(kTextSecretCharacterUnlocked);
            addLevel(*g_secretCharacterLevels, level);
        }
        if (told)
            soundEvent(kSoundCareerUnlock, 0, 0, 0, 0);
    }
    READABLE_FUNCTION_WAITS(checkCareerProgress, Verified);

    // An objective's state set; complete, the first time: its points in the
    // level's record, the career's check, the experience by stat (more for
    // the later kinds); any complete: the sound. The Restart override hooks
    // it to see challenges end.
    void completeChallenge(uint32_t index, uint32_t state)
    {
        const ee::Ptr<skater::Player> player = playerOne();
        const ee::Ptr<Objective> entry = objective(index);
        if (state == stateHash(kStateComplete))
        {
            if (entry->done == 0)
            {
                LevelRecord &record = skaterProfile(player)->levels[*g_objectivesLevel];
                record.challengePoints += entry->points;
                if (entry->kind != kHiddenChallenge)
                    record.goalsDone += 1;
                entry->done = 1;
                checkCareerProgress(player);

                const ChallengeReward &reward = ee::Ptr<ChallengeReward>(sym::challengeRewards)[entry->reward];
                for (uint32_t stat = 0; stat < kStatCount; ++stat)
                {
                    const uint32_t experience = static_cast<uint32_t>(reward.experience[stat]);
                    if (experience != 0)
                    {
                        const uint32_t multiplier = (entry->kind >= 3 ? 2 : entry->kind) + 1;
                        skater::awardExperience(player, stat, static_cast<int32_t>(multiplier * experience));
                    }
                }
            }
            soundEvent(kSoundChallengeComplete, 0, 0, 0, 0);
        }
        entry->state = state;
    }
    READABLE_FUNCTION_WAITS(completeChallenge, Verified);

    void objectiveHookNoopA()
    {
    }
    READABLE_FUNCTION(objectiveHookNoopA, Draft);

    void objectiveHookNoopB()
    {
    }
    READABLE_FUNCTION(objectiveHookNoopB, Draft);

    void objectiveHookNoopC()
    {
    }
    READABLE_FUNCTION(objectiveHookNoopC, Draft);

    // objectiveFlyThru: (was the sweep's objectiveSubtext) the objective's
    // fly-through camera animation (a script string), or 0 when it has none.
    ee::Ptr<char> objectiveFlyThru(uint32_t index)
    {
        const ee::Ptr<Objective> goal = objective(index);
        if (!goal->hasFlyThru)
            return ee::Ptr<char>();
        return script::scriptString(goal->flyThru);
    }
    READABLE_FUNCTION(objectiveFlyThru, Draft);
}
