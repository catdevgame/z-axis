// The UI library pieces of the panel's UI.pmap (panel_maps_parts.h): the
// game's pictures as RGBA, its fonts, and the trick table's rows.
#include "panel_maps_parts.h"

#include "script_text.h"

namespace zx::app::extras::panel_maps
{
    RgbaPicture bmpRgba(ByteView data)
    {
        // A 4- or 8-bit BMP whose palette has PS2 alpha (0 .. 128; all 0 for an opaque TBMP).
        const uint32_t pixels = data.u32(10), head = data.u32(14);
        const int64_t width = data.s32(18), height = data.s32(22);
        const uint32_t bpp = data.u16(28);
        if (bpp > 16 || width < 0)
            throw PanelError("a picture that is not a 4- or 8-bit BMP");
        uint64_t colours = data.u32(46);
        if (colours == 0)
            colours = uint64_t(1) << bpp;
        const ByteView palette = data.slice(14 + size_t(head), 14 + size_t(head) + size_t(colours) * 4);
        bool opaque = true;
        for (uint64_t i = 0; i < colours && opaque; ++i)
            opaque = palette[size_t(i) * 4 + 3] == 0;
        std::vector<std::array<uint8_t, 4>> table;
        for (uint64_t i = 0; i < (uint64_t(1) << bpp); ++i)
        {
            int r = 0, g = 0, b = 0, a = 0;
            if (i < colours)
            {
                if (size_t(i) * 4 + 4 > palette.size)
                    throw PanelError("a picture's palette past its end");
                b = palette[size_t(i) * 4];
                g = palette[size_t(i) * 4 + 1];
                r = palette[size_t(i) * 4 + 2];
                a = palette[size_t(i) * 4 + 3];
            }
            a = opaque ? 255 : std::min(255, a * 2);
            if (r > 200 && b > 200 && g < 70)
                a = 0; // the art's colour key
            table.push_back({uint8_t(r), uint8_t(g), uint8_t(b), uint8_t(a)});
        }
        const int64_t stride = floorDiv(width * bpp + 31, 32) * 4;
        const int64_t rows = height < 0 ? -height : height;
        RgbaPicture out;
        out.width = width;
        out.height = rows;
        // Rows past the file's end are empty: an 8-bit one adds nothing, a
        // 4-bit one is an error. (They are skipped, not walked: a broken
        // header can say two billion rows.)
        const int64_t present = stride == 0 ? 0
                                : pixels >= data.size ? 0
                                                      : int64_t((data.size - pixels + size_t(stride) - 1) / size_t(stride));
        if (rows > present && bpp != 8 && width > 0)
            throw PanelError("a picture's pixels past its end");
        const int64_t firstY = height > 0 ? std::max<int64_t>(0, rows - present) : 0;
        const int64_t endY = height > 0 ? rows : std::min(rows, present);
        for (int64_t y = firstY; y < endY; ++y)
        {
            const size_t at = size_t(pixels) + size_t((height > 0 ? rows - 1 - y : y) * stride);
            const ByteView line = data.slice(at, at + size_t(stride));
            if (bpp == 8)
            {
                for (size_t x = 0; x < line.size && int64_t(x) < width; ++x)
                    out.rgba.insert(out.rgba.end(), table[line.data[x]].begin(), table[line.data[x]].end());
                continue;
            }
            for (int64_t x = 0; x < width; ++x)
            {
                const uint8_t v = line[size_t(x >> 1)];
                const auto &c = table.at((x & 1) == 0 ? v >> 4 : v & 15);
                out.rgba.insert(out.rgba.end(), c.begin(), c.end());
            }
        }
        return out;
    }

    ByteView pictureOf(const Chunk &texture)
    {
        for (const Chunk &k : texture.kids)
            if (k.tag == "TBMP" || k.tag == "TZMP")
                return k.data;
        throw PanelError("a UI texture without its picture");
    }

