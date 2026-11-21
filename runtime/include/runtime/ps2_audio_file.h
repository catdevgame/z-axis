#pragma once

// Audio files from the user (a custom soundtrack in the mods folder) as
// 16-bit stereo frames at the file's own rate. WAV (PCM 8/16/24/32-bit or
// 32-bit float) is read directly; with libavformat in the build (FFmpeg)
// also OGG Vorbis, FLAC and MP3 (and Opus with a system FFmpeg; the app's
// static one has no swresample, which its Opus decoder needs).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zx::audio_file
{
    class Reader
    {
    public:
        virtual ~Reader() = default;
        // Interleaved stereo frames (mono is doubled, channels past two are
        // dropped); fewer than asked, then 0, at the end.
        virtual size_t read(int16_t *out, size_t frames) = 0;
        // Back to the start; false if that failed.
        virtual bool rewind() = 0;
        [[nodiscard]] virtual uint32_t rate() const = 0;
        // Frames in the file (an estimate for compressed files; 0 unknown).
        [[nodiscard]] virtual uint64_t frames() const = 0;
        // A metadata tag of the file ("title", "artist"; any case), empty
        // if it has none (WAV files are not read for tags).
        [[nodiscard]] virtual std::string tag(const std::string &key) const
        {
            (void)key;
            return {};
        }
    };

    // Null if the file cannot be read (unknown format, damaged, no decoder).
    // A path through a zip ("<...>/Mods/songs.zip/Music/01.ogg": a mod's,
    // runtime/ps2_zip_archive.h) is read from it, whole, into memory.
    std::unique_ptr<Reader> open(const std::filesystem::path &path, std::string *error = nullptr);

    // The extensions open() can read in this build (".wav", ...), lower case.
    std::vector<std::string> extensions();

    // A song's name from its file name: no extension, and no leading track
    // number with the separators after it ("01 - My Song.ogg" -> "My Song").
    std::string nameFromFileName(const std::filesystem::path &path);
}
