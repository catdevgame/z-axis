// DM2's high score screen (screens.h; the common UI's keyboard frame,
// "CommonUI.KeyboardFrame"): the name a record is set under, typed in.
// DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/action/saved_records.h"
#include "dm2/game/players.h"
#include "dm2/game/records.h"
#include "dm2/game/round.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/overlay/screen_high_score.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kKeyboardFrame{sym::keyboardFramePath};       // "CommonUI.KeyboardFrame"
        constexpr ee::Ptr<const char> kTopHeaderTitle{sym::screenTopHeaderTitlePath}; // "CommonUI.TopHeader.Title"
        constexpr ee::Ptr<const char> kTitleName{sym::screenTitleName};             // "Title"
        constexpr ee::Ptr<const char> kKeyboardName{sym::screenKeyboardName};       // "Keyboard"
        constexpr ee::Ptr<const char> kEditBoxName{sym::keyboardEditBoxPath};       // "Keyboard.EditBox"
        constexpr ee::Ptr<const char> kText{sym::screenTextName};                   // "Text"
        constexpr ee::Ptr<const char> kNoCancelName{sym::screenNoCancelName};       // "NoCancel"
        constexpr ee::Ptr<const char> kMaxItemsName{sym::screenMaxItemsName};       // "MaxItems"
        constexpr ee::Ptr<const char> kOnOkName{sym::screenOnOkName};               // "OnOk"
        constexpr ee::Ptr<const char> kOnCancelName{sym::screenOnCancelName};       // "OnCancel"
        constexpr ee::Ptr<const char> kStringName{sym::screenStringName};           // "String"
        constexpr ee::Ptr<const char> kItemsName{sym::g_textItems};                 // "Items"
        constexpr uint32_t kNoText = sym::g_textEmpty;                              // ""
        // The headers' texts: "$KSTR_HIGHSCORE_00" on top, "..._01" in the frame.
        constexpr uint32_t kTopTitleKey = sym::highScoreTitleKey;
        constexpr uint32_t kFrameTitleKey = sym::highScorePromptKey;
        // What OK runs: "SET Root.MsgType {0}\nCALL bmxoverlayHighScoreHandler".
        constexpr uint32_t kOkScript = sym::highScoreOkScript;
        constexpr uint32_t kNameFormat = sym::screenStringFormat;   // "%s"
        constexpr uint32_t kDefaultName = sym::defaultPlayerName;   // "player"
        // The name: ten characters at most; it is typed into this.
        constexpr int32_t kNameChars = 10;
        constexpr uint32_t kNameText = sym::g_highScoreName;
    }

    // showHighScoreScreen: as it opens, the keyboard frame set up for a
    // record's name (ten characters, no cancel; OK hands it to
    // bmxoverlayHighScoreHandler), the current player's name in it, and its
    // cursor on the first key for the default "player", else on its last.
    // As it closes, the top header's title cleared and the frame's DeInit.
    void showHighScoreScreen(uint32_t shown)
    {
        ee::Local<char[0x40]> keys;
        ee::Local<int32_t> value;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kKeyboardFrame);
        if (shown == 0)
        {
            zge::zgui::setVariable(uiWidget(kTopHeaderTitle), kText, kNoText);
            playWidgetAction(frame, kDeInitAction);
            return;
        }
        zge::zgui::setVariable(uiWidget(kTopHeaderTitle), kText, kTopTitleKey);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kText, kFrameTitleKey);
        *value = 1;
        zge::zgui::setVariable(frame, kNoCancelName, value);
        *value = kNameChars;
        zge::zgui::setVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kMaxItemsName, value);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kOnOkName, kOkScript);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kOnCancelName, kNoText);
        playWidgetAction(frame, kInitAction);

        const ee::Ptr<PlayerSetup> setup = ee::Ptr<PlayerSetup>(sym::g_playerSetups)
                                         + static_cast<ptrdiff_t>(*g_currentPlayer);
        zge::formatString(kNameText, kNameFormat, ee::Ptr<char>(setup->name));
        zge::zgui::setVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kStringName, kNameText);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kEditBoxName, 0), kText, kNameText);
        if (zge::stringCompareNoCase(kNameText, kDefaultName) == 0)
        {
            *value = 0;
        }
        else
        {
            zge::zgui::copyVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kItemsName, keys);
            *value = zge::stringLength(keys) - 1;
        }
        zge::zgui::setVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kSelectedIndexName, value);
    }
    READABLE_FUNCTION(showHighScoreScreen, Verified);

    namespace
    {
        // What the keyboard's script asks (g_uiMessageType): the name as it
        // stands taken, or its last character deleted.
        inline ee::Global<int32_t, sym::g_uiMessageType> g_uiMessageType;
        constexpr int32_t kNameTaken = 0, kNameBackspace = -1;
    }

    // bmxoverlayHighScoreHandler: the high score screen's keyboard. A
    // backspace takes the name's last character off (the keyboard's string
    // and its edit box), with the UI's cancel sound, or its blocked sound
    // when the name is empty. The name taken goes on every record the turn
    // set (each kind's saved record: its value, the player's character and
    // its bike, the name) and becomes the player's name; then the records
    // shown at a turn's end.
    void bmxoverlayHighScoreHandler()
    {
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kKeyboardFrame);
        const ee::Ptr<char> name(kNameText);
        if (*g_uiMessageType == kNameBackspace)
        {
            zge::zgui::copyVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kStringName, name);
            if (name[0] == 0)
            {
                zge::zgui::g_zgui->blockedSound();
                return;
            }
            name[zge::stringLength(name) - 1] = 0;
            zge::zgui::setVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kStringName, name);
            zge::zgui::setVariable(zge::zgui::findNode(frame, kEditBoxName, 0), kText, name);
            zge::zgui::g_zgui->cancelSound();
            return;
        }
        if (*g_uiMessageType != kNameTaken)
            return;
        zge::zgui::copyVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kStringName, name);
        for (uint32_t kind = 0; kind < kRecordKinds; ++kind)
        {
            const uint8_t mode = multiplayerMode();
            const ee::Ptr<GameRecord> record = gameRecord(recordKey(mode, singlePlayerMode(), kind, kSavedRecords));
            if (!record || !(record->marks & kRecordSet))
                continue;
            const uint8_t savedMode = multiplayerMode();
            const ee::Ptr<SavedRecord> saved = savedRecord(recordKey(savedMode, singlePlayerMode(), kind, kSavedRecords));
            saved->flags |= kSavedRecordSet;
            saved->value = record->value;
            PlayerSetup &setup = (*g_playerSetups)[*g_currentPlayer];
            saved->character = setup.character;
            saved->bikeModel = playerCharacter(*g_currentPlayer)->bikeModel;
            saved->bike = playerCharacter(*g_currentPlayer)->bike;
            zge::formatString(ee::Ptr<char>(saved->name), kNameFormat, name);
            zge::formatString(ee::Ptr<char>((*g_playerSetups)[*g_currentPlayer].name), kNameFormat, name);
        }
        game::tail(showGameScreen, kScreenTurnRecords);
    }
    READABLE_FUNCTION(bmxoverlayHighScoreHandler, Verified);
}
