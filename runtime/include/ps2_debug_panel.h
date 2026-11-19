#ifndef PS2_DEBUG_PANEL_H
#define PS2_DEBUG_PANEL_H

#include <string>

class PS2Runtime;

class PS2DebugPanel
{
public:
    void initialize();
    void shutdown();
    void draw(PS2Runtime &runtime);

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; }
    void toggleVisible() { m_visible = !m_visible; }
    // A frame rate counter in the top right corner, also while the panel
    // is hidden (the app's Show Frame Rate setting).
    void setShowFps(bool show) { m_showFps = show; }

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
    bool m_showRegisters = true;
    unsigned int m_memoryAddress = 0x00100000u;
    unsigned int m_memoryBytes = 0x100u;
};

#endif // PS2_DEBUG_PANEL_H
