// DM2's objectives screen (screens.h; "InGameUI.FrameObjectives"): the
// current challenge's goals listed, the done ones crossed out, and its
// menu's script function. DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/action/action_state.h"
#include "dm2/game/players.h"
#include "dm2/game/round.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/screen_objectives.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kFrameObjectivesMenu{sym::g_textFrameObjectivesMenu}; // "InGameUI.FrameObjectives.Menu"
        constexpr ee::Ptr<const char> kMessageTypeName{sym::g_textMsgType};                 // "MsgType"
        // What the menu's "MsgType" says was pressed.
        constexpr int32_t kMenuBack = -1;
        constexpr int32_t kMenuAccept = 1;
    }

    // bmxoverlayObjectivesHandler: back leaves the objectives asked for from
    // the pause menu for that menu; accept starts the turn from the
    // objectives shown before it, or goes on to the game over screen from
    // the ones shown at the game's end.
    void bmxoverlayObjectivesHandler()
    {
        ee::Local<int32_t> pressed;
        zge::zgui::copyVariable(uiWidget(kFrameObjectivesMenu), kMessageTypeName, pressed);
        if (pressed == kMenuBack)
        {
            if (*g_gameScreen == kScreenPauseObjectives)
                showGameScreen(kScreenPause);
        }
        else if (pressed == kMenuAccept)
        {
            const uint8_t screen = *g_gameScreen;
            if (screen == kScreenObjectives)
            {
                showGameScreen(kScreenNone);
                setRoundPhase(kTurnRunningPhase);
            }
            else if (screen == kScreenEndObjectives)
            {
                showGameScreen(kScreenGameOver);
            }
        }
    }
    READABLE_FUNCTION(bmxoverlayObjectivesHandler, Verified);

    namespace
    {
        constexpr ee::Ptr<const char> kGoalTypeLabel{sym::objectivesGoalTypePath};         // "...FrameObjectives.GoalType"
        constexpr ee::Ptr<const char> kGoalsLabel{sym::objectivesGoalsPath};               // "...FrameObjectives.Goals"
        constexpr ee::Ptr<const char> kCrossoutLabel{sym::objectivesCrossoutPath};         // "...FrameObjectives.GoalCrossout"
        constexpr ee::Ptr<const char> kInstructionLabel{sym::objectivesInstructionPath};   // "...FrameObjectives.InstructionStr"
        constexpr ee::Ptr<const char> kText{sym::screenTextName};                          // "Text"
        constexpr ee::Ptr<char> kObjectivesInit{sym::objectivesInitScript};
        constexpr ee::Ptr<char> kObjectivesDeInit{sym::objectivesDeInitScript};
        // The bottom header's first button, its caption and its glyph
        // ("1", "3": the button font's).
        constexpr ee::Ptr<const char> kButtonCaption{sym::bottomButtonCaptionPath};
        constexpr ee::Ptr<const char> kButtonGlyph{sym::bottomButtonPath};
        constexpr uint32_t kNextCaption = sym::screenNextText;     // "Next"
        constexpr uint32_t kBackCaption = sym::screenBackText;     // "Back"
        constexpr uint32_t kSelectCaption = sym::screenSelectText; // "Select"
        constexpr uint32_t kGlyph1 = sym::screenButton1Text;       // "1"
        constexpr uint32_t kGlyph3 = sym::screenButton3Text;       // "3"

        // The challenge tiers' names ("$KSTRSP_OBJCTV_ROOKIE" .. "_EXPERT",
        // then "_DONE"), the words of their goals' keys from the second on
        // ("EASY", "HARD", "XTRA", "EXPERT"), and the instruction by tier.
        constexpr uint32_t kTierNames = sym::objectiveTierNames;
        constexpr uint32_t kTierWords = sym::objectiveTierWords;
        constexpr uint32_t kTierDone = 5;
        constexpr uint32_t kLastChallenge = kLevelChallenges - 1;
        constexpr uint32_t kInstructionKey = sym::objectivesInstructionKey; // "$KSTRSP_OBJCTV_INST_00_%02d"
        uint32_t textIn(uint32_t table, uint32_t index)
        {
            return *(ee::Ptr<uint32_t>(table) + static_cast<ptrdiff_t>(index));
        }
        // How many goals each challenge of the session level has.
        inline ee::Global<uint8_t[kLevelChallenges], sym::g_challengeGoalCounts> g_challengeGoalCounts;

        // A goal's text: the string table's, by key: the rookie challenge's
        // one goal "KSTRSP_GOAL_LEV%02d_ROOKIE", the others' "KSTRSP_GOAL_LEV
        // %02d_<word>_<goal>" once revealed, else "KSTRSP_GOAL_UNKNOWN".
        constexpr uint32_t kRookieGoalKey = sym::goalKeyRookieFormat;
        constexpr uint32_t kGoalKey = sym::goalKeyFormat;
        constexpr uint32_t kUnknownGoalKey = sym::goalKeyUnknown;
        constexpr uint32_t kGoalLineChars = 35;

        constexpr uint32_t kNameFormat = sym::screenStringFormat;    // "%s"
        constexpr uint32_t kPairFormat = sym::trickWordFormat;       // "%s%s"
        constexpr uint32_t kNoText = sym::g_textEmpty;               // ""
        constexpr uint32_t kGoalSpacing = sym::goalSpacingText;      // "\n \n" between goals
        constexpr uint32_t kMarkFormat = sym::crossoutFormat;        // "%c"
        constexpr uint32_t kMarkLineFormat = sym::crossoutLineFormat; // "\n%c"
        // The crossout column: a dash beside each line of a done goal.
        constexpr char kDoneMark = '-', kNoMark = ' ';

        // `text` put after `into`, after the goals' spacing unless it is the first.
        void appendGoal(ee::Ptr<char> into, uint32_t goal, ee::Address text)
        {
            const int32_t end = zge::stringLength(into);
            zge::formatString(into + end, kPairFormat, goal == 0 ? kNoText : kGoalSpacing, text);
        }
    }

    // showObjectivesScreen (screens 14 .. 16's show handler): as it opens,
    // the current player's challenge on the level: its tier's name, its
    // goals' texts (35 characters a line), a dash beside every line of a
    // done goal, the tier's instruction; then Init, and the bottom button
    // "Next" before the turn (14) or "Back" from the pause menu (15). As it
    // closes, the button back to "Select" and DeInit.
    void showObjectivesScreen(uint32_t shown)
    {
        ee::Local<char[0x20]> key;
        ee::Local<char[0x200]> markText;
        ee::Local<char[0x400]> goalsText;
        ee::Local<char[0x400]> marks;
        ee::Local<char[0x200]> goalText;
        ee::Local<char[0x200]> lines;

        const ee::Ptr<LevelGoals> goals = playerLevelGoals(*g_currentPlayer, *g_sessionLevel);
        if (shown == 0)
        {
            zge::zgui::setVariableAtPath(kButtonCaption, kSelectCaption);
            zge::zgui::setVariableAtPath(kButtonGlyph, kGlyph1);
            runUiScript(kObjectivesDeInit);
            return;
        }

        // The tier: the challenge, or "done" once the last one's goals are.
        uint32_t tier = goals->challenge;
        if (tier == kLastChallenge)
            tier = goals->goalsDone == (*g_challengeGoalCounts)[kLastChallenge] ? kTierDone : kLastChallenge;
        zge::formatString(markText, kNameFormat, textIn(kTierNames, tier));
        zge::zgui::setVariable(uiWidget(kGoalTypeLabel), kText, markText);

        goalsText[0] = 0;
        marks[0] = 0;
        if (tier < kTierDone && (*g_challengeGoalCounts)[goals->challenge] != 0)
        {
            for (uint32_t goal = 0;; ++goal)
            {
                const uint32_t challenge = goals->challenge;
                if (challenge == 0)
                {
                    zge::formatString(key, kRookieGoalKey, *g_sessionLevel);
                    zge::stringCopy(goalText, zge::stringTableText(zge::nameHash(key)));
                }
                else if (goals->challenges[challenge].goals[goal].flags & kGoalRevealed)
                {
                    zge::formatString(key, kGoalKey, *g_sessionLevel, textIn(kTierWords, challenge - 1), goal);
                }
                else
                {
                    zge::formatString(key, kUnknownGoalKey);
                }
                joinTextLines(goalText, zge::stringTableText(zge::nameHash(key)));
                breakTextLines(lines, goalText, kGoalLineChars);
                appendGoal(goalsText, goal, lines);

                // Its marks: one for its first line and one for each line break.
                const char mark = (goals->challenges[goals->challenge].goals[goal].flags & kGoalDone) ? kDoneMark
                                                                                                    : kNoMark;
                zge::formatString(markText, kMarkFormat, mark);
                for (ee::Ptr<const char> at = lines; *at != 0;)
                {
                    if (*at == '\r')
                    {
                        zge::formatString(markText + zge::stringLength(markText), kMarkLineFormat, mark);
                        at = at + 1;
                    }
                    else if (*at == '\\')
                    {
                        if (at[1] == 'n')
                        {
                            zge::formatString(markText + zge::stringLength(markText), kMarkLineFormat, mark);
                            at = at + 2;
                        }
                        else
                        {
                            at = at + 1;
                        }
                    }
                    else
                    {
                        at = at + 1;
                    }
                }
                appendGoal(marks, goal, markText);
                if ((*g_challengeGoalCounts)[goals->challenge] <= goal + 1)
                    break;
            }
        }

        zge::zgui::setVariable(uiWidget(kGoalsLabel), kText, goalsText);
        zge::zgui::setVariable(uiWidget(kCrossoutLabel), kText, marks);
        zge::formatString(key, kInstructionKey, tier);
        zge::zgui::setVariable(uiWidget(kInstructionLabel), kText, key);
        runUiScript(kObjectivesInit);
        if (*g_gameScreen == kScreenObjectives)
        {
            zge::zgui::setVariableAtPath(kButtonCaption, kNextCaption);
            zge::zgui::setVariableAtPath(kButtonGlyph, kGlyph1);
        }
        else if (*g_gameScreen == kScreenPauseObjectives)
        {
            zge::zgui::setVariableAtPath(kButtonCaption, kBackCaption);
            zge::zgui::setVariableAtPath(kButtonGlyph, kGlyph3);
        }
    }
    READABLE_FUNCTION(showObjectivesScreen, Verified);
}
