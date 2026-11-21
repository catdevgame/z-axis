// The front end's rider choice (player_menu.h): DM2's own code, registered
// as script functions by the menu set-up.

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

#include <cstddef>
#include <cstdint>
#include "sym/dm2/menu/player_menu.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The choice's last value, and the characters its 15th and 16th
        // places stand for (the menu counts characters from 1).
        constexpr ee::Ptr<char> kMaxValuePath{sym::selectPlayerMaxValuePath}; // "MainMenu.SelectPlayer.MaxValue"
        constexpr ee::Ptr<char> kPlace15Path{sym::selectPlayer15IndexPath};   // "MainMenu.SelectPlayer.Player15Index"
        constexpr ee::Ptr<char> kPlace16Path{sym::selectPlayer16IndexPath};   // "MainMenu.SelectPlayer.Player16Index"

        // The two hidden riders, as the menu counts them.
        constexpr uint32_t kFirstHidden = 15;
        constexpr uint32_t kSecondHidden = 16;

        bool locked(uint32_t menuCharacter)
        {
            return (riderCharacter(menuCharacter)->flags & kCharacterLocked) != 0;
        }

        // zgui's root node.
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;

        constexpr ee::Ptr<const char> kRiderChoicePath{sym::selectPlayerPath};  // "MainMenu.SelectPlayer"
        constexpr uint32_t kCurrentValueName = sym::g_textCurValue;             // "CurValue"
        constexpr uint32_t kPlace15Name = sym::g_textPlayer15Index;             // "Player15Index"
        constexpr uint32_t kPlace16Name = sym::g_textPlayer16Index;             // "Player16Index"
        constexpr ee::Ptr<const char> kStatBarsPath{sym::selectPlayerValuePath}; // "MainMenu.SelectPlayer.Frame.Value"
        constexpr uint32_t kTextName = sym::g_textText;                         // "Text"
        // "CommonUI.SponsorLogo.Logo%d.Visible"
        constexpr uint32_t kSponsorLogoShownFormat = sym::sponsorLogoVisibleFormat;
        // "PLAY MainMenu.SelectPlayer.EnableCompetition", "... DisableCompetition"
        constexpr ee::Ptr<char> kEnableCompetition{sym::enableCompetitionScript};
        constexpr ee::Ptr<char> kDisableCompetition{sym::disableCompetitionScript};
        // "Respect:\n#RGB{55 138 230} %d/40,000", and "%d,%03d/40,000" from a thousand up.
        constexpr uint32_t kRespectFormat = sym::respectFormat;
        constexpr uint32_t kRespectThousandsFormat = sym::respectThousandsFormat;
        // "MainMenu.SelectPlayer.Frame.Respect.Text", "MainMenu.SelectBike.Frame.Respect.Text"
        constexpr ee::Ptr<const char> kRiderChoiceRespectPath{sym::selectPlayerRespectTextPath};
        constexpr ee::Ptr<const char> kBikeChoiceRespectPath{sym::selectBikeRespectTextPath};

        // A stat's bar is drawn as this character, once a point.
        constexpr int32_t kBarCharacter = '>';
        constexpr uint32_t kStatBarsBytes = 0x100;

        // DM2's findNode takes no kind: the shared declaration's is passed as 0.
        ee::Ptr<zge::zgui::Node> riderChoice()
        {
            return zge::zgui::findNode(*g_uiRoot, kRiderChoicePath, 0);
        }

        // Scaffolding (address-call): libc's memset (no ported declaration
        // game code can call by name yet).
        void clearBytes(ee::Ptr<char> bytes, uint32_t count) { ee::game(sym::memset, bytes, 0u, count); }

        // zgui's variable at a path from the UI's root set to a number (its
        // float in `value`).
        void setMenuNumber(ee::Ptr<char> path, ee::Local<ee::Float> &value, float number)
        {
            *value = number;
            zge::zgui::setVariableAtPath(path, value);
        }
    }

    void bmxSetupUnLockPlayer()
    {
        // Each case's values have their own place, as in the original.
        ee::Local<ee::Float> lastWithBoth;
        ee::Local<ee::Float> place15WithBoth;
        ee::Local<ee::Float> place16WithBoth;
        ee::Local<ee::Float> lastWithFirst;
        ee::Local<ee::Float> place15WithFirst;
        ee::Local<ee::Float> lastWithSecond;
        ee::Local<ee::Float> place15WithSecond;
        ee::Local<ee::Float> lastWithNeither;
        const bool firstLocked = locked(kFirstHidden);
        const bool secondLocked = locked(kSecondHidden);
        if (!firstLocked && !secondLocked)
        {
            setMenuNumber(kMaxValuePath, lastWithBoth, 16.0f);
            setMenuNumber(kPlace15Path, place15WithBoth, 15.0f);
            setMenuNumber(kPlace16Path, place16WithBoth, 16.0f);
        }
        else if (!firstLocked)
        {
            setMenuNumber(kMaxValuePath, lastWithFirst, 15.0f);
            setMenuNumber(kPlace15Path, place15WithFirst, 15.0f);
        }
        else if (!secondLocked)
        {
            // Only the second is open: it takes the 15th place.
            setMenuNumber(kMaxValuePath, lastWithSecond, 15.0f);
            setMenuNumber(kPlace15Path, place15WithSecond, 16.0f);
        }
        else
        {
            setMenuNumber(kMaxValuePath, lastWithNeither, 14.0f);
        }
    }
    READABLE_FUNCTION(bmxSetupUnLockPlayer, Verified);

    ee::Ptr<RiderCharacter> riderCharacter(uint32_t character)
    {
        return ee::Ptr<RiderCharacter>(sym::g_riderCharacters) + static_cast<ptrdiff_t>(character);
    }

    uint8_t riderStatBars(uint32_t character, uint32_t stat)
    {
        return *(ee::Ptr<uint8_t>(sym::g_riderStatBars) + static_cast<ptrdiff_t>(character * kRiderStats + stat));
    }

    void showRespect(ee::Ptr<char> text, uint32_t respect)
    {
        if (respect < 1000)
            zge::formatString(text, kRespectFormat, respect);
        else
            zge::formatString(text, kRespectThousandsFormat, respect / 1000, respect % 1000);
        zge::zgui::setVariableAtPath(kRiderChoiceRespectPath, text);
        zge::zgui::setVariableAtPath(kBikeChoiceRespectPath, text);
    }

    void bmxSetupPlayerStat()
    {
        ee::Local<char[kStatBarsBytes]> text;
        ee::Local<char[0x40]> logoShownPath;
        ee::Local<ee::Float> character;

        clearBytes(text, kStatBarsBytes);
        zge::zgui::copyVariable(riderChoice(), kCurrentValueName, character);
        // The choice's last two places stand for the hidden riders that are open.
        if (*character == 15.0f)
            zge::zgui::copyVariable(riderChoice(), kPlace15Name, character);
        else if (*character == 16.0f)
            zge::zgui::copyVariable(riderChoice(), kPlace16Name, character);

        // The stats, a line of bar characters each.
        ee::Ptr<char> line = text;
        for (uint32_t stat = 0; stat < kRiderStats; ++stat)
        {
            const uint8_t bars = riderStatBars(sce::libgcc::fptoui(*character), stat);
            zge::memoryFill(line, kBarCharacter, bars);
            line += bars;
            line[0] = '\r';
            line[1] = '\n';
            line[2] = 0;
            line += 2;
        }
        zge::zgui::setVariable(zge::zgui::findNode(*g_uiRoot, kStatBarsPath, 0), kTextName, text);

        // The character's sponsors' logos (the script counts them from 1).
        for (uint32_t sponsor = 1; sponsor <= kSponsors; ++sponsor)
        {
            zge::formatString(logoShownPath, kSponsorLogoShownFormat, sponsor);
            const ee::Ptr<RiderCharacter> record = riderCharacter(sce::libgcc::fptoui(*character));
            zge::zgui::setVariableAtPath(logoShownPath, &record->sponsorShown[sponsor]);
        }

        if (riderCharacter(sce::libgcc::fptoui(*character))->openLooks < 2)
            zge::zgui::runScriptLines(ee::Ptr<zge::zgui::Node>(), kDisableCompetition);
        else
            zge::zgui::runScriptLines(ee::Ptr<zge::zgui::Node>(), kEnableCompetition);

        showRespect(text, riderCharacter(sce::libgcc::fptoui(*character))->respect);
    }
    READABLE_FUNCTION(bmxSetupPlayerStat, Verified);
}
