// ps2xNativeAssets: checks the native asset readers against what the build
// tools make from the same files.
//
//   ps2xNativeAssets --pack <xbox-pack> --ps2 <PS2 disc folder> --xbox <Xbox folder> [--show n]
//
// For every library the pack (tools/xbox_pack.py) covers, the native
// textures of the PS2 file (zge/native/native_textures.h) must be the
// pack's entries: the same keys, pictures byte for byte. Exit 0 if all are.
//
//   ps2xNativeAssets --compare-geo 1 --ps2 <PS2 disc folder> --xbox <Xbox folder> [--show n]
//
// The two releases' geometry side by side (zge/native/geo_file.h): for
// every GEO and ZIT both have, object by object and part by part, whether
// the vertex streams are the same (byte for byte, or the same vertices in
// another order) or the geometry differs (and by how many vertices).
//
//   ps2xNativeAssets --xbox-models <out> --ps2 <PS2 disc folder> --xbox <Xbox folder>
//
// The packaging step's model converter (zge/native/xbox_models.h): every
// skater model (ASSETS/SKATERS) both releases have, the PS2 file with the
// Xbox geometry, to <out>/models/<PS2 path>, for those whose Xbox model has
// more (or fewer) vertices; models left out are listed with the reason.

#include "zge/native/geo_file.h"
#include "zge/native/native_textures.h"
#include "zge/native/xbox_models.h"

#include "ps2_runtime.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_texture_replace.h"
#include "runtime/ps2_xbox_disc.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <cstring>
#include <map>
#include <algorithm>
#include <tuple>
#include <vector>

// ps2_runtime needs a function table; nothing runs from it here.
extern const uint32_t g_ps2RecompiledFunctionTableBase = 0x00000000u;
extern const uint32_t g_ps2RecompiledFunctionTableEnd = 0x00000004u;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount = 1u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

namespace
{
    namespace zn = zge::native;

    // A part's vertices as comparable rows (position, UV, colour), sorted:
    // the same vertices in any order compare equal.
    std::vector<std::vector<uint32_t>> vertexRows(const zn::GeoFilePart &p)
    {
        std::vector<std::vector<uint32_t>> rows(p.vertexCount);
        for (uint32_t v = 0; v < p.vertexCount; ++v)
        {
            auto &r = rows[v];
            for (int c = 0; c < 3; ++c)
            {
                uint32_t bits = 0;
                if (p.positions.size() >= (v + 1) * 3u)
                    std::memcpy(&bits, &p.positions[v * 3u + c], 4);
                r.push_back(bits);
            }
            for (int c = 0; c < 2; ++c)
                r.push_back(p.uvs.size() >= (v + 1) * 2u ? static_cast<uint16_t>(p.uvs[v * 2u + c]) : 0u);
            uint32_t colour = 0;
            if (p.colours.size() >= (v + 1) * 4u)
                std::memcpy(&colour, &p.colours[v * 4u], 4);
            r.push_back(colour);
        }
        std::sort(rows.begin(), rows.end());
        return rows;
    }

    // One stream's values per vertex, sorted (order-free comparison).
    template <typename T>
    std::vector<std::vector<T>> sortedRows(const std::vector<T> &values, size_t perVertex, size_t vertices)
    {
        std::vector<std::vector<T>> rows;
        if (values.size() < perVertex * vertices)
            return rows;
        for (size_t v = 0; v < vertices; ++v)
            rows.emplace_back(values.begin() + v * perVertex, values.begin() + (v + 1) * perVertex);
        std::sort(rows.begin(), rows.end());
        return rows;
    }

    // What differs between two parts of the same vertex count.
    std::string whatDiffers(const zn::GeoFilePart &a, const zn::GeoFilePart &b)
    {
        std::string out;
        const size_t n = a.vertexCount;
        auto positionBits = [](const zn::GeoFilePart &p)
        {
            std::vector<uint32_t> bits(p.positions.size());
            std::memcpy(bits.data(), p.positions.data(), bits.size() * 4);
            return bits;
        };
        if (sortedRows(positionBits(a), 3, n) != sortedRows(positionBits(b), 3, n))
            out += " positions";
        if (sortedRows(a.uvs, 2, n) != sortedRows(b.uvs, 2, n))
            out += " uvs";
        if (sortedRows(a.colours, 4, n) != sortedRows(b.colours, 4, n))
        {
            std::vector<uint8_t> swapped = b.colours;
            for (size_t i = 0; i + 3 < swapped.size(); i += 4)
                std::swap(swapped[i], swapped[i + 2]);
            out += sortedRows(a.colours, 4, n) == sortedRows(swapped, 4, n) ? " colours(BGRA)" : " colours";
        }
        if (a.hasNormals != b.hasNormals || sortedRows(a.normalBytes, a.normalStride, n) !=
                                                 sortedRows(b.normalBytes, b.normalStride ? b.normalStride : 1, n))
            out += " normals";
        return out.empty() ? " (vertex rows pair differently)" : out;
    }

