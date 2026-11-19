#pragma once

// Button prompts for the device in use, shared by the games built on the
// Z-Axis engine (zge_prompts.cpp). Each game shows PlayStation buttons as
// pictures in its texture libraries (whole textures: "select_button") and
// as characters of fonts whose glyphs are button pictures (a cell of the
// font's atlas). With keyboard and mouse, an Xbox-style or a Nintendo-style
// gamepad those become that device's keys and buttons, following
// controls.ini, and change when the player switches device
// (PSPadBackend::activeDevice): the game's own art of its other releases
// where its disc has it, else drawn here in the look of the game's own
// (ps2_glyph_art.h: key caps labelled in one of the game's fonts).
//
// The engine loads every texture of a library through its TXRC chunk
// reader (loadTextureChunk, which makes the object with newLoadedTexture;
// releaseLoadedTexture frees it): the ones a game's tables name are noted.
// A texture object (0x560 bytes, the same in ZGE2, ZGE3 and ZGE4) keeps its
// name at +0x10, its picture in RAM (+0x40 pixels, +0x4C height << 16 |
// width, +0x60 the DMA chain that uploads it; +0x4A8 the CLUT, +0x4C0 its
// chain) and the game uploads it to GS memory once, where it stays. On a
// device change the pixels and CLUT in RAM are redrawn (in a font atlas
// only the button characters' cells, with colours from palette entries the
// rest of the atlas does not use; PlayStation pictures put back as loaded)
// and the game's own chains are sent to the GIF again, on the EE thread
// before a line of text is drawn.
//
// Aggressive Inline (aggressive_inline_prompts.cpp, which adds its words,
// languages and Tutorial screens), Dave Mirra BMX 2 (dave_mirra_prompts.cpp)
// and BMX XXX (bmx_xxx_prompts.cpp) each describe theirs in a Game record.

#include "runtime/ps2_glyph_art.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_zaxis_ui.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

class PS2Runtime;

namespace zge_host::prompts
{
    using Device = PSPadBackend::InputDevice;

    // What a picture shows.
    enum class Shows
    {
        Button,     // a DualShock 2 button (PadButton)
        DPad,       // the d-pad
        ButtonLeft, // a shoulder button with an arrow to its left (AI's park editor)
        ButtonRight,
        LeftStick,  // the sticks (a keyboard's keys for them)
        RightStick,
        Pad,        // a whole pad: the Controller screen's picture (PadPhoto)
    };

    struct Picture
    {
        Shows shows = Shows::Button;
        int button = -1;
        bool lit = false;  // drawn highlighted (AI's park editor's pressed L1/R1)
        bool wide = false; // a key or label as wide as the cell, not as tall
    };

    // Whole textures, by TXRH name ("select_button").
    using TexturePictures = std::map<std::string, Picture>;
    // Font atlases by texture name ("ps2_btn_t0.zmp"): the characters that
    // are button pictures.
    using FontPictures = std::map<std::string, std::vector<std::pair<char, Picture>>>;

    // An ArtSource's button for the d-pad's picture.
    constexpr int kDPadArt = -1;
    // An ArtSource's button for a picture of the whole pad (the Controller
    // screen's).
    constexpr int kPadArt = -2;

    // The Controller screen's picture of the pad (DM2's Controller screen,
    // BMX XXX's Controls): the PS2 releases' photo of a DualShock 2, with
    // the game's captions and leader lines to its controls around it. For
    // another device the picture is that device's own pad picture where the
    // game has one that its screen's lines fit (an ArtSource with
    // kPadArt), else the photo with each control drawn over as the device's
    // button, key caps or d-pad, at the places the lines point to (the
    // sticks stay, but for the keyboard's keys of them).
    struct PadPlace
    {
        Picture picture;
        float x, y, w, h; // in the photo's pixels
    };
    struct PadPhoto
    {
        const char *file; // its artist's source file (ArtSource file), the PlayStation pad's kPadArt
        int width;
        int height;
        const PadPlace *places;
        size_t placeCount;
    };
    // cntrlr_ps2.zmp (256 by 256), DM2's ps2Ctrl.zif and BMX XXX's ps2ctrl.zif
    // (the same picture).
    const PadPhoto &dualShockPhoto();

    // A picture of the game's own art of one device's button: a whole
    // texture of a library (`file` its artist's source file, lower case, as
    // ps2x::zaxis::sourceFileName gives it: "xbox_btn_a.tga"; character 0)
    // or a character of a font (`file` the font's: "xbox_btn.zfn"). `button`
    // is the physical button it shows by position (PadButton numbering: the
    // Xbox A is Cross, a Nintendo A, on the right, Circle), or kDPadArt. The
    // first source found for a device's button is kept.
    struct ArtSource
    {
        Device device;
        const char *file;
        char character;
        int button;
    };

    // A phrase of the game's text that names a button ("square button"):
    // shown as the device names that button. Matched whole-word and without
    // regard to case.
    struct Phrase
    {
        const char *words;
        int button;
    };

    struct Game
    {
        const char *tag; // the log's ("[dave-mirra]")
        // The engine's texture loading (TXRC chunks).
        uint32_t loadTextureChunk;
        uint32_t newLoadedTexture;
        uint32_t releaseLoadedTexture;
        // The engine's line of text (font, position, text): the textures
        // are brought up to date before it draws and its text gets the
        // device's phrases. 0: the game's override hooks its text itself and
        // calls refresh (AI's words).
        uint32_t drawText;
        const TexturePictures *textures;
        const FontPictures *fonts;
        const ArtSource *art;
        size_t artCount;
        // The font key caps and drawn buttons are labelled in ("fnt_14pt.zfn").
        const char *labelFont;
        const Phrase *phrases = nullptr;
        size_t phraseCount = 0;
        // The Controller screen's photo, a texture of `textures` that shows
        // Shows::Pad.
        const PadPhoto *padPhoto = nullptr;
    };

    // Hooks the game's texture loading (and drawText); the art comes from
    // UILIB.ZAL as the game reads it, after the patches registered before
    // (a read-only patch of its own). False if a hook could not be
    // installed.
    bool install(PS2Runtime &runtime, const Game &game);

    // Brings every noted texture up to date with the device; on the EE
    // thread, between the game's own GS transfers.
    void refresh(uint8_t *rdram, PS2Runtime *runtime);

    // ---- what the device calls a button (for a game's words) ---------------

    // A button's label on the device ("RB", "ZL", "START"; a PlayStation
    // pad's shoulders "L1" ...).
    std::string padLabel(int physical, Device device);
    // The first key or mouse button bound to a game button (-1: none).
    int firstKey(int button);
    // A PlayStation pad whose [gamepad] remap presses this game button with
    // another one.
    bool remapped(int button);
    bool anyRemapped();
    // How a sentence names the key for a game button: "Space", "F", "the
    // left mouse button".
    std::string keyPhrase(int button);
    // Replaces whole-word occurrences of `from` (case-insensitive; a capital
    // first letter is kept).
    void replaceWords(std::string &text, const std::string &from, const std::string &to);

    // ---- the art captured --------------------------------------------------

    // Whether the Controller screen shows the device's own pad picture (an
    // ArtSource with kPadArt) rather than the photo (the game shows the
    // captions that go with it).
    bool padArtShown(Device device);

    // The label font (empty until captured).
    ps2x::glyphs::Font labelFont();
    // A font of the UI library by its atlas texture name
    // ("fnt_ovr_small_t0.zmp"); false if it has none.
    bool atlasFont(const std::string &atlas, ps2x::zaxis::LibraryFont &font);
}
