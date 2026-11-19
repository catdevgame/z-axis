// The skater's score bookkeeping: the score (Player::score's record, the
// HUD's score counter) and the trick score banked into it on landing, the
// points counter (Player::points: a career challenge's score, set by the
// setChallengeScore native, and the split-screen games' points) and the
// juice a crash costs.

#include "skater.h"

#include "aggressive/conversions.h"
#include "aggressive/game_state.h"
#include "aggressive/globals.h"
#include "aggressive/hud/hud.h"
#include "aggressive/objects.h"
#include "aggressive/tuning.h"
#include "zge/zgui/script.h"
#include "aggressive/zgui.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/route.h"
#include "ee/plain.h"
#include "zge/text/cstring.h"
#include "sce/libgcc.h"
#include "action_tricks.h"
#include "moves.h"
#include "trick_objects.h"
#include "aggressive/script/script.h"
#include "aggressive/game_start.h"
#include "aggressive/level/objectives.h"
#include "sym/aggressive/skater/score.cpp.h"

namespace aggressive::skater { uint32_t confirmVaultTrick(ee::Ptr<Player>); }
namespace aggressive::skater { uint32_t airComboTrickId(uint32_t, uint32_t); }
namespace aggressive::skater { uint32_t airFlipTrickId(uint32_t); }
namespace aggressive::skater { uint32_t airGrabTrickId(uint32_t); }
namespace aggressive::skater { uint32_t cessSlideTrickId(uint32_t); }
namespace aggressive::skater { uint32_t grindTrickId(uint32_t); }
namespace aggressive::skater { uint32_t manualTrickId(uint32_t); }
namespace aggressive::skater { uint32_t poleTrickId(uint32_t); }
namespace aggressive::skater { uint32_t stallTrickId(uint32_t); }
namespace aggressive::skater { uint32_t vaultTrickId(uint32_t); }
namespace aggressive::skater { uint32_t wallTrickId(uint32_t); }

namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kIsJizzed = sym::isJizzed;         // (player)
        constexpr uint32_t kSetJizzed = sym::setJizzed;        // (player, on)
        // (player, stat, amount) a stat's experience (experience.cpp); an
        // amount below 0 gives none.
        constexpr uint32_t kStatExperience = sym::statExperience;
        constexpr uint32_t kGetJuice = sym::juiceAmount;         // (meter, float *juice)
        constexpr uint32_t kJuiceDrains = sym::juiceDrains;      // () -> whether the level drains juice
        constexpr uint32_t kSetJuice = sym::setJuiceMeter;         // (meter, juice)
        constexpr uint32_t kPointsLocked = 0x2;             // Player::scoreFlags
        inline ee::Global<uint8_t, sym::g_gameTypeByte> g_gameTypeByte;
        constexpr uint8_t kSplitScreen = 4;

        constexpr uint32_t kRolloverScore = 999999999u;
        constexpr uint32_t kRollover = sym::rollover;         // "Rollover!"
        inline constexpr ee::Float kRolloverSeconds = 3.0f;
        // The scripts hear of every change: "skaterPointsChanged" (its hash
        // made once).
        constexpr uint32_t kPointsChangedName = sym::pointsChangedName;
        inline ee::Global<uint32_t, sym::g_hashPointsChanged> g_hashPointsChanged;
        inline ee::Global<uint32_t, sym::g_pointsChanged> g_pointsChanged;
        constexpr uint32_t kListenersScore = 2;
        inline ee::Global<uint32_t, sym::g_levelNumber> g_level;

        // Banking: the score's text in the HUD's trick line.
        constexpr uint32_t kMemset = sym::memset;           // libc (dst, byte, bytes)
        constexpr uint32_t kFloatToInt = sym::fptoui;       // libgcc __fixsfsi
        constexpr uint32_t kBankedFormat = sym::bankedFormat;     // "#RGB{17 160 11}%d" (green)

        // Scoring: Juice / Juice Lost/Level on crash.
        ee::Float juiceLostPerLevel()
        {
            return (*g_tuningCategories)[6].entries[2].value;
        }
    }

    int32_t skaterPoints(ee::Ptr<Player> player)
    {
        return player->points;
    }
    READABLE_FUNCTION(skaterPoints, Verified);

    void setSkaterPoints(ee::Ptr<Player> player, int32_t points)
    {
        player->points = points;
        if (player->scoreFlags & kPointsLocked)
            game::tail(aggressive::hud::showScore, player, points, 1);
        else if (*g_gameTypeByte != kSplitScreen)
            game::tail(aggressive::hud::showPoints, player, points, 1);
    }
    READABLE_FUNCTION(setSkaterPoints, Verified);

    int32_t juiceDrains();

    int32_t crashJuiceLoss(ee::Ptr<Player> player)
    {
        // The carton's juice comes back through a local in the original's frame.
        ee::Local<ee::Float> juice;
        *juice = 0.0f;
        if (static_cast<int32_t>(isJizzed(player)))
        {
            // Speed experience offered with -1: none (the call does nothing).
            statExperience(player, 1, ee::Float(-1.0f));
            setJizzed(player, 0);
            crashStopJizzed(player->juiceMeter);
        }
        juiceAmount(player->juiceMeter, juice);
        if (juiceDrains())
        {
            const ee::Ptr<SkaterProfile> profile = skaterProfile(player);
            computeLevel(profile);
            *juice = *juice - juiceLostPerLevel() * ee::toFloat(profile->level);
        }
        *juice = ee::max(*juice, 0.0f);
        setJuiceMeter(player->juiceMeter, *juice);
        clearPotentialJuice(player->juiceMeter);
        return 1;
    }
    READABLE_FUNCTION(crashJuiceLoss, Verified);

    int32_t trickMultiplier(ee::Ptr<Player> player)
    {
        return player->multiplier;
    }
    READABLE_FUNCTION(trickMultiplier, Verified);

    int32_t skaterScore(ee::Ptr<Player> player)
    {
        return static_cast<int32_t>(player->scoreRecord->score);
    }
    READABLE_FUNCTION(skaterScore, Verified);

    ee::Float pendingTrickScore(ee::Ptr<Player> player)
    {
        trickMultiplier(player); // (its result is not used)
        // The multiplier as an unsigned number.
        const int32_t count = player->multiplier;
        ee::Float multiplier;
        if (count >= 0)
            multiplier = ee::toFloat(count);
        else
        {
            const uint32_t u = static_cast<uint32_t>(count);
            const ee::Float half = ee::toFloat(static_cast<int32_t>((u >> 1) | (u & 1u)));
            multiplier = half + half;
        }
        return ee::max(player->pendingScore * multiplier, 0.0f);
    }
    READABLE_FUNCTION(pendingTrickScore, Verified);

    void setSkaterScore(ee::Ptr<Player> player, uint32_t score, int32_t animate)
    {
        const ee::Ptr<LevelRecord> record = player->scoreRecord;
        const uint32_t old = record->score;
        record->score = score;
        if (score != old)
        {
            // The scripts hear of a change.
            if (*g_hashPointsChanged)
            {
                *g_hashPointsChanged = 0;
                *g_pointsChanged = zge::nameHash(ee::Ptr<const char>(kPointsChangedName));
            }
            aggressive::script::broadcastEvent(*g_pointsChanged, kListenersScore);
            if (score > kRolloverScore)
            {
                // Past 999,999,999: a rollover (counted, and in the level's
                // record), "Rollover!", and the score starts again at 0.
                skaterProfile(player)->rollovers += 1;
                skaterProfile(player)->levels[*g_level].rollovers += 1;
                hud::showMessage(kRollover, kRolloverSeconds);
                setSkaterScore(player, 0, 1);
            }
        }
        // The counter shows it (also when unchanged).
        if (!(player->scoreFlags & kPointsLocked))
            game::tail(aggressive::hud::showScore, player, score, animate);
    }
    READABLE_FUNCTION_WAITS(setSkaterScore, Verified);

    namespace
    {
        void memset(ee::Address m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    void bankTrickScore(ee::Ptr<Player> player)
    {
        ee::Local<char[0xB0]> m;
        if (0.0f < player->pendingScore && player->multiplier != 0)
        {
            // "+<points>" in green on the HUD's trick line, the score up by them.
            memset(m, 0, 0x80);
            const ee::Float points = pendingTrickScore(player);
            const int32_t banked = static_cast<int32_t>(sce::libgcc::fptoui(points));
            zge::formatString(m, kBankedFormat, banked);
            const int32_t score = skaterScore(player);
            setSkaterScore(player, score + banked, 1);
            hud::setTrickText(player->hud, m);
        }
        player->unreadLineWord = 0;
        player->multiplier = 1;
        player->pendingScore = 0.0f;
    }
    READABLE_FUNCTION_WAITS(bankTrickScore, Verified);
}
namespace aggressive::skater
{
    namespace
    {
        // (player) -> the word at Player +0x8D4: the score the split-screen
        // points show (inferred).
        constexpr uint32_t kSplitScore = sym::splitScore;
        constexpr uint32_t kSetSkaterPointsAt = sym::setSkaterPoints; // setSkaterPoints, above
    }

    // updatePlayersStep: the players' frame step: the method at +0x0C of the
    // table the players' block points to (+0x04) is called (with the
    // registers as they came: it takes none); in split screen, with the
    // block's byte +0x02 set to 1, each player's points are set to its
    // split score. Returns the method's result (0 without players).
    int32_t updatePlayersStep(ee::Float seconds)
    {
        const ee::Ptr<Level> level = currentLevel();
        if (!level)
            return 0;
        const int32_t result = level->mode->update(seconds);
        if (*g_gameTypeByte == kSplitScreen && level->splitGame == 1)
        {
            for (int32_t i = 0; i < 2; ++i)
            {
                const ee::Ptr<Player> player = &level->players[i];
                setSkaterPoints(player, splitScore(player));
            }
        }
        return result;
    }
    READABLE_FUNCTION_WAITS(updatePlayersStep, Verified);

    // ---- the score flags and the split score (were the sweep's
    // isJizzed, setJizzed, storedPendingScore, splitScore, setSplitScore) ------------------
    namespace
    {
        constexpr uint32_t kJizzedFlag = 0x1; // Player::scoreFlags: the juice meter full
    }

    // isJizzed: whether the skater is jizzed (on full juice).
    uint32_t isJizzed(ee::Ptr<Player> player)
    {
        return player->scoreFlags & kJizzedFlag;
    }
    READABLE_FUNCTION(isJizzed, Verified);

    // setJizzed: jizzed on or off. The original reads and writes the
    // flags as a doubleword (the score word after them too) and returns it.
    uint64_t setJizzed(ee::Ptr<Player> player, uint32_t on)
    {
        uint64_t &flags = player->flagsAndScore;
        flags = (flags & ~uint64_t{kJizzedFlag}) | (on & kJizzedFlag);
        return flags;
    }
    READABLE_FUNCTION(setJizzed, Verified);

    // storedPendingScore: the trick score waiting to be banked, as stored (the
    // juice meter's fill reads it; pendingTrickScore works it out anew).
    ee::Float storedPendingScore(ee::Ptr<Player> player)
    {
        return player->pendingScore;
    }
    READABLE_FUNCTION(storedPendingScore, Verified);

    // splitScore / setSplitScore: the split-screen score.
    int32_t splitScore(ee::Ptr<Player> player)
    {
        return player->splitScore;
    }
    READABLE_FUNCTION(splitScore, Verified);
    void setSplitScore(ee::Ptr<Player> player, int32_t score)
    {
        player->splitScore = score;
    }
    READABLE_FUNCTION(setSplitScore, Verified);
}

// ---- the jump's spin (were the sweep's setClockwiseSpins, setCounterClockwiseSpins, clockwiseSpins,
// counterClockwiseSpins) -------------------------------------------------------------------
//
// The air states count the jump's spin in half turns, each way
// (Player::spinsClockwise, spinsCounterClockwise). Each half turn more is
// worth 25 points on the trick score, each full turn of the two together
// one more on the multiplier, and the HUD's trick list shows the spin.
namespace aggressive::skater
{
    int32_t setTrickMultiplier(ee::Ptr<Player> player, uint32_t multiplier);

    namespace
    {
        constexpr uint32_t kSetTrickMultiplier = sym::setTrickMultiplier; // (player, multiplier)
        // hud::showSpinText, for what it leaves as a result.
        inline constexpr ee::Float kPointsPerHalfTurn = 25.0f;

        // The spin one way set to `halfTurns`: scored when it grew, back to 0
        // when it did not. Whether it grew.
        int32_t setSpins(ee::Ptr<Player> player, int32_t Player::*spins, int32_t Player::*otherWay, uint32_t halfTurns)
        {
            const uint32_t had = static_cast<uint32_t>((*player).*spins);
            const int32_t grew = had < halfTurns;
            if (halfTurns == had)
                return grew;
            if (!grew)
            {
                (*player).*spins = 0;
                return grew;
            }
            const uint32_t other = static_cast<uint32_t>((*player).*otherWay);
            const int32_t more = static_cast<int32_t>(halfTurns - had);
            const int32_t turnsBefore = static_cast<int32_t>((other + had) >> 1);
            const int32_t turnsAfter = static_cast<int32_t>((other + halfTurns) >> 1);
            if (turnsAfter != turnsBefore)
            {
                const int32_t multiplier = trickMultiplier(player);
                setTrickMultiplier(player, multiplier + (turnsAfter - turnsBefore));
            }
            const ee::Float points = unsignedToFloat(static_cast<uint32_t>(more)) * kPointsPerHalfTurn;
            const ee::Float pending = storedPendingScore(player);
            setPendingScore(player, points + pending);
            (*player).*spins = static_cast<int32_t>(halfTurns);
            return aggressive::hud::showSpinText(player);
        }
    }

    int32_t setClockwiseSpins(ee::Ptr<Player> player, uint32_t halfTurns)
    {
        return setSpins(player, &Player::spinsClockwise, &Player::spinsCounterClockwise, halfTurns);
    }
    READABLE_FUNCTION_WAITS(setClockwiseSpins, Verified);

    int32_t setCounterClockwiseSpins(ee::Ptr<Player> player, uint32_t halfTurns)
    {
        return setSpins(player, &Player::spinsCounterClockwise, &Player::spinsClockwise, halfTurns);
    }
    READABLE_FUNCTION_WAITS(setCounterClockwiseSpins, Verified);

    int32_t clockwiseSpins(ee::Ptr<Player> player)
    {
        return player->spinsClockwise;
    }
    READABLE_FUNCTION(clockwiseSpins, Verified);

    int32_t counterClockwiseSpins(ee::Ptr<Player> player)
    {
        return player->spinsCounterClockwise;
    }
    READABLE_FUNCTION(counterClockwiseSpins, Verified);
}

// ---- the trick score and the line (were the sweep's showTrickScore, setupSkaterScore,
// freeSkaterScore, loseTrickScore, setPendingScore, setTrickMultiplier,
// juiceDrains, freeToAct, noTrickPending) ------------------------------------
namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kSetupSkaterHud = sym::setupSkaterHud;  // hud::setupSkaterHud (player)
        constexpr uint32_t kFreeSkaterHud = sym::freeSkaterHud;   // hud::freeSkaterHud (player)
        constexpr uint32_t kTrickTextMemset = sym::memset; // libc memset (dst, byte, bytes)
        // hud::setTrickText, for what it leaves as a result.
        // setTrickText is void: the original returns what it leaves in the result register.
        int32_t setTrickTextKeepingResult(ee::Ptr<hud::SkaterHud> hud, ee::Address text) { return ee::game<int32_t>(sym::setTrickText, hud, text); }
        // (the objects' messages: may the current action be cut)
        // The level index read as a word (with the three bytes after it, as the original does).
        inline ee::Global<int32_t, sym::g_levelNumber> g_levelIndexWord;

        // Scoring: Juice's entries (tuning.h): the juice a trick score is worth.
        constexpr uint32_t kJuiceScoreRatio = 0;      // "Juice:Score ratio"
        constexpr uint32_t kAwardIsPremultiplied = 1; // "Award is Premultiplied": the juice from the raw score
        constexpr int32_t kMaxMultiplier = 199;
        constexpr uint32_t kTuningCategories = sym::g_tuning;

        constexpr uint32_t kTrickScoreText = sym::trickScoreText; // "%d#RGB{154 28 33} - %dx": the score and its multiplier (red)
        constexpr uint32_t kPlainScoreText = sym::plainScoreText; // "%d"
        constexpr uint32_t kTextsHigh = ee::high(sym::plainScoreText);      // its address's upper half
        constexpr uint32_t kLostScoreText = sym::lostScoreText;  // "#RGB{154 28 33}%d": the score the bail lost (red)
        constexpr uint32_t kJuiceMeterOne = sym::juiceMeterOne;  // "MultiplayerUI.Overlay.PlayerOne.JuiceMeter"
        constexpr uint32_t kJuiceMeterTwo = sym::juiceMeterTwo;  // "MultiplayerUI.Overlay.PlayerTwo.JuiceMeter"
        constexpr uint32_t kJuiceMeter = sym::juiceMeter;     // "InGameUI.Overlay.JuiceMeter"
        inline constexpr ee::Float kDefaultCartonSize = 50.0f, kStartingJuice = 0.5f; // a level starts half full
        constexpr uint64_t kScoreFlagsCleared = ~uint64_t{3}; // jizzed and the points lock off
        constexpr uint32_t kTextBytes = 0x80;
    }

    int32_t showTrickScore(ee::Ptr<Player> player)
    {
        ee::Local<char[0xB0]> text;
        memset(text, 0, kTextBytes);
        const int32_t multiplier = trickMultiplier(player);
        if (static_cast<uint32_t>(multiplier) >= 2)
        {
            const int32_t points = static_cast<int32_t>(sce::libgcc::fptoui(storedPendingScore(player)));
            zge::formatString(text, kTrickScoreText, points, multiplier);
        }
        else if (player->pendingScore <= ee::Float(0.0f))
            text[0] = 0;
        else
        {
            const int32_t points = static_cast<int32_t>(sce::libgcc::fptoui(storedPendingScore(player)));
            zge::formatString(text, kPlainScoreText, points);
        }
        return setTrickTextKeepingResult(player->hud, text);
    }
    READABLE_FUNCTION_WAITS(showTrickScore, Verified);

    int32_t loseTrickScore(ee::Ptr<Player> player)
    {
        ee::Local<char[0xB0]> text;
        if (ee::Float(0.0f) < player->pendingScore)
        {
            // The score the line was worth, shown in red.
            if (player->multiplier != 0)
            {
                memset(text, 0, kTextBytes);
                const ee::Float score = pendingTrickScore(player);
                const int32_t points = static_cast<int32_t>(sce::libgcc::fptoui(score));
                zge::formatString(text, kLostScoreText, points);
                hud::setTrickText(player->hud, text);
            }
        }
        player->unreadLineWord = 0;
        player->multiplier = 1;
        player->pendingScore = 0.0f;
        return 1;
    }
    READABLE_FUNCTION_WAITS(loseTrickScore, Verified);

    int32_t setPendingScore(ee::Ptr<Player> player, ee::Float score)
    {
        player->pendingScore = score;
        // The juice the line would add: from the raw score or the multiplied one.
        ee::Float juiceScore;
        if (!(tuningValue(kTuningScoringJuice, kAwardIsPremultiplied) == ee::Float(0.0f)))
            juiceScore = storedPendingScore(player);
        else
            juiceScore = pendingTrickScore(player);
        setPotentialJuice(player->juiceMeter, juiceScore * tuningValue(kTuningScoringJuice, kJuiceScoreRatio));
        return game::tail(aggressive::skater::showTrickScore, player);
    }
    READABLE_FUNCTION_WAITS(setPendingScore, Verified);

    int32_t setTrickMultiplier(ee::Ptr<Player> player, uint32_t multiplier)
    {
        player->multiplier = multiplier <= kMaxMultiplier ? static_cast<int32_t>(multiplier) : kMaxMultiplier;
        if (tuningValue(kTuningScoringJuice, kAwardIsPremultiplied) == ee::Float(0.0f))
        {
            const ee::Float juiceScore = pendingTrickScore(player);
            setPotentialJuice(player->juiceMeter, juiceScore * tuningValue(kTuningScoringJuice, kJuiceScoreRatio));
        }
        return game::tail(aggressive::skater::showTrickScore, player);
    }
    READABLE_FUNCTION_WAITS(setTrickMultiplier, Verified);

    int32_t juiceDrains()
    {
        // Level 7 never drains; levels 10 .. 13 only in Free Skate and the
        // Timed Run; the rest always.
        const int32_t level = *g_levelIndexWord;
        if (level == 7)
            return 0;
        if (static_cast<uint32_t>(level) >= 10 && static_cast<uint32_t>(level) < 14)
            return static_cast<uint32_t>(static_cast<int32_t>(*g_gameType) - 2) < 2u;
        return 1;
    }
    READABLE_FUNCTION(juiceDrains, Verified);

    uint32_t freeToAct(ee::Ptr<Player> player)
    {
        ee::Local<uint32_t> free;
        // Free with no current object; else as the object answers.
        *free = 0;
        const ee::Ptr<GameObject> current = skaterCurrentObject(player);
        *free = current == 0;
        if (current)
            sendObjectMessage(ee::Ptr<GameObject>(current), *g_msgAreYouFinished, player, free);
        return *free;
    }
    READABLE_FUNCTION_WAITS(freeToAct, Verified);

    int32_t noTrickPending(ee::Ptr<Player> player)
    {
        // On the ground (or standing) with no trick score waiting and no
        // object, or only a kind-1 one.
        int32_t none = 0;
        if (static_cast<uint8_t>(player->state) < static_cast<uint8_t>(State::Air))
        {
            if (storedPendingScore(player) <= ee::Float(0.0f))
            {
                const ee::Ptr<GameObject> current = skaterCurrentObject(player);
                if (!current)
                    none = 1;
                else
                    none = static_cast<uint32_t>(gameObjectKind(ee::Ptr<GameObject>(current))) == 1;
            }
        }
        return none;
    }
    READABLE_FUNCTION(noTrickPending, Verified);

    void setupSkaterScore(ee::Ptr<Player> player)
    {
        ee::Local<ee::Float> size;
        *size = kDefaultCartonSize;
        // The HUD's widgets to the points counter cleared (0x24 bytes), the HUD found.
        zge::memoryFill(&player->hud, 0, offsetof(Player, points) + 4 - offsetof(Player, hud));
        hud::setupSkaterHud(player);
        const int32_t level = *g_levelIndexWord;
        player->pendingScore = 0.0f;
        player->multiplier = 1;
        player->splitScore = 0;
        player->unreadLineWord = 0;
        // The score is this level's record's.
        const ee::Ptr<LevelRecord> record = &skaterProfile(player)->levels[level];
        player->scoreRecord = record;
        hud::showScore(player, static_cast<int32_t>(record->score), 1);

        // The juice carton: the player's own in split screen (none for a third).
        uint32_t meterPath = 0;
        if (*g_gameType == GameType::SplitScreen)
        {
            if (player->number == 0)
                meterPath = kJuiceMeterOne;
            else if (player->number == 1)
                meterPath = kJuiceMeterTwo;
        }
        else
            meterPath = kJuiceMeter;
        if (meterPath)
            player->juiceMeter = zgui::findNode(ee::Ptr<zgui::Node>(*g_uiRoot), ee::Ptr<const char>(meterPath), uint64_t{1});
        const ee::Ptr<zgui::Node> meter = player->juiceMeter;
        player->flagsAndScore &= kScoreFlagsCleared;
        // Half full: half the carton's size (50 when it names none).
        if (!cartonSize(meter, size))
            return;
        setJuiceMeter(player->juiceMeter, *size * kStartingJuice);
    }
    READABLE_FUNCTION_WAITS(setupSkaterScore, Verified);

    int32_t freeSkaterScore(ee::Ptr<Player> player)
    {
        hud::freeSkaterHud(player);
        player->juiceMeter = 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(freeSkaterScore, Verified);

    namespace
    {
        inline ee::Global<int32_t, sym::g_competitionScoring> g_competitionScoring; // g_competitionScoring; its name is a low-confidence guess (readable_names.tsv)
    }

    // (player): when its current object (skaterCurrentObject) is a vault (kind 7), confirmVaultTrick of the
    // player.
    uint32_t currentVaultObjectValue(ee::Ptr<Player> player)
    {
        const ee::Ptr<GameObject> skaterCurrentObjectResult = skaterCurrentObject(player);
        int32_t confirmVaultTrick = 0;
        if (skaterCurrentObjectResult)
        {
            const uint64_t gameObjectKindResult = gameObjectKind(skaterCurrentObjectResult);
            confirmVaultTrick = 0;
            if (gameObjectKindResult == 0x7ull)
            {
                confirmVaultTrick = aggressive::skater::confirmVaultTrick(player);
                return confirmVaultTrick;
            }
            return confirmVaultTrick;
        }
        else
            confirmVaultTrick = 0;
        return confirmVaultTrick;
    }
    READABLE_FUNCTION(currentVaultObjectValue, Draft);

    // (key) -> key >> 27.
    uint32_t trickKeyClass(uint32_t key)
    {
        return key >> 27;
    }
    READABLE_FUNCTION(trickKeyClass, Draft);

    void gameHookNoopB()
    {
    }
    READABLE_FUNCTION(gameHookNoopB, Draft);
}

