// BMX XXX's STRIP CHALLENGE (round.h's kStripMode; "go for the biggest
// combo. Each time you break the record, your opponent loses an article of
// clothing. Keep going until one of you gets nekkid!"): the mode's own frame
// of a turn's end, which takes the riders' clothes off a garment at a time
// by changing their models' entries. BMX XXX's own code (game.c, its game
// block).

#include "player_riders.h"
#include "round.h"

#include "bmx/action/action_state.h"
#include "bmx/overlay/displays.h"
#include "bmx/profile/players.h"
#include "bmx/rider/rider.h"
#include "bmx/sound/events.h"
#include "bmx/sound/sound_mode.h"
#include "ee/abi.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "zge/scene/animate_system.h"
#include "zge/scene/model_records.h"
#include "zge/text/cstring.h"

#include <cstddef>
#include <cstdint>
#include "sym/bmx/game/strip_challenge.cpp.h"

namespace game::bmx
{
    using action::kActionFlag1; // the play's frame runs only the game's step: the riders held

    namespace
    {
        // A garment as the models name it: the start of its entries' asset
        // names, and the asset worn dressed and stripped ("GMU_SKIJACKET",
        // "GMU_0NOSHIRT"). An empty stripped name takes the entry away.
        struct Garment
        {
            ee::Ptr<const char> prefix;   // +0x00 "GMU"
            ee::Ptr<const char> dressed;  // +0x04
            ee::Ptr<const char> stripped; // +0x08
        };
        static_assert(sizeof(Garment) == 0x0C);
        // By body (0 male, 1 female: isFemaleRider), by garment in the order
        // they come off (shoes, top, bottom), each as its part (a "G" name)
        // and its texture (a "T" name).
        constexpr uint32_t kBodies = 2, kGarments = 3, kGarmentEntries = 2;
        inline ee::Global<Garment[kBodies][kGarments][kGarmentEntries], sym::g_stripGarments> g_stripGarments;

        // stripGarmentVisit's context: the garment, which of its names to
        // wear, and what the visit found and changed.
        struct GarmentSearch
        {
            ee::Ptr<const Garment> garment; // +0x00
            uint32_t dressed;               // +0x04 1: its dressed name, 0: its stripped one
            uint32_t found;                 // +0x08 an entry of the garment's was visited
            uint32_t changed;               // +0x0C an entry was changed
        };
        static_assert(sizeof(GarmentSearch) == 0x10);

        // The mode's stages of a turn's end (RoundState::turnEndStage).
        enum StripTurnStage : uint8_t
        {
            kStripStart = 0,
            kStripDress = 1,          // every rider dressed (frame 1), the play held
            kStripDressedWait = 2,    // until every rider's model is ready
            kStripRiding = 3,         // until a record decides the turn (checkGameRecord: kTurnDecided)
            kStripRecordBroken = 4,   // kTurnDecided: the other player loses a garment, or with all three lost, the turn ends
            kStripUndress = 5,        // the garment comes off (frame 60), the play held
            kStripUndressedWait = 6,  // until the rider's model is ready: riding again
            kStripDecided = 7,        // a rider with kFlagDowned before it: 240 frames
            kStripRedress = 8,        // (nothing sets it) every rider's kept model put back (frame 60)
            kStripRedressedWait = 9,  // (after 8) until every rider's model is ready
            kStripRedressed = 10,
            kStripEnded = 11,
        };
        constexpr uint32_t kUndressFrames = 60;

        // The garments a rider has lost: the turn's score counts them.
        constexpr ee::Float kGarmentsToLose = ee::Float(3.0f);
        // The countdown's sound (its last count's, 0) as a naked rider loses
        // again, and the sound as a garment comes off (the game bank's 118).
        constexpr uint32_t kCountdownSound = 0x38, kGarmentOffSound = 0x46;

        // The riders whose models are ready again (a bit a player).
        inline ee::Global<uint32_t, sym::g_stripModelsReady> g_stripModelsReady;
        // Each player's model as the game started (gameSetup keeps a copy).
        inline ee::Global<ee::Ptr<zge::ModelRecord>[kMaxPlayers], sym::g_keptRiderModels> g_keptRiderModels;

        // (riderModelReady's) the rider's model is changing.
        constexpr rider::RiderFlag kFlagModelChanging{0, 0x10};

        // The rider's object rebuilt from its model: marked changing, its
        // soft parts' and limbs' rigs freed (riderModelReady makes them again
        // once the model is built).
        void rebuildRiderModel(ee::Ptr<rider::Rider> rider)
        {
            zge::bindModelToObject(rider->messenger, rider->model);
            rider::setFlag(&*rider, kFlagModelChanging);
            if (rider->softParts)
            {
                zge::freeSoftPartsRig(rider->softParts);
                rider->softParts = {};
            }
            if (rider->limbRig != 0)
            {
                zge::freeBodyRig(ee::Ptr<zge::BodyRig>(rider->limbRig));
                rider->limbRig = 0;
            }
        }

