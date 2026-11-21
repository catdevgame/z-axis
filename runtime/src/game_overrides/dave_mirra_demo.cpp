// Dave Mirra Freestyle BMX 2 (SLUS-20159): the attract demo, as the Xbox and
// GameCube releases play it ([extras] attract_demo, ZX_DM2_ATTRACT_DEMO=1;
// off by default).
//
// Left at the main menu, the game plays one of its own replays (REPLAY.ZAL)
// with the replay overlay's Demo frame, and a button or the replay's end
// brings the menu back. The PS2 keeps most of it: the main menu's idle
// timer (UILIB MainMenu.MainMenuFrame.DemoMode, a guiIdleFunc of 2800
// frames), CommonUI's StartDemo (the load screen's fade) and FlowControl
// ("Call BmxStartDemo"), the level state's demo (levelStateEnter with no
// players: the level and replay of g_demoLevel and g_demoReplayChoice read
// from the library, counted in g_demoReplaysRead) and actionStart's demo
// mode (kActionDemo: the replay playing, OverlayReplay.Demo shown). What it
// lacks is the Xbox's (its executable's addresses):
//   - the idle timer's script (frontEndEnter 0x14610 sets DemoMode's
//     OnActivationScript to PLAY PlayDemo; PlayDemo loads a load screen and
//     plays CommonUI.StartDemo): added to the UILIB, with the PS2's own load
//     screens (UILIB:Load1..9.bmp, picked as its game starts pick them: the
//     Xbox's LoadDemo.bmp is not on the PS2 disc); StartDemo's end plays
//     FlowControl (the PS2's plays StartGameReal, BmxStartGame);
//   - BmxStartDemo (0x13F60): no players, the level state asked for as
//     BmxStartGame asks for it. The demo's level and replay are the Xbox
//     table's (0x1A0D10: level 3, then Woodward Camp, then level 7, each its
//     first replay), in turn by the demos read (its levelStateEnter picks
//     them there; the PS2's reads the two bytes set here);
//   - the demo mode's own frame (the action modes' table, 0x1A0D00: mode 4
//     is 0x2C3A0, not the replays' frame): the picture fades in from white
//     (120 frames); the demo's replay plays (the replay's rider, the level's
//     contacts, its objects at a 60th of a second, their sounds, the effects'
//     clock; the song's sound events at frames 60, 6,900 and 7,800); any
//     newly pressed button on either pad, or the replay's end, fades out to
//     white (120 frames) and the front end comes back. On the PS2 mode 4's
//     frame is replayModeFrame, which drives the round's replay
//     (shownReplay) and ends in play: replaced in mode 4 by that frame.
// Two things the PS2 adds: its main menu at boot is still the intro
// state's, whose exit keeps the menu scene for the front end (a level
// entered from it finds a scene and does not load), so the intro left for
// the demo runs the front end's exit after its own; and its REPLAY.ZAL
// holds level 3's demo replays alone (see kDiscDemos).

#include "dave_mirra.h"
#include "sym/game_overrides/dave_mirra_demo.cpp.h"
#include "zge_host.h"

#include "ps2_runtime.h"
#include "runtime/ps2_boot_config.h"
#include "runtime/ps2_disc_patch.h"
#include "runtime/ps2_game_files.h"
#include "runtime/ps2_session.h"
#include "runtime/ps2_zaxis_ui.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <iostream>
#include <string>
#include <vector>

namespace dave_mirra::demo
{
    namespace
    {

        constexpr const char kTag[] = "[demo]";
        constexpr const char kOwner[] = "dm2-demo";
        constexpr int kRegV0 = 2, kRegSp = 29, kRegRa = 31;

        // The Xbox's demos: level, and which of the level's two replays.
        struct Demo
        {
            uint8_t level;
            uint8_t replay;
        };
        constexpr Demo kDemos[] = {{3, 0}, {2, 0}, {7, 0}};
        // The PS2 disc's REPLAY.ZAL holds level 3's demo replays alone (its
        // other demo groups are left-over bytes that crash the level they
        // load); the Xbox's holds the rest. Made at app build
        // (tooling/ai/tools/dm2_xbox.py: <ZX_DM2_XBOX_LEVELS_DIR>/demo/
        // REPLAY.ZAL, the PS2's with the demo groups it lacks the Xbox's);
        // without it the demo is level 3's alone.
        constexpr size_t kDiscDemos = 1;
        constexpr const char kReplayLibrary[] = "ASSETS/REPLAY.ZAL";

