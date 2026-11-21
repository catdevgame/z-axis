// The pause menu's pages (the "inlinePause..." functions its zgui scripts
// call): a skater's attributes, the level's objectives and what they earn,
// a challenge's fly-through and the sound sliders. The objective list comes
// from the level's scripts; a page shows it as lines appended to the widget
// text of four parallel columns (STRCAT Items {...}): "!" is the zgui's
// highlight; "~" and "`" are the font's check and cross (inferred from where
// the pages use them).

#include "aggressive/camera.h"
#include "aggressive/globals.h"
#include "aggressive/level/level_object.h"
#include "aggressive/level/pause_parts.h"
#include "aggressive/script/script.h"
#include "aggressive/skater/skater.h"
#include "aggressive/volume_menu.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "zge/zgui/widget.h"

#include "zge/scene/animator.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "zge/scene/animation.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "aggressive/sound/events.h"
#include "aggressive/npc.h"
#include "aggressive/game_start.h"
#include "aggressive/play_modes.h"
#include "aggressive/level/objectives.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/level/pause_menus.cpp.h"

namespace aggressive
{
    namespace
    {
        using parts::levelObject;
        using parts::levelPlayer;
        using parts::setLevelState;
        using zgui::Node;

        using pause::find;
        using pause::kCurrentObjectives;
        using pause::kRunScript;
        using pause::Objective;
        using pause::run;
        constexpr uint32_t kHiddenChallenge = 3;

        // A skater attribute (Player +0x638, SkaterProfile::stats).
        struct AttributeRecord
        {
            uint8_t _00;
            uint8_t level;        // +0x01
            uint8_t powerUp;      // +0x02 with the skates and power-ups won
            uint8_t _03;
            uint32_t experience;  // +0x04
            uint8_t _08[4];
        };
        static_assert(sizeof(AttributeRecord) == 12);
        constexpr uint32_t kAttributeRecords = 0x638u; // Player
        constexpr uint32_t kAttributeCount = 7;

        constexpr uint64_t kLootSkates = 0x100, kLootPowerUp = 0x80;
        constexpr int32_t kLastLevel = 6;

        constexpr uint32_t kMemset = sym::memset;      // libc
        constexpr uint32_t kFollowCamera = sym::g_followCameras;

        constexpr uint32_t kEndLevelQuit = 2u;
        constexpr uint32_t kLevelStateFlyThru = 11u;
        constexpr int32_t kPlayPhaseFlyThru = 6;
        constexpr uint32_t kFollowFlyThruMode = 7u;
        constexpr int32_t kTopLevelForPoints = 6;

        // The strings.
        inline ee::Global<uint32_t[7], sym::attributeNames> g_attributeNames; // the attributes' widget names
        constexpr uint32_t kAttributesMenu = sym::g_textInGameUIPauseMenusFrameAttributes;     // "InGameUI.PauseMenus.Frame.Attributes.Menu"
        constexpr uint32_t kGrindEarnedVisible = sym::g_textGrindEarnedVisible; // "Grind.Earned.Visible"
        constexpr uint32_t kAdvancement = sym::g_textAdvancement;        // "Advancement"
        constexpr ee::Ptr<const char> kEarnedAmount{sym::g_textEarnedAmount};       // "Earned.Amount"
        constexpr ee::Ptr<const char> kPowerUpAmount{sym::g_textPowerUpAmount};      // "PowerUp.Amount"
        constexpr ee::Ptr<const char> kSkatesAmount{sym::g_textSkatesAmount};       // "Skates.Amount"
        constexpr ee::Ptr<const char> kNeeded{sym::g_textNeeded};             // "needed"
        constexpr ee::Ptr<const char> kExperience{sym::g_textXp};         // "xp"
        constexpr uint32_t kPlayRefresh = sym::g_textPlayRefresh;        // "PLAY Refresh"
        constexpr uint32_t kPlaySwap = sym::g_textPlaySwap;           // "PLAY Swap"

