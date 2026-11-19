// ps2xMapConvert: the collection's map converter (collection/levels) on the
// command line, for the checks against the tooling's Python converters (the
// reference: tooling/ai/tools/zge_level_gltf.py export, then
// ai_level_convert.py, dm2_level_convert.py or bmx_level_convert.py
// convert). The app converts in memory and writes nothing; this writes the
// same files to a folder so they can be compared byte for byte.
//
//   ps2xMapConvert roundtrip --game ai|dm2|bmx FOLDER...
//       each level folder's GEO, INS, DAT and ZIT (and DM2's TXR) read into
//       the records and written back, compared with the files
//   ps2xMapConvert export --game ai|dm2|bmx FOLDER
//       the level folder's model (collection/levels/export.h) as text: what
//       the tooling's export writes as a .gltf and its reader gives back
//       (freeplay4's dump_model.py writes the Python side's)
//   ps2xMapConvert convert --game ai|dm2|bmx FOLDER --to ai|dm2|bmx --out DIR
//       the level folder exported and converted for the --to game's Custom
//       Freeplay slot, its files written under DIR in a Mods folder's layout,
//       with the time each step took
//
// Never commit what this writes from a disc: it is the disc's data.

#include "collection/levels/convert.h"
#include "collection/levels/export.h"
#include "collection/levels/geometry.h"
#include "collection/levels/scene.h"

