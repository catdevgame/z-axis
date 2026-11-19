// DM2's records screen (screens.h; "InGameUI.FrameRecords"): the level's
// saved records shown, and the screen's script function. DM2's own code
// (overlay.c).

#include "screens.h"

#include "dm2/action/saved_records.h"
#include "dm2/game/players.h"
#include "dm2/game/records.h"
#include "dm2/game/round.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_records.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kRecordNames{sym::recordsNamePath}; // "InGameUI.FrameRecords.RecordName"
        constexpr ee::Ptr<const char> kMessageTypeName{sym::g_textMsgType}; // "MsgType"
        // What the record names' script asks: back, or go on.
        constexpr int32_t kRecordsBack = -1, kRecordsGoOn = 1;
    }

    namespace
    {
        // The screen's four columns: each record's name, the last name of
        // the character who set it, the name entered for it, and its value.
        constexpr ee::Ptr<const char> kCharacterColumn{sym::recordsCharacterPath}; // "InGameUI.FrameRecords.CharName"
        constexpr ee::Ptr<const char> kPlayerColumn{sym::recordsPlayerPath};       // "InGameUI.FrameRecords.playerName"
        constexpr ee::Ptr<const char> kValueColumn{sym::recordsValuePath};       // "InGameUI.FrameRecords.RecordValue"
        constexpr ee::Ptr<const char> kItemsName{sym::g_textItems};               // "Items"
        constexpr ee::Ptr<char> kRecordsInit{sym::recordsInitScript};
        constexpr ee::Ptr<char> kRecordsDeInit{sym::recordsDeInitScript};
        constexpr uint32_t kRecordNameKey = sym::recordNameKeyFormat;  // "$KSTR_RECORDS_NAME%02d :"
        constexpr uint32_t kNewLine = sym::g_textNewLine;              // "\n"
        constexpr uint32_t kNameLineFormat = sym::recordNameLineFormat; // "%s\n"
        constexpr uint32_t kNumberFormat = sym::recordNumberFormat;    // "%ld"
        constexpr uint32_t kTricksFormat = sym::recordTricksFormat;    // "%ld tricks"
        constexpr uint32_t kDegreesFormat = sym::recordDegreesFormat;  // "%ld degrees"
        // A spin record counts half turns.
        constexpr uint32_t kDegreesPerHalfTurn = 180;
        // The bottom header's first button: "Next" at a turn's end, "Back"
        // where the screen goes back to another.
        constexpr ee::Ptr<const char> kButtonCaption{sym::bottomButtonCaptionPath};
        constexpr ee::Ptr<const char> kButtonGlyph{sym::bottomButtonPath};
        constexpr uint32_t kNextCaption = sym::screenNextText;     // "Next"
        constexpr uint32_t kBackCaption = sym::screenBackText;     // "Back"
        constexpr uint32_t kGlyph1 = sym::screenButton1Text;       // "1"
        constexpr uint32_t kGlyph3 = sym::screenButton3Text;       // "3"

        // A record's value as the screen writes it: the turn score's by the
        // game's mode (the distance modes' a distance, mode 9's tricks, mode
        // 10's spin), the tricks of a run, a spin in degrees, a distance or
        // a height in feet and inches, else the number.
        void formatRecordValue(ee::Ptr<char> out, uint32_t kind, ee::Float value)
        {
            enum Shown { kNumber, kTricks, kDegrees, kDistance };
            Shown shown = kNumber;
            if (kind == kTurnScoreRecord)
            {
                switch (multiplayerMode())
                {
                case 6: case 7: case 8: case 11: case 12: case 13:
                    shown = kDistance;
                    break;
                case 9:
                    shown = kTricks;
                    break;
                case 10:
                    shown = kDegrees;
                    break;
                default:
                    break;
                }
            }
            else if (kind == kRunTricksRecord)
                shown = kTricks;
            else if (kind == kSpinRecord)
                shown = kDegrees;
            else if (kind >= kAirHeightRecord && kind <= kSkidRecord)
                shown = kDistance;
            switch (shown)
            {
            case kDistance:
                formatFeetAndInches(out, value);
                break;
            case kTricks:
                zge::formatString(out, kTricksFormat, sce::libgcc::fptoui(value));
                break;
            case kDegrees:
                zge::formatString(out, kDegreesFormat, sce::libgcc::fptoui(value) * kDegreesPerHalfTurn);
                break;
            case kNumber:
                zge::formatString(out, kNumberFormat, sce::libgcc::fptoui(value));
                break;
            }
        }
    }

    // showRecordsScreen: as it opens, the level's saved records of the game
    // being played, kinds 1 .. 12 (the turn score to the skid), a line each
    // in the four columns; the bottom button "Next" at a turn's end, "Back"
    // where it goes back to another screen; then Init. As it closes, DeInit.
    void showRecordsScreen(uint32_t shown)
    {
        ee::Local<char[0x100]> text;
        ee::Local<char[0x130]> line;
        ee::Local<char[0x400]> names;
        ee::Local<char[0x400]> characters;
        ee::Local<char[0x400]> players;
        ee::Local<char[0x400]> values;
        names[0] = 0;
        characters[0] = 0;
        players[0] = 0;
        values[0] = 0;
        if (shown == 0)
        {
            runUiScript(kRecordsDeInit);
            return;
        }
        uint32_t namesLeft = kRecordKinds - 1;
        for (uint32_t kind = kTurnScoreRecord; kind < kRecordKinds; ++kind)
        {
            const uint8_t mode = multiplayerMode();
            --namesLeft;
            const ee::Ptr<SavedRecord> record = savedRecord(recordKey(mode, singlePlayerMode(), kind, kSavedRecords));
            zge::formatString(text, kRecordNameKey, kind);
            zge::stringCopy(line, text);
            if (namesLeft != 0)
                zge::appendString(line, kNewLine);
            zge::appendString(names, line);
            zge::stringCopy(line, kCharacterNames[record->character].lastName);
            zge::appendString(line, kNewLine);
            zge::appendString(characters, line);
            zge::formatString(text, kNameLineFormat, ee::Ptr<char>(record->name));
            zge::appendString(players, text);
            formatRecordValue(text, kind, record->value);
            zge::stringCopy(line, text);
            zge::appendString(line, kNewLine);
            zge::appendString(values, line);
        }
        zge::zgui::setVariable(uiWidget(kRecordNames), kItemsName, names);
        zge::zgui::setVariable(uiWidget(kCharacterColumn), kItemsName, characters);
        zge::zgui::setVariable(uiWidget(kPlayerColumn), kItemsName, players);
        zge::zgui::setVariable(uiWidget(kValueColumn), kItemsName, values);
        switch (*g_gameScreen)
        {
        case kScreenTurnRecords:
            zge::zgui::setVariableAtPath(kButtonCaption, kNextCaption);
            zge::zgui::setVariableAtPath(kButtonGlyph, kGlyph1);
            break;
        case kScreenRecords:
        case kScreenRoundOverRecords:
        case kScreenGameOverRecords:
            zge::zgui::setVariableAtPath(kButtonCaption, kBackCaption);
            zge::zgui::setVariableAtPath(kButtonGlyph, kGlyph3);
            break;
        default:
            break;
        }
        runUiScript(kRecordsInit);
    }
    READABLE_FUNCTION(showRecordsScreen, Verified);

    // bmxoverlayRecordsHandler: back goes to the screen the records were
    // asked from (the pause menu, the round over or the game over screen).
    // Going on from the records shown at a turn's end moves the game on: a
    // ProQuest level to its reward screen, a competition to its
    // leaderboard, any other single-player game to its end; HORSE to its
    // end, its next turn or its time up; the best-turn modes to their end
    // or their next turn; the other multiplayer modes to their end or their
    // time up.
    void bmxoverlayRecordsHandler()
    {
        ee::Local<int32_t> message;
        zge::zgui::copyVariable(uiWidget(kRecordNames), kMessageTypeName, message);
        if (*message == kRecordsBack)
        {
            switch (*g_gameScreen)
            {
            case kScreenRecords:
                showGameScreen(kScreenPause);
                break;
            case kScreenRoundOverRecords:
                showGameScreen(kScreenRoundOver);
                break;
            case kScreenGameOverRecords:
                showGameScreen(kScreenGameOver);
                break;
            default:
                break;
            }
            return;
        }
        if (*message != kRecordsGoOn || *g_gameScreen != kScreenTurnRecords)
            return;

        const uint8_t mode = multiplayerMode();
        if (mode == kHorseMode)
        {
            if (roundFlags(kGameEndsFlag))
            {
                showGameScreen(kScreenGameOver);
            }
            else if (!roundFlags(kRoundDoneFlag))
            {
                showGameScreen(kScreenNone);
                setRoundPhase(kTurnStartPhase);
            }
            else
            {
                showGameScreen(kScreenMultiplayerTimeUp);
            }
            return;
        }
        if (mode == 0)
        {
            if (singlePlayerMode() == kLevelGoals)
                showGameScreen(kScreenReward);
            else if (singlePlayerMode() == kCompetition)
                showGameScreen(kScreenLeaderboard);
            else
                showGameScreen(kScreenGameOver);
            return;
        }
        // (Mode 13, the last best-turn mode, goes with the others here.)
        if (mode >= kFirstBestTurnMode && mode < kLastBestTurnMode)
        {
            if (roundFlags(kGameEndsFlag))
            {
                showGameScreen(kScreenGameOver);
            }
            else
            {
                showGameScreen(kScreenNone);
                setRoundPhase(kTurnStartPhase);
            }
            return;
        }
        showGameScreen(roundFlags(kGameEndsFlag) ? kScreenGameOver : kScreenMultiplayerTimeUp);
    }
    READABLE_FUNCTION(bmxoverlayRecordsHandler, Verified);
}
