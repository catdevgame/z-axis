// What the skater stands on (was the sweep's keepContact): after a move
// (moveSkater) the face and object it touched become the surface and
// object it is on. A change tells the level: "OnEnd" to the object left,
// "OnStart" to the one reached (each with its surface kind), and in a
// manual "ManualEnd" (the kind and the distance manualled, to the object
// and the player's script) and "ManualStart", the distance starting again.
//

#include "skater.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "moves.h"
#include "sym/aggressive/skater/contact.cpp.h"

namespace aggressive::skater
{
    int32_t sendObjectEvent(ee::Ptr<Player> /*player*/, ee::Ptr<zge::Node> object, ee::Address name, ee::Address data);

    namespace
    {
        constexpr uint32_t kOnEnd = sym::onEnd;       // "OnEnd"
        constexpr uint32_t kOnStart = sym::onStart;     // "OnStart"
        constexpr uint32_t kManualEnd = sym::manualEnd;   // "ManualEnd"
        constexpr uint32_t kManualStart = sym::manualStart; // "ManualStart"
        constexpr uint32_t kNoFace = 0xFFFFFFFFu;

        // ManualEnd's data: the surface kind and the distance manualled.
        struct ManualEndData
        {
            uint32_t surfaceKind;
            ee::Float distance;
        };
    }

    // keepContact: the face and object the move touched (Player::contact,
    // contactObject) become the ones the skater is on (surface, object;
    // lastObject keeps the one left), and the surface normal the ground's.
    void keepContact(ee::Ptr<Player> player)
    {
        // The original's frame: ManualEnd's data at +0x00, the other
        // events' (a surface kind) at +0x10.
        ee::Local<ManualEndData> manualEnd;
        ee::Local<uint32_t> eventKind;

        if (player->surface != kNoFace && player->object != 0)
        {
            const uint32_t heldKind = surfaceKind(player->objectNode, player->surface);
            const uint32_t touchedKind = surfaceKind(player->contactNode, player->contact);
            if (heldKind == touchedKind && player->object == player->contactObject)
            {
                // The same surface of the same object: nothing to tell.
                player->surface = player->contact;
                player->object = player->contactObject;
                player->surfaceNormal = player->groundNormal;
                return;
            }
            *eventKind = surfaceKind(player->objectNode, player->surface);
            sendObjectEvent(player, player->objectNode, kOnEnd, eventKind);
            *eventKind = surfaceKind(player->contactNode, player->contact);
            sendObjectEvent(player, player->contactNode, kOnStart, eventKind);
            if (player->state == State::Manual)
            {
                manualEnd->distance = player->distance;
                manualEnd->surfaceKind = surfaceKind(player->objectNode, player->surface);
                sendObjectEvent(player, player->objectNode, kManualEnd, manualEnd);
                if (player->script)
                    sendScriptEvent(player, player->script, kManualEnd, manualEnd);
                *eventKind = surfaceKind(player->contactNode, player->contact);
                sendObjectEvent(player, player->contactNode, kManualStart, eventKind);
                player->distance = 0.0f;
            }
            if (player->object != player->contactObject)
                player->lastObject = player->object;
        }
        else
        {
            if (player->contactObject)
            {
                if (player->contact == kNoFace)
                {
                    player->surface = kNoFace;
                    player->object = player->contactObject;
                    player->surfaceNormal = player->groundNormal;
                    return;
                }
                *eventKind = surfaceKind(player->contactNode, player->contact);
                sendObjectEvent(player, player->contactNode, kOnStart, eventKind);
                if (player->state == State::Manual)
                {
                    *eventKind = surfaceKind(player->contactNode, player->contact);
                    sendObjectEvent(player, player->contactNode, kManualStart, eventKind);
                    player->distance = 0.0f;
                }
            }
        }
        player->surface = player->contact;
        player->object = player->contactObject;
        player->surfaceNormal = player->groundNormal;
    }
    READABLE_FUNCTION_WAITS(keepContact, Verified);
}