// ---- the line's counters and its tricks (were the sweep's resetLineCounters,
// countSameTrick, countSimilarTrick, timesInLine, similarInLine) -------------------------------
namespace aggressive::skater
{
    namespace
    {
        constexpr uint32_t kCountSameTrick = sym::countSameTrick;     // countSameTrick below, as the visit
        constexpr uint32_t kCountSimilarTrick = sym::countSimilarTrick;  // countSimilarTrick below
        constexpr uint32_t kTrickGroup = 0xF8000000u, kTrickFlags = 0x07FFFFFFu;

        // A visit's context: the trick id looked for and the tricks matched.
        struct TrickCount
        {
            uint32_t uid;
            int32_t count;
        };
    }

    // resetLineCounters: the line's counters back to 0: the balance meters, the
    // short and hold times, the grind, manual, wall ride and air times and
    // the jump's spin in degrees.
    void resetLineCounters(ee::Ptr<Player> player)
    {
        player->balance[1] = 0.0f;
        player->grindTime = 0.0f;
        player->shortCount = 0;
        player->holdTime = 0.0f;
        player->manualTime = 0.0f;
        player->wallRideTime = 0.0f;
        player->totalAirTime = 0.0f;
        player->spinDegreesCounterClockwise = 0.0f;
        player->spinDegreesClockwise = 0.0f;
        player->balance[0] = 0.0f;
    }
    READABLE_FUNCTION(resetLineCounters, Verified);