        // game/dm2 (action/action.h, action/action_state.h, replay/replays.h,
        // overlay/overlay.h, level/level_load_progress.h, rider/rider.h).
        constexpr uint32_t kGameStateFrontEnd = 0, kGameStateLevel = 1;
        constexpr uint8_t kActionDemo = 4;
        constexpr uint8_t kReplayPlaying = 2, kReplayPlayedOut = 4;
        constexpr uint32_t kLevelLoadParts = 9;
        constexpr uint32_t kOverlayFade = 17;
        constexpr uint32_t kOverlaySongInfo = 14;
        constexpr uint32_t kPlayPhaseStopped = 4;
        constexpr uint32_t kAnyPressed = 0;
        constexpr uint32_t kEveryButton = 0xFFFFu;
        constexpr uint32_t kPads = 2;

        // The demo frame's steps (the Xbox's, in the action step byte).
        constexpr uint8_t kStepFadeIn = 0, kStepPlaying = 1, kStepLeft = 2, kStepPlayedOut = 3;
        constexpr uint32_t kFadeFrames = 120;
        constexpr float kFrameSeconds = 1.0f / 60.0f;
        // At these frames of the demo's play the song playing (soundEvent 2:
        // the track asked for, else the one playing; then 4, the soundtrack's
        // index, as the Xbox asks it) is shown by the song's label.
        constexpr uint32_t kSongCueFrames[] = {60, 6900, 7800};
        constexpr uint32_t kSoundTrackPlaying = 2, kSoundSoundtrack = 4;
        // A fade (overlay message kOverlayFade): frames, flags, colour.
        struct Fade
        {
            uint16_t frames;
            uint8_t flags; // 1: from the colour, 0: to it
            uint8_t red, green, blue;
        };
        constexpr uint8_t kFadeFromColour = 1, kFadeToColour = 0;

        // MainMenu: the idle timer plays the demo.
        constexpr const char kDemoAction[] = R"ZSC(ACTION "Script" "PlayDemo"
{
	//////////////////////////////
	// Property
	StartDelay	0
	Script	"DECLARE Sint32 Type\nRAND Type {1} {9}\nDECLARE String Tmp\nSET Tmp {\"UILIB:Load\"}\nSTRCAT Tmp Type\nSTRCAT Tmp {\".bmp\"}\nPLAY .DeInit\nLOADIMAGE LoadScreen *Tmp Root.CommonUI.StartDemo\nUNDECLARE Tmp\nUNDECLARE Type\n"
}
)ZSC";

        struct State
        {
            PS2Runtime::RecompiledFunction replayFrame = nullptr;
            PS2Runtime::RecompiledFunction introExit = nullptr;
            bool starting = false; // BmxStartDemo asked for the level state, not yet entered
            std::filesystem::path library; // the demo replays' library made at app build, if any
            uint32_t frames = 0; // the demo frame's count in its step
            uint32_t played = 0; // frames of the replay played
        };
        State &state() { return zx::session::state<State>(); }

        uint8_t readByte(uint8_t *rdram, uint32_t at)
        {
            const uint8_t *p = getConstMemPtr(rdram, at);
            return p ? *p : 0u;
        }
        void writeByte(uint8_t *rdram, uint32_t at, uint8_t value)
        {
            if (uint8_t *p = getMemPtr(rdram, at))
                *p = value;
        }

        // A guest call from a hook, its stack below the caller's.
        struct Calls
        {
            uint8_t *rdram;
            R5900Context *ctx;
            PS2Runtime *runtime;
            uint32_t stack;
            uint32_t operator()(uint32_t function, std::initializer_list<uint32_t> args = {}) const
            {
                uint32_t v0 = 0;
                runtime->callGuestFunction(rdram, ctx, function, args, stack, &v0);
                return v0;
            }
            uint32_t scratch() const { return stack + 0x100u; }
        };
        Calls calls(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            return {rdram, ctx, runtime, (getRegU32(ctx, kRegSp) - 0x400u) & ~0xFu};
        }

        void sendFade(const Calls &call, uint8_t flags)
        {
            const Fade fade{static_cast<uint16_t>(kFadeFrames), flags, 0xFF, 0xFF, 0xFF};
            if (uint8_t *p = getMemPtr(call.rdram, call.scratch()))
                std::memcpy(p, &fade, sizeof(fade));
            call(sym::dm2::overlayMessage, {kOverlayFade, call.scratch()});
        }

