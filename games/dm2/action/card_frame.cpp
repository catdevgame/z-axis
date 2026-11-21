// The memory card frame's steps (card_frame.h): the step change that shows a
// step, the frame's run that drives the card work, and the script function
// that takes the user's answers. DM2's own code (action.c).

#include "card_frame.h"
#include "ui_frame.h"

#include "dm2/sound/sound_events.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "dm2/editor/edit_gui.h"
#include "ee/readable.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/action/card_frame.cpp.h"

namespace game::dm2
{
    namespace
    {
        using zge::zgui::Node;

        // The frame and the widgets below it.
        constexpr uint32_t kMemCardFramePath = sym::memCardFramePath;       // "CommonUI.FrameMemCard"
        constexpr uint32_t kFrameMenuPath = sym::frameMenuPath;             // "CommonUI.FrameMemCard.FrameMenu"
        constexpr uint32_t kFrameMenuMenuPath = sym::frameMenuMenuPath;     // "CommonUI.FrameMemCard.FrameMenu.Menu"
        constexpr uint32_t kFrameMenuMenuName = sym::frameMenuMenuName;     // "FrameMenu.Menu"
        constexpr uint32_t kFrameMenuMessageName = sym::frameMenuMessageName; // "FrameMenu.Message"
        constexpr uint32_t kFrameTitleName = sym::frameTitleName;           // "FrameTitle"
        constexpr uint32_t kReplaySlotMenuName = sym::replaySlotMenuName;   // "FrameReplaySlot.Menu"
        constexpr uint32_t kEnterNameFramePath = sym::enterNameFramePath;   // "CommonUI.FrameMemCard.FrameEnterName"
        constexpr uint32_t kEnterParkNameFramePath = sym::enterParkNameFramePath; // "...FrameEnterParkName"
        constexpr uint32_t kTopHeaderTitlePath = sym::topHeaderTitlePath;   // "CommonUI.TopHeader.Title"
        // Their variables.
        constexpr uint32_t kText = sym::textVariableName;         // "Text"
        constexpr uint32_t kSelectedIndex = sym::selectedIndexName; // "SelectedIndex"
        constexpr uint32_t kItems = sym::itemsName;               // "Items"
        constexpr uint32_t kVisibleCount = sym::visibleCountName; // "VisibleCount"
        constexpr uint32_t kState = sym::stateName;               // "State"
        // Variables by their path from the root.
        constexpr ee::Ptr<const char> kProgressBarWidthPath{sym::progressBarWidthPath}; // "...FrameMenu.FrameProgress.Bar.Width"
        constexpr ee::Ptr<const char> kSelectCardIndexPath{sym::selectCardIndexPath};   // "...FrameSelectCard.Menu.SelectedIndex"
        constexpr ee::Ptr<const char> kFrameMenuIndexPath{sym::frameMenuIndexPath};     // "...FrameMenu.Menu.SelectedIndex"
        constexpr ee::Ptr<const char> kEnterNameStatePath{sym::enterNameStatePath};     // "...FrameEnterName.State"
        constexpr ee::Ptr<const char> kEnterNameTextPath{sym::enterNameTextPath};       // "...FrameEnterName.Name.Text"
        constexpr ee::Ptr<const char> kEnterParkNameStatePath{sym::enterParkNameStatePath};     // "...FrameEnterParkName.State"
        constexpr ee::Ptr<const char> kEnterParkAuthorStatePath{sym::enterParkAuthorStatePath}; // "...FrameEnterParkAuthorName.State"
        constexpr ee::Ptr<const char> kEnterParkNameTextPath{sym::enterParkNameTextPath};       // "...FrameEnterParkName.Name.Text"
        constexpr ee::Ptr<const char> kEnterParkAuthorTextPath{sym::enterParkAuthorTextPath};   // "...FrameEnterParkAuthorName.Name.Text"
        constexpr ee::Ptr<const char> kEnterAuthorNameStatePath{sym::enterAuthorNameStatePath}; // "...FrameEnterAuthorName.State"

