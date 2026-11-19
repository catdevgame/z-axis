#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_boot_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace ps2x::disc_patch
{
    namespace
    {
        struct Owned
        {
            std::string owner;
            Patch patch;
        };

        struct Entry
        {
            // In registration order, each on the previous one's result.
            std::vector<Owned> patches;
            bool made = false;
            std::shared_ptr<const std::vector<uint8_t>> contents;
        };

        struct Registry
        {
            std::mutex mutex;
            std::unordered_map<std::string, Entry> entries;
        };

        // The boot's patches: each game boot registers its own at its
        // install, so a game booted after another in the same process sees
        // its own files, not the other's patches (runtime/ps2_boot_config.h).
        Registry &registry()
        {
            static ps2x::boot::PerBoot<std::unique_ptr<Registry>> ofBoot([] { return std::make_unique<Registry>(); });
            return *ofBoot.get();
        }

        std::string key(std::string_view fileName)
        {
            const size_t slash = fileName.find_last_of("/\\:");
            if (slash != std::string_view::npos)
                fileName = fileName.substr(slash + 1);
            // ISO 9660 names may end in ";1".
            if (const size_t semicolon = fileName.find(';'); semicolon != std::string_view::npos)
                fileName = fileName.substr(0, semicolon);
            std::string out(fileName);
            for (char &c : out)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return out;
        }
    }

    void registerPatch(const std::string &fileName, Patch patch) { registerPatch(fileName, std::move(patch), {}); }

    void registerPatch(const std::string &fileName, Patch patch, std::string owner)
    {
        if (!patch)
            return;
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        Entry &entry = r.entries[key(fileName)];
        entry.patches.push_back({std::move(owner), std::move(patch)});
        entry.made = false;
        entry.contents.reset();
    }

    void removePatches(const std::string &owner)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        for (auto it = r.entries.begin(); it != r.entries.end();)
        {
            std::vector<Owned> &patches = it->second.patches;
            const size_t before = patches.size();
            patches.erase(std::remove_if(patches.begin(), patches.end(),
                                         [&owner](const Owned &patch) { return patch.owner == owner; }),
                          patches.end());
            if (patches.empty())
            {
                it = r.entries.erase(it);
                continue;
            }
            if (patches.size() != before)
            {
                it->second.made = false;
                it->second.contents.reset();
            }
            ++it;
        }
    }

    void clearPatches()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.entries.clear();
    }

    bool hasPatch(std::string_view fileName)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        return r.entries.count(key(fileName)) != 0u;
    }

    std::shared_ptr<const std::vector<uint8_t>> contents(std::string_view fileName,
                                                         const std::function<bool(std::vector<uint8_t> &)> &readOriginal)
    {
        Registry &r = registry();
        const std::string name = key(fileName);
        std::vector<Owned> patches;
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            auto it = r.entries.find(name);
            if (it == r.entries.end())
                return nullptr;
            if (it->second.made)
                return it->second.contents;
            patches = it->second.patches;
        }
        // The file counts as changed if any patch changed it.
        const auto patch = [&patches](std::vector<uint8_t> &bytes)
        {
            bool changed = false;
            for (const Owned &owned : patches)
                changed = owned.patch(bytes) || changed;
            return changed;
        };
        // Made without the lock: a patch may open other game files (which
        // asks this registry) while it runs.
        std::shared_ptr<const std::vector<uint8_t>> made;
        std::vector<uint8_t> bytes;
        if (!readOriginal || !readOriginal(bytes))
        {
            std::fprintf(stderr, "[disc-patch] %s: cannot read the original\n", name.c_str());
        }
        else if (patch(bytes))
        {
            std::fprintf(stderr, "[disc-patch] %s: patched (%zu bytes)\n", name.c_str(), bytes.size());
            made = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
        }
        else
        {
            std::fprintf(stderr, "[disc-patch] %s: left unchanged\n", name.c_str());
        }
        std::lock_guard<std::mutex> lock(r.mutex);
        auto it = r.entries.find(name);
        if (it == r.entries.end())
            return made;
        // Two threads may both have made it; the first one kept wins.
        if (!it->second.made)
        {
            it->second.made = true;
            it->second.contents = std::move(made);
        }
        return it->second.contents;
    }
}
