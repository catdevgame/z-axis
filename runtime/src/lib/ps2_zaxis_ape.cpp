#include "runtime/ps2_zaxis_ape.h"

#include <cstring>
#include <string_view>

namespace ps2x::zaxis::ape
{
    size_t recolour(std::vector<uint8_t> &zit)
    {
        size_t recoloured = 0;
        const std::string_view view(reinterpret_cast<const char *>(zit.data()), zit.size());
        for (size_t at = view.find("MATI"); at != std::string_view::npos && at + 20u <= zit.size();
             at = view.find("MATI", at + 4u))
        {
            static constexpr uint8_t kBlack[4] = {0x00, 0x00, 0x80, 0xFF};
            static constexpr uint8_t kSkater[4] = {0xE5, 0xE5, 0xE5, 0xFF};
            if (std::memcmp(zit.data() + at + 16u, kBlack, 4) == 0)
            {
                std::memcpy(zit.data() + at + 16u, kSkater, 4);
                ++recoloured;
            }
        }
        return recoloured;
    }

    bool build(const zoo::Reader &xbox, zoo::Made &out, std::ostream &log, std::string *error)
    {
        auto fail = [&](const std::string &why)
        {
            if (error)
                *error = why;
            return false;
        };
        std::vector<uint8_t> bytes;
        if (!xbox(kXboxModel, bytes))
            return fail(std::string("no ") + kXboxModel + " in the Xbox files");
        xbox::ModelReport report;
        std::vector<xbox::HostTexture> host;
        std::string why;
        if (!xbox::convertModel(bytes, &report, &why, kMaxTextureSide, &host))
            return fail("GOR.ZIT: " + why);
        log << "[ape] GOR.ZIT: " << report.textures.size() << " textures, " << report.geometry.groupsIn
            << " strips cut into " << report.geometry.groupsOut << " groups, " << report.geometry.materials
            << " materials, " << bytes.size() / 1024u << " KB\n";
        log << "[ape] GOR.ZIT: " << recolour(bytes) << " materials' first colour as the other skaters'\n";
        out.files[kModelPath] = std::move(bytes);
        out.files[std::string(zoo::kPictureFolder) + kModelPath + ".xtp"] = zoo::picturePack(host);
        log << "[ape] " << host.size() << " Xbox originals of the Ape's textures for the GPU renderer\n";
        return true;
    }
}
