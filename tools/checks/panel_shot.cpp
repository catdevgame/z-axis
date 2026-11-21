// zx-panelshot: the Thor panel on the Mac, from a RAM dump of a real run.
//   zx-panelshot --data <pmap folder> --ram <file.ram> --out <file.png>
//                 [--touch "tap X Y; drag X Y X Y; pinch X0 Y0 X1 Y1 SCALE; wheel X Y NOTCHES; wait S"] [--time N]
//                 [--goal-lines 1]  (each goal's title and progress line as the list shows them, and its bar's count)
//   zx-panelshot --data <pmap folder> --text-fit 1
//                 every goal of every level file in the folder (each goal set: AI's PS2 and Xbox scripts) laid out
//                 as the live card, the goal list and the items list draw it: a line for each text, its lines and
//                 those drawn, its widest line against its room; "CUT" for one not shown whole. Exit 1 if any.
//                 [--fit-shot LEVEL:GOAL:PREFIX]  (that goal's card, goal list and items list as PNGs)
//                 [--game dm2]   (Dave Mirra BMX 2's RAM: its own capture, game_overrides/dave_mirra_panel.cpp)
//                 [--dm2-xbox-levels VENICE,GREENVIL]   (its Xbox levels' folders, values 9 on, as levels.txt)
//                 [--game bmx]   (BMX XXX's RAM: its own capture, game_overrides/bmx_xxx_panel.cpp)
// The same panel core the phone runs, with each game's capture compiled in
// (Aggressive Inline's unless --game says another). PNGs of a real run are
// game-derived, so they stay in scratch.

#include "collection/panel.h"
#include "collection/panel_file.h"
#include "collection/panel_layout.h"

