// Button prompts for the device in use (zge_prompts.h), one body for the
// games built on the Z-Axis engine.

#include "zge_prompts.h"

#include "ps2_runtime.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <mutex>
#include <set>

namespace zge_host::prompts
{
    namespace
    {
        namespace gl = ps2x::glyphs;
        namespace hi = ps2_host_input;
        namespace zx = ps2x::zaxis;

        constexpr int kRegA0 = 4, kRegA1 = 5, kRegA2 = 6, kRegA3 = 7, kRegSp = 29, kRegRa = 31;

        struct Installed
        {
            const Game *game = nullptr;
            int trace = 0; // PS2X_TRACE_PROMPTS=1, 2: every texture, 3: every text drawn
        };
        Installed &installed() { return ps2x::session::state<Installed>(); }

        uint32_t readWord(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            if (const uint8_t *p = getConstMemPtr(rdram, address))
                std::memcpy(&value, p, sizeof(value));
            return value;
        }

        uint64_t readDword(const uint8_t *rdram, uint32_t address)
        {
            return static_cast<uint64_t>(readWord(rdram, address)) |
                   static_cast<uint64_t>(readWord(rdram, address + 4u)) << 32;
        }

        std::string readString(const uint8_t *rdram, uint32_t address, size_t limit)
        {
            std::string out;
            for (size_t i = 0; i < limit; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, address + static_cast<uint32_t>(i));
                if (!p || *p == 0u)
                    break;
                out.push_back(static_cast<char>(*p));
            }
            return out;
        }

        // ---- art from the disc (the UI library, read when the game opens it)

        constexpr size_t kDevices = 4u; // Device's values
        struct Art
        {
            std::mutex mutex;
            bool captured = false;
            gl::Font label;                                  // key cap and button labels
            std::array<std::map<int, gl::Image>, kDevices> buttons; // by device, by physical button (kDPadArt)
            std::map<std::string, zx::LibraryFont> fonts;    // by atlas texture name
        };
        Art &art() { return ps2x::session::state<Art>(); }

        const gl::Image *artFor(Device device, int button)
        {
            const auto &pictures = art().buttons[static_cast<size_t>(device)];
            const auto it = pictures.find(button);
            return it == pictures.end() ? nullptr : &it->second;
        }

        // A glyph of a font as a picture of its own.
        gl::Image glyphImage(const zx::LibraryFont &font, const zx::FontGlyph &g)
        {
            gl::Image image(g.width, g.height);
            for (int y = 0; y < g.height; ++y)
                for (int x = 0; x < g.width; ++x)
                {
                    const int ax = g.x + x, ay = g.y + y;
                    if (ax < font.atlas.width && ay < font.atlas.height)
                        image.pixels[static_cast<size_t>(y) * g.width + static_cast<size_t>(x)] =
                            font.atlas.rgba[static_cast<size_t>(ay) * static_cast<size_t>(font.atlas.width) +
                                            static_cast<size_t>(ax)];
                }
            return image;
        }

        // ---- painting a button for a device -----------------------------------

        // A picture in a square of the rectangle's height (or width), its
        // aspect kept, centred.
        void drawFitted(gl::Image &image, const gl::Image &picture, float x, float y, float w, float h)
        {
            const float s = std::min(w, h);
            const float k = s / static_cast<float>(std::max(picture.width, picture.height));
            const float pw = static_cast<float>(picture.width) * k, ph = static_cast<float>(picture.height) * k;
            gl::drawImage(image, picture, x + (w - pw) * 0.5f, y + (h - ph) * 0.5f, pw, ph);
        }

        void paintKey(gl::Image &image, float x, float y, float w, float h, int code, bool pressed = false)
        {
            if (hi::isMouseCode(code))
            {
                gl::drawMouse(image, x, y, w, h, code - hi::kMouseCodeBase);
                return;
            }
            const std::string label = code < 0 ? "?" : hi::hostCodeLabel(code);
            // Key caps are wider than tall for long names.
            const float capH = std::min(h, w * 0.86f);
            gl::drawKeycap(image, x, y + (h - capH) * 0.5f, w, capH, art().label, label, pressed);
        }

        // The arrow keys as four small caps (the d-pad).
        void paintArrowKeys(gl::Image &image, float x, float y, float w, float h)
        {
            const Art &a = art();
            const float s = std::min(w / 3.0f, h / 2.0f);
            const float x0 = x + (w - 3.0f * s) * 0.5f, y0 = y + (h - 2.0f * s) * 0.5f;
            const auto label = [](int button, const char *fallback)
            {
                for (int code : PSPadBackend::keyBindings(button))
                    if (!hi::isMouseCode(code))
                        return hi::hostCodeLabel(code);
                return std::string(fallback);
            };
            gl::drawKeycap(image, x0 + s, y0, s, s, a.label, label(hi::kUp, "Up"));
            gl::drawKeycap(image, x0, y0 + s, s, s, a.label, label(hi::kLeft, "Left"));
            gl::drawKeycap(image, x0 + s, y0 + s, s, s, a.label, label(hi::kDown, "Down"));
            gl::drawKeycap(image, x0 + 2.0f * s, y0 + s, s, s, a.label, label(hi::kRight, "Right"));
        }

        // A stick's four keys as a cross (up above left, down, right); none
        // bound: nothing drawn.
        void paintStickKeys(gl::Image &image, float x, float y, float w, float h, bool left)
        {
            const int first = left ? hi::kLeftUp : hi::kRightUp;
            int codes[4] = {-1, -1, -1, -1}; // up, down, left, right (StickDirection's order)
            bool any = false;
            for (int i = 0; i < 4; ++i)
            {
                const std::vector<int> keys = PSPadBackend::stickKeyBindings(first + i);
                if (!keys.empty())
                {
                    codes[i] = keys.front();
                    any = true;
                }
            }
            if (!any)
                return;
            const float s = std::min(w / 3.0f, h / 2.0f);
            const float x0 = x + (w - 3.0f * s) * 0.5f, y0 = y + (h - 2.0f * s) * 0.5f;
            paintKey(image, x0 + s, y0, s, s, codes[0]);
            paintKey(image, x0, y0 + s, s, s, codes[2]);
            paintKey(image, x0 + s, y0 + s, s, s, codes[1]);
            paintKey(image, x0 + 2.0f * s, y0 + s, s, s, codes[3]);
        }

