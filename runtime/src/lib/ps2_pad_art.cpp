#include "runtime/ps2_pad_art.h"

#include <algorithm>
#include <cmath>

namespace zx::glyphs
{
    namespace
    {
        // The pad in units of its width: x 0-1 left to right, y 0 to
        // 1 / kGamepadAspect top to bottom.
        struct Box
        {
            float x = 0.0f;
            float y = 0.0f;
            float s = 0.0f; // the pad's width in pixels
            float px(float u) const { return x + u * s; }
            float py(float v) const { return y + v * s; }
        };

        Box fit(float x, float y, float w, float h)
        {
            const float pw = std::max(0.0f, std::min(w, h * kGamepadAspect));
            const float ph = pw / kGamepadAspect;
            return {x + (w - pw) * 0.5f, y + (h - ph) * 0.5f, pw};
        }

        struct Spot
        {
            float u;
            float v;
            float r;
        };

        constexpr float kFaceU = 0.79f, kFaceV = 0.225f, kFaceStep = 0.068f, kFaceR = 0.038f;

        Spot spot(PadControl control)
        {
            switch (control)
            {
            case PadControl::South:
                return {kFaceU, kFaceV + kFaceStep, kFaceR};
            case PadControl::East:
                return {kFaceU + kFaceStep, kFaceV, kFaceR};
            case PadControl::West:
                return {kFaceU - kFaceStep, kFaceV, kFaceR};
            case PadControl::North:
                return {kFaceU, kFaceV - kFaceStep, kFaceR};
            case PadControl::L1:
                return {0.34f, 0.086f, 0.022f};
            case PadControl::R1:
                return {0.66f, 0.086f, 0.022f};
            case PadControl::L2:
                return {0.13f, 0.036f, 0.02f};
            case PadControl::R2:
                return {0.87f, 0.036f, 0.02f};
            case PadControl::Select:
                return {0.455f, 0.25f, 0.024f};
            case PadControl::Start:
                return {0.545f, 0.25f, 0.024f};
            case PadControl::LeftStick:
                return {0.25f, 0.2f, 0.07f};
            case PadControl::RightStick:
                return {0.63f, 0.355f, 0.062f};
            case PadControl::DPad:
                return {0.35f, 0.34f, 0.058f};
            default:
                return {0.5f, 0.3f, 0.0f};
            }
        }

        // A stick: its well, the cap lit from above, a dished top.
        void drawStick(Image &image, float cx, float cy, float r)
        {
            fillEllipse(image, cx, cy, r, r, rgba(12, 12, 14));
            Image cap(image.width, image.height);
            fillEllipse(cap, cx, cy - r * 0.04f, r * 0.8f, r * 0.8f, rgba(255, 255, 255));
            paintShadow(image, cap, r * 0.04f, r * 0.1f, std::max(1, static_cast<int>(r * 0.1f)), rgba(0, 0, 0, 200));
            paintShaded(image, cap, {rgba(122, 122, 130), rgba(36, 36, 40), std::max(1.0f, r * 0.08f), 0.5f, 0.5f});
            Image dish(image.width, image.height);
            fillEllipse(dish, cx, cy - r * 0.04f, r * 0.56f, r * 0.56f, rgba(255, 255, 255));
            paintShaded(image, dish, {rgba(40, 40, 46), rgba(84, 84, 92), std::max(1.0f, r * 0.05f), 0.0f, 0.0f});
        }

        // A shoulder button or trigger: a grey key, its label above the
        // body's edge.
        void drawShoulder(Image &image, const Box &b, float u0, float v0, float u1, float v1, const Font &font,
                          const std::string &label, float labelV, float labelU = -1.0f)
        {
            Image shape(image.width, image.height);
            const float radius = (v1 - v0) * b.s * 0.45f;
            fillRoundRect(shape, b.px(u0), b.py(v0), b.px(u1), b.py(v1), radius, rgba(255, 255, 255));
            paintShadow(image, shape, 0.0f, b.s * 0.006f, std::max(1, static_cast<int>(b.s * 0.006f)), rgba(0, 0, 0, 170));
            paintShaded(image, shape, {rgba(150, 150, 158), rgba(70, 70, 78), std::max(1.0f, b.s * 0.004f), 0.5f, 0.4f});
            const float h = b.s * 0.052f;
            drawBoldText(image, font, label, b.px(labelU >= 0.0f ? labelU : (u0 + u1) * 0.5f), b.py(labelV), h, rgba(24, 24, 28));
        }
    }

