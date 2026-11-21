// The trick input: each frame the controls watch the trick button; on a press
// (and after "# cmd delay frames" held) the trick input matches the recent
// stick directions against the trick table (matchTrick) and gets a trick
// object, then asks the skater's state for the move the trick wants
// (requestMove, states.cpp). The state's answer decides: a new state, or the
// move of the state the skater is in already, starts the trick (its side and
// stance from the skater's, then the skater's current action object); a
// refusal drops it.
//
// Trick objects are game objects (aggressive/objects.h) told things by
// messages.

#include "skater.h"
#include "trick_objects.h"

#include "aggressive/objects.h"

#include "aggressive/tuning.h"
#include "ee/dispatch.h"
#include "ee/function_ptr.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "action_tricks.h"
#include "sym/aggressive/skater/tricks.cpp.h"

namespace aggressive::skater
{
    namespace
    {
        // TrickState::type.
        constexpr uint32_t kTypeDoubleGrind = 24;
        constexpr uint32_t kTypeHandGrind = 25;
        constexpr uint32_t kTypePastGrinds = 26; // past Scoring: Grinds' 25: no maker sets it, and no state answers its move

        // Eight stick directions, as a pattern to match Controls::history
        // and the mask of the bytes it uses.
        union DirectionPattern
        {
            uint8_t directions[8];
            uint32_t words[2];
        };
        static_assert(sizeof(DirectionPattern) == 8);

        using TrickMaker = ee::FunctionPtr<ee::Ptr<GameObject>()>;

        // A row of a trick table: the pattern (the masks are filled in at
        // start-up) and the trick it makes.
        struct TrickButtonEntry
        {
            TrickMaker create;        // +0x00
            uint32_t unlockBit;       // +0x04 a bit of Player::unlockedTricks the trick needs (0: none)
            DirectionPattern pattern; // +0x08
            DirectionPattern mask;    // +0x10
        };
        static_assert(sizeof(TrickButtonEntry) == 0x18);
        constexpr uint32_t kTrickButtonEntries = 25;
        inline ee::Global<TrickButtonEntry[kTrickButtonEntries], sym::g_trickButtons> g_trickButtons;
        // The stall's own table.
        constexpr uint32_t kStallTrickEntries = 4;
        inline ee::Global<TrickButtonEntry[kStallTrickEntries], sym::g_stallTricks> g_stallTricks;
        // The air tricks' table: flips, combinations of up to four stick
        // directions, the eight single ones last.
        constexpr uint32_t kAirTrickEntries = 55;
        inline ee::Global<TrickButtonEntry[kAirTrickEntries], sym::g_airTricks> g_airTricks;
        // The two manuals' table.
        constexpr uint32_t kManualTrickEntries = 2;
        inline ee::Global<TrickButtonEntry[kManualTrickEntries], sym::g_manualTricks> g_manualTricks;
        constexpr TrickMaker kMakeDoubleGrind{sym::newDoubleGrindTrick}; // the trick table's first row

        constexpr int32_t kRefused = -1;
        constexpr uint32_t kTrickButton = 5; // of the button layout

        // Control / # cmd delay frames.
        ee::Float commandDelayFrames()
        {
            return (*g_tuningCategories)[5].entries[3].value;
        }

        ee::Ptr<TrickState> trickState(ee::Ptr<GameObject> trick)
        {
            return ee::Ptr<TrickState>(gameObjectState(trick));
        }

        // The move the skater's state stands for (a refused request is still
        // the trick's when the skater is in that move already).
        int32_t moveOfState(State state)
        {
            switch (state)
            {
            case State::Grind: return kMoveGrind;
            case State::HandGrind: return kMoveHandGrind;
            case State::DoubleGrind: return kMoveDoubleGrind;
            case State::WallRide:
            case State::WallTap: return kMoveWallRide;
            case State::Manual: return kMoveManual;
            default: return kMoveHold;
            }
        }

        // One history word is kept from row to row, starting with the first:
        // once a row's first word has matched, the second word is what every
        // later row's first word is compared with. So after a near miss the
        // rest of the table is matched against the wrong word. This is the
        // game's behaviour.
        class HistoryMatch
        {
        public:
            explicit HistoryMatch(const Controls &controls) : m_controls(controls), m_word(controls.historyWords[0]) {}
            bool operator()(const TrickButtonEntry &entry)
            {
                if ((m_word & entry.mask.words[0]) != (entry.pattern.words[0] & entry.mask.words[0]))
                    return false;
                m_word = m_controls.historyWords[1];
                return (m_word & entry.mask.words[1]) == (entry.pattern.words[1] & entry.mask.words[1]);
            }