        bool faceButton(int physical)
        {
            return physical == hi::kCross || physical == hi::kCircle || physical == hi::kSquare || physical == hi::kTriangle;
        }

        // A game button as the device shows it, fitted into the rectangle.
        void paintButton(gl::Image &image, float x, float y, float w, float h, const Picture &picture, Device device)
        {
            const Art &a = art();
            if (picture.shows == Shows::Pad)
                return; // the photo's own (redrawPad)
            if (picture.shows == Shows::LeftStick || picture.shows == Shows::RightStick)
            {
                // A pad's sticks are the photo's; the keyboard's keys for them.
                if (device == Device::Keyboard)
                    paintStickKeys(image, x, y, w, h, picture.shows == Shows::LeftStick);
                return;
            }
            if (picture.shows == Shows::DPad)
            {
                if (device == Device::Keyboard)
                {
                    paintArrowKeys(image, x, y, w, h);
                    return;
                }
                const float s = std::min(w, h);
                const gl::Image *own = device == Device::PlayStation ? nullptr : artFor(device, kDPadArt);
                if (own)
                    drawFitted(image, *own, x, y, w, h);
                else if (device == Device::Nintendo)
                    gl::drawGreyDpad(image, x + (w - s) * 0.5f, y + (h - s) * 0.5f, s, s);
                else
                    gl::drawDpad(image, x + (w - s) * 0.5f, y + (h - s) * 0.5f, s, s);
                return;
            }
            if (picture.shows == Shows::ButtonLeft || picture.shows == Shows::ButtonRight)
            {
                // An arrow beside the button, as AI's park editor's L1/R1.
                const float aw = h * 0.55f;
                const bool left = picture.shows == Shows::ButtonLeft;
                const float ax = left ? x + 1.0f : x + w - 1.0f - aw;
                const uint32_t arrow = picture.lit ? gl::rgba(120, 190, 255) : gl::rgba(220, 220, 226);
                const float cy = y + h * 0.5f;
                if (left)
                    gl::fillPolygon(image, {ax, cy, ax + aw, cy - h * 0.34f, ax + aw, cy + h * 0.34f}, arrow);
                else
                    gl::fillPolygon(image, {ax + aw, cy, ax, cy - h * 0.34f, ax, cy + h * 0.34f}, arrow);
                Picture plain = picture;
                plain.shows = Shows::Button;
                plain.lit = false;
                const float bx = left ? x + aw + 2.0f : x;
                paintButton(image, bx, y, w - aw - 2.0f, h, plain, device);
                return;
            }
            if (picture.wide)
            {
                // As wide as the cell, about as tall as the picture it replaces.
                const float ph = std::min(h, w * 0.62f);
                y += (h - ph) * 0.5f;
                h = ph;
            }
            if (device == Device::Keyboard)
            {
                paintKey(image, x, y, w, h, firstKey(picture.button), picture.lit);
                return;
            }
            int physical = PSPadBackend::gamepadButtonFor(picture.button);
            if (physical < 0)
                physical = picture.button;
            const float s = std::min(w, h);
            const gl::Image *own = artFor(device, physical);
            if (device == Device::PlayStation || device == Device::Nintendo)
            {
                // A remapped PlayStation pad: the physical button's own
                // picture; a Nintendo-style pad: the release's picture of
                // the button with that letter (in a wide cell only a face
                // button's; shoulders below).
                if (own && (!picture.wide || faceButton(physical)))
                {
                    drawFitted(image, *own, x, y, w, h);
                    return;
                }
            }
            const std::string label = padLabel(physical, device);
            const bool shoulder = physical == hi::kL1 || physical == hi::kR1 || physical == hi::kL2 || physical == hi::kR2;
            if (device == Device::PlayStation && picture.wide && shoulder)
            {
                // AI's park editor's grey L1 key look, with the button that presses it.
                gl::drawShoulderButton(image, x, y, w, h, a.label, label, picture.lit);
                return;
            }
            if (device == Device::Nintendo)
            {
                // Round dark face buttons and + / -, grey shoulder keys.
                if (faceButton(physical) || physical == hi::kStart || physical == hi::kSelect)
                {
                    const float d = faceButton(physical) ? s : s * 0.8f;
                    gl::drawRoundButton(image, x + (w - d) * 0.5f, y + (h - d) * 0.5f, d, d, a.label, label);
                    return;
                }
                if (shoulder)
                {
                    const float ph = std::min(h, w * 0.62f);
                    gl::drawShoulderButton(image, x, y + (h - ph) * 0.5f, w, ph, a.label, label, picture.lit);
                    return;
                }
            }
            if (device == Device::Xbox && own)
            {
                drawFitted(image, *own, x, y, w, h);
                return;
            }
            if (faceButton(physical))
            {
                // A round face button with its letter.
                const float r = s * 0.46f;
                gl::fillEllipse(image, x + w * 0.5f, y + h * 0.5f, r, r, gl::rgba(210, 210, 216));
                gl::fillEllipse(image, x + w * 0.5f, y + h * 0.5f, r - 1.2f, r - 1.2f, gl::rgba(38, 38, 44));
                gl::drawText(image, a.label, label, x + w * 0.5f, y + h * 0.5f, s * 0.62f, gl::rgba(244, 244, 248));
                return;
            }
            const float ph = std::min(h, w * 0.62f);
            gl::drawPadLabel(image, x, y + (h - ph) * 0.5f, w, ph, a.label, label, physical == hi::kStart || physical == hi::kSelect,
                             picture.lit);
        }

        // ---- textures in RAM ---------------------------------------------------

        // The engine's texture object.
        constexpr uint32_t kTexName = 0x10;
        constexpr uint32_t kTexPixels = 0x40;
        constexpr uint32_t kTexSize = 0x4C;
        constexpr uint32_t kTexChain = 0x60;
        constexpr uint32_t kTexClut = 0x4A8;
        constexpr uint32_t kTexClutChain = 0x4C0;

