// The front end's trick menu (trick_menu.h): DM2's own code, registered as
// script functions by the menu set-up.

#include "trick_menu.h"
#include "player_menu.h"
#include "../game/players.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include "sym/dm2/menu/trick_menu.cpp.h"

namespace game::dm2
{
    namespace
    {
        // zgui's root node.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;

        constexpr ee::Ptr<const char> kRiderChoicePath{sym::selectPlayerPath}; // "MainMenu.SelectPlayer"
        constexpr ee::Ptr<const char> kTrickMenuPath{sym::trickMenuPath};      // "MainMenu.TrickMenu"
        constexpr uint32_t kCurrentValueName = sym::g_textCurValue;            // "CurValue"
        constexpr uint32_t kTrick1Name = sym::g_textTrick1;                    // "Trick1"
        constexpr uint32_t kTrick2Name = sym::g_textTrick2;                    // "Trick2"

        // The tricks the menu lists, in its order (trick numbers). The game
        // keeps the list as data, which harvestTrickMenu copies; the menu's
        // set-up has the same list written into its code and never reads the
        // data, so it has its own copy here.
        constexpr uint32_t kMenuTricks = 8;
        constexpr ee::Ptr<uint8_t> kMenuTrickNumbers{sym::g_trickMenuTricks};
        constexpr std::array<uint8_t, kMenuTricks> kMenuTrickList{12, 6, 38, 39, 40, 41, 42, 43};

        // DM2's findNode takes no kind: the shared declaration's is passed as 0.
        ee::Ptr<zge::zgui::Node> menuNode(ee::Ptr<const char> path)
        {
            return zge::zgui::findNode(*g_uiRoot, path, 0);
        }

    }

    void initializeTrickMenu()
    {
        ee::Local<ee::Float> character;
        ee::Local<int32_t> place;

        zge::zgui::copyVariable(menuNode(kRiderChoicePath), kCurrentValueName, character);
        const ee::Ptr<RiderCharacter> record = riderCharacter(sce::libgcc::fptoui(*character));
        for (uint32_t slot = 0; slot < kCharacterTricks; ++slot)
        {
            const uint32_t name = slot == 0 ? kTrick1Name : kTrick2Name;
            // Where the character's trick is in the menu's list. A trick the
            // menu does not list leaves `place` as it was: the first slot's
            // then shows whatever the stack held, the second repeats the first.
            const uint8_t trick = record->tricks[slot];
            for (uint32_t listed = 0; listed < kMenuTricks; ++listed)
                if (kMenuTrickList[listed] == trick)
                    *place = static_cast<int32_t>(listed);
            zge::zgui::setVariable(menuNode(kTrickMenuPath), name, place);
        }
    }
    READABLE_FUNCTION(initializeTrickMenu, Verified);

    void harvestTrickMenu()
    {
        // The menu's list, copied to the stack as the original has it.
        ee::Local<uint8_t[kMenuTricks]> menuTricks;
        ee::Local<ee::Float> character;
        ee::Local<int32_t> place;

        for (uint32_t listed = 0; listed < kMenuTricks; ++listed)
            menuTricks[listed] = kMenuTrickNumbers[listed];
        zge::zgui::copyVariable(menuNode(kRiderChoicePath), kCurrentValueName, character);
        const ee::Ptr<RiderCharacter> record = riderCharacter(sce::libgcc::fptoui(*character));
        const ee::Ptr<zge::zgui::Node> menu = menuNode(kTrickMenuPath);
        zge::zgui::copyVariable(menu, kTrick1Name, place);
        record->tricks[0] = *(ee::Ptr<uint8_t>(menuTricks) + static_cast<ptrdiff_t>(*place));
        zge::zgui::copyVariable(menu, kTrick2Name, place);
        record->tricks[1] = *(ee::Ptr<uint8_t>(menuTricks) + static_cast<ptrdiff_t>(*place));
    }
    READABLE_FUNCTION(harvestTrickMenu, Verified);
}
