// BMX XXX's high score screen (screens.h; the common UI's
// "KeyboardFrame"): the name the turn's records are saved under. BMX XXX's
// own code (./src/com/action/overlay.c).

#include "screens.h"

#include "bmx/game/records.h"
#include "bmx/game/round.h"
#include "bmx/overlay/screen_calls.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/overlay/screen_high_score.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        constexpr uint32_t kKeyboardFrame = sym::g_textKeyboardFrame;      // "UI_common.KeyboardFrame"
        constexpr uint32_t kTopHeaderTitle = sym::g_textTopHeaderTitle;    // "UI_common.TopHeader.Title"
        constexpr uint32_t kKeyboardWidget = sym::g_textKeyboard;          // "Keyboard"
        constexpr uint32_t kEditBoxWidget = sym::g_textKeyboardEditBox;    // "Keyboard.EditBox"
        constexpr uint32_t kTitleWidget = sym::g_textTitle;                // "Title"
        constexpr uint32_t kStringName = sym::g_textString;                // "String"
        constexpr uint32_t kNoCancelName = sym::g_textNoCancel;            // "NoCancel"
        constexpr uint32_t kMaxItemsName = sym::g_textMaxItems;            // "MaxItems"
        constexpr uint32_t kOnOkName = sym::g_textOnOk;                    // "OnOk"
        constexpr uint32_t kOnCancelName = sym::g_textOnCancel;            // "OnCancel"
        constexpr uint32_t kHeaderText = sym::g_textHighScore00;           // "$KSTR_HIGHSCORE_00"
        constexpr uint32_t kTitleText = sym::g_textHighScore01;            // "$KSTR_HIGHSCORE_01"
        // "SET Root.MsgType {0}\nCALL bmxuiHandler": the keyboard's OK is this screen's "chosen".
        constexpr uint32_t kOkScript = sym::g_textKeyboardOkScript;
        constexpr uint32_t kNameFormat = sym::bmx::g_textStringFormat;      // "%s"
        constexpr uint32_t kDefaultName = sym::g_textPlayer;               // "player"

        // The name being entered.
        constexpr uint32_t kNameChars = 0x20;
        inline ee::Global<char[kNameChars], sym::g_enteredName> g_enteredName;
        constexpr uint32_t kLongestName = kNameChars - 1;
    }

    // highScoreScreenMessage: the keyboard opens on the player's name (on
    // its first letter for the default "player", past its last otherwise).
    // OK saves every record this game set under the name, with the player's
    // character and bike, keeps the name as the player's, and shows the
    // records. Cancel rubs out the last letter (the keyboard itself cannot
    // be left).
    void highScoreScreenMessage(int32_t message)
    {
        ee::Local<char[0x40]> items;
        ee::Local<uint32_t> value;

        const ee::Ptr<Node> keyboardFrame = zge::zgui::findNode(*zge::zgui::g_root, kKeyboardFrame, 1);
        if (!keyboardFrame)
            return;

        switch (message)
        {
        case kScreenChosen:
            zge::zgui::copyVariable(zge::zgui::findNode(keyboardFrame, kKeyboardWidget, 1), kStringName, &*g_enteredName);
            for (uint32_t kind = 0; kind < kSavedRecordKinds; ++kind)
            {
                const ee::Ptr<GameRecord> record = gameRecord(recordKey(gameMode(), kind, 0));
                if (!record || (record->marks & kRecordSet) == 0)
                    continue;
                const ee::Ptr<SavedRecord> saved = savedRecord(recordKey(gameMode(), kind, 0));
                saved->flags |= kSavedRecordNew;
                saved->value = record->value;
                saved->character = calls::playerSetup(*calls::g_currentPlayer)->character;
                saved->bike = playerCharacter(*calls::g_currentPlayer)->bike;
                saved->bikeChoice = playerCharacter(*calls::g_currentPlayer)->bikeChoice;
                zge::formatString(ee::Ptr<char>(&saved->name[0]), kNameFormat, &*g_enteredName);
                zge::formatString(ee::Ptr<char>(&calls::playerSetup(*calls::g_currentPlayer)->name[0]), kNameFormat,
                                  &*g_enteredName);
            }
            showGameScreen(kScreenNewRecords);
            return;

        case kScreenEnter:
            zge::zgui::setVariable(zge::zgui::findNode(*zge::zgui::g_root, kTopHeaderTitle, 1), kTextName, kHeaderText);
            zge::zgui::setVariable(zge::zgui::findNode(keyboardFrame, kTitleWidget, 1), kTextName, kTitleText);
            value = 1;
            zge::zgui::setVariable(keyboardFrame, kNoCancelName, value);
            value = kLongestName;
            zge::zgui::setVariable(zge::zgui::findNode(keyboardFrame, kKeyboardWidget, 1), kMaxItemsName, value);
            zge::zgui::setVariable(zge::zgui::findNode(keyboardFrame, kKeyboardWidget, 1), kOnOkName, kOkScript);
            zge::zgui::setVariable(zge::zgui::findNode(keyboardFrame, kKeyboardWidget, 1), kOnCancelName, kNoSeparator);
            playUiNode(keyboardFrame, kInitAction);

            zge::formatString(&*g_enteredName, kNameFormat,
                              ee::Ptr<char>(&calls::playerSetup(*calls::g_currentPlayer)->name[0]));
            zge::zgui::setVariable(zge::zgui::findNode(keyboardFrame, kKeyboardWidget, 1), kStringName, &*g_enteredName);
            zge::zgui::setVariable(zge::zgui::findNode(keyboardFrame, kEditBoxWidget, 1), kTextName, &*g_enteredName);
            if (zge::stringCompareNoCase(&*g_enteredName, kDefaultName) == 0)
            {
                value = 0;
            }
            else
            {
                zge::zgui::copyVariable(zge::zgui::findNode(keyboardFrame, kKeyboardWidget, 1), kItemsName, items);
                value = static_cast<uint32_t>(zge::stringLength(items) - 1);
            }
            zge::zgui::setVariable(zge::zgui::findNode(keyboardFrame, kKeyboardWidget, 1), kSelectedIndexName, value);
            return;

        case kScreenLeaveNow:
            zge::zgui::setVariable(zge::zgui::findNode(*zge::zgui::g_root, kTopHeaderTitle, 1), kTextName, kNoSeparator);
            playUiNode(keyboardFrame, kShutdownAction);
            return;

        case kScreenCancelled:
            zge::zgui::copyVariable(zge::zgui::findNode(keyboardFrame, kKeyboardWidget, 1), kStringName, &*g_enteredName);
            if ((*g_enteredName)[0] == '\0')
            {
                zge::zgui::g_zgui->blockedSound();
                return;
            }
            (*g_enteredName)[zge::stringLength(&*g_enteredName) - 1] = '\0';
            zge::zgui::setVariable(zge::zgui::findNode(keyboardFrame, kKeyboardWidget, 1), kStringName, &*g_enteredName);
            zge::zgui::setVariable(zge::zgui::findNode(keyboardFrame, kEditBoxWidget, 1), kTextName, &*g_enteredName);
            zge::zgui::g_zgui->cancelSound();
            return;

        default:
            return;
        }
    }
    READABLE_FUNCTION(highScoreScreenMessage, Draft);
}
