#pragma once

// Python's difflib.SequenceMatcher (CPython 3.15, Lib/difflib.py), ported as
// it is for what the extras' converters use: two lists of lines compared
// with no junk and autojunk off (SequenceMatcher(None, a, b, autojunk=False)),
// and the opcodes that turn one into the other. tools/bmx_releases.py picks
// lines by these opcodes, so the converter's files are the tool's only with
// the same ones.

#include <cstddef>
#include <string>
#include <vector>

namespace ps2x::app::extras
{
    struct DiffOpcode
    {
        enum class Tag
        {
            Replace,
            Delete,
            Insert,
            Equal,
        };
        Tag tag;
        size_t aBegin, aEnd, bBegin, bEnd; // a[aBegin:aEnd] becomes b[bBegin:bEnd]
    };

    // get_opcodes() of SequenceMatcher(None, a, b, autojunk=False).
    std::vector<DiffOpcode> diffOpcodes(const std::vector<std::string> &a, const std::vector<std::string> &b);
}