#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;
    namespace lv = ps2x::app::levels;

    int usage(const std::string &why)
    {
        std::cerr << "ps2xMapConvert: " << why << "\n"
                  << "usage: ps2xMapConvert roundtrip --game ai|dm2|bmx FOLDER...\n"
                  << "       ps2xMapConvert export --game ai|dm2|bmx FOLDER\n"
                  << "       ps2xMapConvert convert --game ai|dm2|bmx FOLDER --to ai|dm2|bmx --out DIR\n";
        return 2;
    }

    bool readFile(const fs::path &file, lv::Bytes &out)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    std::string upper(std::string text)
    {
        for (char &c : text)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return text;
    }

    // The folder's file named <folder>.<ext>, any case.
    fs::path levelFile(const fs::path &folder, const std::string &ext)
    {
        const std::string want = upper(folder.filename().string()) + "." + ext;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec))
            if (upper(it->path().filename().string()) == want)
                return it->path();
        return {};
    }

    bool engineOf(const std::string &game, lv::Engine &engine)
    {
        if (game == "ai")
            engine = lv::Engine::Zge3;
        else if (game == "dm2")
            engine = lv::Engine::Zge2;
        else if (game == "bmx")
            engine = lv::Engine::Zge4;
        else
            return false;
        return true;
    }

    // One file read and written back: true when byte-identical.
    template <typename Write>
    bool same(const char *what, const fs::path &file, Write write)
    {
        lv::Bytes data;
        if (file.empty() || !readFile(file, data))
        {
            std::cout << "  " << what << ": missing\n";
            return false;
        }
        try
        {
            const lv::Bytes out = write(data);
            if (out == data)
            {
                std::cout << "  " << what << ": " << data.size() << " bytes, byte-identical\n";
                return true;
            }
            size_t at = 0;
            while (at < out.size() && at < data.size() && out[at] == data[at])
                ++at;
            std::cout << "  " << what << ": DIFFERS at byte " << at << " (" << data.size() << " read, " << out.size() << " written)\n";
        }
        catch (const lv::LevelError &e)
        {
            std::cout << "  " << what << ": " << e.what() << "\n";
        }
        return false;
    }

    int roundtrip(lv::Engine engine, const std::vector<std::string> &folders)
    {
        int bad = 0;
        for (const std::string &name : folders)
        {
            const fs::path folder = fs::path(name);
            std::cout << folder.string() << "\n";
            bad += !same("GEO", levelFile(folder, "GEO"), [engine](const lv::Bytes &d) { return lv::Geometry::read(d, engine).write(engine); });
            bad += !same("INS", levelFile(folder, "INS"), [engine](const lv::Bytes &d) { return lv::Instances::read(d, engine).write(engine); });
            bad += !same("DAT", levelFile(folder, "DAT"), [engine](const lv::Bytes &d) { return lv::SceneData::read(d, engine).write(engine); });
            bad += !same("ZIT", levelFile(folder, "ZIT"), [](const lv::Bytes &d) { return lv::SceneFile::read(d).write(); });
            if (engine == lv::Engine::Zge2)
                bad += !same("TXR", levelFile(folder, "TXR"), [](const lv::Bytes &d) { return lv::TextureLibrary::read(d).write(); });
        }
        std::cout << (bad ? "roundtrip: " + std::to_string(bad) + " files differ" : std::string("roundtrip: every file byte-identical")) << std::endl;
        return bad ? 1 : 0;
    }

    // The level folder's <name>.GEO, .INS, .DAT, .TXR (and .ZIT when it has one).
    bool levelFiles(const fs::path &folder, lv::LevelFiles &files)
    {
        for (const char *ext : {"GEO", "INS", "DAT", "TXR", "ZIT"})
        {
            const fs::path file = levelFile(folder, ext);
            if (file.empty())
            {
                if (std::string(ext) == "ZIT")
                    continue;
                std::cerr << "ps2xMapConvert: " << folder.string() << " has no " << ext << std::endl;
                return false;
            }
            if (!readFile(file, files[ext]))
                return false;
        }
        return true;
    }

    std::string hexDouble(double v)
    {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        char text[24];
        std::snprintf(text, sizeof text, "%016" PRIx64, bits);
        return text;
    }

    template <size_t N>
    std::string hexDoubles(const std::array<double, N> &v)
    {
        std::string out;
        for (size_t i = 0; i < N; ++i)
            out += (i ? " " : "") + hexDouble(v[i]);
        return out;
    }

    std::string opt(const std::optional<int> &v) { return v ? std::to_string(*v) : "-"; }

    // The model as dump_model.py writes the Python reader's.
    void dumpModel(const lv::Model &m, std::ostream &out)
    {
        for (size_t i = 0; i < m.nodes.size(); ++i)
        {
            const lv::Node &n = m.nodes[i];
            out << "node " << i << " " << n.name << " mesh=" << opt(n.mesh) << " t=" << hexDoubles(n.translation)
                << " r=" << hexDoubles(n.rotation) << " s=" << hexDoubles(n.scale) << "\n";
        }
        for (size_t i = 0; i < m.meshes.size(); ++i)
        {
            out << "mesh " << i << " " << m.meshes[i].name << "\n";
            for (const lv::Primitive &p : m.meshes[i].primitives)
            {
                auto count = [](const auto &v) { return v ? std::to_string(v->size()) : std::string("-"); };
                out << " prim material=" << opt(p.material) << " positions=" << p.positions.size() << " triangles=" << p.triangles.size()
                    << " lines=" << p.lines.size() << " normals=" << count(p.normals) << " uvs=" << count(p.uvs)
                    << " colours=" << count(p.colours) << "\n";
                for (size_t k = 0; k < p.positions.size(); ++k)
                {
                    out << "  v " << hexDoubles(p.positions[k]);
                    if (p.normals)
                        out << " n " << hexDoubles((*p.normals)[k]);
                    if (p.uvs)
                        out << " u " << hexDoubles((*p.uvs)[k]);
                    if (p.colours)
                        out << " c " << hexDoubles((*p.colours)[k]);
                    out << "\n";
                }
                for (const auto &t : p.triangles)
                    out << "  t " << t[0] << " " << t[1] << " " << t[2] << "\n";
                for (const auto &l : p.lines)
                    out << "  l " << l[0] << " " << l[1] << "\n";
            }
        }
        for (size_t i = 0; i < m.materials.size(); ++i)
            out << "material " << i << " " << m.materials[i].name << " colour=" << hexDoubles(m.materials[i].baseColour)
                << " texture=" << opt(m.materials[i].texture) << "\n";
        for (size_t i = 0; i < m.images.size(); ++i)
        {
            uint64_t v = 0xcbf29ce484222325ull;
            for (const lv::Rgba &px : m.images[i].pixels)
                for (uint8_t b : px)
                {
                    v ^= b;
                    v *= 0x100000001b3ull;
                }
            char hash[24];
            std::snprintf(hash, sizeof hash, "%016" PRIx64, v);
            out << "image " << i << " " << m.images[i].name << " " << m.images[i].width << "x" << m.images[i].height << " " << hash << "\n";
        }
    }

    int exportCommand(lv::Engine engine, const std::vector<std::string> &args)
    {
        if (args.size() != 1)
            return usage("export takes one level folder");
        lv::LevelFiles files;
        if (!levelFiles(args[0], files))
            return 1;
        try
        {
            dumpModel(lv::exportLevel(engine, files), std::cout);
        }
        catch (const lv::LevelError &e)
        {
            std::cerr << "ps2xMapConvert: " << e.what() << std::endl;
            return 1;
        }
        return 0;
    }

    int convertCommand(lv::Engine engine, const std::vector<std::string> &args)
    {
        std::string folder, to, out;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--to" && i + 1 < args.size())
                to = args[++i];
            else if (args[i] == "--out" && i + 1 < args.size())
                out = args[++i];
            else if (folder.empty())
                folder = args[i];
            else
                return usage("convert: what is " + args[i] + "?");
        }
        lv::Engine target{};
        if (folder.empty() || out.empty() || !engineOf(to, target))
            return usage("convert takes a level folder, --to and --out");
        lv::LevelFiles files;
        if (!levelFiles(folder, files))
            return 1;
        try
        {
            using Clock = std::chrono::steady_clock;
            const auto start = Clock::now();
            const lv::Model model = lv::exportLevel(engine, files);
            const auto exported = Clock::now();
            const lv::Converted converted = lv::convertModel(model, target);
            const auto done = Clock::now();
            for (const std::string &line : converted.report)
                std::cout << line << "\n";
            size_t bytes = 0;
            for (const auto &[path, data] : converted.files)
            {
                const fs::path file = fs::path(out) / path;
                fs::create_directories(file.parent_path());
                std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
                bytes += data.size();
            }
            auto ms = [](auto d) { return std::chrono::duration_cast<std::chrono::milliseconds>(d).count(); };
            std::cout << "converted: " << converted.files.size() << " files, " << bytes << " bytes; export " << ms(exported - start)
                      << " ms, convert " << ms(done - exported) << " ms" << std::endl;
        }
        catch (const lv::LevelError &e)
        {
            std::cerr << "ps2xMapConvert: " << e.what() << std::endl;
            return 1;
        }
        return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage("no command");
    const std::string command = argv[1];
    std::string game;
    std::vector<std::string> rest;
    for (int i = 2; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--game" && i + 1 < argc)
            game = argv[++i];
        else
            rest.push_back(arg);
    }
    lv::Engine engine{};
    if (!engineOf(game, engine))
        return usage("--game ai, dm2 or bmx");
    if (command == "roundtrip")
        return roundtrip(engine, rest);
    if (command == "export")
        return exportCommand(engine, rest);
    if (command == "convert")
        return convertCommand(engine, rest);
    return usage("no command " + command);
}
