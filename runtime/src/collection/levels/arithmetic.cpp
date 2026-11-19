// The Python reference's arithmetic (collection/levels/arithmetic.h).
#include "collection/levels/arithmetic.h"

#include <cfloat>
#include <cmath>

namespace ps2x::app::levels
{
    namespace
    {
        double sumRange(const double *values, size_t count)
        {
            if (count == 0)
                return 0.0;
            // sum() starts from the int 0: 0 + the first, then each next with
            // its rounding error kept apart and added at the end.
            double total = 0.0 + values[0];
            double compensation = 0.0;
            for (size_t i = 1; i < count; ++i)
            {
                const double x = values[i];
                const double t = total + x;
                if (std::fabs(total) >= std::fabs(x))
                    compensation += (total - t) + x;
                else
                    compensation += (x - t) + total;
                total = t;
            }
            if (compensation != 0.0 && std::isfinite(compensation))
                total += compensation;
            return total;
        }

        struct DoubleLength
        {
            double hi, lo;
        };

        DoubleLength fastSum(double a, double b)
        {
            const double x = a + b;
            return {x, (a - x) + b};
        }

        DoubleLength exactProduct(double x, double y)
        {
            const double z = x * y;
            return {z, std::fma(x, y, -z)};
        }

        // CPython's vector_norm (mathmodule.c) of non-negative values whose
        // largest is `max`.
        double vectorNorm(double *values, size_t count, double max)
        {
            if (std::isinf(max))
                return max;
            if (max == 0.0 || count <= 1)
                return max;
            int maxExponent = 0;
            std::frexp(max, &maxExponent);
            if (maxExponent < -1023)
            {
                for (size_t i = 0; i < count; ++i)
                    values[i] /= DBL_MIN;
                return DBL_MIN * vectorNorm(values, count, max / DBL_MIN);
            }
            const double scale = std::ldexp(1.0, -maxExponent);
            double csum = 1.0, frac1 = 0.0, frac2 = 0.0;
            for (size_t i = 0; i < count; ++i)
            {
                const double x = values[i] * scale;
                const DoubleLength pr = exactProduct(x, x);
                const DoubleLength sm = fastSum(csum, pr.hi);
                csum = sm.hi;
                frac1 += pr.lo;
                frac2 += sm.lo;
            }
            double h = std::sqrt(csum - 1.0 + (frac1 + frac2));
            const DoubleLength pr = exactProduct(-h, h);
            const DoubleLength sm = fastSum(csum, pr.hi);
            csum = sm.hi;
            frac1 += pr.lo;
            frac2 += sm.lo;
            const double x = csum - 1.0 + (frac1 + frac2);
            h += x / (2.0 * h);
            return h / scale;
        }
    }

    double sumOf(std::initializer_list<double> values) { return sumRange(values.begin(), values.size()); }

    double sumOf(const std::vector<double> &values) { return sumRange(values.data(), values.size()); }

    double distanceOf(const Vec3 &p, const Vec3 &q)
    {
        double diffs[3];
        double max = 0.0;
        bool nan = false;
        for (int i = 0; i < 3; ++i)
        {
            diffs[i] = std::fabs(p[i] - q[i]);
            nan = nan || std::isnan(diffs[i]);
            if (diffs[i] > max)
                max = diffs[i];
        }
        if (nan)
            return std::nan("");
        return vectorNorm(diffs, 3, max);
    }
}
