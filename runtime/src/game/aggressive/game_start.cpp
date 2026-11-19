// The game's set-up and shutdown: the systems in order, the save state's
// defaults, the level records, and the short countdown before a top-level
// state change.

#include "aggressive/career.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/skater/action_tricks.h"
#include "aggressive/skater/skater.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/assets/asset_library.h"
#include "zge/platform/save_system.h"
#include "zge/sound/sound.h"
#include "zge/collision/dynamics.h"
#include "aggressive/sound/sound_mode.h"
#include "aggressive/camera.h"
#include "aggressive/game_save.h"
#include "aggressive/pads.h"
#include "aggressive/sound/events.h"
#include "aggressive/fx/particles.h"
#include "aggressive/level/park_save.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include "sym/aggressive/game_start.cpp.h"

namespace aggressive { int32_t switchGameState(uint32_t); }

namespace aggressive
{
    namespace
    {
        constexpr uint32_t kSetStringTable = sym::setStringTable;
        constexpr uint32_t kDebugUnlockEverything = sym::debugUnlockEverything;
        constexpr uint32_t kInitZguiSystem = sym::initZguiSystem;
        constexpr uint32_t kShutdownZguiSystem = sym::shutdownZguiSystem;

        constexpr uint32_t kZguiGlobals = sym::g_zgui;

        constexpr uint32_t kLevels = 14;
        // The defaults of a whole level come from defaultBestRuns over the
        // "test" entries.
        inline ee::Global<uint32_t[kLevels], sym::names> g_summaryLevelNames; // the levels' names
        constexpr uint32_t kTestName = sym::g_textTest; // "test"
        // The rows of a level's best runs that start with a value of their
        // own after the defaults are copied in. What each row counts is not
        // settled: they keep their row number.
        constexpr uint32_t kSeededRow5 = 5, kSeededRow6 = 6, kSeededRow8 = 8, kSeededRow12 = 12, kSeededRow13 = 13;

        constexpr uint32_t kStartButton = 0x4000, kAcceptMask = 0x80, kOptionMask = 0x40, kCancelMask = 0x10;

        inline ee::Global<uint32_t, sym::g_uiAssets> g_uiAssets;
        inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_uiStrings> g_uiStrings;
        inline ee::Global<uint64_t, sym::g_debugDisplay> g_debugDisplayWord;
        inline ee::Global<uint16_t, sym::g_unlockedSkaters2> g_unlockedSkaters;
        inline ee::Global<uint16_t, sym::g_mediaUnlocks> g_mediaUnlocks;
        inline ee::Global<uint16_t, sym::g_mediaWatched> g_mediaWatched;
        inline ee::Global<uint16_t, sym::g_unlockedCheats> g_unlockedCheats;
        inline ee::Global<int32_t, sym::g_careerCheats> g_careerCheats;
        inline ee::Global<int32_t, sym::g_gameplayCheats> g_gameplayCheats;
        inline ee::Global<uint8_t, sym::g_masterVolume> g_masterVolume;
        inline ee::Global<uint8_t, sym::g_musicVolume> g_musicVolume;
        inline ee::Global<uint8_t, sym::g_soundVolume> g_soundVolume;
        inline ee::Global<uint16_t, sym::g_lockedSecretPieces> g_lockedSecretPieces;
        inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_stringTableBlock> g_stringTableBlock;
        inline ee::Global<uint8_t, sym::g_frameCountdown> g_frameCountdown;
        inline ee::Global<uint8_t, sym::g_pendingStateDelay> g_pendingStateDelay;
        inline ee::Global<int32_t, sym::g_nextGameState> g_nextGameState;

        constexpr uint32_t kLevelBits = 13;
        constexpr uint64_t kClearedOnReset = (3u << 20) | 0xC00000u, kStartingPieces = 0x3000000u;
        constexpr uint16_t kStartingSkaters = 0x2A0u;
        constexpr uint8_t kFullVolume = 127, kStartingMusic = 51;
    }

    namespace
    {
        ee::Ptr<char> nameOf(BestRun &run)
        {
            return run.name;
        }
        ee::Ptr<BestRun> firstOf(LevelBestRuns &runs)
        {
            return &runs[0];
        }
    }

