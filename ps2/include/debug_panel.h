#ifndef PS2_DEBUG_PANEL_H
#define PS2_DEBUG_PANEL_H

#include "notice_overlay.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class PS2Machine;

// The runtime debugger (F1): CPU, threads, kernel, IOP, the pads, files,
// GS, DMA and memory, in Dear ImGui windows over the game. It draws them in
// the frame of the player's messages (notice_overlay.h), which it owns.
class PS2DebugPanel
{
public:
    void initialize();
    void shutdown();
    void draw(PS2Machine &runtime);

    bool isVisible() const { return m_visible; }
    void setVisible(bool visible) { m_visible = visible; }
    void toggleVisible() { m_visible = !m_visible; }
    // The gamepad test: the panel shown at its PAD tab (what the game reads
    // from each pad), or hidden again when it is up, at the next frame: for
    // a device with no F1 key (Settings > Controls > Gamepad Test, with the
    // Developer Menu on). Any thread.
    void togglePadTest() { m_padTestToggle = true; }
    // A frame rate counter in the top right corner, also while the panel
    // is hidden (the app's Show Frame Rate setting).
    void setShowFps(bool show) { m_notices.setShowFps(show); }
    // The player's messages (zx::settings::notify) in the app's own look
    // (PS2NoticeOverlay::setNoticeStyle, setNoticeFont). Before initialize.
    // The debug windows keep ImGui's font.
    void setNoticeStyle(std::string fontFile, uint32_t colour) { m_notices.setNoticeStyle(std::move(fontFile), colour); }
    void setNoticeFont(std::vector<uint8_t> ttf, uint32_t colour) { m_notices.setNoticeFont(std::move(ttf), colour); }

private:
    void drawWindows(PS2Machine &runtime);

    PS2NoticeOverlay m_notices;
    bool m_initialized = false;
    bool m_visible = true;
    bool m_selectPadTab = false;          // the PAD tab chosen at the next frame
    std::atomic<bool> m_padTestToggle{false}; // togglePadTest, for the window thread
    bool m_showRegisters = true;
    unsigned int m_memoryAddress = 0x00100000u;
    unsigned int m_memoryBytes = 0x100u;
};

#endif // PS2_DEBUG_PANEL_H
