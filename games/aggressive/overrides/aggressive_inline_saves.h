#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class PS2Machine;

namespace aggressive_inline
{
    // Native save files instead of the memory card (aggressive_inline_saves.cpp).
    // ZX_SAVES=card keeps the original memory card path.
    void installNativeSaves(PS2Machine &runtime);
    // Where the native saves go and the profile's file name stem
    // ("<saves>/<stem>.sav"), for files kept beside the career save.
    std::filesystem::path nativeSavesFolder();
    std::string nativeSaveProfile();
    // A change made to every career body the native saves write (the
    // autosave's and the menus' Save, after the Ape's career is added), in
    // the order added: body is the save's data after its checksum word
    // (the globals from 0x24CBB8 first, then the skaters). An extra level
    // that borrows a retail level's slot in RAM puts the retail level's own
    // data back there (aggressive_inline_estate.cpp: the Tutorial's records).
    using CareerBodyFilter = std::function<void(std::vector<uint8_t> &body)>;
    void addCareerBodyFilter(CareerBodyFilter filter);
}
