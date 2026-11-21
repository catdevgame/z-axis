// What the front end shows of each game, from its disc (collection/front_art.h).
#include "collection/front_art.h"
#include "collection/game_order.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace zx::collection
{
    namespace
    {
        uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24; }

        bool tagIs(const uint8_t *p, const char *tag) { return std::memcmp(p, tag, 4) == 0; }

        bool printableTag(const uint8_t *p)
        {
            for (int i = 0; i < 4; ++i)
                if (p[i] < 0x20 || p[i] > 0x7E)
                    return false;
            return true;
        }

        bool readFile(const std::filesystem::path &path, std::vector<uint8_t> &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }

        bool isFile(const std::filesystem::path &path)
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(path, ec);
        }

        // A TXRC entry (zge/native/texture_library.h has the format): its
        // TXRH name at +0x14, then its picture, a BMP in a TBMP (opaque) or
        // TZMP (with alpha) chunk; smaller mip levels may follow.
        bool readEntry(const uint8_t *d, size_t begin, size_t end, std::string_view name, Image &out)
        {
            bool named = false;
            for (size_t p = begin; p + 8 <= end;)
            {
                const uint32_t size = u32(d + p + 4);
                const size_t body = p + 8;
                if (size > end - body)
                    return false;
                if (tagIs(d + p, "TXRH"))
                {
                    if (size <= 0x14)
                        return false;
                    const char *text = reinterpret_cast<const char *>(d + body + 0x14);
                    named = std::string_view(text, strnlen(text, size - 0x14)) == name;
                    if (!named)
                        return false;
                }
                else if (named && (tagIs(d + p, "TBMP") || tagIs(d + p, "TZMP")))
                {
                    const bool opaque = tagIs(d + p, "TBMP");
                    if (!readBmpImage(std::vector<uint8_t>(d + body, d + body + size), out))
                        return false;
                    uint32_t most = 0;
                    for (const uint32_t pixel : out.pixels)
                        most = std::max(most, pixel >> 24);
                    // The PS2's alpha: 0x80 is opaque.
                    for (uint32_t &pixel : out.pixels)
                    {
                        const uint32_t alpha = opaque ? 0xFFu : most <= 0x80u ? std::min(0xFFu, (pixel >> 24) * 2u) : pixel >> 24;
                        pixel = (pixel & 0x00FFFFFFu) | alpha << 24;
                    }
                    return true;
                }
                p = body + size;
            }
            return false;
        }

        // The same walk as zge::native::readTextureLibrary's: TXRC entries in
        // FORMs and the model, UI and list containers.
        bool find(const uint8_t *d, size_t begin, size_t end, std::string_view name, Image &out, int depth)
        {
            if (depth > 16)
                return false;
            static const char *const kContainers[] = {"ULIB", "UAST", "UMTL", "UFNT", "LIST", "CAT ", "FMDL", "MODL"};
            for (size_t p = begin; p + 8 <= end;)
            {
                const uint32_t size = u32(d + p + 4);
                const size_t body = p + 8;
                if (size > end - body || !printableTag(d + p))
                    return false;
                if (tagIs(d + p, "TXRC"))
                {
                    if (readEntry(d, body, body + size, name, out))
                        return true;
                }
                else if (tagIs(d + p, "FORM"))
                {
                    const size_t inner = size >= 4 && tagIs(d + body, "TXRC") ? body : body + 4;
                    if (inner <= body + size && find(d, inner, body + size, name, out, depth + 1))
                        return true;
                }
                else
                    for (const char *container : kContainers)
                        if (tagIs(d + p, container))
                        {
                            if (find(d, body, body + size, name, out, depth + 1))
                                return true;
                            break;
                        }
                p = body + size;
            }
            return false;
        }

        // The pictures of a library by name, in the order asked; the ones it
        // lacks go to `missing`.
        std::vector<Image> pictures(const std::filesystem::path &file, const std::vector<const char *> &names,
                                    std::vector<std::string> *missing)
        {
            std::vector<Image> out;
            std::vector<uint8_t> bytes;
            if (!readFile(file, bytes))
            {
                if (missing)
                    missing->push_back(file.filename().string());
                return out;
            }
            for (const char *name : names)
            {
                Image picture;
                if (readLibraryPicture(bytes, name, picture))
                    out.push_back(std::move(picture));
                else if (missing)
                    missing->push_back(file.filename().string() + " " + name);
            }
            return out;
        }

        // A game's own files the front end reads, below its disc folder.
        struct GameFiles
        {
            const char *game;
            const char *library;              // the menu library the cut-outs are in
            std::vector<const char *> riders; // its cut-outs, in the roster's order
            const char *loop;                 // the menu's background movie ("" none)
            const char *still;                // the menu's background picture when there is no loop
            MovieClip intro;                  // below the disc folder
            Rect stillPart;                   // of still: what the menu shows of it (empty: all of it)
        };

        const GameFiles *gameFiles(std::string_view game)
        {
            // AI's ten pros (its secret skaters left out); the logo shots end
            // where the movies cut away (AI 5.53 s to its office skit, DM2
            // 3.33 s after "ROCK." to the staff's faces).
            static const GameFiles kGames[] = {
                {"ai", "ASSETS/UILIB.ZAL",
                 {"cha_taig", "cha_jaren", "cha_frankie", "cha_chris", "cha_eito", "cha_matts", "cha_mattl", "cha_sam",
                  "cha_shane", "cha_sven"},
                 "ASSETS/MOVIES/BACKGRND.PSM", "", {"ASSETS/MOVIES/Z-AXIS.PSS", 0.0, 5.5}},
                {"dm2", "", {}, "ASSETS/MOVIE/UIMOVIE.PSM", "", {"ASSETS/MOVIE/ZINTRO.PSS", 0.0, 3.3}},
                // bg_MainMenu (512 x 256) has its menu's title, "MAIN MENU",
                // drawn in at the top right (its letters and their shadow
                // x 302-432, rows 18-34): the menu shows the rows below it.
                {"bmx", "ASSETS/UILIB.ZAL", {"rider_Dave", "rider_Mika"}, "", "bg_MainMenu", {}, {0, 35, 512, 221}},
            };
            for (const GameFiles &files : kGames)
                if (game == files.game)
                    return &files;
            return nullptr;
        }

        // BMX XXX's Xbox extras (the releases folder tools/bmx_releases.py
        // makes): the Xbox's level previews as PSM movies, and its Z-Axis
        // logo movie (zaxis.xmv as a PSS).
        constexpr const char *kBmxPreviewFolder = "movies/ASSETS/MOVIES/MENU";
        constexpr const char *kBmxPreviews[] = {"BRONX.PSM", "DAM.PSM", "LASVEGAS.PSM", "NASA.PSM",
                                                "RAMPAGE.PSM", "SHEEP.PSM", "SYRACUSE.PSM", "UGP.PSM"};
        constexpr const char *kBmxLogoMovie = "movies/ASSETS/MOVIES/ZAXIS.PSS";

        bool readArt(const ArtSource &art, const std::filesystem::path &disc, Image &out)
        {
            std::vector<uint8_t> bytes;
            if (!readFile(disc / art.file, bytes))
                return false;
            if (art.kind == ArtSource::LibraryImage)
            {
                if (!readLibraryPicture(bytes, art.texture, out))
                    return false;
                for (uint32_t &pixel : out.pixels) // a title picture is opaque
                    pixel |= 0xFF000000u;
                return true;
            }
            if (!readRawImage(bytes, art.width, art.height, art.bits, out))
                return false;
            if (art.kind == ArtSource::RawField)
                out = doubleRows(out);
            return true;
        }
    }

    uint32_t gameColour(std::string_view game)
    {
        if (game == "ai")
            return rgba(90, 160, 230);
        if (game == "dm2")
            return rgba(240, 160, 40);
        if (game == "bmx")
            return rgba(210, 40, 45);
        return rgba(220, 220, 220);
    }

    bool readLibraryPicture(const std::vector<uint8_t> &file, std::string_view name, Image &out)
    {
        for (const auto &[offset, length] : archiveFiles(file))
            if (find(file.data(), offset, offset + length, name, out, 0))
                return true;
        return false;
    }

    FrontGame loadFrontGame(std::string_view game, const std::filesystem::path &disc, const std::filesystem::path &extras)
    {
        FrontGame out;
        out.id = std::string(game);
        out.colour = gameColour(game);
        out.installed = !disc.empty() && isFile(disc / "SYSTEM.CNF");
        if (!out.installed)
            return out;

        if (const ArtSource *art = artSource(game))
        {
            if (readArt(*art, disc, out.card))
                out.cardFocus = art->focus;
            else
            {
                out.card = {};
                out.missing.push_back(std::string(art->file) + (art->kind == ArtSource::LibraryImage ? std::string(" ") + art->texture : ""));
            }
        }
        const GameFiles *files = gameFiles(game);
        if (!files)
            return out;
        if (!files->riders.empty())
            out.riders = pictures(disc / files->library, files->riders, &out.missing);
        if (files->loop[0])
        {
            if (isFile(disc / files->loop))
                out.loops.push_back(disc / files->loop);
            else
                out.missing.push_back(files->loop);
        }
        if (!files->intro.empty())
        {
            if (isFile(disc / files->intro.movie))
                out.intro = {disc / files->intro.movie, files->intro.start, files->intro.end};
            else
                out.missing.push_back(files->intro.movie.string());
        }
        if (game == "bmx" && !extras.empty())
        {
            for (const char *preview : kBmxPreviews)
                if (isFile(extras / kBmxPreviewFolder / preview))
                    out.loops.push_back(extras / kBmxPreviewFolder / preview);
            if (isFile(extras / kBmxLogoMovie))
                out.intro = {extras / kBmxLogoMovie, 0.0, 0.0};
        }
        if (out.loops.empty() && files->still[0])
        {
            std::vector<Image> still = pictures(disc / files->library, {files->still}, &out.missing);
            if (!still.empty())
            {
                out.still = std::move(still.front());
                // (the part as measured, when the picture is the one it was measured on)
                const Rect &part = files->stillPart;
                if (part.x >= 0 && part.y >= 0 && part.x + part.width <= out.still.width && part.y + part.height <= out.still.height)
                    out.stillPart = part;
            }
        }
        return out;
    }

    std::vector<FrontGame> loadFrontGames(const std::function<std::filesystem::path(std::string_view)> &discOf,
                                          const std::filesystem::path &extras)
    {
        std::vector<FrontGame> out;
        for (const std::string_view game : kGameOrder)
            out.push_back(loadFrontGame(game, discOf(game), game == "bmx" ? extras : std::filesystem::path()));
        return out;
    }

    std::vector<Image> loadRosterHeads(std::string_view game, const std::filesystem::path &disc)
    {
        if (disc.empty() || !isFile(disc / "SYSTEM.CNF"))
            return {};
        if (game == "ai")
            return pictures(disc / "ASSETS/UILIB.ZAL",
                            {"head_taig", "head_franky", "head_jaren", "head_matt", "head_shane", "head_sven", "head_chris"}, nullptr);
        if (game == "dm2")
        {
            static const std::vector<const char *> kFaces = {"face_0", "face_1", "face_2", "face_3", "face_4", "face_5",
                                                             "face_6", "face_7", "face_8", "face_9", "face_10", "face_11",
                                                             "face_12", "face_13", "face_14", "face_15", "face_16"};
            return pictures(disc / "ASSETS/UILIB.ZAL", kFaces, nullptr);
        }
        return {};
    }

    MovieClip introFor(const std::vector<FrontGame> &games, uint32_t launch)
    {
        std::vector<const MovieClip *> clips;
        for (const FrontGame &game : games)
            if (game.installed && !game.intro.empty())
                clips.push_back(&game.intro);
        if (clips.empty())
            return {};
        return *clips[launch % clips.size()];
    }
}