    enum class Verdict
    {
        Same,       // the vertex streams byte for byte
        Reordered,  // the same vertices, another order
        Different,  // other vertices
    };

    Verdict comparePart(const zn::GeoFilePart &a, const zn::GeoFilePart &b)
    {
        if (a.vertexCount == b.vertexCount && a.positions == b.positions && a.uvs == b.uvs && a.colours == b.colours)
            return Verdict::Same;
        if (a.vertexCount == b.vertexCount && vertexRows(a) == vertexRows(b))
            return Verdict::Reordered;
        return Verdict::Different;
    }

    int compareGeometry(const std::string &ps2Root, int show)
    {
        namespace fs = std::filesystem;
        size_t files = 0, onlyPs2 = 0, filesSame = 0, filesReordered = 0, filesDifferent = 0;
        size_t parts = 0, partsSame = 0, partsReordered = 0, partsDifferent = 0, objectsMissing = 0;
        long verticesPs2 = 0, verticesXbox = 0; // of the parts that differ
        std::map<std::string, size_t> differs;
        int shown = 0;
        const fs::path assets = fs::path(ps2Root) / "ASSETS";
        for (const auto &e : fs::recursive_directory_iterator(assets))
        {
            if (!e.is_regular_file())
                continue;
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
            if (ext != ".GEO" && ext != ".ZIT")
                continue;
            const std::string relative = fs::relative(e.path(), assets).generic_string();
            std::vector<uint8_t> ps2Bytes, xboxBytes;
            {
                std::ifstream in(e.path(), std::ios::binary);
                ps2Bytes.assign(std::istreambuf_iterator<char>(in), {});
            }
            if (!ps2_xbox_disc::readFile(relative, xboxBytes))
            {
                ++onlyPs2;
                continue;
            }
            ++files;
            const auto ps2 = zn::readGeoFile(ps2Bytes), xbox = zn::readGeoFile(xboxBytes);
            Verdict worst = Verdict::Same;
            std::string first;
            for (size_t o = 0; o < ps2.size(); ++o)
            {
                // The Xbox object of the same name (the first not yet taken).
                const zn::GeoFileObject *x = nullptr;
                if (o < xbox.size() && xbox[o].name == ps2[o].name)
                    x = &xbox[o];
                else
                    for (const auto &candidate : xbox)
                        if (candidate.name == ps2[o].name)
                        {
                            x = &candidate;
                            break;
                        }
                if (!x || x->parts.size() != ps2[o].parts.size())
                {
                    ++objectsMissing;
                    worst = Verdict::Different;
                    if (first.empty())
                        first = "object " + ps2[o].name + (x ? ": part count" : ": not in the Xbox file");
                    continue;
                }
                for (size_t k = 0; k < x->parts.size(); ++k)
                {
                    ++parts;
                    const Verdict v = comparePart(ps2[o].parts[k], x->parts[k]);
                    if (v == Verdict::Same)
                        ++partsSame;
                    else if (v == Verdict::Reordered)
                        ++partsReordered;
                    else
                    {
                        ++partsDifferent;
                        if (ps2[o].parts[k].vertexCount == x->parts[k].vertexCount)
                            ++differs[whatDiffers(ps2[o].parts[k], x->parts[k])];
                        else
                            ++differs[" vertex count"];
                        verticesPs2 += ps2[o].parts[k].vertexCount;
                        verticesXbox += x->parts[k].vertexCount;
                        if (first.empty())
                            first = "object " + ps2[o].name + " part " + std::to_string(k) + ": " +
                                    std::to_string(ps2[o].parts[k].vertexCount) + " vertices on the PS2, " +
                                    std::to_string(x->parts[k].vertexCount) + " on the Xbox";
                    }
                    worst = std::max(worst, v);
                }
            }
            if (xbox.size() != ps2.size() && first.empty())
                first = std::to_string(ps2.size()) + " objects on the PS2, " + std::to_string(xbox.size()) + " on the Xbox";
            (worst == Verdict::Same ? filesSame : worst == Verdict::Reordered ? filesReordered : filesDifferent)++;
            if (std::getenv("PS2X_COMPARE_TOTALS"))
            {
                long a = 0, b = 0;
                for (const auto &o : ps2)
                    for (const auto &q : o.parts)
                        a += q.vertexCount;
                for (const auto &o : xbox)
                    for (const auto &q : o.parts)
                        b += q.vertexCount;
                if (a != b)
                    std::cout << "totals " << relative << ": " << a << " vertices on the PS2, " << b << " on the Xbox ("
                              << ps2.size() << " / " << xbox.size() << " objects)" << std::endl;
            }
            if (worst != Verdict::Same && shown++ < show)
                std::cout << relative << ": " << (worst == Verdict::Reordered ? "reordered" : "different") << " ("
                          << first << ")" << std::endl;
        }
        std::cout << "geometry: " << files << " files both releases have (" << onlyPs2 << " only on the PS2): "
                  << filesSame << " the same vertex streams, " << filesReordered << " the same vertices reordered, "
                  << filesDifferent << " different" << std::endl;
        std::cout << "parts: " << parts << ": " << partsSame << " the same, " << partsReordered << " reordered, "
                  << partsDifferent << " different (" << verticesPs2 << " vertices on the PS2, " << verticesXbox
                  << " on the Xbox); " << objectsMissing << " objects not matched by name and parts" << std::endl;
        for (const auto &[what, count] : differs)
            std::cout << "  differ in" << what << ": " << count << " parts" << std::endl;
        return 0;
    }
}

