// The park editor's interface: the object behind ParkEditor.zsc, the flash its
// cursors pulse with, the pad's sticks and buttons read each frame into the
// button page's rows (level/park_cells.cpp and the tools bind handlers to them),
// the handlers run from those rows, what the tool panel shows and the help line.

#include "zge/editor/park_ui.h"
#include "zge/editor/game.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "zge/editor/edit_records.h"
#include "aggressive/zgui.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/container/list.h"
#include "zge/input/pads.h"
#include "zge/math/types.h"
#include "zge/memory/regions.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/script.h"
#include "sce/libgcc.h"
#include "zge/editor/park_classes.h"
#include "zge/editor/park_anchors.h"
#include "zge/editor/edit_core.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/editor/park_ui.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kRunScript = sym::runScript;          // (widget, text, 0)
        constexpr uint32_t kUnlockEditUi = sym::unlockEditUi;
        constexpr uint32_t kLockController = sym::editUILockController;
        constexpr uint32_t kUnlockController = sym::editUIUnlockController;

        constexpr uint32_t kTextEditorVisible = sym::g_textRootParkEditorVisible;  // "Root.ParkEditor.Visible"
        constexpr uint32_t kTextTapeFormat = sym::g_textRootParkEditorMainUITapeTape2;     // "Root.ParkEditor.MainUI.Tape.Tape.TextFormat"
        constexpr uint32_t kTextOnEnterEdit = sym::g_textPlayRootParkEditorOnEnterEdit;    // "PLAY Root.ParkEditor.OnEnterEdit"
        constexpr uint32_t kTextOnDestroy = sym::g_textPlayRootParkEditorOnDestroyParkEdi;      // "PLAY Root.ParkEditor.OnDestroyParkEditor"
        constexpr ee::Ptr<const char> kTextLockController{sym::g_textEditUILockController};   // "editUILockController"
        constexpr uint32_t kTextUnlockController = sym::g_textEditUIUnlockController; // "editUIUnlockController"
        constexpr uint32_t kTextMemory = sym::g_textParkEditorMainUIMemoryText;         // "ParkEditor.MainUI.Memory.Text"
        constexpr uint32_t kTextToolObject = sym::g_textParkEditorMainUIToolObject;     // "ParkEditor.MainUI.Tool.Object.Selection.Text"
        constexpr uint32_t kTextPreviousObject = sym::g_textPlayRootParkEditorMainUITool; // "PLAY Root.ParkEditor.MainUI.Tool.Object.OnPrevObject"
        constexpr uint32_t kTextNextObject = sym::g_textPlayRootParkEditorMainUITool2;     // "PLAY Root.ParkEditor.MainUI.Tool.Object.OnNextObject"
        constexpr uint32_t kTextInfoHelp = sym::g_textParkEditorMainUIInfoHelpText;       // "ParkEditor.MainUI.Info.Help.Text"
        constexpr uint32_t kTextInfoButton = sym::g_textParkEditorMainUIInfoButton;     // "ParkEditor.MainUI.Info.Button%d.Text.NewText"
        constexpr uint32_t kTextInfoButtonChange = sym::g_textPlayParkEditorMainUIInfoButton; // "PLAY ParkEditor.MainUI.Info.Button%d.Text.OnChange"
        constexpr uint32_t kTextRollIcons = sym::g_textParkEditorMainUIToolIconRoll;      // "ParkEditor.MainUI.Tool.IconRoll.String"
        constexpr uint32_t kTextRollTotal = sym::g_textParkEditorMainUIToolIconRoll2;      // "ParkEditor.MainUI.Tool.IconRoll.TotalIcon"
        constexpr uint32_t kTextRollInc = sym::g_textParkEditorMainUIToolInc;        // "ParkEditor.MainUI.Tool.inc"
        constexpr uint32_t kTextRollDec = sym::g_textParkEditorMainUIToolDec;        // "ParkEditor.MainUI.Tool.dec"
        constexpr uint32_t kTextRollAngle = sym::g_textParkEditorMainUIToolIconRoll3;      // "ParkEditor.MainUI.Tool.IconRoll.RollAngle"
        constexpr uint32_t kTextNextTool = sym::g_textPlayRootParkEditorMainUITool3;       // "PLAY Root.ParkEditor.MainUI.Tool.OnNextTool"
        constexpr uint32_t kTextPreviousTool = sym::g_textPlayRootParkEditorMainUITool4;   // "PLAY Root.ParkEditor.MainUI.Tool.OnPrevTool"
        constexpr uint32_t kTextOnEditPause = sym::g_textPlayRootParkEditorOnEditPause;    // "PLAY Root.ParkEditor.OnEditPause"
        constexpr uint32_t kTextOnTestPause = sym::g_textPlayRootParkEditorOnTestPause;    // "PLAY Root.ParkEditor.OnTestPause"
        constexpr uint32_t kTextMainHelp = sym::g_textParkEditorMainUIMainHelpHelp;       // "ParkEditor.MainUI.MainHelp.Help.Text"
        constexpr uint32_t kTextMainHelpOnInit = sym::g_textPlayRootParkEditorMainUIMainHelp; // "Play Root.ParkEditor.MainUI.MainHelp.OnInit"
        constexpr uint32_t kTextMainHelpOnShutdown = sym::g_textPlayRootParkEditorMainUIMainHelp2; // "Play Root.ParkEditor.MainUI.MainHelp.OnShutdown"
        constexpr uint32_t kTextCameraHelp = sym::g_textLeftAnalogStickRotateCamera;     // "Left analog stick = rotate camera \n Right analog stick = zoom camera"
        constexpr uint32_t kTextTapeOnInit = sym::g_textPlayRootParkEditorMainUITape;     // "Play Root.ParkEditor.MainUI.Tape.OnInit"
        constexpr uint32_t kTextTapeOnShutdown = sym::g_textPlayRootParkEditorMainUITape2; // "Play Root.ParkEditor.MainUI.Tape.OnShutdown"
        constexpr uint32_t kTextFeetFormat = sym::g_text0fF;     // "%2.0fF"
        constexpr uint32_t kTextCountFormat = sym::textCountFormat;    // "%d"

        inline ee::Global<ee::Ptr<zgui::Node>, sym::g_uiRoot> g_uiRoot;
        inline ee::Global<uint32_t, sym::g_editHelpPages> g_editHelpPages;     // one page, EditHelpPage
        inline ee::Global<uint32_t, sym::g_editUiController> g_editUiController;  // the interface whose script plays
        inline ee::Global<uint32_t, sym::g_editScriptPlaying> g_editScriptPlaying; // cleared before each script

        constexpr uint32_t kPressed = 1, kHeld = 2, kReleased = 4, kUp = 8; // a row's state
        constexpr uint32_t kFullTurn = 0x00010000u;           // angle units
    }

    int32_t setEditUiPad(uint32_t address, ee::Address pad);
    int32_t showEditMemory(uint32_t ui, uint32_t count);

    uint32_t newUiButtonPage();

    void unlockEditUi(uint32_t ui);

    int32_t initEditHelpPages();

    // newEditUi: the interface on the zgui's pad, its button page and help pages.
    uint32_t newEditUi()
    {
        const uint32_t address = editAllocate(static_cast<uint32_t>(sizeof(EditUi)));
        const ee::Ptr<EditUi> ui(address);
        ui->onPadLost = 0;
        ui->padLost = 0;
        const ee::Ptr<zgui::Pad> pad = zgui::zguiPad(0);
        setEditUiPad(address, pad);
        ui->buttonPage = newUiButtonPage();
        zge::initList(address + offsetof(EditUi, pages));
        unlockEditUi(address);
        ui->made = 1;
        initEditHelpPages();
        return address;
    }
    READABLE_FUNCTION_WAITS(newEditUi, Verified);

    void attachEditUiScript(uint32_t /*ui*/, uint32_t fileName)
    {
        game::tail(zge::zgui::loadZguiScript, fileName, *g_uiRoot);
    }
    READABLE_FUNCTION_WAITS(attachEditUiScript, Verified);

    uint32_t registerEditUIFunctions();

    int32_t playEditScript(uint32_t ui, uint32_t script);

    void editUiShowHook();

    void showEditUi(uint32_t ui)
    {
        ee::Local<uint32_t> value;
        *value = 1;
        zgui::setNodeValueByPath(kTextEditorVisible, value);
        zgui::setNodeValueByPath(kTextTapeFormat, kTextFeetFormat);
        registerEditUIFunctions();
        playEditScript(ui, kTextOnEnterEdit);
        editUiShowHook();
    }
    READABLE_FUNCTION_WAITS(showEditUi, Verified);

    namespace
    {
        namespace tail
        {
        int32_t runScript(ee::Address widget, uint32_t text, uint32_t argument) { return ee::tailCall<int32_t>(kRunScript, widget, text, argument); }
        }
    }

    int32_t playDestroyParkEditorScript()
    {
        return tail::runScript(*g_uiRoot, kTextOnDestroy, 0);
    }
    READABLE_FUNCTION_WAITS(playDestroyParkEditorScript, Verified);

    int32_t freeUiButtonPage(uint32_t page);

    // drawEditUi: the debug line of blocks the editor holds.
    void drawEditUi(uint32_t ui)
    {
        const uint32_t count = editAllocationCount();
        game::tail(showEditMemory, ui, count);
    }
    READABLE_FUNCTION_WAITS(drawEditUi, Verified);

    // drawEditor: the park editor's part of the UI's draw (zgui's draw
    // callback): its current tool, then its debug line.
    uint32_t drawEditor(ee::Ptr<EditToolSet> toolSet)
    {
        const int32_t tool = toolSet->currentTool;
        if (tool != 0)
            drawEditTool(tool);
        drawEditUi(toolSet->ui);
        return 1;
    }
    READABLE_FUNCTION_WAITS(drawEditor, Verified);

    // watchEditUiPad: a pad that goes calls onPadLost once and stops the input
    // until it is connected again.
    void watchEditUiPad(uint32_t address)
    {
        const ee::Ptr<EditUi> ui(address);
        const ee::Ptr<zge::Pad> pad(ui->pad);
        if (ui->padLost != 0)
        {
            if ((pad->flags & zge::kPadConnected) != 0)
                ui->padLost = 0;
            return;
        }
        if ((pad->flags & zge::kPadConnected) == 0)
        {
            const ee::FunctionPtr<void(uint32_t)> handler(ui->onPadLost);
            ui->padLost = 1;
            if (handler)
                handler(address);
        }
    }
    READABLE_FUNCTION(watchEditUiPad, Verified);

    // updateEditUi: the frame's input, then each row's handler if the row is in
    // one of the states it runs in: a repeating row (held) only every repeatPeriod
    // frames, its press and release always.
    void updateEditUi(uint32_t address)
    {
        const ee::Ptr<EditUi> ui(address);
        if (ui->locked != 0)
            return;
        readEditUiButtons(address);
        for (uint32_t i = 0;;)
        {
            const uint32_t pageAddress = ui->buttonPage;
            const ee::Ptr<UiButtonPage> page(pageAddress);
            const uint32_t state = ui->state[i];
            const uint32_t rowOffset = i * static_cast<uint32_t>(sizeof(UiButtonRow));
            const auto runHandler = [&]()
            {
                page->rows[i].ui = address;
                const ee::FunctionPtr<void(uint32_t, uint32_t, uint32_t, uint32_t)> handler(page->handler[i]);
                handler(pageAddress + offsetof(UiButtonPage, rows) + rowOffset, pageAddress, rowOffset, i * 4);
            };
            bool joined = true; // through the block after a repeat-less handler
            if ((page->states[i] & state) != 0)
            {
                if (page->disabled[i] != 0)
                    joined = false;
                else if (const uint32_t repeatPeriod = page->rows[i].repeatPeriod; repeatPeriod != 0)
                {
                    joined = false;
                    bool due = true;
                    if ((state & (kPressed | kReleased)) == 0)
                        due = ui->frames[i] % repeatPeriod == 0;
                    if (due)
                        runHandler();
                }
                else
                    runHandler();
            }
            i = i + 1;
            if (!(i < kButtonCount))
                break;
        }
    }
    READABLE_FUNCTION_WAITS(updateEditUi, Verified);

    int32_t lockEditUi(uint32_t ui)
    {
        ee::Ptr<EditUi>(ui)->locked = 1;
        return 1;
    }
    READABLE_FUNCTION(lockEditUi, Verified);

    void unlockEditUi(uint32_t ui)
    {
        ee::Ptr<EditUi>(ui)->locked = 0;
    }
    READABLE_FUNCTION(unlockEditUi, Verified);

    uint32_t registerEditUIFunctions()
    {
        registerEditCoreFunctions();
        zge::zgui::registerScriptFunction(kTextLockController, kLockController);
        return game::tail(zge::zgui::registerScriptFunction, kTextUnlockController, kUnlockController);
    }
    READABLE_FUNCTION_WAITS(registerEditUIFunctions, Verified);

    void editUiShowHook()
    {
    }
    READABLE_FUNCTION(editUiShowHook, Verified);

    // showEditMemory: the count in the memory line. The format has one number: the
    // words after it are what the caller's registers held.
    int32_t showEditMemory(uint32_t /*ui*/, uint32_t count)
    {
        ee::Local<uint8_t[0x50]> out;
        zge::formatString(out, kTextCountFormat, count);
        return zgui::setNodeValueByPath(kTextMemory, out);
    }
    READABLE_FUNCTION_WAITS(showEditMemory, Verified);

    // setObjectSelectionText: the tool line's text (a piece's name, a
    // tool's).
    int32_t setObjectSelectionText(uint32_t /*ui*/, ee::Ptr<const char> text)
    {
        return game::tail(zge::zgui::setNodeValueByPath, kTextToolObject, text);
    }
    READABLE_FUNCTION_WAITS(setObjectSelectionText, Verified);

    void playPreviousObjectScript()
    {
        tail::runScript(*g_uiRoot, kTextPreviousObject, 0);
    }
    READABLE_FUNCTION_WAITS(playPreviousObjectScript, Verified);

    void playNextObjectScript()
    {
        tail::runScript(*g_uiRoot, kTextNextObject, 0);
    }
    READABLE_FUNCTION_WAITS(playNextObjectScript, Verified);

    int32_t setInfoHelpText(uint32_t /*ui*/, uint32_t text)
    {
        return game::tail(zge::zgui::setNodeValueByPath, kTextInfoHelp, text);
    }
    READABLE_FUNCTION_WAITS(setInfoHelpText, Verified);

    namespace
    {
        int32_t runScript(ee::Address widget, ee::Address text, uint32_t argument) { return ee::game<int32_t>(kRunScript, widget, text, argument); }
    }

    // setInfoButtonText: a button of the info panel takes its new text and plays its
    // OnChange. Both formats have one number, as in showEditMemory.
    int32_t setInfoButtonText(uint32_t /*ui*/, uint32_t button, uint32_t text)
    {
        ee::Local<uint8_t[0x400]> path;
        ee::Local<uint8_t[0x440]> script;
        zge::formatString(path, kTextInfoButton, button);
        zge::formatString(script, kTextInfoButtonChange, button);
        zgui::setNodeValueByPath(path, text);
        return runScript(*g_uiRoot, script, 0);
    }
    READABLE_FUNCTION_WAITS(setInfoButtonText, Verified);

    void setEditUiPadLostHandler(uint32_t ui, uint32_t handler)
    {
        ee::Ptr<EditUi>(ui)->onPadLost = handler;
    }
    READABLE_FUNCTION(setEditUiPadLostHandler, Verified);

    // setToolRoll: the tool panel's icon roll: its string of icons, how many, the
    // step between two, and the angle a full roll ends at. 0x10000 angle units
    // round: the integer division of the turn by the icons, a sixteenth of it a frame.
    int32_t setToolRoll(uint32_t /*ui*/, ee::Address icons)
    {
        ee::Local<int> iconCount;
        ee::Local<ee::Float> frameStep;
        ee::Local<ee::Float> rollAngle;
        const int32_t count = zge::stringLength(icons);
        const uint32_t step = kFullTurn / static_cast<uint32_t>(count);
        *iconCount = count;
        ee::Float asFloat;
        if (static_cast<int32_t>(step) >= 0)
            asFloat = ee::toFloat(static_cast<int32_t>(step));
        else
        {
            const ee::Float half = ee::toFloat(static_cast<int32_t>((step & 1u) | (step >> 1)));
            asFloat = half + half;
        }
        *frameStep = asFloat * ee::Float(0.0625f);
        zgui::setNodeValueByPath(kTextRollIcons, icons);
        zgui::setNodeValueByPath(kTextRollTotal, iconCount);
        zgui::setNodeValueByPath(kTextRollInc, frameStep);
        *frameStep = -*frameStep;
        zgui::setNodeValueByPath(kTextRollDec, frameStep);
        *rollAngle = ee::Float(16384.0f) - *frameStep * ee::Float(16.0f);
        return zgui::setNodeValueByPath(kTextRollAngle, rollAngle);
    }
    READABLE_FUNCTION_WAITS(setToolRoll, Verified);

    void playNextToolScript()
    {
        tail::runScript(*g_uiRoot, kTextNextTool, 0);
    }
    READABLE_FUNCTION_WAITS(playNextToolScript, Verified);

    void playPreviousToolScript()
    {
        tail::runScript(*g_uiRoot, kTextPreviousTool, 0);
    }
    READABLE_FUNCTION_WAITS(playPreviousToolScript, Verified);

    // setUiButton: a row's handler, the states it runs in, its repeat, data and
    // dead zone.
    void setUiButton(uint32_t ui, uint32_t button, uint32_t states, uint32_t handler, ee::Address data, uint32_t repeatPeriod, ee::Float deadzone)
    {
        const ee::Ptr<UiButtonPage> page(ee::Ptr<EditUi>(ui)->buttonPage);
        page->handler[button] = handler;
        page->states[button] = states;
        page->rows[button].repeatPeriod = repeatPeriod;
        page->rows[button].data = data;
        page->rows[button].deadzone = deadzone;
    }
    READABLE_FUNCTION(setUiButton, Verified);

    // fireUiButton: runs a row's handler as if the row were in `state`, whatever
    // the pad says (resetToolSetButton, when the pad goes). The registers past the
    // row are what the original's own work left there: no handler reads them.
    int32_t fireUiButton(uint32_t address, uint32_t button, uint32_t state)
    {
        const ee::Ptr<EditUi> ui(address);
        const uint32_t pageAddress = ui->buttonPage;
        const ee::Ptr<UiButtonPage> page(pageAddress);
        const uint32_t rowOffset = button * static_cast<uint32_t>(sizeof(UiButtonRow));
        int32_t result = sizeof(UiButtonRow);
        if ((page->states[button] & state) != 0)
        {
            page->rows[button].ui = address;
            const uint32_t currentAddress = ui->buttonPage;
            const ee::Ptr<UiButtonPage> current(currentAddress);
            result = current->handler[button];
            if (result == 0)
                return result;
            const ee::FunctionPtr<int32_t(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)> handler(result);
            result = handler(currentAddress + offsetof(UiButtonPage, rows) + rowOffset, rowOffset, state, pageAddress, button * 4);
        }
        return result;
    }
    READABLE_FUNCTION(fireUiButton, Verified);

    void clearUiButton(uint32_t ui, uint32_t button)
    {
        const ee::Ptr<UiButtonPage> page(ee::Ptr<EditUi>(ui)->buttonPage);
        page->handler[button] = 0;
        page->states[button] = 0;
        UiButtonRow *const row = &page->rows[button];
        row->data = 0;
        row->repeatPeriod = 1;
        row->state = kUp;
        row->deadzone = 0;
        row->value = 0;
    }
    READABLE_FUNCTION(clearUiButton, Verified);

    void setUiButtonRepeat(uint32_t ui, uint32_t button, uint32_t repeatPeriod)
    {
        const ee::Ptr<UiButtonPage> page(ee::Ptr<EditUi>(ui)->buttonPage);
        page->rows[button].repeatPeriod = repeatPeriod;
    }
    READABLE_FUNCTION(setUiButtonRepeat, Verified);

    void clearUiButtonPage(uint32_t address);

    uint32_t newUiButtonPage()
    {
        const uint32_t page = editAllocate(static_cast<uint32_t>(sizeof(UiButtonPage)));
        clearUiButtonPage(page);
        return page;
    }
    READABLE_FUNCTION_WAITS(newUiButtonPage, Verified);

    int32_t freeUiButtonPage(uint32_t page)
    {
        return game::tail(editFree, page);
    }
    READABLE_FUNCTION_WAITS(freeUiButtonPage, Verified);

    void clearUiButtonPage(uint32_t address)
    {
        const ee::Ptr<UiButtonPage> page(address);
        for (uint32_t i = 0;;)
        {
            page->handler[i] = 0;
            page->states[i] = 0;
            page->disabled[i] = 0;
            page->rows[i].data = 0;
            page->rows[i].repeatPeriod = 1;
            page->rows[i].state = kUp;
            page->rows[i].deadzone = 0;
            page->rows[i].value = 0;
            if (!(++i < kButtonCount))
                return;
        }
    }
    READABLE_FUNCTION(clearUiButtonPage, Verified);

    // pushUiButtonPage: a tool's own handlers over the page in use.
    int32_t pushUiButtonPage(uint32_t address)
    {
        const ee::Ptr<EditUi> ui(address);
        zge::listPush(address + offsetof(EditUi, pages), ui->buttonPage);
        const uint32_t page = newUiButtonPage();
        ui->buttonPage = page;
        return page;
    }
    READABLE_FUNCTION_WAITS(pushUiButtonPage, Verified);

    // popUiButtonPage: back to the list's tail, which stays in the list.
    void popUiButtonPage(uint32_t address)
    {
        const ee::Ptr<EditUi> ui(address);
        freeUiButtonPage(ui->buttonPage);
        ui->buttonPage = ui->pages.last;
    }
    READABLE_FUNCTION_WAITS(popUiButtonPage, Verified);

    void playEditScriptFor(uint32_t ui, uint32_t script)
    {
        game::tail(playEditScript, ui, script);
    }
    READABLE_FUNCTION_WAITS(playEditScriptFor, Verified);

    int32_t editUILockController()
    {
        return game::tail(lockEditUi, *g_editUiController);
    }
    READABLE_FUNCTION(editUILockController, Verified);

    namespace
    {
        // unlockEditUi, and what it leaves in the result register (returned here).
        int32_t unlockEditUiKeepingResult(uint32_t ui) { return ee::tailCall<int32_t>(kUnlockEditUi, ui); }
    }

    int32_t editUIUnlockController()
    {
        return unlockEditUiKeepingResult(*g_editUiController);
    }
    READABLE_FUNCTION(editUIUnlockController, Verified);

    int32_t playEditScript(uint32_t ui, uint32_t script)
    {
        *g_editUiController = ui;
        const ee::Ptr<zgui::Node> root = *g_uiRoot;
        *g_editScriptPlaying = 0;
        return tail::runScript(root, script, 0);
    }
    READABLE_FUNCTION_WAITS(playEditScript, Verified);

    int32_t playEditPauseScript(uint32_t ui)
    {
        return game::tail(playEditScript, ui, kTextOnEditPause);
    }
    READABLE_FUNCTION_WAITS(playEditPauseScript, Verified);

    int32_t playTestPauseScript(uint32_t ui)
    {
        return game::tail(playEditScript, ui, kTextOnTestPause);
    }
    READABLE_FUNCTION_WAITS(playTestPauseScript, Verified);

    void showMainHelp(uint32_t ui, uint32_t text)
    {
        zgui::setNodeValueByPath(kTextMainHelp, text);
        game::tail(playEditScript, ui, kTextMainHelpOnInit);
    }
    READABLE_FUNCTION_WAITS(showMainHelp, Verified);

    void hideMainHelp(uint32_t ui)
    {
        game::tail(playEditScript, ui, kTextMainHelpOnShutdown);
    }
    READABLE_FUNCTION_WAITS(hideMainHelp, Verified);

    // initEditHelpPages: made once, from the heap, and kept between editors, so a
    // help line closed for good stays closed.
    int32_t initEditHelpPages()
    {
        if (*g_editHelpPages != 0)
            return static_cast<int32_t>(*g_editHelpPages);
        const uint32_t address = zge::heapAllocate(static_cast<uint32_t>(sizeof(EditHelpPage)));
        *g_editHelpPages = address;
        const int32_t result = zge::stringCopy(address, kTextCameraHelp);
        const ee::Ptr<EditHelpPage> page(*g_editHelpPages);
        page->shown = 0;
        page->dismissed = 0;
        return result;
    }
    READABLE_FUNCTION_WAITS(initEditHelpPages, Verified);

    int32_t showEditHelpPage(uint32_t ui, uint32_t index)
    {
        const uint32_t offset = index * static_cast<uint32_t>(sizeof(EditHelpPage));
        const uint32_t address = offset + *g_editHelpPages;
        const ee::Ptr<EditHelpPage> page(address);
        uint32_t result = page->dismissed;
        if (result == 0)
        {
            result = page->shown;
            if (result != 0)
                return result;
            showMainHelp(ui, address);
            result = offset + *g_editHelpPages;
            ee::Ptr<EditHelpPage>(result)->shown = 1;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(showEditHelpPage, Verified);

    int32_t hideEditHelpPage(uint32_t ui, uint32_t index)
    {
        const uint32_t offset = index * static_cast<uint32_t>(sizeof(EditHelpPage));
        uint32_t result = offset + *g_editHelpPages;
        if (ee::Ptr<EditHelpPage>(result)->dismissed == 0)
        {
            hideMainHelp(ui);
            result = offset + *g_editHelpPages;
            ee::Ptr<EditHelpPage>(result)->shown = 0;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(hideEditHelpPage, Verified);

    int32_t dismissEditHelpPage(uint32_t ui, uint32_t index)
    {
        const uint32_t offset = index * static_cast<uint32_t>(sizeof(EditHelpPage));
        uint32_t result = offset + *g_editHelpPages;
        if (ee::Ptr<EditHelpPage>(result)->dismissed == 0)
        {
            hideMainHelp(ui);
            result = offset + *g_editHelpPages;
            const ee::Ptr<EditHelpPage> page(result);
            page->dismissed = 1;
            page->shown = 0;
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(dismissEditHelpPage, Verified);

    int32_t playTapeInitScript(uint32_t ui)
    {
        return game::tail(playEditScript, ui, kTextTapeOnInit);
    }
    READABLE_FUNCTION_WAITS(playTapeInitScript, Verified);

    int32_t playTapeShutdownScript(uint32_t ui)
    {
        return game::tail(playEditScript, ui, kTextTapeOnShutdown);
    }
    READABLE_FUNCTION_WAITS(playTapeShutdownScript, Verified);

    void clearCameraButtons(ee::Ptr<EditToolSet> toolSet)
    {
        clearUiButton(toolSet->ui, 0x10);
        clearUiButton(toolSet->ui, 17);
        game::tail(clearUiButton, toolSet->ui, 19);
    }
    READABLE_FUNCTION(clearCameraButtons, Verified);
}
