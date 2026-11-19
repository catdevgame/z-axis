#pragma once

// The game's files read on the host, as the game's file layer sees them
// (the disc folder or image, mods too): for the native asset readers.

#include <cstdint>
#include <string>
#include <vector>

namespace zge::native
{
    // A path as the game opens it ("dvd0:\ASSETS\LEVELS\TUT\TUT.GEO") in the
    // form the file layer keys it ("ASSETS/LEVELS/TUT/TUT.GEO").
    std::string normalizeGamePath(const std::string &gamePath);

    // All of a file (normalized path). Thread-safe.
    bool readGameFile(const std::string &normalizedPath, std::vector<uint8_t> &out);

    // The disc layer (game_overrides/zge_disc.cpp, AI's table): `bytes`
    // of the file at `path` from `offset` were read to guest `address`.
    // Kept per address range (a later read over the same bytes replaces it).
    void noteDiscRead(const std::string &path, uint64_t offset, uint32_t address, uint32_t bytes);

    // The file and offset the guest bytes [address, address + size) were
    // read from, if one file's reads cover them contiguously (normalized
    // path). Thread-safe.
    bool discOrigin(uint32_t address, uint32_t size, std::string &path, uint64_t &offset);

    // How many times the game has started reading the file (a read at
    // offset 0; normalized path), 0 if never. A file read again may have
    // other contents: game overrides serve different files under one path
    // (TUT/Estate as the Tutorial's, the DM2 levels as LOCAL), so a cache
    // keyed by path checks this. Thread-safe.
    uint64_t discFileReads(const std::string &normalizedPath);
}
