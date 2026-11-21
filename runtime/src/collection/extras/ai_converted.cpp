// Aggressive Inline's Zoo, Ape and Estate from the player's discs
// (collection/extras/ai_converted.h).
#include "collection/extras/ai_converted.h"

#include "runtime/ps2_game_files.h"
#include "runtime/ps2_zaxis_ape.h"
#include "runtime/ps2_zaxis_estate.h"
#include "runtime/ps2_zaxis_zoo.h"

#include <sstream>

namespace zx::app::extras
{
    namespace
    {
        // The converters' log lines, kept for the setup's log.
        void keepLines(const std::ostringstream &text, std::vector<std::string> &log)
        {
            std::istringstream lines(text.str());
            for (std::string line; std::getline(lines, line);)
                if (!line.empty())
                    log.push_back(line);
        }
    }

    bool makeAiZooAndApe(const SourceFiles &xbox, const std::filesystem::path &ps2Disc, const std::filesystem::path &out,
                         std::vector<std::string> &log, std::string *error)
    {
        namespace zoo = zx::zaxis::zoo;
        zx::game_files::GameFileRoots roots;
        roots.disc = ps2Disc.string();
        zx::game_files::GameFileSystem disc(roots);
        const zoo::Reader xboxFiles = [&xbox](const std::string &path, std::vector<uint8_t> &data)
        { return xbox.readAny(path, data); };
        const zoo::Reader ps2Files = [&disc](const std::string &path, std::vector<uint8_t> &data)
        {
            std::shared_ptr<zx::game_files::GameFile> file = disc.open(path);
            return file && zx::game_files::readAll(*file, data);
        };
        zoo::Made made;
        std::ostringstream text;
        std::string why;
        const bool ok = zoo::build(xboxFiles, ps2Files, zoo::Options(), made, text, &why) &&
                        zx::zaxis::ape::build(xboxFiles, made, text, &why);
        keepLines(text, log);
        if (!ok)
        {
            *error = "The Zoo and the Ape could not be made from the Xbox disc: " + why;
            return false;
        }
        for (const auto &[path, contents] : made.files)
            if (!writeFile(out / std::filesystem::path(path), contents, error))
                return false;
        return true;
    }

    bool makeAiEstate(const SourceFiles &gc, const std::filesystem::path &out, std::vector<std::string> &log,
                      std::string *error)
    {
        namespace estate = zx::zaxis::estate;
        const estate::Reader files = [&gc](const std::string &name, std::vector<uint8_t> &data)
        { return gc.readAny("levels/TUT/Estate/" + name, data); };
        estate::Made made;
        std::ostringstream text;
        std::string why;
        const bool ok = estate::build(files, made, text, &why);
        keepLines(text, log);
        if (!ok)
        {
            *error = "Estate could not be made from the GameCube disc: " + why;
            return false;
        }
        for (const auto &[name, contents] : made.files)
            if (!writeFile(out / std::filesystem::path(name), contents, error))
                return false;
        return true;
    }
}