namespace
{
    int makeXboxModels(const std::string &ps2Root, const std::string &outRoot)
    {
        namespace fs = std::filesystem;
        const fs::path assets = fs::path(ps2Root) / "ASSETS";
        size_t made = 0, left = 0, same = 0;
        for (const auto &e : fs::recursive_directory_iterator(assets / "SKATERS"))
        {
            std::string ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
            if (!e.is_regular_file() || ext != ".ZIT")
                continue;
            const std::string relative = fs::relative(e.path(), assets).generic_string();
            std::vector<uint8_t> ps2, xbox, out;
            {
                std::ifstream in(e.path(), std::ios::binary);
                ps2.assign(std::istreambuf_iterator<char>(in), {});
            }
            if (!ps2_xbox_disc::readFile(relative, xbox))
                continue;
            zge::native::XboxModelReport report;
            if (!zge::native::makeXboxModel(relative, ps2, xbox, out, report))
            {
                ++left;
                std::cout << relative << ": left out (" << report.why << ")" << std::endl;
                continue;
            }
            if (report.xboxVertices == report.ps2Vertices)
            {
                // The same geometry (vertices in the Xbox exporter's order):
                // nothing to gain.
                ++same;
                continue;
            }
            const fs::path target = fs::path(outRoot) / "models" / ps2x::game_files::normalizePath("ASSETS/" + relative);
            fs::create_directories(target.parent_path());
            std::ofstream(target, std::ios::binary).write(reinterpret_cast<const char *>(out.data()),
                                                           static_cast<std::streamsize>(out.size()));
            ++made;
            std::cout << relative << ": " << report.ps2Vertices << " -> " << report.xboxVertices << " vertices, "
                      << report.remapped << " of " << report.materials << " material textures renamed, largest packet "
                      << report.largestPacket << " bytes (" << report.packetBefore << " as converted, "
                      << report.ps2Packet << " the PS2 model's), "
                      << report.largestEntries << " strip entries (" << report.entriesBefore << "), "
                      << report.triangles << " triangles, the same" << std::endl;
            for (const std::string &kept : report.ps2Pictures)
                std::cout << relative << ": PS2 picture kept for " << kept << std::endl;
        }
        std::cout << "xbox models: " << made << " made, " << same << " the same geometry on both, " << left
                  << " left out" << std::endl;
        return 0;
    }
}

