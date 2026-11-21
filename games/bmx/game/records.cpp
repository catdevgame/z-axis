// BMX XXX's game records (records.h): the record a key names, in the round's
// state. BMX XXX's own code (game.c, its game block), DM2's line.

#include "records.h"

#include "overlay_messages.h"
#include "player_riders.h"
#include "round.h"

#include "bmx/action/camera_state.h"
#include "bmx/fx/spawns.h"
#include "bmx/objectives/objectives.h"
#include "bmx/overlay/displays.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/records.cpp.h"

namespace game::bmx
{
    namespace
    {
        // The record slot a mode's own record (kinds 0 and 1) takes, by mode.
        const ee::Ptr<uint8_t> kModeRecordSlots(sym::g_modeRecordSlots);

        // What the game was started for (bmx.c's): kind 1 sets no saved
        // record.
        inline ee::Global<uint8_t, sym::g_startUpKind> g_startUpKind;
        constexpr uint8_t kStartUpKindNoSavedRecords = 1;
        // checkGameRecord's flags: a saved record beaten is announced once.
        constexpr uint32_t kAnnounceOnce = 0x1;
    }

    // gameRecord: the record a key names: the span's slot of the kind, or of
    // the mode's own record for kinds 0 and 1. (The key's low bits, DM2's
    // entry, are not read.)
    ee::Ptr<GameRecord> gameRecord(uint32_t key)
    {
        const uint32_t span = (key >> kRecordKeySpanShift) & kRecordKeySpanMask;
        const uint32_t kind = (key >> kRecordKeyKindShift) & kRecordKeyKindMask;
        const uint32_t mode = key >> kRecordKeyModeShift;
        const uint32_t slot = kind < kFirstKindRecord ? kModeRecordSlots[mode] : kind;
        return ee::Ptr<GameRecord>(&g_round->records[span][slot]);
    }
    READABLE_FUNCTION(gameRecord, Verified);

    // checkGameRecord: a value of a kind of record for a player, while the
    // game plays (not under a screen, a replay or a cut scene). In each span
    // the value beats the record's: it is the record's, the player's, and
    // marked set. The saved span only counts in a mode that keeps records,
    // for a character riding one of its first four bikes (bikeChoice), and not where the game was
    // started for kind 1; a saved record beaten is announced on the reward
    // display (once only, with kAnnounceOnce) and sparkles on the rider. In
    // kStripMode a game record of the mode's own kind that reaches the
    // mode's target decides the turn (the other player's). The objectives are
    // told of the value either way.
    void checkGameRecord(uint32_t player, uint32_t kind, uint32_t rewardKind, uint32_t flags, ee::Float value)
    {
        ee::Local<RewardItemMessage> reward;
        player &= 0xFF;
        if (cameraState() == kCameraStatePlay)
        {
            for (uint32_t span = 0; span < kRecordSpans; ++span)
            {
                if (span == kSavedRecords)
                {
                    if (gameModeRecord()->scored == 0)
                        continue;
                    if (playerCharacter(player)->bikeChoice >= kSavedRecordsBelow)
                        continue;
                    if (*g_startUpKind == kStartUpKindNoSavedRecords)
                        continue;
                }
                const ee::Ptr<GameRecord> record = gameRecord(recordKey(gameMode(), kind, span));
                if (!record || !(record->value < value))
                    continue;
                if (span == kSavedRecords)
                {
                    if ((flags & kAnnounceOnce) == 0 || (record->marks & kRecordAnnounced) == 0)
                    {
                        reward->player = static_cast<uint8_t>(player);
                        reward->kind = static_cast<uint8_t>(rewardKind);
                        reward->value = value;
                        overlayMessage(kOverlayReward, reward);
                        record->marks |= kRecordAnnounced;
                        playerRider(player)->score.recordBeaten = 1;
                    }
                }
                else if (span == kGameRecords)
                {
                    if (kind == kModeRecordSlots[gameMode()] && gameMode() == kStripMode && gameModeRecord()->target <= value)
                    {
                        RoundState &round = *g_round;
                        round.turnEndStage = kTurnDecided;
                        round.decidedPlayer = player == 0 ? 1 : 0;
                        round.turnEndFrames = 0;
                    }
                }
                record->value = value;
                record->marks |= kRecordSet;
                record->player = static_cast<uint8_t>(player);
            }
        }
        objectives::sendRecordEvent(static_cast<uint8_t>(player), kind, value);
    }
    READABLE_FUNCTION(checkGameRecord, Verified);