        constexpr uint32_t kObjectivesData = sym::g_textInGameUIPauseMenusFrameObjectives;     // "InGameUI.PauseMenus.Frame.Objectives.Data.Objectives"
        constexpr uint32_t kTitles = sym::g_textTitles;             // "Titles"
        constexpr uint32_t kPoints = sym::g_textPoints;             // "Points"
        constexpr uint32_t kFlythru = sym::g_textFlythru;            // "Flythru"
        constexpr uint32_t kInit = sym::g_textInit;               // "init"
        constexpr uint32_t kComplete = sym::g_textComplete;           // "complete"
        constexpr uint32_t kClearItems = sym::g_textSetItems;         // "SET Items {""}"
        constexpr uint32_t kNewLine = sym::newLine;            // STRCAT Items {"\\n"}
        constexpr uint32_t kCheckGlyph = sym::checkGlyph;         // "~"
        constexpr uint32_t kCrossGlyph = sym::crossGlyph;         // "`"
        constexpr uint32_t kHighlightedItem = sym::g_textStrcatItems;    // STRCAT Items {"!%s"}
        constexpr uint32_t kItem = sym::g_textStrcatItems2;               // STRCAT Items {"%s"}
        constexpr uint32_t kDashes = sym::g_textStrcatItems5;             // STRCAT Items {"------------------------------"}
        constexpr uint32_t kUnknownPoints = sym::g_textStrcatItemsPts;      // STRCAT Items {"? pts"}
        constexpr uint32_t kHighlightedPoints = sym::g_textStrcatItemsPts2;  // STRCAT Items {"!%d pts"}
        constexpr uint32_t kItemPoints = sym::g_textStrcatItemsPts3;         // STRCAT Items {"%d pts"}
        constexpr uint32_t kFlyThruMark = sym::g_textStrcatItems6;        // STRCAT Items {"^"}
        constexpr uint32_t kInvisibleItem = sym::g_textStrcatItemsInvisible;      // STRCAT Items {"!!invisible"}

        constexpr uint32_t kObjectivesNode = sym::g_textObjectives;     // "Objectives"
        constexpr uint32_t kSelectedIndex = sym::g_textSelectedIndex;      // "SelectedIndex"
        constexpr uint32_t kChallengeDescription = sym::g_textChallengeDetailDescription; // "ChallengeDetail.Description"
        constexpr ee::Ptr<const char> kText{sym::g_textText};               // "Text"
        constexpr uint32_t kChallengeDetail = sym::g_textChallengeDetail;    // "ChallengeDetail"
        constexpr uint32_t kCurValue = sym::g_textCurValue;           // "CurValue"
        constexpr uint32_t kPlayOnChange = sym::g_textPlayOnChange;       // "PLAY OnChange"

        constexpr uint32_t kSummaryTitle = sym::g_textInGameUIPauseMenusFrameObjectives2;       // "InGameUI.PauseMenus.Frame.Objectives.Data.Summary.Title"
        constexpr uint32_t kData = sym::g_textData;               // "Data"
        constexpr uint32_t kTotalPoints = sym::totalPoints;        // "Total Current Points:  %s%d"
        constexpr uint32_t kNextLevelLine = sym::g_textSNextLevelN;      // "%s%s %sNext Level %s(%d)\n"
        constexpr uint32_t kEditorPieceLine = sym::g_textSEditorPieceLoadingScreens;    // "%s%s %sEditor Piece/Loading Screens %s(%d)\n"
        constexpr uint32_t kMoviesLine = sym::g_textLevelMoviesAllChallengesIncluding;         // "%s%s %s%s Level Movies %s(All Challenges, including locked area)\n"
        constexpr uint32_t kSecretCharacterLine = sym::g_textSSecretCharacterAllHiddenChallenge; // "%s%s %sSecret Character %s(All Hidden Challenges)\n"
        constexpr uint32_t kBlueColour = sym::g_textRgb, kWhiteColour = sym::g_textRgb2, kGoldColour = sym::g_textRgb3; // "#RGB{..}", 18, 18, 17 bytes with the 0