        std::shared_ptr<const std::vector<uint8_t>> provideLibrary(const std::string &path)
        {
            if (path != kReplayLibrary)
                return nullptr;
            std::ifstream in(state().library, std::ios::binary);
            if (!in)
                return nullptr;
            return std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        // ---- BmxStartDemo --------------------------------------------------

        void startDemo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Calls call = calls(rdram, ctx, runtime);
            state().starting = true;
            const size_t demos = state().library.empty() ? kDiscDemos : std::size(kDemos);
            uint32_t which = readByte(rdram, sym::dm2::g_demoReplaysRead) % demos;
            if (const char *forced = std::getenv("ZX_DM2_DEMO"); forced && *forced) // developers: that demo each time
                which = static_cast<uint32_t>(std::atoi(forced)) % demos;
            const Demo &demo = kDemos[which];
            writeByte(rdram, sym::dm2::g_demoLevel, demo.level);
            writeByte(rdram, sym::dm2::g_demoReplayChoice, demo.replay);
            writeByte(rdram, sym::dm2::g_multiplayerPlayers, 0);
            call(sym::dm2::setGameState, {kGameStateLevel});
            for (uint32_t part = 0; part < kLevelLoadParts; ++part)
                if (uint8_t *p = getMemPtr(rdram, sym::dm2::g_levelLoadParts + part * 4u))
                    std::memset(p, 0, 4);
            const uint32_t result = call(sym::dm2::setChunkProgressHook, {sym::dm2::levelLoadProgress});
            std::cout << kTag << " level " << int(demo.level) << ", replay " << int(demo.replay) << std::endl;
            SET_GPR_U32(ctx, kRegV0, result);
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        // ---- the demo mode's frame -------------------------------------------

        bool anyButtonPressed(const Calls &call)
        {
            for (uint32_t port = 0; port < kPads; ++port)
                if (call(sym::dm2::inputUpdate, {port, kEveryButton, kAnyPressed}) != 0)
                    return true;
            return false;
        }

        void setStep(uint8_t *rdram, uint8_t step)
        {
            writeByte(rdram, sym::dm2::g_actionStep, step);
            state().frames = 0;
        }

        // Answers 1 when the front end is asked for.
        uint32_t demoFrame(const Calls &call)
        {
            State &st = state();
            uint8_t *rdram = call.rdram;
            const uint8_t replay = readByte(rdram, sym::dm2::g_demoReplay);
            switch (readByte(rdram, sym::dm2::g_actionStep))
            {
            case kStepFadeIn:
                st.starting = false;
                if (++st.frames == 1)
                {
                    st.played = 0;
                    call(sym::dm2::showOverlayFrame, {kOverlayFade, 1});
                    sendFade(call, kFadeFromColour);
                    call(sym::dm2::followCamera);
                }
                else if (st.frames == kFadeFrames)
                {
                    setStep(rdram, kStepPlaying);
                }
                return 0;
            case kStepPlaying:
            {
                if (anyButtonPressed(call))
                {
                    setStep(rdram, kStepLeft);
                    call(sym::dm2::followCamera);
                    return 0;
                }
                call(sym::dm2::replayUpdate);
                const uint8_t played = static_cast<uint8_t>(call(sym::dm2::replayState, {replay}));
                if (played == kReplayPlaying)
                {
                    call(sym::dm2::riderFrame, {call(sym::dm2::replayRider, {replay}) & 0xFFu});
                    call(sym::dm2::runLevelContacts);
                    const float f12 = call.ctx->f[12];
                    call.ctx->f[12] = kFrameSeconds;
                    call(sym::dm2::updateObjects);
                    call.ctx->f[12] = f12;
                    call(sym::dm2::objectSoundsFrame);
                    call(sym::dm2::advanceEffectsClock);
                    ++st.played;
                    for (uint32_t cue : kSongCueFrames)
                        if (st.played == cue)
                        {
                            const uint8_t song = static_cast<uint8_t>(call(sym::dm2::soundEvent, {kSoundTrackPlaying, 0, 0, 0, 0}));
                            call(sym::dm2::soundEvent, {kSoundSoundtrack, 0, 0, 0, 0});
                            writeByte(rdram, call.scratch(), song); // the song label's message: the song
                            call(sym::dm2::overlayMessage, {kOverlaySongInfo, call.scratch()});
                        }
                }
                else if (played == kReplayPlayedOut)
                {
                    setStep(rdram, kStepPlayedOut);
                }
                call(sym::dm2::followCamera);
                return 0;
            }
            case kStepLeft:
            case kStepPlayedOut:
                if (++st.frames == 1)
                {
                    sendFade(call, kFadeToColour);
                    call(sym::dm2::setPlayPhase, {kPlayPhaseStopped});
                    return 0;
                }
                if (st.frames != kFadeFrames)
                    return 0;
                // The menus' player count back (the Xbox's front end sets it at each enter).
                writeByte(rdram, sym::dm2::g_multiplayerPlayers, 1);
                call(sym::dm2::setGameState, {kGameStateFrontEnd});
                return 1;
            default:
                return 0;
            }
        }

        // introStateExit: left for the demo, the front end's
        // exit after it. The PS2's main menu at boot is still the intro
        // state's, which enters the menu scene and, left for the front end,
        // keeps it; its exit leaves the scene to the front end's, so a level
        // entered from it finds a scene in place and does not load.
        void introExitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            st.introExit(rdram, ctx, runtime);
            if (st.starting)
                ctx->pc = sym::dm2::frontEndExit; // run as the intro exit's tail call (it waits: no host-made call)
        }

