#include "MiniTest.h"
#include "runtime/xbox_disc.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    struct TempDir
    {
        std::filesystem::path path;
        TempDir()
        {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            path = std::filesystem::temp_directory_path() / ("zx-xboxdisc-" + std::to_string(now));
            std::filesystem::create_directories(path);
        }
        ~TempDir()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    void put(const std::filesystem::path &path, const std::string &text)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << text;
    }

}

void register_xbox_disc_tests()
{
    MiniTest::Case("XboxDisc", [](TestCase &tc)
    {
        tc.Run("nothing configured", [](TestCase &t)
        {
            ps2_xbox_disc::reset();
#if !defined(_WIN32)
            unsetenv("ZX_XBOX_DIR");
#endif
            t.IsFalse(ps2_xbox_disc::init(), "no disc");
            t.IsFalse(ps2_xbox_disc::available(), "not available");
            std::vector<uint8_t> out;
            t.IsFalse(ps2_xbox_disc::readFile("levels/ZOO/zoo.TXR", out), "no read");
            t.IsTrue(ps2_xbox_disc::list("levels").empty(), "no listing");
        });

        tc.Run("not an Xbox folder", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "x.iso", std::string(16, '\0'));
            t.IsFalse(ps2_xbox_disc::openPath((dir.path / "x.iso").string()), "an image is not read");
            t.IsFalse(ps2_xbox_disc::openPath(dir.path.string()), "a folder without assets");
            t.IsFalse(ps2_xbox_disc::available(), "nothing open");
            t.IsTrue(ps2_xbox_disc::description().find("not usable") != std::string::npos, "says why");
            ps2_xbox_disc::reset();
        });

        tc.Run("extracted folder", [](TestCase &t)
        {
            TempDir dir;
            put(dir.path / "disc" / "default.xbe", "XBE");
            put(dir.path / "disc" / "media" / "assets" / "levels" / "ZOO" / "zoo.TXR", "FORM!");
            put(dir.path / "disc" / "media" / "assets" / "levels" / "ZOO" / "zoot.bin", "BI");
#if !defined(_WIN32)
            setenv("ZX_XBOX_DIR", (dir.path / "disc").string().c_str(), 1);
            ps2_xbox_disc::reset();
            t.IsTrue(ps2_xbox_disc::init(), "found from ZX_XBOX_DIR");
            t.IsTrue(ps2_xbox_disc::init(), "idempotent");
            unsetenv("ZX_XBOX_DIR");
#else
            t.IsTrue(ps2_xbox_disc::openPath((dir.path / "disc").string()), "opened");
#endif
            std::vector<uint8_t> out;
            t.IsTrue(ps2_xbox_disc::readFile("LEVELS/zoo/zoo.txr", out), "read, any case");
            t.Equals(std::string(out.begin(), out.end()), std::string("FORM!"), "contents");
            t.IsTrue(ps2_xbox_disc::readFile("levels\\ZOO\\zoot.bin", out), "backslashes");
            t.Equals(out.size(), size_t(2), "second file");
            t.IsFalse(ps2_xbox_disc::readFile("levels/ZOO", out), "a directory is not a file");
            t.IsFalse(ps2_xbox_disc::exists("levels/ZOO/none.bin"), "missing file");
            t.IsFalse(ps2_xbox_disc::exists("../disc/default.xbe"), "nothing outside the assets");
            t.Equals(ps2_xbox_disc::list("levels").size(), size_t(1), "listing");
            std::vector<std::string> names = ps2_xbox_disc::list("LEVELS/zoo");
            std::sort(names.begin(), names.end());
            t.IsTrue(names == std::vector<std::string>{"zoo.TXR", "zoot.bin"}, "names with their case");
            t.IsTrue(ps2_xbox_disc::description().find(dir.path.string()) != std::string::npos, "description");
            // The assets folder itself works too.
            t.IsTrue(ps2_xbox_disc::openPath((dir.path / "disc" / "media" / "assets").string()), "assets folder");
            t.IsTrue(ps2_xbox_disc::exists("levels/ZOO/zoo.TXR"), "file from the assets folder");
            // The media folder itself works too.
            t.IsTrue(ps2_xbox_disc::openPath((dir.path / "disc" / "media").string()), "media folder");
            t.IsTrue(ps2_xbox_disc::exists("levels/ZOO/zoo.TXR"), "file from the media folder");
            ps2_xbox_disc::reset();
        });
    });
}
