#pragma once

// The park editor's interface (park_ui.cpp): its buttons, pad, tool panel and
// help pages.

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/route.h"
#include "zge/container/list.h"
#include "zge/input/pads.h"
#include "zge/math/types.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/script.h"
#include <cstddef>
#include "zge/editor/edit_records.h"
#include "ee/ee.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace zge::editor
{
    // (park_ui.cpp)
    void attachEditUiScript(uint32_t /*ui*/, uint32_t fileName);
    void clearCameraButtons(ee::Ptr<EditToolSet> toolSet);
    void clearUiButton(uint32_t ui, uint32_t button);
    int32_t dismissEditHelpPage(uint32_t ui, uint32_t index);
    int32_t fireUiButton(uint32_t address, uint32_t button, uint32_t state);
    int32_t hideEditHelpPage(uint32_t ui, uint32_t index);
    uint32_t newEditUi();
    int32_t playDestroyParkEditorScript();
    int32_t playEditPauseScript(uint32_t ui);
    void playEditScriptFor(uint32_t ui, uint32_t script);
    void playNextObjectScript();
    void playNextToolScript();
    void playPreviousObjectScript();
    void playPreviousToolScript();
    int32_t playTapeInitScript(uint32_t ui);
    int32_t playTapeShutdownScript(uint32_t ui);
    int32_t playTestPauseScript(uint32_t ui);
    void popUiButtonPage(uint32_t address);
    int32_t pushUiButtonPage(uint32_t address);
    void setEditUiPadLostHandler(uint32_t ui, uint32_t handler);
    int32_t setInfoButtonText(uint32_t /*ui*/, uint32_t button, uint32_t text);
    int32_t setInfoHelpText(uint32_t /*ui*/, uint32_t text);
    int32_t setObjectSelectionText(uint32_t /*ui*/, ee::Ptr<const char> text);
    int32_t setToolRoll(uint32_t /*ui*/, ee::Address icons);
    void setUiButton(uint32_t ui, uint32_t button, uint32_t states, uint32_t handler, ee::Address data, uint32_t repeatPeriod, ee::Float deadzone);
    void setUiButtonRepeat(uint32_t ui, uint32_t button, uint32_t repeatPeriod);
    int32_t showEditHelpPage(uint32_t ui, uint32_t index);
    void showEditUi(uint32_t ui);
    void updateEditUi(uint32_t address);

    // One input's row in the button page.
    struct UiButtonRow
    {
        uint32_t state;       // +0x00
        ee::Float value;      // +0x04
        uint32_t repeatPeriod; // +0x08 held, the handler runs every this many frames
        ee::Float deadzone;   // +0x0C
        uint32_t data;        // +0x10 the handler's own
        uint32_t ui;          // +0x14 set just before the handler runs
    };
    static_assert(sizeof(UiButtonRow) == 0x18);

    constexpr uint32_t kButtonCount = game::readable::perGame(20u, 20u, 28u), kPadButtonCount = 16; // the rest are the sticks' axes (BMX XXX: and their directions)

    // A set of handlers for the inputs; the tools push one and pop it.
    struct UiButtonPage
    {
        uint32_t links[2];              // +0x000 in the interface's list of pages
        uint32_t handler[kButtonCount]; // +0x008
        uint32_t states[kButtonCount];  // +0x058 the states it runs in, bits of kPressed..kUp
        UiButtonRow rows[kButtonCount]; // +0x0A8
        uint32_t disabled[kButtonCount]; // +0x288
    };
    // (offsets in the comments are Aggressive Inline's, for 20 inputs)
    static_assert(sizeof(UiButtonPage) == game::readable::perGame(728u, 728u, 0x3F8u) &&
                  offsetof(UiButtonPage, states) == game::readable::perGame(0x58u, 0x58u, 0x78u) &&
                  offsetof(UiButtonPage, rows) == game::readable::perGame(0xA8u, 0xA8u, 0xE8u) &&
                  offsetof(UiButtonPage, disabled) == game::readable::perGame(0x288u, 0x288u, 0x388u));

    // The pages under the one in use: a zge list whose nodes are the pages
    // themselves (UiButtonPage::links), the latest pushed last.
    struct PageList
    {
        uint32_t first;  // +0x00
        uint32_t last;   // +0x04
        int32_t count;   // +0x08
    };
    static_assert(sizeof(PageList) == sizeof(zge::List));

    // The one page the game has: the camera's help.
    struct EditHelpPage
    {
        char text[0x100];     // +0x000
        uint32_t dismissed;   // +0x100 the player closed it for good
        uint32_t shown;       // +0x104
    };
    static_assert(sizeof(EditHelpPage) == 0x108);

    struct EditUi
    {
        uint32_t made;        // +0x000 1
        uint32_t pad;         // +0x004 zge::Pad
        uint32_t locked;      // +0x008 no input read, no handler run (the scripts' editUILockController)
        uint32_t mask[kButtonCount]; // +0x00C the PadButton bit, or the PadLevel of an axis
        uint32_t state[kButtonCount]; // +0x05C 1 pressed, 2 held, 4 released, 8 up
        uint32_t isAxis[kButtonCount]; // +0x0AC
        uint32_t frames[kButtonCount]; // +0x0FC since the state last changed
        uint32_t buttonPage;  // +0x14C
        uint8_t _150[4];
        PageList pages;       // +0x154 the pages under this one
        uint32_t onPadLost;   // +0x160 a function, or 0
        uint32_t padLost;     // +0x164 the pad went: no input read until it is back
        uint32_t _168;
    };
    // (offsets in the comments are Aggressive Inline's, for 20 inputs)
    static_assert(sizeof(EditUi) == game::readable::perGame(364u, 364u, 0x1ECu) &&
                  offsetof(EditUi, state) == game::readable::perGame(0x5Cu, 0x5Cu, 0x7Cu) &&
                  offsetof(EditUi, isAxis) == game::readable::perGame(0xACu, 0xACu, 0xECu) &&
                  offsetof(EditUi, frames) == game::readable::perGame(0xFCu, 0xFCu, 0x15Cu) &&
                  offsetof(EditUi, buttonPage) == game::readable::perGame(0x14Cu, 0x14Cu, 0x1CCu) &&
                  offsetof(EditUi, padLost) == game::readable::perGame(0x164u, 0x164u, 0x1E4u));

    // (park_ui.cpp, for the games' own parts)
    void clearUiButtonPage(uint32_t address);
    void editUiShowHook();
    int32_t freeUiButtonPage(uint32_t page);
    void hideMainHelp(uint32_t ui);
    int32_t initEditHelpPages();
    uint32_t newUiButtonPage();
    int32_t playEditScript(uint32_t ui, uint32_t script);
    uint32_t registerEditUIFunctions();
    int32_t showEditMemory(uint32_t , uint32_t count);
    void showMainHelp(uint32_t ui, uint32_t text);
    void unlockEditUi(uint32_t ui);
    void watchEditUiPad(uint32_t address);
}
