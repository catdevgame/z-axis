// Frame interpolation's arithmetic (runtime/gs/gs_interpolate.h).
#include "runtime/gs/gs_interpolate.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace ps2x::gs_interpolate
{
    namespace
    {
        std::atomic<int> g_enabled{-1}; // -1: not read yet

        bool fromEnvironment()
        {
            const char *v = std::getenv("ZAXIS_INTERPOLATION");
            return v && (std::strcmp(v, "on") == 0 || std::strcmp(v, "1") == 0);
        }

        using M3 = double[3][3];

        bool invert3(const M3 &m, M3 &o)
        {
            const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
            const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
            const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
            const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
            if (!(std::fabs(det) > 1e-18))
                return false;
            const double k = 1.0 / det;
            o[0][0] = c00 * k;
            o[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k;
            o[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k;
            o[1][0] = c01 * k;
            o[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k;
            o[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k;
            o[2][0] = c02 * k;
            o[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k;
            o[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k;
            return true;
        }

        double det3(const M3 &m)
        {
            return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        }

        // A proper rotation's unit quaternion (x, y, z, w), and back (the same convention both ways).
        void quaternionOf(const M3 &r, double q[4])
        {
            const double trace = r[0][0] + r[1][1] + r[2][2];
            if (trace > 0.0)
            {
                const double s = std::sqrt(trace + 1.0) * 2.0;
                q[3] = 0.25 * s;
                q[0] = (r[2][1] - r[1][2]) / s;
                q[1] = (r[0][2] - r[2][0]) / s;
                q[2] = (r[1][0] - r[0][1]) / s;
            }
            else if (r[0][0] > r[1][1] && r[0][0] > r[2][2])
            {
                const double s = std::sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2.0;
                q[3] = (r[2][1] - r[1][2]) / s;
                q[0] = 0.25 * s;
                q[1] = (r[0][1] + r[1][0]) / s;
                q[2] = (r[0][2] + r[2][0]) / s;
            }
            else if (r[1][1] > r[2][2])
            {
                const double s = std::sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2.0;
                q[3] = (r[0][2] - r[2][0]) / s;
                q[0] = (r[0][1] + r[1][0]) / s;
                q[1] = 0.25 * s;
                q[2] = (r[1][2] + r[2][1]) / s;
            }
            else
            {
                const double s = std::sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2.0;
                q[3] = (r[1][0] - r[0][1]) / s;
                q[0] = (r[0][2] + r[2][0]) / s;
                q[1] = (r[1][2] + r[2][1]) / s;
                q[2] = 0.25 * s;
            }
            const double n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            for (int i = 0; i < 4; ++i)
                q[i] /= n;
        }

        void rotationOf(const double q[4], M3 &r)
        {
            const double x = q[0], y = q[1], z = q[2], w = q[3];
            r[0][0] = 1.0 - 2.0 * (y * y + z * z);
            r[0][1] = 2.0 * (x * y - z * w);
            r[0][2] = 2.0 * (x * z + y * w);
            r[1][0] = 2.0 * (x * y + z * w);
            r[1][1] = 1.0 - 2.0 * (x * x + z * z);
            r[1][2] = 2.0 * (y * z - x * w);
            r[2][0] = 2.0 * (x * z - y * w);
            r[2][1] = 2.0 * (y * z + x * w);
            r[2][2] = 1.0 - 2.0 * (x * x + y * y);
        }
    }

    bool enabled()
    {
        int v = g_enabled.load(std::memory_order_relaxed);
        if (v < 0)
        {
            v = fromEnvironment() ? 1 : 0;
            g_enabled.store(v, std::memory_order_relaxed);
        }
        return v != 0;
    }

    void setEnabled(bool on)
    {
        g_enabled.store(on ? 1 : 0, std::memory_order_relaxed);
    }

    namespace
    {
        std::atomic<uint64_t> g_vblankTick{0};
        std::atomic<int64_t> g_vblankNs{0};
        std::atomic<int64_t> g_vblankPeriodNs{16683333};
    }

    void noteVblank(uint64_t tick, std::chrono::steady_clock::time_point at)
    {
        const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
        const uint64_t lastTick = g_vblankTick.load(std::memory_order_relaxed);
        const int64_t lastNs = g_vblankNs.load(std::memory_order_relaxed);
        if (lastNs != 0 && tick == lastTick + 1u)
        {
            // (a paced vblank is 16.7 ms after the last; a stall or an unpaced burst is not a period)
            const int64_t dt = now - lastNs;
            int64_t period = g_vblankPeriodNs.load(std::memory_order_relaxed);
            if (dt * 4 > period * 3 && dt * 4 < period * 5)
                g_vblankPeriodNs.store(period + (dt - period) / 32, std::memory_order_relaxed);
        }
        g_vblankNs.store(now, std::memory_order_relaxed);
        g_vblankTick.store(tick, std::memory_order_release);
    }

    VblankClock vblankClock()
    {
        VblankClock c;
        c.tick = g_vblankTick.load(std::memory_order_acquire);
        c.tickNs = g_vblankNs.load(std::memory_order_relaxed);
        c.periodNs = g_vblankPeriodNs.load(std::memory_order_relaxed);
        return c;
    }

    void multiply(const Matrix &a, const Matrix &b, Matrix &o)
    {
        Matrix t;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + a[r][3] * b[3][c];
        std::memcpy(o, t, sizeof(t));
    }

    bool invert(const Matrix &m, Matrix &o)
    {
        double a[4][8];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
            {
                a[r][c] = m[r][c];
                a[r][c + 4] = r == c ? 1.0 : 0.0;
            }
        for (int c = 0; c < 4; ++c)
        {
            int pivot = c;
            for (int r = c + 1; r < 4; ++r)
                if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                    pivot = r;
            if (!(std::fabs(a[pivot][c]) > 1e-20))
                return false;
            if (pivot != c)
                for (int k = 0; k < 8; ++k)
                    std::swap(a[c][k], a[pivot][k]);
            const double inv = 1.0 / a[c][c];
            for (int k = 0; k < 8; ++k)
                a[c][k] *= inv;
            for (int r = 0; r < 4; ++r)
            {
                if (r == c || a[r][c] == 0.0)
                    continue;
                const double f = a[r][c];
                for (int k = 0; k < 8; ++k)
                    a[r][k] -= f * a[c][k];
            }
        }
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                o[r][c] = static_cast<float>(a[r][c + 4]);
        return true;
    }

    bool decompose(const Matrix &m, Affine &out)
    {
        if (std::fabs(m[0][3]) > 1e-6f || std::fabs(m[1][3]) > 1e-6f || std::fabs(m[2][3]) > 1e-6f ||
            std::fabs(m[3][3] - 1.0f) > 1e-6f)
            return false;
        M3 a;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                a[r][c] = m[r][c];
        const double det = det3(a);
        if (!(std::fabs(det) > 1e-12))
            return false;
        // Polar decomposition by Higham's iteration: u = (u + u^-T) / 2 converges to the orthogonal factor.
        M3 u;
        std::memcpy(u, a, sizeof(u));
        for (int it = 0; it < 30; ++it)
        {
            M3 inv;
            if (!invert3(u, inv))
                return false;
            double change = 0.0;
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                {
                    const double n = 0.5 * (u[r][c] + inv[c][r]);
                    change = std::fmax(change, std::fabs(n - u[r][c]));
                    u[r][c] = n;
                }
            if (change < 1e-12)
                break;
        }
        // A mirroring part: the rotation is -u, the stretch carries the sign.
        const double sign = det3(u) < 0.0 ? -1.0 : 1.0;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                u[r][c] *= sign;
        // a = u x p: p = u^T x a.
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
            {
                double s = 0.0;
                for (int k = 0; k < 3; ++k)
                    s += u[k][r] * a[k][c];
                out.stretch[r][c] = static_cast<float>(s);
            }
        double q[4];
        quaternionOf(u, q);
        for (int i = 0; i < 4; ++i)
            out.rotation[i] = static_cast<float>(q[i]);
        for (int c = 0; c < 3; ++c)
            out.translation[c] = m[3][c];
        return true;
    }

    void compose(const Affine &a, Matrix &out)
    {
        const double q[4] = {a.rotation[0], a.rotation[1], a.rotation[2], a.rotation[3]};
        M3 u;
        rotationOf(q, u);
        for (int r = 0; r < 3; ++r)
        {
            for (int c = 0; c < 3; ++c)
            {
                double s = 0.0;
                for (int k = 0; k < 3; ++k)
                    s += u[r][k] * a.stretch[k][c];
                out[r][c] = static_cast<float>(s);
            }
            out[r][3] = 0.0f;
        }
        for (int c = 0; c < 3; ++c)
            out[3][c] = a.translation[c];
        out[3][3] = 1.0f;
    }

    Affine between(const Affine &a, const Affine &b, float t)
    {
        Affine o;
        double qa[4], qb[4];
        for (int i = 0; i < 4; ++i)
        {
            qa[i] = a.rotation[i];
            qb[i] = b.rotation[i];
        }
        double d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
        if (d < 0.0) // (the shorter way round)
        {
            d = -d;
            for (double &v : qb)
                v = -v;
        }
        double wa = 1.0 - t, wb = t;
        if (d < 0.9995)
        {
            const double theta = std::acos(d);
            const double s = std::sin(theta);
            wa = std::sin((1.0 - t) * theta) / s;
            wb = std::sin(t * theta) / s;
        }
        double q[4], n = 0.0;
        for (int i = 0; i < 4; ++i)
        {
            q[i] = wa * qa[i] + wb * qb[i];
            n += q[i] * q[i];
        }
        n = std::sqrt(n);
        for (int i = 0; i < 4; ++i)
            o.rotation[i] = static_cast<float>(q[i] / n);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                o.stretch[r][c] = a.stretch[r][c] + (b.stretch[r][c] - a.stretch[r][c]) * t;
        for (int c = 0; c < 3; ++c)
            o.translation[c] = a.translation[c] + (b.translation[c] - a.translation[c]) * t;
        return o;
    }

    bool betweenAffine(const Matrix &a, const Matrix &b, float t, Matrix &out)
    {
        Affine da, db;
        if (!decompose(a, da) || !decompose(b, db))
            return false;
        compose(between(da, db, t), out);
        return true;
    }

    bool betweenView(const Matrix &a, const Matrix &b, float t, Matrix &out)
    {
        Matrix ia, ib, m;
        if (!invert(a, ia) || !invert(b, ib))
            return false;
        // (exact zeros in column 3: the inverse of an affine matrix is affine)
        for (Matrix *x : {&ia, &ib})
        {
            (*x)[0][3] = (*x)[1][3] = (*x)[2][3] = 0.0f;
            (*x)[3][3] = 1.0f;
        }
        if (!betweenAffine(ia, ib, t, m))
            return false;
        return invert(m, out);
    }

    void lerp(const Matrix &a, const Matrix &b, float t, Matrix &out)
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[r][c] = a[r][c] + (b[r][c] - a[r][c]) * t;
    }

    bool cameraCut(const Matrix &viewA, const Matrix &viewB)
    {
        Matrix ia, ib;
        if (!invert(viewA, ia) || !invert(viewB, ib))
            return true;
        const float dx = ia[3][0] - ib[3][0], dy = ia[3][1] - ib[3][1], dz = ia[3][2] - ib[3][2];
        if (dx * dx + dy * dy + dz * dz > kCutDistance * kCutDistance)
            return true;
        // The camera's forward axis in the world: row 2 of its own transform.
        const float *fa = ia[2], *fb = ib[2];
        const float la = std::sqrt(fa[0] * fa[0] + fa[1] * fa[1] + fa[2] * fa[2]);
        const float lb = std::sqrt(fb[0] * fb[0] + fb[1] * fb[1] + fb[2] * fb[2]);
        if (!(la > 0.0f && lb > 0.0f))
            return true;
        const float cosine = (fa[0] * fb[0] + fa[1] * fb[1] + fa[2] * fb[2]) / (la * lb);
        return cosine < kCutTurnCosine;
    }

    bool jumped(const Matrix &worldA, const Matrix &worldB)
    {
        const float dx = worldA[3][0] - worldB[3][0], dy = worldA[3][1] - worldB[3][1], dz = worldA[3][2] - worldB[3][2];
        return dx * dx + dy * dy + dz * dz > kCutDistance * kCutDistance;
    }

    bool reproject(const Matrix &fromInverse, const Matrix &fromCamera, const Matrix &toCamera, float xyz[3],
                   float &wRatio)
    {
        // (X, Y, Z, 1) * fromInverse is the world point times 1/w.
        const float p[4] = {xyz[0], xyz[1], xyz[2], 1.0f};
        float h[4];
        for (int c = 0; c < 4; ++c)
            h[c] = p[0] * fromInverse[0][c] + p[1] * fromInverse[1][c] + p[2] * fromInverse[2][c] + fromInverse[3][c];
        if (!(std::fabs(h[3]) > 1e-20f))
            return false;
        const float world[3] = {h[0] / h[3], h[1] / h[3], h[2] / h[3]};
        float wa = world[0] * fromCamera[0][3] + world[1] * fromCamera[1][3] + world[2] * fromCamera[2][3] + fromCamera[3][3];
        float c[4];
        for (int k = 0; k < 4; ++k)
            c[k] = world[0] * toCamera[0][k] + world[1] * toCamera[1][k] + world[2] * toCamera[2][k] + toCamera[3][k];
        if (!(wa > 1e-6f) || !(c[3] > 1e-6f))
            return false;
        xyz[0] = c[0] / c[3];
        xyz[1] = c[1] / c[3];
        xyz[2] = c[2] / c[3];
        wRatio = wa / c[3];
        return true;
    }

    const char *skipName(Skip s)
    {
        switch (s)
        {
        case Skip::Exact: return "exact";
        case Skip::Still: return "still";
        case Skip::Cut: return "cut";
        case Skip::Unreplayable: return "unreplayable";
        case Skip::Budget: return "budget";
        case Skip::NotReady: return "not-ready";
        case Skip::NoPair: return "no-pair";
        default: return "?";
        }
    }

    const char *drawName(Draw d)
    {
        switch (d)
        {
        case Draw::Moved: return "moved";
        case Draw::Unmatched: return "unmatched";
        case Draw::Jumped: return "jumped";
        case Draw::NoMotion: return "no-motion";
        case Draw::Reprojected: return "reprojected";
        default: return "?";
        }
    }
}
