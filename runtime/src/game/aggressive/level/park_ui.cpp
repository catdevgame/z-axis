// The park editor's interface: the object behind ParkEditor.zsc, the flash its
// cursors pulse with, the pad's sticks and buttons read each frame into the
// button page's rows (level/park_cells.cpp and the tools bind handlers to them),
// the handlers run from those rows, what the tool panel shows and the help line.
// Aggressive Inline's own part of it; what both games have is zge/editor/park_ui.cpp.

#include "aggressive/level/park_ui.h"
#include "zge/editor/park_ui.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "aggressive/level/edit_records.h"
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
#include "aggressive/level/park_classes.h"
#include "aggressive/level/park_anchors.h"
#include "aggressive/level/edit_core.h"

#include <cstddef>
#include <cstdint>
#include "sym/aggressive/level/park_ui.cpp.h"

namespace zge::editor
{
    // (the game's own names, which this part of the editor calls)
    using namespace aggressive;
    using namespace aggressive::level;

    namespace
    {
        constexpr uint32_t kAngleCos = sym::angleCos;
        constexpr uint32_t kListInit = sym::initList;
        constexpr uint32_t kListPush = sym::listPush;
        constexpr uint32_t kRunScript = sym::runScript;          // (widget, text, 0)
        constexpr uint32_t kWatchEditUiPad = sym::watchEditUiPad;
        constexpr uint32_t kSetEditUiPad = sym::setEditUiPad;
        constexpr uint32_t kUnlockEditUi = sym::unlockEditUi;
        constexpr uint32_t kRegisterEditUiFunctions = sym::registerEditUIFunctions;
        constexpr uint32_t kEditUiShowHook = sym::editUiShowHook;
        constexpr uint32_t kShowEditMemory = sym::showEditMemory;
        constexpr uint32_t kNewUiButtonPage = sym::newUiButtonPage;
        constexpr uint32_t kFreeUiButtonPage = sym::freeUiButtonPage;
        constexpr uint32_t kClearUiButtonPage = sym::clearUiButtonPage;
        constexpr uint32_t kInitEditUiInputs = sym::initEditUiInputs;
        constexpr uint32_t kPlayEditScript = sym::playEditScript;
        constexpr uint32_t kShowMainHelp = sym::showMainHelp;
        constexpr uint32_t kHideMainHelp = sym::hideMainHelp;
        constexpr uint32_t kInitEditHelpPages = sym::initEditHelpPages;

        constexpr uint32_t kTextEditorVisible = sym::g_textRootParkEditorVisible;  // "Root.ParkEditor.Visible"
        constexpr uint32_t kTextOnPlayPause = sym::g_textPlayRootParkEditorOnPlayPause;    // "PLAY Root.ParkEditor.OnPlayPause"
        constexpr uint32_t kTextOnInGameEditPause = sym::g_textPlayRootParkEditorOnInGameEditPaus; // "PLAY Root.ParkEditor.OnInGameEditPause"
        constexpr uint32_t kTextPowerLevel = sym::g_textParkEditorMainUIInfoPowerLevel;     // "ParkEditor.MainUI.Info.Power.Level"
        constexpr uint32_t kTextSetPower = sym::g_textPlayParkEditorMainUIInfoPower;       // "PLAY ParkEditor.MainUI.Info.Power.SetPower"

        constexpr ee::Float kFlashRate = 256.0f;              // angle units a second

        inline ee::Global<ee::Ptr<zgui::Node>, sym::g_uiRoot> g_uiRoot;
        inline ee::Global<int32_t, sym::g_editFlashLevel> g_editFlashLevel;
        inline ee::Global<int16_t, sym::g_editFlashLevel> g_editFlashAngle;

        constexpr uint32_t kPressed = 1, kHeld = 2, kReleased = 4, kUp = 8; // a row's state
        constexpr ee::Float kPowerBarWidth = 574.0f;
    }

