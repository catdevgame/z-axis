#ifndef PS2RECOMP_OUTPUT_MANIFEST_H
#define PS2RECOMP_OUTPUT_MANIFEST_H

// The output manifest: what made a folder of generated code, written into it by the recompiler as
// <output>/.ps2recomp-manifest after its last file. It names the recompiler (a hash of its own sources, the
// runtime headers they include and the compiler that built it), the SHA-256 of the config, of the ELF and of the
// Ghidra function map it read, and every file the run wrote with its size. A run removes the folder's manifest
// before it writes anything, so a folder with one holds a finished run's output.
//
// The app builds (make-app.sh, make-windows.ps1) key the folder by this file instead of reading each of its
// files (48k for AI and DM2: about 50 minutes on Windows under Defender): when the folder's files are exactly
// the manifest's names and sizes and none is newer than the manifest, the folder is what that run wrote;
// otherwise they hash every file as before.

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace ps2recomp
{
    inline constexpr const char *kOutputManifestName = ".ps2recomp-manifest";

    // SHA-256 of BYTES, or of the file at PATH ("" when it cannot be read), as 64 lowercase hex digits.
    std::string sha256Hex(const std::string &bytes);
    std::string sha256FileHex(const std::filesystem::path &path);

    // The recompiler that is running: its sources' hash and the compiler that built it (CMake writes them at
    // build time: cmake/source_id.cmake).
    std::string recompilerIdentity();

    struct OutputManifestInputs
    {
        std::filesystem::path configPath;
        std::filesystem::path elfPath;
        std::filesystem::path ghidraMapPath; // (empty: none)
    };

    // The manifest's text: FILES are each written file's name relative to the output folder ('/' between
    // folders) and its size in bytes, in any order (written sorted by name, byte order).
    std::string formatOutputManifest(const std::string &recompiler, const OutputManifestInputs &inputs,
                                     std::vector<std::pair<std::string, std::uintmax_t>> files);

    // Writes OUTPUT_DIR's manifest for the files WRITTEN (paths inside OUTPUT_DIR), their sizes read from the
    // folder now. False when a file is gone or the manifest cannot be written.
    bool writeOutputManifest(const std::filesystem::path &outputDir, const OutputManifestInputs &inputs,
                             const std::vector<std::filesystem::path> &written, std::string &error);
}

#endif
