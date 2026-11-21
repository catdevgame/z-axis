// DM2's in-game overlay: the replay mark (kOverlayReplayMark): a blinking
// ">" at the screen's top right while the replay recorder is recording or
// playing, in the state's colour. The level's code (action.c) sends it the
// recorder's state. It has no widget: it is drawn in the overlay's small
// font. DM2's own code (overlay.c).

#include "frames.h"
#include "overlay_calls.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/quaternion.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sym/dm2/overlay/frame_replay_mark.cpp.h"

namespace game::dm2
{
    namespace
    {
        // What the mark keeps: the recorder's state as it was sent
        // (kReplayIdle, kReplayRecording, kReplayPlaying and one more) and
        // the frames since.
        struct ReplayMarkState
        {
            uint8_t replayState;
            uint8_t pad01; // padding: no reader (pad2: the senders never set it; only whole-message copies move it)
            uint16_t frames;
        };
        static_assert(offsetof(ReplayMarkState, frames) == 2 && sizeof(ReplayMarkState) == 4);
        inline ee::Global<ReplayMarkState, sym::g_replayMarkState> g_replayMarkState;

        // Each state's letter ("", "R", "P", "p", ""; a state with no letter
        // has no mark: the letter itself is not drawn) and colour.
        constexpr uint32_t kReplayMarkStates = 6;
        inline ee::Global<ee::Ptr<const char>[kReplayMarkStates], sym::g_replayMarkLetters> g_replayMarkLetters;
        inline ee::Global<uint8_t[kReplayMarkStates][4], sym::g_replayMarkColours> g_replayMarkColours;
        constexpr ee::Ptr<const int8_t> kMark{sym::g_textReplayMark}; // ">"

        inline ee::Global<ee::Ptr<zge::Font>, sym::g_overlaySmallFont> g_overlaySmallFont;
        constexpr ee::Float kMarkX = 460.0f, kMarkY = 44.0f;
        constexpr ee::Float kMarkScale = 2.0f;
        constexpr uint32_t kAlignRight = 2; // zge::TextStyle::align
        // The mark is on for sixteen frames and off for sixteen.
        constexpr uint16_t kBlinkOffFrames = 0x10;
    }

    void overlayReplayMarkSetUp()
    {
        g_replayMarkState->replayState = 0;
    }
    READABLE_FUNCTION(overlayReplayMarkSetUp, Verified);

    void overlayReplayMarkFrame()
    {
        ee::Local<zge::TextStyle> style;
        ReplayMarkState &state = *g_replayMarkState;
        const ee::Ptr<const char> letter = (*g_replayMarkLetters)[state.replayState];
        if (*letter != 0 && (state.frames & kBlinkOffFrames) == 0)
        {
            style->scaleX = kMarkScale;
            style->scaleExtra[0] = kMarkScale;
            style->scaleY = kMarkScale;
            zge::quaternionIdentity(calls::textStyleRotation(style));
            style->align = kAlignRight;
            style->lineSpacing = 1.0f;
            style->position.x = kMarkX;
            style->position.y = kMarkY;
            style->letterSpacing = 0;
            std::memset(&style->reserved, 0, 1); // its low byte only
            style->position.z = 0.0f;
            const uint8_t *colour = (*g_replayMarkColours)[state.replayState];
            for (uint32_t i = 0; i < 4; ++i)
                style->colour[i] = colour[i];
            zge::drawText(*g_overlaySmallFont, style, kMark);
        }
        ++state.frames;
    }
    READABLE_FUNCTION(overlayReplayMarkFrame, Draft);

    void overlayReplayMarkShow() {}
    READABLE_FUNCTION(overlayReplayMarkShow, Verified);

    // The recorder's state sent: the blink starts over.
    void overlayReplayMarkMessage(ee::Ptr<const uint8_t> replayState)
    {
        zge::memoryCopy(sym::g_replayMarkState, replayState, sizeof(ReplayMarkState));
        g_replayMarkState->frames = 0;
    }
    READABLE_FUNCTION(overlayReplayMarkMessage, Verified);
}
