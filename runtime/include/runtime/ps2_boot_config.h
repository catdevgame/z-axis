#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// What one game boot is given. The app builds one for each game it boots
// (the game's settings.ini values, its folders, the app's renderer
// settings) and hands it to the runtime it boots the game with
// (PS2Runtime(BootConfig)); a check that boots several games in one
// process gives each its own pad script, dumps and state log. The runtime
// and the game overrides read a value with zx::boot::text(name) where
// they read the environment before:
//
//   1. a value changed while the game runs (zx::boot::change: the
//      player's renderer pick, ZX_CONTROL's "renderer"),
//   2. the process environment, when set and not empty (developer runs,
//      the checkers and the batch tools set ZX_* as before),
//   3. the boot's value,
//   4. the environment as it is (nullptr, or an empty value).
//
// Only the names in BootConfig::names() are carried; the other ZX_*
// switches (traces, dumps of the VU1 and the GS, the clock, pacing, null
// GS) are the process's, read from the environment once.
namespace zx
{
    class BootConfig
    {
    public:
        // The game the boot is of ("ai", "dm2", "bmx"; "" when not known).
        std::string game;

        // false (and nothing kept) for a name BootConfig does not carry.
        bool set(const std::string &name, std::string value);
        // As set, the value winning over the environment (the app's renderer
        // settings, which it has always applied over ZX_GS_SCALE and
        // ZX_GS_FULL_HEIGHT unless the environment picks ZX_GS_BACKEND).
        bool setOverEnvironment(const std::string &name, std::string value);
        void erase(const std::string &name) { m_values.erase(name); }

        // What a boot with this config reads for name (the order above,
        // before any change while it runs); "" when nothing is set.
        std::string text(const std::string &name) const;

        // This config's own value, nullptr if it has none.
        const std::string *find(const std::string &name) const;
        bool overEnvironment(const std::string &name) const;
        [[nodiscard]] bool empty() const { return m_values.empty(); }

        // Every name a BootConfig carries, and whether one is.
        static const std::vector<std::string> &names();
        static bool carries(const std::string &name);

    private:
        struct Value
        {
            std::string text;
            bool overEnvironment = false;
        };
        std::map<std::string, Value> m_values;
    };

    namespace boot
    {
        // A boot starts with config (PS2Runtime's constructor; an empty one:
        // the environment alone). Every PerBoot value is read again, and the
        // game overrides' session before it ends (runtime/ps2_session.h).
        void begin(BootConfig config);
        // The boot ended (~PS2Runtime): an empty config until the next, its
        // session ended.
        void end();

        // The value of name for the boot in progress (see above). The text
        // stays valid for the life of the process.
        const char *text(const char *name);
        // text(name) set and not empty.
        bool has(const char *name);
        // Changes a value of the boot in progress, over the environment
        // (false for a name BootConfig does not carry).
        bool change(const std::string &name, std::string value);

        // The boot in progress: its game, and a number that changes with
        // every boot (PerBoot reads again when it does).
        std::string game();
        uint64_t generation();

        // A boot for the life of an object: PS2Runtime's first member, so
        // its config is in place before any other member is made (the GS
        // renderer reads ZX_GS_BACKEND when it is made) and ends after
        // the last one is gone. One boot at a time.
        class Scope
        {
        public:
            explicit Scope(BootConfig config) { begin(std::move(config)); }
            ~Scope() { end(); }
            Scope(const Scope &) = delete;
            Scope &operator=(const Scope &) = delete;
        };

        // A value read once per boot: a switch a function used to keep in a
        // function static (its first read stood for the whole process).
        // get() reads it again at the first use after a new boot began; boots
        // begin and end only while no game thread runs, so a reference from
        // get() stays good for the boot.
        template <typename T>
        class PerBoot
        {
        public:
            using Reader = T (*)();
            explicit PerBoot(Reader read) : m_read(read) {}
            PerBoot(const PerBoot &) = delete;
            PerBoot &operator=(const PerBoot &) = delete;

            T &get()
            {
                const uint64_t now = generation();
                if (m_generation.load(std::memory_order_acquire) != now)
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_generation.load(std::memory_order_relaxed) != now)
                    {
                        m_value.reset();
                        m_value.emplace(m_read());
                        m_generation.store(now, std::memory_order_release);
                    }
                }
                return *m_value;
            }

        private:
            Reader m_read;
            std::mutex m_mutex;
            std::atomic<uint64_t> m_generation{~uint64_t{0}};
            std::optional<T> m_value;
        };
    }
}
