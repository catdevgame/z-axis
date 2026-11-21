#include "runtime/pad_script.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
    int axisByName(const std::string &name)
    {
        if (name == "RX")
            return PadScript::kAxisRx;
        if (name == "RY")
            return PadScript::kAxisRy;
        if (name == "LX")
            return PadScript::kAxisLx;
        if (name == "LY")
            return PadScript::kAxisLy;
        return PadScript::kAxisNone;
    }

    uint8_t axisByte(double value)
    {
        const double clamped = value < -1.0 ? -1.0 : (value > 1.0 ? 1.0 : value);
        const long byte = std::lround(128.0 + clamped * 127.5);
        return static_cast<uint8_t>(byte < 0 ? 0 : (byte > 255 ? 255 : byte));
    }

    // "<n>" seconds or "<n>v" vsyncs, returned in vsyncs.
    double parseTime(const std::string &text)
    {
        if (!text.empty() && (text.back() == 'v' || text.back() == 'V'))
            return std::atof(text.substr(0, text.size() - 1).c_str());
        return std::atof(text.c_str()) * PadScript::kVsyncsPerSecond;
    }
}

uint16_t PadScript::buttonMask(const std::string &name)
{
    static const struct
    {
        const char *name;
        uint16_t mask;
    } kButtons[] = {
        {"LEFT", 0x0080u}, {"DOWN", 0x0040u}, {"RIGHT", 0x0020u}, {"UP", 0x0010u},
        {"START", 0x0008u}, {"R3", 0x0004u}, {"L3", 0x0002u}, {"SELECT", 0x0001u},
        {"SQUARE", 0x8000u}, {"CROSS", 0x4000u}, {"CIRCLE", 0x2000u}, {"TRIANGLE", 0x1000u},
        {"R1", 0x0800u}, {"L1", 0x0400u}, {"R2", 0x0200u}, {"L2", 0x0100u}};
    for (const auto &button : kButtons)
        if (name == button.name)
            return button.mask;
    return 0u;
}

void PadScript::parse(const std::string &list, std::vector<std::string> *warnings)
{
    m_presses.clear();
    m_syncs.clear();
    size_t pos = 0;
    while (pos < list.size())
    {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos)
            comma = list.size();
        const std::string item = list.substr(pos, comma - pos);
        pos = comma + 1;
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos)
        {
            if (warnings && !item.empty())
                warnings->push_back(item);
            continue;
        }
        const double start = parseTime(item.substr(0, c1));
        const size_t c2 = item.find(':', c1 + 1);
        const std::string what = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        const size_t eq = what.find('=');

        if (eq != std::string::npos && (what.substr(0, eq) == "SYNC" || what.substr(0, eq) == "WAIT"))
        {
            // Conditions use ':'-free syntax, so everything after "SYNC=" up
            // to the comma belongs to the anchor.
            const std::string condText = item.substr(c1 + 1 + eq + 1);
            Sync sync;
            sync.time = start;
            sync.holdOnly = what.substr(0, eq) == "WAIT";
            bool ok = !condText.empty();
            size_t cpos = 0;
            while (ok && cpos <= condText.size())
            {
                size_t amp = condText.find('&', cpos);
                if (amp == std::string::npos)
                    amp = condText.size();
                StateProbe::Condition condition{};
                ok = StateProbe::parseCondition(condText.substr(cpos, amp - cpos), condition);
                if (ok)
                    sync.conditions.push_back(condition);
                cpos = amp + 1;
            }
            if (ok)
                m_syncs.push_back(std::move(sync));
            else if (warnings)
                warnings->push_back(item);
            continue;
        }

        Press press;
        press.start = start;
        if (eq != std::string::npos)
        {
            press.axis = axisByName(what.substr(0, eq));
            press.axisValue = axisByte(std::atof(what.substr(eq + 1).c_str()));
        }
        else
        {
            press.mask = buttonMask(what);
        }
        const double duration = c2 == std::string::npos ? 0.25 * kVsyncsPerSecond : parseTime(item.substr(c2 + 1));
        press.end = press.start + duration;
        if (press.mask != 0u || press.axis != kAxisNone)
            m_presses.push_back(press);
        else if (warnings)
            warnings->push_back(item);
    }
}

uint16_t PadScript::update(double clockVsyncs, uint64_t tick, const uint8_t *rdram, uint8_t *sticks, SyncEvent *event)
{
    if (!m_started)
    {
        m_started = true;
        m_origin = clockVsyncs;
    }
    double now = clockVsyncs - m_origin + m_offset;

    // WAIT anchors: nothing to do before t; at t, stand until the condition holds.
    while (m_nextSync < m_syncs.size() && m_syncs[m_nextSync].holdOnly)
    {
        const Sync &sync = m_syncs[m_nextSync];
        if (now < sync.time)
            break;
        bool holds = rdram != nullptr;
        for (size_t i = 0; holds && i < sync.conditions.size(); ++i)
            holds = StateProbe::evaluate(sync.conditions[i], rdram, tick);
        if (!holds || m_waiting)
        {
            // Stand at t (also on the update that releases a hold, so the
            // inputs right after t are not skipped).
            m_offset += sync.time - now;
            now = sync.time;
        }
        if (!holds)
        {
            m_waiting = true;
            break;
        }
        if (event)
        {
            event->index = m_nextSync;
            event->shift = m_offset - m_offsetAtArm;
            event->waited = m_waiting;
            event->scriptTime = sync.time;
        }
        m_waiting = false;
        m_sawFalse = false;
        m_offsetAtArm = m_offset;
        ++m_nextSync;
    }
    if (m_nextSync < m_syncs.size() && !m_syncs[m_nextSync].holdOnly)
    {
        const Sync &sync = m_syncs[m_nextSync];
        bool holds = rdram != nullptr;
        for (size_t i = 0; holds && i < sync.conditions.size(); ++i)
            holds = StateProbe::evaluate(sync.conditions[i], rdram, tick);
        // The event is the condition becoming true: it must have been false
        // at least once since the anchor was armed, so a chain of anchors on
        // the same state (one per message box) takes one box each.
        const bool hit = holds && m_sawFalse;
        if (!holds)
            m_sawFalse = true;
        if (hit || now >= sync.time)
        {
            // Either the event (jump forward to t) or the clock reached t
            // first (stand at t until the event).
            m_offset += sync.time - now;
            now = sync.time;
        }
        if (hit)
        {
            if (event)
            {
                event->index = m_nextSync;
                event->shift = m_offset - m_offsetAtArm;
                event->waited = m_waiting;
                event->scriptTime = now;
            }
            m_waiting = false;
            m_sawFalse = false;
            m_offsetAtArm = m_offset;
            ++m_nextSync;
        }
        else if (now >= sync.time)
        {
            m_waiting = true;
        }
    }

    m_scriptTime = now;
    return current(sticks);
}

uint16_t PadScript::current(uint8_t *sticks) const
{
    uint16_t down = 0u;
    for (const Press &press : m_presses)
    {
        if (m_scriptTime < press.start || m_scriptTime >= press.end)
            continue;
        down |= press.mask;
        if (press.axis != kAxisNone && sticks)
            sticks[press.axis] = press.axisValue;
    }
    return down;
}
