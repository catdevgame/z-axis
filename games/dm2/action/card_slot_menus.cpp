// The memory card frame's slot lists (card_frame.h): the replay slots the
// user picks from to save or load a replay, and the park slots. DM2's own
// code (action.c).

#include "card_frame.h"
#include "ui_frame.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/action/card_slot_menus.cpp.h"

namespace game::dm2
{
    namespace
    {
        using zge::zgui::Node;

        // The replay slot frame, the question it asks before a used slot is
        // overwritten, and their variables by path.
        constexpr uint32_t kReplaySlotFramePath = sym::replaySlotFramePath;     // "CommonUI.FrameMemCard.FrameReplaySlot"
        constexpr uint32_t kReplaySlotConfirmPath = sym::replaySlotConfirmPath; // "...FrameReplaySlot.FrameConfirm"
        constexpr ee::Ptr<const char> kReplaySlotConfirmingPath{sym::replaySlotConfirmingPath}; // "...FrameReplaySlot.Confirming"
        constexpr ee::Ptr<const char> kReplaySlotIndexPath{sym::replaySlotIndexPath};     // "...FrameReplaySlot.Menu.SelectedIndex"
        // "...FrameReplaySlot.FrameConfirm.Menu.SelectedIndex"
        constexpr ee::Ptr<const char> kReplaySlotConfirmIndexPath{sym::replaySlotConfirmIndexPath};
        constexpr ee::Ptr<char> kInit{sym::initScript};     // "PLAY Init\n"
        constexpr ee::Ptr<char> kDeInit{sym::deInitScript}; // "PLAY DeInit\n"

        // The park slot frame's counterparts, and its three columns (below
        // the memory card frame), whose items hang from each one's RootNode.
        constexpr uint32_t kMemCardFramePath = sym::memCardFramePath;           // "CommonUI.FrameMemCard"
        constexpr uint32_t kParkSlotFramePath = sym::parkSlotFramePath;         // "CommonUI.FrameMemCard.FrameParkSlot"
        constexpr uint32_t kParkSlotConfirmPath = sym::parkSlotConfirmPath;     // "...FrameParkSlot.FrameConfirm"
        constexpr ee::Ptr<const char> kParkSlotConfirmingPath{sym::parkSlotConfirmingPath}; // "...FrameParkSlot.Confirming"
        constexpr ee::Ptr<const char> kParkSlotIndexPath{sym::parkSlotIndexPath};         // "...FrameParkSlot.Info.name.SelectedIndex"
        // "...FrameParkSlot.FrameConfirm.Menu.SelectedIndex"
        constexpr ee::Ptr<const char> kParkSlotConfirmIndexPath{sym::parkSlotConfirmIndexPath};
        constexpr uint32_t kParkSlotNameListName = sym::parkSlotNameListName;     // "FrameParkSlot.Info.name"
        constexpr uint32_t kParkSlotThemeListName = sym::parkSlotThemeListName;   // "FrameParkSlot.Info.theme"
        constexpr uint32_t kParkSlotAuthorListName = sym::parkSlotAuthorListName; // "FrameParkSlot.Info.author"
        constexpr uint32_t kRootNode = sym::rootNodeName;                         // "RootNode"
        constexpr ee::Ptr<char> kParkNameEntered{sym::g_parkNameEntered};

        // A column's items: zgui's tree items (ZGE2's, a list of them below
        // each one).
        using zge::zgui::ListedTreeItem;
        constexpr ee::Ptr<const char> kTopItemText{sym::g_textMaster};    // "Master"
        constexpr uint32_t kParentNode = sym::parentNodeName;             // "ParentNode"
        constexpr ee::Ptr<const char> kEmptySlotText{sym::emptySlotText}; // "* EMPTY *"
        constexpr ee::Ptr<const char> kNoValueText{sym::noValueText};     // "-"
        constexpr ee::Ptr<char> kFrameMenuDeInit{sym::frameMenuDeInitScript}; // "PLAY FrameMenu.DeInit"
        constexpr ee::Ptr<char> kParkSlotInit{sym::parkSlotInitScript};       // "PLAY FrameParkSlot.Init"

        // The name of the replay being saved or loaded.
        constexpr ee::Ptr<char> kReplayNameEntered{sym::g_replayNameEntered};

        // A list's answers: nothing picked (the user backed out), and the
        // question's first item, which agrees.
        constexpr int32_t kNonePicked = -1;
        constexpr int32_t kConfirmYes = 0;

