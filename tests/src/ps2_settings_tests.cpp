#include "MiniTest.h"
#include "ps2_runtime.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_settings.h"
#include "runtime/ps2_tuning_ini.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_zaxis_ui.h"
#include "zge_volumes.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace st = ps2x::settings;
namespace hi = ps2_host_input;
namespace zx = ps2x::zaxis;

namespace
{
    // A game's frame function for zge_volumes' hook (it only counts).
    int g_gameFrames = 0;
    void countedGameFrame(uint8_t *, R5900Context *, PS2Runtime *) { ++g_gameFrames; }

    std::vector<uint8_t> bytesOf(const std::string &text)
    {
        return std::vector<uint8_t>(text.begin(), text.end());
    }

    // A small UIBuilder-style script (CRLF, tabs), as in UILIB.ZAL.
    std::string sampleScript()
    {
        return zx::indentBlock("///////////////////////////\n"
                               "// UIBuilder Generated File\n"
                               "OBJECT \"guiScreenSpace\" \"FrontEnd\"\n"
                               "{\n"
                               "\tVisible\t1\n"
                               "\tOBJECT \"guiGroup\" \"Main\"\n"
                               "\t{\n"
                               "\t\tOBJECT \"guiGroup\" \"Options\"\n"
                               "\t\t{\n"
                               "\t\t\tVisible\t0\n"
                               "\t\t}\n"
                               "\t}\n"
                               "\tOBJECT \"guiGroup\" \"Options\"\n"
                               "\t{\n"
                               "\t\tOnCreate\t\"SET Visible {0}\"\n"
                               "\t\tACTION \"Script\" \"GoBack\"\n"
                               "\t\t{\n"
                               "\t\t\tScript\t{\n"
                               "\t\t\t\tPLAY .Controller.MoveIn\n"
                               "\t\t\t\t}\n"
                               "\t\t}\n"
                               "\t}\n"
                               "}\n",
                               0);
    }
}