        // A garment entry's name changed in the model: each aspect visited
        // until the garment's entry is found.
        void changeGarment(ee::Ptr<rider::Rider> rider, ee::Local<GarmentSearch> &search)
        {
            for (uint32_t aspect = 0; search->garment->prefix[0] != 0 && search->found == 0 && aspect < zge::kModelAspects; ++aspect)
                zge::forEachModelEntry(rider->model, aspect, zge::ModelEntryVisit(sym::stripGarmentVisit), search.address());
        }

        ee::Ptr<const Garment> garmentEntry(uint32_t body, uint32_t garment, uint32_t entry)
        {
            return ee::Ptr<const Garment>(&(*g_stripGarments)[body][garment][entry]);
        }
    }

    // stripGarmentVisit: a model entry's visit (forEachModelEntry): an asset
    // whose name (in capitals) starts with the garment's prefix is the
    // garment's: it is changed to the name the search wears, when it is not
    // that already (taken away, and the new one added unless empty), and the
    // visit stops.
    int32_t stripGarmentVisit(ee::Ptr<zge::ModelRecord> model, uint32_t aspect, ee::Ptr<const char> library, ee::Ptr<const char> asset,
                              int32_t rank, ee::Ptr<GarmentSearch> search)
    {
        ee::Local<char[0x100]> name;

        const ee::Ptr<const Garment> garment = search->garment;
        zge::stringCopy(name.address(), asset);
        zge::stringToUpper(name.address());
        if (zge::compareStringsN(name.address(), garment->prefix, zge::stringLength(garment->prefix)) != 0)
            return 1;
        const ee::Ptr<const char> wear = search->dressed != 0 ? garment->dressed : garment->stripped;
        if (zge::compareStrings(name.address(), wear) != 0)
        {
            zge::removeModelEntry(model, aspect, asset);
            if (wear[0] != 0)
                zge::addModelEntry(model, aspect, library, wear, rank);
            search->changed = 1;
        }
        search->found = 1;
        return 0;
    }
    READABLE_FUNCTION(stripGarmentVisit, Verified);

