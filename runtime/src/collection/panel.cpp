// The panel: the map with the second HUD over it, and the references (goals,
// items, tricks) beside it, by touch alone. Where things lie: panel_layout.h.
//
// What the map shows is one thing at a time, so a glance answers "where do I
// go": the goal the game is running for the player (its targets: what to
// pick up, where to bring it, where to be); with none running, the goals
// that can be started now (their givers, the nearest ringed) with every
// running goal's places small in the goal's colour. The level's categories
// (gaps, coins, stat points, keys: every running goal's places and what lies
// about) have a view of their own, the items screen, and never mix into the
// goal's markers. Each goal's state (active, ready, running, locked, done)
// and progress come from the snapshot: the game's own state, read from its
// RAM (panel.h Snapshot).

#include "collection/panel.h"
#include "collection/panel_canvas.h"
#include "collection/panel_file.h"
#include "collection/panel_layout.h"
#include "collection/panel_policy.h"
#include "collection/panel_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace panel
{
    namespace
    {
        constexpr float kRowHeight = kGoalRowHeight;
        constexpr float kDragThreshold = 18.0f;
        constexpr float kIconTapRadius = 46.0f;
        constexpr float kIconRadius = 21.0f;

        // A reference's header: the tabs, hold and close.
        constexpr float kHeaderHeight = 92.0f;
        constexpr Rect kGoalsTab{kSidePane.x + 6, 8, 112, 76};
        constexpr Rect kItemsTab{kSidePane.x + 122, 8, 112, 76};
        constexpr Rect kTricksTab{kSidePane.x + 238, 8, 112, 76};
        constexpr Rect kHoldButton{kSidePane.x + 356, 8, 70, 76};
        constexpr Rect kCloseButton{kSidePane.x + 430, 8, 64, 76};

        constexpr double kBankHoldSeconds = 2.5;   // a banked line stays this long
        constexpr double kProgressHoldSeconds = 4.0;

        constexpr uint32_t kInk = rgba(244, 240, 226);
        constexpr uint32_t kDim = rgba(150, 156, 170);
        constexpr uint32_t kPanelBack = rgba(16, 20, 28);
        constexpr uint32_t kRowBack = rgba(26, 32, 44);
        constexpr uint32_t kRowSelected = rgba(44, 58, 84);
        constexpr uint32_t kGlass = rgba(10, 13, 19, 199);      // the cards over the map (78 %)
        constexpr uint32_t kGlassRim = rgba(244, 240, 226, 46);  // a faint rim, so a card reads over the empty background too
        constexpr uint32_t kBarBack = rgba(26, 32, 44, 235);    // the buttons along the bottom edge (92 %)
        constexpr uint32_t kBarTrack = rgba(38, 44, 58);        // a progress bar's track
        constexpr uint32_t kAccent = rgba(255, 196, 64);
        constexpr uint32_t kReadyInk = rgba(255, 140, 70);  // a goal that can be started, and its giver
        constexpr uint32_t kDoneInk = rgba(92, 200, 110);
        constexpr uint32_t kLockedInk = rgba(96, 102, 116);
        constexpr uint32_t kPickupInk = rgba(90, 210, 255);
        constexpr uint32_t kDropOffInk = rgba(255, 196, 64);
        constexpr uint32_t kPlaceInk = rgba(120, 220, 130);

        // A goal's look by its type word (the objective's +0x10).
        struct GoalLook
        {
            uint32_t colour;
            const char *icon;
        };
        GoalLook lookOf(uint8_t type)
        {
            switch (type)
            {
            case 1: return {rgba(255, 196, 64), "AttSpeed"};
            case 4: return {rgba(120, 220, 130), "AttWallride"};
            case 5: return {rgba(90, 210, 255), "AttGrind"};
            case 6: return {rgba(90, 210, 255), "AttGrind"};
            case 7: return {rgba(255, 140, 70), "AttJump"};
            default: return {rgba(210, 150, 255), "AttSpin"};
            }
        }

        // What a goal is to the player now (from its state word).
        enum class GoalState : uint8_t
        {
            Locked, // not offered yet: something else comes first
            Ready,  // can be started now: its giver shows
            Open,   // running
            Done,
        };
        GoalState goalStateOf(uint32_t word)
        {
            return word == kStateComplete ? GoalState::Done : word == kStateInit ? GoalState::Locked : word == kStateReady ? GoalState::Ready : GoalState::Open;
        }
        // The list's group a goal that is not the one under way falls in.
        GoalGroup groupOf(GoalState state)
        {
            switch (state)
            {
            case GoalState::Ready: return GoalGroup::CanStart;
            case GoalState::Open: return GoalGroup::Running;
            case GoalState::Done: return GoalGroup::Done;
            default: return GoalGroup::Locked;
            }
        }
        uint32_t groupInk(GoalGroup group)
        {
            switch (group)
            {
            case GoalGroup::Now: return kAccent;
            case GoalGroup::CanStart: return kReadyInk;
            case GoalGroup::Done: return kDoneInk;
            case GoalGroup::Locked: return kLockedInk;
            default: return kInk;
            }
        }

        // 12345 as "12,345", as the game's score does.
        std::string grouped(int value)
        {
            std::string digits = std::to_string(value < 0 ? -value : value);
            for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3)
                digits.insert(static_cast<size_t>(at), ",");
            return value < 0 ? "-" + digits : digits;
        }

        constexpr float kFamilyRowHeight = 120.0f; // two rows of three
        constexpr float kTrickRowHeight = 64.0f;
        // A family of the tricks pane's list, in its row of buttons.
        constexpr const char *kFamilyNames[] = {"GRABS", "FLIPS", "COMBOS", "GRINDS", "MORE", "SPECIAL"};
        constexpr int kFamilies = static_cast<int>(TrickFamily::Count);

        struct Pointer
        {
            int32_t id = -1;
            Point start, now, last;
        };

        enum class Gesture : uint8_t
        {
            None,
            MapTap,
            MapDrag,
            Pinch,
            ListTap,
            ListDrag,
        };

        // A goal icon where the last draw put it, for taps.
        struct Hit
        {
            Point at;
            int goal;
        };

        // A place a goal is at, as the map draws it.
        struct Place
        {
            Point world;
            TargetRole role;
            TargetPhase phase;
        };
    }

    struct Panel::State
    {
        std::string folder;
        UiFile ui;
        bool uiLoaded = false;
        LevelFile level;
        std::string levelName;  // the file loaded, or tried
        bool levelLoaded = false;

        MapView view;
        ScreenPolicy policy;
        bool wholeForPause = false;  // the view went whole by itself; resuming restores it
        float followZoom = 1.0f;
        int selectedGoal = -1;
        bool moveToSelected = false;
        bool scrollToSelected = false;
        GameMode lastPaint = GameMode::NoLevel;
        Point nearestPoint;                 // where nearestGoal found it
        float listScroll = 0.0f;
        float listHeight = 0.0f;
        bool lockedOpen = false;            // the goal list's LOCKED group unfolded
        TrickFamily family = TrickFamily::Grab;

        // The second HUD's memory of the last frames.
        std::vector<int32_t> lastCount;     // a goal's count, to see it step
        std::vector<uint32_t> lastState;    // a goal's state, to see it finish
        int progressGoal = -1;              // the goal that just moved
        double progressUntil = 0;
        bool progressDone = false;
        std::string bankedLine;             // the last line, shown a moment after it banks
        int bankedScore = 0;
        double bankedUntil = 0;
        std::string lastLine;
        int lastLineScore = 0;

        Pointer pointers[2];
        Gesture gesture = Gesture::None;

        std::vector<int> rowGoals;     // the list's goals, top to bottom (-1: a header, or the items' last row)
        std::vector<GoalRow> listRows; // the goal list's rows with their groups
        std::vector<GoalEntry> listEntries;
        std::vector<float> rowHeights;
        std::vector<Hit> hits;
        std::vector<Point> hitPoints;

        // The map's frame: the whole screen on the live screen, the left
        // 740 px beside a reference.
        const Rect &mapPane() const { return policy.isReference() ? kSplitMapPane : kScreen; }
        const Rect &mapClear() const { return policy.isReference() ? kSplitMapClear : kLiveMapClear; }
        const Rect &returnButton() const { return policy.isReference() ? kSplitReturnButton : kLiveReturnButton; }
        const Rect &returnTouch() const { return policy.isReference() ? kSplitReturnTouch : kLiveReturnTouch; }

        const Font *body = nullptr;
        const Font *title = nullptr;
        const Font *numbers = nullptr;
        int bodyEm = 1, titleEm = 1, numbersEm = 1; // what a size in pixels scales against (emOf)

        // The font's em for sizing: its line height, or its tallest glyph
        // where one stands taller (DM2's score font has 19 px digits on a
        // 15 px line), so a size asked for in pixels is never overshot.
        static int emOf(const Font &font)
        {
            int em = font.lineHeight;
            for (int c = '0'; c <= 'Z'; ++c)
                if (font.has[c])
                    em = std::max(em, static_cast<int>(font.glyphs[c].height));
            return std::max(em, 1);
        }
        float textScale(const Font &font, float pixels) const
        {
            return scaleFor(&font == numbers ? numbersEm : &font == title ? titleEm : bodyEm, pixels);
        }

        const Image *collectableArt(uint8_t kind) const
        {
            static constexpr const char *kStats[] = {"AttJump", "AttSpeed", "AttSpin", "AttManual", "AttGrind", "AttWallride", "AttFakie"};
            static constexpr const char *kKeys[] = {"KeyAirfield", "KeyBoardwalk", "KeyCannery", "KeyCivic", "KeyIndustrial", "KeyMovie", "KeyMuseum", "KeyZoo"};
            static constexpr const char *kHeads[] = {"head_shane", "head_jaren", "head_chris", "head_franky", "head_matt", "head_sven", "head_taig"};
            if (kind >= 4 && kind <= 10)
                return icon(kStats[kind - 4]);
            if (kind >= 11 && kind <= 18)
                return icon(kKeys[kind - 11]);
            if (kind >= 19 && kind <= 26)
                return icon("level_lock");
            if (kind >= 50 && kind <= 56)
                return icon(kHeads[kind - 50]);
            return nullptr;
        }

        const Image *icon(const char *name) const
        {
            const auto found = ui.icons.find(name);
            return found == ui.icons.end() ? nullptr : &found->second;
        }

        void loadUi()
        {
            if (uiLoaded)
                return;
            uiLoaded = true;
            loadUiFile(folder + "/UI.pmap", ui);
            const auto font = [&](const char *name) -> const Font * {
                const auto found = ui.fonts.find(name);
                return found == ui.fonts.end() ? nullptr : &found->second;
            };
            body = font("fnt_14pt");
            title = font("fnt_e_12pt");
            numbers = font("fnt_ovr_score");
            if (!title)
                title = body;
            if (!numbers)
                numbers = body;
            bodyEm = body ? emOf(*body) : 1;
            titleEm = title ? emOf(*title) : 1;
            numbersEm = numbers ? emOf(*numbers) : 1;
        }

        void loadLevel(const char *name)
        {
            if (levelName == name)
                return;
            levelName = name;
            levelLoaded = false;
            selectedGoal = -1;
            listScroll = 0;
            lockedOpen = false;
            view = MapView{};
            lastCount.clear();
            lastState.clear();
            progressGoal = -1;
            level = LevelFile{};
            if (name[0])
                levelLoaded = loadLevelFile(folder + "/" + levelName + ".pmap", level);
        }

        // The level file's goals for the script the game has loaded (AI: the
        // PS2 disc's or the Xbox release's): its words are where the game's are.
        void useLoadedScript(const Snapshot &s)
        {
            if (!levelLoaded || level.goalSets.empty() || s.objectiveCount == 0)
                return;
            const int set = matchingGoalSet(level, s.scriptVariableBytes, &s.objectiveText[0][0], s.objectiveCount);
            if (set == level.goalSet)
                return;
            useGoalSet(level, set);
            lastCount.clear();
            lastState.clear();
            progressGoal = -1;
        }

        // The career script's goals; a free skate script has its own (none to speak of).
        bool goalsMatch(const Snapshot &s) const { return s.objectiveCount == level.goals.size(); }

        GoalState stateOf(const Snapshot &s, int goal) const { return goalStateOf(s.objectiveState[goal]); }

        // Done once: done now, or running again after it was (a repeatable job).
        bool doneMarked(const Snapshot &s, int goal) const
        {
            return stateOf(s, goal) == GoalState::Done ||
                   (goal >= 0 && goal < static_cast<int>(kMaxObjectives) && ((s.objectiveDoneMarks >> goal) & 1u));
        }

        bool goalShown(const Snapshot &s, int goal) const
        {
            if (goal >= static_cast<int>(s.objectiveCount))
                return false;
            const GoalRecord &g = level.goals[goal];
            const bool done = doneMarked(s, goal);
            if (g.tier == 3 && !done)
                return false;                 // a hidden challenge: listed once found
            const bool keyHeld = s.level >= 0 && ((s.levelFlags >> (s.level + 9)) & 1u) != 0;
            return !(g.lockedArea && keyHeld == false);
        }

        // The giver of a goal that can be started (a bubble with no script
        // word, by the goal's owner), where the level puts it; false: none.
        bool giverOf(const Snapshot &s, int goal, Point &world) const
        {
            const GoalRecord &g = level.goals[static_cast<size_t>(goal)];
            for (const BubbleRecord &b : level.bubbles)
                if (b.stateWord < 0 && b.nameHash == g.ownerHash)
                {
                    world = objectWorld(s, TargetRecord{b.nameHash, b.x, b.z, -1, 0, 0});
                    return true;
                }
            return false;
        }

        float distanceTo(const Snapshot &s, Point world) const { return std::hypot(world.x - s.x, world.y - s.z); }

        // The goal list's rows: five groups with a header each (panel_layout.h
        // GoalGroup): the goal under way, those that can be started with the
        // nearest giver first, the running ones, the done ones, the locked
        // ones folded into their header until it is tapped.
        void buildRows(const Snapshot &s)
        {
            rowGoals.clear();
            listRows.clear();
            listEntries.clear();
            if (!goalsMatch(s))
                return;
            const int count = static_cast<int>(std::min<size_t>(level.goals.size(), s.objectiveCount));
            const int now = shownGoal(s);
            for (int i = 0; i < count; ++i)
            {
                if (!goalShown(s, i))
                    continue;
                const GoalState state = stateOf(s, i);
                GoalEntry e{i, i == now && state != GoalState::Done ? GoalGroup::Now : groupOf(state), 1e9f};
                Point giver;
                if (e.group == GoalGroup::CanStart && giverOf(s, i, giver))
                    e.distance = distanceTo(s, giver);
                listEntries.push_back(e);
            }
            listRows = groupRows(listEntries, lockedOpen);
            for (const GoalRow &r : listRows)
                rowGoals.push_back(r.goal);
        }

        Point objectWorld(const Snapshot &s, const TargetRecord &t) const
        {
            size_t lo = 0, hi = s.trackedCount;
            while (lo < hi)
            {
                const size_t mid = (lo + hi) / 2;
                if (s.tracked[mid].nameHash < t.nameHash)
                    lo = mid + 1;
                else
                    hi = mid;
            }
            if (lo < s.trackedCount && s.tracked[lo].nameHash == t.nameHash)
                return {s.tracked[lo].x, s.tracked[lo].z};
            return {t.x, t.z};
        }

        bool targetDone(const Snapshot &s, const TargetRecord &t) const
        {
            return t.doneWord >= 0 && static_cast<uint32_t>(t.doneWord) < s.scriptWordCount &&
                   s.scriptWords[t.doneWord] == t.doneValue;
        }

        // The role the level file gives a target of that name, else `fallback`.
        TargetRole roleOf(const GoalRecord &g, uint32_t nameHash, TargetRole fallback) const
        {
            for (uint32_t t = 0; nameHash && t < g.targetCount; ++t)
            {
                const TargetRecord &record = level.targets[g.firstTarget + t];
                if (record.nameHash == nameHash && record.role <= static_cast<uint32_t>(TargetRole::Place))
                    return static_cast<TargetRole>(record.role);
            }
            return fallback;
        }

        // Each place a goal is at: the game's own targets this frame when it
        // has any; else the level file's, else where the level puts the
        // goal, else what the skater takes for it (a key lying about tells a
        // helper object: the prop names it). fn(Place).
        template <typename F> void forGoalPlaces(const Snapshot &s, int goal, F &&fn) const
        {
            const GoalRecord &g = level.goals[static_cast<size_t>(goal)];
            bool live = false;
            for (uint32_t i = 0; i < s.goalTargetCount; ++i)
            {
                const GoalTarget &t = s.goalTargets[i];
                if (t.goal != goal)
                    continue;
                live = true;
                fn(Place{{t.x, t.z}, roleOf(g, t.nameHash, t.role), t.phase});
            }
            if (live)
                return;
            for (uint32_t t = 0; t < g.targetCount; ++t)
            {
                const TargetRecord &record = level.targets[g.firstTarget + t];
                const TargetRole role = record.role <= static_cast<uint32_t>(TargetRole::Place) ? static_cast<TargetRole>(record.role) : TargetRole::Place;
                fn(Place{objectWorld(s, record), role, targetDone(s, record) ? TargetPhase::Done : TargetPhase::Now});
            }
            if (g.targetCount)
                return;
            if (g.anchorX == g.anchorX)
            {
                fn(Place{{g.anchorX, g.anchorZ}, TargetRole::Place, TargetPhase::Now});
                return;
            }
            for (uint32_t i = 0; i < s.collectableCount; ++i)
                if (s.collectables[i].shown && s.collectables[i].ownerHash == g.ownerHash)
                    fn(Place{{s.collectables[i].x, s.collectables[i].z}, TargetRole::Pickup, TargetPhase::Now});
        }

        // The goal's next place: the nearest one the game asks for now;
        // false when it has none left.
        bool nextPlace(const Snapshot &s, int goal, Place &out) const
        {
            float best = 1e12f;
            bool found = false;
            forGoalPlaces(s, goal, [&](const Place &p) {
                if (p.phase != TargetPhase::Now)
                    return;
                const float d = distanceTo(s, p.world);
                if (d < best)
                {
                    best = d;
                    out = p;
                    found = true;
                }
            });
            if (!found)
                forGoalPlaces(s, goal, [&](const Place &p) {
                    if (p.phase != TargetPhase::Later)
                        return;
                    const float d = distanceTo(s, p.world);
                    if (d < best)
                    {
                        best = d;
                        out = p;
                        found = true;
                    }
                });
            return found;
        }

        // What just changed: a count stepped, a goal finished, a line banked.
        void trackProgress(const Snapshot &s, double now, bool lineActive)
        {
            if (goalsMatch(s))
            {
                const size_t count = level.goals.size();
                const bool first = lastCount.size() != count;
                if (first)
                {
                    lastCount.assign(count, 0);
                    lastState.assign(count, 0);
                }
                for (size_t i = 0; i < count; ++i)
                {
                    const int have = goalCount(s, level.goals[i]);
                    const uint32_t state = s.objectiveState[i];
                    const bool stepped = have > lastCount[i];
                    const bool finished = state == kStateComplete && lastState[i] != kStateComplete;
                    if (!first && (stepped || finished))
                    {
                        progressGoal = static_cast<int>(i);
                        progressDone = finished;
                        progressUntil = now + kProgressHoldSeconds;
                    }
                    lastCount[i] = have;
                    lastState[i] = state;
                }
            }
            if (progressGoal >= 0 && now >= progressUntil)
                progressGoal = -1;

            const int score = static_cast<int>(s.pendingScore) * std::max(s.multiplier, 1);
            if (lineActive)
            {
                lastLine = s.line;
                lastLineScore = score;
            }
            else if (!lastLine.empty())
            {
                bankedLine = lastLine;
                bankedScore = lastLineScore;
                bankedUntil = now + kBankHoldSeconds;
                lastLine.clear();
            }
        }

        // The goal that can be started whose giver is nearest, or -1.
        int nearestReadyGoal(const Snapshot &s)
        {
            if (!goalsMatch(s))
                return -1;
            int best = -1;
            float bestDistance = 1e12f;
            for (size_t i = 0; i < level.goals.size(); ++i)
            {
                Point giver;
                if (!goalShown(s, static_cast<int>(i)) || stateOf(s, static_cast<int>(i)) != GoalState::Ready || !giverOf(s, static_cast<int>(i), giver))
                    continue;
                const float d = distanceTo(s, giver);
                if (d < bestDistance)
                {
                    bestDistance = d;
                    best = static_cast<int>(i);
                    nearestPoint = giver;
                }
            }
            return best;
        }

        // The running goal nearest the skater that has a place, or -1.
        int nearestOpenGoal(const Snapshot &s)
        {
            if (!goalsMatch(s))
                return -1;
            int best = -1;
            float bestSquared = 1e12f;
            for (size_t i = 0; i < level.goals.size(); ++i)
            {
                if (!goalShown(s, static_cast<int>(i)) || stateOf(s, static_cast<int>(i)) != GoalState::Open)
                    continue;
                forGoalPlaces(s, static_cast<int>(i), [&](const Place &p) {
                    if (p.phase == TargetPhase::Done)
                        return;
                    const float dx = p.world.x - s.x, dz = p.world.y - s.z;
                    if (dx * dx + dz * dz < bestSquared)
                    {
                        bestSquared = dx * dx + dz * dz;
                        best = static_cast<int>(i);
                        nearestPoint = p.world;
                    }
                });
            }
            return best;
        }

        // The timed goal under way, or -1.
        int attemptedGoal(const Snapshot &s) const
        {
            if (!goalsMatch(s) || s.timerSeconds < 0)
                return -1;
            if (s.activeGoal >= 0 && s.activeGoal < static_cast<int>(level.goals.size()))
                return s.activeGoal;
            // (a challenge's giver sets it active as the clock starts)
            for (size_t i = 0; i < level.goals.size(); ++i)
                if (level.goals[i].scoreKind == 3 && stateOf(s, static_cast<int>(i)) == GoalState::Open)
                    return static_cast<int>(i);
            return -1;
        }

        // The goal the map and the live column are about: the game's active
        // goal, else the timed one, else one that just moved.
        int shownGoal(const Snapshot &s) const
        {
            if (!goalsMatch(s))
                return -1;
            if (s.activeGoal >= 0 && s.activeGoal < static_cast<int>(level.goals.size()))
                return s.activeGoal;
            const int attempt = attemptedGoal(s);
            return attempt >= 0 ? attempt : progressGoal;
        }

        void drawLive(Canvas &c, const Snapshot &snap, double seconds, bool lineActive);
        void drawReference(Canvas &c, const Snapshot &snap);
        void drawGoalList(Canvas &c, const Snapshot &snap, const Rect &list);
        void drawItemList(Canvas &c, const Snapshot &snap, const Rect &list);
        void drawTrickTable(Canvas &c, const Snapshot &snap, const Rect &list);
        void drawMarker(Canvas &c, Point at, const Place &place, uint32_t colour, float radius, bool emphasis, double seconds);
        void drawLegend(Canvas &c, int goal, bool givers, bool running);
        void drawWhereTile(Canvas &c, const Snapshot &snap, Point world, const char *word, uint32_t ink);
        void drawClockTile(Canvas &c, float seconds);
        void drawProgressBar(Canvas &c, const Rect &track, int have, int target);
        bool goalHasBar(const Snapshot &s, int goal, int &have, int &target) const;

        // The goal's progress as the row shows it, "" for none.
        std::string progress(const Snapshot &s, int goal, bool done) const
        {
            const GoalRecord &g = level.goals[static_cast<size_t>(goal)];
            if (done)
                return "Done";
            if (goal == s.activeGoal && s.goalStatus[0])
                return s.goalStatus;
            const int have = goalCount(s, g);
            if (have >= 0)
            {
                const int target = goalTarget(s, g);
                if (g.status && level.string(g.status)[0])
                    return statusLine(level.string(g.status), have, target);
                if (g.targetWord >= 0)
                    return "";  // a count the game itself shows no line for (a set's members done)
                return std::to_string(have) + " / " + std::to_string(target);
            }
            if (g.scoreKind == 1)
                return grouped(s.score) + " / " + grouped(g.scoreTarget);
            if (g.scoreKind == 2)
                return grouped(lineScore(s)) + " / " + grouped(g.scoreTarget);
            if (g.scoreKind == 4)
                return grouped(s.score + lineScore(s)) + " / " + grouped(g.scoreTarget);
            if (g.scoreKind == 3 && s.timerSeconds >= 0)
                return grouped(s.points) + " / " + grouped(g.scoreTarget);
            return "";
        }

        // The running line's points as the scripts get them
        // (getPotentialScoreIncrease: the pending score times the multiplier,
        // made a whole number).
        static int lineScore(const Snapshot &s)
        {
            const float points = s.pendingScore * static_cast<float>(s.multiplier);
            return points > 0.0f ? static_cast<int>(static_cast<uint32_t>(points)) : 0;
        }

        int goalCount(const Snapshot &s, const GoalRecord &g) const
        {
            if (g.counterWord < 0 || static_cast<uint32_t>(g.counterWord) >= s.scriptWordCount)
                return -1;
            const uint32_t word = s.scriptWords[g.counterWord];
            return word == 0xFFFFFFFFu ? -1 : static_cast<int>(word);
        }

        int goalTarget(const Snapshot &s, const GoalRecord &g) const
        {
            if (g.targetWord >= 0 && static_cast<uint32_t>(g.targetWord) < s.scriptWordCount)
                return static_cast<int>(s.scriptWords[g.targetWord]);
            return g.counterTarget;
        }
    };

    Panel::Panel() : state_(std::make_unique<State>()) {}
    Panel::~Panel() = default;

    void Panel::setDataFolder(const std::string &folder)
    {
        state_->folder = folder;
        state_->uiLoaded = false;
        state_->levelName.clear();
    }

    namespace
    {
        // Text wrapped to `width`, at most `most` lines; the line count.
        int wrapped(Canvas *canvas, const Font &font, const char *text, Point at, float width, float scale, float lineGap, uint32_t colour, int most)
        {
            const std::string_view all(text);
            size_t start = 0;
            int lines = 0;
            while (start < all.size() && lines < most)
            {
                size_t end = start, good = start;
                while (end <= all.size())
                {
                    size_t next = all.find(' ', end);
                    if (next == std::string_view::npos)
                        next = all.size();
                    if (Canvas::textWidth(font, all.substr(start, next - start), scale) > width && good > start)
                        break;
                    good = next;
                    if (next == all.size())
                        break;
                    end = next + 1;
                }
                if (canvas)
                    canvas->text(font, all.substr(start, good - start), {at.x, at.y + lineGap * static_cast<float>(lines)}, scale, colour);
                ++lines;
                start = good + 1;
            }
            return lines;
        }

    }

    void Panel::touch(const TouchEvent &e)
    {
        State &s = *state_;
        const Point at{e.x, e.y};
        Pointer *p = nullptr;
        for (Pointer &q : s.pointers)
            if (q.id == e.pointer)
                p = &q;
        switch (e.phase)
        {
        case TouchPhase::Down:
        {
            for (Pointer &q : s.pointers)
                if (!p && q.id < 0)
                {
                    q.id = e.pointer;
                    q.start = q.now = q.last = at;
                    p = &q;
                }
            if (!p)
                return;
            const bool both = s.pointers[0].id >= 0 && s.pointers[1].id >= 0;
            const Rect &pane = s.mapPane();
            if (both && pane.contains(s.pointers[0].start) && s.gesture != Gesture::ListTap && s.gesture != Gesture::ListDrag)
                s.gesture = Gesture::Pinch;
            else if (!s.policy.isReference() && liveButtonAt(at) >= 0)
                s.gesture = Gesture::ListTap;    // the buttons along the bottom edge lie over the map
            else if (pane.contains(at))
                s.gesture = Gesture::MapTap;
            else
                s.gesture = Gesture::ListTap;
            break;
        }
        case TouchPhase::Move:
        {
            if (!p)
                return;
            p->last = p->now;
            p->now = at;
            const float moved = std::hypot(at.x - p->start.x, at.y - p->start.y);
            if (s.gesture == Gesture::MapTap && moved > kDragThreshold)
                s.gesture = Gesture::MapDrag;
            if (s.gesture == Gesture::ListTap && moved > kDragThreshold)
                s.gesture = Gesture::ListDrag;
            if (s.gesture == Gesture::MapDrag && s.levelLoaded)
            {
                s.view.follow = false;
                s.wholeForPause = false;     // the map is the user's now
                s.moveToSelected = false;
                panBy(s.view, at.x - p->last.x, at.y - p->last.y, s.level.transform, s.mapPane());
            }
            else if (s.gesture == Gesture::ListDrag)
            {
                s.listScroll = std::max(0.0f, s.listScroll - (at.y - p->last.y));
            }
            else if (s.gesture == Gesture::Pinch && s.levelLoaded && s.pointers[0].id >= 0 && s.pointers[1].id >= 0)
            {
                const Pointer &a = s.pointers[0], &b = s.pointers[1];
                const Point a0 = p == &a ? a.last : a.now, b0 = p == &b ? b.last : b.now;
                s.view.follow = false;
                s.wholeForPause = false;
                s.moveToSelected = false;
                pinch(s.view, a0, b0, a.now, b.now, s.level.transform, s.mapClear());
            }
            break;
        }
        case TouchPhase::Up:
        {
            if (!p)
                return;
            const bool reference = s.policy.isReference();
            if (s.gesture == Gesture::MapTap)
            {
                if (!s.view.follow && !s.wholeForPause && s.returnTouch().contains(at))
                {
                    s.view.follow = true;
                    s.view.zoom = s.followZoom;
                    s.moveToSelected = false;
                }
                else
                {
                    const int hit = nearestWithin(s.hitPoints.data(), static_cast<int>(s.hitPoints.size()), at, kIconTapRadius);
                    if (hit >= 0)
                    {
                        s.selectedGoal = s.hits[static_cast<size_t>(hit)].goal;
                        s.policy.open(Screen::Goals);
                        s.listScroll = 0;
                        s.scrollToSelected = true;
                    }
                    else
                    {
                        s.selectedGoal = -1;     // a tap on the bare map: back to all the goals
                        s.moveToSelected = false;
                    }
                }
            }
            else if (s.gesture == Gesture::ListTap)
            {
                if (!reference)
                {
                    const int button = liveButtonAt(at);
                    if (button >= 0)
                    {
                        s.policy.open(button == 0 ? Screen::Goals : button == 1 ? Screen::Items : Screen::Tricks);
                        s.listScroll = 0;
                    }
                }
                else if (kCloseButton.contains(at))
                {
                    s.policy.close();
                    s.selectedGoal = -1;
                    s.moveToSelected = false;
                }
                else if (kHoldButton.contains(at))
                {
                    s.policy.togglePin();
                }
                else if (kGoalsTab.contains(at) || kItemsTab.contains(at) || kTricksTab.contains(at))
                {
                    s.policy.open(kGoalsTab.contains(at) ? Screen::Goals : kItemsTab.contains(at) ? Screen::Items : Screen::Tricks);
                    s.listScroll = 0;
                }
                else if (s.policy.screen() == Screen::Tricks)
                {
                    if (at.y >= kHeaderHeight && at.y < kHeaderHeight + kFamilyRowHeight)
                    {
                        const int column = std::clamp(static_cast<int>((at.x - kSidePane.x) / (kSidePane.width / 3.0f)), 0, 2);
                        const int line = at.y < kHeaderHeight + kFamilyRowHeight * 0.5f ? 0 : 1;
                        s.family = static_cast<TrickFamily>(line * 3 + column);
                        s.listScroll = 0;
                    }
                }
                else if (s.policy.screen() == Screen::Goals || s.policy.screen() == Screen::Items)
                {
                    const int row = rowAt(at.y, kHeaderHeight, kScreenHeight - kHeaderHeight, s.listScroll, s.rowHeights.data(), static_cast<int>(s.rowHeights.size()));
                    if (row >= 0)
                    {
                        const int goal = s.rowGoals[static_cast<size_t>(row)];
                        const bool goals = s.policy.screen() == Screen::Goals;
                        if (goals && goal < 0 && static_cast<size_t>(row) < s.listRows.size() && s.listRows[static_cast<size_t>(row)].group == GoalGroup::Locked)
                        {
                            s.lockedOpen = !s.lockedOpen;   // the LOCKED header unfolds its rows
                        }
                        else if (goal == s.selectedGoal || goal < 0)
                        {
                            s.selectedGoal = -1;
                            s.moveToSelected = false;
                        }
                        else
                        {
                            s.selectedGoal = goal;
                            s.moveToSelected = true;
                            s.view.follow = false;
                            s.wholeForPause = false;
                        }
                    }
                }
            }
            p->id = -1;
            if (s.pointers[0].id < 0 && s.pointers[1].id < 0)
                s.gesture = Gesture::None;
            else if (s.gesture == Gesture::Pinch)
                s.gesture = Gesture::MapDrag;
            break;
        }
        case TouchPhase::Cancel:
            s.pointers[0].id = s.pointers[1].id = -1;
            s.gesture = Gesture::None;
            break;
        }
    }

    namespace
    {
        // Lines of `text` wrapped to `width` as views.
        std::vector<std::string_view> wrapLines(const Font &font, std::string_view all, float width, float scale)
        {
            std::vector<std::string_view> lines;
            size_t start = 0;
            while (start < all.size())
            {
                size_t end = start, good = start;
                while (end <= all.size())
                {
                    size_t next = all.find(' ', end);
                    if (next == std::string_view::npos)
                        next = all.size();
                    if (Canvas::textWidth(font, all.substr(start, next - start), scale) > width && good > start)
                        break;
                    good = next;
                    if (next == all.size())
                        break;
                    end = next + 1;
                }
                lines.push_back(all.substr(start, good - start));
                start = good + 1;
            }
            return lines;
        }

        const char *roleWord(TargetRole role)
        {
            switch (role)
            {
            case TargetRole::Pickup: return "PICK UP";
            case TargetRole::DropOff: return "DROP OFF";
            default: return "GO TO";
            }
        }

        uint32_t roleInk(TargetRole role)
        {
            switch (role)
            {
            case TargetRole::Pickup: return kPickupInk;
            case TargetRole::DropOff: return kDropOffInk;
            default: return kPlaceInk;
            }
        }

        // A card's backing over the map: the glass with a 2 px rim (the glass
        // alone is the background's own colour off the map's picture).
        void glassBox(Canvas &c, const Rect &r, uint32_t fill)
        {
            c.roundedRect({r.x - 2, r.y - 2, r.width + 4, r.height + 4}, kCardRadius + 2, kGlassRim);
            c.roundedRect(r, kCardRadius, fill);
        }

        // A small arrow pointing `dx, dy` (screen; unit length) at `at`.
        void arrow(Canvas &c, Point at, float dx, float dy, float size, uint32_t colour)
        {
            const Point tip{at.x + dx * size, at.y + dy * size};
            const Point left{at.x - dx * size * 0.6f + dy * size * 0.55f, at.y - dy * size * 0.6f - dx * size * 0.55f};
            const Point right{at.x - dx * size * 0.6f - dy * size * 0.55f, at.y - dy * size * 0.6f + dx * size * 0.55f};
            const Point tail{at.x - dx * size * 0.25f, at.y - dy * size * 0.25f};
            c.triangle(tip, left, tail, colour);
            c.triangle(tip, tail, right, colour);
        }
    }

    // A target's marker: its shape says its role (a disc to pick up, a
    // square to drop off at, a ring to be at), its colour the role's or the
    // goal's; later steps are dim, done ones small and grey.
    void Panel::State::drawMarker(Canvas &c, Point at, const Place &place, uint32_t colour, float radius, bool emphasis, double seconds)
    {
        if (place.phase == TargetPhase::Done)
        {
            c.disc(at, radius * 0.4f + 2, rgba(10, 13, 19, 160));
            c.disc(at, radius * 0.4f, rgba(120, 124, 132, 200));
            return;
        }
        const uint32_t ink = place.phase == TargetPhase::Later ? (colour & 0x00FFFFFFu) | 0x80000000u : colour;
        const uint32_t back = rgba(10, 13, 19, place.phase == TargetPhase::Later ? 120 : 225);
        switch (place.role)
        {
        case TargetRole::Pickup:
            c.disc(at, radius + 3, back);
            c.disc(at, radius, ink);
            break;
        case TargetRole::DropOff:
            c.fill({at.x - radius - 3, at.y - radius - 3, radius * 2 + 6, radius * 2 + 6}, back);
            c.fill({at.x - radius, at.y - radius, radius * 2, radius * 2}, ink);
            break;
        default:
            c.disc(at, radius + 3, back);
            c.ring(at, radius, 4.0f, ink);
            break;
        }
        if (emphasis)
            c.ring(at, radius + 8.0f + 3.0f * std::sin(static_cast<float>(seconds) * 5.0f), 3.0f, rgba(255, 255, 255, 220));
    }

    // The key to the markers, in the map's bottom left corner (above the
    // buttons on the live screen).
    void Panel::State::drawLegend(Canvas &c, int goal, bool givers, bool running)
    {
        if (!title)
            return;
        struct Entry
        {
            const char *word;
            TargetRole role;
            uint32_t ink;
            float radius;
        };
        std::vector<Entry> entries;
        if (goal >= 0)
            entries = {{"PICK UP", TargetRole::Pickup, kPickupInk, 8}, {"DROP OFF", TargetRole::DropOff, kDropOffInk, 8}, {"GO TO", TargetRole::Place, kPlaceInk, 8}};
        else
        {
            if (givers)
                entries.push_back({"GOAL GIVER", TargetRole::Pickup, kReadyInk, 8});
            if (running)
                entries.push_back({"GOAL IN PROGRESS", TargetRole::Place, lookOf(0).colour, 5});
        }
        if (entries.empty())
            return;
        const float line = 30.0f;
        const float height = line * static_cast<float>(entries.size()) + 12;
        const float bottom = policy.isReference() ? kSplitMapPane.height - 4 : kScreen.height - kBarHeight - 8;
        const float width = policy.isReference() ? 230.0f : 300.0f;
        c.fill({4, bottom - height, width, height}, rgba(16, 20, 28, 200));
        float y = bottom - height + 6;
        for (const Entry &e : entries)
        {
            drawMarker(c, {24, y + 12}, Place{{}, e.role, TargetPhase::Now}, e.ink, e.radius, false, 0);
            c.text(*title, e.word, {44, y + 2}, 1.0f, kDim);
            y += line;
        }
    }

    // The where-to tile, bottom right of the live screen: the role's word
    // in its colour, the distance to `world` in the game's units, and an
    // arrow turned as the skater must turn to face it.
    void Panel::State::drawWhereTile(Canvas &c, const Snapshot &snap, Point world, const char *word, uint32_t ink)
    {
        if (!title || !numbers)
            return;
        const float dx = world.x - snap.x, dz = world.y - snap.z;
        const float distance = std::hypot(dx, dz);
        // the skater's heading on the ground; the target's bearing from it (clockwise on the screen)
        const float fl = std::hypot(snap.forwardX, snap.forwardZ);
        const float fx = fl > 0.01f ? snap.forwardX / fl : 0.0f, fz = fl > 0.01f ? snap.forwardZ / fl : 1.0f;
        const float ahead = distance > 0.01f ? (dx * fx + dz * fz) / distance : 1.0f;
        const float right = distance > 0.01f ? (dx * fz - dz * fx) / distance : 0.0f;
        const Rect &tile = kWhereTile;
        glassBox(c, tile, kGlass);
        const float labelScale = textScale(*title, kLabelPx);
        c.text(*title, word, {tile.x + kCardPadX, tile.y + kCardPadY}, labelScale, ink, kLabelPx * kLabelTracking);
        char text[32];
        std::snprintf(text, sizeof(text), "%d", static_cast<int>(distance + 0.5f));
        c.text(*numbers, text, {tile.x + kCardPadX, tile.y + kCardPadY + kLabelPx + 6}, textScale(*numbers, kDistancePx), kInk);
        arrow(c, {tile.x + tile.width - 72, tile.y + tile.height * 0.5f}, right, -ahead, 34, ink);
    }

    // The clock, top right, while a timed goal runs: red under ten seconds.
    void Panel::State::drawClockTile(Canvas &c, float seconds)
    {
        if (!numbers)
            return;
        // (the score font has no colon: two dots between the numbers)
        const uint32_t clockInk = seconds < 10.0f ? rgba(255, 100, 70) : kAccent;
        char minutes[8], secs[8];
        std::snprintf(minutes, sizeof(minutes), "%d", static_cast<int>(seconds) / 60);
        std::snprintf(secs, sizeof(secs), "%02d", static_cast<int>(seconds) % 60);
        const float scale = textScale(*numbers, kClockPx);
        const float gap = kClockPx * 0.36f;
        const float w0 = Canvas::textWidth(*numbers, minutes, scale), w1 = Canvas::textWidth(*numbers, secs, scale);
        Rect tile = kClockTile;
        tile.width = std::max(tile.width, w0 + gap + w1 + 40);
        tile.x = kScreen.width - kEdge - tile.width;
        glassBox(c, tile, kGlass);
        const float x = tile.x + (tile.width - w0 - gap - w1) * 0.5f;
        const float y = tile.y + (tile.height - kClockPx) * 0.5f;
        c.text(*numbers, minutes, {x, y}, scale, clockInk);
        c.disc({x + w0 + gap * 0.5f, y + kClockPx * 0.3f}, 7, clockInk);
        c.disc({x + w0 + gap * 0.5f, y + kClockPx * 0.68f}, 7, clockInk);
        c.text(*numbers, secs, {x + w0 + gap, y}, scale, clockInk);
    }

    // A goal with a count and a target gets a progress bar.
    bool Panel::State::goalHasBar(const Snapshot &s, int goal, int &have, int &target) const
    {
        const GoalRecord &g = level.goals[static_cast<size_t>(goal)];
        have = goalCount(s, g);
        target = goalTarget(s, g);
        return have >= 0 && target > 0;
    }

    void Panel::State::drawProgressBar(Canvas &c, const Rect &track, int have, int target)
    {
        c.roundedRect(track, track.height * 0.5f, kBarTrack);
        const float share = std::clamp(static_cast<float>(have) / static_cast<float>(std::max(target, 1)), 0.0f, 1.0f);
        if (share > 0)
            c.roundedRect({track.x, track.y, std::max(track.width * share, track.height), track.height}, track.height * 0.5f, kAccent);
    }

    // ---- the second HUD ----------------------------------------------------

    // The live screen: the map under everything; the goal card top left, the
    // clock top right, the where-to tile bottom right, three buttons along
    // the bottom edge. Nothing here is read for long: large type, one idea a
    // card.
    void Panel::State::drawLive(Canvas &c, const Snapshot &snap, double seconds, bool lineActive)
    {
        if (!body || !title || !numbers)
            return;
        const float cardX = kGoalCardFrame.x, cardWidth = kGoalCardFrame.width;
        const float textX = cardX + kCardPadX, textWidth = cardWidth - 2 * kCardPadX;
        const float labelScale = textScale(*title, kLabelPx), tracking = kLabelPx * kLabelTracking;
        const float titleScale = textScale(*body, kTitlePx), titleGap = kTitlePx * 1.05f;
        const float statusScale = textScale(*body, kStatusPx), statusGap = kStatusPx * 1.05f;
        float y = kGoalCardFrame.y;

        // The card: the goal the game runs for the player (its name, its
        // bar, its own status line), else the next goal to start, else the
        // nearest thing to do.
        const int attempt = attemptedGoal(snap);
        const int shown = shownGoal(snap);
        const int ready = shown < 0 ? nearestReadyGoal(snap) : -1;
        const int nearest = shown < 0 && ready < 0 ? nearestOpenGoal(snap) : -1;
        const int card = shown >= 0 ? shown : ready >= 0 ? ready : nearest;
        if (card >= 0)
        {
            const GoalRecord &g = level.goals[static_cast<size_t>(card)];
            const bool active = card == snap.activeGoal;
            const bool done = shown >= 0 && !active && attempt < 0 && progressDone;
            const char *label = shown < 0 ? (ready >= 0 ? "NEXT GOAL" : "NEAREST") : active ? "GOAL" : attempt >= 0 ? "ATTEMPT" : done ? "DONE" : "PROGRESS";
            const uint32_t labelInk = done ? kDoneInk : shown < 0 ? kDim : kAccent;
            int have = 0, target = 0;
            const bool bar = !done && shown >= 0 && goalHasBar(snap, card, have, target);
            // the status: the game's line while the goal runs; what starts a goal that can be started
            std::string status = shown >= 0 ? progress(snap, card, done) : ready >= 0 ? std::string(g.activateText ? level.string(g.activateText) : "") : progress(snap, card, false);
            if (status == "Done")
                status = "DONE";
            const auto titleLines = wrapLines(*body, level.string(g.title), textWidth, titleScale);
            const auto statusLines = status.empty() ? std::vector<std::string_view>{} : wrapLines(*body, status, textWidth, statusScale);
            const size_t titleShown = std::min<size_t>(titleLines.size(), 3), statusShown = std::min<size_t>(statusLines.size(), 3);
            const float height = kCardPadY + kLabelPx + 8 + static_cast<float>(titleShown) * titleGap + (bar ? 16 + kProgressBarHeight + 10 : 0.0f) +
                                 (statusShown ? 10 + static_cast<float>(statusShown) * statusGap : 0.0f) + kCardPadY;
            glassBox(c, {cardX, y, cardWidth, height}, done ? rgba(18, 44, 30, 210) : kGlass);
            float line = y + kCardPadY;
            c.text(*title, label, {textX, line}, labelScale, labelInk, tracking);
            line += kLabelPx + 8;
            for (size_t i = 0; i < titleShown; ++i, line += titleGap)
                c.text(*body, titleLines[i], {textX, line}, titleScale, kInk);
            if (bar)
            {
                line += 16;
                drawProgressBar(c, {textX, line, textWidth, kProgressBarHeight}, have, target);
                line += kProgressBarHeight + 10;
            }
            if (statusShown)
                line += 10;
            for (size_t i = 0; i < statusShown; ++i, line += statusGap)
                c.text(*body, statusLines[i], {textX, line}, statusScale, done ? kDoneInk : shown >= 0 ? kAccent : ready >= 0 ? kDim : kAccent);
            y += height + 12;

            // where to: the goal's next place; a goal to start: its giver
            if (shown >= 0)
            {
                Place next{};
                if (!done && nextPlace(snap, shown, next))
                    drawWhereTile(c, snap, next.world, roleWord(next.role), roleInk(next.role));
            }
            else
                drawWhereTile(c, snap, nearestPoint, ready >= 0 ? "GIVER" : "GO TO", ready >= 0 ? kReadyInk : kPlaceInk);
        }
        if (attempt >= 0)
            drawClockTile(c, snap.timerSeconds);

        // The line, under the card: the score as the HUD has it, the last
        // tricks large.
        const bool banked = !lineActive && seconds < bankedUntil && !bankedLine.empty();
        if (lineActive || banked)
        {
            char score[48];
            if (lineActive)
                std::snprintf(score, sizeof(score), "%s x %d", grouped(static_cast<int>(snap.pendingScore)).c_str(), std::max(snap.multiplier, 1));
            else
                std::snprintf(score, sizeof(score), "%s", grouped(bankedScore).c_str());
            const std::string names = lineActive ? std::string(snap.line) : bankedLine;
            const float trickScale = textScale(*body, kLineTrickPx), trickGap = kLineTrickPx * 1.1f;
            const auto lines = wrapLines(*body, names, textWidth, trickScale);
            const size_t most = 3;
            const size_t first = lines.size() > most ? lines.size() - most : 0;
            const float height = kCardPadY + kLineScorePx + 8 + static_cast<float>(lines.size() - first) * trickGap + kCardPadY;
            glassBox(c, {cardX, y, cardWidth, height}, kGlass);
            float line = y + kCardPadY;
            const float scoreScale = std::min(textScale(*numbers, kLineScorePx), textWidth / std::max(Canvas::textWidth(*numbers, score, 1.0f), 1.0f));
            c.text(*numbers, score, {textX, line}, scoreScale, lineActive ? kAccent : kDoneInk);
            line += kLineScorePx + 8;
            for (size_t i = first; i < lines.size(); ++i, line += trickGap)
                c.text(*body, lines[i], {textX, line}, trickScale, i + 1 == lines.size() ? kInk : kDim);
        }

        // The ways into the references, along the bottom edge.
        int done = 0, shownCount = 0;
        for (uint32_t i = 0; goalsMatch(snap) && i < level.goals.size(); ++i)
            if (goalShown(snap, static_cast<int>(i)))
            {
                ++shownCount;
                done += doneMarked(snap, static_cast<int>(i));
            }
        char goals[32];
        std::snprintf(goals, sizeof(goals), "GOALS %d/%d", done, shownCount);
        const float buttonScale = textScale(*title, kButtonPx), buttonTracking = kButtonPx * kLabelTracking;
        const char *words[3] = {goals, "ITEMS", "TRICKS"};
        for (int i = 0; i < 3; ++i)
        {
            const Rect &r = kLiveButtons[i];
            c.fill(r, kBarBack);
            if (i)
                c.fill({r.x, r.y + 18, 2, r.height - 36}, rgba(60, 68, 84));
            const float w = Canvas::textWidth(*title, words[i], buttonScale, buttonTracking);
            c.text(*title, words[i], {r.x + (r.width - w) * 0.5f, r.y + (r.height - kButtonPx) * 0.5f + 4}, buttonScale, kDim, buttonTracking);
        }
    }

    // ---- the references ----------------------------------------------------

    void Panel::State::drawReference(Canvas &c, const Snapshot &snap)
    {
        if (!body || !title)
            return;
        const Screen screen = policy.screen();
        int done = 0, shown = 0;
        for (uint32_t i = 0; goalsMatch(snap) && i < level.goals.size(); ++i)
            if (goalShown(snap, static_cast<int>(i)))
            {
                ++shown;
                done += doneMarked(snap, static_cast<int>(i));
            }
        char heading[32];
        std::snprintf(heading, sizeof(heading), "%d/%d", done, shown);
        const auto tab = [&](const Rect &r, const char *word, const char *under, bool on) {
            c.fill(r, on ? kRowSelected : kRowBack);
            if (on)
                c.fill({r.x, r.y + r.height - 5, r.width, 5}, kAccent);
            const float scale = std::min(1.2f, (r.width - 12) / Canvas::textWidth(*title, word, 1.0f));
            c.text(*title, word, {r.x + (r.width - Canvas::textWidth(*title, word, scale)) * 0.5f, r.y + (under ? 12.0f : 28.0f)}, scale, on ? kInk : kDim);
            if (under)
                c.text(*title, under, {r.x + (r.width - Canvas::textWidth(*title, under, 1.2f)) * 0.5f, r.y + 42}, 1.2f, on ? kAccent : kDim);
        };
        tab(kGoalsTab, "GOALS", heading, screen == Screen::Goals);
        tab(kItemsTab, "ITEMS", nullptr, screen == Screen::Items);
        tab(kTricksTab, "TRICKS", nullptr, screen == Screen::Tricks);

        // Hold keeps the reference through a line and through play; X leaves it.
        const bool held = policy.pinned();
        c.fill(kHoldButton, held ? kRowSelected : kRowBack);
        if (held)
            c.fill({kHoldButton.x, kHoldButton.y + kHoldButton.height - 5, kHoldButton.width, 5}, kAccent);
        c.text(*title, "HOLD", {kHoldButton.x + 6, kHoldButton.y + 28}, 1.0f, held ? kAccent : kDim);
        c.fill(kCloseButton, kRowBack);
        c.text(*title, "X", {kCloseButton.x + 20, kCloseButton.y + 18}, 2.0f, kInk);

        const Rect list{kSidePane.x, kHeaderHeight, kSidePane.width, kScreenHeight - kHeaderHeight};
        if (screen == Screen::Goals)
            drawGoalList(c, snap, list);
        else if (screen == Screen::Items)
            drawItemList(c, snap, list);
        else
            drawTrickTable(c, snap, list);
    }

    // The goal list: five groups under headers (NOW, CAN START, RUNNING,
    // DONE, LOCKED), no state word on a row; the goal under way open with
    // its status line and a bar; a goal that can be started with its giver's
    // distance at the right, a running one with its count (and DONE before
    // it when it was done once: a repeatable job taken again); the locked ones
    // listed dim once their header is tapped.
    void Panel::State::drawGoalList(Canvas &c, const Snapshot &snap, const Rect &list)
    {
        if (!levelLoaded)
            return;
        c.setClip(list);
        buildRows(snap);
        rowHeights.assign(listRows.size(), kRowHeight);
        const float titleScale = textScale(*body, kRowTitlePx), titleGap = kRowTitlePx * 1.05f;
        const float statusScale = textScale(*body, kRowStatusPx), statusGap = kRowStatusPx * 1.1f;
        const float textScale2 = textScale(*body, kRowTextPx), textGap = kRowTextPx * 1.1f;
        const float asideScale = textScale(*title, kRowAsidePx), asideTracking = kRowAsidePx * 0.08f;
        const float textX = list.x + 30;
        const float textWidth = list.width - 50;
        const float asideMost = 180.0f; // an aside wider than this goes under the title
        const auto rowText = [&](int goal, GoalState state) -> const char * {
            const GoalRecord &g = level.goals[static_cast<size_t>(goal)];
            if (state == GoalState::Ready && g.activateText)
                return level.string(g.activateText);
            return level.string(g.text);
        };
        // What a row says at its right: a goal to start, its giver's distance; a running one, its count.
        const auto aside = [&](const GoalEntry &e) -> std::string {
            if (e.group == GoalGroup::CanStart)
                return e.distance < 1e8f ? std::to_string(static_cast<int>(e.distance + 0.5f)) : std::string();
            if (e.group != GoalGroup::Running)
                return "";
            int have = 0, target = 0;
            if (goalHasBar(snap, e.goal, have, target))
                return std::to_string(have) + " OF " + std::to_string(target);
            return progress(snap, e.goal, false);
        };
        // A goal running again after it was done keeps its done mark: DONE
        // in the done ink before the aside.
        const auto tagged = [&](const GoalRow &row) { return row.group != GoalGroup::Done && doneMarked(snap, row.goal); };
        const char *const doneTag = "DONE";
        const float tagGap = 16.0f;
        const auto asideWidth = [&](const GoalRow &row, const std::string &right) {
            float width = right.empty() ? 0.0f : Canvas::textWidth(*title, right, asideScale, asideTracking);
            if (tagged(row))
                width += Canvas::textWidth(*title, doneTag, asideScale, asideTracking) + (right.empty() ? 0.0f : tagGap);
            return width;
        };
        const auto drawAside = [&](const GoalRow &row, const std::string &right, Point at, uint32_t ink) {
            if (tagged(row))
                at.x += c.text(*title, doneTag, at, asideScale, kDoneInk, asideTracking) + tagGap;
            if (!right.empty())
                c.text(*title, right, at, asideScale, ink, asideTracking);
        };
        const auto entryOf = [&](int goal) -> const GoalEntry & {
            for (const GoalEntry &e : listEntries)
                if (e.goal == goal)
                    return e;
            return listEntries.front();
        };
        float total = 0;
        for (size_t r = 0; r < listRows.size(); ++r)
        {
            const GoalRow &row = listRows[r];
            float height = kGroupHeaderHeight;
            if (row.goal >= 0)
            {
                const GoalRecord &g = level.goals[static_cast<size_t>(row.goal)];
                const GoalState state = stateOf(snap, row.goal);
                const std::string right = aside(entryOf(row.goal));
                const float rightWidth = asideWidth(row, right);
                const bool rightBelow = rightWidth > asideMost;
                const float titleWidth = rightWidth <= 0.0f || rightBelow ? textWidth : textWidth - rightWidth - 20;
                const int lines = wrapped(nullptr, *body, level.string(g.title), {}, titleWidth, titleScale, titleGap, 0, 2);
                int more = rightBelow ? 1 : 0;
                bool bar = false;
                if (row.group == GoalGroup::Now)
                {
                    const std::string status = progress(snap, row.goal, false);
                    if (!status.empty())
                        more += wrapped(nullptr, *body, status.c_str(), {}, textWidth, statusScale, statusGap, 0, 2);
                    int have = 0, target = 0;
                    bar = goalHasBar(snap, row.goal, have, target);
                }
                float moreHeight = static_cast<float>(more) * statusGap;
                if (row.goal == selectedGoal && rowText(row.goal, state)[0])
                    moreHeight += static_cast<float>(wrapped(nullptr, *body, rowText(row.goal, state), {}, textWidth, textScale2, textGap, 0, 6)) * textGap + 8;
                height = goalRowHeight(lines, titleGap, 0, 0, bar) + moreHeight;
            }
            rowHeights[r] = height;
            total += height;
        }
        listHeight = total;
        if (scrollToSelected)
        {
            float top = 0;
            for (size_t r = 0; r < listRows.size() && listRows[r].goal != selectedGoal; ++r)
                top += rowHeights[r];
            listScroll = std::max(0.0f, top - kGroupHeaderHeight);
            scrollToSelected = false;
        }
        listScroll = std::min(listScroll, std::max(0.0f, total - list.height));
        float y = list.y - listScroll;
        for (size_t r = 0; r < listRows.size(); ++r)
        {
            const GoalRow &row = listRows[r];
            const float height = rowHeights[r];
            if (y + height >= list.y && y < list.y + list.height)
            {
                if (row.goal < 0)
                {
                    // the group's header: its word, its count, a note
                    const int count = groupCount(listEntries, row.group);
                    float x = list.x + 24;
                    const float wordScale = textScale(*title, kHeaderPx);
                    x += c.text(*title, groupWord(row.group), {x, y + 24}, wordScale, kInk, kHeaderPx * 0.08f) + 14;
                    if (row.group != GoalGroup::Now)
                        x += c.text(*title, std::to_string(count), {x, y + 24 + (kHeaderPx - kHeaderCountPx)}, textScale(*title, kHeaderCountPx), kDim, kHeaderCountPx * 0.08f) + 14;
                    // (in the caps font: DM2's body font has no lowercase)
                    const char *note = row.group == GoalGroup::CanStart ? "NEAREST FIRST" : row.group == GoalGroup::Locked ? (lockedOpen ? "TAP TO HIDE" : "TAP TO SHOW") : "";
                    if (note[0])
                        c.text(*title, note, {x, y + 24 + (kHeaderPx - kHeaderNotePx)}, textScale(*title, kHeaderNotePx), kDim, kHeaderNotePx * 0.08f);
                    c.fill({list.x + 12, y + height - 4, list.width - 24, 2}, rgba(44, 50, 64));
                }
                else
                {
                    const GoalRecord &g = level.goals[static_cast<size_t>(row.goal)];
                    const GoalState state = stateOf(snap, row.goal);
                    const bool selected = row.goal == selectedGoal;
                    const uint32_t bar = row.group == GoalGroup::Running ? lookOf(g.type).colour : groupInk(row.group);
                    c.fill({list.x + 6, y + 3, list.width - 12, height - 6}, selected ? kRowSelected : kRowBack);
                    c.fill({list.x + 6, y + 3, 8, height - 6}, bar);
                    const bool dim = row.group == GoalGroup::Done || row.group == GoalGroup::Locked;
                    const uint32_t ink = dim ? kDim : kInk;
                    const std::string right = aside(entryOf(row.goal));
                    const float rightWidth = asideWidth(row, right);
                    const bool rightBelow = rightWidth > asideMost;
                    const uint32_t rightInk = row.group == GoalGroup::CanStart ? kReadyInk : kAccent;
                    float titleWidth = textWidth;
                    if (rightWidth > 0.0f && !rightBelow)
                    {
                        const float asideY = row.group == GoalGroup::Now ? y + 16 : y + (height - kRowAsidePx) * 0.5f;
                        drawAside(row, right, {list.x + list.width - 20 - rightWidth, asideY}, rightInk);
                        titleWidth -= rightWidth + 20;
                    }
                    // a row with nothing under its title has it in the middle
                    const int titleLines = wrapped(nullptr, *body, level.string(g.title), {}, titleWidth, titleScale, titleGap, 0, 2);
                    const bool more = rightBelow || row.group == GoalGroup::Now || (selected && rowText(row.goal, state)[0]);
                    float line = more ? y + 14 : y + std::max(12.0f, (height - static_cast<float>(titleLines) * titleGap) * 0.5f);
                    line += static_cast<float>(wrapped(&c, *body, level.string(g.title), {textX, line}, titleWidth, titleScale, titleGap, ink, 2)) * titleGap;
                    if (rightBelow)
                    {
                        drawAside(row, right, {textX, line + 4}, rightInk);
                        line += statusGap;
                    }
                    if (row.group == GoalGroup::Now)
                    {
                        const std::string status = progress(snap, row.goal, false);
                        if (!status.empty())
                            line += static_cast<float>(wrapped(&c, *body, status.c_str(), {textX, line}, textWidth, statusScale, statusGap, kAccent, 2)) * statusGap;
                        int have = 0, target = 0;
                        if (goalHasBar(snap, row.goal, have, target))
                        {
                            drawProgressBar(c, {textX, line + 6, textWidth, kRowBarHeight}, have, target);
                            line += kRowBarHeight + 14;
                        }
                    }
                    if (selected && rowText(row.goal, state)[0])
                        wrapped(&c, *body, rowText(row.goal, state), {textX, line + 2}, textWidth, textScale2, textGap, rgba(190, 200, 214), 6);
                }
            }
            y += height;
        }
        c.clearClip();
    }

    // The items screen's list: each running goal that has places (a
    // category: the gaps, the coins), with its count, and what lies about.
    void Panel::State::drawItemList(Canvas &c, const Snapshot &snap, const Rect &list)
    {
        if (!levelLoaded)
            return;
        c.setClip(list);
        rowGoals.clear();
        if (goalsMatch(snap))
            for (size_t i = 0; i < level.goals.size(); ++i)
            {
                if (!goalShown(snap, static_cast<int>(i)) || stateOf(snap, static_cast<int>(i)) != GoalState::Open)
                    continue;
                int places = 0;
                forGoalPlaces(snap, static_cast<int>(i), [&](const Place &) { ++places; });
                if (places)
                    rowGoals.push_back(static_cast<int>(i));
            }
        int shownItems = 0;
        for (uint32_t i = 0; i < snap.collectableCount; ++i)
            shownItems += snap.collectables[i].shown ? 1 : 0;
        if (shownItems)
            rowGoals.push_back(-1); // the row of what lies about
        rowHeights.assign(rowGoals.size(), kRowHeight);
        const float textScale = 1.75f;
        const float textX = list.x + 24;
        float y = list.y - listScroll;
        listHeight = static_cast<float>(rowGoals.size()) * kRowHeight;
        listScroll = std::min(listScroll, std::max(0.0f, listHeight - list.height));
        if (rowGoals.empty())
            c.text(*body, "Nothing to collect here", {textX, y + 30}, textScale, kDim);
        for (size_t r = 0; r < rowGoals.size(); ++r, y += kRowHeight)
        {
            const int goal = rowGoals[r];
            const bool selected = goal >= 0 && goal == selectedGoal;
            c.fill({list.x + 6, y + 3, list.width - 12, kRowHeight - 6}, selected ? kRowSelected : kRowBack);
            if (goal < 0)
            {
                char text[48];
                std::snprintf(text, sizeof(text), "%d things lying about", shownItems);
                c.text(*body, text, {textX, y + 30}, textScale, kInk);
                continue;
            }
            const GoalRecord &g = level.goals[static_cast<size_t>(goal)];
            int left = 0, all = 0;
            forGoalPlaces(snap, goal, [&](const Place &p) {
                ++all;
                left += p.phase != TargetPhase::Done;
            });
            c.fill({list.x + 6, y + 3, 7, kRowHeight - 6}, lookOf(g.type).colour);
            const float titleWidth = wrapped(&c, *body, level.string(g.title), {textX, y + 14}, list.width - 170, textScale, 0, kInk, 1) ? list.width - 170 : 0;
            (void)titleWidth;
            std::string count = progress(snap, goal, false);
            if (count.empty())
                count = std::to_string(all - left) + " / " + std::to_string(all);
            c.text(*body, count, {list.x + list.width - 20 - Canvas::textWidth(*body, count, 1.4f), y + 52}, 1.4f, kAccent);
        }
        c.clearClip();
    }

    void Panel::State::drawTrickTable(Canvas &c, const Snapshot &snap, const Rect &list)
    {
        const float chip = list.width / 3.0f;
        for (int f = 0; f < kFamilies; ++f)
        {
            const bool on = static_cast<int>(family) == f;
            const float x = list.x + chip * static_cast<float>(f % 3);
            const float top = list.y + (f < 3 ? 0.0f : kFamilyRowHeight * 0.5f);
            c.fill({x + 3, top + 3, chip - 6, kFamilyRowHeight * 0.5f - 6}, on ? kRowSelected : kRowBack);
            const char *name = ui.familyNames.size() == static_cast<size_t>(kFamilies) ? ui.familyNames[f].name : kFamilyNames[f];
            const float w = Canvas::textWidth(*body, name, 1.3f);
            c.text(*body, name, {x + (chip - w) * 0.5f, top + 17}, 1.3f, on ? kAccent : kInk);
        }
        const float rowTop = list.y + kFamilyRowHeight;
        c.setClip({list.x, rowTop, list.width, list.y + list.height - rowTop});
        static constexpr char kDirections[9] = {0, ',', ']', '>', '\'', '.', ';', '<', '['};
        const auto buttons = ui.fonts.find("fnt_ps2buttons");
        float row = rowTop - listScroll;
        int count = 0;
        const bool perMove = snap.hasMoves && ui.moves.size() == ui.tricks.size();
        const auto moveBit = [](const uint8_t *bits, uint8_t move) { return (bits[move >> 3] >> (move & 7)) & 1u; };
        for (size_t i = 0; i < ui.tricks.size(); ++i)
        {
            const TrickRecord &t = ui.tricks[i];
            if (t.family != family)
                continue;
            bool locked = t.unlockBit && !((snap.specialTricks >> t.unlockBit) & 1u);
            if (perMove)
            {
                const TrickMove &m = ui.moves[i];
                if (m.move >= kMoveBitBytes * 8u || (m.whileAble && moveBit(snap.movesOff, m.move)))
                    continue;
                locked = !moveBit(snap.movesLanded, m.move);
            }
            ++count;
            if (row + kTrickRowHeight >= rowTop && row < list.y + list.height && buttons != ui.fonts.end())
            {
                c.fill({list.x + 6, row + 2, list.width - 12, kTrickRowHeight - 4}, kRowBack);
                const Font &glyphs = buttons->second;
                float x = list.x + list.width - 18;
                // long names give way to the inputs
                const float inputs = (static_cast<float>(t.inputCount) + (t.button ? 1.0f : 0.0f)) * 31.0f * 0.95f + 16.0f;
                const float room = list.width - 36 - inputs;
                const float nameScale = std::min(1.3f, room / std::max(Canvas::textWidth(*body, t.name, 1.0f), 1.0f));
                c.text(*body, t.name, {list.x + 20, row + 18 + (1.3f - nameScale) * 8.0f}, nameScale, locked ? kDim : kInk);
                if (t.button)
                {
                    const char press[2] = {static_cast<char>(t.button), 0};
                    x -= Canvas::textWidth(glyphs, press, 0.95f);
                    c.text(glyphs, press, {x, row + 14}, 0.95f, locked ? 0x60FFFFFFu : 0xFFFFFFFFu);
                    x -= 8;
                }
                for (int k = t.inputCount - 1; k >= 0; --k)
                {
                    const uint8_t d = t.inputs[k];
                    const char arrow[2] = {d < 9 ? kDirections[d] : '?', 0};
                    x -= Canvas::textWidth(glyphs, arrow, 0.95f);
                    c.text(glyphs, arrow, {x, row + 14}, 0.95f, locked ? 0x60FFFFFFu : 0xFFFFFFFFu);
                }
            }
            row += kTrickRowHeight;
        }
        listHeight = static_cast<float>(count) * kTrickRowHeight;
        listScroll = std::min(listScroll, std::max(0.0f, listHeight - (list.y + list.height - rowTop)));
        c.clearClip();
    }

    std::vector<Panel::GoalLine> Panel::goalLines(const Snapshot &snap) const
    {
        const State &s = *state_;
        std::vector<GoalLine> out;
        if (!s.goalsMatch(snap))
            return out;
        for (size_t i = 0; i < s.level.goals.size(); ++i)
        {
            const GoalState state = s.stateOf(snap, static_cast<int>(i));
            GoalLine line{s.level.string(s.level.goals[i].title),
                          state == GoalState::Locked ? std::string() : s.progress(snap, static_cast<int>(i), state == GoalState::Done),
                          s.level.goalSet};
            int have = 0, target = 0;
            if (state != GoalState::Locked && state != GoalState::Done && s.goalHasBar(snap, static_cast<int>(i), have, target))
                line.barHave = have, line.barTarget = target;
            out.push_back(std::move(line));
        }
        return out;
    }

    void Panel::draw(const Snapshot &snap, double seconds, const Surface &surface)
    {
        State &s = *state_;
        s.loadUi();
        Canvas c(surface);
        c.fill({0, 0, static_cast<float>(surface.width), static_cast<float>(surface.height)}, rgba(0, 0, 0));

        const bool lineActive = snap.line[0] != 0 || snap.pendingScore > 0.0f;
        const GameMode was = s.lastPaint;
        s.policy.update(snap.mode, lineActive);
        s.lastPaint = snap.mode;
        if (s.policy.screen() == Screen::Blank)
        {
            s.loadLevel("");
            return;            // the front end and loading: the top screen has the player's eyes
        }
        s.loadLevel(snap.levelFile);
        s.useLoadedScript(snap);
        s.trackProgress(snap, seconds, lineActive);

        // A pause shows the whole level, the moment for planning; play takes
        // the follow view back unless the user moved the map.
        const Rect &pane = s.mapPane();
        const Rect &clear = s.mapClear();
        if (s.levelLoaded)
        {
            if (snap.mode == GameMode::Paused && was == GameMode::Play && s.view.follow)
            {
                s.followZoom = s.view.zoom;
                s.view = wholeView(s.level.transform, pane, clear);
                s.wholeForPause = true;
            }
            else if (snap.mode == GameMode::Play && was == GameMode::Paused && s.wholeForPause)
            {
                s.view.follow = true;
                s.view.zoom = s.followZoom;
                s.wholeForPause = false;
            }
        }
        if (!s.policy.isReference())
            s.moveToSelected = false;
        if (!s.policy.isReference() && s.selectedGoal >= 0 && s.selectedGoal != s.progressGoal)
            s.selectedGoal = -1;

        // ---- the map pane -------------------------------------------------
        s.hits.clear();
        s.hitPoints.clear();
        c.setClip(pane);
        if (s.levelLoaded)
        {
            const MapTransform &map = s.level.transform;
            const Point skater = map.toMap(snap.x, snap.z);
            const bool items = s.policy.screen() == Screen::Items;
            const bool focus = s.selectedGoal >= 0 && s.selectedGoal < static_cast<int>(s.level.goals.size()) && s.goalsMatch(snap);
            // The goal whose places the map shows: the selected one, else
            // the game's; with none, the next goal's giver.
            const int shown = focus ? s.selectedGoal : s.shownGoal(snap);
            const int ready = shown < 0 && !items ? s.nearestReadyGoal(snap) : -1;
            const int nearest = shown < 0 && ready < 0 && !items ? s.nearestOpenGoal(snap) : -1;
            bool givers = false, runningPlaces = false; // what the idle map drew, for its legend
            if (s.view.follow)
            {
                // the skater a third up from the bottom: most of the map is ahead
                s.view.center = clampCenter(followCenter(skater, s.view.zoom, pane), s.view.zoom, map, pane);
            }
            else if (s.moveToSelected && focus)
            {
                // the view moves to the selected goal's places
                float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
                s.forGoalPlaces(snap, s.selectedGoal, [&](const Place &p) {
                    const Point m = map.toMap(p.world.x, p.world.y);
                    x0 = std::min(x0, m.x), y0 = std::min(y0, m.y), x1 = std::max(x1, m.x), y1 = std::max(y1, m.y);
                });
                Point giver;
                if (x1 < x0 && s.giverOf(snap, s.selectedGoal, giver))
                {
                    const Point m = map.toMap(giver.x, giver.y);
                    x0 = x1 = m.x, y0 = y1 = m.y;
                }
                if (x1 >= x0)
                {
                    const float wanted = std::min(pane.width * 0.7f / std::max(x1 - x0, 1.0f), pane.height * 0.7f / std::max(y1 - y0, 1.0f));
                    s.view.zoom += (clampZoom(std::min(wanted, 1.6f), map, clear) - s.view.zoom) * 0.35f;
                    s.view.center.x += ((x0 + x1) * 0.5f - s.view.center.x) * 0.35f;
                    s.view.center.y += ((y0 + y1) * 0.5f - s.view.center.y) * 0.35f;
                }
            }
            c.map(s.level, s.view, pane);

            // The icons shrink as the view takes in more of the level.
            const float fit = fitZoom(map, clear);
            const float iconScale = std::clamp(0.5f + 0.6f * (s.view.zoom - fit) / std::max(1.0f - fit, 0.05f), 0.5f, 1.1f);
            const auto onScreen = [&](Point world, Point &at) {
                at = mapToScreen(s.view, pane, map.toMap(world.x, world.y));
                return at.x >= -40 && at.y >= -40 && at.x <= pane.width + 40 && at.y <= pane.height + 40;
            };

            if (items)
            {
                // The items screen: every running goal's places, small and in
                // the goal's colour, and what lies about (stat points, keys,
                // heads; juice as small dots).
                const int goals = s.goalsMatch(snap) ? static_cast<int>(s.level.goals.size()) : 0;
                for (int i = 0; i < goals; ++i)
                {
                    if (!s.goalShown(snap, i) || s.stateOf(snap, i) != GoalState::Open)
                        continue;
                    const bool selected = i == s.selectedGoal;
                    const uint32_t colour = lookOf(s.level.goals[static_cast<size_t>(i)].type).colour;
                    s.forGoalPlaces(snap, i, [&](const Place &p) {
                        Point at;
                        if (!onScreen(p.world, at))
                            return;
                        s.drawMarker(c, at, p, colour, (selected ? 14.0f : 9.0f) * iconScale, false, seconds);
                        s.hits.push_back({at, i});
                        s.hitPoints.push_back(at);
                    });
                }
                for (uint32_t i = 0; i < snap.collectableCount; ++i)
                {
                    const Collectable &item = snap.collectables[i];
                    Point at;
                    if (!item.shown || !onScreen({item.x, item.z}, at))
                        continue;
                    if (const Image *art = s.collectableArt(item.kind))
                    {
                        const float r = 19.0f * iconScale;
                        c.disc(at, r + 3, rgba(10, 13, 19, 170));
                        c.image(*art, {at.x - r, at.y - r, r * 2, r * 2});
                    }
                    else if (item.kind <= 2)
                    {
                        if (s.view.zoom > fit * 1.4f)
                            c.disc(at, 3.5f, rgba(255, 150, 60, 210));
                    }
                    else
                    {
                        c.disc(at, 7.0f * iconScale, rgba(250, 240, 200, 230));
                        c.ring(at, 8.0f * iconScale, 2.0f, rgba(10, 13, 19, 200));
                    }
                }
            }
            else
            {
                // No goal running: every running goal's places small in the
                // goal's colour (the gaps, the sneakers), and the givers of
                // the goals that can be started, the nearest ringed.
                const int goals = s.goalsMatch(snap) ? static_cast<int>(s.level.goals.size()) : 0;
                if (shown < 0)
                    for (int i = 0; i < goals; ++i)
                    {
                        if (i == nearest || !s.goalShown(snap, i) || s.stateOf(snap, i) != GoalState::Open)
                            continue;
                        const uint32_t colour = lookOf(s.level.goals[static_cast<size_t>(i)].type).colour;
                        s.forGoalPlaces(snap, i, [&](const Place &p) {
                            Point at;
                            if (p.phase == TargetPhase::Done || !onScreen(p.world, at))
                                return;
                            s.drawMarker(c, at, p, colour, 7.0f * iconScale, false, seconds);
                            s.hits.push_back({at, i});
                            s.hitPoints.push_back(at);
                            runningPlaces = true;
                        });
                    }
                if (shown < 0)
                    for (int i = 0; i < goals; ++i)
                    {
                        Point giver, at;
                        if (!s.goalShown(snap, i) || s.stateOf(snap, i) != GoalState::Ready || !s.giverOf(snap, i, giver) || !onScreen(giver, at))
                            continue;
                        const float r = 22.0f * iconScale;
                        c.disc(at, r + 3, rgba(10, 13, 19, 225));
                        c.disc(at, r, kReadyInk);
                        if (s.title)
                        {
                            const float scale = r / 8.5f;
                            c.text(*s.title, "?", {at.x - Canvas::textWidth(*s.title, "?", scale) * 0.5f, at.y - r * 0.8f}, scale, 0xFFFFFFFFu);
                        }
                        if (i == ready)
                            c.ring(at, r + 8.0f + 3.0f * std::sin(static_cast<float>(seconds) * 5.0f), 3.0f, rgba(255, 255, 255, 220));
                        s.hits.push_back({at, i});
                        s.hitPoints.push_back(at);
                        givers = true;
                    }
                // NPCs with a challenge to give (a script word says so): their bubble's question
                for (const BubbleRecord &b : s.level.bubbles)
                {
                    Point at;
                    if (b.stateWord < 0 || static_cast<uint32_t>(b.stateWord) >= snap.scriptWordCount || snap.scriptWords[b.stateWord] != b.stateValue)
                        continue;
                    if (!onScreen(s.objectWorld(snap, TargetRecord{b.nameHash, b.x, b.z, -1, 0, 0}), at))
                        continue;
                    const float r = 22.0f * iconScale;
                    c.disc(at, r + 3, rgba(10, 13, 19, 225));
                    c.disc(at, r, kReadyInk);
                    if (s.title)
                    {
                        const float scale = r / 8.5f;
                        c.text(*s.title, "?", {at.x - Canvas::textWidth(*s.title, "?", scale) * 0.5f, at.y - r * 0.8f}, scale, 0xFFFFFFFFu);
                    }
                }
                // The one goal's places: what to pick up, where to bring it, where to be.
                const int marked = shown >= 0 ? shown : nearest;
                if (marked >= 0 && marked < goals)
                {
                    Place next{};
                    const bool hasNext = s.nextPlace(snap, marked, next);
                    s.forGoalPlaces(snap, marked, [&](const Place &p) {
                        Point at;
                        if (!onScreen(p.world, at))
                            return;
                        const bool isNext = hasNext && p.world.x == next.world.x && p.world.y == next.world.y && p.phase == next.phase;
                        s.drawMarker(c, at, p, roleInk(p.role), (isNext ? kIconRadius * 1.2f : kIconRadius) * iconScale, isNext, seconds);
                        s.hits.push_back({at, marked});
                        s.hitPoints.push_back(at);
                    });
                    Point giver, at;
                    if (s.stateOf(snap, marked) == GoalState::Ready && s.giverOf(snap, marked, giver) && onScreen(giver, at))
                    {
                        const float r = 22.0f * iconScale;
                        c.disc(at, r + 3, rgba(10, 13, 19, 225));
                        c.disc(at, r, kReadyInk);
                        c.ring(at, r + 8.0f, 3.0f, rgba(255, 255, 255, 220));
                    }
                }
            }

            // the skater
            const Point at = mapToScreen(s.view, pane, skater);
            const Point heading = map.direction(snap.forwardX, snap.forwardZ);
            const float length = std::hypot(heading.x, heading.y);
            const float fx = length > 0.01f ? heading.x / length : 0.0f, fy = length > 0.01f ? heading.y / length : -1.0f;
            const Point tip{at.x + fx * 26, at.y + fy * 26};
            const Point left{at.x - fx * 15 + fy * 15, at.y - fy * 15 - fx * 15};
            const Point right{at.x - fx * 15 - fy * 15, at.y - fy * 15 + fx * 15};
            const Point tail{at.x - fx * 7, at.y - fy * 7};
            c.disc(at, 30, rgba(255, 255, 255, 40));
            c.triangle({tip.x + fx * 4, tip.y + fy * 4}, {left.x - fx * 3 + fy * 3, left.y - fy * 3 - fx * 3}, {right.x - fx * 3 - fy * 3, right.y - fy * 3 + fx * 3}, rgba(10, 13, 19));
            c.triangle(tip, left, tail, kAccent);
            c.triangle(tip, tail, right, rgba(255, 228, 150));

            // (the idle legend only explains what the map drew)
            if (!items && (shown >= 0 || nearest >= 0 || givers || runningPlaces))
                s.drawLegend(c, shown >= 0 ? shown : nearest, givers, runningPlaces);
        }
        else if (s.body)
        {
            c.text(*s.body, "No map for this level", {120, 500}, 2.0f, kDim);
        }

        // Follow is the default and needs no control: moving the map brings
        // this small button, which takes the map back.
        if (s.levelLoaded && !s.view.follow && !s.wholeForPause)
        {
            const Rect &button = s.returnButton();
            const Point middle{button.x + button.width * 0.5f, button.y + button.height * 0.5f};
            c.disc(middle, button.width * 0.5f, rgba(16, 20, 28, 235));
            c.ring(middle, 14, 3.5f, kAccent);
            c.disc(middle, 4.5f, kAccent);
            c.line({middle.x, middle.y - 24}, {middle.x, middle.y - 15}, 3.5f, kAccent);
            c.line({middle.x, middle.y + 15}, {middle.x, middle.y + 24}, 3.5f, kAccent);
            c.line({middle.x - 24, middle.y}, {middle.x - 15, middle.y}, 3.5f, kAccent);
            c.line({middle.x + 15, middle.y}, {middle.x + 24, middle.y}, 3.5f, kAccent);
        }
        if (s.title && s.policy.isReference())
        {
            const char *name = s.level.name.c_str();
            c.fill({0, 0, Canvas::textWidth(*s.title, name, 2.0f) + 32, 54}, rgba(16, 20, 28, 200));
            c.text(*s.title, name, {16, 10}, 2.0f, kInk);
        }
        c.clearClip();

        // ---- over the map: the second HUD, or the side pane ---------------
        if (s.policy.isReference())
        {
            c.fill(kSidePane, kPanelBack);
            s.drawReference(c, snap);
        }
        else
            s.drawLive(c, snap, seconds, lineActive);
    }
}
