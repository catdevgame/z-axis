// Trigger zones (updateTriggerZones, once a frame from the player driver): boxes
// in the level that tell their scene object when a skater comes in and
// goes out (the level scripts' "enter" / "exit" style events: goals,
// doors, NPC givers). A skater is in a zone when its feet (the drawn
// position) or its head (2.4 above them, along the drawn up axis) are in
// the box. Zones are found through a coarse grid over the level; a player
// keeps the ones it is in (Player::zones, 10 at most).

#include "skater.h"

#include "aggressive/level/zones.h"
#include "aggressive/script/script.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "zge/math/matrix.h"
#include "zge/math/quaternion.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "sce/libgcc.h"
#include "sym/aggressive/skater/zones.cpp.h"

namespace aggressive::skater
{
    using level::kZoneGrid;
    using level::kZoneGridCells;
    using level::kZoneGridRowWords;
    using level::Zone;
    using level::ZoneListNode;
    using level::g_enterEvent;
    using level::g_exitEvent;
    using level::g_zoneCellDepth;
    using level::g_zoneCellWidth;
    using level::g_zoneGridMinX;
    using level::g_zoneGridMinZ;

    namespace
    {
        // The head's height above the feet (-2.4: y grows downward).
        inline ee::Global<ee::Float, sym::g_headHeight> g_headHeight;
        constexpr uint32_t kZoneSlots = 10;

        // What the zones' events carry.
        struct ZoneEvent
        {
            uint32_t zone;         // the zone's id
            uint32_t player;       // Player::number
        };
    }

    void updateTriggerZones(ee::Ptr<Player> player)
    {
        ee::Local<script::Message> message;
        ee::Local<zge::Vec4> head;
        ee::Local<zge::Matrix> drawnMatrix;
        ee::Local<ZoneEvent> event;
        const zge::Vec4 &feet = player->drawnPosition;

        // The head: (0, height, 0) turned by the drawn orientation, then
        // moved to the feet.
        zge::matrixIdentity(&*drawnMatrix);
        zge::quaternionToMatrix(&*drawnMatrix, &player->drawnOrientation);
        head->x = 0.0f;
        head->y = *g_headHeight;
        head->z = 0.0f;
        *head = zge::rotated(*drawnMatrix, *head);
        *head = *head + feet;

        const auto inZone = [&](ee::Ptr<Zone> zone) {
            return zge::boxContainsPoint(&zone->box, &feet.xyz()) != 0 ||
                   zge::boxContainsPoint(&zone->box, &head->xyz()) != 0;
        };
        // The event's name is read only once a receiver is found (a zone
        // with no target reads neither event word).
        const auto tell = [&](ee::Ptr<Zone> zone, const auto &eventName) {
            const ee::Ptr<ee::Ptr<zge::SceneObject>> receiver = zge::findObjects(&zone->target, 1);
            if (!receiver)
                return;
            event->zone = zone->id;
            event->player = player->number;
            message->target = ee::Address(*receiver); // the message's word
            message->sender = 0;
            message->type = *eventName;
            message->data = event.address();
            message->flags = zge::kMessageNow;
            zge::sendEngineMessage(message);
        };

        // Zones left: an exit event, the slot freed.
        for (uint32_t i = 0; i < kZoneSlots; ++i)
        {
            const ee::Ptr<Zone> zone(player->zones[i]);
            if (zone && !inZone(zone))
            {
                tell(zone, g_exitEvent);
                player->zones[i] = 0;
            }
        }

        // The grid cell under the feet (out of range on either side: the last).
        const int32_t column = sce::libgcc::fptoui((feet.x - *g_zoneGridMinX) / *g_zoneCellWidth);
        const int32_t row = sce::libgcc::fptoui((feet.z - *g_zoneGridMinZ) / *g_zoneCellDepth);
        const uint32_t x = static_cast<uint32_t>(column) < kZoneGridCells ? static_cast<uint32_t>(column) : 5u;
        const uint32_t z = static_cast<uint32_t>(row) < kZoneGridCells ? static_cast<uint32_t>(row) : 5u;

        // Zones entered: kept in the first free slot (none free: ignored)
        // and told.
        const uint32_t firstNode = ee::Ptr<uint32_t>(kZoneGrid)[x * kZoneGridRowWords + z];
        for (ee::Ptr<ZoneListNode> node(firstNode); node; node = ee::Ptr<ZoneListNode>(node->next))
        {
            const ee::Ptr<Zone> zone(node->zone);
            if (!inZone(zone))
                continue;
            bool known = false;
            for (uint32_t i = 0; i < kZoneSlots && !known; ++i)
                known = player->zones[i] == node->zone;
            if (known)
                continue;
            for (uint32_t i = 0; i < kZoneSlots; ++i)
            {
                if (player->zones[i] != 0)
                    continue;
                player->zones[i] = node->zone;
                if (zone->enterHandler)
                    aggressive::level::zoneEnterHandler(zone->target, player);
                else
                    tell(zone, g_enterEvent);
                break;
            }
        }
    }
    READABLE_FUNCTION_WAITS(updateTriggerZones, Verified);
}
