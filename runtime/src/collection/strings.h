#pragma once

// Small text helpers the collection's files share (settings.cpp, disc_check.cpp,
// card_import.cpp).

#include "collection/settings.h"

#include <string>
#include <string_view>

namespace zx::app
{
    // (defined in settings.cpp)
    // The environment's value of name through the app's lookup, "" when unset or empty.
    std::string envString(const GetEnv &getEnv, const char *name);
    // Without leading and trailing white space.
    std::string trim(std::string_view text);
    // ASCII lower case.
    std::string lower(std::string text);
}