        ee::Ptr<Node> nodeAt(uint32_t path) { return zge::zgui::findNode(*g_cardFrameUiRoot, path, 0); }
        void runScript(ee::Ptr<Node> on, ee::Ptr<char> text) { zge::zgui::runScriptLines(on, text); }

        // A column's items replaced by a new top item, which the column
        // takes as both its ParentNode and its RootNode. Returns it.
        // (A tree item is made below its parent, none for a tree's top item.)
        ee::Ptr<ListedTreeItem> newColumnTop(ee::Ptr<Node> column, ee::Local<ee::Ptr<ListedTreeItem>> &top)
        {
            zge::zgui::copyVariable(column, kRootNode, top);
            if (*top)
                zge::zgui::freeTreeItem(*top);
            *top = zge::zgui::newTreeItem(ee::Ptr<ListedTreeItem>(), kTopItemText);
            zge::zgui::setVariable(column, kParentNode, top);
            zge::zgui::setVariable(column, kRootNode, top);
            return *top;
        }
    }

    // showParkSlots (frame): the park slot list filled: its three columns
    // get one item a slot, the park's name, theme and author, or
    // "* EMPTY *", "-", "-" for a slot not used. Then the frame's menu is put
    // away (unless the last message was a cancel) and the list is shown.
    void showParkSlots(ee::Ptr<Node> frame)
    {
        const ee::Ptr<Node> names = zge::zgui::findNode(frame, kParkSlotNameListName, 0);
        const ee::Ptr<Node> themes = zge::zgui::findNode(frame, kParkSlotThemeListName, 0);
        const ee::Ptr<Node> authors = zge::zgui::findNode(frame, kParkSlotAuthorListName, 0);
        ee::Local<ee::Ptr<ListedTreeItem>> nameItems;
        ee::Local<ee::Ptr<ListedTreeItem>> themeItems;
        ee::Local<ee::Ptr<ListedTreeItem>> authorItems;
        const ee::Ptr<ListedTreeItem> nameTop = newColumnTop(names, nameItems);
        const ee::Ptr<ListedTreeItem> themeTop = newColumnTop(themes, themeItems);
        const ee::Ptr<ListedTreeItem> authorTop = newColumnTop(authors, authorItems);
        for (uint32_t slot = 0; slot < kParkSlots; ++slot)
        {
            const ee::Ptr<ParkSlot> park = ee::Ptr<ParkSlot>(sym::g_parkSlots) + static_cast<ptrdiff_t>(slot);
            if (park->flags & kSlotUsed)
            {
                zge::zgui::newTreeItem(nameTop, park->name);
                zge::zgui::newTreeItem(themeTop, park->theme);
                zge::zgui::newTreeItem(authorTop, park->author);
            }
            else
            {
                zge::zgui::newTreeItem(nameTop, kEmptySlotText);
                zge::zgui::newTreeItem(themeTop, kNoValueText);
                zge::zgui::newTreeItem(authorTop, kNoValueText);
            }
        }
        if (*g_uiMessageType != kMessageCancel)
            runScript(frame, kFrameMenuDeInit);
        runScript(frame, kParkSlotInit);
    }
    READABLE_FUNCTION(showParkSlots, Verified);

    void replaySlotMenu()
    {
        ee::Local<int32_t> confirming;
        ee::Local<int32_t> picked;

        // While the overwrite question is coming up the list has no answer.
        zge::zgui::copyVariableAtPath(kReplaySlotConfirmingPath, confirming);
        if (*confirming != 0)
            return;
        zge::zgui::copyVariableAtPath(kReplaySlotIndexPath, picked);
        *g_replaySlotPicked = static_cast<uint8_t>(*picked);
        if (*picked == kNonePicked)
        {
            runScript(nodeAt(kReplaySlotFramePath), kDeInit);
            setUiFrameStep(kStepChooseSaveSlot);
        }
        else if (!((*g_replaySlots)[*picked].flags & kSlotUsed))
        {
            // An empty slot: a save goes on to its name; there is nothing to load.
            if (*g_uiFrameStep == kStepPickReplayToSave)
            {
                runScript(nodeAt(kReplaySlotFramePath), kDeInit);
                setUiFrameStep(kStepEnterName);
            }
        }
        else if (*g_uiFrameStep == kStepPickReplayToSave)
        {
            // A used slot: the save asks first.
            zge::zgui::copyVariableAtPath(kReplaySlotConfirmIndexPath, picked);
            if (*picked == kNonePicked)
                runScript(nodeAt(kReplaySlotConfirmPath), kInit);
            else
            {
                if (*picked == kConfirmYes)
                {
                    runScript(nodeAt(kReplaySlotFramePath), kDeInit);
                    setUiFrameStep(kStepEnterName);
                }
                *picked = kNonePicked;
                zge::zgui::setVariableAtPath(kReplaySlotConfirmIndexPath, picked);
            }
        }
        else
        {
            runScript(nodeAt(kReplaySlotFramePath), kDeInit);
            g_cardRequest->slot = static_cast<uint32_t>(*picked);
            zge::stringCopy(kReplayNameEntered,
                            ee::Ptr<char>(sym::g_replaySlots + static_cast<uint32_t>(*picked) * sizeof(ReplaySlot)
                                          + offsetof(ReplaySlot, name)));
            g_cardRequest->name = kReplayNameEntered;
            setUiFrameStep(kStepLoading);
        }
    }
    READABLE_FUNCTION(replaySlotMenu, Verified);