    namespace
    {
        // A beaten saved record's sparkle (ScoreBlock::recordBeaten counts
        // its 60 frames): a little ahead of the drawn rider along its forward
        // row, pulsing, yellow, fading in over its first 10 frames and out
        // over its last 30.
        constexpr uint32_t kSparkleFrames = 60;
        constexpr uint32_t kSparkleFadeInFrames = 10;
        constexpr uint32_t kSparkleFadeOutFrom = 31;
        inline ee::Global<ee::Float, sym::g_sparkleAhead> g_sparkleAhead;       // 0.15
        inline ee::Global<ee::Float, sym::g_sparklePulseRate> g_sparklePulseRate; // 1 / 240: of a turn a frame
        constexpr ee::Float kAngleUnitsPerTurn = 65536.0f;
        inline ee::Global<ee::Float, sym::g_sparkleFadeIn> g_sparkleFadeIn;     // 19.2: alpha a frame
        inline ee::Global<ee::Float, sym::g_sparkleFadeOut> g_sparkleFadeOut;   // 6.4
        inline ee::Global<ee::Float, sym::g_sparklePulse> g_sparklePulse;       // 1.1
        inline ee::Global<ee::Float, sym::g_sparkleSize> g_sparkleSize;         // 0.4
        constexpr uint8_t kSparkleColour[4] = {0xD0, 0xC8, 0x60, 0xC0};

    }

    // recordEffectFrame: a player's beaten-record sparkle for the frame,
    // while it runs; at 60 frames it ends.
    void recordEffectFrame(uint32_t player)
    {
        ee::Local<zge::Vec4> point;
        ee::Local<uint8_t[4]> colour;
        const ee::Ptr<rider::Rider> rider = playerRider(player & 0xFF);
        rider::ScoreBlock &score = rider->score;
        if (score.recordBeaten == 0)
            return;
        if (score.recordBeaten >= kSparkleFrames)
        {
            score.recordBeaten = 0;
            return;
        }
        *point = *rider::vec4(rider->forward) * *g_sparkleAhead;
        *point = *point + *rider::vec4(rider->drawnPosition);
        const ee::Float frames = ee::toFloat(static_cast<int32_t>(score.recordBeaten));
        const ee::Float pulse = zge::angleSin(static_cast<zge::Angle>(ee::toInt(frames * *g_sparklePulseRate * kAngleUnitsPerTurn)));
        for (uint32_t i = 0; i < 4; ++i)
            colour[i] = kSparkleColour[i];
        if (score.recordBeaten < kSparkleFadeInFrames)
            colour[3] = static_cast<uint8_t>(ee::toInt(ee::toFloat(static_cast<int32_t>(score.recordBeaten)) * *g_sparkleFadeIn));
        else if (score.recordBeaten >= kSparkleFadeOutFrom)
            colour[3] = static_cast<uint8_t>(ee::toInt(ee::toFloat(static_cast<int32_t>(kSparkleFrames - score.recordBeaten)) * *g_sparkleFadeOut));
        // (fx.c's effect spawn, empty in this build: the sparkle shows nothing)
        unusedSpawnHook(point, rider::vec4(rider->velocity), colour, (pulse * *g_sparklePulse + 1.0f) * *g_sparkleSize);
        score.recordBeaten = score.recordBeaten + 1;
    }
    READABLE_FUNCTION(recordEffectFrame, Verified);
}
