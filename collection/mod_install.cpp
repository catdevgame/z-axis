#include "collection/mod_install.h"

#include "collection/maps.h"
#include "collection/mods.h"

#include <cctype>
#include <system_error>

namespace zx::app
{
    namespace
    {
        // The folder copies are made in before they are moved into place, and
        // the one a copy replaces is kept in until the copy is in its place.
        constexpr const char *kIncomingFolder = ".incoming";
        constexpr const char *kReplacedFolder = ".replaced";

        // A name a folder's entry can have: not empty, not "." or "..".
        bool entryName(const std::filesystem::path &name)
        {
            return !name.empty() && name != "." && name != "..";
        }

        // The path as it names a file or a folder: its "." and ".." parts
        // worked out and no separator at its end (".../park/" names park).
        std::filesystem::path namedPath(const std::filesystem::path &path)
        {
            std::filesystem::path out = path.lexically_normal();
            if (!out.has_filename() && out.has_relative_path())
                out = out.parent_path();
            return out;
        }

        // Something is at `path` (a link too, whatever it points to).
        bool there(const std::filesystem::path &path)
        {
            std::error_code ec;
            return std::filesystem::exists(std::filesystem::symlink_status(path, ec));
        }

        bool isZip(const std::filesystem::path &path)
        {
            std::string extension = path.extension().string();
            for (char &c : extension)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return extension == ".zip";
        }
    }

    DropInstall planDropInstall(const std::filesystem::path &dropped, const std::filesystem::path &supportDir)
    {
        DropInstall plan;
        plan.from = namedPath(dropped);
        if (!entryName(plan.from.filename())) // (a root, or ".." past the start: nothing a folder could hold)
            return plan;
        std::error_code ec;
        const bool folder = std::filesystem::is_directory(plan.from, ec);
        if (!folder && !(std::filesystem::is_regular_file(plan.from, ec) && isZip(plan.from)))
            return plan;
        // (as the collection's Mods folder reads it)
        const Mod mod = sharedModAt(plan.from);
        plan.title = mod.title.empty() ? plan.from.filename().string() : mod.title;
        if (mod.manifest || mod.isMap())
        {
            // A map described by a map.ini alone, in a folder: the maps folder's layout. Every other mod or
            // map: the Mods folder's (zips are read in place only there).
            const bool mapsFolder = mod.isMap() && !mod.manifest && folder &&
                                    !std::filesystem::exists(plan.from / kModManifestFile, ec);
            plan.kind = mapsFolder ? DropInstall::Map : DropInstall::Mod;
            plan.to = supportDir / (mapsFolder ? kMapsFolder : kModsFolder) / plan.from.filename();
            plan.replaces = there(plan.to);
            plan.inPlace = plan.replaces && std::filesystem::equivalent(plan.from, plan.to, ec);
            return plan;
        }
        if (mod.problem.rfind("mod.ini: ", 0) == 0 || mod.problem.rfind("map.ini: ", 0) == 0)
        {
            plan.kind = DropInstall::Unreadable;
            plan.why = mod.problem;
            return plan;
        }
        // A mod's own parts (its music, textures, tuning) and no disc paths: a mod that does not say its games.
        // (A disc, or a folder of discs, has disc paths: the disc search's.)
        if ((mod.contents & (kModMusic | kModTextures | kModTuning)) != 0u && (mod.contents & kModFiles) == 0u)
        {
            plan.kind = DropInstall::NoManifest;
            plan.why = "it has no mod.ini naming its games";
        }
        return plan;
    }

    bool installDrop(const DropInstall &plan, const std::filesystem::path &supportDir, std::string *error)
    {
        auto fail = [error](const std::string &why, const std::error_code &ec)
        {
            if (error)
                *error = why + (ec ? ": " + ec.message() : std::string());
            return false;
        };
        if (plan.kind != DropInstall::Mod && plan.kind != DropInstall::Map)
            return fail("not a mod or a map", {});
        // (what is removed below is <folder>/<name> only: never the folder itself or what holds it)
        const std::filesystem::path name = plan.to.filename();
        if (!entryName(name) || !entryName(plan.to.parent_path().filename()))
            return fail("it has no name to be put under", {});
        if (plan.inPlace)
            return true;
        std::error_code ec, ignored;
        const std::filesystem::path incoming = supportDir / kIncomingFolder;
        const std::filesystem::path replacedFolder = supportDir / kReplacedFolder;
        const std::filesystem::path part = incoming / name;
        const std::filesystem::path replaced = replacedFolder / name;
        auto tidy = [&]()
        {
            std::filesystem::remove_all(part, ignored);
            std::filesystem::remove(incoming, ignored); // (each only when empty)
            std::filesystem::remove(replacedFolder, ignored);
        };
        std::filesystem::create_directories(incoming, ec);
        std::filesystem::remove_all(part, ec);
        ec.clear();
        // (a link in a folder copied as the link, never followed: a link to a large folder, or round to its own,
        // copies no more than the mod)
        if (std::filesystem::is_directory(plan.from, ec))
            std::filesystem::copy(plan.from, part,
                                  std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks, ec);
        else
            std::filesystem::copy_file(plan.from, part, ec);
        if (ec)
        {
            tidy();
            return fail("the copy failed", ec);
        }
        const std::string folder = plan.to.parent_path().filename().string();
        std::filesystem::create_directories(plan.to.parent_path(), ec);
        ec.clear();
        // The one there moved aside, and removed only once the copy is in its place: put back when the copy cannot
        // go in, so a Replace that fails leaves the one there as it was.
        const bool replacing = there(plan.to);
        if (replacing)
        {
            std::filesystem::create_directories(replacedFolder, ec);
            std::filesystem::remove_all(replaced, ec); // (one left by a run that ended halfway)
            ec.clear();
            std::filesystem::rename(plan.to, replaced, ec);
            if (ec)
            {
                tidy();
                return fail("the one in " + folder + " could not be moved aside", ec);
            }
        }
        std::filesystem::rename(part, plan.to, ec);
        if (ec)
        {
            if (replacing)
                std::filesystem::rename(replaced, plan.to, ignored);
            tidy();
            return fail("it could not be put in " + folder, ec);
        }
        if (replacing)
            std::filesystem::remove_all(replaced, ignored);
        tidy();
        return true;
    }
}