    // stripTurnEndFrame: the strip challenge's turn end (StripTurnStage).
    // The turn starts with every rider dressed. A record that decides the
    // turn (checkGameRecord) takes the next garment off the other player's
    // rider while the play is held, the screens fading out and in; with all
    // three gone, the next one ends the turn. A stage that moves on is run
    // again at once.
    void stripTurnEndFrame(uint32_t player)
    {
        ee::Local<GarmentSearch> search;

        player &= 0xFF;
        gameModeRecord(); // (asked for; not used)
        const ee::Ptr<rider::Rider> rider = playerRider(player);
        rider::ScoreBlock &score = rider->score;
        RoundState &round = *g_round;
        bool moved;
        do
        {
            moved = false;
            if (rider::hasFlag(&*rider, rider::kFlagDowned) && round.turnEndStage < kStripDecided)
            {
                round.decidedPlayer = static_cast<uint8_t>(player);
                round.turnEndStage = kStripDecided;
                round.turnEndFrames = 0;
            }
            const auto moveTo = [&](uint8_t stage) {
                round.turnEndStage = stage;
                round.turnEndFrames = 0;
                moved = true;
            };
            const auto waitFor = [&](uint8_t stage) {
                round.turnEndStage = stage;
                round.turnEndFrames = 0;
                *g_stripModelsReady = 0;
            };
            // This player's rider marked ready once its model is; whether
            // every player's is.
            const auto everyModelReady = [&]() {
                if (rider::riderModelReady(&*rider))
                    *g_stripModelsReady |= 1u << player;
                return *g_stripModelsReady == (1u << *g_playerCount) - 1;
            };

            switch (round.turnEndStage)
            {
            case kStripStart:
                moveTo(kStripDress);
                break;
            case kStripDress:
                if (round.turnEndFrames == 0)
                {
                    if (player == round.decidedPlayer)
                    {
                        for (uint32_t each = 0; each < *g_playerCount; ++each)
                        {
                            playOverlayFade(each, kOverlayFadeSetOut);
                            playOverlayFade(each, kOverlayFadeLoadingSet);
                        }
                    }
                    setActionFlags(kActionFlag1, 1);
                }
                else if (round.turnEndFrames == 1)
                {
                    for (uint32_t each = 0; each < *g_playerCount; ++each)
                    {
                        const ee::Ptr<rider::Rider> eachRider = playerRider(each);
                        search->changed = 0;
                        for (uint32_t garment = 0; garment < kGarments; ++garment)
                        {
                            for (uint32_t entry = 0; entry < kGarmentEntries; ++entry)
                            {
                                const uint32_t body = isFemaleRider(&(*g_playerSetups)[each]);
                                search->found = 0;
                                search->dressed = 1;
                                search->garment = garmentEntry(body, garment, entry);
                                changeGarment(eachRider, search);
                            }
                        }
                        if (search->changed != 0)
                            rebuildRiderModel(eachRider);
                    }
                    waitFor(kStripDressedWait);
                }
                break;
            case kStripDressedWait:
                if (!everyModelReady())
                    break;
                for (uint32_t each = 0; each < *g_playerCount; ++each)
                {
                    playOverlayFade(each, kOverlayFadeIn);
                    playOverlayFade(each, kOverlayFadeLoadingReset);
                }
                setActionFlags(kActionFlag1, 0);
                moveTo(kStripRiding);
                break;
            case kStripRiding:
                break;
            case kStripRecordBroken:
                setRoundFlags(kTurnAtRestFlag, 1);
                if (player != round.decidedPlayer)
                    break;
                if (score.turnScore < kGarmentsToLose)
                {
                    moveTo(kStripUndress);
                }
                else
                {
                    soundEvent(kCountdownSound, 0, 0, 0, 0);
                    moveTo(kStripEnded);
                }
                break;
            case kStripUndress:
                if (round.turnEndFrames == 0)
                {
                    setGameMode(kPausedMode);
                    if (player == round.decidedPlayer)
                    {
                        for (uint32_t each = 0; each < *g_playerCount; ++each)
                            playOverlayFade(each, kOverlayFadeOut);
                    }
                    setActionFlags(kActionFlag1, 1);
                }
                else if (round.turnEndFrames == kUndressFrames)
                {
                    if (player != round.decidedPlayer)
                        break;
                    playOverlayFade(player, kOverlayFadeLoadingSet);
                    soundEvent(kGarmentOffSound, 0, 0, 0, 0);
                    for (uint32_t each = 0; each < *g_playerCount; ++each)
                    {
                        rider::resetPhysics(&*playerRider(each), 0);
                        rider::riderReset(riderIndexOf(each), 1);
                    }
                    // The next garment: as many are gone as the turn's score
                    // counts.
                    for (uint32_t entry = 0; entry < kGarmentEntries; ++entry)
                    {
                        const uint32_t body = isFemaleRider(&(*g_playerSetups)[player]);
                        const uint32_t garment = static_cast<uint32_t>(ee::toInt(score.turnScore));
                        search->dressed = 0;
                        search->found = 0;
                        search->garment = garmentEntry(body, garment, entry);
                        changeGarment(rider, search);
                    }
                    rebuildRiderModel(rider);
                    score.turnScore = score.turnScore + ee::Float(1.0f);
                    moveTo(kStripUndressedWait);
                }
                break;
            case kStripUndressedWait:
                if (player != round.decidedPlayer || !rider::riderModelReady(&*rider))
                    break;
                playOverlayFade(player, kOverlayFadeLoadingReset);
                for (uint32_t each = 0; each < *g_playerCount; ++each)
                    playOverlayFade(each, kOverlayFadeReset);
                moveTo(kStripRiding);
                setGameMode(kPlayMode);
                setActionFlags(kActionFlag1, 0);
                setRoundFlags(kTurnAtRestFlag, 0);
                break;
            case kStripDecided:
                if (round.turnEndFrames == kTurnDecidedFrames)
                    moveTo(kStripEnded);
                break;
            case kStripRedress:
                if (round.turnEndFrames == 0)
                {
                    if (player == round.decidedPlayer)
                    {
                        for (uint32_t each = 0; each < *g_playerCount; ++each)
                            playOverlayFade(each, kOverlayFadeOut);
                    }
                    setActionFlags(kActionFlag1, 1);
                }
                else if (round.turnEndFrames == kUndressFrames)
                {
                    for (uint32_t each = 0; each < *g_playerCount; ++each)
                    {
                        const ee::Ptr<rider::Rider> eachRider = playerRider(each);
                        zge::copyModelRecord(eachRider->model, (*g_keptRiderModels)[each]);
                        rebuildRiderModel(eachRider);
                    }
                    waitFor(kStripRedressedWait);
                }
                break;
            case kStripRedressedWait:
                if (!everyModelReady())
                    break;
                for (uint32_t each = 0; each < *g_playerCount; ++each)
                    playOverlayFade(each, kOverlayFadeReset);
                setActionFlags(kActionFlag1, 0);
                moveTo(kStripRedressed);
                break;
            case kStripRedressed:
                moveTo(kStripEnded);
                break;
            case kStripEnded:
                setRoundFlags(kTurnEndedFlag, 1);
                break;
            default:
                break;
            }
        } while (moved);
    }
    READABLE_FUNCTION(stripTurnEndFrame, Verified);
}
