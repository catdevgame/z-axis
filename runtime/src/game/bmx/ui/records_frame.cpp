// BMX XXX's records frame (records_frame.h): bmx.c's.

#include "records_frame.h"

#include "strings.h"
#include "ui_calls.h"

#include "bmx/game/records.h"
#include "bmx/game/round.h"
#include "bmx/profile/records.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/ui/records_frame.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The frame's lists (zgui nodes under it, each with its "Items"
        // text), and the texts written into them.
        constexpr uint32_t kRecordNameList = sym::g_textRecordNameList;   // "RecordName"
        constexpr uint32_t kCharacterList = sym::g_textCharNameList;      // "CharName"
        constexpr uint32_t kPlayerNameList = sym::g_textPlayerNameList;   // "playerName"
        constexpr uint32_t kRecordValueList = sym::g_textRecordValueList; // "RecordValue"
        constexpr uint32_t kItems = sym::g_textListItems;                 // "Items"
        constexpr uint32_t kFrameNode = 1;
        constexpr uint32_t kRecordNameFormat = sym::g_textRecordNameFormat; // "$KSTR_RECORDS_NAME%02d :"
        constexpr uint32_t kLineEnd = sym::g_textRecordsLineEnd;            // "\\n" (zgui's line break)
        constexpr uint32_t kPlayerNameFormat = sym::g_textRecordsPlayerName; // "%s\\n"
        constexpr uint32_t kCountFormat = sym::g_textRecordsCount;          // "%ld"
        constexpr uint32_t kTricksFormat = sym::g_textRecordsTricks;        // "%ld tricks"
        constexpr uint32_t kDegreesFormat = sym::g_textRecordsDegrees;      // "%ld degrees"
        constexpr uint32_t kListBytes = 0x400, kLineBytes = 0x130;
        constexpr uint32_t kDegreesPerHalfTurn = 180;

        // The game's table of the rider characters: a row a character, the
        // name the leaderboard lists its fourth word.
        struct CharacterRow
        {
            uint8_t _00[0x0C];
            ee::Ptr<const char> boardName; // +0x0C
            uint8_t _10[0x14 - 0x10];
        };
        static_assert(offsetof(CharacterRow, boardName) == 0x0C && sizeof(CharacterRow) == 0x14);
        const ee::Ptr<CharacterRow> kCharacterRows(sym::g_characterNames);

        // The modes 9 .. 16 keep a record of their own kind in kind 1's place
        // (playerScoreFrame): mode 12's counts tricks, mode 13's spins, the
        // others' are distances.
        constexpr uint8_t kFirstOwnRecordMode = 9, kLastOwnRecordMode = 16;
        constexpr uint8_t kTricksRecordMode = 12, kSpinRecordMode = 13;

        enum class ValueForm
        {
            Count,
            Tricks,
            Degrees,
            Distance,
        };
        // How a kind of record's value is written.
        ValueForm valueForm(uint32_t kind)
        {
            switch (kind)
            {
            case kRecordRunScore:
            {
                const uint8_t mode = gameMode();
                if (mode < kFirstOwnRecordMode || mode > kLastOwnRecordMode)
                    return ValueForm::Count;
                if (mode == kTricksRecordMode)
                    return ValueForm::Tricks;
                if (mode == kSpinRecordMode)
                    return ValueForm::Degrees;
                return ValueForm::Distance;
            }
            case kRecordStackScore:
            case kRecordTrickScore:
                return ValueForm::Count;
            case kRecordCombo:
                return ValueForm::Tricks;
            case kRecordSpin:
                return ValueForm::Degrees;
            default:
                return kind < kSavedRecordKinds ? ValueForm::Distance : ValueForm::Count;
            }
        }
    }

    // fillRecordsFrame: for each kind of record 1 .. 13, its name (its
    // $KSTR_RECORDS_NAMEnn text, a line each but the last), its rider
    // character, its player and its value (a score, tricks, a spin in
    // degrees, or a distance in feet and inches) added to the frame's four
    // lists.
    void fillRecordsFrame(ee::Ptr<zge::zgui::Node> frame, uint32_t level)
    {
        ee::Local<char[0x100]> text;
        ee::Local<char[kLineBytes]> line;
        ee::Local<char[kListBytes]> names;
        ee::Local<char[kListBytes]> characters;
        ee::Local<char[kListBytes]> players;
        ee::Local<char[kListBytes]> values;
        level &= 0xFF;
        names[0] = 0;
        characters[0] = 0;
        players[0] = 0;
        values[0] = 0;
        uint32_t left = kSavedRecordKinds - 1;
        for (uint32_t kind = 1; kind < kSavedRecordKinds; ++kind)
        {
            const ee::Ptr<SavedRecord> record = levelSavedRecord(level, kind & 0xFF);
            --left;
            zge::formatString(text, kRecordNameFormat, kind);
            zge::stringCopy(line, text);
            if (left != 0)
                zge::appendString(line, kLineEnd);
            zge::appendString(names, line);

            zge::stringCopy(line, kCharacterRows[record->character].boardName);
            zge::appendString(line, kLineEnd);
            zge::appendString(characters, line);

            zge::formatString(text, kPlayerNameFormat, ee::Ptr<const char>(record->name));
            zge::appendString(players, text);

            switch (valueForm(kind))
            {
            case ValueForm::Count:
                zge::formatString(text, kCountFormat, sce::libgcc::fptoui(record->value));
                break;
            case ValueForm::Tricks:
                zge::formatString(text, kTricksFormat, sce::libgcc::fptoui(record->value));
                break;
            case ValueForm::Degrees:
                zge::formatString(text, kDegreesFormat, sce::libgcc::fptoui(record->value) * kDegreesPerHalfTurn);
                break;
            case ValueForm::Distance:
                formatFeetInches(text, record->value);
                break;
            }
            zge::stringCopy(line, text);
            zge::appendString(line, kLineEnd);
            zge::appendString(values, line);
        }
        zge::zgui::setVariable(zge::zgui::findNode(frame, kRecordNameList, kFrameNode), kItems, names);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kCharacterList, kFrameNode), kItems, characters);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kPlayerNameList, kFrameNode), kItems, players);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kRecordValueList, kFrameNode), kItems, values);
    }
    READABLE_FUNCTION(fillRecordsFrame, Verified);
}
