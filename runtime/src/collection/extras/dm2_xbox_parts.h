#pragma once

// The parts of the DM2 Xbox converter (collection/extras/dm2_xbox.h), each
// in its file: the levels (dm2_xbox_levels.cpp), the park editor's themes
// and the attract demo (dm2_xbox_themes.cpp), the career (dm2_xbox_career.cpp).
// Each throws dm2::Dm2Error with what is wrong.

#include "collection/extras/dm2_xbox.h"
#include "dm2_files.h"

namespace ps2x::app::extras::dm2
{
    // The files written, by their path below the output folder.
    using Made = std::vector<std::pair<std::string, Bytes>>;

    // VENICE and GREENVIL, their INFO.TXT, PICTURES.ZIF and levels.txt.
    void convertLevels(const SourceFiles &xbox, const Dm2XboxOptions &options, Made &made, std::vector<std::string> &log);

    // themes/<FOLDER>/..., themes/THEMES.TXT, names.txt and PICTURES.ZIF.
    void convertThemes(const SourceFiles &xbox, const SourceFiles &ps2, Made &made, std::vector<std::string> &log);
    // demo/REPLAY.ZAL.
    void demoReplays(const SourceFiles &xbox, const SourceFiles &ps2, Made &made, std::vector<std::string> &log);

    // CAREER.BIN's bytes.
    Bytes buildCareer(const SourceFiles &xbox, const Bytes &defaultXbe, std::vector<std::string> &log);

    // A level file's chunks with its ZFTX, ZFGE, ZFIS and ZFSD names "<name>.<EXT>".
    Bytes convertZit(const Bytes &data, const std::string &name);

    // A file of the source in any case, or Dm2Error naming it.
    Bytes need(const SourceFiles &files, const std::string &path, const char *what);
}