        // replayModeFrame: in the demo mode, the demo's own frame.
        void replayFrameHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            State &st = state();
            if (readByte(rdram, sym::dm2::g_actionMode) != kActionDemo)
            {
                st.replayFrame(rdram, ctx, runtime);
                return;
            }
            const uint32_t result = demoFrame(calls(rdram, ctx, runtime));
            SET_GPR_U32(ctx, kRegV0, result);
            ctx->pc = getRegU32(ctx, kRegRa);
        }

        // ---- the UI --------------------------------------------------------

        bool patchMainMenu(std::string &text)
        {
            zx::zaxis::Span idle = zx::zaxis::findObject(text, "MainMenu/MainMenuFrame/DemoMode");
            if (!idle.found() || zx::zaxis::findProperty(text, idle, "OnActivationScript").found())
                return false;
            const zx::zaxis::Span once = zx::zaxis::findProperty(text, idle, "ActivateOnce");
            if (!once.found())
                return false;
            const int depth = zx::zaxis::indentAt(text, idle.begin) + 1;
            const size_t lineEnd = text.find('\n', once.end);
            if (lineEnd == std::string::npos)
                return false;
            text.insert(zx::zaxis::objectEnd(text, idle), zx::zaxis::indentBlock(kDemoAction, depth));
            text.insert(lineEnd + 1, std::string(static_cast<size_t>(depth), '\t') + "OnActivationScript\t\"PLAY PlayDemo\"\r\n");
            return true;
        }

        bool patchCommonUi(std::string &text)
        {
            zx::zaxis::Span start = zx::zaxis::findObject(text, "CommonUI/StartDemo");
            return start.found() && zx::zaxis::replaceIn(text, start, "PLAY StartGameReal\"", "PLAY FlowControl\"");
        }

        bool patchUiLibrary(std::vector<uint8_t> &bytes)
        {
            std::vector<zx::zaxis::ArchiveEntry> entries;
            if (!zx::zaxis::readArchive(bytes, entries))
                return false;
            int patched = 0;
            for (zx::zaxis::ArchiveEntry &entry : entries)
            {
                const std::string name = zx::zaxis::scriptName(entry);
                if (name != "MainMenu" && name != "CommonUI")
                    continue;
                std::string text(entry.data.begin(), entry.data.end());
                if (name == "MainMenu" ? patchMainMenu(text) : patchCommonUi(text))
                {
                    entry.data.assign(text.begin(), text.end());
                    ++patched;
                }
                else
                {
                    std::cerr << kTag << " " << name << " is not the expected script: no attract demo" << std::endl;
                }
            }
            if (patched != 2)
                return false;
            bytes = zx::zaxis::writeArchive(entries);
            std::cout << kTag << " the main menu's idle timer plays the demo" << std::endl;
            return true;
        }
    }

    bool install(PS2Runtime &runtime)
    {
        const char *on = zx::boot::text("ZX_DM2_ATTRACT_DEMO");
        if (!on || on[0] != '1')
            return false;
        State &st = state();
        st.replayFrame = runtime.lookupFunction(sym::dm2::replayModeFrame);
        if (!st.replayFrame || !runtime.replaceFunction(sym::dm2::replayModeFrame, &replayFrameHook))
        {
            std::cerr << kTag << " replayModeFrame not hooked: no attract demo" << std::endl;
            return false;
        }
        st.introExit = runtime.lookupFunction(sym::dm2::introStateExit);
        if (!st.introExit || !runtime.replaceFunction(sym::dm2::introStateExit, &introExitHook))
        {
            std::cerr << kTag << " introStateExit not hooked: no attract demo" << std::endl;
            return false;
        }
        if (!zge_host::addScriptFunction(runtime, scriptFunctions(), "BmxStartDemo", &startDemo))
            return false;
        zx::disc_patch::registerPatch("UILIB.ZAL", &patchUiLibrary, kOwner);
        std::error_code ec;
        if (const char *dir = zx::boot::text("ZX_DM2_XBOX_LEVELS_DIR"); dir && *dir &&
            std::filesystem::is_regular_file(std::filesystem::path(dir) / "demo" / "REPLAY.ZAL", ec))
        {
            st.library = std::filesystem::path(dir) / "demo" / "REPLAY.ZAL";
            zx::game_files::registerProvider("ASSETS/", &provideLibrary, kOwner);
        }
        std::cout << kTag << " on: the Xbox's attract demo at the main menu ("
                  << (st.library.empty() ? std::string("level 3's alone: no Xbox demo replays") : st.library.string()) << ")"
                  << std::endl;
        return true;
    }
}

namespace dave_mirra
{
    bool installAttractDemo(PS2Runtime &runtime)
    {
        return demo::install(runtime);
    }
}
