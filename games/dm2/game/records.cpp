// DM2's game records (records.h): the best values of a level's games, kept
// for four spans of play. DM2's own code (game.c, its game block).

#include "records.h"
#include "goals.h"
#include "players.h"
#include "round.h"

#include "dm2/action/action.h"
#include "dm2/overlay/overlay.h"
#include "dm2/rider/actions/actions.h"
#include "dm2/rider/rider.h"
#include "ee/plain.h"
#include "sce/libgcc.h"
#include "ee/abi.h"
#include "ee/readable.h"

#include <cstdint>
#include "sym/dm2/game/records.cpp.h"

namespace game::dm2
{
    namespace
    {
        // The records of the four spans, side by side.
        inline ee::Global<GameRecord[kRecordSpans][kRecordsPerSpan], sym::g_gameRecords> g_gameRecords;

        ee::Ptr<GameRecord> recordAt(uint32_t span, uint32_t index)
        {
            return ee::Ptr<GameRecord>(sym::g_gameRecords) + static_cast<ptrdiff_t>(span * kRecordsPerSpan + index);
        }
    }

    // gameRecord: the record a key names, or 0 where a mode keeps none of
    // that kind.
    ee::Ptr<GameRecord> gameRecord(uint32_t key)
    {
        const uint32_t entry = key & kRecordKeyEntryMask;
        const uint32_t span = (key >> kRecordKeySpanShift) & kRecordKeySpanMask;
        const uint32_t kind = (key >> kRecordKeyKindShift) & kRecordKeyKindMask;
        const uint32_t single = (key >> kRecordKeySingleShift) & kRecordKeySingleMask;
        const uint32_t mode = key >> kRecordKeyModeShift;
        if (kind >= kFirstOwnKind)
            return recordAt(span, kKindRecords + kind);
        switch (mode)
        {
        case 0:
            return recordAt(span, kSinglePlayerRecords + entry);
        case 1:
            return recordAt(span, kMode1Records + single);
        case 2:
            return kind == 1 ? recordAt(span, kMode2Record) : ee::Ptr<GameRecord>();
        case 3:
            return kind == 1 ? recordAt(span, kMode3Record) : ee::Ptr<GameRecord>();
        case 4:
            if (kind == 0)
                return recordAt(span, kMode4Records + 1);
            if (kind == 1)
                return recordAt(span, kMode4Records);
            return ee::Ptr<GameRecord>();
        case 5:
            return kind == 1 ? recordAt(span, kMode5Record) : ee::Ptr<GameRecord>();
        case 6: case 7: case 8: case 9: case 10: case 11: case 12: case 13:
            return recordAt(span, kKindRecords + (*g_modeRecordKinds)[mode]);
        case 14:
            return recordAt(span, kMode14Records + single);
        default:
            return ee::Ptr<GameRecord>();
        }
    }
    READABLE_FUNCTION(gameRecord, Verified);

    // checkGameRecord: nothing is a record while the level is not in play
    // (a screen or a replay is up). The saved span is left out in a free
    // ride, in a park's test and for a character whose records are not
    // saved. A saved record beaten is announced on the reward display and
    // marked on the player's rider.
    void checkGameRecord(uint32_t player, uint32_t kind, uint32_t rewardKind, uint32_t flags, ee::Float value)
    {
        ee::Local<OverlayReward> reward;
        if (actionMode() == 0)
        {
            for (uint32_t span = 0; span < kRecordSpans; ++span)
            {
                if (span == kSavedRecords)
                {
                    if (multiplayerMode() == 0)
                    {
                        if (singlePlayerMode() == kFreeRide)
                            continue;
                        if (singlePlayerMode() == kParkTest)
                            continue;
                    }
                    if (playerCharacter(player)->bike >= kSavedRecordsBelow)
                        continue;
                }
                const uint8_t mode = multiplayerMode();
                const uint8_t single = singlePlayerMode();
                const ee::Ptr<GameRecord> record = gameRecord(recordKey(mode, single, kind, span));
                if (!record || !(record->value < value))
                    continue;
                if (span == kSavedRecords && !((flags & kAnnounceOnce) != 0 && (record->marks & kRecordAnnounced) != 0))
                {
                    reward->kind = static_cast<uint8_t>(rewardKind);
                    reward->value = value;
                    overlayMessage(kOverlayReward, reward);
                    record->marks |= kRecordAnnounced;
                    rider::riderOf(riderIndexOf(player))->score.recordBeaten = 1;
                }
                record->value = value;
                record->marks |= kRecordSet;
                record->player = static_cast<uint8_t>(player);
            }
        }
        goalRecord(player, kind, sce::libgcc::fptoui(value));
    }
    READABLE_FUNCTION(checkGameRecord, Verified);
}
