// Ghost riding (runtime/ghost_rider.h, collection/ghost.h): a track written and read back, the stretch a level
// rides, and the ghost's draw made from a live draw (its bones, its place in the live view, see-through).
#include "MiniTest.h"

#include "collection/ghost.h"
#include "runtime/boot_config.h"
#include "runtime/ghost_rider.h"

#include <cmath>
#include <filesystem>
#include <memory>

namespace
{
    namespace gr = zx::ghost_rider;

    std::filesystem::path scratch(const char *name)
    {
        std::error_code ec;
        const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "zaxis-ghost-tests" / name;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    gr::PartPose part(uint32_t vertices, uint32_t bones, float x)
    {
        gr::PartPose p;
        p.vertices = vertices;
        p.bones = bones;
        for (int i = 0; i < 4; ++i)
            p.world[i][i] = 1.0f;
        p.world[3][0] = x;
        for (uint32_t k = 0; k < bones; ++k)
            for (int i = 0; i < 12; ++i)
                p.palette.push_back(static_cast<float>(k * 100 + i));
        return p;
    }
}

void register_ghost_rider_tests()
{
    MiniTest::Case("GhostRider", [](TestCase &tc)
    {
        tc.Run("a track written stretch by stretch reads back whole; a level rides its longest stretch", [](TestCase &t)
        {
            const std::filesystem::path file = scratch("track") / "ai-run.zghost";
            t.Equals(gr::trackOf(scratch("track") / "ai-run.zrec").filename().string(), std::string("ai-run.zghost"), "the track beside a recording");
            gr::Stretch first{"MOVIELOT", "Taylor", {{part(900, 20, 1.0f)}, {}, {part(900, 20, 2.0f), part(40, 2, 3.0f)}}};
            gr::Stretch second{"CIVIC", "", {{part(900, 20, 4.0f)}}};
            gr::Stretch third{"MOVIELOT", "Taylor", {{part(900, 20, 5.0f)}, {part(900, 20, 6.0f)}, {}, {}}};
            t.IsTrue(gr::appendStretch(file, first), "gr::appendStretch(file, first)");
            t.IsTrue(gr::appendStretch(file, second), "gr::appendStretch(file, second)");
            t.IsTrue(gr::appendStretch(file, third), "gr::appendStretch(file, third)");
            gr::Track track;
            t.IsTrue(gr::readTrack(file, track), "gr::readTrack(file, track)");
            t.Equals(track.stretches.size(), size_t{3}, "track.stretches.size()");
            const gr::Stretch &back = track.stretches[0];
            t.Equals(back.level, std::string("MOVIELOT"), "back.level");
            t.Equals(back.rider, std::string("Taylor"), "back.rider");
            t.Equals(back.frames.size(), size_t{3}, "back.frames.size()");
            t.IsTrue(back.frames[1].empty(), "back.frames[1].empty()");
            t.Equals(back.frames[2].size(), size_t{2}, "back.frames[2].size()");
            t.Equals(back.frames[2][1].vertices, 40u, "back.frames[2][1].vertices");
            t.Equals(back.frames[2][1].bones, 2u, "back.frames[2][1].bones");
            t.Equals(back.frames[2][1].world[3][0], 3.0f, "back.frames[2][1].world[3][0]");
            t.Equals(back.frames[2][1].world[3][3], 1.0f, "back.frames[2][1].world[3][3]");
            t.Equals(back.frames[2][1].palette.size(), size_t{24}, "back.frames[2][1].palette.size()");
            t.Equals(back.frames[2][1].palette[23], 111.0f, "back.frames[2][1].palette[23]");
            const gr::Stretch *rides = gr::stretchFor(track, "MOVIELOT");
            t.IsTrue(rides == &track.stretches[2], "rides == &track.stretches[2]");
            t.IsTrue(gr::stretchFor(track, "MUSEUM") == nullptr, "gr::stretchFor(track, \"MUSEUM\") == nullptr");
            // What it holds, its frames not read.
            const std::vector<gr::StretchSummary> heads = gr::trackStretches(file);
            t.Equals(heads.size(), size_t{3}, "the stretches' heads");
            t.Equals(heads[2].frames, 4u, "the third's frames");
            t.Equals(heads[2].rider, std::string("Taylor"), "the third's rider");
            t.Equals(heads[1].rider, std::string(), "a stretch with no rider named");

            // Cut short: the whole stretches before the cut.
            std::filesystem::resize_file(file, std::filesystem::file_size(file) - 3u);
            t.IsTrue(gr::readTrack(file, track), "gr::readTrack(file, track)");
            t.Equals(track.stretches.size(), size_t{2}, "track.stretches.size()");
            gr::Track none;
            t.IsTrue(!gr::readTrack(scratch("track") / "missing.zghost", none), "no track in a missing file");
        });

        tc.Run("the ghost's draw: the live draw's mesh with the ghost's bones and place in the live view, see-through", [](TestCase &t)
        {
            GSNativeDraw live;
            live.first = 30;
            live.count = 60;
            live.motion.key = gsNativeMotionKey(0x123400u, 0);
            for (int i = 0; i < 4; ++i)
                live.motion.world[i][i] = 1.0f;
            auto skin = std::make_shared<GSNativeSkinning>();
            skin->lit = true;
            skin->lightDir[0][0] = 1.0f; // light 0 along the object's x
            live.skin = skin;
            gr::Ghost ghost{part(900, 2, 10.0f), true};
            // The ghost turned a quarter about y: its x axis is the world's -z.
            ghost.pose.world[0][0] = 0.0f;
            ghost.pose.world[0][2] = -1.0f;
            ghost.pose.world[2][0] = 1.0f;
            ghost.pose.world[2][2] = 0.0f;
            float screen[4][4] = {}, clip[4][4] = {};
            for (int i = 0; i < 4; ++i)
                screen[i][i] = clip[i][i] = 2.0f;
            const GSNativeDraw g = gr::ghostDraw(live, ghost, screen, clip);
            t.Equals(g.seeThrough, gr::kSeeThrough, "g.seeThrough");
            t.Equals(g.transform[3][0], 20.0f, "g.transform[3][0]");            // position x 10, times the screen's 2
            t.Equals(g.transform[0][2], -32.0f, "g.transform[0][2]");           // -1 x 2, Z times 16
            t.Equals(g.clipTransform[3][0], 20.0f, "g.clipTransform[3][0]");
            t.Equals(g.skin->palette[4][0], 100.0f, "g.skin->palette[4][0]");       // bone 1's first row
            t.Equals(g.skin->palette[7][2], 111.0f, "g.skin->palette[7][2]");       // its translation's z
            t.Equals(g.skin->palette[7][3], 1.0f, "g.skin->palette[7][3]");
            t.Equals(g.skin->palette[0][3], 0.0f, "g.skin->palette[0][3]");
            // The live node is unturned, so the world's light is along x; the ghost's x is the world's -z: in the
            // ghost's space the light is along its z.
            t.IsTrue(std::abs(g.skin->lightDir[2][0] - 1.0f) < 1e-6f && std::abs(g.skin->lightDir[0][0]) < 1e-6f, "the light turned into the ghost's space");
            t.IsTrue(g.motion.key != live.motion.key && g.motion.key != 0u, "the ghost's own motion key");
            t.Equals(live.skin->palette[4][0], 0.0f, "live.skin->palette[4][0]"); // (the live draw's skin untouched)
            GSNativeDraw run = live;
            run.first = 90;
            run.count = 12;
            const GSNativeDraw r = gr::ofRun(g, run);
            t.Equals(r.first, 90u, "r.first");
            t.Equals(r.count, 12u, "r.count");
            t.Equals(r.transform[3][0], 20.0f, "r.transform[3][0]");
        });

        tc.Run("the app's boot: a track beside a recording made or played back; the ghost chosen for its game once", [](TestCase &t)
        {
            namespace col = zx::collection;
            zx::BootConfig recording;
            recording.set("ZX_RECORD", "/tmp/recordings");
            col::ghostBootConfig(recording, "ai", false, std::nullopt);
            t.Equals(recording.text("ZX_GHOST_TRACK"), std::string("1"), "recording.text(\"ZX_GHOST_TRACK\")");
            zx::BootConfig plain;
            col::ghostBootConfig(plain, "ai", false, std::nullopt);
            t.IsTrue(plain.text("ZX_GHOST_TRACK").empty(), "plain.text(\"ZX_GHOST_TRACK\").empty()");

            col::RecordingEntry entry;
            entry.game = "ai";
            entry.file = "/tmp/ai-run.zrec";
            entry.level = "MOVIELOT";
            entry.levelsNoted = true;
            col::chooseGhost(entry);
            const std::optional<col::GhostChoice> choice = col::takeGhostChoice();
            t.IsTrue(choice.has_value(), "choice.has_value()");
            t.IsTrue(!col::takeGhostChoice().has_value(), "!col::takeGhostChoice().has_value()");
            zx::BootConfig other;
            col::ghostBootConfig(other, "dm2", false, choice);
            t.IsTrue(other.text("ZX_GHOST").empty(), "other.text(\"ZX_GHOST\").empty()");
            zx::BootConfig riding;
            col::ghostBootConfig(riding, "ai", false, choice);
            t.Equals(riding.text("ZX_GHOST"), std::string("/tmp/ai-run.zrec"), "riding.text(\"ZX_GHOST\")");
            t.Equals(riding.text("ZX_GHOST_LEVEL"), std::string("MOVIELOT"), "riding.text(\"ZX_GHOST_LEVEL\")");
            zx::BootConfig playback;
            col::ghostBootConfig(playback, "ai", true, choice);
            t.IsTrue(playback.text("ZX_GHOST").empty(), "playback.text(\"ZX_GHOST\").empty()");
            t.Equals(playback.text("ZX_GHOST_TRACK"), std::string("1"), "playback.text(\"ZX_GHOST_TRACK\")");

            // Aggressive Inline only: DM2 and BMX XXX make no tracks and ride no ghosts from the app.
            zx::BootConfig dm2Recording;
            dm2Recording.set("ZX_RECORD", "/tmp/recordings");
            col::ghostBootConfig(dm2Recording, "dm2", false, std::nullopt);
            t.IsTrue(dm2Recording.text("ZX_GHOST_TRACK").empty(), "no track made in DM2");
            t.IsTrue(!col::offersGhosts("dm2") && !col::offersGhosts("bmx") && col::offersGhosts("ai"), "AI only");
        });

        tc.Run("Ride Against: offered for a recording whose track rode its level, in Aggressive Inline, naming the rider", [](TestCase &t)
        {
            namespace col = zx::collection;
            const std::filesystem::path recording = scratch("offer") / "ai-run.zrec";
            col::RecordingEntry entry;
            entry.game = "ai";
            entry.file = recording;
            entry.level = "MOVIELOT";
            entry.levelsNoted = true;
            t.IsTrue(!col::canRideAgainst(entry), "no track: not offered");
            t.IsTrue(gr::appendStretch(gr::trackOf(recording), gr::Stretch{"MOVIELOT", "Taylor", {{part(900, 20, 1.0f)}}}),
                     "the track written");
            t.IsTrue(col::canRideAgainst(entry), "offered");
            t.Equals(col::ghostRider(entry), std::string("Taylor"), "its rider named");
            col::RecordingEntry civic = entry;
            civic.level = "CIVIC";
            t.IsTrue(!col::canRideAgainst(civic), "not offered for a level the track has no play in");
            col::RecordingEntry dm2 = entry;
            dm2.game = "dm2";
            t.IsTrue(!col::canRideAgainst(dm2), "not offered in DM2");
        });
    });
}