        private:
            const Controls &m_controls;
            uint32_t m_word;
        };

        // The directions the pattern covers: its mask's 0xFF bytes (the first
        // word's four when it is all 0xFF, then the second word's).
        uint32_t directionsCovered(const TrickButtonEntry &entry)
        {
            uint32_t covered = 0;
            const uint8_t *mask = entry.mask.directions;
            if (entry.mask.words[0] == 0xFFFFFFFFu)
            {
                covered = 4;
                mask += 4;
            }
            for (uint32_t i = 0; i < 4; ++i)
            {
                if (mask[i] == 0xFF)
                    ++covered;
            }
            return covered;
        }

        // Up after the pattern switches foot, down makes it fakie: matchTrick's
        // `result`, from the next two directions.
        void noteNextDirections(const Controls &controls, const TrickButtonEntry &entry, ee::Ptr<uint32_t> bits)
        {
            const uint32_t covered = directionsCovered(entry);
            for (uint32_t next = covered; next < covered + 2; ++next)
            {
                if (controls.historyCount < next + 1)
                    return;
                const uint8_t direction = controls.history[next];
                if (direction == kDirUp)
                    *bits |= kMatchedSwitch;
                else if (direction == kDirDown)
                    *bits |= kMatchedFakie;
            }
        }
    }

    namespace
    {
        // A secret trick needs its bit won and the skater jizzed.
        bool trickUnlocked(ee::Ptr<Player> player, const TrickButtonEntry &entry)
        {
            if (!entry.unlockBit)
                return true;
            const uint32_t won = (player->unlockedTricks >> (entry.unlockBit & 31u)) & 1u;
            return won && isJizzed(player) != 0;
        }
    }

    // matchTrick: the trick the recent directions make: the first of the trick
    // table's rows they match (and, for the three with an unlock bit,
    // unlocked) makes a new trick object; none matching, the plain one.
    // `result` (when given) gets kMatchedSwitch and kMatchedFakie from the
    // directions after the pattern's.
    ee::Ptr<GameObject> matchTrick(ee::Ptr<Player> player, ee::Ptr<uint32_t> result, int32_t excludeDoubleGrind)
    {
        if (result)
            *result = 0;
        const Controls &controls = player->controls;
        if (controls.historyCount != 0)
        {
            HistoryMatch matches(controls);
            for (const TrickButtonEntry &entry : *g_trickButtons)
            {
                if (!matches(entry))
                    continue;
                if (excludeDoubleGrind && entry.create == kMakeDoubleGrind)
                    continue;
                if (!trickUnlocked(player, entry))
                    continue;
                if (result)
                    noteNextDirections(controls, entry, result);
                return entry.create();
            }
        }
        return newPlainGrindTrick();
    }
    READABLE_FUNCTION(matchTrick, Verified);

    // matchStallTrick: the stall's trick (a stall asked for by a trick starts
    // its own): the first of the stall table's rows the recent directions
    // match, as matchTrick does; none, the plain stall trick.
    ee::Ptr<GameObject> matchStallTrick(ee::Ptr<Player> player)
    {
        const Controls &controls = player->controls;
        if (controls.historyCount != 0)
        {
            HistoryMatch matches(controls);
            for (const TrickButtonEntry &entry : *g_stallTricks)
            {
                if (matches(entry) && trickUnlocked(player, entry))
                    return entry.create();
            }
        }
        return newHandPlant();
    }
    READABLE_FUNCTION(matchStallTrick, Verified);

    // matchAirTrick: the air trick the recent stick directions make: the first
    // of the air table's rows they match, as matchTrick does, makes a new air
    // trick (trickRelease queues it); none matching, 0.
    ee::Ptr<GameObject> matchAirTrick(ee::Ptr<Player> player)
    {
        const Controls &controls = player->controls;
        if (controls.historyCount == 0)
            return 0;
        HistoryMatch matches(controls);
        for (const TrickButtonEntry &entry : *g_airTricks)
        {
            if (matches(entry) && trickUnlocked(player, entry))
                return entry.create();
        }
        return 0;
    }
    READABLE_FUNCTION(matchAirTrick, Verified);

