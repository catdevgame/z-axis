// BMX XXX's "TRANSFER" objective type: a trick that starts on one surface
// and lands on another (FROM_FACE / FROM_EDGE / FROM_ENTITY and TO_FACE /
// TO_EDGE / TO_ENTITY: a face tag's or an edge tag's word, or a level
// object's name), either way round unless ONE_WAY, with SPIN degrees at
// least. BMX XXX's own code (its game block); the names are the script's
// keywords and the type's callbacks' places in its registration.

#include "objective_calls.h"
#include "objectives.h"

#include "bmx/rider/collision/collision.h"
#include "bmx/rider/rider.h"
#include "bmx/rider/vector.h"
#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/angle.h"
#include "zge/math/types.h"
#include "zge/math/vec.h"
#include "zge/math/vector.h"
#include "zge/render/material.h"
#include "zge/render/primitives.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/bmx/objectives/transfer_type.cpp.h"

namespace game::bmx::objectives
{
    using rider::Rider;
    namespace collision = rider::collision;

    namespace
    {
        // The transfer's two sides: where the trick starts and where it lands.
        enum TransferSide : uint32_t
        {
            kFrom = 0,
            kTo = 1,
            kSides = 2,
        };
        // The block's keys: FROM_FACE, FROM_EDGE, FROM_ENTITY, TO_FACE, TO_EDGE, TO_ENTITY, ONE_WAY, SPIN, ALLEY_OOP.
        constexpr uint32_t kTransferKeys = 9;
        // A side's three keys, in the order of TrickSurfaceSort (a face, an edge, an object).
        constexpr uint32_t kSideKeys = 3;
        constexpr uint32_t kNameBytes = 0x40;
        constexpr uint32_t kMaxTargets = 32;

        // One place of a side, by the side's sort: a face's or an edge's
        // reference, or a level object in the low word.
        union TransferTarget
        {
            uint64_t reference;               // a collision::FaceRef or EdgeRef
            ee::Ptr<zge::SceneObject> entity; // (the low word)
        };
        static_assert(sizeof(TransferTarget) == 8);

        // A TRANSFER objective's record (0x2B0 bytes, from the type's pool).
        struct TransferObjective
        {
            uint8_t flags;                                    // +0x000 kOneWay
            uint8_t _001[3];
            uint32_t sort[kSides];                            // +0x004 what each side is (TrickSurfaceSort)
            uint32_t nameHash[kSides];                        // +0x00C each side's name: a tag's word, or the object's name hash
            uint8_t spin;                                     // +0x014 SPIN in half turns (0: any)
            uint8_t trick;                                    // +0x015 a trick's number the line must have (kWhichever from the reader: this block has no key for it; kAnyAir: any air trick)
            uint8_t modifier;                                 // +0x016 a second trick's number (kWhichever from the reader; kAnyModifier: any at all)
            uint8_t lineFlags;                                // +0x017 kAlleyOop (ALLEY_OOP; kept, not read by the type)
            TransferTarget targets[kSides][kMaxTargets + 1];  // +0x018 each side's places, ended by a 0
            uint8_t _228[8];
            zge::Box sideBounds[kSides];               // +0x230 the box round each side's places
            zge::Vec4 centres[kSides];                        // +0x270 the middle of each side's box
            zge::Vec4 arcDirection;                           // +0x290 from the first centre towards the second, length 1
            ee::Float arcSpan;                                // +0x2A0 how far apart the centres are
            ee::Float arcCurve;                               // +0x2A4 the arc's height is arcCurve * (distance along - arcApex)^2 + arcPeak (y grows downwards)
            ee::Float arcApex;                                // +0x2A8 how far along the arc is highest
            ee::Float arcPeak;                                // +0x2AC its height there over the first centre (negative: above)
        };
        static_assert(offsetof(TransferObjective, sort) == 0x04 && offsetof(TransferObjective, nameHash) == 0x0C);
        static_assert(offsetof(TransferObjective, spin) == 0x14 && offsetof(TransferObjective, targets) == 0x18);
        static_assert(offsetof(TransferObjective, sideBounds) == 0x230 && offsetof(TransferObjective, centres) == 0x270);
        static_assert(offsetof(TransferObjective, arcDirection) == 0x290 && offsetof(TransferObjective, arcSpan) == 0x2A0);
        static_assert(offsetof(TransferObjective, arcPeak) == 0x2AC && sizeof(TransferObjective) == 0x2B0);
        constexpr uint8_t kOneWay = 0x1;       // ONE_WAY
        constexpr uint8_t kAlleyOop = 0x1;     // ALLEY_OOP (in lineFlags)
        constexpr uint8_t kWhichever = 0xFF;
        constexpr uint8_t kAnyAir = 0xFE;      // (trick)
        constexpr uint8_t kAnyModifier = 0xFE; // (modifier)
        constexpr uint8_t kNoModifier = 0xFF;  // ScoreTrick::modifier of a trick with none
        constexpr int32_t kDegreesPerHalfTurn = 180;

