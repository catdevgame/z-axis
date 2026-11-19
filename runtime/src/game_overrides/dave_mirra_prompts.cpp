// Dave Mirra Freestyle BMX 2 (SLUS-20159): button prompts for the device in
// use, on zge_prompts.cpp (shared with AI and BMX XXX).
//
// DM2 draws its buttons as characters of one font, the ButtonLib library's
// "button" (the front end's footer prompts, "1 Select" "3 Back"; the trick
// list's "AH1"; the pause screen's Continue): CommonUI's OnCreate loads
// ps2button.zif, xboxbutton.zif or gcbutton.zif as ButtonLib by
// Root.PlatformID, three fonts with the same characters for the same
// buttons ('1' Cross, '2' Square, '3' Triangle, '4' Circle by position:
// the Xbox A X Y B, the GameCube's A B Y X; A-H the stick's eight
// directions, U up and down). The PS2 disc has all three, so the PS2 font's
// atlas (ps2_btn_t0.zmp, 128 by 128) gets the device's: the Xbox font's
// pictures for an Xbox-style pad, the GameCube font's for a Nintendo-style
// pad (the picture of the button with the letter the pad's button has:
// its A, on the right, is Circle), key caps in the menus' mnu_tiny for the
// keyboard; the directions stay. Its text names a button in the trick
// lists and tips ("square button"): the device's name for it.
//
// The Controller screen (Options > Controller 1 / 2, MainMenu's
// ControllerFrame) shows the Controller library's "image" with captions and
// leader lines to its controls, all three releases' in one script: by
// Root.PlatformID it loads ps2Ctrl.zif, xboxCtrl.zif or gcCtrl.zif and shows
// the Ps2Line, XBoxLine or GcLine group (the configuration toggler's
// Ps2Config, XBoxConfig and GcConfig set their captions). With an
// Xbox-style pad the PS2 photo becomes the Xbox release's picture
// (cntrlr_xbox.tga, on the PS2 disc too) and the XBoxLine group shows, as
// on the Xbox; with a Nintendo-style pad (the GameCube picture's buttons
// are not where a Nintendo-style pad has them), the keyboard or a remapped
// pad, the photo's controls are drawn over (zge_prompts.h PadPhoto) under
// the Ps2Line captions. The frame's Init and the toggler's ShowConfig end
// with CALL pcPadPicture (the captions for the device), its DeInit with
// pcPadPictureClose, and while it is open two actions call
// pcPadPictureTick in turn (a device picked up meanwhile).
//
// With the app's settings (PS2X_GAME_SETTINGS_MENUS=1), as the Settings
// page: the redrawn atlas is the game's RAM, the Controller screen's script
// functions move its heap.

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_prompts.cpp.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_host_input.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_zaxis_ui.h"
#include "zge_host.h"
#include "zge_prompts.h"
#include "zge_settings.h"