        struct Texture
        {
            uint32_t object = 0;
            std::string name;
            uint32_t pixels = 0;
            uint32_t clut = 0;
            int width = 0;
            int height = 0;
            int bits = 8; // 8 or 4
            std::vector<uint8_t> originalPixels;
            std::vector<uint8_t> originalClut; // as in RAM (PS2 alpha, CSM1 order)
            uint32_t appliedGeneration = 0;
            bool appliedOnce = false;
        };

        struct TexturesInRam
        {
            std::mutex mutex;
            std::map<uint32_t, Texture> textures; // by object
            std::atomic<uint32_t> lastAlloc{0};
            bool inTextureLoad = false;
            uint32_t appliedGeneration = 0;
            bool appliedAny = false;

            PS2Runtime::RecompiledFunction origAlloc = nullptr;
            PS2Runtime::RecompiledFunction origLoad = nullptr;
            PS2Runtime::RecompiledFunction origFree = nullptr;
            PS2Runtime::RecompiledFunction origText = nullptr;
        };
        TexturesInRam &ramTextures() { return ps2x::session::state<TexturesInRam>(); }

        size_t pixelBytes(const Texture &t)
        {
            return static_cast<size_t>(t.width) * static_cast<size_t>(t.height) * static_cast<size_t>(t.bits) / 8u;
        }

        size_t clutEntries(const Texture &t) { return t.bits == 8 ? 256u : 16u; }

        bool copyFromGuest(const uint8_t *rdram, uint32_t address, size_t size, std::vector<uint8_t> &out)
        {
            out.resize(size);
            for (size_t i = 0; i < size; ++i)
            {
                const uint8_t *p = getConstMemPtr(rdram, address + static_cast<uint32_t>(i));
                if (!p)
                    return false;
                out[i] = *p;
            }
            return true;
        }

        void copyToGuest(uint8_t *rdram, uint32_t address, const std::vector<uint8_t> &data)
        {
            for (size_t i = 0; i < data.size(); ++i)
                if (uint8_t *p = getMemPtr(rdram, address + static_cast<uint32_t>(i)))
                    *p = data[i];
        }

        // The texture's palette and pixels as RGBA (alpha 0-255, palette in
        // index order).
        std::vector<uint32_t> clutColors(const Texture &t, const std::vector<uint8_t> &clut)
        {
            const size_t count = clutEntries(t);
            std::vector<uint32_t> out(count, 0u);
            for (size_t i = 0; i < count; ++i)
            {
                const size_t slot = t.bits == 8 ? gl::clutSlot(i) : i;
                uint32_t c = 0;
                std::memcpy(&c, clut.data() + 4u * slot, 4);
                const uint32_t a = std::min<uint32_t>(255u, (c >> 24) * 2u);
                out[i] = (c & 0xFFFFFFu) | a << 24;
            }
            return out;
        }

        void putClutColor(const Texture &t, std::vector<uint8_t> &clut, size_t index, uint32_t color)
        {
            const size_t slot = t.bits == 8 ? gl::clutSlot(index) : index;
            const uint32_t a = ((color >> 24) + 1u) / 2u;
            const uint32_t c = (color & 0xFFFFFFu) | a << 24;
            std::memcpy(clut.data() + 4u * slot, &c, 4);
        }