    void parkSlotMenu()
    {
        ee::Local<int32_t> confirming;
        ee::Local<int32_t> picked;
        ee::Local<ee::Ptr<ListedTreeItem>> nameItems;
        ee::Local<ee::Ptr<ListedTreeItem>> noItems;
        ee::Local<ee::Ptr<ListedTreeItem>> themeItems;
        ee::Local<ee::Ptr<ListedTreeItem>> authorItems;

        bool listLeft = false;
        zge::zgui::copyVariableAtPath(kParkSlotConfirmingPath, confirming);
        if (*confirming == 0)
        {
            zge::zgui::copyVariableAtPath(kParkSlotIndexPath, picked);
            *g_parkSlotPicked = static_cast<uint8_t>(*picked);
            if (*picked == kNonePicked)
            {
                listLeft = true;
                runScript(nodeAt(kParkSlotFramePath), kDeInit);
                setUiFrameStep(kStepChooseSaveSlot);
            }
            else if (!((*g_parkSlots)[*picked].flags & kSlotUsed))
            {
                if (*g_uiFrameStep == kStepPickParkToSave)
                {
                    listLeft = true;
                    runScript(nodeAt(kParkSlotFramePath), kDeInit);
                    setUiFrameStep(kStepEnterName);
                }
            }
            else if (*g_uiFrameStep == kStepPickParkToSave)
            {
                zge::zgui::copyVariableAtPath(kParkSlotConfirmIndexPath, picked);
                if (*picked == kNonePicked)
                    runScript(nodeAt(kParkSlotConfirmPath), kInit);
                else
                {
                    listLeft = *picked == kConfirmYes;
                    if (listLeft)
                    {
                        runScript(nodeAt(kParkSlotFramePath), kDeInit);
                        setUiFrameStep(kStepEnterName);
                    }
                    *picked = kNonePicked;
                    zge::zgui::setVariableAtPath(kParkSlotConfirmIndexPath, picked);
                }
            }
            else
            {
                listLeft = true;
                runScript(nodeAt(kParkSlotFramePath), kDeInit);
                g_cardRequest->slot = static_cast<uint32_t>(*picked);
                zge::stringCopy(kParkNameEntered,
                                ee::Ptr<char>(sym::g_parkSlots + static_cast<uint32_t>(*picked) * sizeof(ParkSlot)
                                              + offsetof(ParkSlot, name)));
                g_cardRequest->name = kParkNameEntered;
                setUiFrameStep(kStepLoading);
            }
        }
        if (listLeft)
        {
            // The three columns' items (showParkSlots made them) are freed
            // and the columns left with none.
            *noItems = ee::Ptr<ListedTreeItem>();
            const ee::Ptr<Node> frame = nodeAt(kMemCardFramePath);
            const ee::Ptr<Node> names = zge::zgui::findNode(frame, kParkSlotNameListName, 0);
            const ee::Ptr<Node> themes = zge::zgui::findNode(frame, kParkSlotThemeListName, 0);
            const ee::Ptr<Node> authors = zge::zgui::findNode(frame, kParkSlotAuthorListName, 0);
            zge::zgui::copyVariable(names, kRootNode, nameItems);
            if (*nameItems)
                zge::zgui::freeTreeItem(*nameItems);
            zge::zgui::setVariable(names, kRootNode, noItems);
            zge::zgui::copyVariable(themes, kRootNode, themeItems);
            if (*themeItems)
                zge::zgui::freeTreeItem(*themeItems);
            zge::zgui::setVariable(themes, kRootNode, noItems);
            zge::zgui::copyVariable(authors, kRootNode, authorItems);
            if (*authorItems)
                zge::zgui::freeTreeItem(*authorItems);
            zge::zgui::setVariable(authors, kRootNode, noItems);
        }
    }
    READABLE_FUNCTION(parkSlotMenu, Verified);
}
