// DM2's HORSE word screen (screens.h; the common UI's keyboard frame,
// "CommonUI.KeyboardFrame"): the word a HORSE game is played for, typed in
// before its first turn. DM2's own code (overlay.c).

#include "screens.h"

#include "dm2/game/round.h"
#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/dm2/overlay/screen_horse_word.cpp.h"

namespace game::dm2
{
    namespace
    {
        constexpr ee::Ptr<const char> kKeyboardFrame{sym::keyboardFramePath};         // "CommonUI.KeyboardFrame"
        constexpr ee::Ptr<const char> kTopHeaderTitle{sym::screenTopHeaderTitlePath}; // "CommonUI.TopHeader.Title"
        constexpr ee::Ptr<const char> kTitleName{sym::screenTitleName};               // "Title"
        constexpr ee::Ptr<const char> kKeyboardName{sym::screenKeyboardName};         // "Keyboard"
        constexpr ee::Ptr<const char> kEditBoxName{sym::keyboardEditBoxPath};         // "Keyboard.EditBox"
        constexpr ee::Ptr<const char> kText{sym::screenTextName};                     // "Text"
        constexpr ee::Ptr<const char> kMaxItemsName{sym::screenMaxItemsName};         // "MaxItems"
        constexpr ee::Ptr<const char> kOnOkName{sym::screenOnOkName};                 // "OnOk"
        constexpr ee::Ptr<const char> kOnCancelName{sym::screenOnCancelName};         // "OnCancel"
        constexpr ee::Ptr<const char> kStringName{sym::screenStringName};             // "String"
        constexpr ee::Ptr<const char> kNoText{sym::g_textEmpty};                      // ""
        // The headers: HORSE's mode name on top, the prompt in the frame.
        constexpr ee::Ptr<const char> kModeNameKey{sym::multiplayerModeNameKey};    // "$KSTRMP_MODENAME_MODE%02d"
        constexpr ee::Ptr<const char> kPromptKey{sym::horseWordPromptKey};          // "$KSTRMP_MPHORSEWORD_SELECTWORD"
        // What OK and cancel run: "SET Root.MsgType {0}" / "{-1}", then
        // "CALL bmxoverlayMPHorseWordHandler".
        constexpr ee::Ptr<const char> kOkScript{sym::horseWordOkScript};
        constexpr ee::Ptr<const char> kCancelScript{sym::horseWordCancelScript};
        // The word as it is typed ("BMX" to begin with): eight letters at most.
        constexpr ee::Ptr<char> kTypedWord{sym::g_horseWordEntry};
        constexpr int32_t kWordLetters = 8;

        // What the keyboard's script asks (g_uiMessageType): the word taken,
        // or the screen left.
        inline ee::Global<int32_t, sym::g_uiMessageType> g_uiMessageType;
        constexpr int32_t kWordTaken = 0, kWordCancelled = -1;
        // The menus' blocked sound (sound/menu_sounds.cpp's sound event 10):
        // an empty word is not taken.
        constexpr uint32_t kBlockedSoundEvent = 10;
    }

    // showHorseWordScreen: as it opens, any player's pad answers; the top
    // header shows HORSE's name and the frame its prompt; the keyboard takes
    // eight letters, OK and cancel hand them to
    // bmxoverlayMPHorseWordHandler, and it starts with the word typed last.
    // As it closes, the top header's title cleared and the frame's DeInit.
    void showHorseWordScreen(uint32_t shown)
    {
        ee::Local<char[0x40]> title;
        ee::Local<int32_t> value;
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kKeyboardFrame);
        if (!shown)
        {
            zge::zgui::setVariable(uiWidget(kTopHeaderTitle), kText, kNoText);
            playWidgetAction(frame, kDeInitAction);
            return;
        }
        *g_uiModalPlayer = kAnyPlayer;
        zge::formatString(title, kModeNameKey, kHorseMode);
        zge::zgui::setVariable(uiWidget(kTopHeaderTitle), kText, title);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kTitleName, 0), kText, kPromptKey);
        *value = kWordLetters;
        zge::zgui::setVariable(zge::zgui::findNode(frame, kEditBoxName, 0), kMaxItemsName, value);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kOnOkName, kOkScript);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kOnCancelName, kCancelScript);
        playWidgetAction(frame, kInitAction);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kStringName, kTypedWord);
        zge::zgui::setVariable(zge::zgui::findNode(frame, kEditBoxName, 0), kText, kTypedWord);
    }
    READABLE_FUNCTION(showHorseWordScreen, Verified);

    // bmxoverlayMPHorseWordHandler: the keyboard's word taken: an empty one
    // with the blocked sound; else it becomes the HORSE word and the first
    // turn starts. Cancelled, back to the game select.
    void bmxoverlayMPHorseWordHandler()
    {
        const ee::Ptr<zge::zgui::Node> frame = uiWidget(kKeyboardFrame);
        if (*g_uiMessageType == kWordCancelled)
        {
            game::tail(showGameScreen, kScreenMultiplayerGameSelect);
            return;
        }
        if (*g_uiMessageType != kWordTaken)
            return;
        zge::zgui::copyVariable(zge::zgui::findNode(frame, kKeyboardName, 0), kStringName, kTypedWord);
        if (zge::stringLength(kTypedWord) == 0)
        {
            game::tail(soundEvent, kBlockedSoundEvent, 0, 0, 0, 0);
            return;
        }
        showGameScreen(kScreenNone);
        setHorseWord(kTypedWord);
        game::tail(setRoundPhase, kTurnStartPhase);
    }
    READABLE_FUNCTION(bmxoverlayMPHorseWordHandler, Verified);
}
