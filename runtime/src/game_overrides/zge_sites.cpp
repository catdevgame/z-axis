// Hook places counted (zge_sites.h).

#include "zge_sites.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

namespace zge_host::sites
{
    void seen(const char *site)
    {
        static const bool on = [] { const char *v = std::getenv("PS2X_HOOK_SITES"); return v && v[0] == '1'; }();
        if (!on)
            return;
        static std::mutex mutex;
        static std::map<std::string, uint64_t> counts;
        std::lock_guard<std::mutex> lock(mutex);
        const uint64_t n = ++counts[site];
        bool power = n < 1000u;
        for (uint64_t p = n; power && p > 1u; p /= 10u)
            power = p % 10u == 0u;
        if (power || n % 1000u == 0u)
            std::cout << "[hook-site] " << site << " " << n << std::endl;
    }
}
