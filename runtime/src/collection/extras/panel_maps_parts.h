#pragma once

// What the panel file makers (collection/extras/panel_maps.h) share: the
// readers of the games' files, the map picture, the goals' records and the
// UI library's fonts. The reference is the tooling's panel_map.py, which
// computes in Python's doubles and integers: these keep its operations and
// their order (no fused multiply-adds, Python's rounding and floor division,
// its compensated sum), so the files come out as the same bytes.

#include "collection/extras/panel_maps.h"
#include "dm2_files.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ps2x::app::extras::panel_maps
{
    // A file that does not read as the maker expects, said in plain words.
    class PanelError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    // ---- the reference's arithmetic -----------------------------------------

    // Python's // and % on integers: rounded toward minus infinity.
    inline int64_t floorDiv(int64_t a, int64_t b)
    {
        const int64_t q = a / b;
        return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
    }
    inline int64_t floorMod(int64_t a, int64_t b) { return a - floorDiv(a, b) * b; }
    // int() of a float: toward zero.
    inline int64_t truncated(double x)
    {
        if (!(x > -9.0e18 && x < 9.0e18))
            throw PanelError("a coordinate out of range");
        return int64_t(x);
    }
    // a // b of floats where b is a power of two (the division is exact).
    inline int64_t floorByPowerOfTwo(double a, double b) { return truncated(std::floor(a / b)); }
    // max(a, b) and min(a, b) as Python picks them: the first unless the
    // second is greater (smaller).
    inline double pyMax(double a, double b) { return b > a ? b : a; }
    inline double pyMin(double a, double b) { return b < a ? b : a; }
    // sum() of floats: CPython's (3.12 on) compensated sum (Neumaier's),
    // started from the integer 0.
    double pythonSum(const double *values, size_t count);
    inline double pythonMean(const std::vector<double> &values)
    {
        return pythonSum(values.data(), values.size()) / double(values.size());
    }

    // ---- bytes ----------------------------------------------------------------

    // Bytes held elsewhere (a file read whole, a chunk inside it).
    struct ByteView
    {
        const uint8_t *data = nullptr;
        size_t size = 0;

        ByteView() = default;
        ByteView(const uint8_t *d, size_t n) : data(d), size(n) {}
        ByteView(const Bytes &b) : data(b.data()), size(b.size()) {}

        bool empty() const { return size == 0; }
        uint8_t operator[](size_t at) const
        {
            if (at >= size)
                throw PanelError("a field past the end of a record");
            return data[at];
        }
        uint16_t u16(size_t at) const;
        uint32_t u32(size_t at) const;
        int32_t s32(size_t at) const { return int32_t(u32(at)); }
        // A float field widened to a double.
        double f32(size_t at) const;
        // [from, to) clipped to the bytes, as a Python slice of them.
        ByteView slice(size_t from, size_t to = SIZE_MAX) const;
        // The bytes from `from` to the first zero (data[from:].split(b"\0")[0]).
        std::string text(size_t from, size_t to = SIZE_MAX) const;
        // The bytes from `at` to the first zero, which must be there (bytes.index).
        std::string zeroEnded(size_t at) const;
        Bytes bytes() const { return Bytes(data, data + size); }
        std::string str() const { return std::string(reinterpret_cast<const char *>(data), size); }
        bool operator==(const ByteView &o) const { return size == o.size && (size == 0 || std::memcmp(data, o.data, size) == 0); }
        bool startsWith(std::string_view prefix) const
        {
            return size >= prefix.size() && std::memcmp(data, prefix.data(), prefix.size()) == 0;
        }
    };

    // Little-endian fields appended as struct.pack writes them; a value the
    // field cannot hold is an error, as it is to struct.pack.
    void putU8(Bytes &out, int64_t v);
    void putU16(Bytes &out, int64_t v);
    void putU32(Bytes &out, int64_t v);
    void putS32(Bytes &out, int64_t v);
    void putF32(Bytes &out, double v); // a double narrowed to a float
    void putBytes(Bytes &out, const Bytes &more);
    void putText(Bytes &out, std::string_view text);
    // text cut to `keep` bytes, zeros to `size` (text[:keep].ljust(size, b"\0")).
    void putPadded(Bytes &out, std::string_view text, size_t keep, size_t size);
    // A file chunk: the tag, the size, the bytes, zeros to 4 bytes.
    Bytes chunk(std::string_view tag, const Bytes &body);

    // ---- chunk trees ------------------------------------------------------------

    // A chunk of a file read whole; its bytes stay the file's.
    struct Chunk
    {
        std::string tag;
        ByteView data;
        std::vector<Chunk> kids;
        bool container = false;

        // The first child with that tag, or null.
        const Chunk *first(std::string_view t) const;
    };
    using Chunks = std::vector<Chunk>;
    // The panel's chunk trees: ai_level.py's containers and the UI
    // library's (ULIB, UAST, UMTL, UFNT), as panel_map.py reads every file
    // it opens with ai_level. Throws PanelError on a chunk past its parent
    // or bytes left over.
    Chunks parseChunks(ByteView data);
    // Every chunk, each before its children.
    void walkChunks(const Chunks &chunks, const std::function<void(const Chunk &)> &visit);

    // ---- the games' executables, archives and string tables ----------------------

    // The game's executable, read by its EE addresses (its loaded segments).
    class Elf
    {
    public:
        explicit Elf(Bytes data);
        uint32_t word(uint32_t address) const;
        // The zero-ended text at an address.
        std::string text(uint32_t address) const;

    private:
        struct Segment
        {
            uint32_t address, offset, size;
        };
        size_t fileOffset(uint32_t address) const;
        Bytes m_data;
        std::vector<Segment> m_segments;
    };

    // A ZDIR archive's (.ZAL) entries.
    std::vector<ByteView> zdirEntries(ByteView archive);
    // {name hash: entry} of a ZDIR archive, by its GRID chunk's hashes.
    std::unordered_map<uint32_t, ByteView> zdirNamed(ByteView archive);
    // DM2's (and BMX XXX's) string tables: {key hash: text}.
    std::unordered_map<uint32_t, std::string> dm2Strings(ByteView table);

    // Reads a file of the disc (any case); missing: empty.
    Bytes readFile(const SourceFiles &files, std::string_view path);
    // Reads a file the maker cannot do without (any case).
    Bytes needFile(const SourceFiles &files, std::string_view path);
    // str.upper() of Latin-1 text (\xdf is "SS"); a letter whose capital is
    // no Latin-1 one cannot be written, as the reference cannot.
    std::string pythonUpper(std::string_view text);

    // ---- the map ------------------------------------------------------------------

    using Vec3 = std::array<double, 3>;
    using Rgb = std::array<double, 3>;

    // A face the map can show, in world space (+y down).
    struct MapFace
    {
        double top = 0;  // the smallest y of its corners
        Vec3 a{}, b{}, c{};
        Vec3 normal{};
        // One colour for the face, or one for each corner.
        bool cornerColours = false;
        std::array<Rgb, 3> colour{};
        bool water = false;
    };
    // A place the level puts the rider or a thing for it: world x, z and,
    // where known, the height it sits at.
    struct Seed
    {
        double x = 0, z = 0;
        std::optional<double> y;
    };
    struct Box
    {
        double x0 = 0, x1 = 0, z0 = 0, z1 = 0;
    };
    // The box of the level proper: the connected cells of faces holding the
    // seeds (none held: the cell with the densest neighbourhood).
    Box levelBox(const std::vector<MapFace> &faces, const std::vector<Seed> &seeds);
    // The pixels a world unit: at most three, at most 2400 on a side.
    double pixelsPerUnit(const Box &box);

    // World to picture: top down with +x right and +z up, or, for a level
    // wider than tall, a quarter turn clockwise so its long side lies along
    // the tall pane (panel_view.h MapTransform).
    struct Frame
    {
        Frame(const Box &box, double ppu);
        std::pair<double, double> point(double x, double z) const;
        double worldLeft() const { return box.x0; }
        double worldTop() const { return turned ? box.z0 : box.z1; }

        Box box;
        double ppu;
        bool turned;
        int64_t width, height;
    };

    // The picture: RGB rows, and a byte of height a pixel (0 no ground,
    // kWaterCode water, else 2 .. 253 from low to high).
    struct MapPicture
    {
        int64_t width = 0, height = 0;
        Bytes rgb;
        Bytes heights;
        double codesPerUnit = 0; // of the height bytes
        double highest = 0;      // the world y of the top height
        uint8_t *pixel(int64_t x, int64_t y) { return rgb.data() + 3 * size_t(y * width + x); }
    };
    MapPicture rasterise(const std::vector<MapFace> &faces, const Frame &frame);
    // The cells the skater can roll to from the seeds, and what it cannot
    // reach dimmed (the walls in its way: `barriers`, triangles). The share
    // of the ground kept.
    double muteOutOfPlay(MapPicture &picture, const Frame &frame, const std::vector<Seed> &seeds,
                         const std::vector<std::array<Vec3, 3>> &barriers);
    // Thick walls (short empty runs between ground) drawn as walls.
    void fillGaps(MapPicture &picture, double ppu);
    // Height steps as ink.
    void outline(MapPicture &picture);
    // Grind rails ([a, b] in world space) drawn over the picture.
    void drawRails(MapPicture &picture, const Frame &frame, const std::vector<std::array<Vec3, 2>> &rails);
    // <LEVEL>.pmap: the name, the picture with its transform, and the goals' chunks.
    Bytes levelFile(std::string_view title, const Frame &frame, const MapPicture &picture, const Bytes &goals);

    // ---- goals ------------------------------------------------------------------

    // TargetRecord.role (panel.h TargetRole): what the rider does at a target.
    constexpr uint32_t kTargetPickup = 0, kTargetDropOff = 1, kTargetPlace = 2;

    // TEXT: the goals' strings, zero-ended, after a first empty one.
    struct TextTable
    {
        Bytes bytes{0};
        uint32_t add(std::string_view text);
    };

    // A GoalRecord (panel_file.h) as the makers fill it.
    struct GoalFields
    {
        uint32_t title = 0, text = 0, owner = 0;
        int64_t counterWord = -1, counterTarget = 0;
        int64_t firstTarget = 0, targetCount = 0;
        int64_t type = 0, tier = 0, locked = 0, points = 0;
        double anchorX = std::numeric_limits<double>::quiet_NaN();
        double anchorZ = std::numeric_limits<double>::quiet_NaN();
        int64_t scoreKind = 0, scoreTarget = 0;
        uint32_t status = 0;
        int64_t targetWord = -1;
        uint32_t activate = 0;
    };
    void putGoal(Bytes &out, const GoalFields &goal);
    void putTarget(Bytes &out, uint32_t name, double x, double z, int64_t doneWord, int64_t doneValue, uint32_t role);
    void putBubble(Bytes &out, uint32_t name, double x, double z, int64_t stateWord, int64_t stateValue);
    // World points joined into places (x, z): a point within 12 units of
    // one of a group's is the group's; each group's mean.
    std::vector<std::pair<double, double>> joinPlaces(const std::vector<Vec3> &points);

    // ---- the levels in DM2's format (Dave Mirra BMX 2, BMX XXX) -------------------

    enum PlaceKind
    {
        PlaceFace,
        PlaceEdge,
        PlaceObject,
        PlaceKinds,
    };
    // What goals name in a level: {name hash: the faces' centres, the
    // edges' ends, the objects' positions}, in the level's order.
    using GoalPlaces = std::unordered_map<uint32_t, std::array<std::vector<Vec3>, PlaceKinds>>;
    GoalPlaces dm2GoalPlaces(const Bytes &geo, const Bytes &ins);
    // A DM2-format level's map file and its log line. `capitals`: the title
    // in capitals, as DM2's menus show level names.
    Bytes dm2FormatLevelFile(const Bytes &geo, const Bytes &ins, const std::string &level, const std::string &title,
                             const Bytes &goals, int goalCount, bool capitals, std::string &logLine);

    // ---- the UI library -------------------------------------------------------------

    // A game picture as RGBA rows, top down.
    struct RgbaPicture
    {
        int64_t width = 0, height = 0;
        Bytes rgba;
    };
    RgbaPicture bmpRgba(ByteView bmp);
    // A TXRC's picture (its TBMP or TZMP).
    ByteView pictureOf(const Chunk &texture);
    // Each UAST of the ULIB entries of a UI library (UILIB.ZAL): its name
    // (HAST +128) and source file (HAST +0).
    void forEachUiAsset(ByteView uilib, const std::function<void(const std::string &name, const std::string &source,
                                                                 const Chunk &asset)> &visit);
    // A ButtonLib font's name: its source file's, without folder or extension.
    std::string sourceStem(const std::string &source);
    // A font of the UI library: its head and pages.
    struct UiFont
    {
        uint32_t lineHeight = 0, glyphs = 0, pages = 0;
        std::vector<RgbaPicture> pictures;
        Bytes glyphRecords; // 12 bytes a glyph
    };
    UiFont readUiFont(const Chunk &font);
    // A FONT chunk: the panel's name, then the font.
    Bytes fontChunk(std::string_view panelName, const UiFont &font);

    // A TRIK row: the family, the unlock bit, the directions, the button glyph, the name.
    Bytes trickRecord(int64_t family, int64_t unlock, const Bytes &inputs, int64_t button, std::string_view name);
    // A TRIK row of a ButtonLib buttons text ("CGE 2": the stick's
    // directions, A-H from up clockwise, ? any, then the button).
    Bytes buttonsTrickRecord(int64_t family, const std::string &buttons, const std::string &name);
    // ButtonLib's direction glyphs as the panel's trick table asks for them.
    uint8_t panelDirectionGlyph(uint8_t buttonLibGlyph);

    // ---- the three games -------------------------------------------------------------

    // Each throws PanelError; `levels` as PanelExtras::levels.
    void makeAggressiveInlineMaps(const SourceFiles &disc, const PanelExtras &extras, const std::filesystem::path &out,
                                  std::vector<std::string> &log);
    void makeDaveMirraMaps(const SourceFiles &disc, const PanelExtras &extras, const std::filesystem::path &out,
                           std::vector<std::string> &log);
    void makeBmxXxxMaps(const SourceFiles &disc, const PanelExtras &extras, const std::filesystem::path &out,
                        std::vector<std::string> &log);

    // Writes a made file; throws PanelError when it cannot.
    void writeMade(const std::filesystem::path &path, const Bytes &data);
    // printf-style text for the log and the errors.
    std::string formatted(const char *pattern, ...);
    bool wanted(const PanelExtras &extras, const std::string &level);
    // Runs `make`, an error in it said as one of `what` ("the level MOVIE").
    template <typename Make>
    void making(const std::string &what, Make make)
    {
        try
        {
            make();
        }
        catch (const std::exception &e)
        {
            throw PanelError(what + ": " + e.what());
        }
    }
}
