#include "runtime/ps2_settings.h"
#include "runtime/play_recording.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <utility>

namespace ps2x::settings
{
    namespace
    {
        struct Entry
        {
            Setting setting;
            int value = 0;
        };

        struct Registry
        {
            std::mutex mutex;
            std::vector<Entry> entries;
            std::vector<std::function<void()>> posted;
            // Changes not yet applied (key, value), in the order they were
            // made: a key changed again moves to the end with its new value.
            std::vector<std::pair<std::string, int>> pendingApply;
            std::function<void()> openHandler;
            std::string notice;
            std::chrono::steady_clock::time_point noticeUntil{};
            // Changes made (sets and activations) and, of them, those
            // runPending has run, for the game's page to wait on.
            uint64_t changes = 0;
            uint64_t changesRun = 0;
            bool frameLoop = false;
            std::condition_variable changesRunChanged;

            Entry *find(const std::string &key)
            {
                for (Entry &entry : entries)
                    if (entry.setting.key == key)
                        return &entry;
                return nullptr;
            }

            bool pending(const std::string &key) const
            {
                return std::any_of(pendingApply.begin(), pendingApply.end(),
                                   [&](const auto &change) { return change.first == key; });
            }
        };

        Registry &registry()
        {
            static Registry instance;
            return instance;
        }

        // The game's thread reads the registry through the play recording
        // (runtime/play_recording.h): what a Settings page shows, its rows,
        // names, values and texts, is the host's state (the window, the
        // renderer, the volume, the settings files), so a replay hands the
        // game what the recorded game read. One stream per read and setting.
        bool gameReads() { return play_recording::active() && play_recording::onGameThread(); }

        uint32_t streamOf(const char *read, const std::string &key)
        {
            uint32_t hash = 2166136261u; // FNV-1a
            auto add = [&hash](char c)
            {
                hash ^= static_cast<uint8_t>(c);
                hash *= 16777619u;
            };
            for (const char *c = read; *c; ++c)
                add(*c);
            add('\0');
            for (char c : key)
                add(c);
            return hash;
        }

        std::string recordedText(const char *read, const std::string &key, std::string text)
        {
            play_recording::readText(play_recording::Channel::Setting, streamOf(read, key), text);
            return text;
        }

        // A list of texts as one text (none has these separators).
        constexpr char kField = '\x1f';
        constexpr char kItem = '\x1e';

        std::string joined(const std::vector<std::string> &items, char separator)
        {
            std::string out;
            for (size_t i = 0; i < items.size(); ++i)
                out += (i ? std::string(1, separator) : std::string()) + items[i];
            return out;
        }

        std::vector<std::string> split(const std::string &text, char separator)
        {
            std::vector<std::string> out;
            size_t start = 0;
            for (size_t i = 0; i <= text.size(); ++i)
                if (i == text.size() || text[i] == separator)
                {
                    out.push_back(text.substr(start, i - start));
                    start = i + 1;
                }
            return out;
        }

        // describe()'s answer as a text: found, then the setting's fields
        // (its callbacks stay the live setting's: the same key's).
        std::string describedText(bool found, const Setting &setting)
        {
            if (!found)
                return "0";
            return joined({"1", setting.label, std::to_string(static_cast<int>(setting.kind)),
                           joined(setting.choices, kItem), std::to_string(setting.minValue),
                           std::to_string(setting.maxValue), std::to_string(setting.step), setting.unit,
                           setting.restart ? "1" : "0", setting.help},
                          kField);
        }

        bool describedFrom(const std::string &text, Setting &setting)
        {
            const std::vector<std::string> fields = split(text, kField);
            if (fields.size() != 10 || fields[0] != "1")
                return false;
            setting.label = fields[1];
            setting.kind = static_cast<Kind>(std::atoi(fields[2].c_str()));
            setting.choices = fields[3].empty() ? std::vector<std::string>{} : split(fields[3], kItem);
            setting.minValue = std::atoi(fields[4].c_str());
            setting.maxValue = std::atoi(fields[5].c_str());
            setting.step = std::atoi(fields[6].c_str());
            setting.unit = fields[7];
            setting.restart = fields[8] == "1";
            setting.help = fields[9];
            return true;
        }