    namespace
    {
        // A mask word: 0xFF over each pattern byte in use.
        uint32_t maskOfPattern(const uint8_t *pattern)
        {
            uint32_t mask = 0;
            for (uint32_t byte = 0; byte < sizeof(uint32_t); ++byte)
            {
                if (pattern[byte] != 0)
                    mask |= 0xFFu << (byte * 8);
            }
            return mask;
        }

        template <uint32_t Count>
        void fillTrickMasks(TrickButtonEntry (&table)[Count])
        {
            for (TrickButtonEntry &entry : table)
            {
                entry.mask.words[0] = maskOfPattern(&entry.pattern.directions[0]);
                entry.mask.words[1] = maskOfPattern(&entry.pattern.directions[4]);
            }
        }
    }

    // initTrickMasks: the masks of the four trick tables, from their patterns
    // (at start-up, registerDebugMenuFunctions).
    void initTrickMasks()
    {
        fillTrickMasks(*g_airTricks);
        fillTrickMasks(*g_trickButtons);
        fillTrickMasks(*g_manualTricks);
        fillTrickMasks(*g_stallTricks);
    }
    READABLE_FUNCTION(initTrickMasks, Verified);

    namespace
    {
        // The first `length` directions are the pattern, as far as it goes.
        bool startsWithPattern(const uint8_t *history, const uint8_t *pattern, uint32_t length)
        {
            for (uint32_t i = 0;;)
            {
                if (pattern[i] != history[i])
                    return false;
                ++i;
                if (i >= length || pattern[i] == 0)
                    return true;
            }
        }
    }

    // matchManualTrick: the manual the recent stick directions make: the oldest
    // directions held longer than `timeout` do not count (at least two must
    // be left), and those left must start with a row's pattern and be no
    // longer than the pattern's room (its byte after them is 0). It makes a
    // new manual trick; none matching, 0.
    ee::Ptr<GameObject> matchManualTrick(ee::Ptr<Player> player, ee::Float timeout)
    {
        const Controls &controls = player->controls;
        uint32_t length = controls.historyCount;
        if (length < 2)
            return 0;
        while (timeout < controls.historyHeld[length - 1])
        {
            --length;
            if (length < 2)
                return 0;
        }
        for (const TrickButtonEntry &entry : *g_manualTricks)
        {
            const uint8_t *pattern = entry.pattern.directions;
            if (length < 8 && pattern[length] != 0)
                continue;
            if (startsWithPattern(controls.history, pattern, length) && trickUnlocked(player, entry))
                return entry.create();
        }
        return 0;
    }
    READABLE_FUNCTION(matchManualTrick, Verified);

    // releasedWithinAcidDropTimeout: whether the layout's button 3 was let go within Control /
    // Acid Drop Timeout. Nothing calls it.
    int32_t releasedWithinAcidDropTimeout(ee::Ptr<Controls> controls)
    {
        const bool within = controls->releasedTime < (*g_tuningCategories)[5].entries[7].value;
        return within ? 1 : 0;
    }
    READABLE_FUNCTION(releasedWithinAcidDropTimeout, Verified);

    // trickMove: the move a trick asks of the skater's state.
    int32_t trickMove(ee::Ptr<GameObject> trick)
    {
        const uint32_t type = trickState(trick)->type;
        if (type == kTypeHandGrind)
            return kMoveHandGrind;
        if (type == kTypePastGrinds)
            return kMovePastGrinds;
        if (type == kTypeDoubleGrind)
            return kMoveDoubleGrind;
        return kMoveGrind;
    }
    READABLE_FUNCTION(trickMove, Verified);

    // setTrickSide: the trick's side (and its stance too for kind 1).
    int32_t setTrickSide(ee::Ptr<GameObject> trick, uint32_t side)
    {
        const ee::Ptr<TrickState> state = trickState(trick);
        state->side = side;
        if (state->kind == 1)
            state->stance = side;
        return 1;
    }
    READABLE_FUNCTION(setTrickSide, Verified);

    // setTrickFakie: the trick's fakie flag.
    int32_t setTrickFakie(ee::Ptr<GameObject> trick, int32_t fakie)
    {
        const ee::Ptr<TrickState> state = trickState(trick);
        state->flags &= ~kTrickFakie;
        if (fakie)
            state->flags |= kTrickFakie;
        return 1;
    }
    READABLE_FUNCTION(setTrickFakie, Verified);

    // setTrickStance: the trick's stance.
    int32_t setTrickStance(ee::Ptr<GameObject> trick, uint32_t stance)
    {
        trickState(trick)->stance = stance;
        return 1;
    }
    READABLE_FUNCTION(setTrickStance, Verified);

