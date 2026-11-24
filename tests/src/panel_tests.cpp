#include "MiniTest.h"
#include "collection/panel.h"
#include "collection/panel_canvas.h"
#include "collection/panel_file.h"
#include "collection/panel_layout.h"
#include "collection/panel_policy.h"
#include "collection/panel_view.h"
#include "aggressive/overrides/aggressive_inline_panel.h"
// (DM2's and BMX XXX's globals at their addresses, as their captures read them)
#include "sym/game_overrides/dave_mirra_panel.cpp.h"
#include "sym/game_overrides/bmx_xxx_panel.cpp.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace panel;

namespace dave_mirra::panel_state
{
    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out);
}
namespace bmx_xxx::panel_state
{
    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out);
}

namespace
{
    bool near(float a, float b, float tolerance = 1e-3f) { return std::fabs(a - b) <= tolerance; }

    MapTransform movieLike()
    {
        MapTransform map;
        map.worldLeft = -164.0f;
        map.worldTop = 220.0f;
        map.pixelsPerUnit = 3.0f;
        map.width = 1753;
        map.height = 1537;
        return map;
    }

    constexpr Rect kPane{0, 0, 740, 1080};

    void putWord(std::vector<uint8_t> &ram, uint32_t address, uint32_t value) { std::memcpy(ram.data() + address, &value, 4); }
    void putFloat(std::vector<uint8_t> &ram, uint32_t address, float value) { std::memcpy(ram.data() + address, &value, 4); }

    void appendChunk(std::vector<uint8_t> &file, const char *tag, const std::vector<uint8_t> &data)
    {
        file.insert(file.end(), tag, tag + 4);
        const uint32_t size = static_cast<uint32_t>(data.size());
        file.insert(file.end(), reinterpret_cast<const uint8_t *>(&size), reinterpret_cast<const uint8_t *>(&size) + 4);
        file.insert(file.end(), data.begin(), data.end());
        file.resize(file.size() + (-size & 3u));
    }

    // The engine's name hash (zge/text/strings.h nameHash).
    uint32_t engineNameHash(const char *name)
    {
        uint32_t hash = 0;
        for (const char *c = name; *c; ++c)
        {
            uint32_t letter = static_cast<uint8_t>(*c);
            if (letter >= 'a' && letter <= 'z')
                letter -= 'a' - 'A';
            hash = (hash << 4) + letter;
            const uint32_t high = hash & 0xF0000000u;
            if (high)
                hash ^= high >> 24;
            hash &= ~high;
        }
        return hash;
    }

    // A DM2 label widget showing `text`, at `object` (ZGE2: a scene object of kind 4, its ListedLabel at +0x3C).
    void putLabel(std::vector<uint8_t> &ram, uint32_t object, const char *text)
    {
        const uint32_t record = object + 0x40u, chars = object + 0x100u;
        putWord(ram, object, 0x54u);
        putWord(ram, object + 0x3Cu, record);
        putWord(ram, record + 0x64u, chars);
        std::memcpy(ram.data() + chars, text, std::strlen(text) + 1);
    }

    // A BMX XXX (ZGE4) widget whose "Text" variable holds `text`: the widget
    // node, its one child the variable (kind 2, named "Text"), the variable's
    // value a holder of the text's address.
    void putWidgetText(std::vector<uint8_t> &ram, uint32_t widget, const char *text)
    {
        const uint32_t variable = widget + 0x40u, holder = widget + 0x80u, chars = widget + 0x100u;
        putWord(ram, widget + 0x0Cu, 1u);
        putWord(ram, widget + 0x04u, variable);
        putWord(ram, variable + 0x0Cu, 2u);
        putWord(ram, variable + 0x10u, engineNameHash("Text"));
        putWord(ram, variable + 0x18u, holder);
        putWord(ram, holder, chars);
        std::memcpy(ram.data() + chars, text, std::strlen(text) + 1);
    }

    template <typename T> void append(std::vector<uint8_t> &out, const T &value)
    {
        out.insert(out.end(), reinterpret_cast<const uint8_t *>(&value), reinterpret_cast<const uint8_t *>(&value) + sizeof(T));
    }
}