    // countSameTrick: (a visit) one more when the trick is the one looked
    // for. Always 1 (go on).
    uint32_t countSameTrick(ee::Ptr<GameObject> trick, ee::Ptr<TrickCount> count)
    {
        if (trickKeyOf(trick) == count->uid)
            ++count->count;
        return 1;
    }
    READABLE_FUNCTION(countSameTrick, Verified);

    // countSimilarTrick: (a visit) one more when the trick is of the same group
    // and shares a flag with the one looked for, or has the same flags.
    // Always 1 (go on).
    uint32_t countSimilarTrick(ee::Ptr<GameObject> trick, ee::Ptr<TrickCount> count)
    {
        const uint32_t uid = trickKeyOf(trick);
        const uint32_t wanted = count->uid;
        const uint32_t wantedFlags = wanted & kTrickFlags, flags = uid & kTrickFlags;
        if ((uid & kTrickGroup) == (wanted & kTrickGroup))
        {
            if ((flags & wantedFlags) == 0 && flags != wantedFlags)
                return 1;
            ++count->count;
        }
        return 1;
    }
    READABLE_FUNCTION(countSimilarTrick, Verified);

    namespace
    {
        // What endTrickChain adds up over the line's objects (awardQueuedTrick).
        struct LineTotals
        {
            ee::Ptr<Player> player; // +0x00
            int32_t tricks;       // +0x04
            int32_t airTricks;    // +0x08 air tricks (kObjectAirTrick)
            uint32_t hasGrind;    // +0x0C
            uint32_t hasManual;   // +0x10
            uint32_t hasWallRide; // +0x14
            int32_t multiplier;   // +0x18 from 1: each object's bonus, the spins, a trick more than one
            int32_t score;        // +0x1C each object's points, the spins' 25 a half turn
            uint32_t _20;
            uint32_t bestBonus;   // +0x24
            ee::Float bestScore;  // +0x28 the best object's points
        };
        static_assert(sizeof(LineTotals) == 44);