int main(int argc, char **argv)
{
    namespace fs = std::filesystem;
    namespace tr = ps2x::texture_replace;
    std::string pack, ps2Root, xboxRoot;
    int show = 5;
    bool compareGeo = false;
    std::string modelsOut;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const std::string a = argv[i];
        if (a == "--pack")
            pack = argv[i + 1];
        else if (a == "--ps2")
            ps2Root = argv[i + 1];
        else if (a == "--xbox")
            xboxRoot = argv[i + 1];
        else if (a == "--show")
            show = std::atoi(argv[i + 1]);
        else if (a == "--compare-geo")
            compareGeo = std::atoi(argv[i + 1]) != 0;
        else if (a == "--xbox-models")
            modelsOut = argv[i + 1];
    }
    if (!modelsOut.empty())
    {
        if (ps2Root.empty() || xboxRoot.empty() || !ps2_xbox_disc::openPath(xboxRoot))
        {
            std::cerr << "usage: ps2xNativeAssets --xbox-models <out> --ps2 <PS2 disc folder> --xbox <Xbox folder>"
                      << std::endl;
            return 2;
        }
        return makeXboxModels(ps2Root, modelsOut);
    }
    if (compareGeo)
    {
        if (ps2Root.empty() || xboxRoot.empty() || !ps2_xbox_disc::openPath(xboxRoot))
        {
            std::cerr << "usage: ps2xNativeAssets --compare-geo 1 --ps2 <PS2 disc folder> --xbox <Xbox folder>"
                      << std::endl;
            return 2;
        }
        return compareGeometry(ps2Root, show);
    }
    if (pack.empty() || ps2Root.empty() || xboxRoot.empty() || !ps2_xbox_disc::openPath(xboxRoot))
    {
        std::cerr << "usage: ps2xNativeAssets --pack <xbox-pack> --ps2 <PS2 disc folder> --xbox <Xbox folder>"
                  << std::endl;
        return 2;
    }
    ps2x::game_files::GameFileRoots roots;
    roots.disc = ps2Root;
    ps2x::game_files::GameFileSystem files(roots);

    size_t libraries = 0, same = 0, packEntries = 0, nativeEntries = 0;
    int shown = 0;
    const fs::path textures = fs::path(pack) / "textures";
    for (const auto &e : fs::recursive_directory_iterator(textures))
    {
        if (!e.is_regular_file() || e.path().extension() != ".xtp")
            continue;
        std::string normalized = fs::relative(e.path(), textures).generic_string();
        normalized.resize(normalized.size() - 4);
        ++libraries;
        std::ifstream in(e.path(), std::ios::binary);
        const std::vector<uint8_t> xtp((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::vector<tr::Entry> expected;
        tr::parsePack(xtp, expected);
        std::vector<uint8_t> ps2, xbox;
        const auto file = files.open(normalized);
        const std::string relative = normalized.substr(std::string("ASSETS/").size());
        if (!file || !ps2x::game_files::readAll(*file, ps2) || !ps2_xbox_disc::readFile(relative, xbox))
        {
            std::cout << normalized << ": cannot read both files" << std::endl;
            continue;
        }
        const zge::native::FileTextures native = zge::native::texturesForFile(relative, ps2, xbox);
        packEntries += expected.size();
        nativeEntries += native.entries.size();
        std::map<std::tuple<uint32_t, uint32_t, uint64_t>, const tr::Entry *> byKey;
        for (const tr::Entry &n : native.entries)
            byKey[{n.width, n.height, n.indexHash}] = &n;
        std::string problem;
        for (const tr::Entry &x : expected)
        {
            const auto it = byKey.find({x.width, x.height, x.indexHash});
            if (it == byKey.end())
            {
                problem = "missing " + (x.picture ? x.picture->name : std::string("?"));
                break;
            }
            const tr::Picture &a = *x.picture, &b = *it->second->picture;
            if (a.width != b.width || a.height != b.height || a.mips != b.mips || a.data != b.data)
            {
                problem = "picture differs for " + a.name;
                break;
            }
        }
        if (problem.empty() && expected.size() != native.entries.size())
            problem = std::to_string(native.entries.size()) + " entries, the pack has " + std::to_string(expected.size());
        if (problem.empty())
            ++same;
        else if (shown++ < show)
            std::cout << normalized << ": " << problem << std::endl;
    }
    std::cout << same << " of " << libraries << " libraries identical to the pack (" << nativeEntries
              << " native entries, " << packEntries << " in the pack)" << std::endl;
    return same == libraries ? 0 : 1;
}