namespace aggressive_inline::panel_state
{
    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out);
}
namespace dave_mirra::panel_state
{
    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out);
    void setXboxLevels(const char *const *folders, size_t count);
}
namespace bmx_xxx::panel_state
{
    void capture(const uint8_t *guestRam, size_t ramBytes, panel::Snapshot &out);
}

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    uint32_t crcTable[256];
    void initCrc()
    {
        for (uint32_t n = 0; n < 256; ++n)
        {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crcTable[n] = c;
        }
    }
    uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
    {
        for (size_t i = 0; i < n; ++i)
            c = crcTable[(c ^ p[i]) & 255u] ^ (c >> 8);
        return c;
    }
    void put32(std::vector<uint8_t> &o, uint32_t v)
    {
        for (int s = 24; s >= 0; s -= 8)
            o.push_back(static_cast<uint8_t>(v >> s));
    }
    void chunk(FILE *f, const char *tag, const std::vector<uint8_t> &data)
    {
        std::vector<uint8_t> head;
        put32(head, static_cast<uint32_t>(data.size()));
        std::fwrite(head.data(), 1, 4, f);
        std::fwrite(tag, 1, 4, f);
        std::fwrite(data.data(), 1, data.size(), f);
        uint32_t c = crc(0xFFFFFFFFu, reinterpret_cast<const uint8_t *>(tag), 4);
        c = crc(c, data.data(), data.size()) ^ 0xFFFFFFFFu;
        std::vector<uint8_t> tail;
        put32(tail, c);
        std::fwrite(tail.data(), 1, 4, f);
    }
    // An uncompressed PNG (stored deflate blocks).
    void writePng(const char *path, const uint32_t *pixels, int width, int height)
    {
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return;
        std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        std::vector<uint8_t> hdr;
        put32(hdr, static_cast<uint32_t>(width));
        put32(hdr, static_cast<uint32_t>(height));
        hdr.insert(hdr.end(), {8, 2, 0, 0, 0});
        chunk(f, "IHDR", hdr);
        std::vector<uint8_t> raw;
        raw.reserve((size_t(width) * 3 + 1) * height);
        for (int y = 0; y < height; ++y)
        {
            raw.push_back(0);
            for (int x = 0; x < width; ++x)
            {
                const uint32_t p = pixels[size_t(y) * width + x];
                raw.push_back(p & 255u);
                raw.push_back((p >> 8) & 255u);
                raw.push_back((p >> 16) & 255u);
            }
        }
        std::vector<uint8_t> z = {0x78, 0x01};
        uint32_t a = 1, b = 0;
        for (uint8_t v : raw)
        {
            a = (a + v) % 65521u;
            b = (b + a) % 65521u;
        }
        for (size_t at = 0; at < raw.size();)
        {
            const size_t n = std::min<size_t>(65535, raw.size() - at);
            z.push_back(at + n == raw.size() ? 1 : 0);
            z.push_back(n & 255u);
            z.push_back((n >> 8) & 255u);
            z.push_back(~n & 255u);
            z.push_back((~n >> 8) & 255u);
            z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
            at += n;
        }
        put32(z, (b << 16) | a);
        chunk(f, "IDAT", z);
        chunk(f, "IEND", {});
        std::fclose(f);
    }

    std::vector<uint8_t> g_memory;
    std::unique_ptr<panel::Snapshot> g_snapshot;
    std::vector<uint32_t> g_pixels;

    void loadRam(const std::string &path)
    {
        if (FILE *f = std::fopen(path.c_str(), "rb"))
        {
            (void)!std::fread(g_memory.data(), 1, g_memory.size(), f);
            std::fclose(f);
        }
        else
            std::fprintf(stderr, "no RAM dump %s\n", path.c_str());
        panel::captureSnapshot(g_memory.data(), g_memory.size(), *g_snapshot);
    }

    void touchScript(panel::Panel &p, const std::string &script, double &clock)
    {
        using namespace panel;
        const Surface surface{g_pixels.data(), kScreenWidth, kScreenHeight, kScreenWidth};
        std::stringstream all(script);
        std::string one;
        const auto send = [&](TouchPhase phase, int id, float x, float y) { p.touch({phase, id, x, y, clock}); };
        while (std::getline(all, one, ';'))
        {
            std::stringstream s(one);
            std::string cmd;
            s >> cmd;
            if (cmd == "tap")
            {
                float x, y;
                s >> x >> y;
                send(TouchPhase::Down, 1, x, y);
                clock += 0.05;
                send(TouchPhase::Up, 1, x, y);
            }
            else if (cmd == "drag")
            {
                float x0, y0, x1, y1;
                s >> x0 >> y0 >> x1 >> y1;
                send(TouchPhase::Down, 1, x0, y0);
                for (int i = 1; i <= 8; ++i)
                {
                    clock += 0.016;
                    send(TouchPhase::Move, 1, x0 + (x1 - x0) * i / 8.0f, y0 + (y1 - y0) * i / 8.0f);
                }
                send(TouchPhase::Up, 1, x1, y1);
            }
            else if (cmd == "pinch")
            {
                float x0, y0, x1, y1, factor;
                s >> x0 >> y0 >> x1 >> y1 >> factor;
                const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
                send(TouchPhase::Down, 1, x0, y0);
                send(TouchPhase::Down, 2, x1, y1);
                for (int i = 1; i <= 8; ++i)
                {
                    const float k = 1.0f + (factor - 1.0f) * i / 8.0f;
                    clock += 0.016;
                    send(TouchPhase::Move, 1, cx + (x0 - cx) * k, cy + (y0 - cy) * k);
                    send(TouchPhase::Move, 2, cx + (x1 - cx) * k, cy + (y1 - cy) * k);
                }
                send(TouchPhase::Up, 1, 0, 0);
                send(TouchPhase::Up, 2, 0, 0);
            }
            else if (cmd == "wheel")        // a mouse wheel's notches at X Y (up positive), as the Map Window's
            {
                float x, y, notches;
                s >> x >> y >> notches;
                clock += 0.016;
                p.wheel(x, y, notches, clock);
            }
            else if (cmd == "wait")
            {
                double seconds = 0;
                s >> seconds;
                clock += seconds;
                p.draw(*g_snapshot, clock, surface);
            }
            else if (cmd == "ram")          // the next moment of the run
            {
                std::string path;
                s >> path;
                loadRam(path);
                p.draw(*g_snapshot, clock += 0.05, surface);
            }
            else if (cmd == "line")         // (a shot of the screen as it stands: the state machine's draw)
            {
                p.draw(*g_snapshot, clock += 0.05, surface);
            }
        }
    }
}