        inline ee::Global<uint32_t, sym::g_zguiCancelSound> g_zguiCancelSound;
        inline ee::Global<ee::Ptr<Node>, sym::g_uiRoot> g_uiRootNode;
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelNumber;
        inline ee::Global<uint32_t, sym::g_objectiveHashesPending> g_objectiveHashesPending;
        inline ee::Global<uint32_t, sym::g_detailHashPending> g_detailHashPending;
        inline ee::Global<uint32_t, sym::g_summaryHashPending> g_summaryHashPending;
        inline ee::Global<uint32_t, sym::g_flyThruHashPending> g_flyThruHashPending;
        // What the summary's two rewards ask of a level's points.
        inline ee::Global<int32_t, sym::g_nextLevelPoints> g_nextLevelPoints;
        inline ee::Global<int32_t, sym::g_editorPiecePoints> g_editorPiecePoints;
        inline ee::Global<uint32_t, sym::g_initHash> g_initHash;
        inline ee::Global<uint32_t, sym::g_completeHash> g_completeHash;
        inline ee::Global<uint32_t, sym::g_detailCompleteHash> g_detailCompleteHash;
        inline ee::Global<uint32_t, sym::g_summaryCompleteHash> g_summaryCompleteHash;
        inline ee::Global<uint32_t, sym::g_flyThruCompleteHash> g_flyThruCompleteHash;

        ee::Ptr<Objective> objectiveAt(uint32_t objectives, int32_t index)
        {
            return ee::Ptr<Objective>(objectives + static_cast<uint32_t>(index) * sizeof(Objective));
        }

        // A once-only hash of "complete".
        template <typename Pending, typename Hash>
        void hashCompleteOnce(Pending &pending, Hash &hash)
        {
            if (*pending)
            {
                *hash = zge::nameHash(ee::at<const char>(kComplete));
                *pending = 0;
            }
        }
    }