        // The scripts, run on the root or on the frame.
        constexpr ee::Ptr<char> kTitleHidden{sym::titleHiddenScript}; // the top header's title and notch hidden
        constexpr ee::Ptr<char> kTitleShown{sym::titleShownScript};   // and shown
        constexpr ee::Ptr<char> kRootLockUser{sym::rootLockUserScript}; // "PLAY Root.CommonUI.LockUser"
        constexpr ee::Ptr<char> kFrameDeInit{sym::frameDeInitScript}; // "PLAY FrameMenu.DeInit\nPLAY FrameTitle.DeInit"
        constexpr ee::Ptr<char> kFrameMenuDeInit{sym::frameMenuDeInitScript};   // "PLAY FrameMenu.DeInit"
        constexpr ee::Ptr<char> kFrameTitleDeInit{sym::frameTitleDeInitScript}; // "PLAY FrameTitle.DeInit"
        constexpr ee::Ptr<char> kTopHeaderHidden{sym::topHeaderHiddenScript};   // "SET Root.CommonUI.TopHeader.Visible {0}"
        constexpr ee::Ptr<char> kBottomHeaderHidden{sym::bottomHeaderHiddenScript}; // the same for BottomHeader
        constexpr ee::Ptr<char> kSelectCardDeInit{sym::selectCardDeInitScript}; // "PLAY FrameSelectCard.DeInit"
        constexpr ee::Ptr<char> kSelectCardInit{sym::selectCardInitScript};     // "PLAY FrameSelectCard.Init"
        constexpr ee::Ptr<char> kFrameMenuShow{sym::frameMenuShowScript};       // "PLAY FrameMenu.Show"
        constexpr ee::Ptr<char> kReplaySlotInit{sym::replaySlotInitScript};     // "PLAY FrameReplaySlot.Init"
        constexpr ee::Ptr<char> kFrameTitleGone{sym::frameTitleGoneScript}; // "PLAY FrameTitle.DeInit\nSET FrameTitle.Visible {0}"
        constexpr ee::Ptr<char> kProgressShown{sym::progressShownScript};   // the progress bar emptied and shown
        constexpr ee::Ptr<char> kProgressHidden{sym::progressHiddenScript}; // "SET FrameMenu.FrameProgress.Visible {0}"
        constexpr ee::Ptr<char> kRootBackgroundVisible{sym::rootBackgroundVisibleScript};
        constexpr ee::Ptr<char> kFadeInButton1{sym::fadeInButton1Script}; // "PLAY Root.CommonUI.FadeIn\nPLAY Root.CommonUI.DisplayButton1"
        constexpr ee::Ptr<char> kUnlockUserButton1{sym::unlockUserButton1Script}; // the user unlocked, one button shown
        constexpr ee::Ptr<char> kUnlockUserButton2{sym::unlockUserButton2Script}; // two buttons shown
        constexpr ee::Ptr<char> kDisplayButton2{sym::displayButton2Script}; // "PLAY Root.CommonUI.DisplayButton2"
        constexpr ee::Ptr<char> kMessageShown{sym::messageShownScript};     // "SET FrameMenu.Message.Visible {1}"
        constexpr ee::Ptr<char> kMenuShown{sym::menuShownScript};           // "SET FrameMenu.Menu.Visible {1}"
        constexpr ee::Ptr<char> kMenuHidden{sym::menuHiddenScript};         // "SET FrameMenu.Menu.Visible {0}"
        constexpr ee::Ptr<char> kMenuFocus{sym::menuFocusScript};           // "SETFOCUS FrameMenu.Menu"
        constexpr ee::Ptr<char> kKillFocus{sym::killFocusScript};           // "KILLFOCUS"
        constexpr ee::Ptr<char> kInit{sym::initScript};                     // "PLAY Init\n"

        // Formats and pieces of text.
        constexpr uint32_t kSaveTitleFormat = sym::saveTitleFormat;     // "$KSTR_SAVE_TITLE%02d", by the frame's kind
        constexpr uint32_t kSaveMessageFormat = sym::saveMessageFormat; // "KSTR_SAVE_MESSAGE%02d", by the step
        constexpr uint32_t kBootMessageFormat = sym::bootMessageFormat; // "KSTR_BOOT_MESSAGE%02d"
        constexpr uint32_t kSaveButtonFormat = sym::saveButtonFormat;   // "$KSTR_SAVE_BUTTON%02d"
        // "PLAY FrameMenu.Init\nSET FrameTitle.Message.Text {"$KSTR_SAVE_SLOT%d"}", and the same with
        // "PLAY Root.CommonUI.DisplayButton1" between the two.
        constexpr uint32_t kFrameMenuInitSlotFormat = sym::frameMenuInitSlotFormat;
        constexpr uint32_t kFrameMenuInitButton1SlotFormat = sym::frameMenuInitButton1SlotFormat;
        constexpr uint32_t kTwoStringsFormat = sym::twoStringsFormat; // "%s%s"
        constexpr uint32_t kLineBreak = sym::lineBreakText;           // "\n" as a menu's Items spell it
        constexpr uint32_t kEmptySlotLine = sym::emptySlotLine;       // "* EMPTY *" and a line break
        constexpr uint32_t kNoText = sym::emptyText;                  // ""
        // What a card-changed or card-removed message says failed.
        constexpr uint32_t kFormatWord = sym::formatWord, kLoadWord = sym::loadWord, kSaveWord = sym::saveWord,
                           kOperationWord = sym::operationWord;
        // The start check's messages: no space, no card, not formatted.
        constexpr uint32_t kBootNoSpace = 0, kBootNoCard = 1, kBootUnformatted = 2;

        // What each frame kind's data is called in the messages ("game",
        // "replay", "park editor") and the kilobytes it needs on the card.
        inline ee::Global<uint32_t[8], sym::g_cardDataNames> g_cardDataNames;
        inline ee::Global<uint32_t[8], sym::g_cardDataKilobytes> g_cardDataKilobytes;

        // The names entered for a replay, a park and the park's author.
        constexpr ee::Ptr<char> kReplayNameEntered{sym::g_replayNameEntered};
        constexpr ee::Ptr<char> kParkNameEntered{sym::g_parkNameEntered};
        constexpr ee::Ptr<char> kParkAuthorEntered{sym::g_parkAuthorEntered};

