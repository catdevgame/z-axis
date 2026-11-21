// Experience points (round 8): each of the skater's seven stats (Jump,
// Speed, Spin, Grind, Manual, Fakie, Wallride) has a level and the points
// toward the next one (Player +0x638, 12 bytes a stat). Tricks award them
// (completeChallenge, awardObjective: "Jump XP +25" on the HUD); 100, 500, 2,000,
// 10,000, 25,000, 55,000 and 100,000 points make the levels 1 to 7, and a
// level up shows "<stat> Level Up" and plays its effect and sound. Recorded
// over games/aggressive/tools/routes.py's `challenge` and the Tutorial moves.

#include "skater.h"

#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/angle.h"
#include "zge/text/cstring.h"
#include "moves.h"
#include "aggressive/sound/events.h"
#include "aggressive/hud/hud.h"
#include "aggressive/fx/library.h"
#include "sym/aggressive/skater/experience.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        constexpr uint8_t kMaxLevel = 7;

        // The points each level wants, by the current level.
        inline ee::Global<uint32_t[8], sym::g_levelPoints> g_levelPoints;
        // The stats' names: "Jump", "Speed", "Spin", "Grind", "Manual", "Fakie", "Wallride".
        constexpr uint32_t kStatNames[kStatCount] = {sym::g_textJump2, sym::g_textSpeed2, sym::g_textSpin, sym::g_textGrind2,
                                                     sym::g_textManual, sym::g_textFakie, sym::g_textWallride};
        constexpr uint32_t kXpFormat = sym::xpFormat;      // "%s XP"
        constexpr uint32_t kLevelUpFormat = sym::levelUpFormat; // "%s Level Up"

        constexpr uint32_t kLevelUpSound = 0x4B;
        inline constexpr ee::Float kLevelUpSeconds = 3.5f;

        inline ee::Global<uint8_t, sym::g_gameTypeByte> g_gameType;  // 2 .. 4: games without experience (inferred)
        inline ee::Global<uint32_t, sym::g_levelNumber> g_level;
        constexpr uint32_t kLevelQuietLevelUp = 7;           // the Museum (inferred): a level up only counts
    }

    // computeLevel: a profile's level: its stats' totals, and three a
    // level-up.
    void computeLevel(ee::Ptr<SkaterProfile> profile)
    {
        const uint32_t levelUps = profile->levelUps;
        profile->statTotal = 0;
        for (uint32_t stat = 0;;)
        {
            profile->statTotal = static_cast<uint8_t>(profile->statTotal + profile->stats[stat].total);
            if (++stat >= kStatCount)
                break;
        }
        profile->level = static_cast<uint8_t>(profile->statTotal + levelUps * 3);
    }
    READABLE_FUNCTION(computeLevel, Verified);

    void levelUpEffect(ee::Ptr<Player> player);

    // awardExperience: `points` experience points for a stat (none in game
    // types 2 to 4, none past level 7): "<stat> XP" on the HUD, the points
    // added (not below 0); over the level's points a level up (shown,
    // with its effect, attributes and sound, but in level 7).
    void awardExperience(ee::Ptr<Player> player, uint32_t stat, int32_t points)
    {
        if (static_cast<uint32_t>(*g_gameType - 2) < 3)
            return;
        if (points <= 0)
            return;
        stat &= 0xFFu;
        StatRecord &record = skaterProfile(player)->stats[stat];
        if (record.level >= kMaxLevel)
            return;

        // An unknown stat leaves the name as the stack had it.
        ee::Local<uint8_t[0x40]> name;
        ee::Local<uint8_t[0xA0]> text;
        if (stat < kStatCount)
            zge::stringCopy(name, kStatNames[stat]);
        zge::formatString(text.address(), kXpFormat, name.address());
        aggressive::hud::showXpReward(player, text, ee::toInt(ee::toFloat(points)));

        const int32_t sum = static_cast<int32_t>(static_cast<uint32_t>(record.points) + static_cast<uint32_t>(points));
        record.points = sum >= 0 ? sum : 0;
        const uint32_t needed = (*g_levelPoints)[record.level];
        if (!(needed < static_cast<uint32_t>(record.points)))
            return;

        // Level up.
        record.points = static_cast<int32_t>(static_cast<uint32_t>(record.points) - needed);
        ++record.level;
        record.total = static_cast<uint8_t>(record.level + record.bonus);
        record.totalF = ee::toFloat(static_cast<int32_t>(record.total));
        // In level 7 nothing more: no message, effect, attributes or sound.
        if (*g_level == kLevelQuietLevelUp)
            return;
        zge::formatString(text.address(), kLevelUpFormat, name.address());
        aggressive::hud::showMessage(text, kLevelUpSeconds);
        levelUpEffect(player);
        aggressive::fx::startLevelUpFlash(player);
        aggressive::soundEvent(kLevelUpSound, 0, 0, 0, 0);
    }
    READABLE_FUNCTION(awardExperience, Verified);

    namespace
    {
        // Skater tuning: the XP a trick of each stat is worth.
        constexpr uint32_t kSkater = 4;
        constexpr uint32_t kStatXpEntry[kStatCount] = {0 /* Jump Trick XP */, 2 /* Top Speed XP */, 6 /* Spin XP */,
                                                       11 /* Grind XP */, 8 /* Manual XP */, 16 /* Fakie XP */,
                                                       18 /* Wallride XP */};
        // Per stat: the factor for a positive amount and for none (1 and 0.5 for all).
        struct XpFactors
        {
            ee::Float positive;
            ee::Float zero;
        };
        // One a stat (any stat number indexes it, as the original does).
        constexpr ee::Ptr<XpFactors> kXpFactors{sym::xpFactors};
        constexpr uint32_t kLevelsWithExperience = 10; // the level index below which tricks give experience
    }

    // statExperience: a trick's experience for a stat: the Skater tuning's XP
    // for it, times the factor for `amount` (1 above 0, 0.5 at 0), the
    // skater's multiplier and `amount`; none in game types 2 and 3, in the
    // levels past the careers', for an amount below 0 or a result under 1e-5.
    void statExperience(ee::Ptr<Player> player, uint32_t stat, ee::Float amount)
    {
        if (static_cast<uint32_t>(*g_gameType - 2) < 2)
            return;
        if (!(*g_level < kLevelsWithExperience))
            return;
        if (amount < 0.0f)
            return;
        stat &= 0xFFu;
        ee::Float xp = 0.0f;
        if (stat < kStatCount)
        {
            xp = (*g_tuningCategories)[kSkater].entries[kStatXpEntry[stat]].value;
        }
        const XpFactors &factors = kXpFactors[stat];
        ee::Float factor = factors.positive;
        if (!(0.0f < amount))
            factor = factors.zero;
        xp = xp * factor;
        xp = xp * ee::toFloat(static_cast<int32_t>(skaterProfile(player)->experienceMultiplier));
        xp = xp * amount;
        const bool none = ee::abs(xp) < zge::kMathEpsilon;
        if (none)
            return;
        awardExperience(player, stat, ee::toInt(xp));
    }
    READABLE_FUNCTION(statExperience, Verified);

    // ---- the attribute points and the skater's level (were the sweep's) ----
    namespace
    {
        constexpr uint32_t kSplitScreenCarton = 100, kCarton = 50; // the juice carton's size at level 0
        constexpr uint8_t kGameTypeSplitScreen = 4;
    }

    // levelUpEffect: the skater's level again, and the juice carton's size
    // with it (two more a level).
    void levelUpEffect(ee::Ptr<Player> player)
    {
        ee::Local<ee::Float> size2;
        const ee::Ptr<SkaterProfile> profile = skaterProfile(player);
        const uint32_t startSize = *g_gameType == kGameTypeSplitScreen ? kSplitScreenCarton : kCarton;
        computeLevel(profile);
        const uint32_t size = startSize + profile->level * 2;
        const ee::Ptr<zge::zgui::Node> carton = player->juiceMeter;
        *size2 = ee::toFloat(static_cast<int32_t>(size));
        setCartonSize(carton, size2);
    }
    READABLE_FUNCTION_WAITS(levelUpEffect, Verified);

    // levelUpAttribute: an attribute point spent on a stat: its total up, "<stat>
    // Level Up" on the HUD, the skater's level, the flash.
    void levelUpAttribute(ee::Ptr<Player> player, uint32_t statArgument)
    {
        ee::Local<uint8_t[0x40]> name;
        ee::Local<uint8_t[0x70]> text;
        const uint32_t stat = statArgument & 0xFFu;
        StatRecord &record = skaterProfile(player)->stats[stat];
        ++record.bonus;
        record.total = static_cast<uint8_t>(record.level + record.bonus);
        record.totalF = ee::toFloat(static_cast<int32_t>(record.total));
        if (stat < kStatCount)
            zge::stringCopy(name, kStatNames[stat]);
        zge::formatString(text.address(), kLevelUpFormat, name.address());
        aggressive::hud::showMessage(text, kLevelUpSeconds);
        levelUpEffect(player);
        aggressive::fx::startLevelUpFlash(player);
    }
    READABLE_FUNCTION_WAITS(levelUpAttribute, Verified);

    // levelUpBonus: a level-up bought outright.
    void levelUpBonus(ee::Ptr<Player> player)
    {
        ++skaterProfile(player)->levelUps;
        game::tail(aggressive::skater::levelUpEffect, player);
    }
    READABLE_FUNCTION_WAITS(levelUpBonus, Verified);

    // levelPointsFor: the points the next level of a stat wants, from `level`.
    uint32_t levelPointsFor(uint32_t level)
    {
        return (*g_levelPoints)[level];
    }
    READABLE_FUNCTION(levelPointsFor, Verified);

    // ---- the stats' effects (was the sweep's skaterStat) ----------------------
    namespace
    {
        // The tuning entries skaterStat reads (their names in the ELF's table).
        enum PhysicsEntry : uint32_t
        {
            kPhysicsJump = 2,
            kPhysicsMaxSpeed = 3,
            kPhysicsAirRotate = 33,
            kPhysicsFakieMult = 37,
        };
        enum SkaterEntry : uint32_t
        {
            kSkaterJumpSpeedAdd = 1,
            kSkaterTopSpeedSpeedAdd = 3,
            kSkaterTopSpeedJuiceAdd = 4,
            kSkaterTopSpeedJuiceSpeedAdd = 5,
            kSkaterSpinAdd = 7,
            kSkaterManualBalanceAdd = 9,
            kSkaterManualBalanceInitAdd = 10,
            kSkaterGrindGrindAdd = 12,
            kSkaterGrindGrindInitAdd = 13,
            kSkaterGrindStallAdd = 14,
            kSkaterGrindStallInitAdd = 15,
            kSkaterFakieAdd = 17,
            kSkaterWallrideAdd = 19,
        };
        constexpr uint32_t kJuiceMaxJuiceTime = 5; // Scoring: Juice
        enum GrindWallsEntry : uint32_t
        {
            kGrindBalanceFactor = 3,
            kGrindBalanceInit = 5,
            kStallBalanceFactor = 14,
            kStallBalanceInit = 16,
            kManualBalanceFactor = 19,
            kManualBalanceInit = 21,
            kWallRideGravity = 43,
        };
        constexpr uint32_t kStatCases[kStatCount] = {points::skaterStat::next18, points::skaterStat::notTaken2, points::skaterStat::notTaken6, points::skaterStat::notTaken13,
                                                     points::skaterStat::notTaken10, points::skaterStat::notTaken9, points::skaterStat::notTaken8};
    }

    // skaterStat: what one of the skater's stats does, its tuning value
    // moved by the stat's total (StatRecord::totalF): `use` picks among a
    // stat's values. Jump: the jump speed. Speed: 0 the top speed (more on
    // juice), 1 the juice time. Spin: the air rotation speed. Grind: 0 and 1
    // the grind balance's drift and its start, 2 and 3 the stall's. Manual:
    // 0 and 1 the manual balance's. Fakie: the fakie multiplier. Wallride:
    // the wall ride's gravity. Riding fakie, the jump, the top speed and the
    // spin are times the Fakie stat. 0 for any other.
    ee::Float skaterStat(ee::Ptr<Player> player, uint32_t stat, uint32_t use);

    ee::Float skaterStat(ee::Ptr<Player> player, uint32_t stat, uint32_t use)
    {
        stat &= 0xFFu;
        use &= 0xFFu;
        const auto total = [&] {
            return skaterProfile(player)->stats[stat].totalF;
        };
        const auto done = [](ee::Float value)
        {
            return value;
        };
        const auto none = []
        {
            return ee::Float(0.0f);
        };
        // A value that falls as the stat rises (the balance drifts, the wall
        // ride's gravity).
        const auto less = [&](uint32_t category, uint32_t base, uint32_t add)
        {
            return done(tuningValue(category, base) - tuningValue(kTuningSkater, add) * total());
        };
        // Riding fakie: times the Fakie stat (its own call of itself).
        const auto fakieTimes = [&](ee::Float value)
        {
            if (player->fakie)
            {
                value = value * skaterStat(player, kStatFakie, 0);
            }
            return done(value);
        };
        if (!(stat < kStatCount))
            return done(ee::Float(0.0f));
        switch (stat)
        {
        case kStatJump:
            return fakieTimes(tuningValue(kTuningSkater, kSkaterJumpSpeedAdd) * total() +
                              tuningValue(kTuningPhysics, kPhysicsJump));
        case kStatSpeed:
            if (use == 0)
            {
                ee::Float top = tuningValue(kTuningSkater, kSkaterTopSpeedSpeedAdd) * total() +
                                tuningValue(kTuningPhysics, kPhysicsMaxSpeed);
                if (isJizzed(player))
                    top = top + tuningValue(kTuningSkater, kSkaterTopSpeedJuiceSpeedAdd) * total();
                return fakieTimes(top);
            }
            if (use != 1)
                return none();
            return done(tuningValue(kTuningSkater, kSkaterTopSpeedJuiceAdd) * total() +
                        tuningValue(kTuningScoringJuice, kJuiceMaxJuiceTime));
        case kStatSpin:
            return fakieTimes(tuningValue(kTuningSkater, kSkaterSpinAdd) * total() +
                              tuningValue(kTuningPhysics, kPhysicsAirRotate));
        case kStatGrind:
            if (use == 0)
                return less(kTuningGrindWalls, kGrindBalanceFactor, kSkaterGrindGrindAdd);
            if (use == 1)
                return less(kTuningGrindWalls, kGrindBalanceInit, kSkaterGrindGrindInitAdd);
            if (use == 2)
                return less(kTuningGrindWalls, kStallBalanceFactor, kSkaterGrindStallAdd);
            if (use == 3)
                return less(kTuningGrindWalls, kStallBalanceInit, kSkaterGrindStallInitAdd);
            return none();
        case kStatManual:
            if (use == 0)
                return less(kTuningGrindWalls, kManualBalanceFactor, kSkaterManualBalanceAdd);
            if (use == 1)
                return less(kTuningGrindWalls, kManualBalanceInit, kSkaterManualBalanceInitAdd);
            return none();
        case kStatFakie:
            return done(tuningValue(kTuningSkater, kSkaterFakieAdd) * total() +
                        tuningValue(kTuningPhysics, kPhysicsFakieMult));
        default: // kStatWallRide
            return less(kTuningGrindWalls, kWallRideGravity, kSkaterWallrideAdd);
        }
    }
    READABLE_FUNCTION(skaterStat, Verified);
}
