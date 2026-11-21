#pragma once

// libmc, the memory card library, as game code meets it. Sony's own header is
// not at hand: every name and layout here is ps2sdk's public libmc.h and
// libmc-common.h, which describe the same library and the same card format.

#include "ee/ee.h"

#include <cstddef>
#include <cstdint>

namespace sce
{
    // sceMcGetInfo's card types.
    constexpr int32_t sceMcTypeNoCard = 0, sceMcTypePS1 = 1, sceMcTypePS2 = 2, sceMcTypePDA = 3;

    // What a command ends with (sceMcSync's result). A negative number below
    // these is the card failing or gone.
    constexpr int32_t sceMcResSucceed = 0, sceMcResChangedCard = -1, sceMcResNoFormat = -2, sceMcResFullDevice = -3,
                      sceMcResNoEntry = -4, sceMcResDeniedPermit = -5, sceMcResNotEmpty = -6,
                      sceMcResUpLimitHandle = -7, sceMcResFailReplace = -8;

    // sceMcSync's modes.
    constexpr int32_t sceMcSyncWait = 0, sceMcSyncPoll = 1;

    // sceMcGetDir's mode: the first match, or the next one.
    constexpr int32_t sceMcGetDirFirst = 0, sceMcGetDirNext = 1;

    // sceMcOpen's modes: 1 reading, 2 writing, 3 both; sceMcFileCreateFile
    // makes the file when it is missing.
    constexpr int32_t kMcOpenRead = 1, kMcOpenWrite = 2, kMcOpenReadWrite = 3, sceMcFileCreateFile = 0x200;

    struct sceMcStDateTime
    {
        uint8_t Resv2, Sec, Min, Hour, Day, Month;
        uint16_t Year;
    };

    // One directory entry from sceMcGetDir.
    struct sceMcTblGetDir
    {
        sceMcStDateTime _Create;
        sceMcStDateTime _Modify;
        uint32_t FileSizeByte;
        uint16_t AttrFile;
        uint16_t Reserve1;
        uint32_t Reserve2;
        uint32_t PdaAplNo;
        char EntryName[32];
    };
    static_assert(sizeof(sceMcTblGetDir) == 64 && offsetof(sceMcTblGetDir, EntryName) == 32);

    // icon.sys on the card (ps2sdk's mcIcon).
    struct mcIcon
    {
        char head[4];        // "PS2D"
        uint16_t type;
        uint16_t nlOffset;   // where the title's second line starts, in bytes
        uint32_t _08;
        int32_t trans;
        int32_t bgCol[4][4];
        ee::Float lightDir[3][4];
        ee::Float lightCol[3][4];
        ee::Float lightAmbient[4];
        uint16_t title[34];  // Shift-JIS, big-endian
        char view[64];       // the icon files, by what the browser is doing
        char copy[64];
        char del[64];
        char _1c4[512];
    };
    static_assert(sizeof(mcIcon) == 964 && offsetof(mcIcon, title) == 0xC0 && offsetof(mcIcon, del) == 0x184);
}