    // inlinePausePopulateAttributeMenu: the Attributes page: each attribute's level, what the
    // power-ups and skates won add, and the experience on its bar.
    void inlinePausePopulateAttributeMenu()
    {
        ee::Local<int32_t> value;
        ee::Local<int32_t> powerUps;
        ee::Local<int32_t> neededPoints;
        ee::Local<int32_t> visible;
        const ee::Ptr<zgui::Node> menu = find(*zgui::g_root, kAttributesMenu);
        const uint32_t player = parts::skaterHandle(0);
        const ee::Ptr<SkaterProfile> profile = skater::skaterProfile(levelPlayer(0));

        for (uint32_t attribute = 0; attribute < kAttributeCount; ++attribute)
        {
            const ee::Ptr<zgui::Node> widget = find(menu, (*g_attributeNames)[attribute]);
            *value = 0;
            *powerUps = 0;
            const ee::Ptr<zgui::Node> advancement = find(widget, kAdvancement);
            const ee::Ptr<AttributeRecord> record(player + kAttributeRecords + attribute * sizeof(AttributeRecord));

            *value = record->level;
            zgui::setVariable(widget, kEarnedAmount, value);
            const int32_t needed = static_cast<int32_t>(skater::levelPointsFor(*value));
            const uint64_t loot = profile->lootFlags;
            *neededPoints = needed;
            *powerUps = 0;
            if (loot & kLootSkates)
                *powerUps = 1;
            if (loot & kLootPowerUp)
                *powerUps += 1;
            *value = record->powerUp - *powerUps;
            zgui::setVariable(widget, kPowerUpAmount, value);
            zgui::setVariable(widget, kSkatesAmount, powerUps);
            if (*neededPoints == -1)
            {
                // At the top: the bar is full.
                const int32_t top = static_cast<int32_t>(skater::levelPointsFor(kTopLevelForPoints));
                *value = top;
                *neededPoints = top;
            }
            else
                *value = static_cast<int32_t>(record->experience);
            zgui::setVariable(advancement, kNeeded, neededPoints);
            zgui::setVariable(advancement, kExperience, value);
            run(widget, kPlayRefresh);
        }
        pause::copyInto(menu, kGrindEarnedVisible, visible);
        if (*visible == 0)
            run(menu, kPlaySwap);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateAttributeMenu, Verified);

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // inlinePausePopulateObjectivesMenu: the Objectives page: for each objective a line in the
    // four columns (check, title, points, fly-through mark). A hidden
    // challenge not yet done, and an objective in an area still locked, take
    // invisible lines. An objective still to be found shows dashes.
    void inlinePausePopulateObjectivesMenu()
    {
        ee::Local<char[0x400]> line;
        const uint32_t objectives = aggressive::level::currentObjectives();
        const uint32_t count = aggressive::level::objectiveCount();
        const ee::Ptr<SkaterProfile> profile = skater::skaterProfile(levelPlayer(0));
        memset(line, 0, 0x400);
        const ee::Ptr<zgui::Node> checks = find(*zgui::g_root, kObjectivesData);
        const ee::Ptr<zgui::Node> titles = find(checks, kTitles);
        const ee::Ptr<zgui::Node> points = find(checks, kPoints);
        const ee::Ptr<zgui::Node> flyThrus = find(checks, kFlythru);
        if (*g_objectiveHashesPending)
        {
            *g_initHash = zge::nameHash(ee::Ptr<const char>(kInit));
            *g_completeHash = zge::nameHash(ee::Ptr<const char>(kComplete));
            *g_objectiveHashesPending = 0;
        }
        // The level's key opens its locked area (LootFlag, bit level + 9).
        const bool areaLocked = !((profile->lootFlags >> ((*g_levelNumber + 9) & 0x3F)) & 1u);
        for (const ee::Ptr<zgui::Node> column : {checks, titles, points, flyThrus})
            run(column, kClearItems);

        for (uint32_t index = 0; index < count; ++index)
        {
            if (index != 0)
            {
                for (const ee::Ptr<zgui::Node> column : {checks, titles, points, flyThrus})
                    run(column, kNewLine);
            }
            const ee::Ptr<Objective> objective = ee::Ptr<Objective>(objectives) + index;
            bool invisible = objective->kind == kHiddenChallenge && objective->state != *g_completeHash;
            if (!invisible)
                invisible = objective->lockedArea != 0 && areaLocked;
            if (invisible)
            {
                for (const ee::Ptr<zgui::Node> column : {checks, titles, points, flyThrus})
                    run(column, kInvisibleItem);
                continue;
            }
            const uint32_t state = objective->state;
            if (state == *g_completeHash)
                zge::formatString(line, kHighlightedItem, kCheckGlyph);
            else
            {
                if (state == *g_initHash)
                    zge::formatString(line, kHighlightedItem, kCrossGlyph);
                else
                    zge::formatString(line, kItem, kCrossGlyph);
            }
            run(checks, line);
            if (objective->state == *g_initHash)
            {
                run(titles, kDashes);
                run(points, kUnknownPoints);
            }
            else if (objective->state == *g_completeHash)
            {
                const ee::Ptr<char> name = script::scriptString(objective->name);
                zge::formatString(line, kHighlightedItem, name);
                run(titles, line);
                zge::formatString(line, kHighlightedPoints, static_cast<int32_t>(objective->points));
                run(points, line);
            }
            else
            {
                const ee::Ptr<char> name = script::scriptString(objective->name);
                zge::formatString(line, kItem, name);
                run(titles, line);
                zge::formatString(line, kItemPoints, static_cast<int32_t>(objective->points));
                run(points, line);
                if (objective->hasFlyThru != 0)
                    run(flyThrus, kFlyThruMark);
            }
        }
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateObjectivesMenu, Verified);

    // inlinePausePopulateChallengeDetail: (the page,): the selected objective's description.
    void inlinePausePopulateChallengeDetail(pause::Arguments arguments)
    {
        ee::Local<int32_t> selected;
        const uint32_t objectives = aggressive::level::currentObjectives();
        *selected = -1;
        hashCompleteOnce(g_detailHashPending, g_detailCompleteHash);
        const ee::Ptr<zgui::Node> list = find(arguments[0], kObjectivesNode);
        const ee::Ptr<zgui::Node> description = find(arguments[0], kChallengeDescription);
        pause::copyInto(list, kSelectedIndex, selected);
        const ee::Ptr<char> text = script::scriptString(objectiveAt(objectives, *selected)->description);
        zgui::setVariable(description, kText, text);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateChallengeDetail, Verified);

