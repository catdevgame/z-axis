#include "runtime/gs/gs_quality.h"

#include "runtime/ps2_session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>

namespace ps2x::gs_quality
{
    namespace
    {
        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::vector<OptionInfo> makeOptions()
        {
            std::vector<OptionInfo> o;
            o.push_back({Option::AntiAliasing, "display.anti_aliasing", "anti_aliasing", "PS2X_GS_AA", "Anti-Aliasing",
                         "Smooths jagged edges.", {"off", "fxaa"}, {"Off", "FXAA"}, 1});
            o.push_back({Option::Anisotropy, "display.texture_filtering", "texture_filtering", "PS2X_GS_ANISO",
                         "Texture Filtering", "Sharper ground at an angle.",
                         {"off", "2x", "4x", "8x", "16x"}, {"Off", "2x", "4x", "8x", "16x"}, 4});
            o.push_back({Option::Fog, "display.fog", "fog", "PS2X_NATIVE_FOG", "Fog",
                         "Smooth fog without bands.", {"original", "smooth"},
                         {"Original", "Smooth"}, 1});
            o.push_back({Option::Shadows, "display.shadows", "shadows", "PS2X_NATIVE_SHADOWS", "Shadows",
                         "Sharp draws the shadow in one piece.", {"original", "sharp"},
                         {"Original", "Sharp"}, 1});
            o.push_back({Option::DepthPrecision, "display.depth_precision", "depth_precision", "PS2X_GS_PRECISE_DEPTH",
                         "Depth Precision", "High stops far surfaces flickering.",
                         {"original", "high"}, {"Original", "High"}, 1});
            o.push_back({Option::Perspective, "display.perspective_correct", "perspective_correct", "PS2X_NATIVE_PERSPECTIVE",
                         "Perspective Correction", "Textures without the PS2's warping.",
                         {"off", "on"}, {"Off", "On"}, 1});
            o.push_back({Option::Lighting, "display.character_lighting", "character_lighting", "PS2X_NATIVE_LIGHTING",
                         "Character Lighting", "People lit as on PS2 or Xbox.",
                         {"ps2", "xbox"}, {"PS2", "Xbox"}, 1});
            // Fast: phones and handhelds (Android) by default, exact elsewhere (GPU_BACKEND.md, PS2X_GS_DEPTH16).
#if defined(__ANDROID__)
            constexpr int kFastDepthDefault = 1;
#else
            constexpr int kFastDepthDefault = 0;
#endif
            o.push_back({Option::DepthTest, "display.depth_test", "depth_test", "PS2X_GS_DEPTH16", "Depth Test",
                         "Fast suits phones, a few pixels differ.",
                         {"exact", "fast"}, {"Exact", "Fast"}, kFastDepthDefault});
            return o;
        }

        std::array<std::atomic<int>, kOptionCount> &values()
        {
            static std::array<std::atomic<int>, kOptionCount> v{};
            static const bool init = []()
            {
                for (const OptionInfo &i : options())
                    v[static_cast<size_t>(i.option)].store(i.defaultValue);
                return true;
            }();
            (void)init;
            return v;
        }
    }

    const std::vector<OptionInfo> &options()
    {
        static const std::vector<OptionInfo> o = makeOptions();
        return o;
    }

    std::array<int, kOptionCount> defaults()
    {
        std::array<int, kOptionCount> d{};
        for (const OptionInfo &i : options())
            d[static_cast<size_t>(i.option)] = i.defaultValue;
        return d;
    }

    const OptionInfo &info(Option option)
    {
        return options()[static_cast<size_t>(option)];
    }

    int get(Option option)
    {
        return values()[static_cast<size_t>(option)].load(std::memory_order_relaxed);
    }

    void set(Option option, int value)
    {
        const int top = static_cast<int>(info(option).choices.size()) - 1;
        values()[static_cast<size_t>(option)].store(std::clamp(value, 0, top), std::memory_order_relaxed);
    }

    bool parseValue(Option option, const std::string &text, int &out)
    {
        const OptionInfo &i = info(option);
        const std::string v = lower(text);
        for (size_t k = 0; k < i.choices.size(); ++k)
            if (v == i.choices[k])
            {
                out = static_cast<int>(k);
                return true;
            }
        // Toggles also take true/false, on/off.
        if (i.choices.size() == 2u && (v == "true" || v == "on" || v == "false"))
        {
            out = v == "false" ? 0 : 1;
            return true;
        }
        if (!v.empty() && std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }))
        {
            const long n = std::strtol(v.c_str(), nullptr, 10);
            if (n >= 0 && n < static_cast<long>(i.choices.size()))
            {
                out = static_cast<int>(n);
                return true;
            }
        }
        return false;
    }

    std::string valueName(Option option, int value)
    {
        const OptionInfo &i = info(option);
        return i.choices[static_cast<size_t>(std::clamp(value, 0, static_cast<int>(i.choices.size()) - 1))];
    }

    void applyEnvironment()
    {
        for (const OptionInfo &i : options())
        {
            const char *e = std::getenv(i.env);
            int v = 0;
            if (e && e[0] && parseValue(i.option, e, v))
                set(i.option, v);
        }
    }

    void addSettings(std::function<void(Option, int)> save, bool sessionRows)
    {
        namespace st = ps2x::settings;
        for (const OptionInfo &i : options())
        {
            if (st::has(i.settingKey))
                continue;
            st::Setting s;
            s.key = i.settingKey;
            s.label = i.label;
            s.help = i.help;
            s.kind = st::Kind::Choice;
            s.choices = i.labels;
            s.maxValue = static_cast<int>(i.labels.size()) - 1;
            // The depth test is chosen when the game starts (the page adds its note).
            s.restart = i.option == Option::DepthTest;
            const Option option = i.option;
            s.current = [option]() { return get(option); };
            s.apply = [option, save](int value)
            {
                set(option, value);
                if (save)
                    save(option, get(option));
            };
            if (sessionRows)
                ps2x::session::addSetting(s);
            else
                st::add(s);
        }
    }
}
