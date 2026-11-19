// BMX XXX's UI as a whole (ui_system.h): BMX XXX's own code (bmx.c).

#include "ui_system.h"

#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "bmx/ui/menu.h"
#include "bmx/ui/strings.h"
#include "bmx/ui/zgui_globals.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/assets/asset_library.h"
#include "zge/memory/dmm.h"
#include "zge/movie/movie.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include "sym/bmx/ui/ui_system.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The engine's pad as zgui's own view of it, and the UI's heap as the DMM region it is.
        ee::Ptr<zge::zgui::Pad> zguiPadOf(ee::Ptr<zge::Pad> pad) { return pad.cast<zge::zgui::Pad>(); }
        ee::Ptr<zge::DmmRegion> heapRegion(ee::Ptr<zge::Heap> heap) { return heap.cast<zge::DmmRegion>(); }
        constexpr ee::Ptr<const char> kCommonUi{sym::g_textUiCommon}; // "UI_common"
        // findNode's kind: a frame.
        constexpr uint32_t kFrameNode = 1;

        constexpr ee::Ptr<const char> kUiLibrary{sym::g_textUiLibrary};       // "assets\\uilib.zal"
        constexpr ee::Ptr<const char> kCommonScript{sym::g_textCommonScript}; // "common.zsc"
        constexpr ee::Ptr<const char> kHandlerName{sym::g_textBmxuiHandler};  // "bmxuiHandler"
        // The property types' names.
        constexpr ee::Ptr<const char> kString{sym::g_textTypeString}; // "String"
        constexpr ee::Ptr<const char> kSint32{sym::g_textTypeSint32}; // "Sint32"
        constexpr ee::Ptr<const char> kColour{sym::g_textTypeColour}; // "ZRGBA_PAL"
        // The accept and cancel buttons' pictures in the menus' prompts,
        // as the UI's scripts name them.
        constexpr ee::Ptr<const char> kButtonSelect{sym::g_textButtonSelect}; // "ButtonSelect"
        constexpr ee::Ptr<const char> kButtonCancel{sym::g_textButtonCancel}; // "ButtonCancel"
        constexpr ee::Ptr<const char> kButtonSelectValue{sym::g_textButtonSelectValue}; // "1"
        constexpr ee::Ptr<const char> kButtonCancelValue{sym::g_textButtonCancelValue}; // "3"
        constexpr uint32_t kButtonSelectProperty = sym::g_buttonSelect;
        constexpr uint32_t kButtonCancelProperty = sym::g_buttonCancel;
        constexpr ee::Ptr<const char> kDisplayMode{sym::g_textDisplayMode};       // "DisplayMode"
        constexpr ee::Ptr<const char> kMsgType{sym::g_textMsgType};               // "MsgType"
        constexpr ee::Ptr<const char> kModalPlayerNum{sym::g_textModalPlayerNum}; // "ModalPlayerNum"
        constexpr uint32_t kDisplayModeProperty = sym::g_displayMode;
        constexpr uint32_t kMsgTypeProperty = sym::g_msgType;
        constexpr uint32_t kModalPlayerNumProperty = sym::g_modalPlayerNum;
        // The menus' colours: twelve properties, each a colour record of
        // eight bytes, one after the other in this order.
        constexpr uint32_t kUiColours = 12;
        constexpr uint32_t kUiColourBytes = 8;
        constexpr uint32_t kUiColourValues = sym::g_uiColours;
        constexpr uint32_t kUiColourNames[kUiColours] = {
            sym::g_textColBGFrame,           // "ColBGFrame"
            sym::g_textColBGFrameOff,        // "ColBGFrameOff"
            sym::g_textColTextTitle,         // "ColTextTitle"
            sym::g_textColTextTitleOff,      // "ColTextTitleOff"
            sym::g_textColTextInfo,          // "ColTextInfo"
            sym::g_textColTextInfoOff,       // "ColTextInfoOff"
            sym::g_textColTextUnselected,    // "ColTextUnselected"
            sym::g_textColTextUnselectedOff, // "ColTextUnselectedOff"
            sym::g_textColTextSelected,      // "ColTextSelected"
            sym::g_textColTextSelectedOff,   // "ColTextSelectedOff"
            sym::g_textColTextDisabled,      // "ColTextDisabled"
            sym::g_textColTextDisabledOff,   // "ColTextDisabledOff"
        };
        // Four words the start clears (nothing read so far reads them).
        inline ee::Global<uint32_t[4], sym::g_uiStartCleared> g_uiStartCleared;
        // zgui's memory.
        constexpr uint32_t kZguiBytes = 1024 * 1024;
        // The pad buttons the UI takes.
        constexpr uint32_t kUiStart = 0x4000, kUiAccept = 0x80, kUiOption = 0x20, kUiOption2 = 0x40, kUiCancel = 0x10;

        // ZGE4's zgui icon roll's kind registered (no declaration a game
        // file can call yet).
        // Scaffolding (address-call): until zgui declares it.
        namespace calls
        {
            void registerZguiIconRoll() { ee::game(sym::registerZguiIconRoll); }
        }
    }

    void setUpUi()
    {
        BmxZgui &ui = *g_bmxZgui;
        zge::movieInit();
        zge::zgui::initZgui(kZguiBytes);
        ui.alpha = 1.0f;
        calls::registerZguiIconRoll();
        for (uint32_t port = 0; port < kMaxPlayers; ++port)
            zge::zgui::setZguiPad(zguiPadOf(rider::padInput(port)->pad), port); // (zgui reads the engine pad as its own view of it)

        zge::zgui::defineTypedProperty(ui.root, kButtonSelect, kString, kButtonSelectProperty);
        zge::zgui::defineTypedProperty(ui.root, kButtonCancel, kString, kButtonCancelProperty);
        zge::zgui::setVariable(ui.root, kButtonSelect, kButtonSelectValue);
        zge::zgui::setVariable(ui.root, kButtonCancel, kButtonCancelValue);

        ui.selectSound = sym::playSelectSound;
        ui.cancelSound = sym::playCancelSound;
        ui.moveSound = sym::playMoveSound;
        ui.changeSound = sym::playChangeSound;
        ui.blockedSound = sym::playBlockedSound;
        ui.startButton = kUiStart;
        ui.acceptButton = kUiAccept;
        ui.optionButton = kUiOption;
        ui.option2Button = kUiOption2;
        ui.cancelButton = kUiCancel;
        ui.assets = zge::openAssetLibrary(kUiLibrary);

        zge::zgui::registerScriptFunction(kHandlerName, sym::bmxuiHandler);
        zge::zgui::defineTypedProperty(ui.root, kDisplayMode, kSint32, kDisplayModeProperty);
        zge::zgui::defineTypedProperty(ui.root, kMsgType, kSint32, kMsgTypeProperty);
        zge::zgui::defineTypedProperty(ui.root, kModalPlayerNum, kSint32, kModalPlayerNumProperty);
        for (uint32_t colour = 0; colour < kUiColours; ++colour)
            zge::zgui::defineTypedProperty(ui.root, kUiColourNames[colour], kColour, kUiColourValues + colour * kUiColourBytes);

        setLanguage(g_savedGame->language);
        zge::zgui::loadZguiScript(kCommonScript, ui.root);
        (*g_uiStartCleared)[3] = 0;
        (*g_uiStartCleared)[2] = 0;
        (*g_uiStartCleared)[0] = 0;
        (*g_uiStartCleared)[1] = 0;
    }
    READABLE_FUNCTION_WAITS(setUpUi, Verified);

    void playSelectSound()
    {
        game::tail(soundEvent, kSoundMenuSelect, 0, 0, 0, 0);
    }
    READABLE_FUNCTION(playSelectSound, Verified);

    void playCancelSound()
    {
        game::tail(soundEvent, kSoundMenuCancel, 0, 0, 0, 0);
    }
    READABLE_FUNCTION(playCancelSound, Verified);

    void playMoveSound()
    {
        game::tail(soundEvent, kSoundMenuMove, 0, 0, 0, 0);
    }
    READABLE_FUNCTION(playMoveSound, Verified);

    void playChangeSound()
    {
        game::tail(soundEvent, kSoundMenuChange, 0, 0, 0, 0);
    }
    READABLE_FUNCTION(playChangeSound, Verified);

    void playBlockedSound()
    {
        game::tail(soundEvent, kSoundMenuBlocked, 0, 0, 0, 0);
    }
    READABLE_FUNCTION(playBlockedSound, Verified);

    void shutDownUi()
    {
        zge::zgui::Zgui &ui = *zge::zgui::g_zgui;
        freeGameStrings(*g_gameStrings);
        const ee::Ptr<zge::zgui::Node> common = zge::zgui::findNode(ui.root, kCommonUi, kFrameNode);
        *g_gameStrings = ee::Ptr<zge::StringTable>();
        zge::zgui::destroyNode(common);
        zge::closeAssetLibrary(ui.assets);
        zge::dmmCompact(heapRegion(ui.heap));
        game::tail(zge::movieShutdown);
    }
    READABLE_FUNCTION_WAITS(shutDownUi, Verified);
}
