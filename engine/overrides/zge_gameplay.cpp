// The gameplay pages' host side (zge_gameplay.h): gameplay.ini, the rows,
// the shortcut keys and the game values the options change.

#include "zge_gameplay.h"

#include "machine.h"
#include "runtime/play_recording.h"
#include "runtime/pad.h"
#include "runtime/session.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace zge_host::gameplay
{
    namespace st = zx::settings;

    void Options::define(const OptionInfo *options, int count, std::string header)
    {
        info_.assign(options, options + count);
        values_ = std::make_unique<std::atomic<int>[]>(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
            values_[static_cast<size_t>(i)] = options[i].defaultValue;
        header_ = std::move(header);
    }

    void Options::load()
    {
        for (int i = 0; i < count(); ++i)
            set(i, info(i).defaultValue);
        if (file_.empty())
            return;
        std::ifstream in(file_, std::ios::binary);
        std::string line, section;
        while (std::getline(in, line))
        {
            if (const size_t comment = line.find_first_of(";#"); comment != std::string::npos)
                line.erase(comment);
            line.erase(0, line.find_first_not_of(" \t\r"));
            line.erase(line.find_last_not_of(" \t\r") + 1);
            if (line.empty())
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            std::string name = line.substr(0, eq);
            name.erase(name.find_last_not_of(" \t") + 1);
            const std::string value = line.substr(line.find_first_not_of(" \t", eq + 1) == std::string::npos
                                                      ? line.size()
                                                      : line.find_first_not_of(" \t", eq + 1));
            const std::string key = section + "." + name;
            for (int i = 0; i < count(); ++i)
                if (key == info(i).key || (info(i).formerKey && key == info(i).formerKey))
                {
                    char *end = nullptr;
                    const long parsed = std::strtol(value.c_str(), &end, 10);
                    if (end != value.c_str())
                        set(i, static_cast<int>(parsed));
                    else
                        std::cerr << "[gameplay] " << file_.filename().string() << ": bad value for " << key << std::endl;
                }
        }
    }

    void Options::save()
    {
        std::lock_guard<std::mutex> lock(fileMutex_);
        if (file_.empty())
            return;
        std::ostringstream out;
        out << header_;
        std::string section;
        for (int i = 0; i < count(); ++i)
        {
            const std::string key = info(i).key;
            const std::string s = key.substr(0, key.find('.'));
            if (s != section)
            {
                out << (section.empty() ? "" : "\n") << "[" << s << "]\n";
                section = s;
            }
            out << key.substr(key.find('.') + 1) << " = " << value(i) << "\n";
        }
        std::error_code ec;
        std::filesystem::create_directories(file_.parent_path(), ec);
        const std::filesystem::path temp = file_.string() + ".tmp";
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            file << out.str();
            if (!file)
            {
                std::cerr << "[gameplay] cannot write " << temp.string() << std::endl;
                return;
            }
        }
        std::filesystem::rename(temp, file_, ec);
    }

    void Options::takeRowValues()
    {
        for (int i = 0; i < count(); ++i)
            set(i, st::value(info(i).key, info(i).defaultValue));
    }

    st::Setting Options::row(int option, const char *label, const char *help)
    {
        st::Setting setting;
        setting.key = info(option).key;
        setting.label = label;
        setting.help = help;
        setting.current = [this, option]() { return value(option); };
        setting.apply = [this, option](int v)
        {
            set(option, v);
            save();
        };
        return setting;
    }

    st::Setting Options::rangeRow(int option, const char *label, int lo, int hi, int step, const char *unit,
                                  const char *help)
    {
        st::Setting setting = row(option, label, help);
        setting.kind = st::Kind::Range;
        setting.minValue = lo;
        setting.maxValue = hi;
        setting.step = step;
        setting.unit = unit;
        return setting;
    }

    st::Setting Options::choiceRow(int option, const char *label, std::vector<std::string> choices, const char *help)
    {
        st::Setting setting = row(option, label, help);
        setting.kind = st::Kind::Choice;
        setting.choices = std::move(choices);
        return setting;
    }

    void Hotkeys::poll()
    {
        for (int i = 0; i < ps2_host_input::kHotkeyCount; ++i)
        {
            const uint32_t presses = PSPadBackend::hotkeyPresses(i);
            pressed_[i] = presses != seen_[i];
            seen_[i] = presses;
        }
    }

    bool sameFloat(float a, float b)
    {
        uint32_t x = 0, y = 0;
        std::memcpy(&x, &a, sizeof(x));
        std::memcpy(&y, &b, sizeof(y));
        return x == y;
    }

    float readFloat(const uint8_t *rdram, uint32_t address)
    {
        float value = 0.0f;
        if (const uint8_t *p = getConstMemPtr(rdram, address))
            std::memcpy(&value, p, sizeof(value));
        return value;
    }

    void writeFloat(uint8_t *rdram, uint32_t address, float value)
    {
        if (uint8_t *p = getMemPtr(rdram, address))
            std::memcpy(p, &value, sizeof(value));
    }

    void takeGameValue(const uint8_t *rdram, GameFloat &value, uint32_t address)
    {
        if (value.address != 0u || address == 0u)
            return;
        value.address = address;
        value.game = readFloat(rdram, address);
    }

    void apply(uint8_t *rdram, GameFloat &value, float want)
    {
        if (!value.address)
            return;
        if (!sameFloat(want, value.game))
        {
            if (!sameFloat(readFloat(rdram, value.address), want))
                writeFloat(rdram, value.address, want);
            value.written = true;
        }
        else if (value.written)
        {
            writeFloat(rdram, value.address, value.game);
            value.written = false;
        }
    }

    // ---- Clean Top Screen ----------------------------------------------------------

    namespace
    {
        struct CleanTop
        {
            std::atomic<uint32_t> leftOut{0};
            std::atomic<bool> told[static_cast<size_t>(panel::HudPiece::Count)]{};
        };
        CleanTop &cleanTop() { return zx::session::state<CleanTop>(); }
    }

    bool leftOffTopScreen(const uint8_t *rdram, uint32_t widget, std::span<const TopScreenPiece> pieces, NodeNamed named,
                          NodeParent parent, const char *tag)
    {
        if (!widget)
            return false;
        for (const TopScreenPiece &p : pieces)
        {
            if (!named(rdram, widget, p.widget))
                continue;
            const uint32_t group = parent(rdram, widget);
            const uint32_t screen = group ? parent(rdram, group) : 0u;
            if (!group || !screen || !named(rdram, group, p.group) || !named(rdram, screen, p.screen))
                continue;
            const auto key = static_cast<uint32_t>(p.piece);
            if (!play_recording::read(play_recording::Channel::TopScreenHud, panel::hiddenOnTop(p.piece), key))
                return false;
            CleanTop &c = cleanTop();
            c.leftOut.fetch_add(1u, std::memory_order_relaxed);
            if (!c.told[key].exchange(true))
                std::cout << "[" << tag << "] Clean Top Screen: " << p.screen << "." << p.group << "." << p.widget
                          << " left off the top screen" << std::endl;
            return true;
        }
        return false;
    }

    uint32_t takeLeftOffTopScreen()
    {
        return cleanTop().leftOut.exchange(0u);
    }
}
