// The game's UI set-up and the common UI's unloading (ui_frame.h): DM2's own
// code (action.c).

#include "card_frame.h"
#include "ui_frame.h"
#include "../rider/rider.h"
#include "../sound/menu_sounds.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "dm2/editor/edit_gui.h"
#include "ee/readable.h"
#include "zge/assets/asset_library.h"
#include "zge/input/pads.h"
#include "zge/memory/dmm.h"
#include "zge/movie/movie.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/action/ui_setup.cpp.h"

namespace zge
{
    uint32_t dmmCompact(ee::Ptr<DmmRegion> region); // memory/dmm.cpp
}

namespace game::dm2
{
    namespace rider
    {
        PadInput *padInput(uint32_t port); // input.cpp
    }

    namespace
    {
        // zgui's memory, in bytes.
        constexpr uint32_t kZguiBytes = 0xC8000;
        constexpr uint32_t kUilibPath = sym::uilibPath;                   // "assets\\Uilib.zal"
        constexpr uint32_t kStringTablePath = sym::stringTablePath;       // "assets\\string.eus"
        constexpr uint32_t kCommonUiScriptFile = sym::commonUiScriptFile; // "commonui.zsc"
        constexpr uint32_t kCommonUiName = sym::commonUiName;             // "CommonUI"
        constexpr uint32_t kMemCardHandlerName = sym::memCardHandlerName; // "bmxuiMemCardHandler"
        // The root's variables the memory card frame's scripts use.
        constexpr ee::Ptr<const char> kMessageTypeName{sym::messageTypeName}; // "MsgType"
        constexpr ee::Ptr<const char> kModalPlayerName{sym::modalPlayerName}; // "ModalPlayerNum"
        constexpr ee::Ptr<const char> kSint32TypeName{sym::sint32TypeName};   // "Sint32"


        // The UI's string table (zgui keeps it for the texts' $KEYs).
        inline ee::Global<ee::Ptr<zge::StringTable>, sym::g_uiStrings> g_uiStrings;
    }

    void initGameUi()
    {
        using zge::zgui::g_zgui;
        zge::movieInit();
        zge::zgui::initZgui(kZguiBytes);
        g_zgui->alpha = 1.0f;
        zge::zgui::setZguiPad(ee::Ptr<zge::zgui::Pad>(rider::padInput(0)->pad), 0);
        zge::zgui::setZguiPad(ee::Ptr<zge::zgui::Pad>(rider::padInput(1)->pad), 1);
        g_zgui->moveSound = zge::zgui::SoundHook(sym::playMoveSound);
        g_zgui->selectSound = zge::zgui::SoundHook(sym::playSelectSound);
        g_zgui->cancelSound = zge::zgui::SoundHook(sym::playCancelSound);
        g_zgui->changeSound = zge::zgui::SoundHook(sym::playChangeSound);
        g_zgui->blockedSound = zge::zgui::SoundHook(sym::playBlockedSound);
        g_zgui->startButton = zge::kPadStart;
        g_zgui->acceptButton = zge::kPadCross;
        g_zgui->optionButton = zge::kPadSquare;
        g_zgui->cancelButton = zge::kPadTriangle;
        g_zgui->assets = zge::openAssetLibrary(kUilibPath);
        zge::zgui::registerScriptFunction(kMemCardHandlerName, sym::bmxuiMemCardHandler);
        // (The root widget's object: ZGE2's widgets are scene objects.)
        zge::zgui::defineWidgetProperty(ee::Ptr<zge::SceneObject>(g_zgui->root.address), kMessageTypeName, kSint32TypeName, sym::g_uiMessageType);
        zge::zgui::defineWidgetProperty(ee::Ptr<zge::SceneObject>(g_zgui->root.address), kModalPlayerName, kSint32TypeName, sym::g_uiModalPlayer);
        *g_uiStrings = zge::loadStringTableFile(kStringTablePath);
        zge::setStringTable(*g_uiStrings);
        zge::zgui::loadZguiScript(kCommonUiScriptFile, g_zgui->root);
        g_uiFrameRequest->onDone = UiFrameCallback();
        g_uiFrameRequest->dataHandle = 0;
        g_uiFrameRequest->onClosed = UiFrameCallback();
        g_uiFrameRequest->data = 0;
    }
    READABLE_FUNCTION(initGameUi, Verified);

    uint32_t unloadCommonUi()
    {
        using zge::zgui::g_zgui;
        zge::deleteSceneObject(widgetObject(zge::zgui::findNode(g_zgui->root, kCommonUiName, 0)));
        return game::tail(zge::dmmCompact, zgui::heapRegion(g_zgui->heap));
    }
    READABLE_FUNCTION(unloadCommonUi, Verified);
}
