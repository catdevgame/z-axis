#pragma once
// zgui's globals as BMX XXX's engine (ZGE4) lays them out: AI's (zge/zgui/
// widget.h, Zgui) up to the pads, then ZGE4's own. Only what BMX XXX's game
// code reads or sets is named; the offsets are read in the UI's start and
// the menus' sound script functions.
//
// Scaffolding (address-call): the engine's own layout once zge/zgui is in
// BMX XXX's instance.

#include "ee/ee.h"
#include "zge/scene/scene.h"
#include "zge/text/strings.h"
#include "zge/zgui/widget.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/ui/zgui_globals.h.h"

namespace game::bmx
{
    struct BmxZgui
    {
        uint32_t initialised;                   // +0x000
        ee::Ptr<zge::Heap> heap;                // +0x004
        uint32_t assets;                        // +0x008 the UI's library (assets\uilib.zal)
        ee::Ptr<zge::zgui::Node> root;          // +0x00C
        uint8_t _010[0x08];
        ee::Float alpha;                        // +0x018 1 at the UI's start
        uint8_t _01c[0x240];                    // (AI's four pads)
        ee::Ptr<zge::SceneObject> camera;       // +0x25C the UI's camera
        ee::Ptr<zge::StringTable> strings;      // +0x260 the UI's string table
        uint8_t _264[0x10];
        // +0x274 the UI's sound hooks, in AI's order one word further on
        // (the menus' script functions "bmxmenuscriptPlayOnSelectSound",
        // "..CancelSound" and "..ChangeSound" call the first, second and
        // fourth).
        zge::zgui::SoundHook selectSound, cancelSound, moveSound, changeSound, blockedSound;
        uint8_t _288[0x8C];
        // +0x314 the pad buttons the UI takes as start, accept, option, a
        // second option and cancel (r9bmxzgui's reading of ZGE4's zgui).
        uint32_t startButton;
        uint32_t acceptButton;
        uint32_t optionButton;
        uint32_t option2Button;                 // +0x320
        uint32_t cancelButton;                  // +0x324
    };
    static_assert(offsetof(BmxZgui, alpha) == 0x18 && offsetof(BmxZgui, camera) == 0x25C && offsetof(BmxZgui, strings) == 0x260);
    static_assert(offsetof(BmxZgui, selectSound) == 0x274 && offsetof(BmxZgui, blockedSound) == 0x284);
    static_assert(offsetof(BmxZgui, startButton) == 0x314 && offsetof(BmxZgui, cancelButton) == 0x324);
    inline ee::Global<BmxZgui, sym::g_zgui> g_bmxZgui;
}