    // updateEditFlash: the cursors' flash goes round at 256 units a second, the
    // counter a whole word kept through an unsigned conversion.
    int32_t updateEditFlash(ee::Float seconds)
    {
        const int32_t level = *g_editFlashLevel;
        ee::Float asFloat;
        if (level >= 0)
            asFloat = ee::toFloat(level);
        else
        {
            const ee::Float half = ee::toFloat(static_cast<int32_t>((static_cast<uint32_t>(level) & 1u) | (static_cast<uint32_t>(level) >> 1)));
            asFloat = half + half;
        }
        const int32_t next = static_cast<int32_t>(sce::libgcc::fptoui(asFloat + seconds * kFlashRate));
        *g_editFlashLevel = next;
        return next;
    }
    READABLE_FUNCTION(updateEditFlash, Verified);

    namespace
    {
        int32_t angleCos(uint32_t angle) { return ee::game<int32_t>(kAngleCos, angle); }
    }

    int32_t editFlashLevel()
    {
        return angleCos(*g_editFlashAngle);
    }
    READABLE_FUNCTION(editFlashLevel, Draft);

    int32_t setEditUiPad(uint32_t address, ee::Address pad);
    int32_t showEditMemory(uint32_t ui, uint32_t count);
    int32_t initEditUiInputs(uint32_t inputs);

    uint32_t newUiButtonPage();

    void unlockEditUi(uint32_t ui);

    int32_t initEditHelpPages();

    uint32_t registerEditUIFunctions();

    int32_t playEditScript(uint32_t ui, uint32_t script);

    void editUiShowHook();

    namespace
    {
        namespace tail
        {
        int32_t runScript(ee::Address widget, uint32_t text, uint32_t argument) { return ee::tailCall<int32_t>(kRunScript, widget, text, argument); }
        }
    }

    int32_t freeUiButtonPage(uint32_t page);

    void freeEditUi(uint32_t address)
    {
        ee::Local<uint32_t> value;
        *value = 0;
        zgui::setNodeValueByPath(kTextEditorVisible, value);
        freeUiButtonPage(ee::Ptr<EditUi>(address)->buttonPage);
        editFree(address);
    }
    READABLE_FUNCTION_WAITS(freeEditUi, Verified);