namespace
{
    // --text-fit: each level file's goals, each goal as the card's (the game's active goal, running), then the goal
    // list (GOALS) with it chosen and the items list (ITEMS): every text the draws laid out, measured.
    int textFit(const std::string &data, const std::string &shot)
    {
        // --fit-shot LEVEL:GOAL:PREFIX: that goal's three screens as pictures (PREFIX-card.png, -goals.png, -items.png)
        std::string shotLevel, shotPrefix;
        int shotGoal = -1;
        if (const size_t a = shot.find(':'), b = shot.find(':', a == std::string::npos ? a : a + 1); b != std::string::npos)
        {
            shotLevel = shot.substr(0, a);
            shotGoal = std::atoi(shot.substr(a + 1, b - a - 1).c_str());
            shotPrefix = shot.substr(b + 1);
        }
        using namespace panel;
        std::vector<std::filesystem::path> files;
        for (const auto &entry : std::filesystem::directory_iterator(data))
            if (entry.path().extension() == ".pmap" && entry.path().stem() != "UI")
                files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        std::vector<uint32_t> pixels(size_t(kScreenWidth) * kScreenHeight);
        const Surface surface{pixels.data(), kScreenWidth, kScreenHeight, kScreenWidth};
        int texts = 0, cut = 0;
        size_t longest = 0;
        std::string longestText;
        for (const auto &file : files)
        {
            LevelFile level;
            if (!loadLevelFile(file.string(), level))
            {
                std::printf("%s: not read\n", file.filename().string().c_str());
                continue;
            }
            for (size_t set = 0; set < std::max<size_t>(level.goalSets.size(), 1); ++set)
            {
                const GoalSet *goalSet = level.goalSets.empty() ? nullptr : &level.goalSets[set];
                const size_t goals = goalSet ? goalSet->goals.size() : level.goals.size();
                for (size_t goal = 0; goal < goals; ++goal)
                {
                    auto snap = std::make_unique<Snapshot>();
                    snap->mode = GameMode::Play;
                    snap->level = 0;
                    std::snprintf(snap->levelFile, sizeof(snap->levelFile), "%s", file.stem().string().c_str());
                    snap->objectiveCount = static_cast<uint32_t>(goals);
                    if (goalSet && goalSet->keyed)
                    {
                        snap->scriptVariableBytes = goalSet->variableBytes;
                        for (size_t i = 0; i < goalSet->objectiveTexts.size() && i / 2 < kMaxObjectives; ++i)
                            snap->objectiveText[i / 2][i % 2] = goalSet->objectiveTexts[i];
                    }
                    for (size_t i = 0; i < goals; ++i)
                        snap->objectiveState[i] = i == goal ? kStateOpen : kStateInit;
                    snap->activeGoal = static_cast<int32_t>(goal);
                    // the live card; then, each in a panel of its own, the goal list and the items list
                    std::vector<Panel::TextFit> fits;
                    for (int screen = 0; screen < 3; ++screen)
                    {
                        Panel p;
                        p.setDataFolder(data);
                        p.recordTextFits(true);
                        double clock = 1.0;
                        p.draw(*snap, clock, surface);
                        if (screen > 0)
                        {
                            const Rect &button = kLiveButtons[screen - 1]; // GOALS, ITEMS
                            const float x = button.x + button.width / 2, y = button.y + button.height / 2;
                            p.touch({TouchPhase::Down, 1, x, y, clock += 0.05});
                            p.touch({TouchPhase::Up, 1, x, y, clock += 0.05});
                            p.recordTextFits(true); // (only the reference's own texts)
                            p.draw(*snap, clock += 0.05, surface);
                        }
                        for (Panel::TextFit &fit : p.takeTextFits())
                            fits.push_back(std::move(fit));
                        if (file.stem() == shotLevel && static_cast<int>(goal) == shotGoal && set == 0)
                        {
                            static const char *const kScreens[3] = {"card", "goals", "items"};
                            writePng((shotPrefix + "-" + kScreens[screen] + ".png").c_str(), pixels.data(), kScreenWidth, kScreenHeight);
                        }
                    }
                    for (const Panel::TextFit &fit : fits)
                    {
                        ++texts;
                        if (fit.text.size() > longest)
                            longest = fit.text.size(), longestText = fit.text;
                        if (!fit.whole())
                            ++cut;
                        std::printf("%s %s set %zu goal %d %s: %d of %d lines at %.2f, widest %.0f of %.0f \"%s\"\n",
                                    fit.whole() ? "ok " : "CUT", file.stem().string().c_str(), set, fit.goal, fit.where.c_str(),
                                    fit.drawn, fit.lines, fit.scale, fit.widest, fit.width, fit.text.c_str());
                    }
                }
            }
        }
        std::printf("%d texts, %d cut; the longest %zu characters: \"%s\"\n", texts, cut, longest, longestText.c_str());
        return cut ? 1 : 0;
    }
}

