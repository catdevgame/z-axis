#include "runtime/host_input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace ps2_host_input
{
    namespace
    {
        struct NamedCode
        {
            const char *name;
            int code;
        };

        // GLFW key numbers (raylib's KEY_*) and mouse buttons. The first
        // name of a code is its canonical name.
        const NamedCode kNamedCodes[] = {
            {"SPACE", 32}, {"APOSTROPHE", 39}, {"COMMA", 44}, {"MINUS", 45}, {"PERIOD", 46}, {"SLASH", 47},
            {"0", 48}, {"1", 49}, {"2", 50}, {"3", 51}, {"4", 52}, {"5", 53}, {"6", 54}, {"7", 55}, {"8", 56}, {"9", 57},
            {"SEMICOLON", 59}, {"EQUAL", 61},
            {"A", 65}, {"B", 66}, {"C", 67}, {"D", 68}, {"E", 69}, {"F", 70}, {"G", 71}, {"H", 72}, {"I", 73},
            {"J", 74}, {"K", 75}, {"L", 76}, {"M", 77}, {"N", 78}, {"O", 79}, {"P", 80}, {"Q", 81}, {"R", 82},
            {"S", 83}, {"T", 84}, {"U", 85}, {"V", 86}, {"W", 87}, {"X", 88}, {"Y", 89}, {"Z", 90},
            {"LEFT_BRACKET", 91}, {"BACKSLASH", 92}, {"RIGHT_BRACKET", 93}, {"GRAVE", 96},
            {"ESCAPE", 256}, {"ENTER", 257}, {"TAB", 258}, {"BACKSPACE", 259}, {"INSERT", 260}, {"DELETE", 261},
            {"RIGHT", 262}, {"LEFT", 263}, {"DOWN", 264}, {"UP", 265}, {"PAGE_UP", 266}, {"PAGE_DOWN", 267},
            {"HOME", 268}, {"END", 269}, {"CAPS_LOCK", 280}, {"SCROLL_LOCK", 281}, {"NUM_LOCK", 282},
            {"PRINT_SCREEN", 283}, {"PAUSE", 284},
            {"F1", 290}, {"F2", 291}, {"F3", 292}, {"F4", 293}, {"F5", 294}, {"F6", 295}, {"F7", 296}, {"F8", 297},
            {"F9", 298}, {"F10", 299}, {"F11", 300}, {"F12", 301},
            {"KP_0", 320}, {"KP_1", 321}, {"KP_2", 322}, {"KP_3", 323}, {"KP_4", 324}, {"KP_5", 325}, {"KP_6", 326},
            {"KP_7", 327}, {"KP_8", 328}, {"KP_9", 329}, {"KP_DECIMAL", 330}, {"KP_DIVIDE", 331},
            {"KP_MULTIPLY", 332}, {"KP_SUBTRACT", 333}, {"KP_ADD", 334}, {"KP_ENTER", 335}, {"KP_EQUAL", 336},
            {"LEFT_SHIFT", 340}, {"LEFT_CONTROL", 341}, {"LEFT_ALT", 342}, {"LEFT_SUPER", 343},
            {"RIGHT_SHIFT", 344}, {"RIGHT_CONTROL", 345}, {"RIGHT_ALT", 346}, {"RIGHT_SUPER", 347}, {"MENU", 348},
            {"MOUSE_LEFT", kMouseCodeBase + 0}, {"MOUSE_RIGHT", kMouseCodeBase + 1},
            {"MOUSE_MIDDLE", kMouseCodeBase + 2}, {"MOUSE_SIDE", kMouseCodeBase + 3},
            {"MOUSE_EXTRA", kMouseCodeBase + 4}, {"MOUSE_FORWARD", kMouseCodeBase + 5},
            {"MOUSE_BACK", kMouseCodeBase + 6},
            // Aliases.
            {"ESC", 256}, {"RETURN", 257}, {"BACKQUOTE", 96}, {"TILDE", 96},
            {"SHIFT", 340}, {"CTRL", 341}, {"CONTROL", 341}, {"ALT", 342}, {"OPTION", 342},
            {"LEFT_OPTION", 342}, {"RIGHT_OPTION", 346}, {"CMD", 343}, {"COMMAND", 343}, {"LEFT_COMMAND", 343},
            {"RIGHT_COMMAND", 347}, {"LEFT_CTRL", 341}, {"RIGHT_CTRL", 345},
            {"MOUSE1", kMouseCodeBase + 0}, {"MOUSE2", kMouseCodeBase + 1}, {"MOUSE3", kMouseCodeBase + 2},
            {"MOUSE4", kMouseCodeBase + 3}, {"MOUSE5", kMouseCodeBase + 4},
        };

        const char *const kButtonNames[kPadButtonCount] = {
            "select", "l3", "r3", "start", "up", "right", "down", "left",
            "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};

        const char *const kStickNames[kStickDirectionCount] = {
            "left_up", "left_down", "left_left", "left_right",
            "right_up", "right_down", "right_left", "right_right"};

        const NamedCode kGamepadNames[] = {
            {"SOUTH", kCross}, {"EAST", kCircle}, {"WEST", kSquare}, {"NORTH", kTriangle},
            {"L1", kL1}, {"R1", kR1}, {"L2", kL2}, {"R2", kR2}, {"START", kStart}, {"SELECT", kSelect},
            {"L3", kL3}, {"R3", kR3}, {"DPAD_UP", kUp}, {"DPAD_DOWN", kDown}, {"DPAD_LEFT", kLeft},
            {"DPAD_RIGHT", kRight},
            // Aliases.
            {"A", kCross}, {"B", kCircle}, {"X", kSquare}, {"Y", kTriangle}, {"CROSS", kCross},
            {"CIRCLE", kCircle}, {"SQUARE", kSquare}, {"TRIANGLE", kTriangle}, {"LB", kL1}, {"RB", kR1},
            {"LT", kL2}, {"RT", kR2}, {"BACK", kSelect}, {"LEFT_STICK", kL3}, {"RIGHT_STICK", kR3},
            {"UP", kUp}, {"DOWN", kDown}, {"LEFT", kLeft}, {"RIGHT", kRight},
        };

        std::string upper(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return text;
        }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        std::string trim(const std::string &text)
        {
            size_t begin = 0;
            size_t end = text.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return text.substr(begin, end - begin);
        }

        std::vector<std::string> split(const std::string &text, char separator)
        {
            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos <= text.size())
            {
                size_t next = text.find(separator, pos);
                if (next == std::string::npos)
                    next = text.size();
                parts.push_back(text.substr(pos, next - pos));
                pos = next + 1;
            }
            return parts;
        }

        bool parseNumber(const std::string &text, float &out)
        {
            const std::string value = trim(text);
            if (value.empty())
                return false;
            char *end = nullptr;
            const float parsed = std::strtof(value.c_str(), &end);
            if (!end || *end != '\0' || !std::isfinite(parsed))
                return false;
            out = parsed;
            return true;
        }

        bool parseBool(const std::string &text, bool &out)
        {
            const std::string value = lower(trim(text));
            if (value == "true" || value == "yes" || value == "on" || value == "1")
                out = true;
            else if (value == "false" || value == "no" || value == "off" || value == "0")
                out = false;
            else
                return false;
            return true;
        }

        // "NAME" or "NAME*value", comma separated; empty = no bindings.
        bool parseBindings(const std::string &text, std::vector<Binding> &out, std::vector<std::string> *bad)
        {
            out.clear();
            bool ok = true;
            for (const std::string &part : split(text, ','))
            {
                const std::string item = trim(part);
                if (item.empty())
                    continue;
                Binding binding;
                std::string name = item;
                const size_t star = item.find('*');
                if (star != std::string::npos)
                {
                    name = trim(item.substr(0, star));
                    float value = 0.0f;
                    if (!parseNumber(item.substr(star + 1), value) || value <= 0.0f || value > 1.0f)
                    {
                        if (bad)
                            bad->push_back(item);
                        ok = false;
                        continue;
                    }
                    binding.value = value;
                }
                binding.code = hostCodeByName(name);
                if (binding.code < 0)
                {
                    if (bad)
                        bad->push_back(item);
                    ok = false;
                    continue;
                }
                out.push_back(binding);
            }
            return ok;
        }

        // "<n>" seconds or "<n>v" vsyncs, in vsyncs (as ZX_PAD_SCRIPT).
        double parseScriptTime(const std::string &text)
        {
            constexpr double kVsyncsPerSecond = 59.94;
            if (!text.empty() && (text.back() == 'v' || text.back() == 'V'))
                return std::atof(text.substr(0, text.size() - 1).c_str());
            return std::atof(text.c_str()) * kVsyncsPerSecond;
        }

        bool active(const std::vector<Binding> &bindings, const HostCodeSet &down, float &value)
        {
            bool any = false;
            value = 0.0f;
            for (const Binding &binding : bindings)
            {
                if (binding.code >= 0 && binding.code < kHostCodeCount && down.test(static_cast<size_t>(binding.code)))
                {
                    any = true;
                    value = std::max(value, binding.value);
                }
            }
            return any;
        }

        bool anyPressed(const std::vector<Binding> &bindings, const HostCodeSet &pressed)
        {
            for (const Binding &binding : bindings)
                if (binding.code >= 0 && binding.code < kHostCodeCount && pressed.test(static_cast<size_t>(binding.code)))
                    return true;
            return false;
        }
    }

    int hostCodeByName(const std::string &name)
    {
        const std::string key = upper(trim(name));
        for (const NamedCode &entry : kNamedCodes)
            if (key == entry.name)
                return entry.code;
        return -1;
    }

    std::string hostCodeName(int code)
    {
        for (const NamedCode &entry : kNamedCodes)
            if (entry.code == code)
                return entry.name;
        return {};
    }

    std::string hostCodeLabel(int code)
    {
        const std::string name = hostCodeName(code);
        if (name.empty())
            return {};
        static const struct
        {
            const char *name;
            const char *label;
        } kLabels[] = {
            {"SPACE", "Space"}, {"ENTER", "Enter"}, {"ESCAPE", "Esc"}, {"TAB", "Tab"}, {"BACKSPACE", "Backspace"},
            {"UP", "Up"}, {"DOWN", "Down"}, {"LEFT", "Left"}, {"RIGHT", "Right"},
            {"LEFT_SHIFT", "Shift"}, {"RIGHT_SHIFT", "Shift"}, {"LEFT_CONTROL", "Ctrl"}, {"RIGHT_CONTROL", "Ctrl"},
#if defined(__APPLE__)
            {"LEFT_ALT", "Option"}, {"RIGHT_ALT", "Option"}, {"LEFT_SUPER", "Cmd"}, {"RIGHT_SUPER", "Cmd"},
#else
            {"LEFT_ALT", "Alt"}, {"RIGHT_ALT", "Alt"}, {"LEFT_SUPER", "Win"}, {"RIGHT_SUPER", "Win"},
#endif
            {"GRAVE", "`"}, {"MINUS", "-"}, {"EQUAL", "="}, {"LEFT_BRACKET", "["}, {"RIGHT_BRACKET", "]"},
            {"BACKSLASH", "\\"}, {"SEMICOLON", ";"}, {"APOSTROPHE", "'"}, {"COMMA", ","}, {"PERIOD", "."},
            {"SLASH", "/"}, {"INSERT", "Ins"}, {"DELETE", "Del"}, {"HOME", "Home"}, {"END", "End"},
            {"PAGE_UP", "PgUp"}, {"PAGE_DOWN", "PgDn"}, {"CAPS_LOCK", "Caps"},
            {"MOUSE_LEFT", "Left Click"}, {"MOUSE_RIGHT", "Right Click"}, {"MOUSE_MIDDLE", "Middle Click"},
            {"MOUSE_SIDE", "Mouse 4"}, {"MOUSE_EXTRA", "Mouse 5"},
        };
        for (const auto &entry : kLabels)
            if (name == entry.name)
                return entry.label;
        if (name.rfind("KP_", 0) == 0)
            return "Num " + name.substr(3);
        if (name.rfind("MOUSE_", 0) == 0)
            return "Mouse " + name.substr(6);
        return name;
    }

    std::array<std::vector<int>, kPadButtonCount> defaultGamepadButtons()
    {
        std::array<std::vector<int>, kPadButtonCount> out{};
        for (int i = 0; i < kPadButtonCount; ++i)
            out[static_cast<size_t>(i)] = {i};
        return out;
    }

    std::array<std::vector<int>, kPadButtonCount> gamepadLayoutButtons(GamepadLayout layout)
    {
        std::array<std::vector<int>, kPadButtonCount> out = defaultGamepadButtons();
        if (layout == GamepadLayout::ModernTriggers)
        {
            out[kL1] = {kL2};
            out[kR1] = {kR2};
            out[kL2] = {kL1};
            out[kR2] = {kR1};
        }
        return out;
    }

    GamepadLayout gamepadLayoutOf(const std::array<std::vector<int>, kPadButtonCount> &buttons)
    {
        for (int layout = 0; layout < static_cast<int>(GamepadLayout::Count); ++layout)
            if (buttons == gamepadLayoutButtons(static_cast<GamepadLayout>(layout)))
                return static_cast<GamepadLayout>(layout);
        return GamepadLayout::Count;
    }

    int gamepadButtonByName(const std::string &name)
    {
        const std::string key = upper(trim(name));
        for (const NamedCode &entry : kGamepadNames)
            if (key == entry.name)
                return entry.code;
        return -1;
    }

    std::string gamepadButtonName(int physical)
    {
        for (const NamedCode &entry : kGamepadNames)
            if (entry.code == physical)
                return entry.name;
        return {};
    }

    std::string formatBindings(const std::vector<Binding> &bindings)
    {
        std::string out;
        for (const Binding &binding : bindings)
        {
            const std::string name = hostCodeName(binding.code);
            if (name.empty())
                continue;
            if (!out.empty())
                out += ", ";
            out += name;
            if (binding.value < 1.0f)
            {
                char value[32];
                std::snprintf(value, sizeof(value), "*%g", static_cast<double>(binding.value));
                out += value;
            }
        }
        return out;
    }

    std::string setIniValue(const std::string &text, const std::string &section, const std::string &key,
                            const std::string &value)
    {
        const std::string wantSection = lower(section);
        const std::string wantKey = lower(key);
        const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
        std::vector<std::string> lines;
        {
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos)
                    end = text.size();
                std::string line = text.substr(pos, end - pos);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
                pos = end + 1;
            }
        }
        std::string current;
        int sectionEnd = -1; // index after the last key line of the wanted section
        bool sectionSeen = false;
        for (size_t i = 0; i < lines.size(); ++i)
        {
            std::string body = lines[i];
            const size_t comment = body.find_first_of("#;");
            if (comment != std::string::npos)
                body = body.substr(0, comment);
            body = trim(body);
            if (body.empty())
                continue;
            if (body.front() == '[' && body.back() == ']')
            {
                current = lower(trim(body.substr(1, body.size() - 2)));
                if (current == wantSection)
                {
                    sectionSeen = true;
                    sectionEnd = static_cast<int>(i) + 1;
                }
                continue;
            }
            if (current != wantSection)
                continue;
            sectionEnd = static_cast<int>(i) + 1;
            const size_t eq = body.find('=');
            if (eq == std::string::npos || lower(trim(body.substr(0, eq))) != wantKey)
                continue;
            // Rewrite the value, keep "key   = " and a trailing comment.
            const std::string &line = lines[i];
            const size_t lineEq = line.find('=');
            size_t valueStart = lineEq + 1;
            while (valueStart < line.size() && (line[valueStart] == ' ' || line[valueStart] == '\t'))
                ++valueStart;
            const size_t lineComment = line.find_first_of("#;", valueStart);
            std::string tail;
            if (lineComment != std::string::npos)
            {
                size_t gap = lineComment;
                while (gap > valueStart && (line[gap - 1] == ' ' || line[gap - 1] == '\t'))
                    --gap;
                tail = line.substr(gap);
                const size_t oldLength = gap - valueStart;
                if (oldLength > value.size() && tail.size() > 0)
                    tail.insert(0, std::string(oldLength - value.size(), ' '));
                else if (!tail.empty() && tail[0] != ' ' && tail[0] != '\t')
                    tail.insert(0, " ");
            }
            std::string prefix = line.substr(0, lineEq + 1);
            prefix += value.empty() ? "" : " ";
            lines[i] = prefix + value + tail;
            std::string out;
            for (size_t j = 0; j < lines.size(); ++j)
                out += lines[j] + (j + 1 < lines.size() || (!text.empty() && text.back() == '\n') ? eol : "");
            return out;
        }
        const std::string added = key + " = " + value;
        if (sectionSeen)
            lines.insert(lines.begin() + sectionEnd, added);
        else
        {
            if (!lines.empty() && lines.back().empty())
                lines.pop_back();
            if (!lines.empty())
                lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(added);
            lines.push_back("");
        }
        std::string out;
        for (size_t j = 0; j < lines.size(); ++j)
            out += lines[j] + (j + 1 < lines.size() ? eol : "");
        if (!out.empty() && (text.empty() || text.back() == '\n') && out.back() != '\n')
            out += eol;
        return out;
    }

    const char *padButtonName(int button)
    {
        return (button >= 0 && button < kPadButtonCount) ? kButtonNames[button] : "";
    }

    const char *hotkeyName(int hotkey)
    {
        static const char *const kNames[kHotkeyCount] = {"restart",        "hide_hud",       "screenshot",     "photo_mode",
                                                               "switch_game",    "developer_menu", "next_track",     "previous_track",
                                                               "shuffle",        "practice_point", "bottom_screen"};
        return (hotkey >= 0 && hotkey < kHotkeyCount) ? kNames[hotkey] : "";
    }

    const char *promptStyleName(PromptStyle style)
    {
        switch (style)
        {
        case PromptStyle::Keyboard:
            return "keyboard";
        case PromptStyle::Xbox:
            return "xbox";
        case PromptStyle::PlayStation:
            return "playstation";
        case PromptStyle::Nintendo:
            return "nintendo";
        default:
            return "auto";
        }
    }

    const char *stickDirectionName(int direction)
    {
        return (direction >= 0 && direction < kStickDirectionCount) ? kStickNames[direction] : "";
    }

    uint8_t stickByte(float value)
    {
        const float clamped = std::clamp(value, -1.0f, 1.0f);
        const long byte = std::lround(128.0 + static_cast<double>(clamped) * 127.5);
        return static_cast<uint8_t>(std::clamp<long>(byte, 0, 255));
    }

    uint8_t pressureByte(float value)
    {
        const long byte = std::lround(static_cast<double>(std::clamp(value, 0.0f, 1.0f)) * 255.0);
        return static_cast<uint8_t>(std::clamp<long>(byte, 1, 255));
    }

    // ---- config -------------------------------------------------------------

    std::string HostInputConfig::defaultIniText()
    {
        return gameControls("ai").defaultIni;
    }

    HostInputConfig HostInputConfig::defaults()
    {
        return defaults(defaultIniText());
    }

    HostInputConfig HostInputConfig::defaults(const std::string &iniText)
    {
        HostInputConfig config;
        config.mouseMotion = MouseMotion::None; // set by the ini text below
        config.applyIni(iniText);
        return config;
    }

    void HostInputConfig::applyIni(const std::string &text, std::vector<std::string> *warnings)
    {
        auto warn = [warnings](const std::string &message)
        {
            if (warnings)
                warnings->push_back(message);
        };

        std::string section;
        int lineNumber = 0;
        std::istringstream stream(text);
        std::string rawLine;
        while (std::getline(stream, rawLine))
        {
            ++lineNumber;
            std::string line = rawLine;
            const size_t comment = line.find_first_of("#;");
            if (comment != std::string::npos)
                line = line.substr(0, comment);
            line = trim(line);
            if (line.empty())
                continue;
            const std::string where = "line " + std::to_string(lineNumber) + ": ";
            if (line.front() == '[')
            {
                if (line.back() != ']')
                {
                    warn(where + "bad section '" + line + "'");
                    section.clear();
                    continue;
                }
                section = lower(trim(line.substr(1, line.size() - 2)));
                if (section != "buttons" && section != "sticks" && section != "mouse" && section != "gamepad" &&
                    section != "hotkeys")
                    warn(where + "unknown section [" + section + "]");
                continue;
            }
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                warn(where + "expected key = value");
                continue;
            }
            const std::string key = lower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));

            auto bindingsInto = [&](std::vector<Binding> &target)
            {
                std::vector<std::string> bad;
                std::vector<Binding> parsed;
                parseBindings(value, parsed, &bad);
                for (const std::string &item : bad)
                    warn(where + "unknown key or bad value '" + item + "' for " + key);
                target = parsed;
            };
            auto numberInto = [&](float &target, float lo, float hi)
            {
                float parsed = 0.0f;
                if (!parseNumber(value, parsed) || parsed < lo || parsed > hi)
                    warn(where + key + " wants a number from " + std::to_string(lo) + " to " + std::to_string(hi));
                else
                    target = parsed;
            };
            auto boolInto = [&](bool &target)
            {
                if (!parseBool(value, target))
                    warn(where + key + " wants true or false");
            };

            bool known = false;
            if (section == "buttons")
            {
                for (int i = 0; i < kPadButtonCount; ++i)
                    if (key == kButtonNames[i])
                    {
                        bindingsInto(buttons[static_cast<size_t>(i)]);
                        known = true;
                    }
            }
            else if (section == "sticks")
            {
                for (int i = 0; i < kStickDirectionCount; ++i)
                    if (key == kStickNames[i])
                    {
                        bindingsInto(stick[static_cast<size_t>(i)]);
                        known = true;
                    }
                if (key == "modifier")
                {
                    bindingsInto(stickModifier);
                    known = true;
                }
                else if (key == "modifier_amount")
                {
                    numberInto(stickModifierAmount, 0.0f, 1.0f);
                    known = true;
                }
            }
            else if (section == "mouse")
            {
                known = true;
                const std::string v = lower(value);
                if (key == "motion")
                {
                    if (v == "right_stick" || v == "camera")
                        mouseMotion = MouseMotion::RightStick;
                    else if (v == "left_stick")
                        mouseMotion = MouseMotion::LeftStick;
                    else if (v == "none" || v.empty())
                        mouseMotion = MouseMotion::None;
                    else
                        warn(where + "motion wants right_stick, left_stick or none");
                }
                else if (key == "mode")
                {
                    // offset / velocity: what "mode" meant before mouse-look
                    // (now stick_mode); they leave look/stick as it is.
                    if (v == "look")
                        mouseCamera = MouseCamera::Look;
                    else if (v == "stick")
                        mouseCamera = MouseCamera::Stick;
                    else if (v == "offset")
                        mouseMode = MouseMode::Offset;
                    else if (v == "velocity")
                        mouseMode = MouseMode::Velocity;
                    else
                        warn(where + "mode wants look or stick");
                }
                else if (key == "stick_mode")
                {
                    if (v == "offset")
                        mouseMode = MouseMode::Offset;
                    else if (v == "velocity")
                        mouseMode = MouseMode::Velocity;
                    else
                        warn(where + "stick_mode wants offset or velocity");
                }
                else if (key == "look_speed")
                    numberInto(lookDegreesPerPixel, 0.0f, 10.0f);
                else if (key == "look_up_limit")
                    numberInto(lookUpLimit, 0.0f, 89.0f);
                else if (key == "look_down_limit")
                    numberInto(lookDownLimit, 0.0f, 89.0f);
                else if (key == "auto_recentre")
                    numberInto(lookAutoRecentre, 0.0f, 3600.0f);
                else if (key == "look_snap")
                    boolInto(lookSnap);
                else if (key == "recentre")
                    bindingsInto(recentreKeys);
                else if (key == "sensitivity")
                {
                    numberInto(mouseSensitivityX, 0.0f, 100.0f);
                    mouseSensitivityY = mouseSensitivityX;
                }
                else if (key == "sensitivity_x")
                    numberInto(mouseSensitivityX, 0.0f, 100.0f);
                else if (key == "sensitivity_y")
                    numberInto(mouseSensitivityY, 0.0f, 100.0f);
                else if (key == "invert_x")
                    boolInto(mouseInvertX);
                else if (key == "invert_y")
                    boolInto(mouseInvertY);
                else if (key == "decay")
                    numberInto(mouseDecay, 0.0f, 1.0f);
                else if (key == "deadzone")
                    numberInto(mouseDeadzone, 0.0f, 1.0f);
                else if (key == "capture")
                {
                    if (v == "click")
                        capture = CaptureMode::Click;
                    else if (v == "always")
                        capture = CaptureMode::Always;
                    else if (v == "never")
                        capture = CaptureMode::Never;
                    else
                        warn(where + "capture wants click, always or never");
                }
                else if (key == "release")
                    bindingsInto(releaseKeys);
                else
                    known = false;
            }
            else if (section == "gamepad")
            {
                known = true;
                if (key == "enabled")
                    boolInto(gamepadEnabled);
                else if (key == "stick_threshold")
                    numberInto(gamepadStickThreshold, 0.0f, 1.0f);
                else if (key == "prompts")
                {
                    const std::string v = lower(value);
                    bool found = false;
                    for (PromptStyle style : {PromptStyle::Auto, PromptStyle::Keyboard, PromptStyle::Xbox,
                                              PromptStyle::PlayStation, PromptStyle::Nintendo})
                        if (v == promptStyleName(style))
                        {
                            prompts = style;
                            found = true;
                        }
                    if (!found)
                        warn(where + "prompts wants auto, keyboard, xbox, playstation or nintendo");
                }
                else
                {
                    known = false;
                    for (int i = 0; i < kPadButtonCount; ++i)
                    {
                        if (key != kButtonNames[i])
                            continue;
                        known = true;
                        std::vector<int> physical;
                        for (const std::string &part : split(value, ','))
                        {
                            const std::string name = trim(part);
                            if (name.empty())
                                continue;
                            const int code = gamepadButtonByName(name);
                            if (code < 0)
                                warn(where + "unknown gamepad button '" + name + "' for " + key);
                            else
                                physical.push_back(code);
                        }
                        gamepadButtons[static_cast<size_t>(i)] = physical;
                    }
                }
            }
            else if (section == "hotkeys")
            {
                for (int i = 0; i < kHotkeyCount; ++i)
                {
                    if (key == hotkeyName(i))
                    {
                        bindingsInto(hotkeys[static_cast<size_t>(i)]);
                        known = true;
                    }
                    else if (key == std::string(hotkeyName(i)) + "_pad")
                    {
                        known = true;
                        std::vector<int> chord;
                        for (const std::string &part : split(value, '+'))
                        {
                            const std::string name = trim(part);
                            if (name.empty())
                                continue;
                            const int code = gamepadButtonByName(name);
                            if (code < 0)
                                warn(where + "unknown gamepad button '" + name + "' for " + key);
                            else
                                chord.push_back(code);
                        }
                        hotkeyPads[static_cast<size_t>(i)] = chord;
                    }
                }
            }
            else
            {
                continue; // unknown section, already reported
            }
            if (!known)
                warn(where + "unknown key '" + key + "' in [" + section + "]");
        }
    }

    // ---- mapper -------------------------------------------------------------

    void HostInputMapper::updateCapture(const RawHostInput &raw)
    {
        if (!raw.focused)
        {
            m_captured = false;
            m_swallowed.reset();
            return;
        }
        // Mouse buttons held since the capturing click stay out of the game
        // until they are released.
        for (int i = 0; i < kMouseButtonCount; ++i)
            if (!raw.down.test(static_cast<size_t>(kMouseCodeBase + i)))
                m_swallowed.reset(static_cast<size_t>(kMouseCodeBase + i));

        switch (m_config.capture)
        {
        case CaptureMode::Never:
            m_captured = false;
            return;
        case CaptureMode::Click:
        case CaptureMode::Always:
            break;
        }
        if (m_captured)
        {
            if (anyPressed(m_config.releaseKeys, raw.pressed))
                m_captured = false;
            return;
        }
        bool clicked = false;
        for (int i = 0; i < kMouseButtonCount; ++i)
            if (raw.pressed.test(static_cast<size_t>(kMouseCodeBase + i)))
                clicked = true;
        if (clicked)
        {
            m_captured = true;
            for (int i = 0; i < kMouseButtonCount; ++i)
                if (raw.down.test(static_cast<size_t>(kMouseCodeBase + i)))
                    m_swallowed.set(static_cast<size_t>(kMouseCodeBase + i));
        }
        else if (m_config.capture == CaptureMode::Always && !m_releasedByKey)
        {
            m_captured = true;
        }
    }

    float HostInputMapper::filterMouse(float &state, float delta, float sensitivity, double dtVsyncs) const
    {
        constexpr float kPerPixel = 1.0f / 200.0f;
        const float push = delta * sensitivity * kPerPixel;
        const double dt = std::max(0.0, dtVsyncs);
        if (m_config.mouseMode == MouseMode::Offset)
        {
            if (delta != 0.0f)
                state = std::clamp(state + push, -1.0f, 1.0f);
            else
                state *= static_cast<float>(std::pow(static_cast<double>(m_config.mouseDecay), dt));
        }
        else
        {
            // Pixels per vsync drive the push (25 px per vsync = full at
            // sensitivity 1); without motion it decays as in offset mode.
            if (delta != 0.0f)
                state = std::clamp(static_cast<float>(push / std::max(dt, 1.0)) * 8.0f, -1.0f, 1.0f);
            else
                state *= static_cast<float>(std::pow(static_cast<double>(m_config.mouseDecay), dt));
        }
        // A decaying push ends at centre (motion of any size keeps it).
        if (delta == 0.0f && std::fabs(state) < 0.01f)
            state = 0.0f;
        if (state == 0.0f)
            return 0.0f;
        const float dz = std::clamp(m_config.mouseDeadzone, 0.0f, 1.0f);
        const float magnitude = dz + (1.0f - dz) * std::fabs(state);
        return state > 0.0f ? magnitude : -magnitude;
    }

    PadReport HostInputMapper::map(const RawHostInput &raw, double dtVsyncs)
    {
        if (raw.focused != m_lastFocused)
        {
            m_lastFocused = raw.focused;
            m_releasedByKey = false;
        }
        const bool wasCaptured = m_captured;
        updateCapture(raw);
        if (wasCaptured && !m_captured && raw.focused)
            m_releasedByKey = true;
        if (!wasCaptured && m_captured)
            m_releasedByKey = false;

        PadReport report;
        std::array<float, 4> keyStick{}; // RX, RY, LX, LY

        // Keyboard and mouse count only while the window has focus. Mouse
        // buttons need the cursor captured (unless capture = never), and the
        // click that captured it is held back.
        HostCodeSet down;
        if (raw.focused)
        {
            down = raw.down;
            const bool mouseLive = m_captured || m_config.capture == CaptureMode::Never;
            for (int i = 0; i < kMouseButtonCount; ++i)
            {
                const size_t code = static_cast<size_t>(kMouseCodeBase + i);
                if (!mouseLive || m_swallowed.test(code))
                    down.reset(code);
            }
        }

        for (int i = 0; i < kPadButtonCount; ++i)
        {
            float value = 0.0f;
            if (active(m_config.buttons[static_cast<size_t>(i)], down, value))
            {
                report.buttons = static_cast<uint16_t>(report.buttons | (1u << i));
                report.pressure[static_cast<size_t>(i)] = pressureByte(value);
            }
        }

        float modifierValue = 0.0f;
        const float scale = active(m_config.stickModifier, down, modifierValue) ? m_config.stickModifierAmount : 1.0f;
        auto direction = [&](int dir)
        {
            float value = 0.0f;
            return active(m_config.stick[static_cast<size_t>(dir)], down, value) ? value : 0.0f;
        };
        keyStick[kLx] = (direction(kLeftRight) - direction(kLeftLeft)) * scale;
        keyStick[kLy] = (direction(kLeftDown) - direction(kLeftUp)) * scale;
        keyStick[kRx] = (direction(kRightRight) - direction(kRightLeft)) * scale;
        keyStick[kRy] = (direction(kRightDown) - direction(kRightUp)) * scale;

        // Mouse motion: only while captured (or capture = never) and focused.
        const bool cursorLive = raw.focused && (m_captured || m_config.capture == CaptureMode::Never);
        const float dx = cursorLive ? raw.mouseDx * (m_config.mouseInvertX ? -1.0f : 1.0f) : 0.0f;
        const float dy = cursorLive ? raw.mouseDy * (m_config.mouseInvertY ? -1.0f : 1.0f) : 0.0f;
        if (m_config.mouseCamera == MouseCamera::Look)
        {
            // Mouse-look: motion goes to the look accumulators (taken by
            // cameraFrame), not to a stick. Motion of a read period in
            // which the game's camera did not run is dropped.
            m_mouseX = 0.0f;
            m_mouseY = 0.0f;
            if (m_cameraSinceRead)
            {
                m_readsWithoutCamera = 0;
            }
            else
            {
                m_lookPendingX = 0.0f;
                m_lookPendingY = 0.0f;
                if (m_readsWithoutCamera < kLookResetReads && ++m_readsWithoutCamera == kLookResetReads)
                {
                    m_lookYaw = 0.0f;
                    m_lookPitch = 0.0f;
                }
            }
            m_cameraSinceRead = false;
            // dx > 0: turn right; dy > 0: look up (invert_y = true makes mouse up positive).
            m_lookPendingX += dx * m_config.mouseSensitivityX * m_config.lookDegreesPerPixel;
            m_lookPendingY += dy * m_config.mouseSensitivityY * m_config.lookDegreesPerPixel;
            if (dx != 0.0f || dy != 0.0f)
                m_lookIdleVsyncs = 0.0;
            else
                m_lookIdleVsyncs += std::max(0.0, dtVsyncs);
            HostCodeSet pressed;
            if (raw.focused)
            {
                pressed = raw.pressed;
                for (int i = 0; i < kMouseButtonCount; ++i)
                {
                    const size_t code = static_cast<size_t>(kMouseCodeBase + i);
                    if (!down.test(code))
                        pressed.reset(code);
                }
            }
            if (anyPressed(m_config.recentreKeys, pressed))
                m_lookRecentre = true;
        }
        else
        {
            const bool motionLive = cursorLive && m_config.mouseMotion != MouseMotion::None;
            if (!motionLive)
            {
                m_mouseX = 0.0f;
                m_mouseY = 0.0f;
            }
            const float mouseX = filterMouse(m_mouseX, motionLive ? dx : 0.0f, m_config.mouseSensitivityX, dtVsyncs);
            const float mouseY = filterMouse(m_mouseY, motionLive ? dy : 0.0f, m_config.mouseSensitivityY, dtVsyncs);
            if (m_config.mouseMotion != MouseMotion::None)
            {
                const int xi = m_config.mouseMotion == MouseMotion::RightStick ? kRx : kLx;
                const int yi = m_config.mouseMotion == MouseMotion::RightStick ? kRy : kLy;
                // The mouse wins over keys on the same stick axis when it pushes.
                if (mouseX != 0.0f)
                    keyStick[static_cast<size_t>(xi)] = mouseX;
                if (mouseY != 0.0f)
                    keyStick[static_cast<size_t>(yi)] = mouseY;
            }
        }

        for (size_t i = 0; i < 4; ++i)
            report.sticks[i] = stickByte(keyStick[i]);

        // Gamepad: buttons add (pressure full), a pushed stick wins.
        m_lookPadStick = applyGamepad(m_config, raw.gamepad, report);
        return report;
    }

    bool applyGamepad(const HostInputConfig &config, const GamepadState &pad, PadReport &report)
    {
        if (!config.gamepadEnabled || !pad.present)
            return false;
        // [gamepad] button remapping (by default each its own).
        for (int i = 0; i < kPadButtonCount; ++i)
        {
            bool down = false;
            for (int physical : config.gamepadButtons[static_cast<size_t>(i)])
                if (physical >= 0 && physical < kPadButtonCount && (pad.buttons & (1u << physical)))
                    down = true;
            if (!down)
                continue;
            report.buttons = static_cast<uint16_t>(report.buttons | (1u << i));
            report.pressure[static_cast<size_t>(i)] = 0xFFu;
        }
        const float threshold = config.gamepadStickThreshold;
        auto pushed = [threshold](float x, float y)
        { return std::fabs(x) > threshold || std::fabs(y) > threshold; };
        const bool right = pushed(pad.axes[kRx], pad.axes[kRy]);
        if (right)
        {
            report.sticks[kRx] = static_cast<uint8_t>(128 + pad.axes[kRx] * 127);
            report.sticks[kRy] = static_cast<uint8_t>(128 + pad.axes[kRy] * 127);
        }
        if (pushed(pad.axes[kLx], pad.axes[kLy]))
        {
            report.sticks[kLx] = static_cast<uint8_t>(128 + pad.axes[kLx] * 127);
            report.sticks[kLy] = static_cast<uint8_t>(128 + pad.axes[kLy] * 127);
        }
        return right;
    }

    int GamepadSlots::connect(uint32_t id)
    {
        if (id == 0u)
            return -1;
        if (const int known = slotOf(id); known >= 0)
            return known;
        m_order.push_back(id);
        return count() - 1;
    }

    bool GamepadSlots::disconnect(uint32_t id)
    {
        const int slot = slotOf(id);
        if (slot < 0)
            return false;
        if (slot < kPlayerSlots && count() > kPlayerSlots)
        {
            // A player lost their pad and a spare is connected: the first
            // spare takes over, the other player keeps theirs.
            m_order[static_cast<size_t>(slot)] = m_order[kPlayerSlots];
            m_order.erase(m_order.begin() + kPlayerSlots);
        }
        else
        {
            // The pads after it move up (player 2's pad becomes player 1's
            // when player 1's goes and nothing else is connected).
            m_order.erase(m_order.begin() + slot);
        }
        return true;
    }

    void GamepadSlots::swapPlayers()
    {
        if (count() >= 2)
            std::swap(m_order[0], m_order[1]);
    }

    int GamepadSlots::slotOf(uint32_t id) const
    {
        for (size_t i = 0; i < m_order.size(); ++i)
            if (m_order[i] == id)
                return static_cast<int>(i);
        return -1;
    }

    uint32_t GamepadSlots::idAt(int slot) const
    {
        return slot >= 0 && slot < count() ? m_order[static_cast<size_t>(slot)] : 0u;
    }

    PortSource routePort(int port, int gamepadCount, bool gamepadEnabled, bool mirrorPort1)
    {
        PortSource source;
        const int pads = gamepadEnabled ? std::max(gamepadCount, 0) : 0;
        if (port == 0)
        {
            source.connected = true;
            source.keyboardMouse = true;
            source.gamepadSlot = pads >= 1 ? 0 : -1;
        }
        else if (port == 1 && pads >= 2)
        {
            source.connected = true;
            source.gamepadSlot = 1;
        }
        else if (port == 1 && mirrorPort1)
        {
            source.connected = true;
            source.mirrorPort0 = true;
        }
        return source;
    }

    LookFrame HostInputMapper::cameraFrame(float basePitch, LookContext context)
    {
        const bool takeMotion = context == LookContext::Play;
        m_cameraSinceRead = true;
        LookFrame frame;
        frame.upLimit = m_config.lookUpLimit;
        frame.downLimit = m_config.lookDownLimit;
        const float dx = takeMotion ? m_lookPendingX : 0.0f;
        const float dy = takeMotion ? m_lookPendingY : 0.0f;
        m_lookPendingX = 0.0f;
        m_lookPendingY = 0.0f;
        if (!takeMotion)
            m_lookIdleVsyncs = 0.0;
        if (m_config.mouseCamera != MouseCamera::Look)
            return frame;
        if (m_lookPadStick || m_lookRecentre || context == LookContext::Cut)
        {
            // The gamepad's camera stick takes over, a recentre key, or the
            // game cuts to its own camera: back behind the skater (the game
            // eases the camera there).
            m_lookRecentre = false;
            m_lookYaw = 0.0f;
            m_lookPitch = 0.0f;
            return frame;
        }
        const bool moved = dx != 0.0f || dy != 0.0f;
        const float oldYaw = m_lookYaw;
        const float oldPitch = m_lookPitch;
        if (moved)
        {
            float yaw = std::remainder(m_lookYaw + dx, 360.0f);
            if (yaw <= -180.0f)
                yaw += 360.0f;
            m_lookYaw = yaw;
            // Keep basePitch + pitch within the limits, but never pull an
            // accumulated pitch back towards them by more than the motion.
            float pitch = m_lookPitch + dy;
            const float hi = m_config.lookUpLimit - basePitch;
            const float lo = -m_config.lookDownLimit - basePitch;
            if (dy > 0.0f && pitch > hi)
                pitch = std::max(hi, m_lookPitch);
            if (dy < 0.0f && pitch < lo)
                pitch = std::min(lo, m_lookPitch);
            m_lookPitch = pitch;
        }
        else if (m_config.lookAutoRecentre > 0.0f &&
                 m_lookIdleVsyncs >= static_cast<double>(m_config.lookAutoRecentre) * 59.94)
        {
            m_lookYaw = 0.0f;
            m_lookPitch = 0.0f;
        }
        frame.active = m_lookYaw != 0.0f || m_lookPitch != 0.0f;
        frame.yaw = m_lookYaw;
        frame.pitch = m_lookPitch;
        if (moved && m_config.lookSnap)
        {
            frame.snapYaw = std::remainder(m_lookYaw - oldYaw, 360.0f);
            frame.snapPitch = m_lookPitch - oldPitch;
        }
        return frame;
    }

    // ---- camera geometry ----------------------------------------------------

    namespace
    {
        constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

        struct D3
        {
            double x, y, z;
        };
        D3 d3(const Vec3 &v) { return D3{v.x, v.y, v.z}; }
        double dot(const D3 &a, const D3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
        D3 cross(const D3 &a, const D3 &b)
        {
            return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        }
        D3 scale(const D3 &a, double k) { return D3{a.x * k, a.y * k, a.z * k}; }
        D3 add(const D3 &a, const D3 &b) { return D3{a.x + b.x, a.y + b.y, a.z + b.z}; }

        // Offset split about up: unit up, unit horizontal direction, length,
        // elevation (radians, > 0 above). False if degenerate.
        bool splitOffset(const Vec3 &offset, const Vec3 &up, D3 &u, D3 &h, double &length, double &elevation)
        {
            const D3 o = d3(offset);
            u = d3(up);
            const double ul = std::sqrt(dot(u, u));
            length = std::sqrt(dot(o, o));
            if (!(ul > 1e-6) || !(length > 1e-6) || !std::isfinite(ul) || !std::isfinite(length))
                return false;
            u = scale(u, 1.0 / ul);
            const double vertical = dot(o, u);
            h = add(o, scale(u, -vertical));
            const double hl = std::sqrt(dot(h, h));
            if (!(hl > 1e-6 * length))
                return false;
            h = scale(h, 1.0 / hl);
            elevation = std::atan2(vertical, hl);
            return true;
        }
    }

    float viewPitchOfOffset(const Vec3 &offset, const Vec3 &up)
    {
        D3 u{}, h{};
        double length = 0.0, elevation = 0.0;
        if (!splitOffset(offset, up, u, h, length, elevation))
            return 0.0f;
        return static_cast<float>(-elevation / kDegToRad);
    }

    bool limitCameraDrop(Vec3 &offset, const Vec3 &up, float maxDrop, float minLength, float allowedDrop)
    {
        D3 u{}, h{};
        double length = 0.0, elevation = 0.0;
        if (!splitOffset(offset, up, u, h, length, elevation))
            return false;
        const double drop = -std::sin(elevation) * length;
        const double limit = std::max<double>(maxDrop, allowedDrop);
        if (!(drop > limit) || !(length > minLength))
            return false;
        const double newLength = std::max<double>(minLength, length * limit / drop);
        const double k = newLength / length;
        offset = Vec3{static_cast<float>(offset.x * k), static_cast<float>(offset.y * k), static_cast<float>(offset.z * k)};
        return true;
    }

    bool orbitCameraOffset(Vec3 &offset, const Vec3 &up, float yawDeg, float pitchDeg, float upLimit,
                           float downLimit)
    {
        D3 u{}, h{};
        double length = 0.0, elevation = 0.0;
        if (!splitOffset(offset, up, u, h, length, elevation))
            return false;
        // View pitch = -elevation. Tilt by pitchDeg, but a tilt never takes
        // it past a limit it was inside of (and never pulls it back in).
        const double basePitch = -elevation / kDegToRad;
        double pitch = basePitch + pitchDeg;
        if (pitchDeg > 0.0f && pitch > upLimit)
            pitch = std::max<double>(upLimit, basePitch);
        if (pitchDeg < 0.0f && pitch < -downLimit)
            pitch = std::min<double>(-downLimit, basePitch);
        const double newElevation = -pitch * kDegToRad;
        // Turning the view right is a negative (right-handed) rotation about
        // up; the offset is the view direction reversed, so it turns the same.
        const double a = -static_cast<double>(yawDeg) * kDegToRad;
        const D3 turned = add(scale(h, std::cos(a)), scale(cross(u, h), std::sin(a)));
        const D3 result = scale(add(scale(turned, std::cos(newElevation)), scale(u, std::sin(newElevation))), length);
        offset = Vec3{static_cast<float>(result.x), static_cast<float>(result.y), static_cast<float>(result.z)};
        return true;
    }

    Quat quatMultiply(const Quat &a, const Quat &b)
    {
        const D3 av{a.x, a.y, a.z};
        const D3 bv{b.x, b.y, b.z};
        const D3 v = add(add(scale(bv, a.w), scale(av, b.w)), cross(av, bv));
        const double w = static_cast<double>(a.w) * b.w - dot(av, bv);
        return Quat{static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z), static_cast<float>(w)};
    }

    Vec3 quatRotate(const Quat &q, const Vec3 &v)
    {
        const Quat p{v.x, v.y, v.z, 0.0f};
        const Quat conj{-q.x, -q.y, -q.z, q.w};
        const Quat r = quatMultiply(quatMultiply(q, p), conj);
        return Vec3{r.x, r.y, r.z};
    }

    namespace
    {
        Quat axisAngle(const D3 &unitAxis, double radians)
        {
            const double s = std::sin(radians * 0.5);
            return Quat{static_cast<float>(unitAxis.x * s), static_cast<float>(unitAxis.y * s),
                        static_cast<float>(unitAxis.z * s), static_cast<float>(std::cos(radians * 0.5))};
        }
    }

    Quat lookRotation(const Vec3 &offset, const Vec3 &up, float yawDeg, float pitchDeg)
    {
        D3 u{}, h{};
        double length = 0.0, elevation = 0.0;
        if (!splitOffset(offset, up, u, h, length, elevation))
            return Quat{};
        // Looking up lowers the camera: about u x h, a positive angle takes
        // the offset towards -u.
        const Quat tilt = axisAngle(cross(u, h), static_cast<double>(pitchDeg) * kDegToRad);
        const Quat turn = axisAngle(u, -static_cast<double>(yawDeg) * kDegToRad);
        return quatMultiply(turn, tilt);
    }

    // ---- script -------------------------------------------------------------

    void HostInputScript::parse(const std::string &spec, std::vector<std::string> *warnings)
    {
        m_keys.clear();
        m_motions.clear();
        m_focus.clear();
        m_wheel.clear();
        for (const std::string &rawItem : split(spec, ','))
        {
            const std::string item = trim(rawItem);
            if (item.empty())
                continue;
            const std::vector<std::string> fields = split(item, ':');
            const size_t eq = fields.size() >= 2 ? fields[1].find('=') : std::string::npos;
            if (fields.size() < 2 || fields.size() > 3 || eq == std::string::npos)
            {
                if (warnings)
                    warnings->push_back(item);
                continue;
            }
            const double start = parseScriptTime(trim(fields[0]));
            const std::string what = upper(trim(fields[1].substr(0, eq)));
            const std::string arg = trim(fields[1].substr(eq + 1));
            const bool hasDuration = fields.size() == 3;
            const double duration = hasDuration ? parseScriptTime(trim(fields[2])) : 0.0;
            if (what == "KEY")
            {
                KeyHold hold;
                hold.code = hostCodeByName(arg);
                hold.start = start;
                hold.end = start + (hasDuration ? duration : 0.25 * 59.94);
                if (hold.code >= 0)
                {
                    m_keys.push_back(hold);
                    continue;
                }
            }
            else if (what == "MOUSE")
            {
                const size_t slash = arg.find('/');
                float dx = 0.0f;
                float dy = 0.0f;
                if (slash != std::string::npos && parseNumber(arg.substr(0, slash), dx) &&
                    parseNumber(arg.substr(slash + 1), dy))
                {
                    Motion motion;
                    motion.start = start;
                    motion.duration = std::max(0.0, duration);
                    motion.dx = dx;
                    motion.dy = dy;
                    m_motions.push_back(motion);
                    continue;
                }
            }
            else if (what == "FOCUS" && !hasDuration && (arg == "0" || arg == "1"))
            {
                m_focus.push_back(Focus{start, arg == "1"});
                continue;
            }
            else if (what == "WHEEL" && !hasDuration)
            {
                float steps = 0.0f;
                if (parseNumber(arg, steps))
                {
                    m_wheel.push_back(Wheel{start, steps});
                    continue;
                }
            }
            if (warnings)
                warnings->push_back(item);
        }
        std::stable_sort(m_focus.begin(), m_focus.end(),
                         [](const Focus &a, const Focus &b)
                         { return a.time < b.time; });
    }

    RawHostInput HostInputScript::sample(double from, double to) const
    {
        RawHostInput raw;
        for (const KeyHold &hold : m_keys)
        {
            const size_t code = static_cast<size_t>(hold.code);
            if (to >= hold.start && to < hold.end)
                raw.down.set(code);
            // A press that started in the interval counts even if it already
            // ended (a tap shorter than the sample interval).
            if (hold.start > from && hold.start <= to)
            {
                raw.pressed.set(code);
                raw.down.set(code);
            }
        }
        for (const Motion &motion : m_motions)
        {
            if (motion.duration <= 0.0)
            {
                if (motion.start > from && motion.start <= to)
                {
                    raw.mouseDx += motion.dx;
                    raw.mouseDy += motion.dy;
                }
                continue;
            }
            const double lo = std::max(from, motion.start);
            const double hi = std::min(to, motion.start + motion.duration);
            if (hi > lo)
            {
                const double share = (hi - lo) / motion.duration;
                raw.mouseDx += static_cast<float>(motion.dx * share);
                raw.mouseDy += static_cast<float>(motion.dy * share);
            }
        }
        for (const Wheel &wheel : m_wheel)
            if (wheel.time > from && wheel.time <= to)
                raw.wheel += wheel.steps;
        raw.focused = true;
        for (const Focus &focus : m_focus)
            if (focus.time <= to)
                raw.focused = focus.focused;
        return raw;
    }

    // ---- paths --------------------------------------------------------------

    std::string userConfigDir(const std::string &appName)
    {
#if defined(_WIN32)
        if (const char *appData = std::getenv("APPDATA"); appData && appData[0])
            return std::string(appData) + "\\" + appName;
        return {};
#else
        const char *home = std::getenv("HOME");
#if defined(__APPLE__)
        if (home && home[0])
            return std::string(home) + "/Library/Application Support/" + appName;
        return {};
#else
        if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0])
            return std::string(xdg) + "/" + appName;
        if (home && home[0])
            return std::string(home) + "/.config/" + appName;
        return {};
#endif
#endif
    }
}