    // inlinePausePopulateSummaryScreen: the Objectives page's summary: the level's points and,
    // once they are met, what they unlock: the next level, the editor piece
    // and loading screens, with all the challenges the level's movies, with
    // the hidden ones the secret character. The check and cross say which.
    void inlinePausePopulateSummaryScreen()
    {
        ee::Local<char[0x20]> blue;
        ee::Local<char[0x20]> white;
        ee::Local<char[0x20]> gold;
        ee::Local<char[0x400]> body;
        ee::Local<char[0x100]> line;
        std::memcpy(&blue[0], ee::Ptr<char>(kBlueColour), 18);
        std::memcpy(&white[0], ee::Ptr<char>(kWhiteColour), 18);
        const uint32_t nextLevelPoints = *g_nextLevelPoints;
        const int32_t editorPiecePoints = *g_editorPiecePoints;
        std::memcpy(&gold[0], ee::Ptr<char>(kGoldColour), 17);

        uint32_t allChallenges = 1, allObjectives = 1;
        const uint32_t objectives = aggressive::level::currentObjectives();
        const uint32_t count = aggressive::level::objectiveCount();
        const ee::Ptr<SkaterProfile> profile = skater::skaterProfile(levelPlayer(0));
        memset(body, 0, 0x400);
        const ee::Ptr<zgui::Node> title = find(*zgui::g_root, kSummaryTitle);
        if (*g_summaryHashPending)
        {
            *g_summaryCompleteHash = zge::nameHash(ee::Ptr<const char>(kComplete));
            *g_summaryHashPending = 0;
        }
        const ee::Ptr<zgui::Node> data = find(title, kData);

        for (uint32_t index = 0; index < count; ++index)
        {
            const ee::Ptr<Objective> objective = objectiveAt(objectives, static_cast<int32_t>(index));
            if (allChallenges && objective->kind != kHiddenChallenge && objective->state != *g_summaryCompleteHash)
                allChallenges = 0;
            allObjectives = allObjectives && objective->state == *g_summaryCompleteHash ? 1u : 0u;
        }

        const uint32_t points = profile->levels[*g_levelNumber].challengePoints;
        zge::formatString(body, kTotalPoints, gold, points);
        zgui::setVariable(title, kText, body);

        uint32_t belowFirst = 0;
        if (*g_levelNumber != kLastLevel)
        {
            belowFirst = points < nextLevelPoints ? 1u : 0u;
            uint32_t glyph = kCheckGlyph;
            if (belowFirst)
                glyph = kCrossGlyph;
            zge::formatString(body, kNextLevelLine, white, glyph, blue, gold, nextLevelPoints);
        }
        else
        {
            body[0] = 0;
            belowFirst = points < nextLevelPoints ? 1u : 0u;
        }
        if (!(belowFirst && *g_levelNumber != kLastLevel))
        {
            const uint32_t belowEditor = points < static_cast<uint32_t>(editorPiecePoints) ? 1u : 0u;
            memset(line, 0, 0x100);
            uint32_t glyph = kCheckGlyph;
            if (belowEditor)
                glyph = kCrossGlyph;
            zge::formatString(line, kEditorPieceLine, white, glyph, blue, gold, editorPiecePoints);
            zge::appendString(body, line);
            if (!belowEditor)
            {
                uint32_t moviesGlyph = kCheckGlyph;
                if (!allChallenges)
                    moviesGlyph = kCrossGlyph;
                const uint32_t levelName = summaryScreenEntry(*g_levelNumber);
                zge::formatString(line, kMoviesLine, white, moviesGlyph, blue, levelName, gold);
                zge::appendString(body, line);
                if (allChallenges)
                {
                    uint32_t secretGlyph = kCheckGlyph;
                    if (!allObjectives)
                        secretGlyph = kCrossGlyph;
                    zge::formatString(line, kSecretCharacterLine, white, secretGlyph, blue, gold);
                    zge::appendString(body, line);
                }
            }
        }
        zgui::setVariable(data, kText, body);
    }
    READABLE_FUNCTION_WAITS(inlinePausePopulateSummaryScreen, Verified);

