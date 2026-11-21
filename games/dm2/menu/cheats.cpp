// The main menu's cheat codes (cheats.h): DM2's own code, registered as a
// script function by the menu set-up.
//
// A code is a string of key digits. The keys pressed are kept in a ring of
// 32; after each key every code is looked for at every place of the ring.
// There are general codes and, for each of seventeen characters, one code
// of each of six kinds.

#include "cheats.h"
#include "player_menu.h"
#include "../game/players.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"
#include "zge/zgui/widget.h"
#include "zge/zgui/widget_parts.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/menu/cheats.cpp.h"

namespace game::dm2
{
    namespace
    {
        // "MainMenu.MainMenuFrame.MainMenu.CheatSniffer.KeyIndex"
        constexpr ee::Ptr<const char> kKeyIndexPath{sym::cheatKeyIndexPath};

        // The last keys, as digits ('0' + the key's index), and where the
        // next one goes.
        constexpr uint32_t kKeysKept = 32;
        constexpr ee::Ptr<char> kKeys{sym::g_cheatKeys};
        inline ee::Global<uint32_t, sym::g_cheatKeyAt> g_cheatKeyAt;

        // The codes: the general ones, and a table of one a character for
        // each kind.
        using CodeTable = ee::Ptr<ee::Ptr<const char>>;
        constexpr CodeTable kGeneralCodes{sym::g_generalCodes};
        constexpr CodeTable kBikeCodes{sym::g_bikeCodes};
        constexpr CodeTable kCompetitionCodes{sym::g_competitionCodes};
        constexpr CodeTable kMovieCodes{sym::g_movieCodes};
        constexpr CodeTable kSponsorCodes{sym::g_sponsorCodes};
        constexpr CodeTable kLevelCodes{sym::g_levelCodes};
        constexpr CodeTable kTrickCodes{sym::g_trickCodes};

        // What each general code opens. The table has a tenth code (the
        // competition entry for everyone) that the check never reaches: it
        // goes over nine.
        enum GeneralCode : uint32_t
        {
            kAllThemes,        // the park editor's themes
            kAllEditorObjects, // the park editor's secret objects
            kAllMovies,        // the first sixteen characters' movies
            kFirstHiddenRider,
            kSecondHiddenRider,
            kTaunt,            // a sentence no keys can spell: nothing
            kAllBikes,
            kAllLevels,
            kEverything,
            kGeneralCodesChecked,
        };

        constexpr uint32_t kCheatCharacters = 17;
        constexpr uint32_t kMovieCharacters = 16;
        constexpr uint32_t kFirstHidden = 15, kSecondHidden = 16;
        constexpr uint8_t kAllThemesOpen = 10;
        constexpr uint8_t kAllSecretLevels = 14;
        constexpr uint8_t kAllBikesOpen = 5;
        // RiderCharacter::openLooks as the codes leave it: both looks open
        // (the rider choice's competition entry enabled).
        constexpr uint8_t kAllLooksOpen = 2;

        // The saved settings the general codes open: how many park themes
        // are open, and up to which secret level the park editor's objects are.
        inline ee::Global<uint8_t, sym::g_openThemeCount> g_openThemeCount;
        inline ee::Global<uint8_t, sym::g_secretLevelUnlocked> g_secretLevelUnlocked;

        // Each character's own four tricks (the tricks that are off for
        // everyone at first and on for their owner), six bytes an entry, the
        // trick's number first. The trick codes open each character's first.
        constexpr uint32_t kOwnTrickBytes = 6, kOwnTricksEach = 4;
        uint8_t firstOwnTrick(uint32_t character)
        {
            return *(ee::Ptr<uint8_t>(sym::g_characterOwnTricks)
                     + static_cast<ptrdiff_t>(character * (kOwnTrickBytes * kOwnTricksEach)));
        }

        // The code is at this place of the ring: taken out.
        // Bug kept: the keys cleared are the code's length of them from the
        // place after this one, so the code's first key stays and the key
        // after its last goes.
        bool takeCode(ee::Ptr<const char> code, uint32_t length, uint32_t at)
        {
            if (kKeys[at] != code[0])
                return false;
            uint32_t matched = 0;
            if (length != 0)
            {
                matched = 1;
                while (matched < length && kKeys[(at + matched) % kKeysKept] == code[matched])
                    ++matched;
            }
            if (matched != length)
                return false;
            for (uint32_t key = matched; key != 0; --key)
                kKeys[(at + key) % kKeysKept] = 0;
            return true;
        }

        // The UI's select sound (zgui's hook, as the menus play it).
        void selectSound() { zge::zgui::g_zgui->selectSound(); }

        void openSponsors(ee::Ptr<RiderCharacter> character)
        {
            for (uint32_t sponsor = 0; sponsor <= kSponsors; ++sponsor)
                character->sponsorShown[sponsor] = 1;
        }

        void openLevels(ee::Ptr<RiderCharacter> character)
        {
            for (uint32_t level = 0; level < kRespectLevels; ++level)
                character->levels[level].flags |= kLevelOpen;
        }

