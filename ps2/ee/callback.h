#pragma once

// The same thing as ee::FunctionPtr under the name two lanes wrote it by: new code says FunctionPtr.

#include "function_ptr.h"

namespace ee
{
    template <typename Signature>
    using Callback = FunctionPtr<Signature>;
}