    void forEachUiAsset(ByteView uilib, const std::function<void(const std::string &name, const std::string &source,
                                                                 const Chunk &asset)> &visit)
    {
        for (const ByteView &entry : zdirEntries(uilib))
        {
            if (!entry.startsWith("ULIB"))
                continue;
            const Chunks tree = parseChunks(entry);
            if (tree.empty())
                throw PanelError("an empty UI library");
            for (const Chunk &asset : tree[0].kids)
            {
                if (asset.tag != "UAST")
                    continue;
                const Chunk *head = asset.first("HAST");
                if (!head)
                    throw PanelError("a UI asset without its head");
                visit(head->data.text(128, 160), head->data.text(0, 128), asset);
            }
        }
    }

    std::string sourceStem(const std::string &source)
    {
        const size_t slash = source.find_last_of("\\/");
        std::string file = slash == std::string::npos ? source : source.substr(slash + 1);
        const size_t dot = file.rfind('.');
        return dot == std::string::npos ? file : file.substr(0, dot);
    }

    UiFont readUiFont(const Chunk &font)
    {
        const Chunk *head = font.first("HFNT");
        if (!head)
            throw PanelError("a UI font without its head");
        UiFont out;
        out.lineHeight = head->data[0];
        out.glyphs = head->data.u16(2);
        out.pages = head->data.u16(4);
        for (const Chunk &t : font.kids)
            if (t.tag == "TXRC")
                out.pictures.push_back(bmpRgba(pictureOf(t)));
        const size_t glyphsAt = 6 + 40 * size_t(out.pages);
        out.glyphRecords = head->data.slice(glyphsAt, glyphsAt + 12 * size_t(out.glyphs)).bytes();
        return out;
    }

    Bytes fontChunk(std::string_view panelName, const UiFont &font)
    {
        Bytes body;
        putPadded(body, panelName, 31, 32);
        putU32(body, font.lineHeight);
        putU32(body, font.glyphs);
        putU32(body, font.pages);
        for (const RgbaPicture &p : font.pictures)
        {
            putU32(body, p.width);
            putU32(body, p.height);
            putBytes(body, p.rgba);
        }
        putBytes(body, font.glyphRecords);
        return chunk("FONT", body);
    }

    Bytes trickRecord(int64_t family, int64_t unlock, const Bytes &inputs, int64_t button, std::string_view name)
    {
        Bytes out;
        putU8(out, family);
        putU8(out, unlock);
        putU8(out, int64_t(inputs.size()));
        putU8(out, button);
        for (size_t i = 0; i < 4; ++i)
            out.push_back(i < inputs.size() ? inputs[i] : 0);
        putPadded(out, name, 39, 40);
        return out;
    }

    Bytes buttonsTrickRecord(int64_t family, const std::string &buttons, const std::string &name)
    {
        static const std::string kDirections = "ABCDEFGH";
        const std::string text = script_text::strip(buttons);
        const size_t space = text.find(' ');
        std::string stick = text.substr(0, space);
        std::string press = space == std::string::npos ? "" : text.substr(space + 1);
        // str.isdigit: the digits, and Latin-1's superscript two, three and one
        auto digit = [](unsigned char c) { return (c >= '0' && c <= '9') || c == 0xB2 || c == 0xB3 || c == 0xB9; };
        if (!stick.empty() && press.empty() && std::all_of(stick.begin(), stick.end(), [&](char c) { return digit(uint8_t(c)); }))
        {
            press = stick;
            stick.clear();
        }
        Bytes inputs;
        for (char d : stick)
        {
            const size_t at = kDirections.find(d);
            inputs.push_back(at == std::string::npos ? 9 : uint8_t(at + 1)); // 1 up, then clockwise, 9 any
        }
        if (inputs.size() > 4)
            inputs.resize(4);
        return trickRecord(family, 0, inputs, press.empty() ? 0 : uint8_t(press[0]), name);
    }

    uint8_t panelDirectionGlyph(uint8_t glyph)
    {
        // ButtonLib's A-H (up, then clockwise) as AI's fnt_ps2buttons characters
        static const std::string kButtonLib = "ABCDEFGH", kPanel = ",]>'.;<[";
        const size_t at = glyph ? kButtonLib.find(char(glyph)) : std::string::npos;
        return at == std::string::npos ? glyph : uint8_t(kPanel[at]);
    }
}
