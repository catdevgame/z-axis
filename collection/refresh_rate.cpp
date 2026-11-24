// The refresh rate the app asks of the screen (collection/refresh_rate.h).
#include "collection/refresh_rate.h"

#include <cmath>

namespace zx::app
{
    float refreshRateToAsk(bool frameInterpolation, std::span<const float> offeredRates)
    {
        if (frameInterpolation)
            for (const float rate : offeredRates)
                if (std::fabs(rate - kInterpolatedRefreshRate) < 1.0f)
                    return kInterpolatedRefreshRate;
        return kGameRefreshRate;
    }
}