    // inlinePausePlayFlyThru: (the page,): the selected challenge's fly-through: the
    // follow camera runs its animation with the NPCs out of the way.
    int32_t inlinePausePlayFlyThru(pause::Arguments arguments)
    {
        ee::Local<int32_t> selected;
        ee::Local<ee::Ptr<zge::Motion>> motion;
        const uint32_t objectives = aggressive::level::currentObjectives();
        *motion = {};
        *selected = -1;
        hashCompleteOnce(g_flyThruHashPending, g_flyThruCompleteHash);
        const ee::Ptr<zgui::Node> list = find(arguments[0], kObjectivesNode);
        find(arguments[0], kChallengeDetail);
        pause::copyInto(list, kSelectedIndex, selected);
        const ee::Ptr<char> name = script::scriptString(objectiveAt(objectives, *selected)->flyThru);
        zge::findAnimation(name, motion);
        playSelectSound();
        setLevelState(kLevelStateFlyThru);
        setPlayPhase(kPlayPhaseFlyThru);
        hideNpcsForFlyThru();
        zge::startAnimation(ee::Ptr<zge::SceneObject>(ee::Ptr<FollowCamera>(kFollowCamera)->node), *motion, ee::Float(0.0f));
        return static_cast<int32_t>(setFollowCameraMode(ee::Ptr<FollowCamera>(kFollowCamera), kFollowFlyThruMode));
    }
    READABLE_FUNCTION_WAITS(inlinePausePlayFlyThru, Verified);

    // inlinePausePlayCancelSound: the zgui's cancel sound, when it has one.
    void inlinePausePlayCancelSound()
    {
        const ee::FunctionPtr<void()> sound(*g_zguiCancelSound);
        if (sound)
            sound();
    }
    READABLE_FUNCTION(inlinePausePlayCancelSound, Verified);

    // inlinePauseModeExit: the pause menu's Exit: the level is over.
    int32_t inlinePauseModeExit()
    {
        return game::tail(enterGameMode, kEndLevelQuit);
    }
    READABLE_FUNCTION_WAITS(inlinePauseModeExit, Verified);

    // The sliders (volume_menu.h).
    int32_t inlinePauseSetSFXVolume(uint32_t arguments)
    {
        return setChannelVolume(sym::inlinePauseSetSFXVolume, arguments, kVolumeSfx);
    }
    READABLE_FUNCTION_WAITS(inlinePauseSetSFXVolume, Verified);

    int32_t inlinePauseSetSpeechVolume(uint32_t arguments)
    {
        return setChannelVolume(sym::inlinePauseSetSpeechVolume, arguments, kVolumeSpeech);
    }
    READABLE_FUNCTION_WAITS(inlinePauseSetSpeechVolume, Verified);

    int32_t inlinePauseSetMusicVolume(uint32_t arguments)
    {
        return setChannelVolume(sym::inlinePauseSetMusicVolume, arguments, kVolumeMusic);
    }
    READABLE_FUNCTION_WAITS(inlinePauseSetMusicVolume, Verified);

    int32_t inlinePauseGetSFXVolume(uint32_t arguments)
    {
        return showChannelVolume(sym::inlinePauseGetSFXVolume, arguments, kVolumeSfx, kCurValue, kPlayOnChange);
    }
    READABLE_FUNCTION_WAITS(inlinePauseGetSFXVolume, Verified);

    int32_t inlinePauseGetSpeechVolume(uint32_t arguments)
    {
        return showChannelVolume(sym::inlinePauseGetSpeechVolume, arguments, kVolumeSpeech, kCurValue, kPlayOnChange);
    }
    READABLE_FUNCTION_WAITS(inlinePauseGetSpeechVolume, Verified);

    int32_t inlinePauseGetMusicVolume(uint32_t arguments)
    {
        return showChannelVolume(sym::inlinePauseGetMusicVolume, arguments, kVolumeMusic, kCurValue, kPlayOnChange);
    }
    READABLE_FUNCTION_WAITS(inlinePauseGetMusicVolume, Verified);
}
