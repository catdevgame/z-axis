// The player's messages, the frame rate counter and photo mode's framing
// grid over the game (notice_overlay.h).

#include "notice_overlay.h"
#include "machine.h"
#include "runtime/photo.h"
#include "runtime/settings_registry.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "host_sdl.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <utility>

namespace
{
    // Photo mode's framing grid (zx::photo::framingGrid) over the picture
    // as shown, under the message: light lines with a dark edge, seen on
    // bright and dark scenes alike. Drawn over the window only, so the
    // saved pictures have none.
    void drawFramingGrid(const zx::photo::FramingGrid &grid)
    {
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        const SDL_FRect shown = ps2_host::shownPicture();
        if (shown.w <= 0.0f || shown.h <= 0.0f)
            return;
        const float x = viewport->Pos.x + shown.x * viewport->Size.x, y = viewport->Pos.y + shown.y * viewport->Size.y;
        const float scale = SDL_GetWindowDisplayScale(ps2_host::sdlWindow());
        const float width = 1.0f * (scale > 0.0f ? scale : 1.0f);
        ImDrawList *draw = ImGui::GetBackgroundDrawList();
        for (const zx::photo::Line &line :
             zx::photo::thirdsLines(x, y, shown.w * viewport->Size.x, shown.h * viewport->Size.y, grid.letterbox))
        {
            const ImVec2 from(line.x0, line.y0), to(line.x1, line.y1);
            draw->AddLine(from, to, IM_COL32(0, 0, 0, 110), width * 3.0f);
            draw->AddLine(from, to, IM_COL32(255, 255, 255, 170), width);
        }
    }

    constexpr ImGuiWindowFlags kLabelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
}

void PS2NoticeOverlay::setNoticeStyle(std::string fontFile, uint32_t colour)
{
    m_noticeFontFile = std::move(fontFile);
    m_noticeFontBytes.clear();
    m_noticeColour = colour;
}

void PS2NoticeOverlay::setNoticeFont(std::vector<uint8_t> ttf, uint32_t colour)
{
    m_noticeFontFile.clear();
    m_noticeFontBytes = std::move(ttf);
    m_noticeColour = colour;
}

bool PS2NoticeOverlay::initialize()
{
    if (m_initialized)
        return true;
    if (!ps2_host::sdlWindow() || !ps2_host::gpuDevice())
        return false;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    // ImGui's own font first (the default: the debug panel's windows), then
    // the messages' (setNoticeStyle), when its file or bytes read.
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->AddFontDefault();
    m_noticeFont = nullptr;
    if (!m_noticeFontFile.empty())
    {
        std::ifstream file(m_noticeFontFile, std::ios::binary);
        m_noticeFontBytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    if (!m_noticeFontBytes.empty())
    {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        m_noticeFont = io.Fonts->AddFontFromMemoryTTF(m_noticeFontBytes.data(), static_cast<int>(m_noticeFontBytes.size()),
                                                      16.0f, &config);
    }
    if (!m_noticeFont && (!m_noticeFontFile.empty() || !m_noticeFontBytes.empty()))
        std::fprintf(stderr, "[notices] notice font %s not read: ImGui's own\n",
                     m_noticeFontFile.empty() ? "(bytes)" : m_noticeFontFile.c_str());
    ImGui_ImplSDL3_InitForSDLGPU(ps2_host::sdlWindow());
    ImGui_ImplSDLGPU3_InitInfo info{};
    info.Device = ps2_host::gpuDevice();
    info.ColorTargetFormat = ps2_host::swapchainFormat();
    ImGui_ImplSDLGPU3_Init(&info);
    ps2_host::Overlay overlay;
    overlay.prepare = [this](SDL_GPUCommandBuffer *commands)
    {
        if (m_frameReady)
            ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), commands);
    };
    overlay.render = [this](SDL_GPUCommandBuffer *commands, SDL_GPURenderPass *pass)
    {
        if (m_frameReady)
            ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), commands, pass);
        m_frameReady = false;
    };
    ps2_host::setOverlay(std::move(overlay));
    m_initialized = true;
    return true;
}

void PS2NoticeOverlay::shutdown()
{
    if (!m_initialized)
        return;
    ps2_host::setOverlay({});
    SDL_WaitForGPUIdle(ps2_host::gpuDevice());
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
    m_frameReady = false;
}

// The game's frames per second (display buffer flips), measured over half
// a second, in a small box at the top right.
void PS2NoticeOverlay::drawFps()
{
    const double now = static_cast<double>(SDL_GetTicksNS()) / 1e9;
    const unsigned long long flips = PS2Machine::displayFlips();
    if (m_fpsSince == 0.0)
    {
        m_fpsSince = now;
        m_fpsFlips = flips;
    }
    else if (now - m_fpsSince >= 0.5)
    {
        m_fps = static_cast<double>(flips - m_fpsFlips) / (now - m_fpsSince);
        m_fpsSince = now;
        m_fpsFlips = flips;
    }
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = SDL_GetWindowDisplayScale(ps2_host::sdlWindow());
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 12.0f, viewport->WorkPos.y + 12.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("##fps", nullptr, kLabelFlags))
    {
        ImGui::SetWindowFontScale(scale > 0.0f ? scale * 1.25f : 1.25f);
        ImGui::Text("%.0f FPS", m_fps);
    }
    ImGui::End();
}

// A short message for the player (zx::settings::notify), top left.
void PS2NoticeOverlay::drawNotice(const std::string &text)
{
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float scale = SDL_GetWindowDisplayScale(ps2_host::sdlWindow());
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f, viewport->WorkPos.y + 12.0f), ImGuiCond_Always,
                            ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("##notice", nullptr, kLabelFlags))
    {
        // (the size ImGui's own font had here: 13 px, 1.25 times the display's scale)
        const float size = 13.0f * 1.25f * (scale > 0.0f ? scale : 1.0f);
        const uint32_t c = m_noticeColour;
        ImGui::PushFont(m_noticeFont, size);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4((c & 0xFFu) / 255.0f, (c >> 8 & 0xFFu) / 255.0f, (c >> 16 & 0xFFu) / 255.0f,
                                                    (c >> 24 & 0xFFu) / 255.0f));
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    ImGui::End();
}

void PS2NoticeOverlay::draw(const std::function<void()> &windows)
{
    m_frameReady = false;
    if (!m_initialized)
        return;
    const std::string notice = zx::settings::notice();
    const zx::photo::FramingGrid grid = zx::photo::framingGrid();
    if (!windows && !m_showFps && notice.empty() && !grid.shown)
        return;

    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (grid.shown)
        drawFramingGrid(grid);
    if (m_showFps)
        drawFps();
    if (!notice.empty())
        drawNotice(notice);
    if (windows)
        windows();
    ImGui::Render();
    m_frameReady = true;
}