void register_ps2_settings_tests()
{
    MiniTest::Case("Settings", [](TestCase &tc)
    {
        tc.Run("tuning.ini: names by category, any case, ambiguous names, comments", [](TestCase &t)
        {
            namespace tn = ps2x::tuning;
            std::vector<tn::Entry> entries;
            auto add = [&entries](const char *category, const char *name, uint32_t address, float value)
            {
                tn::Entry e;
                e.category = category;
                e.name = name;
                e.address = address;
                e.value = value;
                e.minValue = 0.0f;
                e.maxValue = 100.0f;
                e.step = 0.5f;
                entries.push_back(e);
            };
            add("Camera", "FOV", 0x100, 75.0f);
            add("Physics", "Forward Friction", 0x200, 0.0f);
            add("Grind/Walls", "Forward Friction", 0x300, 0.0f);
            add("Control", "# cmd delay frames", 0x400, 3.0f);
            add("BoobTech", "Implant Size", 0x500, 1.0f);
            add("Mem Check", "Implant Size", 0x500, 1.0f);
            std::vector<std::string> warnings;
            const auto lines = tn::parse("; comment\r\n"
                                         "fov = 90\r\n"
                                         "[physics]\n"
                                         "  Forward Friction =  0.25 \n"
                                         "[]\n"
                                         "Forward Friction = 1\n"
                                         "# cmd delay frames = 5\n"
                                         "# FOV = 10 (a comment)\n"
                                         "implant size = 2\n"
                                         "Nope = 1\n"
                                         "FOV = wide\n"
                                         "no equals\n",
                                         &warnings);
            t.Equals(lines.size(), static_cast<size_t>(6), "lines with a number (the '#' comment too)");
            const auto overrides = tn::match(lines, entries, &warnings);
            t.Equals(overrides.size(), static_cast<size_t>(4), "matched entries");
            t.IsTrue(overrides.size() == 4 && overrides[0].entry->address == 0x100 && overrides[0].value == 90.0f,
                     "no section: any category, any case");
            t.IsTrue(overrides.size() == 4 && overrides[1].entry->address == 0x200 && overrides[1].value == 0.25f,
                     "the section picks the category");
            t.IsTrue(overrides.size() == 4 && overrides[2].entry->address == 0x400 && overrides[2].value == 5.0f,
                     "a name starting with '#'");
            t.IsTrue(overrides.size() == 4 && overrides[3].entry->address == 0x500, "the same entry in two categories");
            t.Equals(warnings.size(), static_cast<size_t>(4), "bad number, no '=', ambiguous name, unknown name");
            const std::string reference = tn::reference(entries, "; header\n");
            t.IsTrue(reference.find("[Camera]\n; FOV = 75    (0 to 100, step 0.5)\n") != std::string::npos,
                     "reference lists entries by category, commented out");
        });
        tc.Run("registry: values, steps, texts, applies on the window thread", [](TestCase &t)
        {
            st::clear();
            int applied = -1;
            int source = 3;
            st::Setting volume;
            volume.key = "audio.volume";
            volume.label = "Master Volume";
            volume.kind = st::Kind::Range;
            volume.minValue = 0;
            volume.maxValue = 100;
            volume.step = 5;
            volume.unit = "%";
            volume.current = [] { return 80; };
            volume.apply = [&applied](int v) { applied = v; };
            st::add(volume);

            st::Setting mode;
            mode.key = "display.window";
            mode.label = "Display Mode";
            mode.kind = st::Kind::Choice;
            mode.choices = {"Window", "Full Screen", "Borderless"};
            mode.current = [&source] { return source; };
            st::add(mode);

            st::Setting vsync;
            vsync.key = "display.vsync";
            vsync.label = "VSync";
            st::add(vsync);

            t.Equals(st::value("audio.volume"), 80, "initial value from current()");
            t.Equals(st::value("display.window"), 2, "current() clamped to the choices");
            t.Equals(st::keys("display.").size(), static_cast<size_t>(2), "keys by group, in order");
            t.Equals(st::keys("display.").front(), std::string("display.window"), "registration order");
            t.Equals(st::text("display.vsync"), std::string("Off"), "toggles read Off/On");

            t.IsTrue(st::step("audio.volume", 1, false), "step a range");
            t.Equals(st::value("audio.volume"), 85, "by its step");
            t.Equals(st::text("audio.volume"), std::string("85%"), "with its unit");
            t.Equals(applied, -1, "not applied before the window thread runs");
            st::runPending();
            t.Equals(applied, 85, "applied by runPending");
            st::set("audio.volume", 250);
            t.Equals(st::value("audio.volume"), 100, "clamped to the range");

            st::step("display.window", 1, true);
            t.Equals(st::value("display.window"), 0, "choices wrap when asked");
            st::step("display.window", -1, false);
            t.Equals(st::value("display.window"), 0, "and stop at the ends otherwise");
            st::runPending();
            source = 1;
            st::runPending();
            t.Equals(st::value("display.window"), 1, "a change made elsewhere shows up after runPending");
            t.IsFalse(st::set("nope", 1), "unknown keys are refused");
            st::clear();
        });

        tc.Run("registry: actions and the open request", [](TestCase &t)
        {
            st::clear();
            int activated = 0;
            st::Setting reset;
            reset.key = "controls.reset";
            reset.label = "Reset";
            reset.kind = st::Kind::Action;
            reset.activate = [&activated] { ++activated; };
            reset.format = [](int) { return std::string("SPACE, K"); };
            st::add(reset);
            t.Equals(st::text("controls.reset"), std::string("SPACE, K"), "an action shows its format()");
            t.IsFalse(st::set("controls.reset", 1), "an action has no value");
            t.IsTrue(st::activate("controls.reset"), "activate");
            t.Equals(activated, 0, "on the window thread only");
            st::runPending();
            t.Equals(activated, 1, "ran");

            t.IsFalse(st::requestOpen(), "no settings screen yet");
            int opened = 0;
            st::setOpenHandler([&opened] { ++opened; });
            t.IsTrue(st::requestOpen(), "a game override shows them");
            st::runPending();
            t.Equals(opened, 1, "the handler ran on the window thread");
            st::clear();
        });

        tc.Run("registry: a change from the game's page is run before the game goes on, in the order made", [](TestCase &t)
        {
            st::clear();
            std::vector<std::string> runs;
            std::mutex runsMutex;
            auto row = [&](const char *key)
            {
                st::Setting setting;
                setting.key = key;
                setting.label = key;
                setting.kind = st::Kind::Range;
                setting.minValue = 0;
                setting.maxValue = 10;
                setting.apply = [&, key](int value)
                {
                    std::lock_guard<std::mutex> lock(runsMutex);
                    runs.push_back(std::string(key) + "=" + std::to_string(value));
                };
                st::add(setting);
            };
            row("game.b");
            row("game.a");

            // No host frame loop: run at once, on this thread.
            t.IsTrue(st::stepFromGame("game.b", 1, false), "a step with no frame loop");
            t.Equals(runs.size(), static_cast<size_t>(1), "run at once");

            // Changes made elsewhere first run in the order made (b before a, not by key).
            st::set("game.b", 5);
            st::set("game.a", 7);
            st::runPending();
            t.Equals(runs.size(), static_cast<size_t>(3), "both run");
            t.Equals(runs[1], std::string("game.b=5"), "the first change first");
            t.Equals(runs[2], std::string("game.a=7"), "then the second");

            // A host frame loop on another thread: the game's step returns only once it has run it.
            st::setFrameLoop(true);
            std::atomic<bool> stop{false};
            std::thread window([&]()
                               {
                                   while (!stop.load())
                                   {
                                       std::this_thread::sleep_for(std::chrono::milliseconds(5));
                                       st::runPending();
                                   } });
            for (int i = 0; i < 20; ++i)
            {
                t.IsTrue(st::stepFromGame(i % 2 ? "game.a" : "game.b", 1, false), "a step from the game");
                std::lock_guard<std::mutex> lock(runsMutex);
                t.Equals(runs.size(), static_cast<size_t>(4 + i), "run before the game goes on");
            }
            t.Equals(runs.back(), std::string("game.a=10"), "a's tenth step from 7: at its top");
            // The loop ends: a game thread waiting goes on.
            stop.store(true);
            window.join();
            std::atomic<bool> returned{false};
            std::thread game([&]()
                             {
                                 st::stepFromGame("game.b", -1, false);
                                 returned.store(true); });
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            t.IsFalse(returned.load(), "waits while the loop runs no frame");
            st::setFrameLoop(false);
            game.join();
            t.IsTrue(returned.load(), "goes on when the loop ends");
            st::clear();
        });

        tc.Run("controls.ini: values written back keep the file", [](TestCase &t)
        {
            const std::string text = "# comment\n[buttons]\ncross    = SPACE, K   # jump\nsquare   = J\n\n[mouse]\ninvert_y = true\n";
            std::string out = hi::setIniValue(text, "buttons", "cross", "J");
            t.Equals(out, std::string("# comment\n[buttons]\ncross    = J          # jump\nsquare   = J\n\n[mouse]\ninvert_y = true\n"),
                     "value replaced, alignment and comment kept");
            out = hi::setIniValue(out, "mouse", "sensitivity_x", "1.5");
            t.IsTrue(out.find("invert_y = true\nsensitivity_x = 1.5\n") != std::string::npos, "a missing key is added to its section");
            out = hi::setIniValue(out, "gamepad", "cross", "EAST");
            t.IsTrue(out.find("\n[gamepad]\ncross = EAST\n") != std::string::npos, "a missing section is added");
            out = hi::setIniValue(out, "buttons", "square", "");
            t.IsTrue(out.find("square   =\n") != std::string::npos, "an empty value unbinds");
            const std::string crlf = hi::setIniValue("[buttons]\r\ncross = K\r\n", "buttons", "cross", "L");
            t.Equals(crlf, std::string("[buttons]\r\ncross = L\r\n"), "CRLF files stay CRLF");

            // The documented defaults, edited the way the settings screen
            // does, read back as the edit.
            std::string ini = hi::HostInputConfig::defaultIniText();
            ini = hi::setIniValue(ini, "buttons", "cross", "J, SPACE");
            ini = hi::setIniValue(ini, "buttons", "square", "MOUSE_LEFT");
            ini = hi::setIniValue(ini, "mouse", "sensitivity_x", "1.5");
            ini = hi::setIniValue(ini, "mouse", "invert_y", "false");
            ini = hi::setIniValue(ini, "gamepad", "stick_threshold", "0.35");
            hi::HostInputConfig config = hi::HostInputConfig::defaults();
            std::vector<std::string> warnings;
            config.applyIni(ini, &warnings);
            t.IsTrue(warnings.empty(), "the edited file reads without warnings");
            t.Equals(hi::formatBindings(config.buttons[hi::kCross]), std::string("J, SPACE"), "rebound keys");
            t.Equals(hi::formatBindings(config.buttons[hi::kSquare]), std::string("MOUSE_LEFT"), "a binding removed");
            t.IsTrue(config.mouseSensitivityX == 1.5f && !config.mouseInvertY, "mouse values");
            t.IsTrue(config.gamepadStickThreshold == 0.35f, "stick dead zone");
            t.Equals(hi::formatBindings(hi::HostInputConfig::defaults().buttons[hi::kR2]), std::string("C"),
                     "defaults format as written");
            std::vector<hi::Binding> half{{hi::hostCodeByName("C"), 0.5f}};
            t.Equals(hi::formatBindings(half), std::string("C*0.5"), "pressure values are kept");
        });

        tc.Run("gamepad buttons: rebinding in controls.ini", [](TestCase &t)
        {
            hi::HostInputConfig config = hi::HostInputConfig::defaults();
            t.Equals(config.gamepadButtons[hi::kCross].size(), static_cast<size_t>(1), "one button by default");
            t.Equals(config.gamepadButtons[hi::kCross][0], static_cast<int>(hi::kCross), "its own");
            std::vector<std::string> warnings;
            config.applyIni("[gamepad]\ncross = EAST\ncircle = SOUTH, Y\nl3 =\nr3 = BOGUS\n", &warnings);
            t.Equals(warnings.size(), static_cast<size_t>(1), "unknown gamepad button reported");
            t.Equals(hi::gamepadButtonName(config.gamepadButtons[hi::kCross][0]), std::string("EAST"), "cross on east");
            t.IsTrue(config.gamepadButtons[hi::kL3].empty(), "l3 unbound");

            hi::HostInputMapper mapper(config);
            hi::RawHostInput raw;
            raw.gamepad.present = true;
            raw.gamepad.buttons = static_cast<uint16_t>(1u << hi::kCircle); // the east button
            hi::PadReport report = mapper.map(raw, 1.0);
            t.IsTrue((report.buttons & (1u << hi::kCross)) != 0u, "east presses the game's cross");
            t.IsTrue((report.buttons & (1u << hi::kCircle)) == 0u, "and not circle any more");
            raw.gamepad.buttons = static_cast<uint16_t>(1u << hi::kTriangle); // north = Y
            report = mapper.map(raw, 1.0);
            t.IsTrue((report.buttons & (1u << hi::kCircle)) != 0u, "north presses circle (alias Y)");
            raw.gamepad.buttons = static_cast<uint16_t>(1u << hi::kL3);
            report = mapper.map(raw, 1.0);
            t.Equals(static_cast<int>(report.buttons), 0, "an unbound button does nothing");

            hi::HostInputMapper plain(hi::HostInputConfig::defaults());
            raw.gamepad.buttons = static_cast<uint16_t>((1u << hi::kStart) | (1u << hi::kR2));
            report = plain.map(raw, 1.0);
            t.Equals(static_cast<int>(report.buttons), static_cast<int>((1u << hi::kStart) | (1u << hi::kR2)),
                     "default mapping passes buttons through as before");
        });

        tc.Run("Z-Axis archive: read, write, rebuild with a longer entry", [](TestCase &t)
        {
            std::vector<zx::ArchiveEntry> entries(3);
            entries[0].nameHash = 0x1234u;
            entries[0].flags = 5u;
            entries[0].data = bytesOf("ULIB....library bytes");
            entries[1].nameHash = 0x5678u;
            entries[1].flags = 5u;
            entries[1].data = bytesOf(sampleScript());
            entries[2].nameHash = 0x9ABCu;
            entries[2].flags = 5u;
            entries[2].data = bytesOf("x");
            const std::vector<uint8_t> file = zx::writeArchive(entries);
            t.IsTrue(std::memcmp(file.data(), "ZDIR", 4) == 0, "ZDIR header");
            t.Equals(file.size() % 16u, static_cast<size_t>(0), "padded to 16 bytes");
            std::vector<zx::ArchiveEntry> back;
            std::string error;
            t.IsTrue(zx::readArchive(file, back, &error), "reads back: " + error);
            t.Equals(back.size(), static_cast<size_t>(3), "three entries");
            t.IsTrue(back[1].data == entries[1].data && back[1].nameHash == 0x5678u && back[1].flags == 5u, "same entry");
            t.Equals(zx::writeArchive(back), file, "rewriting gives the same bytes");
            t.Equals(zx::scriptName(back[1]), std::string("FrontEnd"), "the script's main object");
            t.Equals(zx::scriptName(back[0]), std::string(), "a library is not a script");

            back[1].data.insert(back[1].data.end(), 100, ' ');
            std::vector<zx::ArchiveEntry> grown;
            t.IsTrue(zx::readArchive(zx::writeArchive(back), grown), "a longer entry is laid out again");
            t.IsTrue(grown[2].data == entries[2].data, "the entries after it move");
            t.IsFalse(zx::readArchive(bytesOf("not an archive at all, really"), grown), "garbage is refused");
        });

        tc.Run("UI script: objects by path and edits", [](TestCase &t)
        {
            std::string text = sampleScript();
            const zx::Span options = zx::findObject(text, "FrontEnd/Options");
            t.IsTrue(options.found(), "direct child found");
            t.IsTrue(text.compare(options.begin, 10, "\tOBJECT \"g") == 0, "span starts at its OBJECT line");
            t.IsTrue(text.find("OnCreate", options.begin) < options.end, "contains its properties");
            const zx::Span nested = zx::findObject(text, "FrontEnd/Main/Options");
            t.IsTrue(nested.found() && nested.begin < options.begin, "the other Options, under Main");
            t.IsFalse(zx::findObject(text, "FrontEnd/Missing").found(), "missing path");
            zx::Span goBack = zx::findObject(text, "GoBack", options);
            t.IsTrue(goBack.found(), "actions found by name");
            t.Equals(zx::indentAt(text, goBack.begin), 2, "indent of its line");
            t.IsTrue(zx::replaceIn(text, goBack, ".Controller.MoveIn", ".PcSettings.MoveIn"), "replace inside");
            t.IsFalse(zx::replaceIn(text, goBack, ".Controller.MoveIn", "x"), "only inside the span");
            t.IsTrue(text.find(".PcSettings.MoveIn") != std::string::npos, "edited");
            const std::vector<zx::ObjectInfo> objects = zx::listObjects(text);
            t.Equals(objects.size(), size_t(4), "every OBJECT, no ACTION");
            t.IsTrue(objects.size() == 4 && objects[0].path == "FrontEnd" && objects[0].type == "guiScreenSpace" &&
                         objects[2].path == "FrontEnd/Main/Options" && objects[3].path == "FrontEnd/Options",
                     "paths and types in text order");
            t.IsTrue(objects.size() == 4 && objects[3].span.begin == options.begin && objects[3].span.end == options.end,
                     "spans as findObject's");
            const zx::Span visible = zx::findProperty(text, zx::findObject(text, "FrontEnd/Main/Options"), "Visible");
            t.IsTrue(visible.found() && text.substr(visible.begin, visible.end - visible.begin) == "0", "own property value");
            t.IsFalse(zx::findProperty(text, zx::findObject(text, "FrontEnd/Main"), "Visible").found(),
                      "a child's property is not the object's");
            t.IsTrue(zx::findProperty(text, zx::findObject(text, "FrontEnd"), "Visible").found(), "top level");
            const std::string block = zx::indentBlock("OBJECT \"guiLabel\" \"A\"\n{\n\tVisible\t1\n}\n", 2);
            t.Equals(block, std::string("\t\tOBJECT \"guiLabel\" \"A\"\r\n\t\t{\r\n\t\t\tVisible\t1\r\n\t\t}\r\n"),
                     "blocks re-indented with CRLF");
        });

        tc.Run("UI script: text widgets as narrow as another release's", [](TestCase &t)
        {
            auto script = [](const char *menuScale, const char *titleScale, const char *imageScale)
            {
                return zx::indentBlock(std::string("OBJECT \"guiGroup\" \"Pause\"\n{\n"
                                                   "\tOBJECT \"guiMenu\" \"Menu\"\n\t{\n\t\tScale\t") +
                                           menuScale +
                                           "\n\t\tItems\t{\n\t\t\tResume\n\t\t\t}\n\t}\n"
                                           "\tOBJECT \"guiLabel\" \"Title\"\n\t{\n\t\tScale\t" +
                                           titleScale +
                                           "\n\t}\n"
                                           "\tOBJECT \"guiImage\" \"Frame\"\n\t{\n\t\tScale\t" +
                                           imageScale + "\n\t}\n}\n",
                                       0);
            };
            std::string ps2 = script("1.000000 1.000000 1.000000", "1.500000 1.500000 1.000000", "1.000000 1.000000 1.000000");
            const std::string xbox =
                script("0.800000 1.000000 1.000000", "1.600000 1.600000 1.000000", "0.500000 0.500000 1.000000");
            const std::vector<std::string> paths = zx::narrowLike(ps2, xbox);
            t.Equals(paths.size(), size_t(1), "one widget");
            t.IsTrue(!paths.empty() && paths[0] == "Pause/Menu", "the narrower menu");
            t.IsTrue(ps2.find("Scale\t0.800000 1.000000 1.000000") != std::string::npos, "its width, its own height");
            t.IsTrue(ps2.find("Scale\t1.500000 1.500000 1.000000") != std::string::npos, "a wider one is kept");
            t.IsTrue(ps2.find("Scale\t0.5") == std::string::npos, "pictures are not text");
            std::string again = ps2;
            t.IsTrue(zx::narrowLike(again, xbox).empty() && again == ps2, "once");
        });

        tc.Run("disc patch: contents made once, by file name", [](TestCase &t)
        {
            ps2x::disc_patch::clearPatches();
            int reads = 0;
            ps2x::disc_patch::registerPatch("uilib.zal", [](std::vector<uint8_t> &bytes)
                                            {
                                                bytes.push_back('!');
                                                return true;
                                            });
            t.IsTrue(ps2x::disc_patch::hasPatch("cdrom0:\\ASSETS\\UILIB.ZAL;1"), "any folder, case, version");
            auto read = [&reads](std::vector<uint8_t> &bytes)
            {
                ++reads;
                bytes = bytesOf("abc");
                return true;
            };
            auto first = ps2x::disc_patch::contents("ASSETS/UILIB.ZAL", read);
            auto second = ps2x::disc_patch::contents("UILIB.ZAL", read);
            t.IsTrue(first && *first == bytesOf("abc!"), "patched contents");
            t.IsTrue(first == second && reads == 1, "made once");
            t.IsFalse(static_cast<bool>(ps2x::disc_patch::contents("OTHER.ZAL", read)), "other files untouched");
            ps2x::disc_patch::clearPatches();
        });
        tc.Run("a game volume changed in the settings reads as changed until the game's frame has passed it", [](TestCase &t)
        {
            // zge_volumes' apply leaves the change for the game's next frame, and runPending reads every row
            // again right after its apply: a row whose current() read the byte before that frame read the old
            // value (BMX XXX's Settings page: Right then Left on Music Volume gave 40%, not 50%, when the
            // window thread's runPending came between the apply and the game's frame).
            namespace zv = zge_host::volumes;
            ps2x::session::end();
            PS2Runtime runtime;
            t.IsTrue(runtime.memory().initialize(), "memory");
            uint8_t *rdram = runtime.memory().getRDRAM();
            constexpr uint32_t kFrame = 0x0011F400u, kSoundEvent = 0x0011F500u;
            constexpr uint32_t kSaved = 0x00200000u, kHeard = 0x00200010u, kSoundMode = 0x00200020u;
            t.IsTrue(runtime.registerFunction(kFrame, &countedGameFrame), "the game's frame");
            rdram[kSaved] = 64u; // 50 %
            rdram[kHeard] = 64u;
            const uint32_t soundNotUp = 0xFFFFFFFFu; // only the bytes are written: no sound event
            std::memcpy(rdram + kSoundMode, &soundNotUp, sizeof(soundNotUp));
            static const zv::Volume kVolumes[] = {
                {"audio.test_music_volume", "Music Volume", "", 1u, kSaved, kHeard},
            };
            static const zv::Game kGame{.tag = "[test]", .frame = kFrame, .soundEvent = kSoundEvent,
                                        .fullVolume = 127, .soundMode = kSoundMode, .volumes = kVolumes, .count = 1u};
            t.IsTrue(zv::install(runtime, kGame), "installed");
            st::runPending(); // the rows
            const std::string key = "audio.test_music_volume";
            t.Equals(st::value(key, -1), 50, "the row from the byte");

            R5900Context ctx{};
            ctx.r[29] = _mm_set_epi64x(0, 0x01F00000);
            const auto gameFrame = [&]() { runtime.lookupFunction(kFrame)(rdram, &ctx, &runtime); };

            t.IsTrue(st::step(key, 1, false), "Right");
            st::runPending(); // applied, read again before the game's frame
            t.Equals(st::value(key, -1), 60, "still 60 before the game has it");
            t.Equals(static_cast<int>(rdram[kSaved]), 64, "the byte not yet written");
            t.IsTrue(st::step(key, -1, false), "Left");
            st::runPending();
            t.Equals(st::value(key, -1), 50, "Left from 60");
            g_gameFrames = 0;
            gameFrame();
            t.Equals(g_gameFrames, 1, "the game's own frame ran");
            t.Equals(static_cast<int>(rdram[kSaved]), 64, "the saved byte 50 %");
            t.Equals(static_cast<int>(rdram[kHeard]), 64, "the heard byte 50 %");
            st::runPending();
            t.Equals(st::value(key, -1), 50, "the row from the byte again");

            t.IsTrue(st::step(key, 1, false), "Right");
            st::runPending();
            gameFrame();
            t.Equals(static_cast<int>(rdram[kSaved]), 76, "60 % written by the game's frame");
            st::runPending();
            t.Equals(st::value(key, -1), 60, "and read back from the byte");
            rdram[kSaved] = 25u; // the game's own options menu
            st::runPending();
            t.Equals(st::value(key, -1), 20, "a change the game made is the row's");
            ps2x::session::end();
            t.IsFalse(st::has(key), "the row goes with the session");
        });
    });
}