void register_panel_tests()
{
    MiniTest::Case("Panel", [](TestCase &tc) {
        tc.Run("world to map is a top-down view with +z up", [](TestCase &t) {
            const MapTransform map = movieLike();
            const Point corner = map.toMap(-164.0f, 220.0f);
            t.IsTrue(near(corner.x, 0) && near(corner.y, 0), "the picture's corner");
            const Point east = map.toMap(-154.0f, 220.0f);
            t.IsTrue(near(east.x, 30.0f) && near(east.y, 0), "+x goes right");
            const Point south = map.toMap(-164.0f, 210.0f);
            t.IsTrue(near(south.x, 0) && near(south.y, 30.0f), "-z goes down the picture");
        });

        tc.Run("map and world round trip", [](TestCase &t) {
            const MapTransform map = movieLike();
            const Point m = map.toMap(302.177f, -153.865f);
            const Point w = map.toWorld(m);
            t.IsTrue(near(w.x, 302.177f, 1e-2f) && near(w.y, -153.865f, 1e-2f), "world back");
        });

        tc.Run("a turned map is a quarter turn, not a mirror", [](TestCase &t) {
            MapTransform map = movieLike();
            map.worldTop = -292.0f;
            map.turned = true;
            const Point east = map.toMap(-154.0f, -292.0f);
            t.IsTrue(near(east.x, 0) && near(east.y, 30.0f), "+x goes down the picture");
            const Point north = map.toMap(-164.0f, -282.0f);
            t.IsTrue(near(north.x, 30.0f) && near(north.y, 0), "+z goes right");
            // right of north stays right of north: east is clockwise of north on the picture too
            const Point n = map.direction(0, 1), e = map.direction(1, 0);
            t.IsTrue(n.x * e.y - n.y * e.x > 0, "not mirrored");
            const Point plainN = movieLike().direction(0, 1), plainE = movieLike().direction(1, 0);
            t.IsTrue(plainN.x * plainE.y - plainN.y * plainE.x > 0, "the plain map neither");
            const Point w = map.toWorld(map.toMap(12.0f, -40.0f));
            t.IsTrue(near(w.x, 12.0f, 1e-2f) && near(w.y, -40.0f, 1e-2f), "world back");
        });

        tc.Run("the whole view fills the free area", [](TestCase &t) {
            const MapTransform map = movieLike();
            const Rect clear{4, 66, 732, 926};
            const MapView view = wholeView(map, kPane, clear);
            const Point a = mapToScreen(view, kPane, {0, 0});
            const Point b = mapToScreen(view, kPane, {static_cast<float>(map.width), static_cast<float>(map.height)});
            t.IsTrue(a.x >= clear.x - 0.5f && a.y >= clear.y - 0.5f, "inside at the top left");
            t.IsTrue(b.x <= clear.x + clear.width + 0.5f && b.y <= clear.y + clear.height + 0.5f, "inside at the bottom right");
            t.IsTrue(b.x - a.x >= clear.width * 0.95f || b.y - a.y >= clear.height * 0.95f, "fills one side");
        });

        tc.Run("map and screen round trip with the pane offset", [](TestCase &t) {
            MapView view;
            view.center = {400.0f, 500.0f};
            view.zoom = 1.5f;
            const Rect pane{20, 10, 740, 1080};
            const Point s = mapToScreen(view, pane, {430.0f, 520.0f});
            t.IsTrue(near(s.x, 20 + 370 + 45.0f) && near(s.y, 10 + 540 + 30.0f), "screen");
            const Point m = screenToMap(view, pane, s);
            t.IsTrue(near(m.x, 430.0f) && near(m.y, 520.0f), "map back");
        });

        tc.Run("the fit zoom shows the whole picture", [](TestCase &t) {
            const MapTransform map = movieLike();
            const float zoom = fitZoom(map, kPane);
            t.IsTrue(static_cast<float>(map.width) * zoom <= kPane.width, "width fits");
            t.IsTrue(static_cast<float>(map.height) * zoom <= kPane.height, "height fits");
            t.IsTrue(near(clampZoom(0.001f, map, kPane), zoom), "no smaller than fit");
            t.IsTrue(near(clampZoom(100.0f, map, kPane), kMaxZoom), "no larger than the most");
        });

        tc.Run("a drag moves the map with the finger", [](TestCase &t) {
            const MapTransform map = movieLike();
            MapView view;
            view.center = {800.0f, 700.0f};
            view.zoom = 2.0f;
            const Point before = mapToScreen(view, kPane, {820.0f, 720.0f});
            panBy(view, 30.0f, -40.0f, map, kPane);
            const Point after = mapToScreen(view, kPane, {820.0f, 720.0f});
            t.IsTrue(near(after.x - before.x, 30.0f) && near(after.y - before.y, -40.0f), "the point under the finger follows it");
        });

        tc.Run("a drag stops at the picture's edge", [](TestCase &t) {
            const MapTransform map = movieLike();
            MapView view;
            view.center = {10.0f, 10.0f};
            view.zoom = 1.0f;
            panBy(view, 500.0f, 500.0f, map, kPane);
            t.IsTrue(near(view.center.x, 0) && near(view.center.y, 0), "clamped to the corner");
            panBy(view, -100000.0f, -100000.0f, map, kPane);
            t.IsTrue(near(view.center.x, static_cast<float>(map.width)) && near(view.center.y, static_cast<float>(map.height)), "the far corner");
        });

        tc.Run("a pinch zooms about the fingers' midpoint", [](TestCase &t) {
            const MapTransform map = movieLike();
            MapView view;
            view.center = {800.0f, 700.0f};
            view.zoom = 1.0f;
            const Point a0{300, 400}, b0{500, 400}, a1{200, 400}, b1{600, 400};
            const Point held = screenToMap(view, kPane, {400, 400});
            pinch(view, a0, b0, a1, b1, map, kPane);
            t.IsTrue(near(view.zoom, 2.0f), "zoomed by the spread");
            const Point stillAt = mapToScreen(view, kPane, held);
            t.IsTrue(near(stillAt.x, 400.0f, 0.01f) && near(stillAt.y, 400.0f, 0.01f), "the midpoint's map point stayed");
        });

        tc.Run("a pinch that moves pans too", [](TestCase &t) {
            const MapTransform map = movieLike();
            MapView view;
            view.center = {800.0f, 700.0f};
            view.zoom = 1.0f;
            const Point held = screenToMap(view, kPane, {400, 400});
            pinch(view, {300, 400}, {500, 400}, {330, 460}, {530, 460}, map, kPane);
            t.IsTrue(near(view.zoom, 1.0f), "same spread, same zoom");
            const Point stillAt = mapToScreen(view, kPane, held);
            t.IsTrue(near(stillAt.x, 430.0f, 0.01f) && near(stillAt.y, 460.0f, 0.01f), "moved with the midpoint");
        });

        tc.Run("a pinch stops at the zoom limits", [](TestCase &t) {
            const MapTransform map = movieLike();
            MapView view;
            view.center = {800.0f, 700.0f};
            view.zoom = 3.5f;
            pinch(view, {390, 400}, {410, 400}, {100, 400}, {700, 400}, map, kPane);
            t.IsTrue(near(view.zoom, kMaxZoom), "the most zoom");
        });

        tc.Run("a tap takes the nearest icon in reach", [](TestCase &t) {
            const Point icons[3] = {{100, 100}, {130, 100}, {400, 400}};
            t.Equals(nearestWithin(icons, 3, {112, 100}, 46.0f), 0, "the nearer of two");
            t.Equals(nearestWithin(icons, 3, {120, 100}, 46.0f), 1, "the other one");
            t.Equals(nearestWithin(icons, 3, {700, 700}, 46.0f), -1, "none in reach");
            t.Equals(nearestWithin(icons, 0, {100, 100}, 46.0f), -1, "no icons");
        });

        tc.Run("a tap finds its row in a scrolled list", [](TestCase &t) {
            const float rows[3] = {92, 150, 92};
            t.Equals(rowAt(100, 92, 988, 0, rows, 3), 0, "first row");
            t.Equals(rowAt(200, 92, 988, 0, rows, 3), 1, "second row, taller");
            t.Equals(rowAt(100, 92, 988, 92, rows, 3), 1, "scrolled by a row");
            t.Equals(rowAt(50, 92, 988, 0, rows, 3), -1, "above the list");
            t.Equals(rowAt(1000, 92, 988, 0, rows, 3), -1, "below the rows");
        });

        tc.Run("the default needs nothing pressed", [](TestCase &t) {
            ScreenPolicy p;
            t.IsTrue(p.screen() == Screen::Blank, "no level: nothing");
            p.update(GameMode::Play, false);
            t.IsTrue(p.screen() == Screen::Live, "in a level: the live screen");
            p.update(GameMode::Play, true);
            t.IsTrue(p.screen() == Screen::Live, "a line stays on it");
            p.update(GameMode::NoLevel, false);
            t.IsTrue(p.screen() == Screen::Blank, "out of the level: nothing");
        });

        tc.Run("a pause brings the reference, play takes it away", [](TestCase &t) {
            ScreenPolicy p;
            p.update(GameMode::Play, false);
            p.update(GameMode::Paused, false);
            t.IsTrue(p.screen() == Screen::Goals, "the goals first");
            p.open(Screen::Tricks);
            p.update(GameMode::Paused, false);
            t.IsTrue(p.screen() == Screen::Tricks, "a touch moves it");
            p.update(GameMode::Play, false);
            t.IsTrue(p.screen() == Screen::Live, "play: back to the live screen");
            p.update(GameMode::Paused, false);
            t.IsTrue(p.screen() == Screen::Tricks, "the next pause: the last reference");
        });

        tc.Run("a line takes a reference opened by touch away", [](TestCase &t) {
            ScreenPolicy p;
            p.update(GameMode::Play, false);
            p.open(Screen::Tricks);
            p.update(GameMode::Play, false);
            t.IsTrue(p.screen() == Screen::Tricks, "it stays while nothing happens");
            p.update(GameMode::Play, true);
            t.IsTrue(p.screen() == Screen::Live, "a line began");
        });

        tc.Run("a reference opened mid-line stays until the next line", [](TestCase &t) {
            ScreenPolicy p;
            p.update(GameMode::Play, true);
            p.open(Screen::Goals);
            p.update(GameMode::Play, true);
            t.IsTrue(p.screen() == Screen::Goals, "the line was there already");
            p.update(GameMode::Play, false);
            p.update(GameMode::Play, true);
            t.IsTrue(p.screen() == Screen::Live, "the next one takes it");
        });

        tc.Run("hold keeps a reference through a line and through play", [](TestCase &t) {
            ScreenPolicy p;
            p.update(GameMode::Play, false);
            p.open(Screen::Tricks);
            p.togglePin();
            p.update(GameMode::Play, true);
            t.IsTrue(p.screen() == Screen::Tricks, "through a line");
            p.update(GameMode::Paused, false);
            p.update(GameMode::Play, false);
            t.IsTrue(p.screen() == Screen::Tricks, "through a pause");
            p.close();
            t.IsTrue(p.screen() == Screen::Live && !p.pinned(), "closing lets go");
        });

        tc.Run("a closed reference stays closed in the pause", [](TestCase &t) {
            ScreenPolicy p;
            p.update(GameMode::Play, false);
            p.update(GameMode::Paused, false);
            p.close();
            p.update(GameMode::Paused, false);
            t.IsTrue(p.screen() == Screen::Live, "the user's choice stands");
        });

        tc.Run("a level file reads back", [](TestCase &t) {
            std::vector<uint8_t> file = {'P', 'M', 'A', 'P'};
            append(file, kFileVersion);
            appendChunk(file, "NAME", {'M', 'o', 'v', 'i', 'e', 0});
            std::vector<uint8_t> image;
            append(image, uint32_t{32});
            append(image, uint32_t{16});
            append(image, -10.0f);
            append(image, 20.0f);
            append(image, 2.0f);
            append(image, uint32_t{1});
            for (int y = 0; y < 16; ++y)
            {
                append(image, uint16_t{32});
                append(image, uint16_t{0xF800});
            }
            appendChunk(file, "MAPI", image);
            std::vector<uint8_t> goals;
            append(goals, uint32_t{1});
            GoalRecord goal{};
            goal.title = 1;
            goal.counterWord = 25;
            goal.counterTarget = 5;
            goal.status = 4;
            goal.targetWord = 26;
            goal.activateText = 17;
            goal.firstTarget = 0;
            goal.targetCount = 1;
            append(goals, goal);
            appendChunk(file, "GOAL", goals);
            std::vector<uint8_t> targets;
            append(targets, uint32_t{1});
            TargetRecord target{0x1234u, 10.0f, -20.0f, -1, 0, static_cast<uint32_t>(TargetRole::DropOff)};
            append(targets, target);
            appendChunk(file, "TARG", targets);
            std::vector<uint8_t> text = {0, 'H', 'i', 0, 'G', 'o', 't', ' ', '%', 'd', ' ', 'o', 'f', ' ', '%', 'd', 0};
            text.insert(text.end(), {'T', 'a', 'l', 'k', 0});
            appendChunk(file, "TEXT", text);

            LevelFile level;
            t.IsTrue(parseLevelFile(file.data(), file.size(), level), "parsed");
            t.Equals(level.name, std::string("Movie"), "the name");
            t.Equals(level.transform.width, 32, "width");
            t.IsTrue(level.transform.turned, "turned");
            t.IsTrue(near(level.transform.worldLeft, -10.0f), "the transform");
            t.Equals(level.mips.size(), size_t{2}, "the picture and one mip (the next would be under 8 high)");
            t.Equals(level.mips[0].pixels[100], uint16_t{0xF800}, "the picture");
            t.Equals(level.mips[1].pixels[3], uint16_t{0xF800}, "the mip keeps a flat colour");
            t.Equals(level.goals.size(), size_t{1}, "goals");
            t.Equals(std::string(level.string(level.goals[0].title)), std::string("Hi"), "the title");
            t.Equals(std::string(level.string(level.goals[0].status)), std::string("Got %d of %d"), "the game's status format");
            t.Equals(level.goals[0].targetWord, 26, "the target's word");
            t.Equals(std::string(level.string(level.goals[0].activateText)), std::string("Talk"), "what starts it");
            t.Equals(level.targets.size(), size_t{1}, "targets");
            t.Equals(level.targets[0].role, static_cast<uint32_t>(TargetRole::DropOff), "a target's role");
            t.IsTrue(near(level.targets[0].z, -20.0f), "its place");
        });

        tc.Run("a level file's goal set is the loaded script's", [](TestCase &t) {
            // Aggressive Inline's Civic: the PS2 disc's script counts the
            // wires to the big ball in word 157, the Xbox release's (the
            // career scripts setting) in word 161, where the PS2's word is a
            // variable's type (4). A set for each, keyed by what the game has
            // loaded: the variables' bytes and the objectives' text offsets.
            std::vector<uint8_t> file = {'P', 'M', 'A', 'P'};
            append(file, kFileVersion);
            std::vector<uint8_t> image;
            for (uint32_t w : {8u, 8u})
                append(image, w);
            for (float f : {0.0f, 0.0f, 1.0f})
                append(image, f);
            append(image, uint32_t{0});
            for (int y = 0; y < 8; ++y)
            {
                append(image, uint16_t{8});
                append(image, uint16_t{0x07E0});
            }
            appendChunk(file, "MAPI", image);
            const auto addSet = [&file](uint32_t variableBytes, uint32_t name, int32_t word, uint8_t scoreKind) {
                std::vector<uint8_t> key, goals;
                append(key, variableBytes);
                append(key, uint32_t{1});
                append(key, name);
                append(key, name + 0x20u);
                appendChunk(file, "SCRP", key);
                append(goals, uint32_t{1});
                GoalRecord goal{};
                goal.title = 1;
                goal.counterWord = word;
                goal.counterTarget = 3;
                goal.scoreKind = scoreKind;
                goal.targetWord = -1;
                append(goals, goal);
                appendChunk(file, "GOAL", goals);
                std::vector<uint8_t> targets;
                append(targets, uint32_t{1});
                append(targets, TargetRecord{0x80BA281u, 1.0f, 2.0f, word + 5, 0x49425u, 0u});
                appendChunk(file, "TARG", targets);
                appendChunk(file, "TEXT", {0, 'W', 'i', 'r', 'e', 's', 0});
            };
            addSet(530u * 4u, 0x100u, 157, 4);
            addSet(540u * 4u, 0x108u, 161, 1);

            LevelFile level;
            t.IsTrue(parseLevelFile(file.data(), file.size(), level), "parsed");
            t.Equals(level.goalSets.size(), size_t{2}, "two scripts' sets");
            t.Equals(level.goalSet, 0, "the first in use at load");
            t.Equals(level.goals[0].counterWord, 157, "the PS2 script's word");

            uint32_t texts[2] = {0x108u, 0x128u};
            t.Equals(matchingGoalSet(level, 540u * 4u, texts, 1), 1, "the Xbox script loaded");
            useGoalSet(level, 1);
            t.Equals(level.goals[0].counterWord, 161, "its word");
            t.Equals(level.targets[0].doneWord, 166, "its target's word");
            t.Equals(static_cast<int>(level.goals[0].scoreKind), 1, "its score kind");
            t.Equals(std::string(level.string(level.goals[0].title)), std::string("Wires"), "its title");
            texts[0] = 0x100u;
            texts[1] = 0x120u;
            t.Equals(matchingGoalSet(level, 530u * 4u, texts, 1), 0, "the PS2 script loaded");
            t.Equals(matchingGoalSet(level, 540u * 4u, texts, 1), -1, "the same texts with other variables: neither");
            t.Equals(matchingGoalSet(level, 530u * 4u, texts, 2), -1, "another objective count: neither");

            useGoalSet(level, -1);
            t.Equals(level.goals.size(), size_t{1}, "an unknown script keeps the titles");
            t.Equals(level.goals[0].counterWord, -1, "but reads no count");
            t.Equals(level.targets[0].doneWord, -1, "and no place's word");
            t.Equals(static_cast<int>(level.goals[0].scoreKind), 0, "and no score");

            // A file without SCRP (DM2, BMX XXX): one set, whatever script runs.
            std::vector<uint8_t> plain = {'P', 'M', 'A', 'P'};
            append(plain, kFileVersion);
            appendChunk(plain, "MAPI", image);
            std::vector<uint8_t> goals;
            append(goals, uint32_t{1});
            append(goals, GoalRecord{});
            appendChunk(plain, "GOAL", goals);
            LevelFile unkeyed;
            t.IsTrue(parseLevelFile(plain.data(), plain.size(), unkeyed), "parsed");
            t.Equals(matchingGoalSet(unkeyed, 0u, texts, 0), 0, "an unkeyed set is always the one");
        });

        tc.Run("a status format takes the count and the target", [](TestCase &t) {
            t.Equals(statusLine("Hookers delivered: %d of %d", 1, 5), std::string("Hookers delivered: 1 of 5"), "two numbers");
            t.Equals(statusLine("Gaps found: %d of %d", 20, 20), std::string("Gaps found: 20 of 20"), "all of them");
            t.Equals(statusLine("no numbers", 1, 2), std::string("no numbers"), "a plain text");
        });

        tc.Run("a goal's state words tell the four states apart", [](TestCase &t) {
            t.IsTrue(kStateInit != kStateReady && kStateReady != kStateOpen && kStateOpen != kStateComplete && kStateInit != kStateComplete, "distinct");
            t.IsTrue(kStateInit == 0x0004E2E4u && kStateComplete == 0x04250E05u, "the script's own names for init and complete");
        });

        tc.Run("a cut level file is refused", [](TestCase &t) {
            std::vector<uint8_t> file = {'P', 'M', 'A', 'P'};
            append(file, kFileVersion);
            std::vector<uint8_t> image;
            append(image, uint32_t{16});
            append(image, uint32_t{8});
            append(image, 0.0f);
            append(image, 0.0f);
            append(image, 2.0f);
            append(image, uint32_t{0});
            append(image, uint16_t{4});     // far too few runs
            append(image, uint16_t{1});
            appendChunk(file, "MAPI", image);
            LevelFile level;
            t.IsTrue(!parseLevelFile(file.data(), file.size(), level), "short picture");
            t.IsTrue(!parseLevelFile(file.data(), 6, level), "short header");
        });

        tc.Run("capture reads the game's fields and stays out of bad pointers", [](TestCase &t) {
            std::vector<uint8_t> ram(32u << 20);
            auto snapshot = std::make_unique<Snapshot>();
            snapshot->activeGoal = 3;
            snapshot->goalTargetCount = 7;
            aggressive_inline::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(snapshot->mode == GameMode::NoLevel, "an empty RAM is no level");
            t.IsTrue(snapshot->activeGoal == -1 && snapshot->goalTargetCount == 0, "no active goal, no targets");

            const uint32_t top = 0x00400000u, level = 0x00410000u, player = level + 0x10u;
            putWord(ram, 0x00276FE4u, 2u);    // play
            putWord(ram, 0x0024C9BCu, top);
            putWord(ram, top + 0x10u, level);
            ram[0x0024C9B4u] = 5;
            putFloat(ram, player + 0x90u, 302.0f);
            putFloat(ram, player + 0x94u, -10.0f);
            putFloat(ram, player + 0x98u, -153.0f);
            putWord(ram, 0x002795A0u, 0xFFFFFFF0u); // a wild objectives pointer
            putWord(ram, 0x002795A4u, 25u);
            putWord(ram, 0x002795C8u, 5u);
            aggressive_inline::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(snapshot->mode == GameMode::Play, "in a level");
            t.Equals(snapshot->level, 5, "the level");
            t.Equals(std::string(snapshot->levelFile), std::string(""), "no folder in this RAM");
            aggressive_inline::panel_state::capture(ram.data(), ram.size(), *snapshot, "AIRPORT");
            t.Equals(std::string(snapshot->levelFile), std::string("AIRPORT"), "the level the host serves");
            t.IsTrue(near(snapshot->x, 302.0f) && near(snapshot->z, -153.0f), "the position");
            t.Equals(snapshot->objectiveCount, 0u, "the wild pointer was not followed");

            const uint32_t records = 0x00420000u;
            putWord(ram, 0x002795A0u, records);
            putWord(ram, records + 0x2Cu * 2u + 0x0Cu, 0x04250E05u);
            putWord(ram, records + 0x2Cu * 2u + 0x18u, 0x1234u);   // its name and description in the script's strings
            putWord(ram, records + 0x2Cu * 2u + 0x1Cu, 0x1240u);
            const uint32_t variables = 0x00450000u;
            putWord(ram, 0x002798BCu, variables);
            putWord(ram, 0x002798D4u, 540u * 4u);  // the Xbox Civic script's variables
            putWord(ram, variables + 161u * 4u, 2u);
            const uint32_t prop = 0x00430000u;
            putWord(ram, 0x00279674u, prop);
            putWord(ram, prop + 0x58u, 17u);
            putWord(ram, prop + 0x64u, 1u);
            putFloat(ram, prop + 0x40u, 57.8f);
            putFloat(ram, prop + 0x48u, 147.7f);
            putWord(ram, prop + 0x7Cu, prop);       // a loop in the list must not hang the capture
            aggressive_inline::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(snapshot->objectiveCount, 25u, "the goals");
            t.Equals(snapshot->objectiveState[2], 0x04250E05u, "a goal's state");
            t.Equals(snapshot->objectiveText[2][0], 0x1234u, "a goal's name, which tells the scripts apart");
            t.Equals(snapshot->objectiveText[2][1], 0x1240u, "its description");
            t.Equals(snapshot->scriptVariableBytes, 540u * 4u, "the loaded script's variables");
            t.Equals(snapshot->scriptWords[161], 2u, "a script word");
            t.IsTrue(snapshot->collectableCount >= 1, "a prop");
            t.Equals(snapshot->collectables[0].kind, uint8_t{17}, "its kind");
            t.IsTrue(near(snapshot->collectables[0].x, 57.8f), "its place");

            const uint32_t hud = 0x00440000u, widget = 0x00441000u, node = 0x00442000u;
            const uint32_t value = 0x00443000u, text = 0x00444000u;
            putWord(ram, player + 0x8B8u, hud);
            putWord(ram, hud + 0x14u, widget);
            putWord(ram, widget + 0x04u, node);
            putWord(ram, node + 0x0Cu, 2u);
            putWord(ram, node + 0x10u, 0x00058AD4u); // "Text"
            putWord(ram, node + 0x18u, value);
            putWord(ram, value, text);
            std::memcpy(ram.data() + text, " ", 2);
            aggressive_inline::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(std::string(snapshot->line), std::string(""), "the space before the first trick is no line");
            std::memcpy(ram.data() + text, "#RGB{176 144 24}Tweaked ", 25);
            aggressive_inline::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(std::string(snapshot->line), std::string("Tweaked"), "a line's text, without its colours");

            putFloat(ram, player + 0x94u, 20000.0f);
            aggressive_inline::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(snapshot->mode == GameMode::NoLevel, "not placed yet");
        });

        tc.Run("DM2's capture reads its clock, trick line, shown goal and the NPCs' goals", [](TestCase &t) {
            std::vector<uint8_t> ram(32u << 20);
            auto snapshot = std::make_unique<Snapshot>();
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(snapshot->mode == GameMode::NoLevel, "an empty RAM is no level");

            // Woodward Camp (menu value 0, game level 2), a ProQuest game: the rider, character 0.
            const uint32_t rider = 0x00400000u, npcRider = 0x00410000u, definition = 0x00420000u;
            ram[sym::dm2::g_currentGameState] = 1;
            putWord(ram, sym::dm2::g_physicsTaskFrames, 1u);
            putWord(ram, sym::dm2::g_riders, rider);
            putWord(ram, sym::dm2::g_currentLevel, 0u);
            ram[sym::dm2::g_sessionLevel] = 2;
            ram[sym::dm2::g_sessionSinglePlayerMode] = 0;
            ram[sym::dm2::g_playerSetups + 3] = 0;
            putFloat(ram, rider + 0x1040u, 12.0f);
            putFloat(ram, rider + 0x1048u, -30.0f);
            // The character's Woodward goals: the rookie goal done, the amateur challenge next, three of its goals given.
            const uint32_t goals = sym::dm2::g_riderCharacters + 0x13Cu + 2u * 0x44u;
            ram[goals] = 0x40 | 0x80;
            ram[goals + 0x3D] = 1;
            for (uint32_t g = 0; g < 3; ++g)
                ram[goals + 0x0C + g * 3] = 0x80;
            // Its NPC: the amateur challenge's last goal; a wild one after it ends nothing.
            ram[sym::dm2::g_npcs + 0x00] = 1;
            putWord(ram, sym::dm2::g_npcs + 0x28u, definition);
            ram[sym::dm2::g_npcs + 0x30] = 0xFF;
            ram[definition + 0x10] = 1;
            ram[definition + 0x11] = 3;
            ram[definition + 0x12] = 3;
            putWord(ram, sym::dm2::g_riders + 4u, npcRider);
            putFloat(ram, npcRider + 0x1040u, 80.0f);
            putFloat(ram, npcRider + 0x1048u, 45.0f);
            ram[sym::dm2::g_objectiveState] = 0xFF;
            putWord(ram, sym::dm2::g_trickDisplayWidgets + 0x00u, 0xFFFFFFF0u); // wild labels are not followed
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(snapshot->mode == GameMode::Play, "in a level");
            t.Equals(snapshot->objectiveState[0], kStateComplete, "the rookie goal done");
            t.Equals(snapshot->objectiveState[4], kStateOpen, "a given goal runs");
            t.Equals(snapshot->objectiveState[7], kStateReady, "the NPC's goal can be started");
            t.Equals(snapshot->objectiveState[8], kStateInit, "the next challenge's are not offered");
            t.Equals(snapshot->trackedCount, 1u, "the giver tracked");
            t.Equals(snapshot->tracked[0].nameHash, engineNameHash("KSTRSP_GOAL_LEV02_EASY_3"), "by the goal's text key");
            t.IsTrue(near(snapshot->tracked[0].x, 80.0f) && near(snapshot->tracked[0].z, 45.0f), "where the NPC's rider is");
            t.Equals(snapshot->activeGoal, -1, "no goal labelled");
            t.IsTrue(snapshot->timerSeconds < 0.0f, "no clock before the turn runs");
            t.Equals(std::string(snapshot->line), std::string(""), "no line");

            // The NPC gave it: it runs, and no giver is tracked.
            ram[goals + 0x0C + 3 * 3] = 0x80 | 0x20;
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(snapshot->objectiveState[7], kStateOpen, "given, it runs");
            t.Equals(snapshot->trackedCount, 0u, "no giver once given");

            // The objective label shows the amateur challenge's second goal.
            ram[sym::dm2::g_objectiveState] = 5;
            ram[sym::dm2::g_objectiveState + 2] = 1;
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(snapshot->activeGoal, 5, "the labelled goal");
            ram[sym::dm2::g_objectiveState + 2] = 0;
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(snapshot->activeGoal, -1, "its label faded out");

            // The turn's clock, in the seconds the timer writes.
            ram[sym::dm2::g_roundPhase] = 5;
            putWord(ram, sym::dm2::g_phaseFramesLeft, 3542u);
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(near(snapshot->timerSeconds, 60.0f), "1:00");
            putWord(ram, sym::dm2::g_phaseFramesLeft, 3541u);
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(near(snapshot->timerSeconds, 59.0f), "0:59");
            putWord(ram, sym::dm2::g_phaseFramesLeft, 61u);
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(near(snapshot->timerSeconds, 1.0f), "a started second counts");
            putWord(ram, sym::dm2::g_phaseFramesLeft, 1u);
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(near(snapshot->timerSeconds, 0.0f), "the last frame reads 0:00, as the timer writes it");
            ram[sym::dm2::g_singlePlayerMode] = 3;
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(snapshot->timerSeconds < 0.0f, "a free ride has no clock");
            ram[sym::dm2::g_singlePlayerMode] = 0;

            // The run's two lines, in the labels they took (the current one's is label 2).
            const uint32_t labels = 0x00430000u;
            for (uint32_t i = 0; i < 4; ++i)
                putWord(ram, sym::dm2::g_trickDisplayWidgets + i * 4u, labels + i * 0x200u);
            putWord(ram, sym::dm2::g_trickDisplayWidgets + 0x14u, 0x00438000u);
            putWord(ram, sym::dm2::g_trickNameLabel, 2u);
            putLabel(ram, labels + 0x000u, "stale");
            putLabel(ram, labels + 0x200u, "#RGB{0 255 0}perfect #RGB{238 181 44}360\ntailwhip");
            putLabel(ram, labels + 0x400u, "#RGB{192 192 192}TO#RGB{238 181 44}\r\nx-up barspin");
            putLabel(ram, labels + 0x800u, "750 points");
            putWord(ram, sym::dm2::g_trickDisplayWidgets + 0x10u, labels + 0x800u);
            ram[rider + 0x1A0u + 0x69Du] = 2;
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(std::string(snapshot->line), std::string("perfect 360 tailwhip TO x-up barspin"), "the run's lines, oldest first");
            t.Equals(std::string(snapshot->currentTrick), std::string("TO x-up barspin"), "the current line");
            t.IsTrue(near(snapshot->pendingScore, 750.0f), "the points under them");
            putLabel(ram, labels + 0x800u, "");
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(near(snapshot->pendingScore, 0.0f), "no points where the game shows none");
            ram[rider + 0x1A0u + 0x69Du] = 0;
            dave_mirra::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(std::string(snapshot->line), std::string(""), "the run over: no line");
        });

        tc.Run("BMX XXX's capture reads its trick line, its points and the givers where they are", [](TestCase &t) {
            std::vector<uint8_t> ram(32u << 20);
            auto snapshot = std::make_unique<Snapshot>();
            bmx_xxx::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(snapshot->mode == GameMode::NoLevel, "an empty RAM is no level");

            // That Dam Level (row 1), riding, a challenge session with no objectives listed.
            const uint32_t rider = 0x00400000u, folder = 0x00410000u, pool = 0x00411000u;
            ram[sym::bmx::g_currentGameState] = 1;
            ram[sym::bmx::g_gameStatePhase] = 0;
            putWord(ram, sym::bmx::g_physicsTaskFrames, 1u);
            putWord(ram, sym::bmx::g_riders, rider);
            ram[sym::bmx::g_currentLevel] = 1;
            putWord(ram, sym::bmx::g_levels + 0x10u + 4u, folder);
            std::memcpy(ram.data() + folder, "CHL_DAM", 8);
            putWord(ram, sym::bmx::g_objectives, pool);
            bmx_xxx::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(snapshot->mode == GameMode::Play, "in a level");
            t.Equals(std::string(snapshot->levelFile), std::string("CHL_DAM"), "by its folder");
            t.Equals(std::string(snapshot->line), std::string(""), "no line");
            t.Equals(snapshot->trackedCount, 0u, "nobody tracked");

            // The line's two lines, in the labels they took (the newest is label 0: the one before it label 3).
            const uint32_t labels = 0x00420000u;
            const uint32_t display = sym::bmx::g_trickDisplay;
            for (uint32_t i = 0; i < 4; ++i)
                putWord(ram, display + 0x04u + i * 4u, labels + i * 0x200u);
            putWord(ram, display + 0x14u, labels + 0x800u);
            ram[display] = 0;
            putWidgetText(ram, labels + 0x600u, "#RGB{0 255 0}PERFECT #RGB{255 153 0}TABLETOP\nTO MANUAL");
            putWidgetText(ram, labels + 0x000u, "#RGB{192 192 192}TO#RGB{255 153 0} 360\nTAILWHIP");
            putWidgetText(ram, labels + 0x200u, "STALE");
            putWidgetText(ram, labels + 0x800u, "1250 points");
            ram[rider + 0x1C0u + 0x1A21u] = 2;
            bmx_xxx::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(std::string(snapshot->line), std::string("PERFECT TABLETOP TO MANUAL TO 360 TAILWHIP"), "the line's lines, oldest first");
            t.Equals(std::string(snapshot->currentTrick), std::string("TO 360 TAILWHIP"), "the newest line");
            t.IsTrue(near(snapshot->pendingScore, 1250.0f), "the points under them");
            t.Equals(snapshot->multiplier, 0, "no multiplier");
            putWidgetText(ram, labels + 0x800u, "");
            bmx_xxx::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.IsTrue(near(snapshot->pendingScore, 0.0f), "no points where the game shows none");
            ram[rider + 0x1C0u + 0x1A21u] = 0;
            bmx_xxx::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(std::string(snapshot->line), std::string(""), "the line over: no line");

            // The givers: a path man with Antenna on its path, two who give Deliver (no one place), one who
            // stands (no follower), and the NPC rider who challenges the player to Fuel.
            const uint32_t men = 0x00430000u, objective = 0x00440000u, follower = 0x00450000u, record = 0x00460000u, npcRider = 0x00470000u;
            const uint32_t menPool = 0x00431000u;
            putWord(ram, sym::bmx::g_pathManRecords, menPool);
            putWord(ram, menPool, men);
            const char *names[4] = {"Antenna", "Deliver", "Deliver", "Rocket"};
            for (uint32_t i = 0; i < 4; ++i)
            {
                const uint32_t link = men + i * 0x100u, man = link + 0x10u;
                putWord(ram, link, i < 3 ? link + 0x100u : 0u);
                putWord(ram, man + 0x04u, i < 3 ? follower + i * 0x80u : 0u);
                putWord(ram, man + 0xC8u, objective);
                std::memcpy(ram.data() + man + 0x88u, names[i], std::strlen(names[i]) + 1);
                putFloat(ram, follower + i * 0x80u + 0x20u, 10.0f + static_cast<float>(i));
                putFloat(ram, follower + i * 0x80u + 0x28u, -20.0f);
            }
            ram[sym::bmx::g_npcs] = 1;
            putWord(ram, sym::bmx::g_npcs + 0x18u, record);
            std::memcpy(ram.data() + record + 0x0Cu, "Fuel", 5);
            ram[sym::bmx::g_npcs + 0x24u] = 0xFF;
            putWord(ram, sym::bmx::g_riders + 4u, npcRider);
            putFloat(ram, npcRider + 0x2650u, 80.0f);
            putFloat(ram, npcRider + 0x2658u, 45.0f);
            bmx_xxx::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(snapshot->trackedCount, 2u, "Antenna's and Fuel's givers");
            const bool fuelFirst = engineNameHash("Fuel") < engineNameHash("Antenna");
            const TrackedObject &antenna = snapshot->tracked[fuelFirst ? 1 : 0], &fuel = snapshot->tracked[fuelFirst ? 0 : 1];
            t.Equals(antenna.nameHash, engineNameHash("Antenna"), "by its objective's name");
            t.IsTrue(near(antenna.x, 10.0f) && near(antenna.z, -20.0f), "where its follower puts it");
            t.Equals(fuel.nameHash, engineNameHash("Fuel"), "the NPC by its challenge");
            t.IsTrue(near(fuel.x, 80.0f) && near(fuel.z, 45.0f), "where its rider is");

            // What lies about: the game's pickup kinds' names, a bike part lying there and one taken, and an
            // objective's item (no kind of the game's own).
            const uint32_t kinds = 0x00480000u, type = 0x00481000u;
            const char *kindNames[5] = {"HEALTH", "WEAPON", "SCORE", "BIKE", "SNIPER"};
            for (uint32_t k = 0; k < 5; ++k)
            {
                putWord(ram, sym::bmx::g_pickupKindNames + k * 8u, kinds + k * 0x10u);
                putWord(ram, sym::bmx::g_pickupKindNames + k * 8u + 4u, static_cast<uint32_t>(std::strlen(kindNames[k])));
                std::memcpy(ram.data() + kinds + k * 0x10u, kindNames[k], std::strlen(kindNames[k]) + 1);
            }
            putWord(ram, type, engineNameHash("Pickup"));
            t.IsTrue(sym::bmx::levelObjectsBegin + 3u * 0x14u <= ram.size(), "the level objects' records in RAM");
            const char *pickupNames[3] = {"Pickup_bike_frame01", "Pickup_bike_seat01", "Pickup_lunchbox01"};
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint32_t record = sym::bmx::levelObjectsBegin + i * 0x14u, object = 0x00490000u + i * 0x1000u;
                const uint32_t node = object + 0x100u, scene = object + 0x400u, table = object + 0x500u, tags = object + 0x600u;
                putWord(ram, record + 0x08u, object);
                putWord(ram, record + 0x0Cu, node);
                putWord(ram, record + 0x10u, type);
                putWord(ram, object + 0x38u, scene);
                ram[scene] = 1;
                std::memcpy(ram.data() + node + 0x128u, pickupNames[i], std::strlen(pickupNames[i]) + 1);
                putWord(ram, node + 0x150u, table);
                putWord(ram, table + 0x0Cu, tags);
                ram[tags] = i == 1 ? 3 : 1; // the seat taken
                putFloat(ram, object + 0x10u, 30.0f + static_cast<float>(i));
                putFloat(ram, object + 0x18u, -5.0f);
            }
            bmx_xxx::panel_state::capture(ram.data(), ram.size(), *snapshot);
            t.Equals(snapshot->collectableCount, 2u, "the two bike parts, not the objective's item");
            t.Equals(static_cast<uint32_t>(snapshot->collectables[0].kind), static_cast<uint32_t>(kBmxPickupKinds + 3), "a bike part");
            t.IsTrue(snapshot->collectables[0].shown && near(snapshot->collectables[0].x, 30.0f) && near(snapshot->collectables[0].z, -5.0f),
                     "the frame lies there");
            t.IsFalse(snapshot->collectables[1].shown, "the seat is taken");
        });

        tc.Run("a UI file's tricks by move and its families' names", [](TestCase &t) {
            std::vector<uint8_t> file = {'P', 'M', 'A', 'P'};
            append(file, kFileVersion);
            std::vector<uint8_t> tricks, moves, families;
            append(tricks, uint32_t{2});
            for (uint8_t family : {uint8_t{0}, uint8_t{5}})
            {
                TrickRecord r{};
                r.family = static_cast<TrickFamily>(family);
                r.inputCount = 1;
                r.inputs[0] = 7;
                r.button = '2';
                std::memcpy(r.name, "TURNDOWN", 9);
                append(tricks, r);
            }
            append(moves, uint32_t{2});
            append(moves, TrickMove{0x00, 0, 0});
            append(moves, TrickMove{0x09, 1, 0});
            append(families, uint32_t{6});
            for (const char *name : {"AIR", "MODIFIERS", "STALLS", "GRINDS", "OTHERS", "SIGNATURE"})
            {
                FamilyName f{};
                std::strncpy(f.name, name, sizeof(f.name) - 1);
                append(families, f);
            }
            appendChunk(file, "TRIK", tricks);
            appendChunk(file, "MOVE", moves);
            appendChunk(file, "FAMS", families);
            UiFile ui;
            t.IsTrue(parseUiFile(file.data(), file.size(), ui), "parsed");
            t.Equals(ui.tricks.size(), size_t{2}, "the tricks");
            t.Equals(ui.moves.size(), size_t{2}, "a move a trick");
            t.Equals(static_cast<int>(ui.moves[1].move), 9, "the second's move");
            t.Equals(static_cast<int>(ui.moves[1].whileAble), 1, "listed while the character can do it");
            t.Equals(ui.familyNames.size(), size_t{6}, "six families");
            t.Equals(std::string(ui.familyNames[5].name), std::string("SIGNATURE"), "the last family's name");
        });

        // ---- Clean Top Screen (panel_snapshot.h) ----

        tc.Run("a bottom screen that draws nothing shows no piece", [](TestCase &t) {
            Panel panel;
            std::vector<uint32_t> pixels(static_cast<size_t>(kScreenWidth) * kScreenHeight, 0u);
            const Surface surface{pixels.data(), kScreenWidth, kScreenHeight, kScreenWidth};
            static Snapshot snapshot; // (large: not on the stack)
            snapshot = Snapshot{};
            panel.draw(snapshot, 1.0, surface);
            t.Equals(panel.shows(), HudPieces{0}, "no level: a blank screen shows none");
            snapshot.mode = GameMode::Play;
            snapshot.level = 0;
            std::snprintf(snapshot.levelFile, sizeof(snapshot.levelFile), "%s", "NOFILE");
            panel.draw(snapshot, 2.0, surface);
            t.Equals(panel.shows(), HudPieces{0}, "in play with no UI file (no fonts): the live screen draws nothing, shows none");
            // (with its files: the clock and the line, the goal with the level file's goals; the end-to-end runs')
        });

        tc.Run("Clean Top Screen leaves out what the bottom screen shows and the game gives it", [](TestCase &t) {
            static Snapshot given;
            given = Snapshot{};
            setGameCapture([](const uint8_t *, size_t, Snapshot &out) {
                out.mode = GameMode::Play;
                out.timerSeconds = given.timerSeconds;
                out.activeGoal = given.activeGoal;
                std::memcpy(out.line, given.line, sizeof(out.line));
                out.pendingScore = 0.0f;
            });
            std::vector<uint8_t> ram(64, 0);
            static Snapshot out;
            captureSnapshot(ram.data(), ram.size(), out);
            const HudPieces all = hudPiece(HudPiece::Clock) | hudPiece(HudPiece::TrickLine) | hudPiece(HudPiece::Goal);

            setCleanTopScreen(CleanTopScreen::Off);
            setBottomScreenShows(all);
            t.IsTrue(bottomScreenUp(), "a picture just painted: a bottom screen is up");
            t.IsTrue(!hiddenOnTop(HudPiece::Clock), "off: the top screen whole");
            setCleanTopScreen(CleanTopScreen::On);
            t.IsTrue(!hiddenOnTop(HudPiece::Clock) && !hiddenOnTop(HudPiece::TrickLine) && !hiddenOnTop(HudPiece::Goal),
                     "on: nothing the game has not given the bottom screen");
            given.timerSeconds = 30.0f;
            std::snprintf(given.line, sizeof(given.line), "%s", "Soul");
            captureSnapshot(ram.data(), ram.size(), out);
            t.IsTrue(hiddenOnTop(HudPiece::Clock) && hiddenOnTop(HudPiece::TrickLine) && !hiddenOnTop(HudPiece::Goal),
                     "the clock and the line the capture gave, not the goal it did not");
            given.line[0] = 0;
            captureSnapshot(ram.data(), ram.size(), out);
            t.IsTrue(hiddenOnTop(HudPiece::TrickLine), "a line given once: the top's line is the bottom's from then on");
            setBottomScreenShows(hudPiece(HudPiece::TrickLine));
            t.IsTrue(!hiddenOnTop(HudPiece::Clock) && hiddenOnTop(HudPiece::TrickLine), "only what the bottom screen's screen shows");
            setBottomScreenShows(0);
            t.IsTrue(!hiddenOnTop(HudPiece::TrickLine), "a reference on the bottom screen: the top screen whole");
            setCleanTopScreen(CleanTopScreen::Always);
            t.IsTrue(hiddenOnTop(HudPiece::Clock) && hiddenOnTop(HudPiece::TrickLine) && hiddenOnTop(HudPiece::Goal),
                     "always (test runs): every piece");
            setCleanTopScreen(CleanTopScreen::Off);
            setGameCapture(nullptr);
        });

        // ---- the layout (panel_layout.h) ----

        tc.Run("the live screen's buttons take a tap along the bottom edge", [](TestCase &t) {
            t.Equals(liveButtonAt({200, 1040}), 0, "GOALS at the left");
            t.Equals(liveButtonAt({620, 1040}), 1, "ITEMS in the middle");
            t.Equals(liveButtonAt({1100, 1040}), 2, "TRICKS at the right");
            t.Equals(liveButtonAt({412, 1079}), 0, "the first button's last pixel");
            t.Equals(liveButtonAt({413, 1079}), 1, "the second's first");
            t.Equals(liveButtonAt({620, 995}), -1, "just above the bar: the map");
            t.Equals(liveButtonAt({620, 500}), -1, "the map");
            float x = 0;
            for (const Rect &b : kLiveButtons)
            {
                t.IsTrue(near(b.x, x) && near(b.y + b.height, kScreen.height), "the buttons lie edge to edge along the bottom");
                t.IsTrue(b.width >= kTouchMinimum && b.height >= kTouchMinimum, "a button is a touch target");
                x += b.width;
            }
            t.IsTrue(near(x, kScreen.width), "the three span the screen");
        });

        tc.Run("the wheel scrolls a reference's list and zooms the map elsewhere", [](TestCase &t) {
            t.IsTrue(wheelTurnsAt({1000, 500}, true) == WheelTurns::List, "a reference: over the side pane, its list");
            t.IsTrue(wheelTurnsAt({kSidePane.x, 1079}, true) == WheelTurns::List, "the pane's first column, its last row");
            t.IsTrue(wheelTurnsAt({kSidePane.x - 1, 500}, true) == WheelTurns::Map, "the map beside it");
            t.IsTrue(wheelTurnsAt({1000, 500}, false) == WheelTurns::Map, "the live screen: the map is the whole screen");
            t.IsTrue(wheelTurnsAt({1000, 1040}, false) == WheelTurns::Nothing, "its buttons take taps, not the wheel");
            t.IsTrue(near(wheelScrolled(0, -1), kWheelScroll), "down a notch: a goal row further down the list");
            t.IsTrue(near(wheelScrolled(3 * kWheelScroll, 2), kWheelScroll), "up two from three rows down: one row down");
            t.IsTrue(near(wheelScrolled(40, 1), 0), "up past the top: the top");
            t.IsTrue(near(wheelScrolled(40, 0.5f), 0) && near(wheelScrolled(100, 0.5f), 100 - kWheelScroll / 2),
                     "a trackpad's part of a notch, in proportion");
        });

        tc.Run("the bottom screen over the game lies in the window's corner", [](TestCase &t) {
            const Rect wide = overlayPlace(1920.0f, 1080.0f);
            t.IsTrue(near(wide.height, 540.0f) && near(wide.width / wide.height, kScreen.width / kScreen.height),
                     "half the height of a wide window, the screen's aspect kept");
            t.IsTrue(near(wide.x + wide.width, 1920.0f - kEdge * 0.5f) && near(wide.y + wide.height, 1080.0f - kEdge * 0.5f),
                     "its edge from the bottom right corner at the scale drawn");
            const Rect tall = overlayPlace(600.0f, 1000.0f);
            t.IsTrue(near(tall.width, 300.0f) && tall.x >= 0.0f && tall.y >= 0.0f, "a narrow window: half its width, inside it");
            const Rect none = overlayPlace(0.0f, 1080.0f);
            t.IsTrue(none.width == 0.0f && none.height == 0.0f, "no window: no place");
        });

        tc.Run("the live screen's overlays stay apart and inside the screen", [](TestCase &t) {
            t.IsTrue(kGoalCardFrame.x + kGoalCardFrame.width < kClockTile.x, "the card ends before the clock");
            t.IsTrue(kClockTile.x + kClockTile.width <= kScreen.width - kEdge + 0.5f, "the clock keeps its edge");
            t.IsTrue(kWhereTile.y + kWhereTile.height <= kLiveButtons[0].y, "the where-to tile sits above the buttons");
            t.IsTrue(kWhereTile.x + kWhereTile.width <= kScreen.width - kEdge + 0.5f, "and keeps its edge");
            t.IsTrue(kLiveReturnButton.x + kLiveReturnButton.width <= kWhereTile.x, "the return button lies left of the tile");
            t.IsTrue(kLiveReturnTouch.width >= kTouchMinimum && kLiveReturnTouch.height >= kTouchMinimum, "the return button's touch area");
            t.IsTrue(kLiveMapClear.y + kLiveMapClear.height <= kLiveButtons[0].y, "the whole level on pause stays above the buttons");
            t.IsTrue(near(kSplitMapPane.width + kSidePane.width, kScreen.width), "a reference's split spans the screen");
        });

        tc.Run("the follow view keeps the skater a third up from the bottom", [](TestCase &t) {
            const Point skater{800, 700};
            for (const Rect &pane : {kScreen, kSplitMapPane})
                for (float zoom : {1.0f, 2.0f})
                {
                    MapView view;
                    view.zoom = zoom;
                    view.center = followCenter(skater, zoom, pane);
                    const Point at = mapToScreen(view, pane, skater);
                    t.IsTrue(near(at.x, pane.x + pane.width * 0.5f, 0.01f), "centred across");
                    t.IsTrue(near(at.y, pane.y + pane.height * kFollowShare, 0.01f), "two thirds down: most of the map ahead");
                }
        });

        tc.Run("the type's scale follows the font's line height", [](TestCase &t) {
            t.IsTrue(near(scaleFor(19, kTitlePx) * 19.0f, 52.0f), "Aggressive Inline's fnt_14pt at 52 px");
            t.IsTrue(near(scaleFor(18, kTitlePx) * 18.0f, 52.0f), "BMX XXX's at 52 px");
            t.IsTrue(near(scaleFor(23, kClockPx) * 23.0f, 96.0f), "the clock");
            t.IsTrue(near(scaleFor(0, 10.0f), 10.0f), "a font with no line height does not divide by zero");
        });

        tc.Run("letter-spaced text is wider by the tracking", [](TestCase &t) {
            Font font;
            font.lineHeight = 16;
            font.has[static_cast<uint8_t>('A')] = font.has[static_cast<uint8_t>('B')] = true;
            font.glyphs[static_cast<uint8_t>('A')].width = 10;
            font.glyphs[static_cast<uint8_t>('B')].width = 12;
            t.IsTrue(near(Canvas::textWidth(font, "AB", 1.0f), 24.0f), "two glyphs and their gaps");
            t.IsTrue(near(Canvas::textWidth(font, "AB", 1.0f, 3.0f), 27.0f), "tracking between them, none after the last");
            t.IsTrue(near(Canvas::textWidth(font, "AB", 2.0f, 3.0f), 51.0f), "scaled glyphs, tracking in pixels");
            t.IsTrue(near(Canvas::textWidth(font, "", 2.0f, 3.0f), 0.0f), "nothing");
        });

        tc.Run("the goal list groups its rows in the player's order", [](TestCase &t) {
            const std::vector<GoalEntry> entries = {
                {0, GoalGroup::Running, 0}, {1, GoalGroup::Locked, 0}, {2, GoalGroup::CanStart, 240}, {3, GoalGroup::Done, 0},
                {4, GoalGroup::Now, 0}, {5, GoalGroup::CanStart, 131}, {6, GoalGroup::Locked, 0}, {7, GoalGroup::CanStart, 388},
            };
            const std::vector<GoalRow> rows = groupRows(entries, false);
            const std::vector<std::pair<int, GoalGroup>> expected = {
                {-1, GoalGroup::Now}, {4, GoalGroup::Now},
                {-1, GoalGroup::CanStart}, {5, GoalGroup::CanStart}, {2, GoalGroup::CanStart}, {7, GoalGroup::CanStart},
                {-1, GoalGroup::Running}, {0, GoalGroup::Running},
                {-1, GoalGroup::Done}, {3, GoalGroup::Done},
                {-1, GoalGroup::Locked},
            };
            t.Equals(rows.size(), expected.size(), "a header a group, the locked ones folded");
            for (size_t i = 0; i < rows.size() && i < expected.size(); ++i)
                t.IsTrue(rows[i].goal == expected[i].first && rows[i].group == expected[i].second, "row " + std::to_string(i));
            const std::vector<GoalRow> open = groupRows(entries, true);
            t.Equals(open.size(), expected.size() + 2, "tapped: the locked goals listed");
            t.IsTrue(open.back().goal == 6 && open[open.size() - 2].goal == 1, "in their order, under their header");
            t.Equals(groupCount(entries, GoalGroup::CanStart), 3, "the header's count");
            t.Equals(groupCount(entries, GoalGroup::Locked), 2, "the locked count");
            t.Equals(std::string(groupWord(GoalGroup::CanStart)), std::string("CAN START"), "the group's word");
            const std::vector<GoalRow> few = groupRows({{3, GoalGroup::Running, 0}}, false);
            t.IsTrue(few.size() == 2 && few[0].goal == -1 && few[0].group == GoalGroup::Running, "an empty group has no header");
        });

        tc.Run("a goal row is as tall as what it shows, never under a touch target", [](TestCase &t) {
            const float titleGap = kRowTitlePx * 1.05f, statusGap = kRowStatusPx * 1.1f;
            t.IsTrue(near(goalRowHeight(1, titleGap, 0, 0, false), kGoalRowHeight), "one line: the least height");
            t.IsTrue(goalRowHeight(2, titleGap, 0, 0, false) > kGoalRowHeight, "two lines: taller");
            t.IsTrue(near(goalRowHeight(2, titleGap, 0, 0, false), 24 + 2 * titleGap), "by the lines");
            t.IsTrue(near(goalRowHeight(1, titleGap, 1, statusGap, true), 24 + titleGap + statusGap + kRowBarHeight + 14), "the open row: its status and its bar");
            t.IsTrue(kGroupHeaderHeight >= kTouchMinimum && kGoalRowHeight >= kTouchMinimum, "headers and rows are touch targets");
            // the tap's row through headers and rows of mixed height
            const float heights[4] = {kGroupHeaderHeight, goalRowHeight(1, titleGap, 1, statusGap, true), kGroupHeaderHeight, kGoalRowHeight};
            t.Equals(rowAt(92 + 30, 92, 988, 0, heights, 4), 0, "the NOW header");
            t.Equals(rowAt(92 + kGroupHeaderHeight + 10, 92, 988, 0, heights, 4), 1, "the open row");
            t.Equals(rowAt(92 + kGroupHeaderHeight + heights[1] + 10, 92, 988, 0, heights, 4), 2, "the next header");
        });
    });
}
