#include "runtime/photo.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace zx::photo
{
    namespace
    {
        constexpr float kDegrees = 3.14159265358979f / 180.0f;

        float clampf(float v, float lo, float hi)
        {
            return std::min(hi, std::max(lo, v));
        }

        float wrapDegrees(float degrees)
        {
            degrees = std::fmod(degrees, 360.0f);
            if (degrees > 180.0f)
                degrees -= 360.0f;
            if (degrees <= -180.0f)
                degrees += 360.0f;
            return degrees;
        }

        // The horizontal direction at `yaw`, as (sin, 0, cos) in the plane
        // across the world's up (y up or down alike).
        Vec3 heading(float yaw)
        {
            return {std::sin(yaw * kDegrees), 0.0f, std::cos(yaw * kDegrees)};
        }

        uint8_t toByte(float v)
        {
            return static_cast<uint8_t>(clampf(v + 0.5f, 0.0f, 255.0f));
        }
    }

    Vec3 add(const Vec3 &a, const Vec3 &b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    Vec3 scale(const Vec3 &a, float s) { return {a.x * s, a.y * s, a.z * s}; }
    float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3 cross(const Vec3 &a, const Vec3 &b)
    {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    float length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
    Vec3 normalized(const Vec3 &a)
    {
        const float l = length(a);
        return l > 1e-9f ? scale(a, 1.0f / l) : Vec3{};
    }

    void FreeCamera::lookFrom(const Vec3 &eye, const Vec3 &target, float fieldOfView)
    {
        position = eye;
        home = eye;
        const Vec3 direction = normalized(add(target, scale(eye, -1.0f)));
        const float upward = clampf(dot(direction, normalized(worldUp)), -1.0f, 1.0f);
        pitch = clampf(std::asin(upward) / kDegrees, -89.0f, 89.0f);
        yaw = std::atan2(direction.x, direction.z) / kDegrees;
        roll = 0.0f;
        fov = clampf(fieldOfView, minFov, maxFov);
    }

    Vec3 FreeCamera::forward() const
    {
        const float p = pitch * kDegrees;
        return add(scale(heading(yaw), std::cos(p)), scale(normalized(worldUp), std::sin(p)));
    }

    Vec3 FreeCamera::right() const
    {
        return heading(yaw + 90.0f);
    }

    Vec3 FreeCamera::up() const
    {
        Vec3 u = normalized(cross(right(), forward()));
        if (dot(u, worldUp) < 0.0f)
            u = scale(u, -1.0f);
        return u;
    }

    void FreeCamera::update(const Controls &c, float seconds)
    {
        seconds = clampf(seconds, 0.0f, 0.1f);
        yaw = wrapDegrees(yaw + c.lookYaw + c.turnYaw * turnRate * seconds);
        pitch = clampf(pitch + c.lookPitch + c.turnPitch * turnRate * seconds, -89.0f, 89.0f);
        roll = clampf(roll + c.roll * rollRate * seconds, -maxRoll, maxRoll);
        fov = clampf(fov - c.zoom * fovStep, minFov, maxFov);
        const float step = speed * std::max(0.0f, c.speedFactor) * seconds;
        // Forward along the view, sideways level, up along the world's up.
        Vec3 move = scale(forward(), c.moveForward * step);
        move = add(move, scale(right(), c.moveRight * step));
        move = add(move, scale(normalized(worldUp), c.moveUp * step));
        position = add(position, move);
        if (maxDistance > 0.0f)
        {
            const Vec3 offset = add(position, scale(home, -1.0f));
            const float distance = length(offset);
            if (distance > maxDistance)
                position = add(home, scale(offset, maxDistance / distance));
        }
    }

    void rollRows(Vec3 &right, Vec3 &up, float degrees)
    {
        const float c = std::cos(degrees * kDegrees), s = std::sin(degrees * kDegrees);
        const Vec3 r = add(scale(right, c), scale(up, -s));
        const Vec3 u = add(scale(up, c), scale(right, s));
        right = r;
        up = u;
    }

    const std::vector<Preset> &presets()
    {
        static const std::vector<Preset> list = {
            {"None", 0u},
            {"Letterbox", kFilterLetterbox},
            {"Vignette", kFilterVignette},
            {"Black & White", kFilterMono},
            {"Cinema", kFilterLetterbox | kFilterVignette},
            {"Noir", kFilterLetterbox | kFilterVignette | kFilterMono},
        };
        return list;
    }

    namespace
    {
        constexpr float kLetterboxAspect = 2.39f;

        // The height of each letterbox bar in a picture `height` high shown
        // at `displayAspect`: none for a picture as wide as the bars' 2.39:1.
        float letterboxBar(float height, float displayAspect)
        {
            if (displayAspect <= 0.0f || displayAspect >= kLetterboxAspect)
                return 0.0f;
            return (height - height * displayAspect / kLetterboxAspect) * 0.5f;
        }
    }

    uint32_t letterboxRows(uint32_t height, float displayAspect)
    {
        return static_cast<uint32_t>(std::lround(letterboxBar(static_cast<float>(height), displayAspect)));
    }

    namespace
    {
        std::atomic<bool> g_gridShown{false};
        std::atomic<bool> g_gridLetterbox{false};
    }

    void setFramingGrid(FramingGrid grid)
    {
        g_gridLetterbox = grid.letterbox;
        g_gridShown = grid.shown;
    }

    FramingGrid framingGrid()
    {
        return {g_gridShown.load(), g_gridLetterbox.load()};
    }

    std::vector<Line> thirdsLines(float x, float y, float width, float height, bool letterbox)
    {
        if (width <= 0.0f || height <= 0.0f)
            return {};
        const float bar = letterbox ? letterboxBar(height, width / height) : 0.0f;
        const float top = y + bar, shown = height - 2.0f * bar;
        std::vector<Line> lines;
        for (int third = 1; third <= 2; ++third)
        {
            const float across = x + width * static_cast<float>(third) / 3.0f;
            lines.push_back({across, top, across, top + shown});
        }
        for (int third = 1; third <= 2; ++third)
        {
            const float down = top + shown * static_cast<float>(third) / 3.0f;
            lines.push_back({x, down, x + width, down});
        }
        return lines;
    }

    void applyFilters(uint8_t *rgba, uint32_t width, uint32_t height, uint32_t stride, uint32_t filters,
                      float displayAspect)
    {
        if (!rgba || width == 0 || height == 0 || filters == 0u)
            return;
        const uint32_t bars = (filters & kFilterLetterbox) ? letterboxRows(height, displayAspect) : 0u;
        // Vignette: darker towards the corners, by the distance from the
        // centre on screen (the display aspect, not the pixel grid).
        std::vector<float> columnTerm;
        if (filters & kFilterVignette)
        {
            columnTerm.resize(width);
            const float aspect = displayAspect > 0.0f ? displayAspect : 4.0f / 3.0f;
            for (uint32_t x = 0; x < width; ++x)
            {
                const float u = ((static_cast<float>(x) + 0.5f) / static_cast<float>(width) - 0.5f) * 2.0f * aspect;
                columnTerm[x] = u * u;
            }
        }
        const float aspect = displayAspect > 0.0f ? displayAspect : 4.0f / 3.0f;
        // r^2 of a corner, for 0..1 across the picture.
        const float cornerSquared = aspect * aspect + 1.0f;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = rgba + static_cast<size_t>(y) * stride;
            if (y < bars || y >= height - bars)
            {
                for (uint32_t x = 0; x < width; ++x)
                    row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = 0u;
                continue;
            }
            const float v = ((static_cast<float>(y) + 0.5f) / static_cast<float>(height) - 0.5f) * 2.0f;
            const float rowTerm = v * v;
            for (uint32_t x = 0; x < width; ++x)
            {
                uint8_t *p = row + 4 * x;
                float r = p[0], g = p[1], b = p[2];
                if (filters & kFilterMono)
                {
                    // Rec. 601 luma, a touch more contrast.
                    const float luma = 0.299f * r + 0.587f * g + 0.114f * b;
                    r = g = b = clampf((luma - 128.0f) * 1.08f + 128.0f, 0.0f, 255.0f);
                }
                if (filters & kFilterVignette)
                {
                    const float t = clampf(((columnTerm[x] + rowTerm) / cornerSquared - 0.22f) / 0.78f, 0.0f, 1.0f);
                    const float shade = 1.0f - 0.65f * t * t * (3.0f - 2.0f * t);
                    r *= shade;
                    g *= shade;
                    b *= shade;
                }
                p[0] = toByte(r);
                p[1] = toByte(g);
                p[2] = toByte(b);
            }
        }
    }
}