    Point padAnchor(PadControl control, float x, float y, float w, float h)
    {
        const Box b = fit(x, y, w, h);
        const Spot s = spot(control);
        return {b.px(s.u), b.py(s.v)};
    }

    float padControlRadius(PadControl control, float x, float y, float w, float h)
    {
        return spot(control).r * fit(x, y, w, h).s;
    }

    std::string padControlLabel(PadControl control, PadStyle style)
    {
        const bool nintendo = style == PadStyle::Nintendo;
        switch (control)
        {
        case PadControl::South:
            return nintendo ? "B" : "A";
        case PadControl::East:
            return nintendo ? "A" : "B";
        case PadControl::West:
            return nintendo ? "Y" : "X";
        case PadControl::North:
            return nintendo ? "X" : "Y";
        case PadControl::L1:
            return nintendo ? "L" : "LB";
        case PadControl::R1:
            return nintendo ? "R" : "RB";
        case PadControl::L2:
            return nintendo ? "ZL" : "LT";
        case PadControl::R2:
            return nintendo ? "ZR" : "RT";
        case PadControl::Select:
            return nintendo ? "-" : "BACK";
        case PadControl::Start:
            return nintendo ? "+" : "START";
        default:
            return {};
        }
    }

    void drawGamepad(Image &image, float x, float y, float w, float h, PadStyle style, const Font &font)
    {
        const Box b = fit(x, y, w, h);
        if (b.s < 16.0f)
            return;
        // Triggers (outer), then shoulder buttons (their inner ends free
        // above), behind the body.
        drawShoulder(image, b, 0.1f, 0.012f, 0.27f, 0.11f, font, padControlLabel(PadControl::L2, style), 0.036f, 0.2f);
        drawShoulder(image, b, 0.73f, 0.012f, 0.9f, 0.11f, font, padControlLabel(PadControl::R2, style), 0.036f, 0.8f);
        drawShoulder(image, b, 0.1f, 0.058f, 0.38f, 0.15f, font, padControlLabel(PadControl::L1, style), 0.086f, 0.23f);
        drawShoulder(image, b, 0.62f, 0.058f, 0.9f, 0.15f, font, padControlLabel(PadControl::R1, style), 0.086f, 0.77f);

        // The body: a wide top with two grips going down and out.
        Image body(image.width, image.height);
        const uint32_t white = rgba(255, 255, 255);
        fillRoundRect(body, b.px(0.07f), b.py(0.11f), b.px(0.93f), b.py(0.39f), b.s * 0.1f, white);
        fillRotatedEllipse(body, b.px(0.2f), b.py(0.4f), b.s * 0.115f, b.s * 0.2f, 0.45f, white);
        fillRotatedEllipse(body, b.px(0.8f), b.py(0.4f), b.s * 0.115f, b.s * 0.2f, -0.45f, white);
        fillRoundRect(body, b.px(0.24f), b.py(0.28f), b.px(0.76f), b.py(0.45f), b.s * 0.07f, white);
        paintShadow(image, body, b.s * 0.012f, b.s * 0.02f, std::max(1, static_cast<int>(b.s * 0.016f)), rgba(0, 0, 0, 190));
        paintShaded(image, body, {rgba(116, 116, 124), rgba(38, 38, 44), std::max(1.0f, b.s * 0.006f), 0.4f, 0.5f});
        // A lighter face plate between the grips.
        Image plate(image.width, image.height);
        fillRoundRect(plate, b.px(0.15f), b.py(0.14f), b.px(0.85f), b.py(0.3f), b.s * 0.07f, white);
        paintShaded(image, plate, {rgba(100, 100, 108), rgba(62, 62, 70), std::max(1.0f, b.s * 0.004f), 0.12f, 0.2f});

        const Spot ls = spot(PadControl::LeftStick), rs = spot(PadControl::RightStick);
        drawStick(image, b.px(ls.u), b.py(ls.v), ls.r * b.s);
        drawStick(image, b.px(rs.u), b.py(rs.v), rs.r * b.s);

        const Spot dp = spot(PadControl::DPad);
        const float dsize = dp.r * 2.0f * b.s;
        fillEllipse(image, b.px(dp.u), b.py(dp.v), dsize * 0.62f, dsize * 0.62f, rgba(20, 20, 24));
        drawGreyDpad(image, b.px(dp.u) - dsize * 0.5f, b.py(dp.v) - dsize * 0.5f, dsize, dsize);

        // The face buttons in a dark well; Xbox-style letters in their
        // usual colours.
        fillEllipse(image, b.px(kFaceU), b.py(kFaceV), (kFaceStep + kFaceR * 1.05f) * b.s, (kFaceStep + kFaceR * 1.05f) * b.s,
                    rgba(24, 24, 28, 200));
        for (PadControl c : {PadControl::South, PadControl::East, PadControl::West, PadControl::North})
        {
            const Spot f = spot(c);
            const float d = f.r * 2.0f * b.s;
            drawRoundButton(image, b.px(f.u) - d * 0.5f, b.py(f.v) - d * 0.5f, d, d, font,
                            style == PadStyle::Nintendo ? padControlLabel(c, style) : std::string());
            if (style == PadStyle::Xbox)
            {
                static const uint32_t kColors[] = {rgba(96, 200, 72), rgba(236, 64, 56), rgba(56, 132, 236), rgba(244, 200, 40)};
                drawBoldText(image, font, padControlLabel(c, style), b.px(f.u), b.py(f.v), d * 0.66f,
                             kColors[static_cast<int>(c)]);
            }
        }

        for (PadControl c : {PadControl::Select, PadControl::Start})
        {
            const Spot m = spot(c);
            const float d = m.r * 2.0f * b.s;
            if (style == PadStyle::Nintendo)
            {
                drawRoundButton(image, b.px(m.u) - d * 0.5f, b.py(m.v) - d * 0.5f, d, d, font, padControlLabel(c, style));
                continue;
            }
            drawRoundButton(image, b.px(m.u) - d * 0.5f, b.py(m.v) - d * 0.5f, d, d, font, std::string());
        }
    }

