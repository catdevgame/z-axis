// The Thor's bottom screen in the collection's menu (collection/menu_bottom.h).
#include "collection/menu_bottom.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace zx::collection
{
    namespace
    {
        // The layout is made for 1240 x 1080 and scaled to the surface.
        constexpr int kDesignWidth = 1240, kDesignHeight = 1080;
        constexpr int kMargin = 40, kCardGap = 30, kButtonGap = 20, kHeadGap = 14;
        constexpr int kHeadSize = 110;
        constexpr float kCardAspect = 1.7f; // the title pictures' shape (AI's 640 x 348)

        uint32_t dimmed(uint32_t colour, int divisor)
        {
            return rgba(static_cast<uint8_t>((colour & 0xFF) / divisor), static_cast<uint8_t>((colour >> 8 & 0xFF) / divisor),
                        static_cast<uint8_t>((colour >> 16 & 0xFF) / divisor));
        }

        uint32_t withAlpha(uint32_t colour, uint8_t alpha) { return (colour & 0x00FFFFFFu) | static_cast<uint32_t>(alpha) << 24; }

        // A line centred in the box, a twentieth of its width clear at each side.
        void centred(Canvas &canvas, const BitmapFont &font, const Rect &box, const std::string &text, float scale, uint32_t colour)
        {
            const FittedLine line = fitLine(font, text, box.width - box.width / 10, scale, scale * 0.6f);
            const int height = static_cast<int>(std::lround(font.height * line.scale));
            canvas.text(font, box.x + (box.width - line.width) / 2, box.y + (box.height - height) / 2, line.text, line.scale, colour);
        }

        const char *buttonLabel(BottomButton button)
        {
            switch (button)
            {
            case BottomButton::CustomFreeplay: return kFreeplayTitle;
            case BottomButton::Settings: return "Settings";
            case BottomButton::Mods: return "Mods";
            case BottomButton::Quit: return "Quit";
            }
            return "";
        }
    }

    BottomLayout bottomLayout(int width, int height, size_t games, size_t heads, bool freeplay)
    {
        BottomLayout out;
        const float sx = static_cast<float>(width) / kDesignWidth, sy = static_cast<float>(height) / kDesignHeight;
        const auto X = [sx](int v) { return static_cast<int>(std::lround(v * sx)); };
        const auto Y = [sy](int v) { return static_cast<int>(std::lround(v * sy)); };
        const int left = X(kMargin), room = width - 2 * left;

        out.name = {left, Y(30), room, Y(60)};
        out.line = {left, Y(96), room, Y(36)};

        const int head = std::min(X(kHeadSize), Y(kHeadSize)), headGap = X(kHeadGap);
        const size_t fit = static_cast<size_t>(std::max(0, (room + headGap) / std::max(1, head + headGap)));
        for (size_t i = 0; i < std::min(heads, fit); ++i)
            out.heads.push_back({left + static_cast<int>(i) * (head + headGap), Y(150), head, head});

        if (games > 0)
        {
            const int gap = X(kCardGap);
            const int cardWidth = (room - gap * static_cast<int>(games - 1)) / static_cast<int>(games);
            const int cardHeight = std::min(static_cast<int>(cardWidth / kCardAspect), Y(230));
            const int total = cardWidth * static_cast<int>(games) + gap * static_cast<int>(games - 1);
            for (size_t i = 0; i < games; ++i)
                out.cards.push_back({left + (room - total) / 2 + static_cast<int>(i) * (cardWidth + gap),
                                     Y(300) + (Y(230) - cardHeight) / 2, cardWidth, cardHeight});
        }

        out.play = {left, Y(570), room, Y(150)};

        if (freeplay)
            out.kinds.push_back(BottomButton::CustomFreeplay);
        out.kinds.insert(out.kinds.end(), {BottomButton::Settings, BottomButton::Mods, BottomButton::Quit});
        const int gap = X(kButtonGap), count = static_cast<int>(out.kinds.size());
        const int buttonWidth = (room - gap * (count - 1)) / count;
        for (int i = 0; i < count; ++i)
            out.buttons.push_back({left + i * (buttonWidth + gap), Y(760), buttonWidth, Y(130)});

        out.hint = {left, Y(940), room, Y(50)};
        return out;
    }

    BottomHit bottomHit(const BottomLayout &layout, float x, float y)
    {
        BottomHit hit;
        for (size_t i = 0; i < layout.cards.size(); ++i)
            if (layout.cards[i].contains(x, y))
            {
                hit.kind = BottomHit::Card;
                hit.index = static_cast<int>(i);
                return hit;
            }
        if (layout.play.contains(x, y))
        {
            hit.kind = BottomHit::Play;
            return hit;
        }
        for (size_t i = 0; i < layout.buttons.size(); ++i)
            if (layout.buttons[i].contains(x, y))
            {
                hit.kind = BottomHit::Button;
                hit.index = static_cast<int>(i);
                hit.button = layout.kinds[i];
                return hit;
            }
        return hit;
    }

    const char *bottomPlayLabel(const FrontGame &game) { return game.installed ? "Play" : "Set Up"; }

    std::string bottomHelp(const FrontGame &game) { return std::string("Tap a game, then ") + bottomPlayLabel(game) + "."; }

    std::string bottomHint(const FrontGame &game, std::optional<HintDevice> pad)
    {
        std::string line = bottomHelp(game);
        if (pad && *pad != HintDevice::Touch && *pad != HintDevice::Keyboard)
            line += "   " + buttonHint(*pad, bottomPlayLabel(game));
        return line;
    }

    FittedText bottomLine(const BitmapFont &font, const std::string &line, int width, float scale)
    {
        return fitText(font, line, width, 1.3f * scale, 1.0f * scale);
    }

    namespace
    {
        // A game's line (bottomLine) from (x, y) down, a line under another.
        void drawLine(Canvas &canvas, const BitmapFont &font, int x, int y, int width, const std::string &text, float scale)
        {
            const FittedText fitted = bottomLine(font, text, width, scale);
            const int pitch = static_cast<int>(std::lround(font.height * fitted.scale));
            for (size_t i = 0; i < fitted.lines.size(); ++i)
                canvas.text(font, x, y + static_cast<int>(i) * pitch, fitted.lines[i], fitted.scale, rgba(200, 200, 200));
        }
    }

    // A setup or update running: what it does, and each game's part as the
    // top screen's rows show it, a bar under each; nothing to tap.
    void drawBusy(Canvas &canvas, const BottomLayout &layout, const BottomView &view, const BitmapFont &font)
    {
        const float scale = static_cast<float>(canvas.height()) / kDesignHeight;
        const auto Y = [scale](int v) { return static_cast<int>(std::lround(v * scale)); };
        canvas.clear(rgba(12, 12, 14));
        canvas.text(font, layout.name.x, layout.name.y, view.busy, 2.4f * scale, rgba(255, 255, 255));
        if (!view.busyLine.empty())
            canvas.text(font, layout.line.x, layout.line.y, view.busyLine, 1.3f * scale, rgba(200, 200, 200));
        const std::vector<FrontGame> empty;
        const std::vector<FrontGame> &games = view.games ? *view.games : empty;
        const int left = layout.name.x, room = layout.name.width;
        int y = Y(200);
        for (size_t row = 0; row < view.busyGames.size(); ++row)
        {
            const size_t game = static_cast<size_t>(view.busyGames[row]);
            if (game < view.names.size())
                canvas.text(font, left, y, view.names[game], 1.6f * scale, rgba(235, 235, 235));
            if (game < view.lines.size())
                drawLine(canvas, font, left, y + Y(48), room, view.lines[game], scale);
            const Rect bar{left, y + Y(96), room, std::max(2, Y(8))};
            canvas.fill(bar, rgba(255, 255, 255, 40));
            if (row < view.busyDone.size() && view.busyDone[row] > 0.0f)
            {
                const uint32_t colour = game < games.size() ? games[game].colour : rgba(220, 160, 60);
                canvas.fill({bar.x, bar.y, static_cast<int>(std::lround(bar.width * std::min(1.0f, view.busyDone[row]))), bar.height},
                            colour);
            }
            y += Y(150);
        }
        if (!view.busyHint.empty())
            centred(canvas, font, layout.hint, view.busyHint, 1.4f * scale, rgba(190, 190, 190));
    }

    void drawBottom(Canvas &canvas, const BottomLayout &layout, const BottomView &view, const BitmapFont &font)
    {
        if (!view.busy.empty())
        {
            drawBusy(canvas, layout, view, font);
            return;
        }
        const std::vector<FrontGame> empty;
        const std::vector<FrontGame> &games = view.games ? *view.games : empty;
        const int chosen = games.empty() ? -1 : std::clamp(view.chosen, 0, static_cast<int>(games.size()) - 1);
        const FrontGame *game = chosen >= 0 ? &games[static_cast<size_t>(chosen)] : nullptr;
        const float scale = static_cast<float>(canvas.height()) / kDesignHeight;
        const Rect screen{0, 0, canvas.width(), canvas.height()};

        // The chosen game's picture, dark, behind it all; a game not set up
        // its colour.
        canvas.clear(rgba(12, 12, 14));
        if (game && !game->card.empty())
            canvas.draw(game->card, game->cardFocus.empty() ? Rect{0, 0, game->card.width, game->card.height} : game->cardFocus,
                        screen, Fit::Cover, 0.2f);
        else if (game)
            canvas.fill(screen, dimmed(game->colour, 6));

        if (game && static_cast<size_t>(chosen) < view.names.size())
            canvas.text(font, layout.name.x, layout.name.y, view.names[static_cast<size_t>(chosen)], 2.4f * scale, rgba(255, 255, 255));
        if (game && static_cast<size_t>(chosen) < view.lines.size())
            drawLine(canvas, font, layout.line.x, layout.line.y, layout.line.width, view.lines[static_cast<size_t>(chosen)], scale);

        if (view.heads)
            for (size_t i = 0; i < layout.heads.size() && i < view.heads->size(); ++i)
            {
                const Image &head = (*view.heads)[i];
                canvas.draw(head, {0, 0, head.width, head.height}, layout.heads[i], Fit::Contain);
            }

        const int frame = std::max(2, static_cast<int>(std::lround(5 * scale)));
        for (size_t i = 0; i < layout.cards.size() && i < games.size(); ++i)
        {
            const FrontGame &card = games[i];
            const Rect &box = layout.cards[i];
            const bool isChosen = static_cast<int>(i) == chosen;
            if (!card.card.empty())
                canvas.draw(card.card, card.cardFocus.empty() ? Rect{0, 0, card.card.width, card.card.height} : card.cardFocus, box,
                            Fit::Cover, isChosen ? 1.0f : 0.55f);
            else
            {
                canvas.fill(box, dimmed(card.colour, isChosen ? 3 : 5));
                if (i < view.names.size())
                    centred(canvas, font, {box.x, box.y, box.width, box.height / 2 + box.height / 6}, view.names[i], 1.4f * scale,
                            rgba(235, 235, 235));
                centred(canvas, font, {box.x, box.y + box.height / 2, box.width, box.height / 3}, "Set Up", 1.2f * scale,
                        rgba(190, 190, 190));
            }
            if (isChosen)
                canvas.frame({box.x - frame, box.y - frame, box.width + 2 * frame, box.height + 2 * frame}, frame, card.colour);
        }

        if (game)
        {
            canvas.fill(layout.play, withAlpha(game->colour, 230));
            centred(canvas, font, layout.play, bottomPlayLabel(*game), 3.0f * scale, rgba(255, 255, 255));
        }
        for (size_t i = 0; i < layout.buttons.size(); ++i)
        {
            canvas.fill(layout.buttons[i], rgba(255, 255, 255, 40));
            canvas.frame(layout.buttons[i], std::max(1, frame / 2), rgba(220, 220, 220));
            centred(canvas, font, layout.buttons[i], buttonLabel(layout.kinds[i]), 1.5f * scale, rgba(235, 235, 235));
        }
        if (game)
            centred(canvas, font, layout.hint, bottomHint(*game, view.pad), 1.4f * scale, rgba(190, 190, 190));
    }

    // ---- BottomScreen ----------------------------------------------------------

    BottomScreen &BottomScreen::shared()
    {
        static BottomScreen screen;
        return screen;
    }

    void BottomScreen::show(std::shared_ptr<const BottomScene> scene)
    {
        std::lock_guard lock(m_mutex);
        if (!scene)
        {
            m_down = false;
            m_taps.clear();
        }
        m_scene = std::move(scene);
    }

    std::shared_ptr<const BottomScene> BottomScreen::scene() const
    {
        std::lock_guard lock(m_mutex);
        return m_scene;
    }

    bool BottomScreen::draw(Canvas &canvas)
    {
        const std::shared_ptr<const BottomScene> shown = scene();
        if (!shown || !shown->games)
            return false;
        const size_t heads = shown->heads ? shown->heads->size() : 0;
        const BottomLayout layout = bottomLayout(canvas.width(), canvas.height(), shown->games->size(), heads, shown->freeplay);
        BottomView view;
        view.games = shown->games.get();
        view.names = shown->names;
        view.lines = shown->lines;
        view.heads = shown->heads.get();
        view.chosen = shown->chosen;
        view.pad = shown->pad;
        view.busy = shown->busy;
        view.busyLine = shown->busyLine;
        view.busyHint = shown->busyHint;
        view.busyGames = shown->busyGames;
        view.busyDone = shown->busyDone;
        static const BitmapFont kNoFont;
        drawBottom(canvas, layout, view, shown->font ? *shown->font : kNoFont);
        return true;
    }

    BottomHit BottomScreen::hitAt(float x, float y, int width, int height) const
    {
        if (!m_scene || !m_scene->games || !m_scene->busy.empty()) // (busy: nothing to tap)
            return {};
        const size_t heads = m_scene->heads ? m_scene->heads->size() : 0;
        return bottomHit(bottomLayout(width, height, m_scene->games->size(), heads, m_scene->freeplay), x, y);
    }

    void BottomScreen::press(float x, float y, int width, int height)
    {
        std::lock_guard lock(m_mutex);
        m_pressed = hitAt(x, y, width, height);
        m_down = true;
    }

    void BottomScreen::release(float x, float y, int width, int height)
    {
        std::lock_guard lock(m_mutex);
        if (!m_down)
            return;
        m_down = false;
        const BottomHit hit = hitAt(x, y, width, height);
        if (hit.kind != BottomHit::None && hit.kind == m_pressed.kind && hit.index == m_pressed.index)
            m_taps.push_back(hit);
    }

    std::vector<BottomHit> BottomScreen::takeTaps()
    {
        std::lock_guard lock(m_mutex);
        return std::exchange(m_taps, {});
    }
}
