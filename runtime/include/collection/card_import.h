#pragma once

// A save imported from a PCSX2 memory card image into the app's card folder
// (the HLE card layout) or a native save file.

#include "collection/settings.h" // GetEnv

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace zx::app
{
    // PCSX2 memory card images (.ps2, 8 MB, with or without ECC spare bytes).
    struct CardSave
    {
        std::string name;
        std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    };
    bool readCardSave(const std::filesystem::path &cardImage, const std::string &saveDir, CardSave &save, std::string *error = nullptr);
    // Copies saveDir from the card image into slotFolder/saveDir (the HLE card
    // layout). An existing save there is kept unless overwrite is set.
    bool importCardSave(const std::filesystem::path &cardImage, const std::string &saveDir,
                        const std::filesystem::path &slotFolder, bool overwrite, std::string *error = nullptr);
    // Copies the data file of saveDir (dataFile; empty: the file named like
    // the folder, e.g. BASLUS-20327CEAIGAME/BASLUS-20327CEAIGAME) from the
    // card image to a native save file, written atomically
    // (runtime/ps2_save_store.h). An existing file is kept unless overwrite
    // is set (then it becomes .bak).
    bool importCardSaveToFile(const std::filesystem::path &cardImage, const std::string &saveDir,
                              const std::filesystem::path &file, bool overwrite, std::string *error = nullptr,
                              const std::string &dataFile = {});
    // Card images under PCSX2's usual memcards folders that hold saveDir.
    std::vector<std::filesystem::path> findPcsx2CardsWithSave(const std::string &saveDir, const GetEnv &getEnv);
}
