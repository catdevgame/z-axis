// A CHD disc image (MAME's compressed format) read through libchdr: a CD's
// first track as its sectors, or a DVD's bytes as they are.
#include "disc_image_formats.h"

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace ps2x::iop::disc_formats
{
    namespace
    {
        // A CD track as a CHD's metadata gives it.
        struct ChdTrack
        {
            int number = 0;
            char type[32] = {};
            int frames = 0;
            int pregap = 0;
            char pregapType[32] = {};
        };

        bool readTrack(chd_file *chd, uint32_t index, ChdTrack &track)
        {
            char text[256] = {};
            uint32_t length = 0u;
            char subtype[32] = {}, pregapSub[32] = {};
            int postgap = 0;
            if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, index, text, sizeof(text) - 1u, &length, nullptr, nullptr) ==
                CHDERR_NONE)
                return std::sscanf(text, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                                   &track.number, track.type, subtype, &track.frames, &track.pregap, track.pregapType,
                                   pregapSub, &postgap) == 8;
            // (the first CD CHDs' metadata: no pregap)
            if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, index, text, sizeof(text) - 1u, &length, nullptr, nullptr) ==
                CHDERR_NONE)
                return std::sscanf(text, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d", &track.number, track.type, subtype,
                                   &track.frames) == 4;
            return false;
        }

        // The bytes of each sector a track type stores (a CHD keeps them at
        // the start of each 2448-byte frame); 0 for no data sectors.
        uint32_t dataBytesOf(const char *type)
        {
            static constexpr struct
            {
                const char *type;
                uint32_t bytes;
            } kTypes[] = {
                {"MODE1", 2048u},     {"MODE2_FORM1", 2048u}, {"MODE1_RAW", 2352u}, {"MODE2_RAW", 2352u},
                {"MODE2", 2336u},     {"MODE2_FORM_MIX", 2336u},
            };
            for (const auto &known : kTypes)
                if (std::strcmp(type, known.type) == 0)
                    return known.bytes;
            return 0u; // AUDIO, MODE2_FORM2 (no ISO9660 there)
        }

        class ChdImage final : public DiscImageFile
        {
        public:
            ChdImage(OwnedFile file, chd_file *chd) : m_file(std::move(file)), m_chd(chd) {}
            ~ChdImage() override { chd_close(m_chd); } // (then the file, which libchdr does not close)

            bool init(std::string *error)
            {
                const chd_header *header = chd_get_header(m_chd);
                if (!header || header->hunkbytes == 0u)
                    return fail(error, "a CHD without its header");
                m_hunkBytes = header->hunkbytes;
                m_hunk.resize(m_hunkBytes);
                ChdTrack track;
                if (!readTrack(m_chd, 0u, track))
                {
                    // A DVD (or any CHD of plain bytes): its logical bytes.
                    m_size = header->logicalbytes;
                    return true;
                }
                // A CD: its first track, where a PS2 CD keeps its data.
                m_sectorBytes = dataBytesOf(track.type);
                if (m_sectorBytes == 0u)
                    return fail(error, "a CD CHD whose first track holds no data");
                m_framesPerHunk = m_hunkBytes / CD_FRAME_SIZE;
                if (m_framesPerHunk == 0u || m_hunkBytes % CD_FRAME_SIZE != 0u || track.frames <= 0)
                    return fail(error, "a CD CHD of unknown layout");
                // A pregap stored with the track ("V" type) comes before its sectors.
                m_firstFrame = track.pregapType[0] == 'V' && track.pregap > 0 ? static_cast<uint64_t>(track.pregap) : 0u;
                m_size = static_cast<uint64_t>(track.frames) * m_sectorBytes;
                if ((m_firstFrame + static_cast<uint64_t>(track.frames)) > static_cast<uint64_t>(header->totalhunks) * m_framesPerHunk)
                    return fail(error, "a CD CHD shorter than its track");
                return true;
            }

            Format format() const override { return Format::Chd; }
            uint64_t size() const override { return m_size; }
            uint32_t sectorSize() const override { return m_sectorBytes; }

            bool read(uint64_t offset, void *destination, size_t size) override
            {
                if (offset > m_size || size > m_size - offset)
                    return false;
                auto *out = static_cast<uint8_t *>(destination);
                while (size != 0u)
                {
                    uint64_t hunk = 0u;
                    size_t within = 0u, count = 0u;
                    if (m_sectorBytes == 0u)
                    {
                        hunk = offset / m_hunkBytes;
                        within = static_cast<size_t>(offset % m_hunkBytes);
                        count = std::min<size_t>(size, m_hunkBytes - within);
                    }
                    else
                    {
                        const uint64_t frame = m_firstFrame + offset / m_sectorBytes;
                        const size_t inSector = static_cast<size_t>(offset % m_sectorBytes);
                        hunk = frame / m_framesPerHunk;
                        within = static_cast<size_t>(frame % m_framesPerHunk) * CD_FRAME_SIZE + inSector;
                        count = std::min<size_t>(size, m_sectorBytes - inSector);
                    }
                    if (!loadHunk(hunk))
                        return false;
                    std::memcpy(out, m_hunk.data() + within, count);
                    out += count;
                    offset += count;
                    size -= count;
                }
                return true;
            }

        private:
            static bool fail(std::string *error, const char *message)
            {
                if (error)
                    *error = message;
                return false;
            }

            bool loadHunk(uint64_t hunk)
            {
                if (static_cast<int64_t>(hunk) == m_loadedHunk)
                    return true;
                m_loadedHunk = -1;
                if (hunk > UINT32_MAX || chd_read(m_chd, static_cast<uint32_t>(hunk), m_hunk.data()) != CHDERR_NONE)
                    return false;
                m_loadedHunk = static_cast<int64_t>(hunk);
                return true;
            }

            OwnedFile m_file;
            chd_file *m_chd = nullptr;
            uint32_t m_hunkBytes = 0u;
            uint64_t m_size = 0u;
            // A CD's: each sector's bytes, frames a hunk, the track's first frame.
            uint32_t m_sectorBytes = 0u;
            uint32_t m_framesPerHunk = 0u;
            uint64_t m_firstFrame = 0u;
            std::vector<uint8_t> m_hunk;
            int64_t m_loadedHunk = -1;
        };
    }

    std::unique_ptr<DiscImageFile> openChd(OwnedFile file, std::string *error)
    {
        chd_file *chd = nullptr;
        const chd_error opened = chd_open_file(file.get(), CHD_OPEN_READ, nullptr, &chd);
        if (opened != CHDERR_NONE)
        {
            if (error)
                *error = opened == CHDERR_REQUIRES_PARENT ? "a CHD that needs its parent CHD"
                                                          : std::string("a CHD libchdr cannot open (") + chd_error_string(opened) + ")";
            return nullptr;
        }
        auto image = std::make_unique<ChdImage>(std::move(file), chd);
        if (!image->init(error))
            return nullptr;
        return image;
    }
}
