#pragma once
// BMX XXX's players in a level (action.c, player_riders.cpp): which rider,
// weapon and camera each player has. BMX XXX's own code, DM2's line (games/dm2/game/players.h).
//
// Names come from the code's use and DM2's readable pairs (no build names
// these functions); "(inferred)" marks a meaning read from use only.

#include "bmx/profile/player_setup.h"
#include "bmx/rider/rider.h"
#include "ee/ee.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/player_riders.h.h"

namespace game::bmx
{
    // What is being played, as the front end and the park editor set it
    // (0 .. 6). Kinds 5 and 6 have one rider, one weapon and one camera for
    // every player; every other kind gives each player their own.
    inline ee::Global<uint8_t, sym::g_sessionKind> g_sessionKind;
    constexpr uint8_t kFirstOneRiderSession = 5;
    constexpr uint8_t kLastOneRiderSession = 6;
    inline bool sessionHasOneRider()
    {
        return *g_sessionKind >= kFirstOneRiderSession && *g_sessionKind <= kLastOneRiderSession;
    }

    // A player's rider (an index for riderOf), weapon and camera (records of
    // weapon.c and camera.c).
    uint8_t riderIndexOf(uint32_t player);
    uint32_t playerWeapon(uint32_t player);
    uint32_t playerCamera(uint32_t player);

    // All the tips still to show (Rider::tipsLeft).
    constexpr uint32_t kAllTips = (1u << 22) - 1;

    // The rider a player has.
    inline ee::Ptr<rider::Rider> playerRider(uint32_t player)
    {
        return rider::riderOf(riderIndexOf(player));
    }

    // What a rider is made from (the rider module's newRider; DM2's
    // NewRider with BMX XXX's sizes): a player's rider from its character,
    // a replay's from the replay's rider head (the model names first), an
    // NPC's from its models' names.
    struct NewRider
    {
        uint8_t flags;                // +0x000 kRiderFromReplay, kRiderNpc
        uint8_t player;               // +0x001
        uint8_t _002[6];
        ee::Ptr<zge::ModelRecord> model; // +0x008 the rider's model record (a character's live model, an NPC's NPR model)
        uint8_t savedModel[0x73C];    // +0x00C a character's saved model (RiderCharacter::savedModel)
        uint8_t characterBike[2];     // +0x748 a character's bike and its byte +0x78D
        uint8_t _74a[6];
        char riderModel[0x50];        // +0x750 the rider's model's name ("rider.Mika"), and
        char bikeModel[0x50];         // +0x7A0 the bike's ("NPR.Bike00"): where a replay's rider head goes
        uint8_t _7f0[0x80];
    };
    static_assert(offsetof(NewRider, model) == 0x008 && offsetof(NewRider, characterBike) == 0x748);
    static_assert(offsetof(NewRider, riderModel) == 0x750 && offsetof(NewRider, bikeModel) == 0x7A0 && sizeof(NewRider) == 0x870);
    // NewRider::flags: the rider's head (its models' names) from the
    // request; and (inferred: only the NPCs' riders set it) an NPC's rider.
    constexpr uint8_t kRiderFromReplay = 0x02;
    constexpr uint8_t kRiderNpc = 0x01;
    // A replay's rider head's bytes (replayRiderHead): the model names and
    // what follows them.
    constexpr uint32_t kReplayRiderHeadBytes = 0x120;

    // What each player picked (bmx.c's table: profile/players.h).
    inline const ee::Ptr<PlayerSetup> kPlayerSetups(sym::g_playerSetups);
    inline ee::Ptr<PlayerSetup> playerSetup(uint32_t player)
    {
        return kPlayerSetups + static_cast<ptrdiff_t>(player);
    }
}
