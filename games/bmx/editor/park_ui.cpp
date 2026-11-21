// The park editor's interface: the object behind ParkEditor.zsc, the flash its
// cursors pulse with, the pad's sticks and buttons read each frame into the
// button page's rows (level/park_cells.cpp and the tools bind handlers to them),
// the handlers run from those rows, what the tool panel shows and the help line.
// BMX XXX's own part of it; what both games have is zge/editor/park_ui.cpp.

#include "bmx/editor/park_ui.h"
#include "zge/editor/park_ui.h"
#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/function_ptr.h"
#include "bmx/editor/edit_records.h"
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
#include "bmx/editor/park_classes.h"
#include "bmx/editor/park_anchors.h"
#include "bmx/editor/edit_core.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/editor/park_ui.cpp.h"

namespace zge::editor
{
    namespace
    {
        constexpr uint32_t kRunScript = sym::runScript;          // (widget, text, 0)
        constexpr uint32_t kUnlockEditUi = sym::unlockEditUi;

        constexpr uint32_t kTextEditorVisible = sym::g_textRootParkEditorVisible;  // "Root.ParkEditor.Visible"
        constexpr uint32_t kTextInfoHelp = sym::g_textParkEditorMainUIInfoHelpText;       // "ParkEditor.MainUI.Info.Help.Text"

        constexpr uint32_t kTextPowerLevel = sym::g_textParkEditorMainUIInfoPowerLevel;     // "ParkEditor.MainUI.Info.Power.Level"
        constexpr uint32_t kTextSetPower = sym::g_textPlayParkEditorMainUIInfoPower;       // "PLAY ParkEditor.MainUI.Info.Power.SetPower"
        constexpr ee::Float kPowerBarWidth = 574.0f;

        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        inline ee::Global<int32_t, sym::g_editFlashLevel> g_editFlashLevel;   // the cursors' flash, 256 units a second
        inline ee::Global<uint32_t, sym::g_editHelpPages> g_editHelpPages;     // one page, EditHelpPage

        constexpr uint32_t kPressed = 1, kHeld = 2, kReleased = 4, kUp = 8; // a row's state
        // The inputs: the pad's buttons, then the axes; BMX XXX's last eight are the sticks' directions, each
        // axis's negative side and then its positive side as an input of its own.
        constexpr uint32_t kFirstStickDirection = 20;
    }

    // updateEditFlash: the cursors' flash goes round at 256 units a second. (BMX XXX adds
    // the frame's whole units to the counter; Aggressive Inline's goes through a float.)
    int32_t updateEditFlash(ee::Float seconds)
    {
        const int32_t next = *g_editFlashLevel + ee::toInt(seconds * ee::Float(256.0f));
        *g_editFlashLevel = next;
        return next;
    }
    READABLE_FUNCTION(updateEditFlash, Verified);

    int32_t setEditUiPad(uint32_t address, ee::Address pad);

    void unlockEditUi(uint32_t ui);

    namespace
    {
        namespace tail
        {
        int32_t runScript(ee::Address widget, uint32_t text, uint32_t argument) { return ee::tailCall<int32_t>(kRunScript, widget, text, argument); }
        }
    }

    int32_t freeUiButtonPage(uint32_t page);

    // freeEditHelpPages: the help pages go with the UI (BMX XXX; Aggressive Inline keeps
    // them between editors).
    void freeEditHelpPages()
    {
        const uint32_t pages = *g_editHelpPages;
        if (pages == 0)
            return;
        *g_editHelpPages = 0;
        zge::heapFree(pages);
    }
    READABLE_FUNCTION_WAITS(freeEditHelpPages, Verified);

    // The editor's UI hidden and freed: its button page, its help pages, then the record.
    void freeEditUi(uint32_t address)
    {
        ee::Local<uint32_t> value;
        *value = 0;
        zge::zgui::setNodeValueByPath(kTextEditorVisible, value);
        freeUiButtonPage(ee::Ptr<EditUi>(address)->buttonPage);
        freeEditHelpPages();
        editFree(address);
    }
    READABLE_FUNCTION_WAITS(freeEditUi, Verified);

    // BMX XXX's readEditUiButtons is not Aggressive Inline's word for word: its original runs until it has a body here.
    // initEditUiInputs: each input's pad bit or axis, every one up; the first sixteen are
    // buttons, the rest axes. (BMX XXX: 28 inputs, the last eight the sticks' directions.)
    int32_t initEditUiInputs(uint32_t inputs)
    {
        static constexpr uint32_t kMasks[kButtonCount] = {
            0x4000, 0x8000, 0x10, 0x20, 0x40, 0x80, 0x100, 0x400, 0x1000, 0x200, 0x800, 0x2000, 1, 2, 4, 8, 0, 1, 2, 3,
            1, 1, 0, 0, 3, 3, 2, 2,
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
                    if (i < kFirstStickDirection)
                        on = row->deadzone < ee::abs(value);
                    else if ((i - kFirstStickDirection) % 2 == 0)
                        on = value < -row->deadzone; // the axis's negative side
                    else
                        on = row->deadzone < value;  // its positive side
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

    // setEditUiPad: the pad the interface reads, and its inputs set up for it. (Aggressive
    // Inline's also releases the pad; BMX XXX's does not.)
    int32_t setEditUiPad(uint32_t address, ee::Address pad)
    {
        ee::Ptr<EditUi>(address)->pad = pad;
        return game::tail(initEditUiInputs, address + offsetof(EditUi, locked));
    }
    READABLE_FUNCTION(setEditUiPad, Verified);

    namespace
    {
        int32_t runScript(ee::Address widget, ee::Address text, uint32_t argument) { return ee::game<int32_t>(kRunScript, widget, text, argument); }
    }

    // setPowerLevel: the power bar, 0 to 1, in the panel's pixels.
    int32_t setPowerLevel(ee::Float level)
    {
        ee::Local<uint32_t> value;
        *value = static_cast<uint32_t>(ee::toInt(level * kPowerBarWidth));
        zge::zgui::setNodeValueByPath(kTextPowerLevel, value);
        return runScript(*g_uiRoot, kTextSetPower, 0);
    }
    READABLE_FUNCTION_WAITS(setPowerLevel, Verified);

    // A place refused because the park's cost allows nothing more: the help line says the park is full.
    // (Aggressive Inline's does nothing.)
    void editRefused()
    {
        game::tail(zge::zgui::setNodeValueByPath, kTextInfoHelp, sym::g_textYourParkIsFull);
    }
    READABLE_FUNCTION_WAITS(editRefused, Draft);
}