        int clampValue(const Setting &setting, int value)
        {
            switch (setting.kind)
            {
            case Kind::Toggle:
                return value != 0 ? 1 : 0;
            case Kind::Choice:
                return std::clamp(value, 0, std::max(0, static_cast<int>(setting.choices.size()) - 1));
            case Kind::Action:
                return 0;
            case Kind::Range:
            default:
                return std::clamp(value, setting.minValue, std::max(setting.minValue, setting.maxValue));
            }
        }
    }

    void add(Setting setting)
    {
        int initial = setting.current ? setting.current() : 0;
        if (setting.kind == Kind::Toggle && setting.choices.size() != 2)
            setting.choices = {"Off", "On"};
        initial = clampValue(setting, initial);
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (Entry *existing = r.find(setting.key))
        {
            existing->setting = std::move(setting);
            existing->value = initial;
            return;
        }
        r.entries.push_back(Entry{std::move(setting), initial});
    }

    void remove(const std::string &key)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.entries.erase(std::remove_if(r.entries.begin(), r.entries.end(),
                                       [&](const Entry &entry) { return entry.setting.key == key; }),
                        r.entries.end());
        std::erase_if(r.pendingApply, [&](const auto &change) { return change.first == key; });
    }

    bool has(const std::string &key)
    {
        bool found = false;
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            found = r.find(key) != nullptr;
        }
        if (gameReads())
            found = play_recording::read(play_recording::Channel::Setting, static_cast<uint8_t>(found ? 1 : 0),
                                         streamOf("has", key)) != 0;
        return found;
    }

    std::vector<std::string> keys(const std::string &prefix)
    {
        std::vector<std::string> out;
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            for (const Entry &entry : r.entries)
                if (entry.setting.key.rfind(prefix, 0) == 0)
                    out.push_back(entry.setting.key);
        }
        if (gameReads())
        {
            const std::string text = recordedText("keys", prefix, joined(out, kItem));
            out = text.empty() ? std::vector<std::string>{} : split(text, kItem);
        }
        return out;
    }

    bool describe(const std::string &key, Setting &out)
    {
        bool found = false;
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            if (const Entry *entry = r.find(key))
            {
                out = entry->setting;
                found = true;
            }
        }
        if (gameReads())
        {
            found = describedFrom(recordedText("describe", key, describedText(found, out)), out);
            if (found)
                out.key = key;
        }
        return found;
    }

    int value(const std::string &key, int fallback)
    {
        int current = fallback;
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            if (const Entry *entry = r.find(key))
                current = entry->value;
        }
        if (gameReads())
            current = play_recording::read(play_recording::Channel::Setting, current, streamOf("value", key));
        return current;
    }

    bool set(const std::string &key, int newValue)
    {
        int value = 0;
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            Entry *entry = r.find(key);
            if (!entry || entry->setting.kind == Kind::Action)
                return false;
            entry->value = clampValue(entry->setting, newValue);
            value = entry->value;
            std::erase_if(r.pendingApply, [&](const auto &change) { return change.first == key; });
            r.pendingApply.emplace_back(key, entry->value);
            ++r.changes;
        }
        // A play recording notes it (the game's own page's changes are its thread's).
        play_recording::onSettingChanged(key, value, play_recording::onGameThread());
        return true;
    }

    bool step(const std::string &key, int delta, bool wrap)
    {
        Setting setting;
        int current = 0;
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            const Entry *entry = r.find(key);
            if (!entry)
                return false;
            setting = entry->setting;
            current = entry->value;
        }
        // (the game's page steps from the value it read)
        if (gameReads())
            current = value(key, current);
        int next = current;
        if (setting.kind == Kind::Range)
        {
            next = current + delta * std::max(1, setting.step);
        }
        else
        {
            const int count = setting.kind == Kind::Toggle ? 2 : static_cast<int>(setting.choices.size());
            next = current + delta;
            if (wrap && count > 0)
                next = ((next % count) + count) % count;
        }
        return set(key, next);
    }

    std::string textFor(const Setting &setting, int value)
    {
        if (setting.format)
            return setting.format(value);
        if (setting.kind == Kind::Action)
            return {};
        if (setting.kind != Kind::Range)
            return value >= 0 && value < static_cast<int>(setting.choices.size()) ? setting.choices[static_cast<size_t>(value)]
                                                                                  : std::string();
        return std::to_string(value) + setting.unit;
    }

    std::string text(const std::string &key)
    {
        Setting setting;
        int current = 0;
        bool found = false;
        {
            Registry &r = registry();
            std::lock_guard<std::mutex> lock(r.mutex);
            if (const Entry *entry = r.find(key))
            {
                setting = entry->setting;
                current = entry->value;
                found = true;
            }
        }
        std::string out = found ? textFor(setting, current) : std::string();
        if (gameReads())
            out = recordedText("text", key, std::move(out));
        return out;
    }

    bool activate(const std::string &key)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        const Entry *entry = r.find(key);
        if (!entry || !entry->setting.activate)
            return false;
        r.posted.push_back(entry->setting.activate);
        ++r.changes;
        return true;
    }

    namespace
    {
        // The game's thread, after a change of its page: until runPending has
        // run every change made so far (or here, with no host frame loop).
        void waitForRun()
        {
            Registry &r = registry();
            std::unique_lock<std::mutex> lock(r.mutex);
            const uint64_t made = r.changes;
            if (!r.frameLoop)
            {
                lock.unlock();
                runPending();
                return;
            }
            r.changesRunChanged.wait(lock, [&]() { return r.changesRun >= made || !r.frameLoop; });
        }
    }

    bool setFromGame(const std::string &key, int value)
    {
        if (!set(key, value))
            return false;
        waitForRun();
        return true;
    }

    bool stepFromGame(const std::string &key, int delta, bool wrap)
    {
        if (!step(key, delta, wrap))
            return false;
        waitForRun();
        return true;
    }

    bool activateFromGame(const std::string &key)
    {
        if (!activate(key))
            return false;
        waitForRun();
        return true;
    }

    void setFrameLoop(bool running)
    {
        Registry &r = registry();
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            r.frameLoop = running;
        }
        r.changesRunChanged.notify_all();
    }

    void setOpenHandler(std::function<void()> handler)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.openHandler = std::move(handler);
    }

    bool requestOpen()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (!r.openHandler)
            return false;
        r.posted.push_back(r.openHandler);
        return true;
    }

    bool hasOpenHandler()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        return static_cast<bool>(r.openHandler);
    }

    void post(std::function<void()> work)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.posted.push_back(std::move(work));
    }

    void notify(const std::string &text, double seconds)
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.notice = text;
        r.noticeUntil = std::chrono::steady_clock::now() +
                        std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
    }

    std::string notice()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        if (r.notice.empty() || std::chrono::steady_clock::now() >= r.noticeUntil)
            return {};
        return r.notice;
    }

    void runPending()
    {
        Registry &r = registry();
        std::vector<std::function<void()>> posted;
        std::vector<std::pair<std::function<void(int)>, int>> applies;
        uint64_t made = 0;
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            made = r.changes;
            posted.swap(r.posted);
            for (const auto &[key, pending] : r.pendingApply)
                if (const Entry *entry = r.find(key); entry && entry->setting.apply)
                    applies.emplace_back(entry->setting.apply, pending);
            r.pendingApply.clear();
        }
        for (auto &work : posted)
            work();
        for (auto &[apply, pending] : applies)
            apply(pending);

        // Values changed elsewhere (or adjusted by the apply).
        std::vector<std::pair<std::string, std::function<int()>>> sources;
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            for (const Entry &entry : r.entries)
                if (entry.setting.current && !r.pending(entry.setting.key))
                    sources.emplace_back(entry.setting.key, entry.setting.current);
        }
        std::vector<std::pair<std::string, int>> fresh;
        fresh.reserve(sources.size());
        for (auto &[key, current] : sources)
            fresh.emplace_back(key, current());
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            for (auto &[key, now] : fresh)
                if (Entry *entry = r.find(key); entry && !r.pending(key))
                    entry->value = clampValue(entry->setting, now);
            r.changesRun = std::max(r.changesRun, made);
        }
        r.changesRunChanged.notify_all();
    }

    void clear()
    {
        Registry &r = registry();
        std::lock_guard<std::mutex> lock(r.mutex);
        r.entries.clear();
        r.posted.clear();
        r.pendingApply.clear();
        r.openHandler = nullptr;
        r.changesRun = r.changes;
    }
}
