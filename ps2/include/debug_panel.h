#ifndef PS2_DEBUG_PANEL_H
#define PS2_DEBUG_PANEL_H

#include <cstdint>
#include <string>

class PS2Machine;
struct ImFont;

class PS2DebugPanel
{
public:
    void initialize();
    void shutdown();
    void draw(PS2Machine &runtime);

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; }
    void toggleVisible() { m_visible = !m_visible; }
    // A frame rate counter in the top right corner, also while the panel
    // is hidden (the app's Show Frame Rate setting).
    void setShowFps(bool show) { m_showFps = show; }
    // The player's messages (zx::settings::notify: a screenshot saved, a
    // recording's playback line) in the app's own look: its menus' font (a
    // TrueType file) and the colour of their help line (RGBA, R in the low
    // byte). Before initialize; none: ImGui's font, in white. The debug
    // windows keep ImGui's font.
    void setNoticeStyle(std::string fontFile, uint32_t colour)
    {
        m_noticeFontFile = std::move(fontFile);
        m_noticeColour = colour;
    }

private:
    bool m_initialized = false;
    bool m_frameReady = false; // an ImGui frame to draw over this host frame
    bool m_visible = true;
    bool m_showFps = false;
    double m_fps = 0.0;
    unsigned long long m_fpsFlips = 0;
    double m_fpsSince = 0.0;
    void drawFps();
    void drawNotice(const std::string &text);
    std::string m_noticeFontFile;
    uint32_t m_noticeColour = 0xFFFFFFFFu;
    ImFont *m_noticeFont = nullptr;
    bool m_showRegisters = true;
    unsigned int m_memoryAddress = 0x00100000u;
    unsigned int m_memoryBytes = 0x100u;
};

#endif // PS2_DEBUG_PANEL_H
