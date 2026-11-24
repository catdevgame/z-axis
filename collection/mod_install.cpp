#include "collection/mod_install.h"

#include "collection/game_setup.h"
#include "collection/maps.h"
#include "collection/mods.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

namespace zx::app
{
    namespace
    {
        // The folder copies are made in before they are moved into place, and
        // the one a copy replaces is kept in until the copy is in its place.
        constexpr const char *kIncomingFolder = ".incoming";
        constexpr const char *kReplacedFolder = ".replaced";
        // The folder a picked document is copied into, to be read as a dropped path.
        constexpr const char *kPickedFolder = ".picked";

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

        std::string lowerCase(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        bool isZip(const std::filesystem::path &path)
        {
            return lowerCase(path.extension().string()) == ".zip";
        }

        // A document's name as one entry of a folder: none (empty) for a
        // name that is not one ("", ".", "..", one with a separator).
        std::filesystem::path entryOf(const std::string &documentName)
        {
            const std::filesystem::path name = pathFromUtf8(documentName);
            if (!entryName(name) || name.has_parent_path() || name.has_root_path())
                return {};
            return name;
        }

        // A picked folder's top holds a mod.ini or a map.ini (in any case, as the Mods folder reads them).
        bool describesItself(const std::vector<DocumentAccess::Entry> &top)
        {
            for (const DocumentAccess::Entry &entry : top)
                if (!entry.folder && (lowerCase(entry.name) == kModManifestFile || lowerCase(entry.name) == kMapDescriptionFile))
                    return true;
            return false;
        }

        // The document, and a folder's every document under it, copied to `to`.
        bool copyDocument(const DocumentAccess &documents, const DocumentAccess::Entry &document,
                          const std::filesystem::path &to, std::string &why)
        {
            std::error_code ec;
            if (document.folder)
            {
                std::filesystem::create_directory(to, ec);
                if (ec)
                {
                    why = "the copy failed: " + ec.message();
                    return false;
                }
                for (const DocumentAccess::Entry &entry : documents.list(document.uri))
                {
                    const std::filesystem::path name = entryOf(entry.name);
                    if (name.empty())
                    {
                        why = "it holds a file named \"" + entry.name + "\", which no folder can hold";
                        return false;
                    }
                    if (!copyDocument(documents, entry, to / name, why))
                        return false;
                }
                return true;
            }
            const std::unique_ptr<std::FILE, int (*)(std::FILE *)> in(documents.open(document.uri), &std::fclose);
            if (!in)
            {
                why = document.name + " could not be read";
                return false;
            }
            std::ofstream out(to, std::ios::binary);
            std::vector<char> buffer(1u << 20);
            size_t got = 0;
            while (out && (got = std::fread(buffer.data(), 1, buffer.size(), in.get())) > 0)
                out.write(buffer.data(), static_cast<std::streamsize>(got));
            if (std::ferror(in.get()))
            {
                why = document.name + " could not be read";
                return false;
            }
            out.close();
            if (!out)
            {
                why = "the copy failed: " + document.name + " could not be written";
                return false;
            }
            return true;
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

    namespace
    {
        bool putInPlace(const DropInstall &plan, const std::filesystem::path &supportDir, std::string *error)
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
            // copies no more than the mod; a picked document's copy is the collection's own: moved)
            if (plan.pickedCopy)
                std::filesystem::rename(plan.from, part, ec);
            else if (std::filesystem::is_directory(plan.from, ec))
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

    bool installDrop(const DropInstall &plan, const std::filesystem::path &supportDir, std::string *error)
    {
        const bool in = putInPlace(plan, supportDir, error);
        if (plan.pickedCopy)
            discardPickedCopy(plan, supportDir); // (left only when it was not moved in)
        return in;
    }

    PickedCopy copyPickedDocument(const DocumentAccess &documents, const std::string &uri, const std::filesystem::path &supportDir)
    {
        PickedCopy out;
        DocumentAccess::Entry document;
        if (!documents.stat(uri, document))
        {
            out.why = "it could not be read";
            return out;
        }
        out.name = document.name;
        const std::filesystem::path name = entryOf(document.name);
        if (name.empty())
        {
            out.why = "it has no name to be put under";
            return out;
        }
        if (document.folder ? !describesItself(documents.list(uri)) : !isZip(name))
        {
            out.why = document.folder ? "it has no mod.ini naming its games" : "it is not a mod's folder or .zip";
            return out;
        }
        std::error_code ec;
        const std::filesystem::path picked = supportDir / kPickedFolder;
        out.copy = picked / name;
        std::filesystem::create_directories(picked, ec);
        std::filesystem::remove_all(out.copy, ec); // (one left by a run that ended halfway)
        out.ok = copyDocument(documents, document, out.copy, out.why);
        if (!out.ok)
        {
            std::filesystem::remove_all(out.copy, ec);
            std::filesystem::remove(picked, ec); // (only when empty)
            out.copy.clear();
        }
        return out;
    }

    void discardPickedCopy(const DropInstall &plan, const std::filesystem::path &supportDir)
    {
        // (what is removed is an entry of <support>/.picked only: never a path the player picked)
        const std::filesystem::path picked = supportDir / kPickedFolder;
        if (!plan.pickedCopy || !entryName(plan.from.filename()) || plan.from.parent_path() != picked)
            return;
        std::error_code ec;
        std::filesystem::remove_all(plan.from, ec);
        std::filesystem::remove(picked, ec); // (only when empty)
    }
}
