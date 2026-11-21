// The front end's high scores (hiscores_menu.h): DM2's own code, registered
// as a script function by the menu set-up.

#include "hiscores_menu.h"
#include "menu_variables.h"
#include "../action/action_state.h"
#include "../action/saved_records.h"
#include "../game/players.h"
#include "../game/records.h"
#include "../game/round.h"
#include "../overlay/screens.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/menu/hiscores_menu.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The frame's record names column: a tree view whose items hang from
        // its RootNode; the frame's level entry is its "CurLevel".
        constexpr uint32_t kRecordNamesPath = sym::hiscoresRecordNamePath; // "MainMenu.HiscoresFrame.RecordName"
        constexpr uint32_t kRootNode = sym::frontEndRootNodeName;         // "RootNode"
        constexpr uint32_t kParentNode = sym::frontEndParentNodeName;     // "ParentNode"
        constexpr uint32_t kCurLevel = sym::frontEndCurLevelName;         // "CurLevel"
        constexpr ee::Ptr<const char> kTopItemText{sym::frontEndMasterText};        // "Master"
        // A record's name: the string table's "$KSTR_RECORDS_NAME01" .. "12".
        constexpr uint32_t kRecordNameFormat = sym::recordNameFormat;     // "$KSTR_RECORDS_NAME%02d :"
        // The other three columns' item texts, a line each.
        constexpr ee::Ptr<const char> kPlayerNamesPath{sym::hiscoresPlayerNamesPath}; // "...RecordName.PlayerName.Items"
        constexpr ee::Ptr<const char> kCharacterNamesPath{sym::hiscoresCharacterNamesPath}; // "...RecordName.CharName.Items"
        constexpr ee::Ptr<const char> kRecordValuesPath{sym::hiscoresRecordValuesPath}; // "...RecordName.RecordValue.Items"
        constexpr uint32_t kLineBreak = sym::frontEndLineBreak;           // "\n"
        // The values: a count, a count of tricks, a spin in degrees, or a
        // distance in feet and inches.
        constexpr uint32_t kCountFormat = sym::frontEndCountFormat;       // "%ld"
        constexpr uint32_t kTricksFormat = sym::tricksCountFormat;        // "%ld $KSTR_TRICKS"
        constexpr uint32_t kDegreesFormat = sym::degreesFormat;           // "%ld degrees"
        constexpr uint32_t kDegreesPerHalfTurn = 180;

        // The records listed: kinds 1 .. 12 (game/records.h), from the turn
        // score to the skid, of a single-player competition.
        constexpr uint32_t kFirstListed = kTurnScoreRecord;
        constexpr uint32_t kLastListed = kSkidRecord;

        // A record's value as the table shows it.
        void formatRecordValue(ee::Ptr<char> text, uint32_t kind, ee::Float value)
        {
            switch (kind)
            {
            case kTurnScoreRecord:
            case kRunPointsRecord:
            case kLinePointsRecord:
                zge::formatString(text, kCountFormat, sce::libgcc::fptoui(value));
                break;
            case kRunTricksRecord:
                zge::formatString(text, kTricksFormat, sce::libgcc::fptoui(value));
                break;
            case kSpinRecord:
                zge::formatString(text, kDegreesFormat, sce::libgcc::fptoui(value) * kDegreesPerHalfTurn);
                break;
            default:
                formatFeetAndInches(text, value);
                break;
            }
        }
    }

    void bmxGetHiscoresData()
    {
        ee::Local<char[0x400]> characters;
        ee::Local<char[0x400]> players;
        ee::Local<char[0x400]> values;
        ee::Local<char[0x40]> valueText;
        ee::Local<char[0x40]> recordName;
        ee::Local<ee::Ptr<zge::zgui::ListedTreeItem>> top;
        ee::Local<int32_t> levelEntry;

        // The record names column gets a new top item (the old one freed
        // with its items).
        const ee::Ptr<zge::zgui::Node> names = zge::zgui::findNode(*g_uiRoot, kRecordNamesPath, 0);
        zge::zgui::copyVariable(names, kRootNode, top);
        zge::zgui::copyVariable(names, kCurLevel, levelEntry);
        if (*top)
            zge::zgui::freeTreeItem(*top);
        *top = zge::zgui::newTreeItem(ee::Ptr<zge::zgui::ListedTreeItem>(), kTopItemText); // (a tree's top item: no parent)
        zge::zgui::setVariable(names, kRootNode, top);
        zge::zgui::setVariable(names, kParentNode, top);

        characters[0] = 0;
        players[0] = 0;
        values[0] = 0;
        *g_sessionLevel = (*g_levelMenuGameLevels)[*levelEntry];

        for (uint32_t kind = kFirstListed; kind <= kLastListed; ++kind)
        {
            zge::formatString(recordName, kRecordNameFormat, kind);
            zge::zgui::newTreeItem(*top, recordName);
            const ee::Ptr<SavedRecord> record = savedRecord(recordKey(0, kCompetition, kind, kSavedRecords));
            if (kind != kFirstListed)
            {
                zge::appendString(characters, kLineBreak);
                zge::appendString(players, kLineBreak);
                zge::appendString(values, kLineBreak);
            }
            zge::appendString(characters, kCharacterNames[record->character].lastName);
            zge::appendString(players, ee::Ptr<char>(record->name));
            formatRecordValue(valueText, kind, record->value);
            zge::appendString(values, valueText);
        }
        zge::zgui::setVariableAtPath(kPlayerNamesPath, players);
        zge::zgui::setVariableAtPath(kCharacterNamesPath, characters);
        zge::zgui::setVariableAtPath(kRecordValuesPath, values);
    }
    READABLE_FUNCTION(bmxGetHiscoresData, Verified);
}