        void applyGeneralCode(uint32_t code)
        {
            switch (code)
            {
            case kAllThemes:
                *g_openThemeCount = kAllThemesOpen;
                break;
            case kAllEditorObjects:
                *g_secretLevelUnlocked = kAllSecretLevels;
                break;
            case kAllMovies:
                for (uint32_t character = 0; character < kMovieCharacters; ++character)
                    riderCharacter(character)->flags |= kCharacterMovieOpen;
                break;
            case kFirstHiddenRider:
                riderCharacter(kFirstHidden)->flags &= ~kCharacterLocked;
                break;
            case kSecondHiddenRider:
                riderCharacter(kSecondHidden)->flags &= ~kCharacterLocked;
                break;
            case kAllBikes:
                for (uint32_t character = 0; character < kCheatCharacters; ++character)
                    riderCharacter(character)->openBikes = kAllBikesOpen;
                break;
            case kAllLevels:
                for (uint32_t character = 0; character < kCheatCharacters; ++character)
                    openLevels(riderCharacter(character));
                break;
            case kEverything:
                riderCharacter(kFirstHidden)->flags &= ~kCharacterLocked;
                riderCharacter(kSecondHidden)->flags &= ~kCharacterLocked;
                for (uint32_t number = 0; number < kCheatCharacters; ++number)
                {
                    const ee::Ptr<RiderCharacter> character = riderCharacter(number);
                    character->openLooks = kAllLooksOpen;
                    character->openBikes = kAllBikesOpen;
                    openSponsors(character);
                    openLevels(character);
                    for (uint8_t &tricks : character->tricksOff)
                        tricks = 0;
                }
                selectSound();
                selectSound();
                break;
            default:
                break;
            }
            selectSound();
        }
    }

    void bmxCheckCheat()
    {
        ee::Local<int32_t> keyIndex;

        zge::zgui::copyVariableAtPath(kKeyIndexPath, keyIndex);
        kKeys[*g_cheatKeyAt] = static_cast<char>(static_cast<uint8_t>(*keyIndex) + '0');
        *g_cheatKeyAt = (*g_cheatKeyAt + 1) % kKeysKept;

        for (uint32_t code = 0; code < kGeneralCodesChecked; ++code)
        {
            const uint32_t length = static_cast<uint32_t>(zge::stringLength(kGeneralCodes[code]));
            for (uint32_t at = 0; at < kKeysKept; ++at)
                if (takeCode(kGeneralCodes[code], length, at))
                    applyGeneralCode(code);
        }

        // A character's bikes.
        for (uint32_t number = 0; number < kCheatCharacters; ++number)
        {
            const uint32_t length = static_cast<uint32_t>(zge::stringLength(kBikeCodes[number]));
            for (uint32_t at = 0; at < kKeysKept; ++at)
            {
                if (!takeCode(kBikeCodes[number], length, at))
                    continue;
                const ee::Ptr<RiderCharacter> character = riderCharacter(number);
                if (character->openBikes != kAllBikesOpen)
                {
                    character->openBikes = kAllBikesOpen;
                    selectSound();
                }
            }
        }
        // A character's competition entry.
        for (uint32_t number = 0; number < kCheatCharacters; ++number)
        {
            const uint32_t length = static_cast<uint32_t>(zge::stringLength(kCompetitionCodes[number]));
            for (uint32_t at = 0; at < kKeysKept; ++at)
            {
                if (!takeCode(kCompetitionCodes[number], length, at))
                    continue;
                const ee::Ptr<RiderCharacter> character = riderCharacter(number);
                if (character->openLooks != kAllLooksOpen)
                {
                    character->openLooks = kAllLooksOpen;
                    selectSound();
                }
            }
        }
        // A character's movie.
        for (uint32_t number = 0; number < kCheatCharacters; ++number)
        {
            const uint32_t length = static_cast<uint32_t>(zge::stringLength(kMovieCodes[number]));
            for (uint32_t at = 0; at < kKeysKept; ++at)
            {
                if (!takeCode(kMovieCodes[number], length, at))
                    continue;
                riderCharacter(number)->flags |= kCharacterMovieOpen;
                selectSound();
            }
        }
        // A character's sponsors.
        for (uint32_t number = 0; number < kCheatCharacters; ++number)
        {
            const uint32_t length = static_cast<uint32_t>(zge::stringLength(kSponsorCodes[number]));
            for (uint32_t at = 0; at < kKeysKept; ++at)
            {
                if (!takeCode(kSponsorCodes[number], length, at))
                    continue;
                openSponsors(riderCharacter(number));
                selectSound();
            }
        }
        // A character's levels.
        for (uint32_t number = 0; number < kCheatCharacters; ++number)
        {
            const uint32_t length = static_cast<uint32_t>(zge::stringLength(kLevelCodes[number]));
            for (uint32_t at = 0; at < kKeysKept; ++at)
            {
                if (!takeCode(kLevelCodes[number], length, at))
                    continue;
                openLevels(riderCharacter(number));
                selectSound();
            }
        }
        // A character's first own trick, opened for every character. A
        // character with none there (0xFF) has a byte past the tricks'
        // bits cleared of its top bit in every record instead: bug kept.
        for (uint32_t number = 0; number < kCheatCharacters; ++number)
        {
            const uint32_t length = static_cast<uint32_t>(zge::stringLength(kTrickCodes[number]));
            for (uint32_t at = 0; at < kKeysKept; ++at)
            {
                if (!takeCode(kTrickCodes[number], length, at))
                    continue;
                const uint8_t trick = firstOwnTrick(number);
                for (uint32_t other = 0; other < kCheatCharacters; ++other)
                {
                    const ee::Ptr<uint8_t> bits(riderCharacter(other)->tricksOff);
                    *(bits + static_cast<ptrdiff_t>(trick >> 3)) &= static_cast<uint8_t>(~(1u << (trick & 7)));
                }
                selectSound();
            }
        }
    }
    READABLE_FUNCTION(bmxCheckCheat, Verified);
}