        inline ee::Global<ee::Ptr<zge::Pool>, sym::g_transferObjectives> g_transferObjectives;

        // The bounds start inside out and end this much wider than the places.
        constexpr ee::Float kFarAway = 99999.9f;
        constexpr ee::Float kFarBelow = -99999.9f;
        constexpr ee::Float kBoundsMargin = 20.0f;
        // The arc: no direction and no curve for less than this.
        constexpr ee::Float kTiny = 0.0001f;
        // The arc's height where it is highest: this part of the span, or
        // kLeastPeak less the second centre's drop where that is more.
        constexpr ee::Float kPeakOfSpan = 0.25f;
        constexpr ee::Float kLeastPeak = 2.0f;

        // What the engine's scale macro carries into VU0 beside its factor
        // (vec.h scaledCarrying): the upper half of the vector last copied.
        uint64_t upperHalf(const zge::Vec4 &v)
        {
            uint64_t bits;
            std::memcpy(&bits, &v.z, 8);
            return bits;
        }

        // ---- The arc's drawing ----
        //
        // The arc is drawn as a tube of 16 pieces, each between two rings
        // of 5 points round the arc; one piece at a time is lit white, and
        // the lit piece runs along the arc.
        constexpr uint32_t kRingPoints = 5;
        constexpr uint32_t kArcPieces = 16;
        constexpr ee::Float kArcPiecesFloat = 16.0f;
        constexpr ee::Float kRingRadius = 0.5f;
        constexpr ee::Float kRingStep = 0.2f;       // a turn's part between two points of a ring
        constexpr ee::Float kAnglesPerTurn = 65536.0f;
        constexpr ee::Float kUp = -1.0f;            // y grows downwards
        // The tube's colour (red, green, blue), and the lit piece's.
        constexpr uint8_t kArcRed = 0xC0, kArcGreen = 0x40, kArcBlue = 0x08;
        constexpr uint8_t kLit = 0xFF;
        // The primitive API's kind the highlights are drawn as (the engine
        // has no name for it yet).
        constexpr uint32_t kDrawKindTwo = 2;
        // The frames' fade (as the other highlights'): every 64 frames
        // from 252 down by 4 a frame, never under 64.
        constexpr uint32_t kFadeFrames = 0x3F;
        constexpr uint32_t kFadeLeast = 0x40;
        // The lit piece moves on every second frame, over 32 frames; when
        // the transfer goes both ways it runs back over the next 32.
        constexpr uint32_t kRunFrames = 0x1F;
        constexpr uint32_t kRunBack = 0x20;

        inline ee::Global<ee::Ptr<zge::Material>, sym::g_highlightMaterial> g_highlightMaterial;
        inline ee::Global<uint32_t, sym::g_highlightFrame> g_highlightFrame;

        // One line of the tube's wire frame: the function fills these and
        // draws none of them.
        struct WireLine
        {
            uint32_t _00;                          // (not set)
            ee::Ptr<zge::PrimitiveVertex> from;    // +0x04
            ee::Ptr<zge::PrimitiveVertex> to;      // +0x08
        };
        static_assert(sizeof(WireLine) == 0x0C);

        // A vertex's position as the quadword the original stores (x, y, z
        // and the word after them).
        zge::Vec4 &positionOf(zge::PrimitiveVertex &vertex)
        {
            return *reinterpret_cast<zge::Vec4 *>(&vertex);
        }

