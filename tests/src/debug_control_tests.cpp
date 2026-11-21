// Tests for ZX_CONTROL (runtime/debug_control.h): commands appended to
// <dir>/cmd run at VBlank, replies go to <dir>/out.
#include "MiniTest.h"
#include "runtime/debug_control.h"
#include "runtime/pad_script.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
    struct ControlDir
    {
        std::filesystem::path dir;
        ControlDir()
        {
            dir = std::filesystem::temp_directory_path() / ("zx-control-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::remove_all(dir);
            std::filesystem::create_directories(dir);
            std::ofstream(dir / "cmd").close();
            ps2_debug_control::setDirectory(dir.string());
        }
        ~ControlDir()
        {
            ps2_debug_control::setDirectory("");
            std::filesystem::remove_all(dir);
        }
        void send(const std::string &lines)
        {
            std::ofstream(dir / "cmd", std::ios::app) << lines;
        }
        std::vector<std::string> replies() const
        {
            std::vector<std::string> out;
            std::ifstream in(dir / "out");
            std::string line;
            while (std::getline(in, line))
                out.push_back(line);
            return out;
        }
    };
}

void register_debug_control_tests()
{
    MiniTest::Case("DebugControl", [](TestCase &tc)
                   {
        tc.Run("parses button lists", [](TestCase &t)
               {
            t.Equals(ps2_debug_control::parseButtons("CROSS"), PadScript::buttonMask("CROSS"), "single");
            t.Equals(ps2_debug_control::parseButtons("UP+CROSS"),
                     static_cast<uint16_t>(PadScript::buttonMask("UP") | PadScript::buttonMask("CROSS")), "combined");
            t.Equals(ps2_debug_control::parseButtons("CROSS+NOPE"), static_cast<uint16_t>(0), "unknown name");
        });

        tc.Run("press holds then releases, peek and poke reply in order", [](TestCase &t)
               {
            ControlDir control;
            std::vector<uint8_t> ram(32u * 1024u * 1024u, 0u);
            const uint32_t value = 0x12345678u;
            std::memcpy(ram.data() + 0x1000, &value, 4);
            control.send("press CROSS 2 1\npeek 0x1000\npoke 0x1004 0xAB\nor 0x1004 0x100\n");
            uint64_t tick = 100;
            ps2_debug_control::onVsync(ram.data(), tick);
            t.Equals(ps2_debug_control::buttons(), PadScript::buttonMask("CROSS"), "held while pressing");
            ps2_debug_control::onVsync(ram.data(), ++tick);
            t.Equals(ps2_debug_control::buttons(), PadScript::buttonMask("CROSS"), "still held");
            ps2_debug_control::onVsync(ram.data(), ++tick);
            t.Equals(ps2_debug_control::buttons(), static_cast<uint16_t>(0), "released after 2 vsyncs");
            ps2_debug_control::onVsync(ram.data(), ++tick);
            const auto replies = control.replies();
            t.Equals(replies.size(), static_cast<size_t>(4), "four replies");
            t.Equals(replies[0], std::string("1 ok"), "press done");
            t.Equals(replies[1], std::string("2 ok 12345678"), "peek");
            uint32_t poked = 0;
            std::memcpy(&poked, ram.data() + 0x1004, 4);
            t.Equals(poked, 0x1ABu, "poke then or");
        });

        tc.Run("until waits for a guest condition", [](TestCase &t)
               {
            ControlDir control;
            std::vector<uint8_t> ram(32u * 1024u * 1024u, 0u);
            control.send("until 0x2000==7 50\necho done\n");
            ps2_debug_control::onVsync(ram.data(), 1);
            ps2_debug_control::onVsync(ram.data(), 2);
            t.Equals(control.replies().size(), static_cast<size_t>(0), "still waiting");
            const uint32_t seven = 7u;
            std::memcpy(ram.data() + 0x2000, &seven, 4);
            ps2_debug_control::onVsync(ram.data(), 3);
            const auto replies = control.replies();
            t.Equals(replies.size(), static_cast<size_t>(2), "condition met, echo ran");
            t.Equals(replies[0], std::string("1 ok tick=3"), "until reply");
            t.Equals(replies[1], std::string("2 ok done tick=3"), "echo reply");
        });
    });
}
