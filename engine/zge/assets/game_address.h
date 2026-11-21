#pragma once

// A record's game address as a plain number, for what keeps it as one: a
// table row, an engine word typed as a number, the native mirrors' keys.

#include "ee/ee.h"

#include <cstdint>

namespace zge
{
    template <typename T>
    constexpr uint32_t gameAddress(ee::Ptr<T> record)
    {
        return record.address;
    }
    template <typename T>
    uint32_t gameAddress(T *record)
    {
        return ee::addressOf(record);
    }
}
