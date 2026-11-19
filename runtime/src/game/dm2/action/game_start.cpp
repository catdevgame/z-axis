// The game's start-up (game_start.h): DM2's own code (action.c).

#include "game_start.h"
#include "action.h"
#include "ui_frame.h"
#include "../game/gaps.h"
#include "../game/players.h"
#include "../input/pads.h"
#include "../sound/menu_sounds.h"
#include "../sound/sound_mode.h"

#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/dm2/action/game_start.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The saved game: the settings, every character's record and the
        // players' set-up (the memory card saves and loads this block whole).
        constexpr uint32_t kSavedGame = sym::g_savedGame;
        constexpr uint32_t kSavedGameBytes = 0x4F98;
        inline ee::Global<uint8_t, sym::g_openThemeCount> g_openThemeCount;
        inline ee::Global<uint8_t, sym::g_secretLevelUnlocked> g_secretLevelUnlocked;
        inline ee::Global<uint8_t, sym::g_sfxVolumeSetting> g_sfxVolumeSetting;
        inline ee::Global<uint8_t, sym::g_musicVolumeSetting> g_musicVolumeSetting;
        constexpr uint8_t kThemesOpenAtFirst = 5;
        constexpr uint8_t kSfxVolumeAtFirst = 0x80, kMusicVolumeAtFirst = 0x40;

        // A created rider's names.
        constexpr uint32_t kCreatedFirstName = sym::g_textCreated;    // "Created"
        constexpr uint32_t kCreatedLastName = sym::g_textRider;       // "Rider"
        constexpr uint32_t kCreatedNickname = sym::g_textCreatedRider; // "Created Rider"
        constexpr uint32_t kPlayerName = sym::g_textPlayer;           // "player"

        // The stats' bar points, eight a character, and the two tricks each
        // character starts with.
        constexpr ee::Ptr<uint8_t> kStatBars{sym::g_riderStatBars};
        constexpr ee::Ptr<uint8_t> kStartTricks{sym::g_characterStartTricks};

        // The tricks: a trick whose kind has this bit starts off for every
        // character; a character's own four (six bytes an entry, the trick's
        // number first, 0xFF for none) are then turned back on.
        constexpr uint32_t kTricks = 0x48;
        constexpr uint16_t kTrickOffAtFirst = 0x8;
        constexpr ee::Ptr<uint16_t> kTrickKinds{sym::g_trickKindsR};
        constexpr ee::Ptr<uint8_t> kOwnTricks{sym::g_characterOwnTricks};
        constexpr uint32_t kOwnTricksEach = 4, kOwnTrickBytes = 6;
        constexpr uint8_t kNoTrick = 0xFF;

        // The characters from this one on are locked at first.
        constexpr uint32_t kFirstLockedCharacter = 14;
        // The levels open to every character at first.
        constexpr uint32_t kFirstLevel = 2, kParkLevelNumber = 8;

        // Ten set-up functions of the game's parts, called in turn.
        constexpr uint32_t kPartSetUps = 10;
        constexpr ee::Ptr<ee::FunctionPtr<void()>> kPartSetUpTable{sym::g_partSetUps};

        // The game state the frame is in: none yet.
        inline ee::Global<uint8_t, sym::g_currentGameState> g_currentGameState;
        constexpr uint8_t kNoGameState = 0xFF;

        void setTrickOff(ee::Ptr<RiderCharacter> character, uint32_t trick, bool off)
        {
            uint8_t &bits = character->tricksOff[trick >> 3];
            const uint8_t bit = static_cast<uint8_t>(1u << (trick & 7));
            bits = off ? (bits | bit) : (bits & static_cast<uint8_t>(~bit));
        }
    }

    uint8_t initGame()
    {
        zge::memoryFill(kSavedGame, 0, kSavedGameBytes);
        for (uint32_t number = 0; number < kCharacters; ++number)
        {
            const ee::Ptr<RiderCharacter> character = ee::Ptr<RiderCharacter>(sym::g_riderCharacters)
                                                    + static_cast<ptrdiff_t>(number);
            zge::memoryFill(character, 0, sizeof(RiderCharacter));
            if (number >= kFirstLockedCharacter)
                character->flags |= kCharacterLocked;
            character->sponsorShown[0] = 1;
            if (number < kBuiltInCharacters)
            {
                const CharacterNames &names = kCharacterNames[number];
                zge::stringCopy(character->firstName, names.firstName);
                zge::stringCopy(character->lastName, names.lastName);
                zge::stringCopy(character->nickname, names.nickname);
                for (uint32_t stat = 0; stat < sizeof(character->stats); ++stat)
                    character->stats[stat] = kStatBars[number * sizeof(character->stats) + stat];
            }
            else
            {
                zge::stringCopy(character->firstName, kCreatedFirstName);
                zge::stringCopy(character->lastName, kCreatedLastName);
                zge::stringCopy(character->nickname, kCreatedNickname);
            }
            character->tricks[0] = kStartTricks[number * kCharacterTricks];
            character->tricks[1] = kStartTricks[number * kCharacterTricks + 1];
            character->openBikes = 1;
            character->openLooks = 1;
            for (uint32_t trick = 0; trick < kTricks; ++trick)
                if (kTrickKinds[trick] & kTrickOffAtFirst)
                    setTrickOff(character, trick, true);
            for (uint32_t own = 0; own < kOwnTricksEach; ++own)
            {
                const uint8_t trick = kOwnTricks[number * (kOwnTricksEach * kOwnTrickBytes) + own * kOwnTrickBytes];
                if (trick != kNoTrick)
                    setTrickOff(character, trick, false);
            }
            character->levels[kFirstLevel].flags |= kLevelOpen;
            character->levels[kParkLevelNumber].flags |= kLevelOpen;
        }

        // The settings' defaults (the rest of them stay cleared).
        *g_openThemeCount = kThemesOpenAtFirst;
        *g_sfxVolumeSetting = kSfxVolumeAtFirst;
        *g_musicVolumeSetting = kMusicVolumeAtFirst;
        *g_secretLevelUnlocked = 0;

        const ee::Ptr<PlayerSetup> setups(sym::g_playerSetups);
        for (uint32_t player = 0; player < kMaxPlayers; ++player)
        {
            PlayerSetup &setup = setups[player];
            setup.controllerLayout = 0;
            setup.cameraView = 0;
            setup.character = 0;
            setup.flags = kPlayerVibration;
            zge::formatString((setups + static_cast<ptrdiff_t>(player))->name, kPlayerName);
        }
        *g_currentPlayer = 0;
        *g_multiplayerPlayers = 1;

        initPads();
        initSound();
        initGameUi();
        registerSoundMenuFunctions();
        for (uint32_t part = 0; part < kPartSetUps; ++part)
            kPartSetUpTable[part]();
        countLevelGaps();
        *g_currentGameState = kNoGameState;
        return game::tail(setGameState, kGameStateIntro);
    }
    READABLE_FUNCTION(initGame, Verified);
}