#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace dave_mirra::prompts
{
    namespace
    {
        namespace hi = ps2_host_input;
        namespace zp = zge_host::prompts;
        using Device = zp::Device;
        using zp::Picture;
        using zp::Shows;

        // The Controller screen's picture (ps2Ctrl.zif).
        const zp::TexturePictures kTextures = {
            {"image", {Shows::Pad, -1}},
        };

        // ButtonLib's font (ps2_btn.zfn): its face buttons.
        const zp::FontPictures kFonts = {
            {"ps2_btn_t0.zmp",
             {{'1', Picture{Shows::Button, hi::kCross}},
              {'2', Picture{Shows::Button, hi::kSquare}},
              {'3', Picture{Shows::Button, hi::kTriangle}},
              {'4', Picture{Shows::Button, hi::kCircle}}}},
        };

        // The three releases' fonts, each button by position.
        constexpr zp::ArtSource kArt[] = {
            {Device::PlayStation, "ps2_btn.zfn", '1', hi::kCross},
            {Device::PlayStation, "ps2_btn.zfn", '2', hi::kSquare},
            {Device::PlayStation, "ps2_btn.zfn", '3', hi::kTriangle},
            {Device::PlayStation, "ps2_btn.zfn", '4', hi::kCircle},
            {Device::Xbox, "xbox_btn.zfn", '1', hi::kCross},    // A
            {Device::Xbox, "xbox_btn.zfn", '2', hi::kSquare},   // X
            {Device::Xbox, "xbox_btn.zfn", '3', hi::kTriangle}, // Y
            {Device::Xbox, "xbox_btn.zfn", '4', hi::kCircle},   // B
            {Device::Nintendo, "gc_btn.zfn", '1', hi::kCircle},   // A, on the right
            {Device::Nintendo, "gc_btn.zfn", '2', hi::kCross},    // B, at the bottom
            {Device::Nintendo, "gc_btn.zfn", '3', hi::kSquare},   // Y, on the left
            {Device::Nintendo, "gc_btn.zfn", '4', hi::kTriangle}, // X, at the top
            // The Xbox release's Controller screen picture (xboxCtrl.zif).
            {Device::Xbox, "cntrlr_xbox.tga", 0, zp::kPadArt},
        };

        // The trick lists' and tips' words (STRING.EUS).
        constexpr zp::Phrase kPhrases[] = {
            {"square button", hi::kSquare},
            {"square", hi::kSquare}, // the secret codes: "U D U L U D R R square"
        };

        const zp::Game kGame{
            .tag = "[dave-mirra]",
            .loadTextureChunk = sym::dm2::loadTextureChunk,
            .newLoadedTexture = sym::dm2::newLoadedTexture,
            .releaseLoadedTexture = sym::dm2::releaseLoadedTexture,
            .drawText = sym::dm2::drawText,
            .textures = &kTextures,
            .fonts = &kFonts,
            .art = kArt,
            .artCount = std::size(kArt),
            .labelFont = "mnu_tiny.zfn",
            .phrases = kPhrases,
            .phraseCount = std::size(kPhrases),
            .padPhoto = &zp::dualShockPhoto(),
        };

        // ---- the Controller screen's captions -------------------------------

        constexpr const char *kFrame = "Root.MainMenu.ControllerFrame";

        struct ControllerScreen
        {
            bool open = false;
            int ticks = 0;
            int shown = -1; // 1: the Xbox release's captions, 0: the PS2's
        };
        ControllerScreen &screen() { return ps2x::session::state<ControllerScreen>(); }

        void runOnRoot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const std::vector<std::string> &lines)
        {
            const uint32_t root = zge_host::settings::readWord(rdram, sym::dm2::g_uiRoot);
            zge_host::settings::runScript(rdram, ctx, runtime, root, lines);
        }

        // The caption group that goes with the picture the device gets, its
        // captions for the configuration picked.
        void showCaptions(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int xbox = zp::padArtShown(PSPadBackend::activeDevice()) ? 1 : 0;
            screen().shown = xbox;
            const std::string frame = kFrame;
            runOnRoot(rdram, ctx, runtime,
                      {"SET " + frame + ".Image.GcLine.Visible {0}",
                       "SET " + frame + ".Image.Ps2Line.Visible {" + (xbox ? "0" : "1") + "}",
                       "SET " + frame + ".Image.XBoxLine.Visible {" + (xbox ? "1" : "0") + "}",
                       "PLAY " + frame + ".Menu.SelectConfiguration." + (xbox ? "XBoxConfig" : "Ps2Config")});
        }

        void returnToScript(R5900Context *ctx) { ctx->pc = getRegU32(ctx, 31); }

        // Init's and ShowConfig's end.
        void padPictureFunction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ControllerScreen &s = screen();
            const bool opening = !s.open;
            s.open = true;
            showCaptions(rdram, ctx, runtime);
            if (opening)
            {
                s.ticks = 0;
                runOnRoot(rdram, ctx, runtime, {std::string("PLAY ") + kFrame + ".PadTickA"});
            }
            returnToScript(ctx);
        }

        // Every other frame while the screen is open: the device's captions.
        void padPictureTickFunction(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ControllerScreen &s = screen();
            if (s.open)
            {
                if ((zp::padArtShown(PSPadBackend::activeDevice()) ? 1 : 0) != s.shown)
                    showCaptions(rdram, ctx, runtime);
                ++s.ticks;
                runOnRoot(rdram, ctx, runtime, {std::string("PLAY ") + kFrame + (s.ticks % 2 ? ".PadTickB" : ".PadTickA")});
            }
            returnToScript(ctx);
        }

        void padPictureCloseFunction(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            screen().open = false;
            returnToScript(ctx);
        }

        const char *const kTicks = R"ZSC(ACTION "Interpolate" "PadTickA"
{
	StartDelay	0
	OnEnd	"CALL pcPadPictureTick"
	InterpolateMode	0
	LoopMode	0
	IntervalMode	1
	IntervalValue	2
	Ratio	1
}
ACTION "Interpolate" "PadTickB"
{
	StartDelay	0
	OnEnd	"CALL pcPadPictureTick"
	InterpolateMode	0
	LoopMode	0
	IntervalMode	1
	IntervalValue	2
	Ratio	1
}
)ZSC";

        // MainMenu's ControllerFrame: the calls and the ticks. False (and
        // the script left alone) if an anchor is missing.
        bool patchControllerFrame(std::string &text)
        {
            namespace zx = ps2x::zaxis;
            std::string edited = text;
            zx::Span frame = zx::findObject(edited, "MainMenu/ControllerFrame");
            zx::Span image = zx::findObject(edited, "MainMenu/ControllerFrame/Image");
            if (!frame.found() || !image.found())
                return false;
            // The ticks before the Image (the frame's actions end there).
            edited.insert(image.begin, zx::indentBlock(kTicks, zx::indentAt(edited, image.begin)));
            frame = zx::findObject(edited, "MainMenu/ControllerFrame");
            const bool ok =
                zx::replaceIn(edited, frame, R"(LOADLIB Controller UILIB:ps2Ctrl.zif LoadComplete\nGOTO End\n:End")",
                              R"(LOADLIB Controller UILIB:ps2Ctrl.zif LoadComplete\nGOTO End\n:End\nCALL pcPadPicture")") &&
                zx::replaceIn(edited, frame, R"(PLAY Ps2Config\nGOTO End\n:End\n")",
                              R"(PLAY Ps2Config\nGOTO End\n:End\nCALL pcPadPicture\n")") &&
                zx::replaceIn(edited, frame, R"(PLAY Root.CommonUI.LockUser\nSET Image.Visible {0}")",
                              R"(PLAY Root.CommonUI.LockUser\nSET Image.Visible {0}\nCALL pcPadPictureClose")");
            if (!ok)
                return false;
            text = std::move(edited);
            return true;
        }

        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            namespace zx = ps2x::zaxis;
            std::vector<zx::ArchiveEntry> entries;
            if (!zx::readArchive(bytes, entries))
                return false;
            for (zx::ArchiveEntry &entry : entries)
            {
                if (zx::scriptName(entry) != "MainMenu")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                if (!patchControllerFrame(text))
                {
                    std::cerr << "[dave-mirra] MainMenu's ControllerFrame is not the expected script: the Controller "
                                 "screen keeps the PS2 captions"
                              << std::endl;
                    return false;
                }
                entry.data.assign(text.begin(), text.end());
                bytes = zx::writeArchive(entries);
                return true;
            }
            return false;
        }
    }
}

namespace dave_mirra
{
    void installPrompts(PS2Runtime &runtime)
    {
        if (const char *on = ps2x::boot::text("PS2X_GAME_SETTINGS_MENUS"); !on || on[0] != '1')
            return;
        if (!zge_host::prompts::install(runtime, prompts::kGame))
            return;
        for (const auto &[name, fn] : {std::pair<const char *, PS2Runtime::RecompiledFunction>{"pcPadPicture", &prompts::padPictureFunction},
                                       {"pcPadPictureTick", &prompts::padPictureTickFunction},
                                       {"pcPadPictureClose", &prompts::padPictureCloseFunction}})
            if (!zge_host::addScriptFunction(runtime, scriptFunctions(), name, fn))
                return;
        ps2x::disc_patch::registerPatch("UILIB.ZAL", &prompts::patchUiLibrary);
    }
}
