#ifndef PS2_NOTICE_OVERLAY_H
#define PS2_NOTICE_OVERLAY_H

// What the app draws over the game for the player, on every platform: the
// player's messages (zx::settings::notify: a screenshot saved, photo mode's
// help, a recording's playback line), the frame rate counter (the Show
// Frame Rate setting) and photo mode's framing grid. Dear ImGui on the SDL3
// window, drawn with SDL_GPU over the game picture (ps2_host::setOverlay).
// The debug panel (debug_panel.h) draws its windows in the same frame
// (draw's `windows`); a build without it draws these alone.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImFont;

class PS2NoticeOverlay
{
public:
    // Once the host window is up (the runtime's debug UI callbacks). False
    // when there is no window or GPU device (headless runs): nothing drawn.
    bool initialize();
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // The counter in the top right corner (the app's Show Frame Rate).
    void setShowFps(bool show) { m_showFps = show; }
    // The messages in the app's own look: its menus' font and the colour of
    // their help line (RGBA, R in the low byte). Before initialize; none:
    // ImGui's font, in white. A TrueType file, or its bytes (Android: the
    // APK's assets, which are no file).
    void setNoticeStyle(std::string fontFile, uint32_t colour);
    void setNoticeFont(std::vector<uint8_t> ttf, uint32_t colour);

    // One host frame (the window thread): the grid, the counter and the
    // message, then `windows` when given (the debug panel's, ImGui calls
    // inside the frame). With none of them to draw no frame is made.
    void draw(const std::function<void()> &windows = {});

private:
    void drawFps();
    void drawNotice(const std::string &text);

    bool m_initialized = false;
    bool m_frameReady = false; // an ImGui frame to draw over this host frame
    bool m_showFps = false;
    double m_fps = 0.0;
    unsigned long long m_fpsFlips = 0;
    double m_fpsSince = 0.0;
    std::string m_noticeFontFile;
    std::vector<uint8_t> m_noticeFontBytes; // (ImGui reads them in place: kept while it runs)
    uint32_t m_noticeColour = 0xFFFFFFFFu;
    ImFont *m_noticeFont = nullptr;
};

#endif // PS2_NOTICE_OVERLAY_H
