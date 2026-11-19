#pragma once

// Dolphin's RVZ disc images (rvz_image.cpp), for collection/extras/
// disc_image.h's openDiscImage.

#include "collection/extras/disc_image.h"

namespace ps2x::app::extras
{
    std::unique_ptr<DiscImage> openRvzImage(std::unique_ptr<ImageFile> file, std::string *error);
}