        // The ring's frame turned to look along `forward` (its third row,
        // set by the caller and made length 1 here) with y up, at `at`.
        // The up row's fourth lane is left as it was.
        void aimRing(zge::Matrix &frame, const zge::Vec4 &at)
        {
            zge::Vec4 &side = zge::matrixRow(frame, 0);
            zge::Vec4 &up = zge::matrixRow(frame, 1);
            zge::Vec4 &forward = zge::matrixRow(frame, 2);
            forward = zge::unit(forward);
            up.x = 0.0f;
            up.y = kUp;
            up.z = 0.0f;
            side = zge::cross(up, forward);
            side = zge::unit(side);
            up = zge::cross(forward, side);
            zge::matrixRow(frame, 3) = at;
        }

        // A ring's five points round the frame, in the tube's colour or lit.
        void setRing(zge::PrimitiveVertex *ring, const zge::Matrix &frame, zge::Vec4 &point, const uint8_t *rgba, bool lit)
        {
            point.z = 0.0f;
            for (uint32_t k = 0; k < kRingPoints; ++k)
            {
                const zge::Angle angle = static_cast<zge::Angle>(ee::toInt(ee::toFloat(static_cast<int32_t>(k)) * kRingStep * kAnglesPerTurn));
                point.x = zge::angleCos(angle) * kRingRadius;
                point.y = zge::angleSin(angle) * kRingRadius;
                positionOf(ring[k]) = zge::transformed(frame, point);
                if (lit)
                {
                    ring[k].rgba[0] = kLit;
                    ring[k].rgba[1] = kLit;
                    ring[k].rgba[2] = kLit;
                    ring[k].rgba[3] = rgba[3];
                }
                else
                {
                    for (uint32_t byte = 0; byte < 4; ++byte)
                        ring[k].rgba[byte] = rgba[byte];
                }
            }
        }

        void turnInsideOut(zge::Box &bounds)
        {
            bounds.low.z = kFarAway;
            bounds.low.x = kFarAway;
            bounds.low.y = kFarAway;
            bounds.high.z = kFarBelow;
            bounds.high.x = kFarBelow;
            bounds.high.y = kFarBelow;
        }
    }

    void readTransferParameters(Objective objective, ee::Ptr<const char> parentKeyword);
    uint32_t readTransferBlock(ee::Ptr<const char> beginKeyword);
    void freeTransfer(Objective objective);
    void resetTransfer();
    void transferEvent(Objective objective, ee::Ptr<TrickEvent> event);
    void transferCompleted();
    void highlightTransfer(Objective objective);
    void updateTransferBounds(Objective objective);
    uint32_t transferFrame(Objective objective, ee::Ptr<ObjectiveEvent> event);
    void setTransferArc(ee::Ptr<TransferObjective> transfer);
    void drawTransferArc(ee::Ptr<TransferObjective> transfer);

    // registerTransferType: the "TRANSFER" type and its callbacks.
    void registerTransferType()
    {
        ee::Local<ObjectiveTypeSetup> setup;
        zge::memoryFill(setup, 0, sizeof(ObjectiveTypeSetup));
        setup->name = ee::Ptr<const char>(sym::transferTypeName);
        setup->hooks.readParameters = sym::readTransferParameters;
        setup->hooks.freeData = sym::freeTransfer;
        setup->hooks.resetProgress = sym::resetTransfer;
        setup->hooks.onEvent = sym::transferEvent;
        setup->hooks.onComplete = sym::transferCompleted;
        setup->hooks.drawForRider = sym::highlightTransfer;
        setup->hooks.moveBounds = sym::updateTransferBounds;
        setup->hooks.inProgress = sym::transferFrame;
        zge::registerObjectiveType(setup);
    }
    READABLE_FUNCTION_WAITS(registerTransferType, Verified);

