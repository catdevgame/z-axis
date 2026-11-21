// The Xbox files Aggressive Inline reads as they are (collection/extras/
// ai_xbox_files.h).
#include "collection/extras/ai_xbox_files.h"

namespace zx::app::extras
{
    const std::vector<std::string> &aiXboxFilePatterns()
    {
        static const std::vector<std::string> patterns = {
            // The free skate scripts for the Boardwalk and the Cannery (game
            // override aggressive_inline_freeskate.cpp): doors, rides, the cutter.
            "levels/brdwlk/brdwlkt.bin",
            "levels/cannery/canneryt.bin",
            // The career scripts, the PS2's with its bug fixes (game override
            // aggressive_inline_careerscripts.cpp), and Civic's motions with
            // the window ledges preview its script plays.
            "levels/brdwlk/brdwlk.bin",
            "levels/cannery/cannery.bin",
            "levels/civic/civic.bin",
            "levels/hanger/hanger.bin",
            "levels/indust/indust.bin",
            "levels/movie/movie.bin",
            "levels/museum/museum.bin",
            "levels/civic/motions.zal",
            // Languages (game override aggressive_inline_language.cpp): the
            // string tables, the fonts with accented characters (uilib.zal),
            // the level scripts' texts in each language and the French speech
            // (aggressive_inline_sound.cpp).
            "string.eus",
            "string.fre",
            "string.ger",
            "string.spa",
            "uilib.zal",
            "levels/*/*.bni",
            "levels/*/*.bnf",
            "levels/*/*.bng",
            "levels/*/*.bns",
            "audio/speech01.zsd",
            // The Tutorial's loading screens with the Xbox controller
            // (aggressive_inline_prompts.cpp).
            "loading/tut/tut?.raw",
            // The Zoo's level-select flythrough (aggressive_inline_zoo.cpp).
            "movies/flythrus/zoo.bik",
        };
        return patterns;
    }

    bool makeAiXboxFiles(const SourceFiles &xbox, const std::filesystem::path &out, std::vector<std::string> &log,
                         std::string *error)
    {
        size_t count = 0;
        uint64_t bytes = 0;
        for (const std::string &file : xbox.files())
        {
            bool wanted = false;
            for (const std::string &pattern : aiXboxFilePatterns())
                wanted = wanted || globMatches(lowerAscii(file), pattern);
            if (!wanted)
                continue;
            Bytes data;
            if (!xbox.read(file, data))
            {
                *error = "Could not read " + file + " from the Xbox disc.";
                return false;
            }
            if (!writeFile(out / "assets" / std::filesystem::path(std::u8string(file.begin(), file.end())), data, error))
                return false;
            ++count;
            bytes += data.size();
        }
        log.push_back("[xbox] bundled " + std::to_string(count) + " Xbox files (" + std::to_string(bytes >> 20) +
                      " MB)");
        return true;
    }
}