        // A replay saved under this name renames every trick to the other.
        constexpr uint32_t kSecretReplayName = sym::secretReplayName; // "ELOCIN"
        constexpr uint32_t kSecretTrickName = sym::secretTrickName;   // "NICOLE"
        constexpr uint32_t kTrickNames = 72;
        inline ee::Global<ee::Ptr<char>[kTrickNames], sym::trickNames> g_trickNames;

        // The game's volume settings (saved with the game): a loaded game's
        // are put into effect.
        inline ee::Global<uint8_t, sym::g_sfxVolumeSetting> g_sfxVolumeSetting;
        inline ee::Global<uint8_t, sym::g_musicVolumeSetting> g_musicVolumeSetting;
        constexpr uint32_t kVolumeEvent = 0, kSfxVolume = 0, kMusicVolume = 1;

        // How long each card step shows its progress bar before the card's
        // answer is looked at, in frames, and the frames at which the work
        // is prepared and started. The bar is 300 units wide when full and
        // never under 2.
        constexpr uint32_t kCardStepFrames = 60, kFormatStepFrames = 120;
        constexpr uint32_t kPrepareFrame = 1, kStartFrame = 10;
        constexpr float kFullBar = 300.0f, kLeastBar = 2.0f, kHalf = 0.5f, kHalfBar = 150.0f;

        // ZGE2's widgets are scene objects; this is the object's shown bit
        // (what a widget's Visible variable reads, and the bit the engine's
        // setSceneObjectShown sets: the same bit as zge::kObjectMoved).
        constexpr uint64_t kWidgetShown = zge::kObjectMoved;

        // The C library's memset, as the game calls it.
        void fillBytes(ee::Address bytes, uint32_t value, uint32_t count) { ee::game(zge::kMemset, bytes, value, count); }

        ee::Ptr<Node> uiRoot() { return *g_cardFrameUiRoot; }
        ee::Ptr<Node> nodeAt(ee::Ptr<Node> below, uint32_t path) { return zge::zgui::findNode(below, path, 0); }
        ee::Ptr<Node> memCardFrame() { return nodeAt(uiRoot(), kMemCardFramePath); }
        void runScript(ee::Ptr<Node> on, ee::Ptr<char> text) { zge::zgui::runScriptLines(on, text); }

        // An unsigned word as a float (the compiled code's two ways).
        ee::Float unsignedToFloat(uint32_t w)
        {
            if (static_cast<int32_t>(w) >= 0)
                return ee::toFloat(static_cast<int32_t>(w));
            const ee::Float half = ee::toFloat(static_cast<int32_t>((w & 1u) | (w >> 1)));
            return half + half;
        }

        bool isAnyOf(uint32_t value, std::initializer_list<uint32_t> values)
        {
            for (const uint32_t one : values)
                if (value == one)
                    return true;
            return false;
        }

        // The frame's work is done: the frame's menu and title played out,
        // the request told, and the frame left at kStepOver.
        void finishRequest()
        {
            runScript(memCardFrame(), kFrameDeInit);
            g_uiFrameRequest->onDone(g_uiFrameRequest->data, g_uiFrameRequest->dataHandle);
            *g_uiFrameStepResults = 0;
            *g_uiFrameStep = kStepOver;
            *g_uiFrameStepFrames = 0;
        }

        // The frame closes without its work: the request told, kStepOver asked for.
        void closeRequest()
        {
            if (g_uiFrameRequest->onClosed)
                g_uiFrameRequest->onClosed(g_uiFrameRequest->data, g_uiFrameRequest->dataHandle);
            setUiFrameStep(kStepOver);
        }

        // The auto load found nothing to load on either card.
        void autoLoadOver()
        {
            if (!g_uiFrameRequest->onDone)
                setUiFrameStep(kStepLoadComplete);
            else
                finishRequest();
        }
    }

