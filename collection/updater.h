#pragma once

// The app's own update (Settings > Check for Updates, Roll Back Update): a
// release's published files and their SHA256SUMS are the source; this
// platform's package is downloaded beside the app, checked against its sum,
// put in the installed copy's place the platform's own way, and the app
// starts again. On the Mac and Windows the version it replaced is kept beside
// it, so Roll Back Update puts it back without the network (and the one
// rolled back from is kept in its turn). The player's own folder (settings, saves, set-up games, logs) is
// never read or written, but for one thing: going back to an older version,
// the root settings.ini's keys that version does not read are kept aside for
// it (setAsideNewerSettings, below). Everything else here is in the app's
// update folder, beside the app, and in the app itself.
//
// The ways each platform installs (UpdatePlatform::Install):
//  - Bundle (macOS): the release's zip holds the .app; unpacked in the update
//    folder, then the installed bundle moved into previous/ and the new one
//    moved into its place (whole, as Finder replaces it: its signature seals
//    it). Its update folder is ".<bundle name> update" beside the bundle.
//  - Folder (Windows): the zip's files written over the app's folder, each
//    only where it differs (as a player's unzip over it would);
//    the files replaced go to previous/files/ (a running .exe and its DLLs
//    can be moved, not overwritten), the files the old version lacked are
//    listed in previous/added.txt. Files the zip has not got stay (portable
//    mode's portable.txt and "<app> Data"). Its update folder is "update" in
//    the app's folder.
//  - Package (Android): the APK handed to the system's package installer (the
//    same key: an update keeps the app's data). Updates only: Android's
//    installer puts no older version over a newer one for an app, so an
//    older release is not offered and there is no Roll Back Update. Its
//    update folder (the download) is in the app's internal storage.
//
// The source: ZX_UPDATE_SOURCE (an http(s) URL of a release's files, or a
// folder holding them, as a release's dry run gives), else the latest
// release on GitHub. Its SHA256SUMS names each package
// ("<sha256>  Z-Axis-Collection-<version>-<platform>-<arch>.<ext>", and
// before 1.4.0 "...-<version>-<platform>.<ext>"); this platform's is the one
// whose name ends with one of UpdatePlatform::packageSuffixes. A source with
// another version than the app's (newer: an update; older: going back) is
// offered.

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace zx::collection
{
    // ---- the release's files ----------------------------------------------------

    // One line of a SHA256SUMS: a published file and its SHA-256.
    struct PublishedFile
    {
        std::string name;
        std::string sha256; // lower-case hex
    };

    // A SHA256SUMS's lines ("<64 hex>  <name>", "<64 hex> *<name>"); lines
    // that are not one are passed over.
    std::vector<PublishedFile> parseSha256Sums(const std::string &text);

    // The version x.y.z in a package's name ("Z-Axis-Collection-1.3.1-mac.zip"
    // -> "1.3.1"), "" when it has none.
    std::string versionInName(const std::string &name);

    // Versions x.y.z compared part by part: below 0 when a is older, 0 the
    // same, above 0 newer. A version that is not x.y.z ("dev") is older than
    // any that is.
    int compareVersions(const std::string &a, const std::string &b);

    // A file's SHA-256 in lower-case hex ("" when it cannot be read).
    std::string fileSha256(const std::filesystem::path &file);

    // ---- a folder's files replaced, and put back (the Folder way) ---------------

    // The files below `from` written over `into` (both kept to their
    // relative paths), each only where it differs: the file it replaces moved
    // to `undo`/files/<path> first, a file `into` lacked listed in
    // `undo`/added.txt. `removed` (relative paths, as an earlier added.txt
    // lists them) are moved from `into` to `undo`/files/ as well. `undo` is a
    // new folder; `from` is emptied. False (and `into` as it was) when a
    // file cannot be moved.
    struct FolderChange
    {
        int written = 0, unchanged = 0, removed = 0;
    };
    bool replaceFiles(const std::filesystem::path &from, const std::filesystem::path &into,
                      const std::vector<std::string> &removed, const std::filesystem::path &undo,
                      FolderChange *change, std::string *error);

    // The relative paths an undo folder's added.txt lists.
    std::vector<std::string> addedFiles(const std::filesystem::path &undo);

    // ---- the player's settings, kept for an older version --------------------------

    // Every version rewrites the collection's root settings.ini at its quit
    // with only the keys it reads, and one may take a key it does not know
    // for a game's (1.3.1 took 1.4.0's [general] practice for one and wrote
    // games/ai/settings.ini anew, all defaults). So before the app restarts
    // into an older version (Roll Back Update, or an older release from the
    // update row), the root keys added after that version
    // (zx::app::rootSettingSince) are moved out of settings.ini into
    // settings.newer.ini beside it; the version that reads them again puts
    // them back at its start, with the player's values. A key's line goes
    // with the comment lines right above it (as formatSettings writes them);
    // the rest of settings.ini stays byte for byte.

    // The side file of a settings.ini: settings.newer.ini beside it.
    std::filesystem::path newerSettingsFile(const std::filesystem::path &settingsFile);

    // A settings text and its side file's text, as moved between them, and
    // the keys moved ("[section] key").
    struct SettingsMove
    {
        std::string settings;
        std::string newer;
        std::vector<std::string> moved;
    };
    using SettingsKeyTest = std::function<bool(const std::string &section, const std::string &key)>;
    // The keys of `settings` that `aside` picks moved into `newer` (an
    // earlier side file: a key it has already is replaced, the others kept).
    SettingsMove setAsideSettings(const std::string &settings, const std::string &newer, const SettingsKeyTest &aside);
    // The keys of `newer` that `known` picks put back into `settings`, at the
    // end of their section (the section added when it has none); one that
    // `settings` sets already keeps its value there. The others stay in
    // `newer` ("" once none is left).
    SettingsMove putBackSettings(const std::string &settings, const std::string &newer, const SettingsKeyTest &known);

    // The files: settingsFile's keys added after `version` set aside, and
    // the side file's keys this version writes (zx::app::rootSettingKnown)
    // put back, the side file removed once empty. Nothing is written when
    // nothing moves. What was done goes to the log.
    void setAsideNewerSettings(const std::filesystem::path &settingsFile, const std::string &version,
                               std::vector<std::string> *log = nullptr);
    void putBackNewerSettings(const std::filesystem::path &settingsFile, std::vector<std::string> *log = nullptr);

    // ---- what each platform does its own way ---------------------------------

    struct UpdatePlatform
    {
        enum class Install
        {
            None,    // no update here (a build with no packages: Linux)
            Bundle,  // the .app replaced whole (macOS)
            Folder,  // the app's folder's files replaced (Windows)
            Package, // the system's package installer (Android)
        } install = Install::None;
        // The ends of this platform's package's name in SHA256SUMS, the first
        // found taken ("-mac-arm64.zip", then 1.3.1's "-mac.zip").
        std::vector<std::string> packageSuffixes;
        // What is installed: the .app bundle, the app's folder, the APK.
        std::filesystem::path installed;
        // Where the downloads, the unpacked package and the previous version
        // are kept (on the installed copy's volume, never the player's folder).
        std::filesystem::path updateFolder;
        // `url` (http or https) into the file `to`; false and why when it
        // cannot (no connection: nothing else changes).
        std::function<bool(const std::string &url, const std::filesystem::path &to, std::string *error)> download;
        // Bundle, Folder: the zip unpacked into the folder `into`.
        std::function<bool(const std::filesystem::path &zip, const std::filesystem::path &into, std::string *error)> unpack;
        // The version a package says it is (Bundle: an unpacked .app; Folder:
        // an unpacked folder; Package: the APK), "" unknown.
        std::function<std::string(const std::filesystem::path &package)> packageVersion;
        // Package: the APK handed to the system's installer; `done` is called
        // with "" once the installer has it and the app is about to be
        // replaced, else with why not. False: it could not even be handed
        // over (`error` says why).
        std::function<bool(const std::filesystem::path &apk, std::function<void(std::string)> done, std::string *error)>
            installPackage;
        // Bundle, Folder: the installed copy started again (as it was started,
        // its environment but ZX_MENU_SCRIPT, which drove this run), its
        // process id, -1 when it could not be.
        std::function<long()> relaunch;
    };

    // ---- the updater ------------------------------------------------------------

    class Updater
    {
    public:
        enum class Stage
        {
            Idle,       // nothing asked yet
            Checking,   // reading the source's SHA256SUMS
            Current,    // the source has this version
            Offered,    // the source has another version: `offered`
            Installing, // downloading, checking and putting it in place
            Restart,    // in place: the app quits and starts again (Bundle, Folder)
            Handed,     // with the system's installer (Package)
            Failed,     // `message` says why; nothing changed
            Unsupported // no update on this platform
        };

        Updater(UpdatePlatform platform, std::string appVersion, std::string source);
        ~Updater();

        // The default source: the latest release's files on GitHub.
        static const char *defaultSource();

        // Settings > Check for Updates' Enter: checks (Idle, Current, Failed),
        // or installs what was offered (Offered). Work runs on a thread of
        // its own; the menu goes on.
        void activate();
        // Settings > Roll Back Update's Enter: the previous version put back.
        void rollBack();

        Stage stage() const;
        // The row's value: "1.3.1", "Checking...", "1.4.0: Enter installs",
        // "Up to date", "No connection" ...
        std::string summary() const;
        // Whether this platform keeps the version an update replaced, for
        // Roll Back Update (the Mac and Windows; not Android).
        bool rollsBack() const;
        // The previous version kept ("" none), the Roll Back row's value.
        std::string previousVersion() const;
        // A line for the menu's notice, once each time it changes ("" none new).
        std::string takeNotice();
        // Restart: the app should quit now and relaunchAtQuit start it again.
        bool restartWanted() const;
        // The version the restart starts ("" no restart).
        std::string restartVersion() const;
        // At the app's quit, after its own (its settings written): the new
        // copy started (Restart only). An older one: settingsFile's keys it
        // does not read set aside for it first (setAsideNewerSettings).
        void relaunchAtQuit(const std::filesystem::path &settingsFile = {});

    private:
        void run(Stage stage, const std::string &message, std::function<void()> work);
        void set(Stage stage, const std::string &message, const std::string &notice);
        void check();
        void install(const PublishedFile &file, const std::string &version);
        bool putInPlace(const std::filesystem::path &package, const std::string &version, bool older,
                        std::string *error);
        void putBack();
        std::filesystem::path previousFolder() const { return m_platform.updateFolder / "previous"; }
        bool fetch(const std::string &name, const std::filesystem::path &to, std::string *error);

        UpdatePlatform m_platform;
        std::string m_appVersion;
        std::string m_source;
        mutable std::mutex m_mutex;
        Stage m_stage = Stage::Idle;
        std::string m_message;
        std::string m_notice;
        std::optional<PublishedFile> m_offered;
        std::string m_offeredVersion;
        std::string m_restartVersion;
        std::thread m_worker;
    };
}
