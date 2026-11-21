// The front end's bike choice (bike_menu.h): DM2's own code, registered as
// script functions by the menu set-up.

#include "bike_menu.h"
#include "menu_variables.h"
#include "player_menu.h"
#include "../game/players.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "sce/libgcc.h"
#include "zge/text/cstring.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstdint>
#include "sym/dm2/menu/bike_menu.cpp.h"

namespace game::dm2
{
    namespace
    {
        // zgui's root node.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;

        constexpr ee::Ptr<const char> kBikeChoicePath{sym::selectBikePath}; // "MainMenu.SelectBike"
        constexpr ee::Ptr<const char> kMainMenuPath{sym::g_textMainMenu};   // "MainMenu"
        constexpr uint32_t kCurrentValueName = sym::g_textCurValue;         // "CurValue"
        constexpr uint32_t kPlayerIndexName = sym::g_textPlayerIndex;       // "PlayerIndex"
        constexpr uint32_t kCurrentRiderFormat = sym::currentRiderFormat;   // "CurrentRider%d"
        constexpr uint32_t kBikeModelFormat = sym::bikeModelFormat;         // "Bike%02d_%d"
        constexpr uint32_t kCurrentBikeName = sym::g_textCurBike;           // "CurBike"

        constexpr ee::Ptr<const char> kPlayerIndexPath{sym::mainMenuPlayerIndexPath};     // "MainMenu.PlayerIndex"
        constexpr ee::Ptr<const char> kStatBarsPath{sym::selectBikeValuePath}; // "MainMenu.SelectBike.Frame.Value"
        constexpr uint32_t kTextName = sym::g_textText;                        // "Text"
        // What follows the rider's bar and the bike's bar on a stat's line:
        // "#RGBA{230 55 38 255}" and "\n#RGBA{55 138 230 255}" (the bike's
        // part of the bar in the first colour, the next line's start in the second).
        constexpr uint32_t kAfterRiderBar = sym::bikeBarColourText;
        constexpr uint32_t kAfterBikeBar = sym::nextStatLineText;
        // "PLAY MainMenu.SelectBike.LockBike", "PLAY MainMenu.SelectBike.UnLockBike"
        constexpr ee::Ptr<char> kLockBike{sym::lockBikeScript};
        constexpr ee::Ptr<char> kUnlockBike{sym::unlockBikeScript};

        // A stat's bar is drawn as this character, once a point.
        constexpr int32_t kBarCharacter = '>';
        constexpr uint32_t kStatBarsBytes = 0x400;
        constexpr uint32_t kBikesPerRider = 5;

        // What a bike adds to its rider's stat, in bar points.
        uint8_t bikeStatBars(uint32_t rider, uint32_t bike, uint32_t stat)
        {
            return *(ee::Ptr<uint8_t>(sym::g_bikeStatBars)
                     + static_cast<ptrdiff_t>(stat + bike * kRiderStats + rider * (kBikesPerRider * kRiderStats)));
        }

        // The rider ("CurrentRider1", "CurrentRider2") of the player the
        // main menu is on (players count from 1).
        uint32_t riderOfPlayer(int32_t player)
        {
            return (*g_currentRiders)[player - 1];
        }

        // Scaffolding (address-call): libc's memset (no ported declaration
        // game code can call by name yet).
        void clearBytes(ee::Ptr<char> bytes, uint32_t count) { ee::game(sym::memset, bytes, 0u, count); }

        // DM2's findNode takes no kind: the shared declaration's is passed as 0.
        ee::Ptr<zge::zgui::Node> menuNode(ee::Ptr<const char> path)
        {
            return zge::zgui::findNode(*g_uiRoot, path, 0);
        }
    }

    void bmxmenuBikeModelUpdate()
    {
        ee::Local<char[64]> bikeModel;
        ee::Local<char[256]> riderVariable;
        ee::Local<ee::Float> choice;
        ee::Local<int32_t> player;
        ee::Local<int32_t> rider;
        const ee::Ptr<zge::zgui::Node> bikeChoice = menuNode(kBikeChoicePath);
        zge::zgui::copyVariable(bikeChoice, kCurrentValueName, choice);
        zge::zgui::copyVariable(menuNode(kMainMenuPath), kPlayerIndexName, player);
        // The player's rider is the root's variable "CurrentRider<player>".
        zge::formatString(riderVariable, kCurrentRiderFormat, *player);
        zge::zgui::copyVariable(*g_uiRoot, riderVariable, rider);
        zge::formatString(bikeModel, kBikeModelFormat, *rider, ee::toInt(*choice));
        zge::zgui::setVariable(bikeChoice, kCurrentBikeName, bikeModel);
    }
    READABLE_FUNCTION(bmxmenuBikeModelUpdate, Draft);

    void bmxSetupBikeStat()
    {
        ee::Local<char[kStatBarsBytes]> text;
        ee::Local<char[0x20]> bar;
        ee::Local<char[0x40]> respectText;
        ee::Local<int32_t> player;
        ee::Local<ee::Float> bike;

        clearBytes(text, kStatBarsBytes);
        zge::zgui::copyVariableAtPath(kPlayerIndexPath, player);
        zge::zgui::copyVariable(menuNode(kBikeChoicePath), kCurrentValueName, bike);

        // Each stat's line: the rider's own bar, then what the bike adds.
        for (uint32_t stat = 0; stat < kRiderStats; ++stat)
        {
            const uint8_t riderBars = riderStatBars(riderOfPlayer(*player), stat);
            zge::memoryFill(bar, kBarCharacter, riderBars);
            bar[riderBars] = 0;
            zge::appendString(text, bar);
            zge::appendString(text, kAfterRiderBar);

            const uint32_t bikeNumber = sce::libgcc::fptoui(*bike);
            const uint8_t bikeBars = bikeStatBars(riderOfPlayer(*player), bikeNumber, stat);
            zge::memoryFill(bar, kBarCharacter, bikeBars);
            bar[bikeBars] = 0;
            zge::appendString(text, bar);
            zge::appendString(text, kAfterBikeBar);
        }

        // The character's bikes open one after another.
        const uint32_t bikeNumber = sce::libgcc::fptoui(*bike);
        const bool open = bikeNumber < riderCharacter(riderOfPlayer(*player))->openBikes;
        zge::zgui::setVariable(menuNode(kStatBarsPath), kTextName, text);
        if (open)
            zge::zgui::runScriptLines(ee::Ptr<zge::zgui::Node>(), kUnlockBike);
        else
            zge::zgui::runScriptLines(ee::Ptr<zge::zgui::Node>(), kLockBike);

        showRespect(respectText, riderCharacter(riderOfPlayer(*player))->respect);
    }
    READABLE_FUNCTION(bmxSetupBikeStat, Verified);
}