    void setUiFrameStep(uint32_t step)
    {
        ee::Local<char[0x100]> message;
        ee::Local<char[0x40]> key;
        ee::Local<char[0x200]> script;
        ee::Local<int32_t> value;

        const ee::Ptr<Node> frame = memCardFrame();
        // What the step being left showed is played out.
        switch (*g_uiFrameStep)
        {
        case kStepChooseSaveSlot:
        case kStepChooseLoadSlot:
            runScript(frame, kSelectCardDeInit);
            break;
        case kStepEnterName:
        case kStepPickReplayToLoad:
        case kStepPickParkToLoad:
            if (*g_uiMessageType != kMessageCancel)
                runScript(frame, kFrameMenuShow);
            break;
        case kStepSaveComplete:
        case kStepLoadComplete:
            if (*g_uiFrameStep == kStepLoadComplete)
            {
                soundEvent(kVolumeEvent, kSfxVolume, *g_sfxVolumeSetting, 0, 0);
                soundEvent(kVolumeEvent, kMusicVolume, *g_musicVolumeSetting, 0, 0);
            }
            runScript(frame, kFrameMenuDeInit);
            runScript(frame, kFrameTitleDeInit);
            break;
        default:
            break;
        }
        if (step == kStepOver)
            return;

        *value = 0;
        const StepButtons &buttons = (*g_stepButtons)[step];
        zge::zgui::setVariable(nodeAt(frame, kFrameMenuMenuName), kSelectedIndex, value);

        // The step's own part of the frame.
        bool secondButtonSelected = false;
        bool shown = false;
        ee::Ptr<Node> target = frame;
        ee::Ptr<char> last;
        switch (step)
        {
        case kStepChooseSaveSlot:
        case kStepChooseLoadSlot:
            last = kSelectCardInit;
            break;
        case kStepAskFormat:
        case kStepAskSave:
        case kStepAskOverwrite:
        case kStepAskLoad:
            if (buttons.count == 1)
                runScript(uiRoot(), kUnlockUserButton1);
            else if (buttons.count == 2)
                runScript(uiRoot(), kUnlockUserButton2);
            runScript(uiRoot(), kDisplayButton2);
            secondButtonSelected = true;
            break;
        case kStepPickReplayToSave:
        case kStepPickReplayToLoad:
        {
            // The replay slots as the list's items, a line each.
            script[0] = 0;
            for (uint32_t slot = 0; slot < kReplaySlots; ++slot)
            {
                if (!((*g_replaySlots)[slot].flags & kSlotUsed))
                    zge::appendString(script, kEmptySlotLine);
                else
                {
                    zge::appendString(script, ee::Ptr<char>(sym::g_replaySlots + slot * sizeof(ReplaySlot)
                                                            + offsetof(ReplaySlot, name)));
                    zge::appendString(script, kLineBreak);
                }
            }
            script[zge::stringLength(script) - 1] = 0;
            zge::zgui::setVariable(nodeAt(frame, kReplaySlotMenuName), kItems, script);
            if (*g_uiMessageType != kMessageCancel)
                runScript(frame, kFrameMenuDeInit);
            last = kReplaySlotInit;
            break;
        }
        case kStepEnterName:
            if (*g_uiFrameKind == kUiFrameSaveReplay)
                target = nodeAt(uiRoot(), kEnterNameFramePath);
            else if (*g_uiFrameKind == kUiFrameSavePark)
                target = nodeAt(uiRoot(), kEnterParkNameFramePath);
            else
            {
                shown = true;
                break;
            }
            last = kInit;
            break;
        case kStepCheckingToSave:
        case kStepFormatting:
        case kStepSaving:
        case kStepCreating:
        case kStepCheckingToLoad:
        case kStepLoading:
        {
            // The frame's menu comes up under the slot's title unless it is
            // up already; the auto load shows no title, and nothing new when
            // it goes from checking a card to loading from it.
            const ee::Ptr<zge::SceneObject> title = widgetObject(nodeAt(frame, kFrameTitleName));
            if (!(title->flags & kWidgetShown))
            {
                if (*g_uiFrameKind != kUiFrameAutoLoad || step != kStepLoading)
                {
                    zge::formatString(script, kFrameMenuInitSlotFormat, *g_cardPort + 1);
                    runScript(frame, script);
                }
                if (*g_uiFrameKind == kUiFrameAutoLoad)
                    runScript(frame, kFrameTitleGone);
            }
            last = kProgressShown;
            break;
        }
        case kStepPickParkToLoad:
        case kStepPickParkToSave:
            showParkSlots(frame);
            shown = true;
            break;
        case kStepStartCheckReport:
            runScript(uiRoot(), kRootBackgroundVisible);
            runScript(uiRoot(), kFadeInButton1);
            zge::formatString(script, kFrameMenuInitButton1SlotFormat, *g_cardPort + 1);
            runScript(frame, script);
            secondButtonSelected = true;
            break;
        default:
            if (buttons.count == 1)
                runScript(uiRoot(), kUnlockUserButton1);
            else if (buttons.count == 2)
                runScript(uiRoot(), kUnlockUserButton2);
            last = kProgressHidden;
            break;
        }
        if (secondButtonSelected)
        {
            *value = 1;
            zge::zgui::setVariable(nodeAt(frame, kFrameMenuMenuName), kSelectedIndex, value);
            last = kProgressHidden;
        }
        if (!shown)
            runScript(target, last);

        // Until the frame's menu is up (its State) the step has no message.
        *value = 0;
        zge::zgui::copyVariable(nodeAt(uiRoot(), kFrameMenuPath), kState, value);
        if (*value == 0)
        {
            *g_uiFrameStep = static_cast<uint8_t>(step);
            *g_uiFrameStepResults = 0;
            *g_uiFrameStepFrames = 0;
            return;
        }

        // The step's message, from the string table, with what it names.
        if (step == kStepStartCheckReport)
        {
            const uint32_t report = *g_cardCheckResult == kCardFull      ? kBootNoSpace
                                    : *g_cardCheckResult == kCardMissing ? kBootNoCard
                                                                         : kBootUnformatted;
            zge::formatString(key, kBootMessageFormat, report);
            const char *const text = zge::stringTableText(zge::nameHash(key));
            zge::formatString(message, text, *g_cardPort + 1);
        }
        else
        {
            zge::formatString(key, kSaveMessageFormat, step);
            if (step == kStepLoading || step == kStepSaving || step == kStepNoData)
            {
                const char *const text = zge::stringTableText(zge::nameHash(key));
                zge::formatString(message, text, (*g_cardDataNames)[*g_uiFrameKind], *g_cardPort + 1);
            }
            else if (step == kStepNoSpace)
            {
                const char *const text = zge::stringTableText(zge::nameHash(key));
                zge::formatString(message, text, *g_cardPort + 1, (*g_cardDataKilobytes)[*g_uiFrameKind],
                                  (*g_cardDataNames)[*g_uiFrameKind]);
            }
            else if (step == kStepCardChanged || step == kStepCardRemoved)
            {
                uint32_t failed = kOperationWord;
                switch (*g_cardOperation)
                {
                case kCardFormat:
                    failed = kFormatWord;
                    break;
                case kCardLoad:
                    failed = kLoadWord;
                    break;
                case kCardCreate:
                case kCardSave:
                    failed = kSaveWord;
                    break;
                default:
                    break;
                }
                const char *const text = zge::stringTableText(zge::nameHash(key));
                zge::formatString(message, text, failed, *g_cardPort + 1);
            }
            else
            {
                const char *const text = zge::stringTableText(zge::nameHash(key));
                zge::formatString(message, text, *g_cardPort + 1);
            }
        }
        zge::zgui::setVariable(nodeAt(frame, kFrameMenuMessageName), kText, message);

        // The step's buttons as the menu's items.
        message[0] = 0;
        for (uint32_t button = 0; button < buttons.count; ++button)
        {
            zge::formatString(key, kSaveButtonFormat, buttons.first + button);
            const int32_t length = zge::stringLength(message);
            zge::formatString(ee::Ptr<char>(message.address() + length), kTwoStringsFormat,
                              button == 0 ? kNoText : kLineBreak, key);
        }
        if (buttons.count == 0)
            runScript(uiRoot(), kRootLockUser);
        zge::zgui::setVariable(nodeAt(frame, kFrameMenuMenuName), kItems, message);
        *value = buttons.count;
        zge::zgui::setVariable(nodeAt(frame, kFrameMenuMenuName), kVisibleCount, value);
        runScript(frame, kMessageShown);
        if (*value < 1)
        {
            runScript(frame, kMenuHidden);
            runScript(uiRoot(), kRootLockUser);
            runScript(frame, kKillFocus);
        }
        else
        {
            runScript(frame, kMenuShown);
            runScript(frame, kMenuFocus);
        }
        if (step == kStepStartCheckReport)
            runScript(uiRoot(), kFadeInButton1);
        *g_uiFrameStepFrames = 0;
        *g_uiFrameStepResults = 0;
        *g_uiFrameStep = static_cast<uint8_t>(step);
    }
    READABLE_FUNCTION(setUiFrameStep, Verified);