    // debugUnlockEverything: the save state's defaults: every level's first records,
    // the first levels open, the volumes. The names3 row calls it an unlock of
    // everything; it runs at every start.
    void debugUnlockEverything()
    {
        *g_unlocks &= ~kClearedOnReset;
        for (uint32_t bit = 0;;)
        {
            *g_unlocks |= uint64_t(1) << bit;
            if (++bit >= kLevelBits)
                break;
        }
        *g_unlockedSkaters |= kStartingSkaters;
        *g_mediaUnlocks = 0;
        *g_mediaWatched = 0;
        *g_unlockedCheats = 0;
        *g_careerCheats = 0;
        *g_gameplayCheats = 0;
        *g_unlocks |= kStartingPieces;
        *g_lockedSecretPieces = 0;
        for (LevelBestRuns &runs : *g_bestRuns)
        {
            for (BestRun &run : runs)
            {
                run.value = 0;
                zge::stringCopy(nameOf(run), kTestName);
            }
            zge::memoryCopy(firstOf(runs), sym::defaultBestRuns, sizeof(LevelBestRuns));
            runs[kSeededRow5].value = 1000;
            runs[kSeededRow6].value = 0x00013F8A;
            runs[kSeededRow8].value = 1;
            runs[kSeededRow12].value = 200;
            runs[kSeededRow13].value = 1;
        }
        *g_soundVolume = kFullVolume;
        *g_musicVolume = kStartingMusic;
        *g_masterVolume = kFullVolume;
    }
    READABLE_FUNCTION(debugUnlockEverything, Verified);

    void initZguiSystem();

    // initGame: the game's start-up, once, in order.
    int32_t initGame()
    {
        aggressive::fx::debugMenuSecondHook();
        initZguiSystem();
        aggressive::level::registerParkSaveClient();
        zgui::g_zgui->startButton = kStartButton;
        zgui::g_zgui->acceptButton = kAcceptMask;
        zgui::g_zgui->cancelButton = kCancelMask;
        zgui::g_zgui->optionButton = kOptionMask;
        for (const auto &function : {std::pair{sym::g_textInlineDebugMenuSelect, sym::inlineDebugMenuSelect}, std::pair{sym::g_textInlineTuneGuiGroupInit, sym::inlineTuneGuiGroupInit}, std::pair{sym::g_textInlineTuneGuiDetailInit, sym::inlineTuneGuiDetailInit},
                                     std::pair{sym::g_textInlineTuneDown, sym::inlineTuneDown}, std::pair{sym::g_textInlineTuneUp, sym::inlineTuneUp}})
            zge::zgui::registerScriptFunction(function.first, function.second);
        *g_uiAssets = zge::openAssetLibrary(sym::g_textAssetsUilibZal); // "assets\\uilib.zal"
        debugUnlockEverything();
        registerGameSaveClient();
        initPlayerPads();
        skater::initTrickMasks();
        initSound();
        setGameMode(0);
        const int32_t alwaysZero = fx::alwaysZero();
        if (alwaysZero == 0)
        {
            for (int32_t index = 0;;)
            {
                skater::resetPlayerProfile(ee::Ptr<skater::SkaterProfile>(index * kSkaterRecordSize + kSkaterRecords), index & 0xFFu);
                for (uint32_t level = 0; level < kCareerLevels; ++level)
                    *careerLevelState(index, level) = CareerLevelState{};
                if (++index >= static_cast<int32_t>(kSkaterRecordCount))
                    break;
            }
        }
        for (uint8_t &skaterChosen : g_gameState->playerSkaters)
            skaterChosen = 0;
        g_gameState->type = GameType::FreeSkate;
        g_gameState->topStateId = kTopStateNone;
        g_gameState->level = 14;
        g_gameState->topState = ee::Ptr<TopState>(0u);
        switchGameState(kTopStateFrontEnd);
        *g_debugDisplayWord &= ~uint64_t(7);
        const ee::Ptr<zge::StringTable> table = zge::loadStringTableFile(sym::g_stringTableName);
        *g_stringTableBlock = table;
        zge::setStringTable(table);
        *g_uiStrings = *g_stringTableBlock;
        return kZguiGlobals;
    }
    READABLE_FUNCTION_WAITS(initGame, Verified);

    uint32_t shutdownZguiSystem();

    namespace
    {
        namespace tail
        {
        int32_t setStringTable(uint32_t table) { return ee::tailCall<int32_t>(kSetStringTable, table); }
        }
    }

    // shutdownGame.
    int32_t shutdownGame()
    {
        releaseFollowCameras();
        shutdownPlayerPads();
        zge::shutdownDynamicsPool();
        shutdownSound();
        shutdownGameSave();
        stopParkSaveSystem();
        shutdownZguiSystem();
        zge::heapFree(*g_stringTableBlock);
        return tail::setStringTable(0);
    }
    READABLE_FUNCTION_WAITS(shutdownGame, Verified);

    // initZguiSystem.
    void initZguiSystem()
    {
        zge::zgui::initZgui(0x000C0000u);
        game::tail(registerZguiIconRoll);
    }
    READABLE_FUNCTION_WAITS(initZguiSystem, Verified);