        constexpr uint32_t kAwardQueuedTrick = sym::awardQueuedTrick;
        enum BestScore : uint32_t
        {
            kBestTrick = 5,
            kBestLine = 6,
        };
    }

    // awardQueuedTrick: a slot's walk (forEachInSlot, the line's objects): an
    // object of a scoring kind (2 to 10) adds to the totals. Returns 1, to
    // go on.
    uint32_t awardQueuedTrick(ee::Ptr<GameObject> object, ee::Ptr<LineTotals> totals)
    {
        ee::Local<int32_t> points;
        ee::Local<uint32_t> result;
        ee::Local<uint32_t> bonus;
        *points = 0;
        *bonus = 0;
        *result = 0;
        const uint64_t kind = gameObjectKind(object);
        if (kind >= 11)
            return 1;
        if (kind < 2)
            return 1;
        sendObjectMessage(object, *g_msgGetScore, points, result);
        sendObjectMessage(object, *g_msgGetMultiplierBonus, bonus, result);
        if (*points == 0)
        {
            if (*bonus == 0)
                return 1;
        }

        // Its kind's mark. The other kinds skip the original's join.
        if (kind == kObjectAirTrick)
            totals->airTricks += 1;
        else if (kind == kObjectTrick)
            totals->hasGrind = 1;
        else
        {
            if (kind == kObjectManualTrick)
                totals->hasManual = 1;
            else if (kind == kObjectWallRideTrick)
                totals->hasWallRide = 1;
        }

        totals->tricks += 1;
        totals->score += *points;
        totals->multiplier += static_cast<int32_t>(*bonus);
        const ee::Float scored = unsignedToFloat(static_cast<uint32_t>(*points));
        if (totals->bestScore < scored)
            totals->bestScore = unsignedToFloat(static_cast<uint32_t>(*points));
        if (totals->bestBonus < *bonus)
            totals->bestBonus = *bonus;
        return 1;
    }
    READABLE_FUNCTION_WAITS(awardQueuedTrick, Verified);