    // readEditUiButtons: each input's row in the button page: its value and state
    // (pressed, held, released, up), a stick past its dead zone counted as held.
    void readEditUiButtons(uint32_t address)
    {
        const ee::Ptr<EditUi> ui(address);
        watchEditUiPad(address);
        if (ui->padLost == 0)
        {
            const uint32_t page = ui->buttonPage;
            const ee::Ptr<zge::Pad> pad(ui->pad);
            const uint32_t changedBits = pad->changed, heldBits = pad->buttons;
            for (uint32_t i = 0;;)
            {
                const uint32_t mask = ui->mask[i];
                const ee::Ptr<UiButtonRow> row(page + offsetof(UiButtonPage, rows) + i * static_cast<uint32_t>(sizeof(UiButtonRow)));
                bool on, changing;
                if (ui->isAxis[i] != 0)
                {
                    const ee::Float value = ee::Ptr<zge::Pad>(ui->pad)->levels[mask];
                    on = row->deadzone < ee::abs(value);
                    changing = false;
                    const bool wasOn = ui->state[i] - kPressed < kHeld;
                    if (!(wasOn && on))
                        changing = true;
                    row->value = value;
                }
                else
                {
                    on = (heldBits & mask) != 0;
                    changing = (changedBits & mask) != 0;
                    if (on)
                        row->value = ee::Float(1.0f);
                    else
                        row->value = 0;
                }
                ui->frames[i] = ui->frames[i] + 1u;
                if (changing)
                {
                    ui->state[i] = on ? kPressed : kReleased;
                    ui->frames[i] = 0;
                }
                else
                    ui->state[i] = on ? kHeld : kUp;
                row->state = ui->state[i];
                if (!(++i < kButtonCount))
                    break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(readEditUiButtons, Verified);

    void editUiNoop()
    {
    }
    READABLE_FUNCTION(editUiNoop, Draft);

    int32_t setEditUiPad(uint32_t address, ee::Address pad)
    {
        ee::Ptr<EditUi>(address)->pad = pad;
        zge::releasePad(ee::Ptr<zge::Pad>(pad));
        return game::tail(initEditUiInputs, address + offsetof(EditUi, locked));
    }
    READABLE_FUNCTION(setEditUiPad, Verified);

    void editUiNoop2()
    {
    }
    READABLE_FUNCTION(editUiNoop2, Draft);

    namespace
    {
        int32_t runScript(ee::Address widget, ee::Address text, uint32_t argument) { return ee::game<int32_t>(kRunScript, widget, text, argument); }
    }

    void editUiNoop3()
    {
    }
    READABLE_FUNCTION(editUiNoop3, Draft);

    void editUiNoop4()
    {
    }
    READABLE_FUNCTION(editUiNoop4, Draft);

    void setUiButtonData(uint32_t ui, uint32_t button, uint32_t data)
    {
        const ee::Ptr<UiButtonPage> page(ee::Ptr<EditUi>(ui)->buttonPage);
        page->rows[button].data = data;
    }
    READABLE_FUNCTION(setUiButtonData, Draft);

    void setUiButtonDeadzone(uint32_t ui, uint32_t button, ee::Float deadzone)
    {
        const ee::Ptr<UiButtonPage> page(ee::Ptr<EditUi>(ui)->buttonPage);
        page->rows[button].deadzone = deadzone;
    }
    READABLE_FUNCTION(setUiButtonDeadzone, Draft);

    void setUiButtonStates(uint32_t ui, uint32_t button, uint32_t states)
    {
        const ee::Ptr<UiButtonPage> page(ee::Ptr<EditUi>(ui)->buttonPage);
        page->states[button] = states;
    }
    READABLE_FUNCTION(setUiButtonStates, Draft);

    void disableUiButton(uint32_t ui, uint32_t button)
    {
        const ee::Ptr<UiButtonPage> page(ee::Ptr<EditUi>(ui)->buttonPage);
        page->disabled[button] = 1;
    }
    READABLE_FUNCTION(disableUiButton, Verified);

    void enableUiButton(uint32_t ui, uint32_t button)
    {
        const ee::Ptr<UiButtonPage> page(ee::Ptr<EditUi>(ui)->buttonPage);
        page->disabled[button] = 0;
    }
    READABLE_FUNCTION(enableUiButton, Verified);

    void clearUiButtonPage(uint32_t address);

    // initEditUiInputs: takes the interface's +8. The pad's buttons by PadButton bit
    // in the order of the rows, then the sticks' four axes.
    int32_t initEditUiInputs(uint32_t inputs)
    {
        static constexpr uint32_t kMasks[kButtonCount] = {
            0x4000, 0x8000, 0x10, 0x20, 0x40, 0x80, 0x100, 0x400, 0x1000, 0x200, 0x800, 0x2000, 1, 2, 4, 8, 0, 1, 2, 3,
        };
        const ee::Ptr<EditUi> ui(inputs - offsetof(EditUi, locked));
        for (uint32_t i = 0; i < kButtonCount; ++i)
            ui->mask[i] = kMasks[i];
        for (uint32_t i = 0;;)
        {
            ui->state[i] = kUp;
            ui->frames[i] = 0;
            if (i < kPadButtonCount)
                ui->isAxis[i] = 0;
            else
                ui->isAxis[i] = 1;
            if (!(++i < kButtonCount))
                return 0;
        }
    }
    READABLE_FUNCTION(initEditUiInputs, Verified);

    int32_t playPlayPauseScript(uint32_t ui)
    {
        return game::tail(playEditScript, ui, kTextOnPlayPause);
    }
    READABLE_FUNCTION_WAITS(playPlayPauseScript, Draft);

    int32_t playInGameEditPauseScript(uint32_t ui)
    {
        return game::tail(playEditScript, ui, kTextOnInGameEditPause);
    }
    READABLE_FUNCTION_WAITS(playInGameEditPauseScript, Verified);

    // setPowerLevel: the power bar, 0 to 1, in the panel's pixels.
    int32_t setPowerLevel(ee::Float level)
    {
        ee::Local<uint32_t> value;
        *value = sce::libgcc::fptoui(level * kPowerBarWidth);
        zgui::setNodeValueByPath(kTextPowerLevel, value);
        return runScript(*g_uiRoot, kTextSetPower, 0);
    }
    READABLE_FUNCTION_WAITS(setPowerLevel, Verified);

    void editRefused()
    {
    }
    READABLE_FUNCTION(editRefused, Verified);
}