    // shutdownZguiSystem.
    uint32_t shutdownZguiSystem()
    {
        return game::tail(zge::zgui::shutdownZgui);
    }
    READABLE_FUNCTION_WAITS(shutdownZguiSystem, Verified);

    // updateLevelRecord: a level's float record (a time or a distance): beaten,
    // it keeps the new value and the skater's name.
    int32_t updateLevelRecord(uint32_t row, uint32_t level, ee::Address name, ee::Float value)
    {
        BestRun &record = (*g_bestRuns)[level][row];
        if (!(record.measure < value))
            return 0;
        record.measure = value;
        zge::stringCopy(nameOf(record), name);
        return 1;
    }
    READABLE_FUNCTION(updateLevelRecord, Verified);

    // recordBestScore: the same for a score.
    int32_t recordBestScore(uint32_t row, uint32_t level, uint32_t value, ee::Address name)
    {
        BestRun &record = (*g_bestRuns)[level][row];
        if (!(record.points < value))
            return 0;
        record.points = value;
        zge::stringCopy(nameOf(record), name);
        return 1;
    }
    READABLE_FUNCTION(recordBestScore, Verified);

    // summaryScreenEntry: a level's name in the summary screen.
    uint32_t summaryScreenEntry(uint32_t index)
    {
        return (*g_summaryLevelNames)[index];
    }
    READABLE_FUNCTION(summaryScreenEntry, Verified);

    // resetFrameCountdown.
    uint32_t resetFrameCountdown()
    {
        *g_frameCountdown = 0;
        return 1;
    }
    READABLE_FUNCTION(resetFrameCountdown, Verified);

    // stepFrameCountdown: the pending state starts when the delay has passed.
    uint32_t stepFrameCountdown()
    {
        const int32_t count = *g_frameCountdown + 1;
        const bool due = *g_pendingStateDelay < (count & 0xFF);
        *g_frameCountdown = static_cast<uint8_t>(count);
        if (due)
            switchGameState(static_cast<uint8_t>(*g_nextGameState));
        return 1;
    }
    READABLE_FUNCTION_WAITS(stepFrameCountdown, Verified);

    // gameNoop: a mode table's empty slot.
    void gameNoop()
    {
    }
    READABLE_FUNCTION(gameNoop, Verified);

    // setPendingStateAction.
    void setPendingStateAction(uint32_t nextState)
    {
        *g_nextGameState = static_cast<int32_t>(nextState);
    }
    READABLE_FUNCTION(setPendingStateAction, Verified);

    // switchGameState: the top state changes: the old one's exit, the new
    // one's enter, which is handed the game state.
    int32_t switchGameState(uint32_t state)
    {
        const uint32_t next = state & 0xFFu;
        const ee::Ptr<TopState> old = g_gameState->topState;
        if (old && old->exit)
            old->exit();
        g_gameState->topStateId = static_cast<int32_t>(next);
        const uint32_t row = sym::topStates + next * static_cast<uint32_t>(sizeof(TopState));
        const ee::Ptr<TopState> record(row);
        g_gameState->topState = record;
        if (record->enter)
            return record->enter(ee::Ptr<GameState>(g_gameState.get()));
        return static_cast<int32_t>(row);
    }
    READABLE_FUNCTION(switchGameState, Verified);

    // setPendingStateDelay: the countdown before the state change, at least a frame.
    int32_t setPendingStateDelay(uint32_t frames)
    {
        const uint32_t delay = frames & 0xFFu;
        if (delay == 0)
        {
            *g_pendingStateDelay = 1;
            return 1;
        }
        *g_pendingStateDelay = static_cast<uint8_t>(delay);
        return 1;
    }
    READABLE_FUNCTION(setPendingStateDelay, Verified);

    namespace
    {
        constexpr uint32_t kParkSaveUnderway = sym::g_parkSaveUnderway;
        inline ee::Global<int32_t, sym::g_parkSaveUnderway> g_parkSaveUnderway; // g_parkSaveUnderway; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_parkSaving> g_parkFileMenuShown; // g_parkFileMenuShown; its name is a low-confidence guess (readable_names.tsv)
        inline ee::Global<int32_t, sym::g_parkSaveBuffer> g_parkSaveBuffer;
        inline ee::Global<int32_t, sym::g_parkSaveCallback> g_parkSaveCallback; // g_parkSaveCallback; its name is a low-confidence guess (readable_names.tsv)
    }

    namespace
    {
        int saveApiLoadChoice(uint32_t xC, uint32_t delayMilliseconds) { return game::tail(zge::saveApiLoadChoice, xC, delayMilliseconds); }
    }

    namespace
    {
        int saveApiSaveChoice(uint32_t xC, uint32_t delayMilliseconds) { return game::tail(zge::saveApiSaveChoice, xC, delayMilliseconds); }
    }
}