    // readTransferParameters (the type's readParameters): the block
    // TRANSFER_BEGIN .. TRANSFER_END is made readable inside the
    // objective's block, with the objective as its owner.
    void readTransferParameters(Objective objective, ee::Ptr<const char> parentKeyword)
    {
        ee::Local<BlockClassSetup> setup;
        ee::Local<ee::Ptr<const char>[kTransferKeys]> keyNames;
        ee::Local<uint32_t[kTransferKeys]> keyKinds;
        const ee::Ptr<ee::Ptr<const char>> names(sym::transferKeyNames);
        for (uint32_t i = 0; i < kTransferKeys; ++i)
            keyNames[i] = names[i];
        calls::memset(keyKinds, 0, sizeof(uint32_t[kTransferKeys]));
        setup->parentBegin = parentKeyword;
        setup->keyNames = keyNames;
        setup->keyKinds = keyKinds;
        setup->begin = ee::Ptr<const char>(sym::transferBeginKeyword);
        setup->end = ee::Ptr<const char>(sym::transferEndKeyword);
        setup->keyCount = kTransferKeys;
        setup->onEnd = sym::readTransferBlock;
        setup->onBegin = 0;
        zge::registerBlockClass(setup);
        zge::setBlockClassOwner(ee::Ptr<const char>(sym::transferBeginKeyword), objective);
    }
    READABLE_FUNCTION_WAITS(readTransferParameters, Verified);

    // readTransferBlock (the block's handler): the objective's record made
    // from the block's values. Each side is the first of its FACE, EDGE
    // and ENTITY lines the block has: the name's hash is kept, and every
    // face or edge of the level with that tag word, or every level object
    // of that name, is one of the side's places (32 at most), with the
    // side's box grown round the faces' centres, the edges' two ends or
    // the objects' positions. A side with none of the three lines is left
    // empty. Then ONE_WAY, SPIN (degrees, kept in half turns), ALLEY_OOP,
    // and the arc between the two sides. Always 1.
    uint32_t readTransferBlock(ee::Ptr<const char> beginKeyword)
    {
        ee::Local<char[kSides][kNameBytes]> names;
        ee::Local<zge::Vec4> point;
        ee::Local<uint32_t[kSides * kSideKeys]> sideKeys;
        const ee::Ptr<uint32_t> keys(sym::transferSideKeys);
        for (uint32_t i = 0; i < kSides * kSideKeys; ++i)
            sideKeys[i] = keys[i];
        const Objective owner(zge::blockClassOwner(beginKeyword));
        zge::objectiveInfo(owner);
        const ee::Ptr<BlockValues> values = zge::blockClassValues(beginKeyword);
        const ee::Ptr<TransferObjective> transfer(zge::poolTakeCreating(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_transferObjectives), sizeof(TransferObjective)));
        zge::memoryFill(transfer, 0, sizeof(TransferObjective));
        transfer->modifier = kWhichever;
        transfer->trick = kWhichever;

        for (uint32_t side = 0; side < kSides; ++side)
        {
            turnInsideOut(transfer->sideBounds[side]);
            const ee::Ptr<zge::Box> sideBounds(&transfer->sideBounds[side]);

            ee::Ptr<BlockValue> value = zge::findScriptValue(values, ee::Ptr<const char>(sideKeys[side * kSideKeys + kTrickOnFace]));
            if (value)
                transfer->sort[side] = kTrickOnFace;
            else
            {
                value = zge::findScriptValue(values, ee::Ptr<const char>(sideKeys[side * kSideKeys + kTrickOnEdge]));
                if (value)
                    transfer->sort[side] = kTrickOnEdge;
                else
                {
                    value = zge::findScriptValue(values, ee::Ptr<const char>(sideKeys[side * kSideKeys + kTrickOnObject]));
                    if (!value)
                        continue;
                    transfer->sort[side] = kTrickOnObject;
                }
            }

            const ee::Ptr<char> name(names.address() + side * kNameBytes);
            zge::scriptValueText(value, name, kNameBytes);
            uint32_t count = 0;
            transfer->nameHash[side] = zge::nameHash(name);
            const ee::Ptr<uint32_t> hash(&transfer->nameHash[side]);
            if (transfer->sort[side] == kTrickOnFace)
            {
                const ee::Ptr<uint64_t> found = rider::collision::facesTagged(hash, 1);
                if (found)
                {
                    for (uint32_t i = 0; found[i] != 0; ++i)
                    {
                        collision::faceCentre(found[i], &point->x);
                        growBounds(sideBounds, point);
                        if (i < kMaxTargets)
                        {
                            transfer->targets[side][i].reference = found[i];
                            ++count;
                        }
                    }
                }
                transfer->targets[side][count].reference = 0;
            }
            else if (transfer->sort[side] == kTrickOnEdge)
            {
                const ee::Ptr<uint64_t> found = rider::collision::edgesTagged(hash, 1);
                if (found)
                {
                    for (uint32_t i = 0; found[i] != 0; ++i)
                    {
                        for (uint32_t end = 0; end < 2; ++end)
                        {
                            collision::edgePoint(found[i], end, &point->x);
                            growBounds(sideBounds, point);
                        }
                        if (i < kMaxTargets)
                        {
                            transfer->targets[side][i].reference = found[i];
                            ++count;
                        }
                    }
                }
                transfer->targets[side][count].reference = 0;
            }
            else if (transfer->sort[side] == kTrickOnObject)
            {
                const ee::Ptr<ee::Ptr<zge::SceneObject>> found = zge::findObjects(hash, 1);
                if (found)
                {
                    for (uint32_t i = 0; found[i]; ++i)
                    {
                        *point = *zge::objectPosition(found[i]);
                        growBounds(sideBounds, point);
                        if (i < kMaxTargets)
                        {
                            transfer->targets[side][i].entity = found[i];
                            ++count;
                        }
                    }
                }
                transfer->targets[side][count].entity = ee::Ptr<zge::SceneObject>();
            }
        }