int main(int argc, char **argv)
{
    for (int i = 1; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "--text-fit" && std::atoi(argv[i + 1]) != 0)
        {
            std::string data, shot;
            for (int j = 1; j + 1 < argc; j += 2)
                if (std::string(argv[j]) == "--data")
                    data = argv[j + 1];
                else if (std::string(argv[j]) == "--fit-shot")
                    shot = argv[j + 1];
            initCrc();
            return textFit(data, shot);
        }

    std::string data, ram, out, touch, xboxLevels;
    int timeRuns = 0;
    bool goalLines = false;
    panel::setGameCapture(&aggressive_inline::panel_state::capture);
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string key = argv[i];
        if (key == "--data")
            data = argv[i + 1];
        else if (key == "--ram")
            ram = argv[i + 1];
        else if (key == "--out")
            out = argv[i + 1];
        else if (key == "--touch")
            touch = argv[i + 1];
        else if (key == "--time")
            timeRuns = std::atoi(argv[i + 1]);
        else if (key == "--goal-lines")
            goalLines = std::atoi(argv[i + 1]) != 0;
        else if (key == "--game" && std::string(argv[i + 1]) == "dm2")
            panel::setGameCapture(&dave_mirra::panel_state::capture);
        else if (key == "--game" && std::string(argv[i + 1]) == "bmx")
            panel::setGameCapture(&bmx_xxx::panel_state::capture);
        else if (key == "--dm2-xbox-levels")
            xboxLevels = argv[i + 1];
    }
    {
        std::vector<std::string> folders;
        std::stringstream list(xboxLevels);
        for (std::string folder; std::getline(list, folder, ',');)
            if (!folder.empty())
                folders.push_back(folder);
        std::vector<const char *> names;
        for (const std::string &folder : folders)
            names.push_back(folder.c_str());
        dave_mirra::panel_state::setXboxLevels(names.data(), names.size());
    }
    initCrc();
    g_memory.resize(32u << 20);
    g_snapshot = std::make_unique<panel::Snapshot>();
    g_pixels.resize(size_t(panel::kScreenWidth) * panel::kScreenHeight);
    loadRam(ram);
    auto &snapshot = g_snapshot;
    auto &memory = g_memory;
    auto &pixels = g_pixels;
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    panel::captureSnapshot(memory.data(), memory.size(), *snapshot);
    auto t1 = Clock::now();
    std::printf("capture %.1f us: level %d mode %d at %.1f %.1f %.1f score %d goals %u words %u objects %u line \"%s\" trick \"%s\" pending %.0f timer %.1f, snapshot %zu bytes\n",
                std::chrono::duration<double, std::micro>(t1 - t0).count(), snapshot->level, static_cast<int>(snapshot->mode),
                snapshot->x, snapshot->y, snapshot->z, snapshot->score, snapshot->objectiveCount, snapshot->scriptWordCount,
                snapshot->trackedCount, snapshot->line, snapshot->currentTrick, snapshot->pendingScore, snapshot->timerSeconds,
                sizeof(panel::Snapshot));
    // The goals as the game has them: each one's state word, the active goal, its status line, the live targets.
    std::printf("goals:");
    for (uint32_t i = 0; i < snapshot->objectiveCount; ++i)
    {
        const bool again = snapshot->objectiveState[i] != panel::kStateComplete && ((snapshot->objectiveDoneMarks >> i) & 1u);
        std::printf(" %s%s", snapshot->objectiveState[i] == panel::kStateComplete ? "done" : snapshot->objectiveState[i] == panel::kStateInit ? "locked"
                             : snapshot->objectiveState[i] == panel::kStateReady ? "ready" : "open",
                    again ? "+done" : "");
    }
    std::printf("\nactive goal %d status \"%s\" targets %u:", snapshot->activeGoal, snapshot->goalStatus, snapshot->goalTargetCount);
    for (uint32_t i = 0; i < snapshot->goalTargetCount; ++i)
    {
        const panel::GoalTarget &t = snapshot->goalTargets[i];
        std::printf(" [g%d %s %s %.0f,%.0f]", t.goal, t.role == panel::TargetRole::Pickup ? "pickup" : t.role == panel::TargetRole::DropOff ? "dropoff" : "place",
                    t.phase == panel::TargetPhase::Now ? "now" : t.phase == panel::TargetPhase::Later ? "later" : "done", t.x, t.z);
    }
    // The objects the game places now (givers, cars), by name hash.
    std::printf("\ntracked %u:", snapshot->trackedCount);
    for (uint32_t i = 0; i < snapshot->trackedCount; ++i)
        std::printf(" [%08x %.1f,%.1f]", snapshot->tracked[i].nameHash, snapshot->tracked[i].x, snapshot->tracked[i].z);
    // What lies about: each collectable's kind, whether it is there, and where.
    std::printf("\ncollectables %u:", snapshot->collectableCount);
    for (uint32_t i = 0; i < snapshot->collectableCount; ++i)
        std::printf(" [k%u %s %.1f,%.1f]", snapshot->collectables[i].kind, snapshot->collectables[i].shown ? "shown" : "gone",
                    snapshot->collectables[i].x, snapshot->collectables[i].z);
    std::printf("\n");

    panel::Surface surface{pixels.data(), panel::kScreenWidth, panel::kScreenHeight, panel::kScreenWidth};
    panel::Panel panelCore;
    panelCore.setDataFolder(data);
    double clock = 100.0;
    panelCore.draw(*snapshot, clock, surface);
    touchScript(panelCore, touch, clock);
    panelCore.draw(*snapshot, clock += 0.05, surface);
    if (goalLines)
        for (const panel::Panel::GoalLine &line : panelCore.goalLines(*snapshot))
        {
            std::printf("goal-line set %d | %s | %s", line.goalSet, line.title.c_str(), line.progress.c_str());
            if (line.barHave >= 0)
                std::printf(" | bar %d of %d", line.barHave, line.barTarget);
            std::printf("\n");
        }
    if (timeRuns > 0)
    {
        double total = 0, worst = 0;
        for (int i = 0; i < timeRuns; ++i)
        {
            snapshot->x += 0.3f; // the follow view moves each frame, as in play
            const auto a = Clock::now();
            panelCore.draw(*snapshot, clock += 0.05, surface);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
            total += ms;
            worst = std::max(worst, ms);
        }
        std::printf("draw %.2f ms mean, %.2f ms worst, over %d frames\n", total / timeRuns, worst, timeRuns);
    }
    if (!out.empty())
        writePng(out.c_str(), pixels.data(), panel::kScreenWidth, panel::kScreenHeight);
    return 0;
}
