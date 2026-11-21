#include "collection/save_profiles.h"

#include "runtime/save_store.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace zx::collection
{
    namespace
    {
        std::string lowerName(const std::string &profile)
        {
            std::string out = zx::saves::safeFileName(profile, kDefaultProfile);
            for (char &c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        bool sameProfile(const std::string &a, const std::string &b) { return lowerName(a) == lowerName(b); }

        std::filesystem::path profileFile(const std::filesystem::path &savesDir, const std::string &profile, const char *extension)
        {
            return savesDir / (zx::saves::safeFileName(profile, kDefaultProfile) + extension);
        }
    }

    std::filesystem::path profileSave(const std::filesystem::path &savesDir, const std::string &profile)
    {
        return profileFile(savesDir, profile, ".sav");
    }

    std::filesystem::path profilePlaytime(const std::filesystem::path &savesDir, const std::string &profile)
    {
        return profileFile(savesDir, profile, ".playtime");
    }

    std::vector<std::string> listProfiles(const std::filesystem::path &savesDir, const std::vector<std::string> &named)
    {
        std::vector<std::string> profiles;
        auto add = [&profiles](const std::string &profile)
        {
            if (profile.empty() || std::any_of(profiles.begin(), profiles.end(), [&](const std::string &p) { return sameProfile(p, profile); }))
                return;
            profiles.push_back(profile);
        };
        std::error_code ec;
        for (std::filesystem::directory_iterator it(savesDir, ec), end; !ec && it != end; it.increment(ec))
        {
            std::string extension = it->path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (extension == ".sav" && !sameProfile(it->path().stem().string(), kBeforeProfilesSave) && it->is_regular_file(ec))
                add(it->path().stem().string());
        }
        for (const std::string &profile : named)
            add(profile);
        std::sort(profiles.begin(), profiles.end(),
                  [](const std::string &a, const std::string &b)
                  {
                      const bool aDefault = sameProfile(a, kDefaultProfile), bDefault = sameProfile(b, kDefaultProfile);
                      if (aDefault != bDefault)
                          return aDefault;
                      return lowerName(a) < lowerName(b);
                  });
        return profiles;
    }

    std::string freeProfileName(const std::vector<std::string> &profiles, const std::string &base)
    {
        auto taken = [&profiles](const std::string &name)
        { return std::any_of(profiles.begin(), profiles.end(), [&](const std::string &p) { return sameProfile(p, name); }); };
        if (!taken(base))
            return base;
        for (int n = 2;; ++n)
            if (const std::string name = base + " " + std::to_string(n); !taken(name))
                return name;
    }

    std::string copyProfile(const std::filesystem::path &savesDir, const std::string &profile, std::string *error)
    {
        auto fail = [error](std::string why)
        {
            if (error)
                *error = std::move(why);
            return std::string();
        };
        const std::filesystem::path from = profileSave(savesDir, profile);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(from, ec))
            return fail(profile + " has no save to copy yet.");
        zx::saves::flushWrites(); // (a save the game is writing now: whole)
        const std::string name = freeProfileName(listProfiles(savesDir, {profile}), profile + " copy");
        // Its files: each <profile>.<kind> (letters' case aside) but a backup, and none of another profile's whose
        // name goes on from this one's ("Career.2"'s files start "Career." too).
        const std::string stem = lowerName(profile);
        std::vector<std::string> longer;
        for (const std::string &other : listProfiles(savesDir, {}))
            if (const std::string otherStem = lowerName(other); otherStem.size() > stem.size() && otherStem.compare(0, stem.size() + 1, stem + ".") == 0)
                longer.push_back(otherStem + ".");
        std::vector<std::pair<std::filesystem::path, std::filesystem::path>> copies;
        for (std::filesystem::directory_iterator it(savesDir, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string file = it->path().filename().string();
            std::string lower = file;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.compare(0, stem.size() + 1, stem + ".") != 0 || (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".bak") == 0) ||
                std::any_of(longer.begin(), longer.end(), [&](const std::string &prefix) { return lower.compare(0, prefix.size(), prefix) == 0; }))
                continue;
            if (std::error_code kind; it->is_regular_file(kind))
                copies.emplace_back(it->path(), savesDir / (zx::saves::safeFileName(name, kDefaultProfile) + file.substr(stem.size())));
        }
        for (size_t i = 0; i < copies.size(); ++i)
        {
            std::filesystem::copy_file(copies[i].first, copies[i].second, std::filesystem::copy_options::none, ec);
            if (ec)
            {
                std::error_code ignored;
                for (size_t made = 0; made < i; ++made)
                    std::filesystem::remove(copies[made].second, ignored);
                return fail("The copy was not written: " + ec.message() + ".");
            }
        }
        return name;
    }

    uint64_t readPlaytime(const std::filesystem::path &file)
    {
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line))
        {
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            std::string key = line.substr(0, equals), value = line.substr(equals + 1);
            key.erase(key.find_last_not_of(" \t\r") + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            value.erase(0, value.find_first_not_of(" \t"));
            if (key != "seconds")
                continue;
            char *end = nullptr;
            const unsigned long long seconds = std::strtoull(value.c_str(), &end, 10);
            return end != value.c_str() ? static_cast<uint64_t>(seconds) : 0u;
        }
        return 0;
    }

    bool addPlaytime(const std::filesystem::path &file, uint64_t seconds)
    {
        const uint64_t total = readPlaytime(file) + seconds;
        std::ostringstream text;
        text << "# The time this save has been played, in seconds (the game's own running time).\n"
             << "seconds = " << total << "\n";
        const std::string bytes = text.str();
        return zx::saves::writeFileAtomic(file, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
    }

    std::string playtimeText(uint64_t seconds)
    {
        const uint64_t minutes = seconds / 60u;
        if (minutes == 0)
            return {};
        const uint64_t hours = minutes / 60u, rest = minutes % 60u;
        if (hours == 0)
            return std::to_string(minutes) + " min";
        return std::to_string(hours) + " h" + (rest ? " " + std::to_string(rest) + " min" : std::string());
    }
}
