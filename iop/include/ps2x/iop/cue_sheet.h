#pragma once

// A CUE sheet: the text a BIN/CUE dump keeps beside its BIN files, naming
// the file each track is in. A PS2 CD's data is its data track's (Mode 1 or
// Mode 2); a multi-track dump has its audio tracks in files of their own, or
// after the data in the same one.

#include <string>
#include <string_view>

namespace ps2x::iop
{
    // Whether the text is a CUE sheet, by what it says (not the file's name):
    // every line one of the sheet's commands, a FILE among them.
    bool isCueSheet(std::string_view text);

    // The file the sheet's first data track is in ("" when it has none; a
    // sheet with no TRACK lines: its first FILE), as the sheet names it (a
    // name beside the sheet).
    std::string cueDataTrackFile(std::string_view text);
}
