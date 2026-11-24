#pragma once

// A mod or a custom map dropped on the collection's menu window, taken in:
// a folder or a zip the collection's Mods folder reads as a mod (its mod.ini
// has a [mod] section) or as a map (a mod.ini [map] section, or a zip with a
// map.ini) is copied into <support>/Mods; a folder with a map.ini alone, the
// layout from before manifests, into <support>/Maps (collection/mods.h,
// collection/maps.h). The menu asks first. Anything else dropped is no mod:
// the menu searches it for discs, as before; a folder of a mod's parts
// (Textures/, Music/, tuning.ini) with no mod.ini naming its games is said
// to need one, and a mod.ini or map.ini that does not read says why.
// A copy is made in <support>/.incoming first and moved into place whole,
// so the Mods page never lists half of one; the one it replaces is moved to
// <support>/.replaced, and removed only once the copy is in its place (put
// back when it cannot go in). A link in a dropped folder is copied as the
// link, never followed. A path that names no entry of a folder (a root, one
// ending in "..") is no mod.

#include <filesystem>
#include <string>

namespace zx::app
{
    class DocumentAccess;
}

namespace zx::app
{
    struct DropInstall
    {
        enum Kind
        {
            NotContent, // no mod or map: the disc search's
            Mod,        // into the Mods folder
            Map,        // into the maps folder
            NoManifest, // a mod's parts without a mod.ini naming its games
            Unreadable, // a mod.ini or map.ini that does not read
        } kind = NotContent;
        std::string why;           // NoManifest, Unreadable: why it is not taken in
        std::string title;         // as the Mods or the map page names it
        std::filesystem::path from; // what was dropped (its "." and ".." parts and a separator at its end worked out)
        std::filesystem::path to;   // where its copy goes: <folder>/<its file name>
        bool replaces = false;      // one of that name is there already
        bool inPlace = false;       // what was dropped is that one: nothing to copy
        bool pickedCopy = false;    // `from` is the collection's own copy of a picked document: moved in, not copied
    };

    // What `dropped` is to the collection whose support folder is `supportDir`.
    DropInstall planDropInstall(const std::filesystem::path &dropped, const std::filesystem::path &supportDir);

    // The copy: into <support>/.incoming, then moved to `to` (what was there
    // before replaced: the player said Replace). False and why, with what
    // was there before as it was.
    // A picked copy (pickedCopy) is gone afterwards, put in or not.
    bool installDrop(const DropInstall &plan, const std::filesystem::path &supportDir, std::string *error = nullptr);

    // A mod picked as a document (collection/game_setup.h DocumentAccess:
    // a folder's tree or a .zip), copied into <support>/.picked/<its name>
    // for planDropInstall to read as it reads a dropped path.
    struct PickedCopy
    {
        bool ok = false;
        std::string name;           // the document's, as the picker shows it
        std::filesystem::path copy; // ok: where the copy is
        std::string why;            // not ok: why there is none
    };
    // A folder with neither a mod.ini nor a map.ini at its top, or a file
    // that is not a .zip, is no mod and is not copied (a folder of discs
    // picked by mistake is gigabytes): not ok, and why.
    PickedCopy copyPickedDocument(const DocumentAccess &documents, const std::string &uri, const std::filesystem::path &supportDir);
    // A picked copy not installed (the player said Cancel, or it is no mod): removed.
    void discardPickedCopy(const DropInstall &plan, const std::filesystem::path &supportDir);
}