    // startTrick: the trick becomes the skater's current action object
    // if it may start: the one before is told it ends, the new one that it
    // begins. 1 when it started.
    int32_t startTrick(ee::Ptr<Player> player, ee::Ptr<GameObject> trick)
    {
        if (!trick)
            return 0;
        ee::Local<uint32_t> mayStart;
        *mayStart = 0;
        sendObjectMessage(trick, *g_msgAreYouPossible, player, mayStart);
        if (!*mayStart)
            return 0;
        if (player->hasCurrentObject)
        {
            const ee::Ptr<GameObject> current = skaterCurrentObject(player);
            sendObjectMessage(current, *g_msgTrickFinished, player, 0);
            player->hasCurrentObject = 0;
        }
        putObjectInSlot(player->currentObjectSlot, trick);
        player->hasCurrentObject = 1;
        sendObjectMessage(trick, *g_msgTrickStarted, player, 0);
        return 1;
    }
    READABLE_FUNCTION(startTrick, Verified);

    // sameTrick: whether two tricks are the same: the same type, stance
    // and flags (the side is not compared). A trick asked for on top of the
    // same one is dropped (trickInput).
    int32_t sameTrick(ee::Ptr<GameObject> a, ee::Ptr<GameObject> b)
    {
        const ee::Ptr<TrickState> first = trickState(a);
        const ee::Ptr<TrickState> second = trickState(b);
        return first->type == second->type && first->stance == second->stance && first->flags == second->flags;
    }
    READABLE_FUNCTION(sameTrick, Verified);

    namespace
    {
        using TrickCombiner = ee::FunctionPtr<int32_t(ee::Ptr<GameObject> trick, ee::Ptr<GameObject> current, uint32_t matched)>;
        constexpr TrickCombiner kSlideOnSlide{sym::combineSlideOnSlide};
        constexpr TrickCombiner kSoulOnSlide{sym::combineSoulOnSlide};
        constexpr TrickCombiner kOtherOnSlide{sym::combineOtherOnSlide};
        constexpr TrickCombiner kSlideOnSoul{sym::combineSlideOnSoul};
        constexpr TrickCombiner kSoulOnSoul{sym::combineSoulOnSoul};
        constexpr TrickCombiner kOtherOnSoul{sym::combineOtherOnSoul};

    }

    // combineGrindTrick: a grind trick asked for while grinding with another
    // trick current: the new trick takes over the current one's stance and
    // side (each of the six combiners copies them its own way, by the two
    // tricks' kinds, with the match result's bits). Their result (1 when
    // combined); 0 for a current trick of another kind.
    int32_t combineGrindTrick(ee::Ptr<GameObject> trick, ee::Ptr<GameObject> current, uint32_t matched)
    {
        const uint32_t kind = trickState(trick)->kind;
        const uint32_t currentKind = trickState(current)->kind;
        if (currentKind == 1)
        {
            const TrickCombiner combine = kind == 1 ? kSlideOnSlide : kind == 2 ? kSoulOnSlide : kOtherOnSlide;
            return combine(trick, current, matched);
        }
        if (currentKind == 0 || currentKind == 2)
        {
            const TrickCombiner combine = kind == 1 ? kSlideOnSoul : kind == 2 ? kSoulOnSoul : kOtherOnSoul;
            return combine(trick, current, matched);
        }
        return 0;
    }
    READABLE_FUNCTION(combineGrindTrick, Verified);

    namespace
    {
        // A trick the skater did not get goes back to its pool. Its handler
        // may answer into `answer`, which nothing reads.
        void dropTrick(ee::Ptr<Player> player, ee::Ptr<GameObject> trick, ee::Ptr<uint32_t> answer)
        {
            sendObjectMessage(trick, *g_msgTrickDroppedFromQueue, player, answer);
            releaseGameObject(trick);
        }

        // The trick is the skater's now: the directions and the delay start again.
        void beginTrick(ee::Ptr<Player> player, ee::Ptr<GameObject> trick)
        {
            Controls &controls = player->controls;
            startTrick(player, trick);
            controls.trickDelay = -1;
            controls.historyCount = 0;
            zge::memoryFill(&controls.history, 0, sizeof(controls.history));
        }

