// BMX XXX's level-script commands of action.c (script_events.h).

#include "script_events.h"

#include "action_state.h"
#include "bmx/overlay/displays.h"
#include "ee/abi.h"
#include "ee/readable.h"
#include "zge/scene/animation.h"

#include <cstdint>
#include "sym/bmx/action/script_events.cpp.h"

namespace game::bmx
{
    using namespace action;

    namespace
    {
        // The commands' fields, by the hash of their names.
        constexpr uint32_t kStringIdField = 0x096E2E54; // "STRINGID"
        constexpr uint32_t kMotorField = 0x00524942;    // "MOTOR"
        constexpr uint32_t kTypeField = 0x00059E45;     // "TYPE"
        constexpr uint32_t kAmountField = 0x04624A34;   // "AMOUNT"
        // A rumble's strength for one percent.
        constexpr ee::Float kStrengthPerPercent = 2.55f;

        // A command's byte field.
        uint8_t eventByte(ee::Ptr<zge::AnimationEvent> event, uint32_t fieldId)
        {
            return *ee::Ptr<uint8_t>(zge::eventFieldAddressCopy(event, fieldId));
        }
    }

    void subtitleEvent(uint32_t, ee::Ptr<zge::AnimationEvent> event)
    {
        ee::Local<uint32_t> stringId;

        if (zge::cutSceneAtOnce() != 0)
            return;
        *stringId = zge::eventInt(event, kStringIdField);
        overlayMessage(kOverlaySubtitle, stringId.address());
    }
    READABLE_FUNCTION(subtitleEvent, Verified);

    void vibrationEvent(uint32_t, ee::Ptr<zge::AnimationEvent> event)
    {
        ee::Local<Rumble> request;

        if (zge::cutSceneAtOnce() != 0)
            return;
        request->motor = eventByte(event, kMotorField);
        request->timing = eventByte(event, kTypeField);
        const uint8_t percent = eventByte(event, kAmountField);
        request->strength = static_cast<uint8_t>(ee::toInt(ee::toFloat(static_cast<int32_t>(percent)) * kStrengthPerPercent));
        for (uint32_t player = 0; player < *g_playerCount; ++player)
            calls::rumble(player, request.address());
    }
    READABLE_FUNCTION(vibrationEvent, Verified);
}
