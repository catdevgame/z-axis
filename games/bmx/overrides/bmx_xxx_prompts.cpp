// BMX XXX (SLUS-20415): button prompts for the device in use, on
// zge_prompts.cpp (shared with AI and DM2).
//
// BMX XXX draws its buttons as pictures of AI's ps2Buttons.zif (the same
// names: select_button Cross, cancel_button Triangle, secondary_button and
// circle_but Circle, Tird_Button and btn_psz_sq Square, shoulder_button R1,
// DPad_Button): the Common script loads it as Buttons by Root.PlatformID
// (xboxButtons.zif, gcButtons.zif on the other releases), the park editor
// as ButtonsPE; and in a level as characters of the Overlay library's
// ps2_btn font. The PS2 disc has the other two libraries, so the pictures
// get the device's: the Xbox release's for an Xbox-style pad, the GameCube
// release's for a Nintendo-style pad (the picture of the button with the
// letter the pad's button has: its A, on the right, is Circle), key caps in
// the game's font_test for the keyboard. Its text names buttons ("X
// button", "CIRCLE button", "L1 button", the $KSTR_PBTN_ strings'): the
// device's names for them.
//
// The Controls screen (UI_menu's Controller) shows the Controller library's
// "image" (ps2ctrl.zif, xboxctrl.zif or gcctrl.zif by Root.PlatformID) with
// one set of captions and leader lines, placed for the PS2 photo: the Xbox
// release shows its own picture (cntrl_xbox.zmp) under the same lines,
// which miss its buttons, and the GameCube release an Xbox pad. So for every
// device the PS2 photo stays, its controls drawn over as the device's
// (zge_prompts.h PadPhoto): the Xbox release's buttons, the GameCube
// release's letters for a Nintendo-style pad, key caps.
//
// With the app's settings (ZX_GAME_SETTINGS_MENUS=1), as the Settings
// page: the redrawn pictures are the game's RAM.

#include "bmx_xxx.h"
#include "sym/game_overrides/bmx_xxx_prompts.cpp.h"

#include "runtime/boot_config.h"
#include "runtime/host_input.h"
#include "overrides/zge_prompts.h"

#include <iterator>

namespace bmx_xxx::prompts
{
    namespace
    {
        namespace hi = ps2_host_input;
        namespace zp = zge_host::prompts;
        using Device = zp::Device;
        using zp::Picture;
        using zp::Shows;

        // ps2Buttons.zif, by TXRH name.
        const zp::TexturePictures kTextures = {
            {"select_button", {Shows::Button, hi::kCross}},
            {"cancel_button", {Shows::Button, hi::kTriangle}},
            {"secondary_button", {Shows::Button, hi::kCircle}},
            {"circle_but", {Shows::Button, hi::kCircle}},
            {"Tird_Button", {Shows::Button, hi::kSquare}},
            {"btn_psz_sq", {Shows::Button, hi::kSquare}},
            {"shoulder_button", {Shows::Button, hi::kR1}},
            {"DPad_Button", {Shows::DPad, -1}},
            // The Controls screen's picture (ps2ctrl.zif).
            {"image", {Shows::Pad, -1}},
        };

        // In a level the footers' buttons are characters of the Overlay
        // library's ps2_btn font ("Overlay.ps2_btn": '1' Cross, '2' Square,
        // '3' Triangle, '4' Circle, as DM2's ButtonLib; the Xbox release
        // sets Overlay.xbox_btn, with the same characters).
        const zp::FontPictures kFonts = {
            {"ps2_btn_t0.zmp",
             {{'1', Picture{Shows::Button, hi::kCross}},
              {'2', Picture{Shows::Button, hi::kSquare}},
              {'3', Picture{Shows::Button, hi::kTriangle}},
              {'4', Picture{Shows::Button, hi::kCircle}}}},
        };

        // The three releases' pictures, each button by position.
        constexpr zp::ArtSource kArt[] = {
            {Device::PlayStation, "btm_psx_x_wb.zmp", 0, hi::kCross},
            {Device::PlayStation, "btm_psx_tri_wb.zmp", 0, hi::kTriangle},
            {Device::PlayStation, "btm_psx_o_wb.zmp", 0, hi::kCircle},
            {Device::PlayStation, "btn_psz_sq.zmp", 0, hi::kSquare},
            {Device::PlayStation, "btn_r1_arrow_up.zmp", 0, hi::kR1},
            {Device::Xbox, "xbox_btn_a.tga", 0, hi::kCross},
            {Device::Xbox, "xbox_btn_b.tga", 0, hi::kCircle},
            {Device::Xbox, "xbox_btn_x.tga", 0, hi::kSquare},
            {Device::Xbox, "xbox_btn_y.tga", 0, hi::kTriangle},
            {Device::Xbox, "xbox_btn_dpad.tga", 0, zp::kDPadArt},
            {Device::Nintendo, "gc_btn_a.zmp", 0, hi::kCircle},   // A, on the right
            {Device::Nintendo, "gc_btn_b.zmp", 0, hi::kCross},    // B, at the bottom
            {Device::Nintendo, "gc_btn_y.zmp", 0, hi::kSquare},   // Y, on the left
            {Device::Nintendo, "gc_btn_x.zmp", 0, hi::kTriangle}, // X, at the top
        };

        // The $KSTR_PBTN_ strings and the tips' words (STRING.ZAL).
        constexpr zp::Phrase kPhrases[] = {
            {"X button", hi::kCross},
            {"CIRCLE button", hi::kCircle},
            {"TRIANGLE button", hi::kTriangle},
            {"SQUARE button", hi::kSquare},
            {"SELECT button", hi::kSelect},
            {"START button", hi::kStart},
            {"L1 button", hi::kL1},
            {"R1 button", hi::kR1},
            {"L2 button", hi::kL2},
            {"R2 button", hi::kR2},
        };

        const zp::Game kGame{
            .tag = "[bmx-xxx]",
            .loadTextureChunk = sym::bmx::loadTextureChunk,
            .newLoadedTexture = sym::bmx::newLoadedTexture,
            .releaseLoadedTexture = sym::bmx::releaseLoadedTexture,
            .drawText = sym::bmx::drawText,
            .textures = &kTextures,
            .fonts = &kFonts,
            .art = kArt,
            .artCount = std::size(kArt),
            .labelFont = "font_test.zfn",
            .phrases = kPhrases,
            .phraseCount = std::size(kPhrases),
            .padPhoto = &zp::dualShockPhoto(),
        };
    }
}

namespace bmx_xxx
{
    void installPrompts(PS2Machine &runtime)
    {
        if (const char *on = zx::boot::text("ZX_GAME_SETTINGS_MENUS"); !on || on[0] != '1')
            return;
        zge_host::prompts::install(runtime, prompts::kGame);
    }
}
