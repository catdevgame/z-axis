#pragma once

// Zips for the tests (mods packed as zips, runtime/zip_archive.h),
// written with miniz: each file deflated, or stored when asked.

#include <miniz.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace test_zip
{
    // Writes `files` (name in the zip, contents) to `zip`; false if miniz
    // could not.
    inline bool write(const std::filesystem::path &zip, const std::vector<std::pair<std::string, std::string>> &files,
                      bool deflate = true)
    {
        std::filesystem::create_directories(zip.parent_path());
        mz_zip_archive archive{};
        if (!mz_zip_writer_init_file(&archive, zip.string().c_str(), 0))
            return false;
        bool ok = true;
        for (const auto &[name, text] : files)
            ok = ok && mz_zip_writer_add_mem(&archive, name.c_str(), text.data(), text.size(),
                                             deflate ? MZ_DEFAULT_COMPRESSION : MZ_NO_COMPRESSION);
        ok = mz_zip_writer_finalize_archive(&archive) && ok;
        mz_zip_writer_end(&archive);
        return ok;
    }
}