    uint32_t uiFrameUpdate()
    {
        ee::Local<char[0x40]> title;
        ee::Local<uint32_t[kStepCount]> stepFrames;
        ee::Local<ee::Float> barWidth;

        fillBytes(stepFrames, 0, sizeof(uint32_t[kStepCount]));
        stepFrames[kStepFormatting] = kFormatStepFrames;
        stepFrames[kStepLoading] = kCardStepFrames;
        stepFrames[kStepCheckingToSave] = kCardStepFrames;
        stepFrames[kStepSaving] = kCardStepFrames;
        stepFrames[kStepCreating] = kCardStepFrames;
        stepFrames[kStepCheckingToLoad] = kCardStepFrames;

        zge::formatString(title, kSaveTitleFormat, *g_uiFrameKind);
        zge::zgui::setVariable(nodeAt(uiRoot(), kTopHeaderTitlePath), kText, title);
        runScript(uiRoot(), *g_uiFrameKind == kUiFrameStartCheck ? kTitleHidden : kTitleShown);
        // The user stays locked out while the card is worked on.
        if (isAnyOf(*g_uiFrameStep,
                    {kStepCheckingToSave, kStepFormatting, kStepSaving, kStepCheckingToLoad, kStepLoading}))
            runScript(uiRoot(), kRootLockUser);
        const uint32_t step = *g_uiFrameStep;

        g_cardRequest->data = g_uiFrameRequest->data;
        g_cardCall->done = ee::FunctionPtr<void(uint32_t, int32_t)>(sym::uiFrameCardDone);
        g_cardCall->argument = sym::g_cardRequest;
        switch (*g_uiFrameKind)
        {
        case kUiFrameSaveReplay:
        case kUiFrameLoadReplay:
            g_cardRequest->replaySlots = ee::Ptr<ReplaySlot>(sym::g_replaySlots);
            break;
        case kUiFrameSavePark:
        case kUiFrameLoadPark:
            g_cardRequest->parkSlots = ee::Ptr<ParkSlot>(sym::g_parkSlots);
            break;
        default:
            break;
        }

        // Only the card steps have work to drive.
        if (stepFrames[step] == 0)
            return 0;
        ++*g_uiFrameStepFrames;
        if (*g_uiFrameStepFrames == kPrepareFrame)
        {
            switch (step)
            {
            case kStepCheckingToSave:
            case kStepCheckingToLoad:
                selectCardPort(*g_cardPort);
                [[fallthrough]];
            case kStepFormatting:
            case kStepSaving:
            case kStepCreating:
            case kStepLoading:
                *g_cardOperationResult = kCardWorking;
                break;
            default:
                break;
            }
            return 0;
        }
        if (*g_uiFrameStepFrames == kStartFrame)
        {
            const ee::Ptr<CardCall> call(sym::g_cardCall);
            switch (step)
            {
            case kStepCheckingToSave:
            case kStepCheckingToLoad:
                startCardCheck(call);
                *g_cardOperation = kCardCheck;
                break;
            case kStepFormatting:
                startCardFormat(call);
                *g_cardOperation = kCardFormat;
                break;
            case kStepSaving:
                startCardSave(call);
                *g_cardOperation = kCardSave;
                break;
            case kStepCreating:
                startCardCreate(call);
                *g_cardOperation = kCardCreate;
                break;
            case kStepLoading:
                startCardLoad(call);
                *g_cardOperation = kCardLoad;
                break;
            default:
                break;
            }
            return 0;
        }
        if (*g_uiFrameStepFrames < stepFrames[step])
        {
            // The progress bar fills over the step's frames. The auto load
            // does both cards in one bar: a half for each card's check, or
            // the second half for loading from the first card.
            ee::Float width = ee::toFloat(static_cast<int32_t>(*g_uiFrameStepFrames))
                            * (ee::Float(kFullBar) / unsignedToFloat(stepFrames[step]));
            if (!(ee::Float(kLeastBar) <= width))
                width = kLeastBar;
            *barWidth = width;
            if (*g_uiFrameKind == kUiFrameAutoLoad)
            {
                if (*g_uiFrameStep == kStepCheckingToLoad)
                {
                    if (*g_cardPort == 0)
                        *barWidth = *barWidth * ee::Float(kHalf);
                    else
                        *barWidth = *barWidth * ee::Float(kHalf) + ee::Float(kHalfBar);
                }
                if (*g_uiFrameStep == kStepLoading && *g_cardPort == 0)
                    *barWidth = *barWidth * ee::Float(kHalf) + ee::Float(kHalfBar);
            }
            zge::zgui::setVariableAtPath(kProgressBarWidthPath, barWidth);
            return 0;
        }
        if (*g_cardOperationResult == kCardWorking)
        {
            g_cardRequest->data = g_uiFrameRequest->data;
            return 0;
        }

        // The card layer has answered.
        ++*g_uiFrameStepResults;
        // These answers are what the step's work expects: a check finds the
        // card changed, a format leaves no file, a save leaves one.
        if ((*g_cardOperationResult == kCardChanged
             && (*g_uiFrameStep == kStepCheckingToSave || *g_uiFrameStep == kStepCheckingToLoad))
            || (*g_cardOperationResult == kCardNoFile && *g_uiFrameStep == kStepFormatting)
            || (*g_cardOperationResult == kCardHasFile && *g_uiFrameStep == kStepSaving))
            *g_cardOperationResult = kCardDone;
        if (*g_cardOperation < kCardSpaceCheck && *g_cardOperation != kCardCheck)
            recheckCard(ee::Ptr<int32_t>(sym::g_cardOperationResult));

        switch (*g_cardOperationResult)
        {
        case kCardDone:
            switch (*g_uiFrameStep)
            {
            case kStepCheckingToSave:
            case kStepCheckingToLoad:
                if (*g_cardOperation != kCardCheck)
                {
                    // The card has room for a new file.
                    if (*g_cardOperation != kCardSpaceCheck)
                        return 0;
                    if (*g_uiFrameKind != kUiFrameSaveReplay && *g_uiFrameKind != kUiFrameSavePark)
                        setUiFrameStep(uiFrameSaves() ? kStepAskSave : kStepLoading);
                    else
                        setUiFrameStep(kStepCreating);
                    return 0;
                }
                // The card is there: its file is looked up next (a replay's
                // or a park's look-up fills the slot list).
                if (*g_uiFrameKind == kUiFrameSaveReplay || *g_uiFrameKind == kUiFrameLoadReplay)
                    g_cardCall->argument = sym::g_replaySlots;
                else if (*g_uiFrameKind == kUiFrameSavePark || *g_uiFrameKind == kUiFrameLoadPark)
                    g_cardCall->argument = sym::g_parkSlots;
                *g_cardOperationResult = kCardWorking;
                startCardFileLookup(ee::Ptr<CardCall>(sym::g_cardCall));
                *g_cardOperation = kCardFileLookup;
                break;
            case kStepFormatting:
                if (*g_uiFrameKind != kUiFrameSaveReplay && *g_uiFrameKind != kUiFrameSavePark)
                    setUiFrameStep(kStepSaving);
                else
                    setUiFrameStep(kStepCreating);
                break;
            case kStepSaving:
                if (!g_uiFrameRequest->onDone)
                    setUiFrameStep(kStepSaveComplete);
                else
                    finishRequest();
                break;
            case kStepCreating:
                if (cardSaveComplete())
                    setUiFrameStep(kStepCheckingToSave);
                else
                    setUiFrameStep(uiFrameSaves() ? kStepSaveFailed : kStepLoadFailed);
                break;
            case kStepLoading:
                if (g_uiFrameRequest->onDone)
                    finishRequest();
                else
                    setUiFrameStep(kStepLoadComplete);
                break;
            default:
                break;
            }
            break;
        case kCardMissing:
            if (*g_uiFrameStep == kStepFormatting)
                setUiFrameStep(kStepFormatFailed);
            else if (*g_uiFrameKind != kUiFrameAutoLoad)
                setUiFrameStep(uiFrameSaves() ? kStepNoCardToSave : kStepNoCardToLoad);
            else if (*g_uiFrameStep == kStepLoading)
                setUiFrameStep(kStepNoCardToLoad);
            else
            {
                // The auto load goes on to the other card.
                ++*g_cardPort;
                if (*g_cardPort == 2)
                    autoLoadOver();
                else
                {
                    *g_uiFrameStepResults = 0;
                    *g_uiFrameStepFrames = 0;
                }
            }
            break;
        case kCardChanged:
            setUiFrameStep(kStepCardChanged);
            break;
        case kCardRemoved:
            setUiFrameStep(kStepCardRemoved);
            break;
        case kCardUnformatted:
            if (uiFrameSaves())
                setUiFrameStep(*g_uiFrameStep != kStepFormatting ? kStepAskFormat : kStepFormatFailed);
            else if (*g_uiFrameKind != kUiFrameAutoLoad || *g_uiFrameStep == kStepLoading)
                setUiFrameStep(kStepNoData);
            else
            {
                ++*g_cardPort;
                if (*g_cardPort == 2)
                    autoLoadOver();
                else
                    *g_uiFrameStepFrames = 0;
            }
            break;
        case kCardNoFile:
            switch (*g_uiFrameKind)
            {
            case kUiFrameAutoLoad:
                if (*g_uiFrameStep != kStepLoading)
                {
                    ++*g_cardPort;
                    if (*g_cardPort == 2)
                        autoLoadOver();
                    else
                    {
                        *g_uiFrameStepResults = 0;
                        *g_uiFrameStepFrames = 0;
                    }
                    break;
                }
                [[fallthrough]];
            case kUiFrameLoadGame:
            case kUiFrameLoadReplay:
            case kUiFrameLoadPark:
                setUiFrameStep(kStepNoData);
                break;
            default:
                // A save with no file yet: is there room for one?
                *g_cardOperationResult = kCardWorking;
                startCardSpaceCheck(ee::Ptr<CardCall>(sym::g_cardCall));
                *g_cardOperation = kCardSpaceCheck;
                break;
            }
            break;
        case kCardHasFile:
            if (cardSaveComplete())
            {
                switch (*g_uiFrameKind)
                {
                case kUiFrameSaveReplay:
                    setUiFrameStep(kStepPickReplayToSave);
                    break;
                case kUiFrameLoadGame:
                    setUiFrameStep(kStepAskLoad);
                    break;
                case kUiFrameLoadReplay:
                    setUiFrameStep(kStepPickReplayToLoad);
                    break;
                case kUiFrameSavePark:
                    setUiFrameStep(kStepPickParkToSave);
                    break;
                case kUiFrameLoadPark:
                    setUiFrameStep(kStepPickParkToLoad);
                    break;
                case kUiFrameAutoLoad:
                    setUiFrameStep(kStepLoading);
                    break;
                default:
                    setUiFrameStep(kStepAskOverwrite);
                    break;
                }
            }
            else
                setUiFrameStep(uiFrameSaves() ? kStepSaveFailed : kStepLoadFailed);
            break;
        case kCardFull:
            setUiFrameStep(kStepNoSpace);
            break;
        default:
            setUiFrameStep(uiFrameSaves() ? kStepSaveFailed : kStepLoadFailed);
            break;
        }
        return 0;
    }
    READABLE_FUNCTION(uiFrameUpdate, Verified);

