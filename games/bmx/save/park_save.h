#pragma once

// Saving a park and loading one (park_save.cpp).

#include "bmx/save/park_buffer.h"
#include "ee/plain.h"
#include "ee/readable.h"

#include <cstdint>

namespace game::bmx
{
    // At most this many park saves are listed (the list's room; Aggressive Inline's 550).
    constexpr uint32_t kMostParkSaves = 50;

    // (park_save.cpp)
    void findUnusedParkName(ee::Ptr<char> name, uint32_t room);
    void freeParkFileList();
    void loadParkSaveIcon();
    // The park's save client registered with the save system (the game's start).
    void registerParkSaveClient();
    int32_t startParkLoad(ee::Ptr<ParkBuffer> buffer, uint32_t done, uint32_t third);
    int32_t startParkSave(ee::Ptr<ParkBuffer> buffer, uint32_t done);
    void setParkSaveAuthorAndName(ee::Ptr<char> author, ee::Ptr<char> name);
}