        uint8_t pixelIndex(const Texture &t, const std::vector<uint8_t> &pixels, int x, int y)
        {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x);
            if (t.bits == 8)
                return pixels[i];
            const uint8_t b = pixels[i / 2u];
            return (i & 1u) ? static_cast<uint8_t>(b >> 4) : static_cast<uint8_t>(b & 0xFu);
        }

        void setPixelIndex(const Texture &t, std::vector<uint8_t> &pixels, int x, int y, uint8_t index)
        {
            const size_t i = static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x);
            if (t.bits == 8)
            {
                pixels[i] = index;
                return;
            }
            uint8_t &b = pixels[i / 2u];
            b = (i & 1u) ? static_cast<uint8_t>((b & 0x0Fu) | (index << 4)) : static_cast<uint8_t>((b & 0xF0u) | (index & 0xFu));
        }

        // The game's upload chain of a texture (DMA tags: CNT data follows,
        // REF/REFE data elsewhere, RET/END last) as one GIF packet.
        bool chainPacket(const uint8_t *rdram, uint32_t tag, std::vector<uint8_t> &packet)
        {
            for (int n = 0; n < 16; ++n)
            {
                const uint64_t word = readDword(rdram, tag);
                const uint32_t qwc = static_cast<uint32_t>(word & 0xFFFFu);
                const uint32_t id = static_cast<uint32_t>((word >> 28) & 7u);
                const uint32_t address = static_cast<uint32_t>((word >> 32) & 0x7FFFFFF0u);
                std::vector<uint8_t> data;
                switch (id)
                {
                case 1: // CNT
                case 7: // END
                    if (!copyFromGuest(rdram, tag + 16u, qwc * 16u, data))
                        return false;
                    packet.insert(packet.end(), data.begin(), data.end());
                    if (id == 7)
                        return true;
                    tag += 16u + qwc * 16u;
                    break;
                case 0: // REFE
                case 3: // REF
                    if (!copyFromGuest(rdram, address, qwc * 16u, data))
                        return false;
                    packet.insert(packet.end(), data.begin(), data.end());
                    if (id == 0)
                        return true;
                    tag += 16u;
                    break;
                case 6: // RET
                    return !packet.empty();
                default:
                    return false;
                }
            }
            return false;
        }

        void upload(PS2Runtime *runtime, const uint8_t *rdram, const Texture &t)
        {
            // TRXPOS = 0 first (the game's chains leave it as it is).
            std::vector<uint8_t> packet(32u, 0u);
            const uint64_t giftag = 1u | (1ull << 15) | (1ull << 60) | (0xEull << 32); // NLOOP 1, EOP, NREG 1, A+D
            std::memcpy(packet.data(), &giftag, 8);
            const uint64_t trxposAddress = 0x51u;
            std::memcpy(packet.data() + 24, &trxposAddress, 8);
            const size_t start = packet.size();
            if (!chainPacket(rdram, t.object + kTexChain, packet) || packet.size() == start)
                return;
            chainPacket(rdram, t.object + kTexClutChain, packet);
            // The chains' own GIF tags have EOP clear; the arbiter takes
            // the packet whole.
            runtime->memory().processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));
        }

        // Is the object still this texture (not freed and reused)?
        bool stillThere(const uint8_t *rdram, const Texture &t)
        {
            return readWord(rdram, t.object + kTexPixels) == t.pixels &&
                   readString(rdram, t.object + kTexName, 32) == t.name;
        }

        // The texture as loaded, as RGBA.
        gl::Image loadedImage(const Texture &t)
        {
            const std::vector<uint32_t> palette = clutColors(t, t.originalClut);
            gl::Image image(t.width, t.height);
            for (int y = 0; y < t.height; ++y)
                for (int x = 0; x < t.width; ++x)
                    image.pixels[static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x)] =
                        palette[pixelIndex(t, t.originalPixels, x, y)];
            return image;
        }

        // Whether a loaded texture is this picture of the disc: the same
        // size and colours (alpha within the PS2's halving) at nearly every
        // pixel.
        bool samePicture(const gl::Image &loaded, const gl::Image &disc)
        {
            if (loaded.width != disc.width || loaded.height != disc.height || loaded.pixels.empty())
                return false;
            size_t same = 0;
            for (size_t i = 0; i < loaded.pixels.size(); ++i)
            {
                const uint32_t a = loaded.pixels[i], b = disc.pixels[i];
                const int da = static_cast<int>(a >> 24) - static_cast<int>(b >> 24);
                if ((a & 0xFFFFFFu) == (b & 0xFFFFFFu) && da >= -2 && da <= 2)
                    ++same;
            }
            return same * 100u >= loaded.pixels.size() * 98u;
        }

        // The Controller screen's photo for the device (PadPhoto), into
        // pixels and clut (left as loaded for a PlayStation pad as mapped by
        // default). False if the texture is not the photo.
        bool redrawPad(const Texture &t, Device device, std::vector<uint8_t> &pixels, std::vector<uint8_t> &clut)
        {
            const PadPhoto *photo = installed().game->padPhoto;
            const gl::Image *disc = artFor(Device::PlayStation, kPadArt);
            if (!photo || !disc || t.width != photo->width || t.height != photo->height)
                return false;
            gl::Image image = loadedImage(t);
            if (!samePicture(image, *disc))
                return false; // another texture of that name
            if (device == Device::PlayStation && !anyRemapped())
                return true;
            if (const gl::Image *own = device == Device::PlayStation ? nullptr : artFor(device, kPadArt))
            {
                image = gl::Image(t.width, t.height);
                gl::drawImage(image, *own, 0.0f, 0.0f, static_cast<float>(t.width), static_cast<float>(t.height));
            }
            else
                for (size_t i = 0; i < photo->placeCount; ++i)
                {
                    const PadPlace &place = photo->places[i];
                    paintButton(image, place.x, place.y, place.w, place.h, place.picture, device);
                }
            const gl::Indexed indexed = gl::quantize(image, clutEntries(t));
            for (size_t i = 0; i < clutEntries(t); ++i)
                putClutColor(t, clut, i, i < indexed.palette.size() ? indexed.palette[i] : 0u);
            for (int y = 0; y < t.height; ++y)
                for (int x = 0; x < t.width; ++x)
                    setPixelIndex(t, pixels, x, y,
                                  indexed.indices[static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x)]);
            return true;
        }

        // Draws the texture for the device into RAM. False if nothing changed.
        bool redraw(uint8_t *rdram, Texture &t, Device device)
        {
            const Game &game = *installed().game;
            std::vector<uint8_t> pixels = t.originalPixels;
            std::vector<uint8_t> clut = t.originalClut;
            const bool playstation = device == Device::PlayStation;
            {
                std::lock_guard<std::mutex> lock(art().mutex);
                const auto whole = game.textures->find(t.name);
                if (whole != game.textures->end() && whole->second.shows == Shows::Pad)
                {
                    if (!redrawPad(t, device, pixels, clut))
                        return false;
                }
                else if (whole != game.textures->end() && playstation && !remapped(whole->second.button))
                {
                    // As loaded.
                }
                else if (whole != game.textures->end())
                {
                    gl::Image image(t.width, t.height);
                    paintButton(image, 0.0f, 0.0f, static_cast<float>(t.width), static_cast<float>(t.height), whole->second,
                                device);
                    const gl::Indexed indexed = gl::quantize(image, clutEntries(t));
                    for (size_t i = 0; i < clutEntries(t); ++i)
                        putClutColor(t, clut, i, i < indexed.palette.size() ? indexed.palette[i] : 0u);
                    for (int y = 0; y < t.height; ++y)
                        for (int x = 0; x < t.width; ++x)
                            setPixelIndex(t, pixels, x, y, indexed.indices[static_cast<size_t>(y) * static_cast<size_t>(t.width) + static_cast<size_t>(x)]);
                }
                else
                {
                    const auto cells = game.fonts->find(t.name);
                    const auto font = art().fonts.find(t.name);
                    if (cells == game.fonts->end() || font == art().fonts.end())
                    {
                        if (!playstation)
                            return false;
                        copyToGuest(rdram, t.pixels, pixels);
                        copyToGuest(rdram, t.clut, clut);
                        return true;
                    }
                    // The cells, and the palette entries the rest of the atlas uses.
                    struct Cell
                    {
                        const zx::FontGlyph *glyph;
                        Picture picture;
                    };
                    std::vector<Cell> targets;
                    for (const auto &[c, picture] : cells->second)
                        if (const zx::FontGlyph *g = font->second.glyph(c); g && (!playstation || remapped(picture.button)))
                            targets.push_back({g, picture});
                    if (targets.empty() && playstation)
                    {
                        copyToGuest(rdram, t.pixels, pixels);
                        copyToGuest(rdram, t.clut, clut);
                        return true;
                    }
                    const auto inCell = [&targets](int x, int y)
                    {
                        for (const Cell &cell : targets)
                            if (x >= cell.glyph->x && y >= cell.glyph->y && x < cell.glyph->x + cell.glyph->width &&
                                y < cell.glyph->y + cell.glyph->height)
                                return true;
                        return false;
                    };
                    std::vector<bool> used(clutEntries(t), false);
                    for (int y = 0; y < t.height; ++y)
                        for (int x = 0; x < t.width; ++x)
                            if (!inCell(x, y))
                                used[pixelIndex(t, t.originalPixels, x, y)] = true;
                    std::vector<uint32_t> palette = clutColors(t, t.originalClut);
                    std::vector<size_t> freeSlots;
                    for (size_t i = 0; i < used.size(); ++i)
                        if (!used[i])
                            freeSlots.push_back(i);
                    // All new cells side by side, quantized together into the
                    // free entries.
                    int sheetW = 0, sheetH = 0;
                    for (const Cell &cell : targets)
                    {
                        sheetW += cell.glyph->width;
                        sheetH = std::max<int>(sheetH, cell.glyph->height);
                    }
                    gl::Image sheet(std::max(1, sheetW), std::max(1, sheetH));
                    int at = 0;
                    for (const Cell &cell : targets)
                    {
                        // Leave the glyph's outline pixel free on each side.
                        paintButton(sheet, static_cast<float>(at) + 0.5f, 0.5f, static_cast<float>(cell.glyph->width) - 1.0f,
                                    static_cast<float>(cell.glyph->height) - 1.0f, cell.picture, device);
                        at += cell.glyph->width;
                    }
                    const gl::Indexed fresh = gl::quantize(sheet, std::max<size_t>(1u, freeSlots.size()));
                    std::vector<uint32_t> combined;
                    std::vector<size_t> combinedSlot;
                    for (size_t i = 0; i < palette.size(); ++i)
                        if (used[i])
                        {
                            combined.push_back(palette[i]);
                            combinedSlot.push_back(i);
                        }
                    for (size_t i = 0; i < fresh.palette.size() && i < freeSlots.size(); ++i)
                    {
                        putClutColor(t, clut, freeSlots[i], fresh.palette[i]);
                        combined.push_back(fresh.palette[i]);
                        combinedSlot.push_back(freeSlots[i]);
                    }
                    at = 0;
                    for (const Cell &cell : targets)
                    {
                        for (int y = 0; y < cell.glyph->height; ++y)
                            for (int x = 0; x < cell.glyph->width; ++x)
                            {
                                const uint32_t color = sheet.at(at + x, y);
                                const size_t index = combinedSlot[gl::nearest(combined, color)];
                                setPixelIndex(t, pixels, cell.glyph->x + x, cell.glyph->y + y, static_cast<uint8_t>(index));
                            }
                        at += cell.glyph->width;
                    }
                }
            }
            copyToGuest(rdram, t.pixels, pixels);
            copyToGuest(rdram, t.clut, clut);
            return true;
        }

        bool interesting(const std::string &name)
        {
            const Game &game = *installed().game;
            return game.textures->count(name) != 0u || game.fonts->count(name) != 0u;
        }

        // A texture the game just made from a TXRC chunk.
        void noteTexture(uint8_t *rdram, uint32_t object)
        {
            const std::string name = readString(rdram, object + kTexName, 32);
            if (installed().trace > 1)
            {
                std::string dump;
                char word[12];
                for (uint32_t o = 0; o < 0x60u; o += 4u)
                {
                    std::snprintf(word, sizeof(word), " %08x", readWord(rdram, object + o));
                    dump += word;
                }
                std::fprintf(stderr, "[prompts] loaded texture %s at 0x%x:%s\n", name.c_str(), object, dump.c_str());
            }
            if (!interesting(name))
                return;
            Texture t;
            t.object = object;
            t.name = name;
            t.pixels = readWord(rdram, object + kTexPixels);
            t.clut = readWord(rdram, object + kTexClut);
            const uint32_t size = readWord(rdram, object + kTexSize);
            t.width = static_cast<int>(size & 0xFFFFu);
            t.height = static_cast<int>(size >> 16);
            // Bits per pixel from the upload sizes (quadwords at +4 of the
            // pixel and CLUT pointers): TEX0 is only set when first drawn.
            const uint32_t pixelQwc = readWord(rdram, object + kTexPixels + 4u);
            const uint32_t clutQwc = readWord(rdram, object + kTexClut + 4u);
            const size_t area = static_cast<size_t>(t.width) * static_cast<size_t>(t.height);
            t.bits = area && pixelQwc * 16u == area ? 8 : area && pixelQwc * 32u == area ? 4 : 0;
            if (t.bits == 0 || !t.pixels || !t.clut || clutQwc * 4u != clutEntries(t) ||
                !copyFromGuest(rdram, t.pixels, pixelBytes(t), t.originalPixels) ||
                !copyFromGuest(rdram, t.clut, clutEntries(t) * 4u, t.originalClut))
            {
                std::cerr << "[prompts] texture " << name << " not understood (" << t.width << "x" << t.height << ", "
                          << pixelQwc << " qw, CLUT " << clutQwc << " qw)" << std::endl;
                return;
            }
            if (installed().trace)
                std::fprintf(stderr, "[prompts] texture %s at 0x%x: %dx%d, %d bits\n", name.c_str(), object, t.width,
                             t.height, t.bits);
            std::lock_guard<std::mutex> lock(ramTextures().mutex);
            ramTextures().textures[object] = std::move(t);
        }

        // ---- hooks ----------------------------------------------------------

        void returnFromHook(R5900Context *ctx, uint32_t result)
        {
            setReturnU32(ctx, result);
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        void textureAllocHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t result = 0;
            runtime->callGuestFunction(rdram, ctx, ramTextures().origAlloc, installed().game->newLoadedTexture,
                                       {getRegU32(ctx, kRegA0), getRegU32(ctx, kRegA1), getRegU32(ctx, kRegA2),
                                        getRegU32(ctx, kRegA3)},
                                       getRegU32(ctx, kRegSp), &result);
            if (ramTextures().inTextureLoad)
                ramTextures().lastAlloc = result;
            returnFromHook(ctx, result);
        }

        void textureLoadHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t result = 0;
            ramTextures().lastAlloc = 0;
            ramTextures().inTextureLoad = true;
            runtime->callGuestFunction(rdram, ctx, ramTextures().origLoad, installed().game->loadTextureChunk,
                                       {getRegU32(ctx, kRegA0), getRegU32(ctx, kRegA1), getRegU32(ctx, kRegA2),
                                        getRegU32(ctx, kRegA3)},
                                       getRegU32(ctx, kRegSp), &result);
            ramTextures().inTextureLoad = false;
            if (const uint32_t object = ramTextures().lastAlloc.exchange(0))
                noteTexture(rdram, object);
            returnFromHook(ctx, result);
        }

        void textureFreeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t object = getRegU32(ctx, kRegA0);
            uint32_t result = 0;
            runtime->callGuestFunction(rdram, ctx, ramTextures().origFree, installed().game->releaseLoadedTexture, {object},
                                       getRegU32(ctx, kRegSp), &result);
            if (result != 0u)
            {
                std::lock_guard<std::mutex> lock(ramTextures().mutex);
                ramTextures().textures.erase(object);
            }
            returnFromHook(ctx, result);
        }

        // ---- the game's phrases ---------------------------------------------

        // What the device calls a game button in a sentence, the phrase's
        // " button" kept ("X button", "Space key", "left mouse button").
        std::string phraseFor(const Phrase &phrase, Device device)
        {
            const std::string words = phrase.words;
            const bool button = words.size() > 7u && words.compare(words.size() - 7u, 7u, " button") == 0;
            if (device == Device::Keyboard)
            {
                const int code = firstKey(phrase.button);
                if (code < 0)
                    return words;
                if (hi::isMouseCode(code))
                {
                    std::string mouse = keyPhrase(phrase.button); // "the left mouse button"
                    return mouse.rfind("the ", 0) == 0 ? mouse.substr(4) : mouse;
                }
                return hi::hostCodeLabel(code) + (button ? " key" : "");
            }
            int physical = PSPadBackend::gamepadButtonFor(phrase.button);
            if (physical < 0)
                physical = phrase.button;
            std::string name;
            if (device == Device::PlayStation && faceButton(physical))
            {
                static const std::pair<int, const char *> kFaces[] = {
                    {hi::kCross, "X"}, {hi::kCircle, "circle"}, {hi::kSquare, "square"}, {hi::kTriangle, "triangle"}};
                for (const auto &[b, n] : kFaces)
                    if (b == physical)
                        name = n;
            }
            else
                name = padLabel(physical, device);
            return name + (button ? " button" : "");
        }

        // The text with each phrase as the device says it, in one pass (so a
        // replacement is never replaced again).
        std::string phrasesFor(const std::string &text, Device device)
        {
            const Game &game = *installed().game;
            const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            const auto isWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
            std::string out;
            size_t pos = 0;
            while (pos < text.size())
            {
                const Phrase *found = nullptr;
                size_t foundLength = 0;
                if (pos == 0 || !isWord(text[pos - 1]))
                    for (size_t i = 0; i < game.phraseCount; ++i)
                    {
                        const std::string words = game.phrases[i].words;
                        if (words.size() <= foundLength || pos + words.size() > text.size())
                            continue;
                        bool match = true;
                        for (size_t k = 0; k < words.size() && match; ++k)
                            match = lower(text[pos + k]) == lower(words[k]);
                        if (match && pos + words.size() < text.size() && isWord(text[pos + words.size()]))
                            match = false;
                        if (match)
                        {
                            found = &game.phrases[i];
                            foundLength = words.size();
                        }
                    }
                if (!found)
                {
                    out.push_back(text[pos++]);
                    continue;
                }
                // In the original's case: all capitals ("PRESS START BUTTON"),
                // else its first letter's.
                std::string replacement = phraseFor(*found, device);
                bool capitals = true;
                for (size_t k = 0; k < foundLength; ++k)
                    capitals = capitals && !std::islower(static_cast<unsigned char>(text[pos + k]));
                if (capitals)
                    for (char &c : replacement)
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                else if (!replacement.empty() && std::isupper(static_cast<unsigned char>(text[pos])))
                    replacement[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(replacement[0])));
                out += replacement;
                pos += foundLength;
            }
            return out;
        }

        // Guest buffers for changed text, used in turn.
        constexpr uint32_t kTextBufferSize = 0x800u;
        constexpr int kTextBufferCount = 8;
        struct TextBuffers
        {
            uint32_t base = 0;
            int next = 0;
            std::set<std::string> traced; // PS2X_TRACE_PROMPTS: each changed text once
        };
        TextBuffers &textBuffers() { return ps2x::session::state<TextBuffers>(); }

        // The text argument changed for the device, or 0 to leave it.
        uint32_t textFor(uint8_t *rdram, uint32_t text)
        {
            if (!text || !textBuffers().base)
                return 0u;
            const Device device = PSPadBackend::activeDevice();
            if (device == Device::PlayStation && !anyRemapped())
                return 0u;
            const std::string original = readString(rdram, text, kTextBufferSize - 1u);
            if (original.empty())
                return 0u;
            if (installed().trace > 2 && textBuffers().traced.size() < 400u && textBuffers().traced.insert("@" + original).second)
                std::fprintf(stderr, "[prompts] drawn \"%s\"\n", original.c_str());
            const std::string changed = phrasesFor(original, device);
            if (changed == original || changed.size() + 1u > kTextBufferSize)
                return 0u;
            if (installed().trace && textBuffers().traced.size() < 64u && textBuffers().traced.insert(changed).second)
                std::fprintf(stderr, "[prompts] text \"%s\" as \"%s\"\n", original.c_str(), changed.c_str());
            const uint32_t buffer = textBuffers().base + kTextBufferSize * static_cast<uint32_t>(textBuffers().next);
            textBuffers().next = (textBuffers().next + 1) % kTextBufferCount;
            for (size_t i = 0; i <= changed.size(); ++i)
                if (uint8_t *p = getMemPtr(rdram, buffer + static_cast<uint32_t>(i)))
                    *p = i < changed.size() ? static_cast<uint8_t>(changed[i]) : 0u;
            return buffer;
        }

        // The line of text: the textures brought up to date, the text's
        // phrases changed (the original then runs in the same frame, a tail
        // call: it can yield to the scheduler, so it is not a nested guest
        // call; the changed string stays in one of the buffers meanwhile).
        void textLineHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            refresh(rdram, runtime);
            if (const uint32_t changed = textFor(rdram, getRegU32(ctx, kRegA2)))
                SET_GPR_U32(ctx, kRegA2, changed);
            ramTextures().origText(rdram, ctx, runtime);
        }
    }

    // ---- what the device calls a button --------------------------------------

    std::string padLabel(int physical, Device device)
    {
        const bool nintendo = device == Device::Nintendo;
        if (device == Device::PlayStation)
        {
            switch (physical)
            {
            case hi::kL1:
                return "L1";
            case hi::kR1:
                return "R1";
            case hi::kL2:
                return "L2";
            case hi::kR2:
                return "R2";
            case hi::kStart:
                return "START";
            case hi::kSelect:
                return "SELECT";
            case hi::kL3:
                return "L3";
            case hi::kR3:
                return "R3";
            default:
                break;
            }
        }
        switch (physical)
        {
        case hi::kL1:
            return nintendo ? "L" : "LB";
        case hi::kR1:
            return nintendo ? "R" : "RB";
        case hi::kL2:
            return nintendo ? "ZL" : "LT";
        case hi::kR2:
            return nintendo ? "ZR" : "RT";
        case hi::kStart:
            return nintendo ? "+" : "START";
        case hi::kSelect:
            return nintendo ? "-" : "BACK";
        case hi::kL3:
            return "LS";
        case hi::kR3:
            return "RS";
        case hi::kCross:
            return nintendo ? "B" : "A";
        case hi::kCircle:
            return nintendo ? "A" : "B";
        case hi::kSquare:
            return nintendo ? "Y" : "X";
        case hi::kTriangle:
            return nintendo ? "X" : "Y";
        default:
            return "?";
        }
    }

    int firstKey(int button)
    {
        const std::vector<int> keys = PSPadBackend::keyBindings(button);
        return keys.empty() ? -1 : keys.front();
    }

    bool remapped(int button)
    {
        const int physical = button < 0 ? -1 : PSPadBackend::gamepadButtonFor(button);
        return physical >= 0 && physical != button;
    }

    bool anyRemapped()
    {
        for (int button = 0; button < hi::kPadButtonCount; ++button)
            if (remapped(button))
                return true;
        return false;
    }

    std::string keyPhrase(int button)
    {
        const int code = firstKey(button);
        if (code < 0)
            return {};
        if (hi::isMouseCode(code))
        {
            static const char *const kMouse[] = {"the left mouse button", "the right mouse button",
                                                 "the middle mouse button"};
            const int index = code - hi::kMouseCodeBase;
            return index < 3 ? kMouse[index] : "mouse button " + std::to_string(index + 1);
        }
        return hi::hostCodeLabel(code);
    }

    void replaceWords(std::string &text, const std::string &from, const std::string &to)
    {
        if (from.empty())
            return;
        const auto lower = [](char c)
        { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
        const auto isWord = [](char c)
        { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
        size_t pos = 0;
        while (pos + from.size() <= text.size())
        {
            bool match = true;
            for (size_t i = 0; i < from.size() && match; ++i)
                match = lower(text[pos + i]) == lower(from[i]);
            if (match && isWord(from.front()) && pos > 0 && isWord(text[pos - 1]))
                match = false;
            if (match && isWord(from.back()) && pos + from.size() < text.size() && isWord(text[pos + from.size()]))
                match = false;
            if (!match)
            {
                ++pos;
                continue;
            }
            std::string replacement = to;
            if (!replacement.empty() && std::isupper(static_cast<unsigned char>(text[pos])) &&
                std::islower(static_cast<unsigned char>(replacement[0])))
                replacement[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(replacement[0])));
            text.replace(pos, from.size(), replacement);
            pos += replacement.size();
        }
    }

    // ---- the Controller screen's picture ---------------------------------------

    const PadPhoto &dualShockPhoto()
    {
        // Each control's place in cntrlr_ps2.zmp: where the leader lines of
        // DM2's Ps2Line captions and of BMX XXX's Controls screen end (the
        // two games' lines meet the same buttons), the box around what the
        // photo shows there.
        static constexpr PadPlace kPlaces[] = {
            {{Shows::Button, hi::kTriangle}, 176.0f, 127.0f, 20.0f, 20.0f},
            {{Shows::Button, hi::kCircle}, 199.0f, 151.0f, 20.0f, 20.0f},
            {{Shows::Button, hi::kCross}, 176.0f, 166.0f, 20.0f, 20.0f},
            {{Shows::Button, hi::kSquare}, 154.0f, 143.0f, 20.0f, 20.0f},
            {{Shows::DPad, kDPadArt}, 14.0f, 109.0f, 38.0f, 38.0f},
            {{Shows::Button, hi::kSelect, false, true}, 77.0f, 132.0f, 24.0f, 12.0f},
            {{Shows::Button, hi::kStart, false, true}, 122.0f, 137.0f, 24.0f, 12.0f},
            {{Shows::Button, hi::kL2, false, true}, 29.0f, 56.0f, 36.0f, 15.0f},
            {{Shows::Button, hi::kL1, false, true}, 29.0f, 75.0f, 36.0f, 15.0f},
            {{Shows::Button, hi::kR2, false, true}, 182.0f, 76.0f, 36.0f, 15.0f},
            {{Shows::Button, hi::kR1, false, true}, 180.0f, 97.0f, 36.0f, 15.0f},
            {{Shows::LeftStick, -1}, 56.0f, 150.0f, 40.0f, 40.0f},
            {{Shows::RightStick, -1}, 123.0f, 158.0f, 44.0f, 44.0f},
        };
        static constexpr PadPhoto kPhoto{"cntrlr_ps2.zmp", 256, 256, kPlaces, std::size(kPlaces)};
        return kPhoto;
    }

    bool padArtShown(Device device)
    {
        std::lock_guard<std::mutex> lock(art().mutex);
        return device != Device::PlayStation && artFor(device, kPadArt) != nullptr;
    }

    // ---- the art --------------------------------------------------------------

    ps2x::glyphs::Font labelFont()
    {
        std::lock_guard<std::mutex> lock(art().mutex);
        return art().label;
    }

    bool atlasFont(const std::string &atlas, ps2x::zaxis::LibraryFont &font)
    {
        std::lock_guard<std::mutex> lock(art().mutex);
        const auto it = art().fonts.find(atlas);
        if (it == art().fonts.end())
            return false;
        font = it->second;
        return true;
    }

    static void captureArt(const std::vector<ps2x::zaxis::ArchiveEntry> &entries)
    {
        const Game *game = installed().game;
        Art &a = art();
        std::lock_guard<std::mutex> lock(a.mutex);
        if (a.captured || !game)
            return;
        const auto keep = [&a](const ArtSource &source, gl::Image image)
        {
            auto &pictures = a.buttons[static_cast<size_t>(source.device)];
            if (!pictures.count(source.button))
                pictures[source.button] = std::move(image);
        };
        for (const zx::ArchiveEntry &entry : entries)
        {
            std::vector<zx::LibraryTexture> textures;
            std::vector<zx::LibraryFont> fonts;
            if (!zx::readLibrary(entry.data, &textures, &fonts))
                continue;
            for (const zx::LibraryTexture &t : textures)
            {
                const std::string file = zx::sourceFileName(t.source);
                if (game->padPhoto && file == game->padPhoto->file)
                {
                    // The PlayStation pad's: what a loaded texture is compared with.
                    gl::Image image(t.width, t.height);
                    image.pixels = t.rgba;
                    keep(ArtSource{Device::PlayStation, game->padPhoto->file, 0, kPadArt}, std::move(image));
                }
                for (size_t i = 0; i < game->artCount; ++i)
                    if (game->art[i].character == 0 && file == game->art[i].file)
                    {
                        gl::Image image(t.width, t.height);
                        image.pixels = t.rgba;
                        keep(game->art[i], std::move(image));
                    }
            }
            for (zx::LibraryFont &f : fonts)
            {
                const std::string file = zx::sourceFileName(f.source);
                for (size_t i = 0; i < game->artCount; ++i)
                    if (game->art[i].character != 0 && file == game->art[i].file)
                        if (const zx::FontGlyph *g = f.glyph(game->art[i].character))
                            keep(game->art[i], glyphImage(f, *g));
                if (file == game->labelFont)
                    a.label = gl::fontFrom(f);
                if (!f.atlas.name.empty())
                    a.fonts[f.atlas.name] = std::move(f);
            }
        }
        a.captured = true;
        std::cerr << "[prompts] art from UILIB.ZAL:";
        static const char *const kNames[] = {" keyboard", " PlayStation", " Xbox", " Nintendo"};
        for (size_t d = 1; d < kDevices; ++d)
            std::cerr << " " << a.buttons[d].size() << kNames[d];
        std::cerr << " pictures, label font " << (a.label.empty() ? "missing" : game->labelFont) << ", " << a.fonts.size()
                  << " fonts" << std::endl;
    }

    void refresh(uint8_t *rdram, PS2Runtime *runtime)
    {
        TexturesInRam &ram = ramTextures();
        const uint32_t generation = PSPadBackend::promptGeneration();
        std::lock_guard<std::mutex> lock(ram.mutex);
        bool pending = false;
        for (auto &entry : ram.textures)
            pending = pending || !entry.second.appliedOnce || entry.second.appliedGeneration != generation;
        if (!pending)
            return;
        const Device device = PSPadBackend::activeDevice();
        for (auto it = ram.textures.begin(); it != ram.textures.end();)
        {
            Texture &t = it->second;
            if (!stillThere(rdram, t))
            {
                it = ram.textures.erase(it);
                continue;
            }
            if (!t.appliedOnce || t.appliedGeneration != generation)
            {
                const bool firstTime = !t.appliedOnce;
                t.appliedOnce = true;
                t.appliedGeneration = generation;
                // A PlayStation picture is already as loaded (unless remapped).
                if (!(firstTime && device == Device::PlayStation && !anyRemapped()) && redraw(rdram, t, device))
                {
                    upload(runtime, rdram, t);
                    if (installed().trace)
                        std::fprintf(stderr, "[prompts] %s drawn for device %d\n", t.name.c_str(), static_cast<int>(device));
                }
            }
            ++it;
        }
        if (installed().trace && (!ram.appliedAny || generation != ram.appliedGeneration))
            std::fprintf(stderr, "[prompts] pictures for device %d\n", static_cast<int>(device));
        ram.appliedAny = true;
        ram.appliedGeneration = generation;
    }

    bool install(PS2Runtime &runtime, const Game &game)
    {
        installed().game = &game;
        const char *trace = std::getenv("PS2X_TRACE_PROMPTS");
        installed().trace = trace ? std::atoi(trace) : 0;
        TexturesInRam &ram = ramTextures();
        ram.origAlloc = runtime.lookupFunction(game.newLoadedTexture);
        ram.origLoad = runtime.lookupFunction(game.loadTextureChunk);
        ram.origFree = runtime.lookupFunction(game.releaseLoadedTexture);
        ram.origText = game.drawText ? runtime.lookupFunction(game.drawText) : nullptr;
        if (game.phraseCount)
            textBuffers().base =
                runtime.reserveAsyncCallbackStack(static_cast<uint32_t>(kTextBufferCount) * kTextBufferSize, 16u);
        if (!ram.origAlloc || !ram.origLoad || !ram.origFree || (game.drawText && !ram.origText) ||
            (game.phraseCount && !textBuffers().base))
        {
            std::cerr << game.tag << " button prompts not installed" << std::endl;
            return false;
        }
        runtime.replaceFunction(game.newLoadedTexture, &textureAllocHook);
        runtime.replaceFunction(game.loadTextureChunk, &textureLoadHook);
        runtime.replaceFunction(game.releaseLoadedTexture, &textureFreeHook);
        if (game.drawText)
            runtime.replaceFunction(game.drawText, &textLineHook);
        // The art, from the UI library as the game reads it (after the
        // patches registered before this one: another language's fonts),
        // left as it is.
        ps2x::disc_patch::registerPatch("UILIB.ZAL",
                                        [](std::vector<uint8_t> &bytes)
                                        {
                                            std::vector<zx::ArchiveEntry> entries;
                                            if (zx::readArchive(bytes, entries))
                                                captureArt(entries);
                                            return false;
                                        });
        std::cerr << game.tag << " button prompts follow the input device" << std::endl;
        return true;
    }
}