        // A trick with no trick under it takes the skater's side, fakie and
        // stance.
        void matchTrickToSkater(ee::Ptr<Player> player, ee::Ptr<GameObject> trick)
        {
            uint32_t side = 3; // on the left, or on neither
            if (player->side != 0)
                side = player->side == 1 ? 2 : 0;
            setTrickSide(trick, side);
            setTrickFakie(trick, player->fakie);
            const uint32_t kind = trickState(trick)->kind;
            if (kind == 1)
            {
                uint32_t stance = side;
                if (player->fakie)
                    stance = side == 2 ? 3 : 2;
                setTrickStance(trick, stance);
            }
            else if (kind == 0 || kind == 2)
            {
                setTrickStance(trick, player->fakie ? 4u : 1u);
            }
        }
    }

    // trickInput: the trick the directions made, asked of the skater's state;
    // `fresh` is 1 from the controls (0 leaves the Double Grind out).
    void trickInput(ee::Ptr<Player> player, int32_t fresh)
    {
        ee::Local<uint32_t> matched;
        ee::Local<uint32_t> mayStart;
        ee::Local<uint32_t> answer;
        ee::Local<uint32_t> lastAnswer;

        int32_t excludeDoubleGrind = fresh == 0;
        ee::Ptr<GameObject> trick;
        for (;;)
        {
            *matched = 0;
            trick = matchTrick(player, matched, excludeDoubleGrind);
            const ee::Ptr<GameObject> current = skaterCurrentObject(player);
            if (!trick)
                return;

            const bool grinding = player->state >= State::Grind && player->state <= State::DoubleGrind;
            *mayStart = 1;
            *answer = 0;
            bool combined = false;
            if (current)
            {
                if (grinding && gameObjectKind(current) == kObjectTrick)
                {
                    combined = true;
                    combineGrindTrick(trick, current, *matched);
                }
                // A trick on top of the same trick does not start again.
                if (gameObjectKind(current) == kObjectTrick)
                    *mayStart = sameTrick(current, trick) == 0;
            }
            if (*mayStart)
                sendObjectMessage(trick, *g_msgAreYouPossible, player, mayStart);
            if (*mayStart && current)
                sendObjectMessage(current, *g_msgAreYouFinished, player, mayStart);
            if (!*mayStart)
                break;

            const State stateBefore = player->state;
            const int32_t move = trickMove(trick);
            const int32_t entered = requestMove(player, move);
            if (move == kMoveDoubleGrind && entered != static_cast<int32_t>(State::DoubleGrind))
            {
                // No Double Grind here: the directions are matched again
                // without it.
                dropTrick(player, trick, answer);
                excludeDoubleGrind = 1;
                continue;
            }
            if (entered == static_cast<int32_t>(State::Stall))
            {
                // A stall: its own trick instead. It starts even if it says
                // it may not, and then goes back all the same.
                dropTrick(player, trick, answer);
                trick = matchStallTrick(player);
                sendObjectMessage(trick, *g_msgAreYouPossible, player, mayStart);
                beginTrick(player, trick);
                if (*mayStart)
                    return;
                break;
            }
            if (entered == kRefused && moveOfState(stateBefore) != move)
                break;
            if (!combined)
                matchTrickToSkater(player, trick);
            beginTrick(player, trick);
            return;
        }
        // Dropped.
        *lastAnswer = 0;
        dropTrick(player, trick, lastAnswer);
    }
    READABLE_FUNCTION_WAITS(trickInput, Verified);

    // trickButtonsUpdate: the controls' trick step. A press of the trick button
    // (still held) starts "# cmd delay frames" running down; when it is out the
    // trick input runs, and while the button stays held the input waits for
    // the next delay (-1: none). The direction history holds still while a
    // delay runs down.
    void trickButtonsUpdate(ee::Ptr<Player> player)
    {
        Controls &controls = player->controls;
        const auto button = [&] { return (*g_buttonLayouts)[controls.layout].buttons[kTrickButton]; };

        int8_t frames = controls.trickDelay;
        if (frames <= 0)
        {
            if (padPressed(button(), controls.port) && padHeld(button(), controls.port))
                controls.trickDelay = static_cast<int8_t>(ee::toInt(commandDelayFrames()));
            frames = controls.trickDelay;
            if (frames == 0)
            {
                trickInput(player, 1);
                if (padHeld(button(), controls.port))
                    frames = controls.trickDelay;
                else
                    frames = controls.trickDelay = -1;
            }
        }
        if (frames > 0)
            controls.trickDelay = frames = static_cast<int8_t>(frames - 1);
        controls.holdHistory = frames >= 0;
    }
    READABLE_FUNCTION(trickButtonsUpdate, Verified);
}
