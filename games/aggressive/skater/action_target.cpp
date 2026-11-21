// What the action button does (actionRequest, asked by the Ground and Air
// states' move 10): a pole or a car to hold in the Action Check Radius, a
// message to the objects there and to the people who answer "AreYouCloser",
// a bar out of reach, or a vault over a rail.

#include "moves.h"

#include "aggressive/tuning.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/scene/message.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"
#include "aggressive/pads.h"
#include "aggressive/level/closers.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/aggressive/skater/action_target.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // zge's collisionQuery by address: its filter context goes as the
        // whole quadword the original's register holds (the upper half of
        // the vector last copied), which the plain call's word cannot carry.
        int32_t collisionQuery(uint32_t kind, ee::Ptr<zge::QuerySphere> shape, uint32_t flags, uint32_t filter, ee::Qword filterContext, uint32_t nodes,
                               ee::Ptr<zge::QueryResults> results, uint32_t cache)
        {
            return ee::game<int32_t>(sym::collisionQuery, kind, shape, flags, filter, filterContext, nodes, results, cache);
        }
    }

    namespace
    {
        inline ee::Global<char, sym::g_textAction> g_textAction; // "Action"
        constexpr uint32_t kActionButton = 4;                    // layoutButton's
        constexpr uint32_t kQuerySphere = 0;                     // a query's shape: a sphere

        // Action tuning, by entry.
        enum ActionTuning : uint32_t
        {
            kActionCheckRadius = 0,
            kVaultCheckRadius = 1,
            kVaultCheckAngle = 2,
            kVaultMinSpeed = 3,
            kVertPoleAngle = 11,
            kPoleGrabTimeout = 12,
        };
        ee::Float actionTuning(ActionTuning entry)
        {
            return tuningValue(kTuningAction, entry);
        }

        // The rail edges closestRailEdge looks for.
        enum EdgeKind : uint32_t
        {
            kEdgeVerticalPole = 3,
            kEdgeHorizontalPole = 4,
            kEdgeCar = 7,
        };

        // The top half of a vector, which the query is handed in its filter
        // context's register: no filter is set, so nothing reads it.
        uint64_t highHalf(const Vec4 &v)
        {
            uint64_t high;
            std::memcpy(&high, &v.z, sizeof(high));
            return high;
        }
    }

    // actionRequest: the action button. A pole or bar in reach (once Pole
    // Grab Timeout has passed) or a car to skitch is taken at once. Else a
    // press tells every object in Action Check Radius "Action"; in the air
    // off a pipe or a spine, not yet turned, the skater drops back to Air with
    // the combo counter up one; a press then tells the closest person; and on
    // the ground, over Vault Min Speed, a rail in reach is vaulted. The state
    // to go to, -1 for none.
    int32_t actionRequest(ee::Ptr<Player> player)
    {
        ee::Local<zge::QuerySphere> sphere;
        ee::Local<zge::QueryResults> hits;
        ee::Local<zge::EngineMessage> message;
        ee::Local<RailRecord> vaultRecord;

        Controls &controls = player->controls;
        RailRecord &edge = railRecordOf(*player);
        const uint32_t actionHash = zge::nameHash(g_textAction.get());

        sphere->radius = actionTuning(kActionCheckRadius);
        sphere->centre = player->position;
        const int32_t queried = collisionQuery(kQuerySphere, sphere, 0, 0, ee::Qword{0, highHalf(player->position)}, 0, hits, 0);
        const bool hit = (queried & 0xFFu) != 0;

        if (hit)
        {
            const auto findEdge = [&](uint32_t kind) {
                return closestRailEdge(player, &edge, hits, kind, actionTuning(kVertPoleAngle));
            };
            const auto lastDirection = [&] { return controls.historyCount != 0 ? controls.history[0] : uint8_t(0); };
            if (actionTuning(kPoleGrabTimeout) <= controls.poleGrabTime)
            {
                if (findEdge(kEdgeVerticalPole) != 0)
                {
                    player->side = lastDirection();
                    return static_cast<int32_t>(State::VerticalPole);
                }
                if (findEdge(kEdgeHorizontalPole) != 0)
                {
                    player->barDirection = lastDirection();
                    return static_cast<int32_t>(State::HorizontalPole);
                }
            }
            if (findEdge(kEdgeCar) != 0)
                return static_cast<int32_t>(State::Skitch);
        }

        const auto actionPressed = [&] {
            const uint32_t pressed = layoutButton(kActionButton, controls.layout);
            if (padPressed(pressed, controls.port) == 0)
                return false;
            const uint32_t held = layoutButton(kActionButton, controls.layout);
            return padHeld(held, controls.port) != 0;
        };
        const auto sendAction = [&](ee::Address target) {
            message->target = target;
            message->sender = 0;
            message->type = actionHash;
            message->data = 0;
            message->flags = zge::kMessageNow;
            zge::sendEngineMessage(message);
        };

        // A press: every object hit is told.
        if (actionPressed() && hit)
        {
            for (uint32_t i = 0; i < static_cast<uint32_t>(hits->count); ++i)
            {
                const ee::Ptr<zge::SceneObject> owner = zge::nodeSceneObject(hits->nodes[i]->node, 0);
                if (owner != 0)
                    sendAction(owner);
            }
        }

        if ((player->state == State::AirQuarterPipe || player->state == State::AirSpine) && player->substate == 0)
        {
            const int32_t combo = playerComboState(player);
            setPlayerComboState(player, combo + 1);
            return static_cast<int32_t>(State::Air);
        }

        if (actionPressed())
        {
            const int32_t person = aggressive::level::findCloserObject(&player->position, 0, actionTuning(kActionCheckRadius));
            if (person != 0)
                sendAction(person);
        }

        const State now = player->state;
        if (now != State::Ground && now != State::Manual && now != State::CessSlide)
            return -1;

        if (actionTuning(kVaultMinSpeed) < player->speed)
        {
            zge::memoryFill(&edge, 0, sizeof(RailRecord));
            if (searchRails(player, vaultRecord, 0, 1, actionTuning(kVaultCheckRadius), actionTuning(kVaultCheckAngle)) != 0)
            {
                zge::memoryCopy(&edge, vaultRecord, sizeof(RailRecord));
                return static_cast<int32_t>(State::Vault);
            }
        }
        return -1;
    }
    READABLE_FUNCTION_WAITS(actionRequest, Verified);
}