    void bmxuiMemCardHandler()
    {
        ee::Local<int32_t> answer;

        const uint32_t step = *g_uiFrameStep;
        // The menu item the answer is about.
        switch (step)
        {
        case kStepChooseSaveSlot:
        case kStepChooseLoadSlot:
            zge::zgui::copyVariableAtPath(kSelectCardIndexPath, answer);
            break;
        case kStepNoCardToSave:
        case kStepFormatComplete:
        case kStepNoSpace:
        case kStepSaveComplete:
        case kStepSaveFailed:
        case kStepFormatFailed:
        case kStepNoCardToLoad:
        case kStepNoData:
        case kStepLoadComplete:
        case kStepLoadFailed:
        case kStepStartCheckReport:
        case kStepCardChanged:
        case kStepCardRemoved:
            zge::zgui::copyVariable(nodeAt(uiRoot(), kFrameMenuMenuPath), kSelectedIndex, answer);
            break;
        case kStepAskFormat:
        case kStepAskSave:
        case kStepAskOverwrite:
        case kStepAskLoad:
            // A question's last button is its "no".
            zge::zgui::copyVariableAtPath(kFrameMenuIndexPath, answer);
            if (*g_uiMessageType == kMessageAccept && *answer == (*g_stepButtons)[step].count - 1)
                *g_uiMessageType = kMessageCancel;
            break;
        case kStepPickReplayToSave:
        case kStepPickReplayToLoad:
            replaySlotMenu();
            return;
        case kStepEnterName:
            if (*g_uiFrameKind == kUiFrameSaveReplay)
            {
                zge::zgui::copyVariableAtPath(kEnterNameStatePath, answer);
                if (*answer == 0)
                    setUiFrameStep(kStepPickReplayToSave);
                else
                {
                    zge::zgui::copyVariableAtPath(kEnterNameTextPath, kReplayNameEntered);
                    g_cardRequest->slot = *g_replaySlotPicked;
                    g_cardRequest->name = kReplayNameEntered;
                    setUiFrameStep(kStepSaving);
                    *answer = 0;
                    zge::zgui::setVariableAtPath(kEnterNameStatePath, answer);
                }
            }
            else if (*g_uiFrameKind == kUiFrameSavePark)
            {
                zge::zgui::copyVariableAtPath(kEnterParkNameStatePath, answer);
                if (*answer != 0)
                {
                    // The park's name is in; its author's follows.
                    zge::zgui::copyVariableAtPath(kEnterParkAuthorStatePath, answer);
                    if (*answer != 1)
                    {
                        zge::zgui::copyVariableAtPath(kEnterParkNameTextPath, kParkNameEntered);
                        zge::zgui::copyVariableAtPath(kEnterParkAuthorTextPath, kParkAuthorEntered);
                        g_cardRequest->slot = *g_parkSlotPicked;
                        g_cardRequest->author = kParkAuthorEntered;
                        g_cardRequest->name = kParkNameEntered;
                        setUiFrameStep(kStepSaving);
                        *answer = 0;
                        zge::zgui::setVariableAtPath(kEnterParkNameStatePath, answer);
                        zge::zgui::setVariableAtPath(kEnterAuthorNameStatePath, answer);
                    }
                }
                else
                    setUiFrameStep(kStepPickParkToSave);
            }
            break;
        case kStepPickParkToLoad:
        case kStepPickParkToSave:
            parkSlotMenu();
            return;
        default:
            *answer = -1;
            break;
        }

        if (*g_uiMessageType == kMessageCancel)
        {
            switch (*g_uiFrameStep)
            {
            case kStepChooseSaveSlot:
            case kStepChooseLoadSlot:
                closeRequest();
                break;
            case kStepPickReplayToSave:
            case kStepCheckingToSave:
            case kStepFormatting:
            case kStepSaving:
            case kStepCheckingToLoad:
            case kStepLoading:
            case kStepPickParkToSave:
            case kStepStartCheckReport:
                break;
            default:
                // Backing out of a question goes back to the slot choice.
                if ((*g_stepButtons)[*g_uiFrameStep].count >= 2)
                    setUiFrameStep(kStepChooseSaveSlot);
                break;
            }
            return;
        }
        if (*g_uiMessageType != kMessageAccept)
            return;

        switch (*g_uiFrameStep)
        {
        case kStepChooseSaveSlot:
        case kStepChooseLoadSlot:
            *g_cardPort = *answer;
            setUiFrameStep(uiFrameSaves() ? kStepCheckingToSave : kStepCheckingToLoad);
            break;
        case kStepNoCardToSave:
        case kStepNoSpace:
        case kStepSaveFailed:
        case kStepFormatFailed:
            setUiFrameStep(kStepChooseSaveSlot);
            break;
        case kStepAskFormat:
            setUiFrameStep(kStepFormatting);
            break;
        case kStepFormatComplete:
            setUiFrameStep(kStepAskSave);
            break;
        case kStepAskSave:
        case kStepAskOverwrite:
            setUiFrameStep(kStepSaving);
            break;
        case kStepSaveComplete:
            if (zge::stringCompareNoCase(ee::Ptr<char>(sym::g_replaySlots + *g_replaySlotPicked * sizeof(ReplaySlot)
                                                       + offsetof(ReplaySlot, name)),
                                         kSecretReplayName) == 0)
            {
                for (uint32_t trick = 0; trick < kTrickNames; ++trick)
                    zge::stringCopy((*g_trickNames)[trick], kSecretTrickName);
            }
            closeRequest();
            break;
        case kStepNoCardToLoad:
        case kStepNoData:
        case kStepLoadFailed:
        case kStepCardChanged:
        case kStepCardRemoved:
            if (*g_uiFrameKind != kUiFrameAutoLoad)
            {
                setUiFrameStep(kStepChooseLoadSlot);
                return;
            }
            runScript(memCardFrame(), kFrameDeInit);
            closeRequest();
            break;
        case kStepAskLoad:
            setUiFrameStep(kStepLoading);
            break;
        case kStepLoadComplete:
            closeRequest();
            break;
        case kStepStartCheckReport:
            if (*answer == 1)
            {
                // Not continuing without a card: the cards are looked at again.
                *g_cardCheckResult = checkMemoryCards(ee::Ptr<uint32_t>(sym::g_cardPort));
                if (*g_cardCheckResult != 0)
                {
                    setUiFrameStep(kStepStartCheckReport);
                    return;
                }
                runScript(memCardFrame(), kFrameMenuDeInit);
                runScript(memCardFrame(), kFrameTitleDeInit);
                runScript(uiRoot(), kTopHeaderHidden);
                runScript(uiRoot(), kBottomHeaderHidden);
                runScript(uiRoot(), kRootLockUser);
            }
            else
            {
                runScript(memCardFrame(), kFrameMenuDeInit);
                runScript(memCardFrame(), kFrameTitleDeInit);
                runScript(uiRoot(), kTopHeaderHidden);
                runScript(uiRoot(), kBottomHeaderHidden);
            }
            closeRequest();
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(bmxuiMemCardHandler, Verified);
}
