// BMX XXX's objectives screens (screens.h; "FrameObjectives" and
// "FrameObjectiveDetail"): the level's goals as a menu, each crossed out
// once done, and one goal's name, description, hint and status. BMX XXX's
// own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/overlay/screen_calls.h"
#include "zge/scene/objectives.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/overlay/screen_objectives.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    // The goals' lines being made, or one line looked for
    // (addObjectiveLine's data).
    // (The handler's frame keeps it on a 16-byte boundary.)
    struct alignas(16) ObjectiveLines
    {
        ee::Ptr<LevelGoals> goals; // +0x00 the player's marks of the level's objectives
        uint32_t place;                   // +0x04 the objective's place among the level's
        uint32_t lines;                   // +0x08 the lines so far
        int32_t wanted;                   // +0x0C the line looked for; kEveryLine: the texts are made
        ee::Ptr<char> text;               // +0x10 the goals' lines
        ee::Ptr<char> crossout;           // +0x14 a line each: '-' for a goal done, a space otherwise
    };
    static_assert(offsetof(ObjectiveLines, crossout) == 0x14 && sizeof(ObjectiveLines) == 0x20);

    namespace
    {
        constexpr uint32_t kFrameObjectives = sym::g_textFrameObjectives;           // "FrameObjectives"
        constexpr uint32_t kFrameObjectiveDetail = sym::g_textFrameObjectiveDetail; // "FrameObjectiveDetail"
        constexpr uint32_t kTitleWidget = sym::g_textTitle;                         // "Title"
        constexpr uint32_t kGoalsWidget = sym::g_textGoals;                         // "Goals"
        constexpr uint32_t kGoalCrossoutWidget = sym::g_textGoalCrossout;           // "GoalCrossout"
        constexpr uint32_t kDetailWidget = sym::g_textDetail;                       // "Detail"
        constexpr uint32_t kStatusWidget = sym::g_textStatus;                       // "Status"
        constexpr uint32_t kPositionName = sym::g_textPosition;                     // "Position"
        // The screen's two titles: "$KSTRSP_OBJCTV_TITLE", and
        // "$KSTRSP_OBJCTV_TITLE_COMPLETE" once every goal is done.
        inline ee::Global<ee::Ptr<const char>[2], sym::g_objectiveTitles> g_objectiveTitles;
        // A goal's texts' keys, each after the objective's string id.
        constexpr uint32_t kNameKey = sym::bmx::g_textObjectiveNameKey;         // "%s_NAME"
        constexpr uint32_t kDescriptionKey = sym::bmx::g_textObjectiveDescKey;  // "%s_DESC"
        constexpr uint32_t kHintKey = sym::bmx::g_textObjectiveHintKey;         // "%s_HINT"
        constexpr uint32_t kActivateKey = sym::bmx::g_textObjectiveActivateKey; // "%s_ACTIVATE"
        constexpr uint32_t kUnknownKey = sym::bmx::g_textObjectiveUnknownKey;   // "KSTRSP_OBJCTV_UNKNOWN"
        constexpr uint32_t kMissingString = sym::bmx::g_textMissingString;      // "Missing string: \"%s\""
        constexpr uint32_t kMissingStringId = sym::bmx::g_textMissingStringId;  // "Missing STRINGID for: \"%s\""
        constexpr uint32_t kCrossoutFormat = sym::bmx::g_textCrossoutFormat;    // "%c"
        constexpr uint32_t kHintLine = sym::bmx::g_textHintLine;                // the hint on a line of its own, "%s" after a line break
        // A goal's line in the menu is cut past 40 characters.
        constexpr uint32_t kGoalLineLimit = 0x28;
        constexpr char kCrossedOut = '-', kNotCrossedOut = ' ';

        constexpr int32_t kEveryLine = -1;
        // The objective of the line looked for, 0 if there is none; the
        // detail screen shows it.
        inline ee::Global<ee::Ptr<zge::Objective>, sym::g_shownObjective> g_shownObjective;

        // Where "FrameContinue" stood before the objectives moved it, and
        // where they put it.
        // (A position's value is 16 bytes; the last word is not set.)
        struct WidgetPosition
        {
            ee::Float x, y;
            uint32_t z;
            uint32_t notSet;
        };
        inline ee::Global<WidgetPosition, sym::g_continuePosition> g_continuePosition;
        constexpr ee::Float kContinueX = -64.0f, kContinueY = 357.0f;

        constexpr uint32_t kChosenSound = 0x0E, kRefusedSound = 0x10;
    }

    // addObjectiveLine: forEachObjective's visitor for the goals' menu. A
    // set's children have no line. With every line wanted the objective's
    // line is added to the two texts: its description once it was
    // activated, what activates it when it can be, "unknown" otherwise.
    // With one line wanted the visit ends at that line's objective.
    uint32_t addObjectiveLine(ee::Ptr<zge::Objective> objective, ObjectiveLines *lines)
    {
        ee::Local<char[0x200]> line;
        ee::Local<char[0x10]> mark;
        ee::Local<char[0x40]> key;

        const ee::Ptr<zge::ObjectiveInfo> info = zge::objectiveInfo(objective);
        if ((info->flags & zge::kObjectiveIsChild) == 0)
        {
            if (lines->wanted != kEveryLine)
            {
                if (lines->wanted == static_cast<int32_t>(lines->lines))
                {
                    *g_shownObjective = objective;
                    return 0;
                }
                lines->lines = lines->lines + 1;
            }
            else
            {
                if ((lines->goals->marks[lines->place] & calls::kGoalActive) != 0)
                    zge::formatString(key, kDescriptionKey, info->stringId);
                else if (zge::objectiveReadyToActivate(objective) != 0)
                    zge::formatString(key, kActivateKey, info->stringId);
                else
                    zge::formatString(key, kUnknownKey);

                const char *text = zge::stringTableText(zge::nameHash(key));
                if (text)
                    copyOnOneLine(line, text, kGoalLineLimit);
                else if (info->stringId[0] != '\0')
                    zge::formatString(line, kMissingString, key);
                else
                    zge::formatString(line, kMissingStringId, zge::objectiveName(objective));
                zge::formatString(lines->text + zge::stringLength(lines->text), kSeparatorAndLine,
                                  lines->lines != 0 ? kLineBreak : kNoSeparator, line);

                const bool done = (lines->goals->marks[lines->place] & calls::kGoalDone) != 0;
                zge::formatString(mark, kCrossoutFormat, done ? kCrossedOut : kNotCrossedOut);
                zge::formatString(lines->crossout + zge::stringLength(lines->crossout), kSeparatorAndLine,
                                  lines->lines != 0 ? kLineBreak : kNoSeparator, mark);
                lines->lines = lines->lines + 1;
            }
        }
        lines->place = lines->place + 1;
        return 1;
    }
    READABLE_FUNCTION(addObjectiveLine, Verified);

    // objectivesScreenMessage: the goals' menu, shown before the turn
    // (going on starts the turn), from the pause menu (back to it), or at
    // the game's end (on to the game over screen). The first and the last
    // show "FrameContinue" below in place of "FrameGoBack". The machine's
    // button 6 opens the selected goal's detail, when the goal is known.
    void objectivesScreenMessage(int32_t message)
    {
        ee::Local<char[0x400]> text;
        ee::Local<char[0x400]> crossout;
        ee::Local<WidgetPosition> position;
        ee::Local<ee::Ptr<const char>[2]> titles;
        ee::Local<ObjectiveLines> lines;
        ee::Local<uint32_t> visible;
        ee::Local<int32_t> selected;

        titles[0] = (*g_objectiveTitles)[0];
        titles[1] = (*g_objectiveTitles)[1];
        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameObjectives, 1);
        if (!frame)
            return;
        zge::setStringTable(*calls::g_levelStrings);

        switch (message)
        {
        case kScreenEnter:
        {
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(*calls::g_currentPlayer, *calls::g_currentLevel);
            const ee::Ptr<Node> title = zge::zgui::findNode(frame, kTitleWidget, 1);
            zge::zgui::setVariable(title, kTextName, titles[goals->goalsDone == goals->goals ? 1 : 0]);

            lines->goals = goals;
            lines->wanted = kEveryLine;
            text[0] = '\0';
            crossout[0] = '\0';
            lines->place = 0;
            lines->lines = 0;
            lines->text = ee::Ptr<char>(text);
            lines->crossout = ee::Ptr<char>(crossout);
            calls::forEachObjective(sym::addObjectiveLine, lines);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kGoalsWidget, 1), kItemsName, text);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kGoalCrossoutWidget, 1), kItemsName, crossout);
            playUiNode(frame, kInitAction);

            if (*g_gameScreen == kScreenObjectives || *g_gameScreen == kScreenEndObjectives)
            {
                visible = 1;
                zge::zgui::setVariable(zge::zgui::findNode(*g_overlayUi, kFrameContinue, 1), kVisibleName, visible);
                zge::zgui::copyVariable(zge::zgui::findNode(*g_overlayUi, kFrameContinue, 1), kPositionName, &*g_continuePosition);
                position->x = kContinueX;
                position->y = kContinueY;
                position->z = 0;
                zge::zgui::setVariable(zge::zgui::findNode(*g_overlayUi, kFrameContinue, 1), kPositionName, position);
                visible = 0;
                zge::zgui::setVariable(zge::zgui::findNode(*g_overlayUi, kFrameGoBack, 1), kVisibleName, visible);
            }
            break;
        }

        case kScreenLeaveNow:
            playUiNode(frame, kShutdownAction);
            break;

        case kScreenLeft:
            visible = 0;
            zge::zgui::setVariable(zge::zgui::findNode(*g_overlayUi, kFrameContinue, 1), kVisibleName, visible);
            zge::zgui::setVariable(zge::zgui::findNode(*g_overlayUi, kFrameGoBack, 1), kVisibleName, visible);
            zge::zgui::setVariable(zge::zgui::findNode(*g_overlayUi, kFrameContinue, 1), kPositionName, &*g_continuePosition);
            break;

        case kScreenChosen:
            if (*g_gameScreen == kScreenObjectives)
            {
                showGameScreen(kScreenPlay);
                *g_resumeAction = kResumeTurnRunning;
            }
            else if (*g_gameScreen == kScreenEndObjectives)
            {
                showGameScreen(kScreenGameOver);
            }
            break;

        case kScreenCancelled:
            if (*g_gameScreen == kScreenPauseObjectives)
                showGameScreen(kScreenPause);
            break;

        case kScreenButton6:
        {
            const ee::Ptr<LevelGoals> goals = playerLevelGoals(*calls::g_currentPlayer, *calls::g_currentLevel);
            zge::zgui::copyVariable(zge::zgui::findNode(frame, kGoalsWidget, 1), kSelectedIndexName, selected);
            if (selected < 0)
                break;
            lines->wanted = selected;
            *g_shownObjective = 0;
            lines->goals = goals;
            lines->place = 0;
            lines->lines = 0;
            calls::forEachObjective(sym::addObjectiveLine, lines);

            const ee::Ptr<zge::Objective> objective = *g_shownObjective;
            if (objective != 0 && ((goals->marks[lines->place] & calls::kGoalActive) != 0 ||
                                   zge::objectiveReadyToActivate(objective) != 0))
            {
                *g_screenBehindExitConfirm = *g_gameScreen;
                if (gameScreen() == kScreenPauseObjectives)
                    showGameScreen(kScreenPauseObjectiveDetail);
                else
                    showGameScreen(kScreenObjectiveDetail);
                soundEvent(kChosenSound, 0, 0, 0, 0);
            }
            else
            {
                soundEvent(kRefusedSound, 0, 0, 0, 0);
            }
            break;
        }

        default:
            break;
        }
        zge::setStringTable(*calls::g_gameStrings);
    }
    READABLE_FUNCTION(objectivesScreenMessage, Verified);

    // objectiveDetailScreenMessage: the shown objective's name as the
    // title, its description with its hint on the line after, and its
    // status. Cancelled, the screen it was asked from comes back.
    void objectiveDetailScreenMessage(int32_t message)
    {
        ee::Local<char[0x40]> key;
        ee::Local<char[0x200]> detail;
        ee::Local<char[0x100]> hint;
        ee::Local<char[0x100]> status;

        const ee::Ptr<Node> frame = zge::zgui::findNode(*g_overlayUi, kFrameObjectiveDetail, 1);
        if (!frame)
            return;
        zge::setStringTable(*calls::g_levelStrings);

        if (message == kScreenEnter)
        {
            const ee::Ptr<zge::Objective> objective = *g_shownObjective;
            const ee::Ptr<zge::ObjectiveInfo> info = zge::objectiveInfo(objective);
            zge::formatString(key, kNameKey, info->stringId);
            const ee::Ptr<Node> title = zge::zgui::findNode(frame, kTitleWidget, 1);
            zge::zgui::setVariable(title, kTextName, zge::stringTableText(zge::nameHash(key)));

            zge::formatString(key, kDescriptionKey, info->stringId);
            copyOnOneLine(detail, zge::stringTableText(zge::nameHash(key)), 0);
            zge::formatString(key, kHintKey, info->stringId);
            copyOnOneLine(hint, zge::stringTableText(zge::nameHash(key)), 0);
            zge::formatString(ee::Ptr<char>(detail) + zge::stringLength(detail), kHintLine, hint);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kDetailWidget, 1), kTextName, detail);

            zge::objectiveStatusText(objective, status);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kStatusWidget, 1), kTextName, status);
            playUiNode(frame, kInitAction);
        }
        else if (message == kScreenLeaveNow)
        {
            playUiNode(frame, kShutdownAction);
        }
        else if (message == kScreenCancelled)
        {
            showGameScreen(*g_screenBehindExitConfirm);
        }
        zge::setStringTable(*calls::g_gameStrings);
    }
    READABLE_FUNCTION(objectiveDetailScreenMessage, Verified);
}