        ee::Ptr<BlockValue> value = zge::findScriptValue(values, ee::Ptr<const char>(sym::transferKeyOneWay));
        if (value)
        {
            const uint32_t on = zge::scriptValueSwitch(value, 0);
            transfer->flags = static_cast<uint8_t>((transfer->flags & ~kOneWay) | (on & 1));
        }
        value = zge::findScriptValue(values, ee::Ptr<const char>(sym::transferKeySpin));
        if (value)
            transfer->spin = static_cast<uint8_t>(zge::scriptValueInteger(value) / kDegreesPerHalfTurn);
        value = zge::findScriptValue(values, ee::Ptr<const char>(sym::transferKeyAlleyOop));
        if (value)
        {
            const uint32_t on = zge::scriptValueSwitch(value, 0);
            transfer->lineFlags = static_cast<uint8_t>((transfer->lineFlags & ~kAlleyOop) | (on & 1));
        }
        setTransferArc(transfer);
        zge::setObjectiveData(owner, ee::Address(transfer));
        zge::unregisterBlockClass(ee::Ptr<const char>(sym::transferBeginKeyword));
        return 1;
    }
    READABLE_FUNCTION_WAITS(readTransferBlock, Verified);

    // freeTransfer (the type's free): the record back to the pool.
    void freeTransfer(Objective objective)
    {
        const ee::Address transfer = zge::objectiveData(objective);
        if (transfer)
            zge::poolGiveDestroying(ee::Ptr<ee::Ptr<zge::Pool>>(sym::g_transferObjectives), transfer);
        calls::setObjectiveDataTail(objective, ee::Address());
    }
    READABLE_FUNCTION_WAITS(freeTransfer, Draft);

    // resetTransfer (the type's reset): nothing to do.
    void resetTransfer()
    {
    }
    READABLE_FUNCTION(resetTransfer, Verified);

    // transferEvent (the type's event), on a trick event. A trick that
    // ends makes the objective active again. A trick that starts with its
    // two wheels' surfaces being the transfer's two sides (or, unless
    // ONE_WAY, the other way round) is the transfer: the objective is done
    // at once when no spin, trick or modifier is asked, else when one
    // trick of the rider's score lines is in a line with the spin at
    // least and is the trick and has the modifier asked.
    void transferEvent(Objective objective, ee::Ptr<TrickEvent> event)
    {
        const ee::Ptr<TransferObjective> transfer(zge::objectiveData(objective));
        if (event->kind != kTrickEvent)
            return;
        if (event->started == 0)
        {
            zge::setObjectiveState(objective, kObjectiveActive);
            return;
        }
        const bool forwards = event->sort[kFrom] == transfer->sort[kFrom] && event->surface[kFrom] == transfer->nameHash[kFrom] &&
                              event->sort[kTo] == transfer->sort[kTo] && event->surface[kTo] == transfer->nameHash[kTo];
        if (!forwards)
        {
            if ((transfer->flags & kOneWay) != 0)
                return;
            const bool backwards = event->sort[kTo] == transfer->sort[kFrom] && event->surface[kTo] == transfer->nameHash[kFrom] &&
                                   event->sort[kFrom] == transfer->sort[kTo] && event->surface[kFrom] == transfer->nameHash[kTo];
            if (!backwards)
                return;
        }
        if (transfer->spin == 0 && transfer->trick == kWhichever && transfer->modifier == kWhichever)
        {
            zge::setObjectiveState(objective, kObjectiveDone);
            return;
        }
        const ee::Ptr<Rider> rider = rider::riderOf(riderIndexOf(event->player));
        const ScoreLine *const lines = rider->score.lines;
        for (uint32_t i = 0; i < rider->score.lineCount; ++i)
        {
            for (ee::Ptr<ScoreTrick> trick = lines[i].tricks; trick; trick = trick->next)
            {
                if (lines[i].spin < transfer->spin)
                    continue;
                if (transfer->trick != kWhichever && transfer->trick != trick->trick)
                {
                    if (transfer->trick != kAnyAir)
                        continue;
                    if (trickType(trick->trick) != kTrickAir)
                        continue;
                }
                if (transfer->modifier != kWhichever && transfer->modifier != trick->modifier)
                {
                    if (transfer->modifier != kAnyModifier)
                        continue;
                    if (trick->modifier == kNoModifier)
                        continue;
                }
                zge::setObjectiveState(objective, kObjectiveDone);
                return;
            }
        }
    }
    READABLE_FUNCTION_WAITS(transferEvent, Verified);

    // transferCompleted (the type's completed): nothing to do.
    void transferCompleted()
    {
    }
    READABLE_FUNCTION(transferCompleted, Draft);

    // highlightTransfer (the type's highlight): the transfer's arc drawn.
    void highlightTransfer(Objective objective)
    {
        const ee::Address transfer = zge::objectiveData(objective);
        game::tail(drawTransferArc, ee::Ptr<TransferObjective>(transfer));
    }
    READABLE_FUNCTION_WAITS(highlightTransfer, Verified);

    // updateTransferBounds (the type's updateBounds): the objective's
    // bounds made the box round both sides' places (the faces' centres,
    // the edges' two ends, the objects' positions), 20 units wider each
    // way.
    void updateTransferBounds(Objective objective)
    {
        ee::Local<zge::Vec4> point;
        const ee::Ptr<TransferObjective> transfer(zge::objectiveData(objective));
        const ee::Ptr<zge::Box> bounds = zge::objectiveBounds(objective);
        bounds->low.z = kFarAway;
        bounds->low.x = kFarAway;
        bounds->low.y = kFarAway;
        bounds->high.x = kFarBelow;
        bounds->high.z = kFarBelow;
        bounds->high.y = kFarBelow;
        for (uint32_t side = 0; side < kSides; ++side)
        {
            if (transfer->sort[side] == kTrickOnFace)
            {
                for (uint32_t i = 0; transfer->targets[side][i].reference != 0; ++i)
                {
                    collision::faceCentre(transfer->targets[side][i].reference, &point->x);
                    growBounds(bounds, point);
                }
            }
            else if (transfer->sort[side] == kTrickOnEdge)
            {
                for (uint32_t i = 0; transfer->targets[side][i].reference != 0; ++i)
                {
                    for (uint32_t end = 0; end < 2; ++end)
                    {
                        collision::edgePoint(transfer->targets[side][i].reference, end, &point->x);
                        growBounds(bounds, point);
                    }
                }
            }
            else if (transfer->sort[side] == kTrickOnObject)
            {
                for (uint32_t i = 0; transfer->targets[side][i].entity; ++i)
                {
                    *point = *zge::objectPosition(transfer->targets[side][i].entity);
                    growBounds(bounds, point);
                }
            }
        }
        const ee::Float minX = bounds->low.x - kBoundsMargin;
        const ee::Float minY = bounds->low.y - kBoundsMargin;
        const ee::Float minZ = bounds->low.z - kBoundsMargin;
        const ee::Float maxX = bounds->high.x + kBoundsMargin;
        const ee::Float maxY = bounds->high.y + kBoundsMargin;
        const ee::Float maxZ = bounds->high.z + kBoundsMargin;
        bounds->low.x = minX;
        bounds->low.y = minY;
        bounds->low.z = minZ;
        bounds->high.x = maxX;
        bounds->high.z = maxZ;
        bounds->high.y = maxY;
    }
    READABLE_FUNCTION_WAITS(updateTransferBounds, Verified);

    // transferFrame (the type's frame): whether the player's rider is
    // inside the objective's bounds.
    uint32_t transferFrame(Objective objective, ee::Ptr<ObjectiveEvent> event)
    {
        ee::Local<zge::Vec4> at;
        const ee::Ptr<Rider> rider = rider::riderOf(riderIndexOf(event->player));
        const ee::Ptr<zge::Box> bounds = zge::objectiveBounds(objective);
        *at = *rider::vec4(rider->position);
        if (!(bounds->low.x <= at->x) || !(bounds->low.y <= at->y) || !(bounds->low.z <= at->z))
            return 0;
        if (!(at->x <= bounds->high.x) || !(at->y <= bounds->high.y) || !(at->z <= bounds->high.z))
            return 0;
        return 1;
    }
    READABLE_FUNCTION_WAITS(transferFrame, Verified);

    // setTransferArc: the arc the highlight draws from one side to the
    // other. Each side's centre is the middle of its box; the arc runs
    // from the first centre along the direction to the second, as a
    // parabola through the first centre that is highest (its height there
    // a quarter of the span, or 2 less the second centre's drop where that
    // is more) at a place chosen so that it comes down through the second
    // centre: half-way when both are level.
    void setTransferArc(ee::Ptr<TransferObjective> transfer)
    {
        ee::Local<zge::Vec4> direction;
        ee::Local<zge::Vec4[kSides]> centres;
        for (uint32_t side = 0; side < kSides; ++side)
        {
            const zge::Box &box = transfer->sideBounds[side];
            centres[side] = box.high - box.low;
            centres[side] = centres[side] * ee::Float(0.5f);
            centres[side] = centres[side] + box.low;
            transfer->centres[side] = centres[side];
        }
        *direction = centres[kTo] - centres[kFrom];
        const ee::Float span = zge::vec3Length(&direction->xyz());
        if (kTiny < ee::abs(span))
            *direction = zge::scaledCarrying(*direction, ee::Float(1.0f) / span, upperHalf(centres[kTo]));
        const ee::Float drop = centres[kTo].y - centres[kFrom].y;
        ee::Float peak = span * kPeakOfSpan;
        const ee::Float least = kLeastPeak - drop;
        if (least < peak)
            peak = -peak;
        else
            peak = -least;
        ee::Float apexOfSpan;
        if (drop == ee::Float(0.0f))
            apexOfSpan = 0.5f;
        else
        {
            const ee::Float ratio = peak / drop;
            const ee::Float root = ee::sqrt(ratio * (ratio - ee::Float(1.0f)));
            if (ratio < root)
                apexOfSpan = ratio + root;
            else
                apexOfSpan = ratio - root;
        }
        const ee::Float apex = span * apexOfSpan;
        ee::Float curve;
        if (kTiny < ee::abs(apex))
            curve = -(peak / (apex * apex));
        else
            curve = 0.0f;
        transfer->arcDirection = *direction;
        transfer->arcPeak = peak;
        transfer->arcSpan = span;
        transfer->arcCurve = curve;
        transfer->arcApex = apex;
    }
    READABLE_FUNCTION(setTransferArc, Verified);

    // drawTransferArc: the transfer's arc drawn as a tube from the first
    // side's centre to the second's, in 16 pieces along the parabola
    // setTransferArc worked out. Each piece joins two rings of five points
    // with ten triangles of the highlight material; the rings face along
    // the arc. The tube fades with the highlight frame counter, and one
    // piece, running along the arc with the frames, is lit white.
    void drawTransferArc(ee::Ptr<TransferObjective> transfer)
    {
        ee::Local<zge::Vec4> point;
        ee::Local<zge::Vec4> at;
        ee::Local<uint8_t[4]> rgba;
        ee::Local<zge::Matrix> frame;
        ee::Local<zge::PrimitiveVertex[kRingPoints]> nearRing;
        ee::Local<zge::PrimitiveVertex[kRingPoints]> farRing;
        ee::Local<zge::PrimitiveTriangle[kRingPoints * 2]> triangles;
        ee::Local<WireLine[kRingPoints * 4]> wire;

        const uint32_t frames = *g_highlightFrame;
        uint32_t strength = (kFadeFrames - (frames & kFadeFrames)) << 2;
        if (!(kFadeFrames < strength))
            strength = kFadeLeast;
        strength &= 0xFF;
        rgba[0] = kArcRed;
        rgba[1] = kArcGreen;
        rgba[2] = kArcBlue;
        rgba[3] = static_cast<uint8_t>(strength);
        int32_t litPiece = ee::toInt(ee::toFloat(static_cast<int32_t>(frames & kRunFrames)) * ee::Float(0.5f));
        if ((transfer->flags & kOneWay) == 0 && (frames & kRunBack) != 0)
            litPiece = static_cast<int32_t>(kArcPieces - 1) - litPiece;

        // The tube's side between two neighbouring points of the rings: two
        // triangles, and the four lines of the wire frame.
        const uint32_t material = ee::Address(*g_highlightMaterial);
        for (uint32_t i = 0; i < kRingPoints; ++i)
        {
            const uint32_t next = (i + 1) % kRingPoints;
            const ee::Ptr<zge::PrimitiveVertex> nearHere(nearRing.address() + i * sizeof(zge::PrimitiveVertex));
            const ee::Ptr<zge::PrimitiveVertex> nearNext(nearRing.address() + next * sizeof(zge::PrimitiveVertex));
            const ee::Ptr<zge::PrimitiveVertex> farHere(farRing.address() + i * sizeof(zge::PrimitiveVertex));
            const ee::Ptr<zge::PrimitiveVertex> farNext(farRing.address() + next * sizeof(zge::PrimitiveVertex));
            triangles[i * 2].vertices[0] = nearHere;
            triangles[i * 2].vertices[1] = nearNext;
            triangles[i * 2].vertices[2] = farNext;
            triangles[i * 2 + 1].vertices[0] = farNext;
            triangles[i * 2 + 1].vertices[1] = farHere;
            triangles[i * 2 + 1].vertices[2] = nearHere;
            triangles[i * 2].texture = material;
            triangles[i * 2 + 1].texture = material;
            wire[i * 4].from = nearHere;
            wire[i * 4].to = nearNext;
            wire[i * 4 + 1].from = nearNext;
            wire[i * 4 + 1].to = farNext;
            wire[i * 4 + 2].from = farNext;
            wire[i * 4 + 2].to = farHere;
            wire[i * 4 + 3].from = nearHere;
            wire[i * 4 + 3].to = farNext;
        }

        // The first ring, at the first centre.
        const ee::Float pieceLength = transfer->arcSpan / kArcPiecesFloat;
        *at = transfer->centres[kFrom];
        zge::Vec4 &forward = zge::matrixRow(*frame, 2);
        forward = transfer->arcDirection;
        forward.y = transfer->arcCurve * -transfer->arcApex;
        aimRing(*frame, transfer->centres[kFrom]);
        setRing(&farRing[0], *frame, *point, &rgba[0], litPiece == 0);

        // Piece by piece: the last ring becomes the near one, and the far
        // one is set at the piece's end. `fromApex` is the end's distance
        // along the arc, counted from where the arc is highest.
        ee::Float fromApex = pieceLength - transfer->arcApex;
        for (uint32_t piece = 0; piece < kArcPieces; ++piece)
        {
            const ee::Float fromApexSquared = fromApex * fromApex;
            const ee::Float nextFromApex = fromApex + pieceLength;
            for (uint32_t k = 0; k < kRingPoints; ++k)
            {
                positionOf(nearRing[k]) = positionOf(farRing[k]);
                for (uint32_t byte = 0; byte < 4; ++byte)
                    nearRing[k].rgba[byte] = farRing[k].rgba[byte];
            }
            *point = zge::scaledCarrying(transfer->arcDirection, fromApex + transfer->arcApex, upperHalf(positionOf(farRing[kRingPoints - 1])));
            point->y = transfer->arcCurve * fromApexSquared + transfer->arcPeak;
            *point = *point + transfer->centres[kFrom];
            *at = *point;
            forward = transfer->arcDirection;
            forward.y = transfer->arcCurve * fromApex;
            aimRing(*frame, *at);
            setRing(&farRing[0], *frame, *point, &rgba[0], litPiece == static_cast<int32_t>(piece));
            zge::drawPrimitiveStrips(kDrawKindTwo, triangles, kRingPoints * 2, ee::Address());
            fromApex = nextFromApex;
        }
    }
    READABLE_FUNCTION_WAITS(drawTransferArc, Verified);
}