    // endTrickChain: the line ends: the objects of the current slot and the
    // spins add up to a score and a multiplier (the spins' half turns, one
    // for each full turn, and one for each trick past the first; 1 at
    // least), the stats get their experience, the records and the split
    // screen's best their score; the score is banked and the line cleared.
    int32_t endTrickChain(ee::Ptr<Player> player)
    {
        ee::Local<LineTotals> totals;

        const int32_t clockwise = clockwiseSpins(player);
        const int32_t spins = clockwise + counterClockwiseSpins(player);
        zge::memoryFill(totals, 0, sizeof(LineTotals));
        totals->player = player;
        totals->multiplier = 1;
        aggressive::level::objectiveCrash(player, 1);
        forEachInSlot(player->currentObjectSlot, kAwardQueuedTrick, totals);

        const auto experience = [&](uint32_t stat, ee::Float amount) {
            statExperience(player, stat, amount);
        };

        if (spins != 0)
        {
            const uint32_t fullTurns = static_cast<uint32_t>(spins) >> 1;
            const ee::Float spinPoints = unsignedToFloat(static_cast<uint32_t>(spins));
            const ee::Float byTurns = spinPoints * kPointsPerHalfTurn;
            const ee::Float points = unsignedToFloat(static_cast<uint32_t>(totals->score));
            totals->score = static_cast<int32_t>(sce::libgcc::fptoui(points + byTurns));
            totals->multiplier += static_cast<int32_t>(fullTurns);
            const ee::Float turns = unsignedToFloat(fullTurns);
            experience(kStatSpin, turns);
        }
        if (totals->airTricks != 0)
        {
            const ee::Float air = unsignedToFloat(static_cast<uint32_t>(totals->airTricks));
            experience(kStatJump, air);
        }
        if (totals->hasGrind != 0)
            experience(kStatGrind, player->grindTime);
        if (totals->hasManual != 0)
            experience(kStatManual, player->manualTime);
        if (totals->hasWallRide != 0)
            experience(kStatWallRide, player->wallRideTime);
        if (player->fakie != 0)
        {
            const ee::Float tricks = unsignedToFloat(static_cast<uint32_t>(totals->tricks));
            experience(kStatFakie, tricks);
        }
        if (static_cast<uint32_t>(totals->tricks) >= 2)
            totals->multiplier += totals->tricks - 1;
        if (totals->multiplier == 0)
            totals->multiplier = 1;
        const uint32_t score = static_cast<uint32_t>(totals->score) * static_cast<uint32_t>(totals->multiplier);
        skaterScore(player);
        if (isJizzed(player) != 0 && score != 0)
        {
            const ee::Float multiplier = unsignedToFloat(static_cast<uint32_t>(totals->multiplier));
            experience(kStatSpeed, multiplier);
        }
        if (static_cast<uint32_t>(splitScore(player)) < score)
            setSplitScore(player, static_cast<int32_t>(score));
        if (*g_gameType == GameType::Career)
        {
            const ee::Ptr<SkaterProfile> first = skaterProfile(&currentLevel()->players[0]);
            if (static_cast<uint32_t>(first->bestTrickString) < score)
                first->bestTrickString = static_cast<int32_t>(score);
        }
        const ee::Ptr<char> name = player->name;
        const uint32_t bestTrick = sce::libgcc::fptoui(totals->bestScore);
        recordBestScore(kBestTrick, *g_level, bestTrick, name);
        recordBestScore(kBestLine, *g_level, score, name);
        awardPotentialJuice(player->juiceMeter);
        bankTrickScore(player);
        emptySlot(player->currentObjectSlot);
        player->hasCurrentObject = 0;
        setClockwiseSpins(player, 0);
        setCounterClockwiseSpins(player, 0);
        return hud::clearTrickList(player);
    }
    READABLE_FUNCTION_WAITS(endTrickChain, Verified);