    void drawArrow(Image &image, Point from, Point bend, Point to, float width, uint32_t color, uint32_t outline)
    {
        // The head points along the curve's last stretch.
        float dx = to.x - bend.x, dy = to.y - bend.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len <= 0.0f)
            return;
        dx /= len;
        dy /= len;
        const float headL = width * 3.2f, headW = width * 2.1f;
        const Point base{to.x - dx * headL * 0.8f, to.y - dy * headL * 0.8f};
        const std::vector<float> head = {to.x, to.y, to.x - dx * headL - dy * headW, to.y - dy * headL + dx * headW,
                                         to.x - dx * headL + dy * headW, to.y - dy * headL - dx * headW};
        const float o = std::max(1.0f, width * 0.45f);
        // The outline: the same shapes wider, under them.
        strokeCurve(image, from.x, from.y, bend.x, bend.y, base.x, base.y, width + 2.0f * o, outline);
        std::vector<float> outer = head;
        for (size_t i = 0; i + 1 < outer.size(); i += 2)
        {
            // Grow the head from its centre.
            const float cx = (head[0] + head[2] + head[4]) / 3.0f, cy = (head[1] + head[3] + head[5]) / 3.0f;
            const float vx = outer[i] - cx, vy = outer[i + 1] - cy, vl = std::sqrt(vx * vx + vy * vy);
            if (vl > 0.0f)
            {
                outer[i] += vx / vl * o * 1.6f;
                outer[i + 1] += vy / vl * o * 1.6f;
            }
        }
        fillPolygon(image, outer, outline);
        strokeCurve(image, from.x, from.y, bend.x, bend.y, base.x, base.y, width, color);
        fillPolygon(image, head, color);
    }

    void drawCrossArrows(Image &image, float cx, float cy, float size, uint32_t color, uint32_t outline)
    {
        const float half = size * 0.5f, w = size * 0.075f;
        for (int pass = 0; pass < 2; ++pass)
        {
            const uint32_t c = pass == 0 ? outline : color;
            const float grow = pass == 0 ? std::max(1.0f, w * 0.45f) : 0.0f;
            const float shaft = half - w * 2.6f;
            strokeLine(image, cx - shaft, cy, cx + shaft, cy, w + 2.0f * grow, c);
            strokeLine(image, cx, cy - shaft, cx, cy + shaft, w + 2.0f * grow, c);
            const float hl = w * 3.0f + grow * 1.5f, hw = w * 2.0f + grow * 1.5f;
            const float tip = half + grow;
            fillPolygon(image, {cx, cy - tip, cx - hw, cy - tip + hl, cx + hw, cy - tip + hl}, c);
            fillPolygon(image, {cx, cy + tip, cx + hw, cy + tip - hl, cx - hw, cy + tip - hl}, c);
            fillPolygon(image, {cx - tip, cy, cx - tip + hl, cy + hw, cx - tip + hl, cy - hw}, c);
            fillPolygon(image, {cx + tip, cy, cx + tip - hl, cy - hw, cx + tip - hl, cy + hw}, c);
        }
    }
}
