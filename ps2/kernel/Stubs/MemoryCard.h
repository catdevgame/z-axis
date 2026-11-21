#pragma once

#include "stubs.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ps2_stubs
{

    struct MemoryCardDebugOpenFile
    {
        int32_t fd = 0;
        int32_t port = 0;
        std::filesystem::path hostPath;
    };

    struct MemoryCardDebugPort
    {
        int32_t port = 0;
        std::string currentDir;
        bool formatted = false;
        std::filesystem::path rootPath;
    };

    struct MemoryCardDebugSnapshot
    {
        int32_t nextFd = 0;
        int32_t lastCmd = 0;
        int32_t lastResult = 0;
        int32_t cvFileCursor = 0;
        std::array<MemoryCardDebugPort, 2> ports{};
        std::vector<MemoryCardDebugOpenFile> openFiles;
    };

    MemoryCardDebugSnapshot getMemoryCardDebugSnapshot();
    // The host folder of the card in a port (0: slot 1, 1: slot 2).
    std::filesystem::path memoryCardFolder(int32_t port);
    // The last command's result, consumed like sceMcSync does, for callers
    // that run a command synchronously (the IOP MCSERV module).
    int32_t takeMemoryCardResult();
    // Power cycle the card slots: the next sceMcInit re-reads
    // ZX_MC_SLOT1/2 and every card counts as newly inserted again.
    void resetMemoryCardSlots();
    void sceMcChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcChdir(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcClose(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcDelete(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcEnd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcFlush(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcFormat(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcGetDir(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcGetEntSpace(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcGetInfo(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcMkdir(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcOpen(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcRead(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcRename(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcSeek(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcSetFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcSync(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcUnformat(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void sceMcWrite(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcCallMessageTypeSe(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcCheckReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcCheckReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcCheckWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcCheckWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcCreateConfigInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcCreateFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcCreateIconInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcCreateSaveFileInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcDispFileName(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcDispFileNumber(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcDisplayFileSelectWindow(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcDisplaySelectFileInfo(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcDisplaySelectFileInfoMesCount(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcDispWindowCurSol(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcDispWindowFoundtion(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mceGetInfoApdx(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mceIntrReadFixAlign(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mceStorePwd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcGetConfigCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcGetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcGetFreeCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcGetIconCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcGetIconFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcGetPortSelectDirInfo(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcGetSaveFileCapacitySize(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcGetStringEnd(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcMoveFileSelectWindowCursor(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcNewCreateConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcNewCreateIcon(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcNewCreateSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcReadIconData(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcReadStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcReadStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcSelectFileInfoInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcSelectSaveFileCheck(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcSetFileSelectWindowCursol(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcSetFileSelectWindowCursolInit(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcSetStringSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcSetTyepWriteMode(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcWriteIconData(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcWriteStartConfigFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
    void mcWriteStartSaveFile(uint8_t *rdram, R5900Context *ctx, PS2Machine *runtime);
}