    namespace
    {
        // The tricks of the line (the current object's slot) a visit counts.
        int32_t countInLine(ee::Ptr<Player> player, uint32_t uid, uint32_t visit)
        {
            ee::Local<TrickCount> count;
            zge::memoryFill(count, 0, sizeof(TrickCount));
            const uint32_t slot = player->currentObjectSlot;
            count->uid = uid;
            count->count = 0;
            aggressive::forEachInSlotFromLast(slot, visit, count);
            return count->count;
        }
    }

    // timesInLine / similarInLine: how many tricks of the line are the trick
    // `uid`; how many are like it (countSimilarTrick).
    uint32_t timesInLine(ee::Ptr<Player> player, uint32_t uid)
    {
        return static_cast<uint32_t>(countInLine(player, uid, kCountSameTrick));
    }
    READABLE_FUNCTION_WAITS(timesInLine, Verified);

    uint32_t similarInLine(ee::Ptr<Player> player, uint32_t uid)
    {
        return static_cast<uint32_t>(countInLine(player, uid, kCountSimilarTrick));
    }
    READABLE_FUNCTION_WAITS(similarInLine, Verified);
}

// ---- the line lost, reset and keyed; the bail entry; the split screen's
// final score (were the sweep's endCombo .. showFinalScore) ----------------

namespace aggressive::skater
{
    namespace
    {
        // Player::objectSlots, named for the states that take from them first
        // (selectStateObject's order).
        constexpr uint32_t kActionSlot = 0, kManualSlot = 1, kGrindSlot = 2, kGroundSlot = 3;
        constexpr uint32_t kPointsLockedFlag = 0x2; // Player::scoreFlags: the score shows over the points

        // An air trick's state block starts with its grab and its flip
        // (air_tricks.cpp's AirTrick); the other kinds' with their type.
        struct AirTrickIds
        {
            uint32_t grab; // +0x00
            uint32_t flip; // +0x04
        };

        // The red bail entry of the trick list (" #RGB{154 28 33}Bail").
        constexpr uint32_t kBailText = sym::g_textRgbBail;
        constexpr uint32_t kNoMultiplierBonus = 0;
        // The trick text emptied ("").
        constexpr uint32_t kEmptyTrickText = sym::g_emptyTrickText;

        // hud::resetTrickList: the trick list cleared at once.

    }

    // endCombo: the line is lost (a crash): the current object let go, the
    // line's objects added up as on landing (awardQueuedTrick) only to take
    // experience back: Jump loses one for each air trick, Fakie one for
    // each trick while riding fakie. The trick score is lost (shown in
    // red), the spins, the trick list and the line's counters cleared.
    // Returns whether the five object slots were all emptied, emptied in
    // turn while each one is (the action's, the manual's, the grind's, the
    // ground's, the current object's).
    int32_t endCombo(ee::Ptr<Player> player)
    {
        ee::Local<LineTotals> totals;
        player->hasCurrentObject = 0;
        zge::memoryFill(totals, 0, sizeof(LineTotals));
        totals->player = player;
        totals->multiplier = 1;
        forEachInSlot(player->currentObjectSlot, kAwardQueuedTrick, totals);
        if (totals->airTricks != 0)
            statExperience(player, kStatJump, -unsignedToFloat(static_cast<uint32_t>(totals->airTricks)));
        if (totals->tricks != 0 && player->fakie != 0)
            statExperience(player, kStatFakie, -unsignedToFloat(static_cast<uint32_t>(totals->tricks)));

        aggressive::skater::loseTrickScore(player);
        setClockwiseSpins(player, 0);
        setCounterClockwiseSpins(player, 0);
        hud::clearTrickList(player);
        resetLineCounters(player);
        return emptySlot(player->objectSlots[kActionSlot]) && emptySlot(player->objectSlots[kManualSlot]) &&
               emptySlot(player->objectSlots[kGrindSlot]) && emptySlot(player->objectSlots[kGroundSlot]) &&
               emptySlot(player->currentObjectSlot);
    }
    READABLE_FUNCTION_WAITS(endCombo, Verified);

    // resetPlayerTricks: everything the skater has going in tricks dropped:
    // the current object hears "TrickFinished", the four object slots are
    // emptied, the line banked (endTrickChain) and its counters reset, the
    // trick text emptied and the trick list cleared at once. Returns 1.
    uint32_t resetPlayerTricks(ee::Ptr<Player> player)
    {
        const ee::Ptr<GameObject> current = skaterCurrentObject(player);
        if (current)
        {
            sendObjectMessage(ee::Ptr<GameObject>(current), *g_msgTrickFinished, player, 0);
            player->hasCurrentObject = 0;
        }
        emptySlot(player->objectSlots[kActionSlot]);
        emptySlot(player->objectSlots[kManualSlot]);
        emptySlot(player->objectSlots[kGrindSlot]);
        emptySlot(player->objectSlots[kGroundSlot]);
        aggressive::skater::endTrickChain(player);
        resetLineCounters(player);
        hud::setTrickText(player->hud, kEmptyTrickText);
        aggressive::hud::resetTrickList(player);
        return 1;
    }
    READABLE_FUNCTION_WAITS(resetPlayerTricks, Verified);

    // trickKeyOf: a trick object's key (countSameTrick and countSimilarTrick
    // compare them, the scripts read it): its kind's maker of ids given the
    // trick's type (the state block's first word). An air trick's is its
    // grab's, its flip's, or with both the two ORed as they are
    // (airComboTrickId); Skitch has one id. A text object and any kind
    // outside the tricks (below 2, above 11) have 0.
    uint32_t trickKeyOf(ee::Ptr<GameObject> object)
    {
        const auto type = [&] { return *ee::Ptr<uint32_t>(gameObjectState(object)); };
        switch (gameObjectKind(object))
        {
        case kObjectTrick:
            return aggressive::skater::grindTrickId(type());
        case kObjectAirTrick:
        {
            const ee::Ptr<AirTrickIds> air(gameObjectState(object));
            if (air->grab == 0)
                return aggressive::skater::airFlipTrickId(air->flip);
            if (air->flip == 0)
                return aggressive::skater::airGrabTrickId(air->grab);
            return aggressive::skater::airComboTrickId(air->flip, air->grab);
        }
        case kObjectManualTrick:
            return aggressive::skater::manualTrickId(type());
        case kObjectWallRideTrick:
            return aggressive::skater::wallTrickId(type());
        case kObjectVaultTrick:
            return aggressive::skater::vaultTrickId(type());
        case kObjectPoleTrick:
            return aggressive::skater::poleTrickId(type());
        case kObjectCessSlide:
            return aggressive::skater::cessSlideTrickId(type());
        case kObjectStallTrick:
            return aggressive::skater::stallTrickId(type());
        case kObjectSkitchTrick:
            return skitchTrickId();
        default: // a text object, or no trick
            return 0;
        }
    }
    READABLE_FUNCTION(trickKeyOf, Verified);

    // playerComboState / setPlayerComboState: Player +0x8D0, a count the
    // line ending clears (loseTrickScore, bankTrickScore). The action
    // button in a quarter pipe's or a spine's air adds one (actionRequest):
    // the trick's juice is offered again as on any score change, and a
    // count one up from before puts the red "Bail" entry in the trick list;
    // the trick score is shown again.
    int32_t playerComboState(ee::Ptr<Player> player)
    {
        return static_cast<int32_t>(player->unreadLineWord);
    }
    READABLE_FUNCTION(playerComboState, Verified);

    int32_t setPlayerComboState(ee::Ptr<Player> player, uint32_t count)
    {
        const uint32_t before = player->unreadLineWord;
        player->unreadLineWord = count;
        // The juice the line would add: from the raw score or the multiplied one.
        ee::Float juiceScore;
        if (!(tuningValue(kTuningScoringJuice, kAwardIsPremultiplied) == ee::Float(0.0f)))
            juiceScore = storedPendingScore(player);
        else
            juiceScore = pendingTrickScore(player);
        setPotentialJuice(player->juiceMeter, juiceScore * tuningValue(kTuningScoringJuice, kJuiceScoreRatio));
        if (before == count - 1)
        {
            const ee::Ptr<GameObject> bail = newTextObject(kBailText, kNoMultiplierBonus, ee::Float(0.0f));
            hud::addCompletedTrick(player, bail);
            aggressive::hud::showSpinText(player);
            releaseGameObject(bail);
        }
        return game::tail(aggressive::skater::showTrickScore, player);
    }
    READABLE_FUNCTION_WAITS(setPlayerComboState, Verified);

    // freezeMultiplayerScore: a split-screen game's end (the pause's
    // multiplayer end): the points locked under the score (setSkaterPoints
    // shows the score from now on); outside split screen the points panel
    // is hidden; the points shown as the score, without counting up.
    void freezeMultiplayerScore(ee::Ptr<Player> player)
    {
        player->flagsAndScore |= kPointsLockedFlag;
        if (*g_gameType != GameType::SplitScreen)
            hud::hidePointsPanel(player);
        game::tail(aggressive::hud::showScore, player, static_cast<uint32_t>(player->points), 0);
    }
    READABLE_FUNCTION_WAITS(freezeMultiplayerScore, Verified);

    // showFinalScore: the points unlocked and the score shown, without
    // counting up; outside split screen the points panel shown with the
    // points (`showPoints` set) or hidden. In split screen it returns the
    // game type (4).
    int32_t showFinalScore(ee::Ptr<Player> player, uint32_t showPoints)
    {
        player->flagsAndScore &= ~uint64_t{kPointsLockedFlag};
        hud::showScore(player, skaterScore(player), 0);
        if (*g_gameType == GameType::SplitScreen)
            return static_cast<int32_t>(GameType::SplitScreen);
        if (showPoints)
        {
            hud::showPointsPanel();
            return game::tail(hud::showPoints, player, player->points, 0);
        }
        return game::tail(hud::hidePointsPanel, player);
    }
    READABLE_FUNCTION_WAITS(showFinalScore, Verified);
}
