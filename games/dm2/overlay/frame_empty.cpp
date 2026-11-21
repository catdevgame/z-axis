// DM2's in-game overlay: the frame with no widget (kOverlayEmpty). Its
// handlers do nothing; the byte the game sends it is kept and never read.
// DM2's own code (overlay.c).

#include "frames.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/cstring.h"

#include <cstdint>
#include "sym/dm2/overlay/frame_empty.cpp.h"

namespace game::dm2
{
    namespace
    {
        inline ee::Global<uint8_t, sym::g_emptyFrameByte> g_emptyFrameByte;
    }

    void overlayEmptySetUp() {}
    READABLE_FUNCTION(overlayEmptySetUp, Verified);

    void overlayEmptyFrame() {}
    READABLE_FUNCTION(overlayEmptyFrame, Draft);

    void overlayEmptyShow() {}
    READABLE_FUNCTION(overlayEmptyShow, Verified);

    void overlayEmptyMessage(ee::Ptr<const uint8_t> value)
    {
        zge::memoryCopy(sym::g_emptyFrameByte, value, sizeof(uint8_t));
    }
    READABLE_FUNCTION(overlayEmptyMessage, Verified);

    // The handlers of two frames of the table that have nothing else: frame
    // 3 has a frame handler alone, frame 15 a set-up and a show handler.
    // All three are empty, and nothing here tells what the frames were.
    void overlayUnnamed3Frame() {}
    READABLE_FUNCTION(overlayUnnamed3Frame, Draft);

    void overlayUnnamed15SetUp() {}
    READABLE_FUNCTION(overlayUnnamed15SetUp, Verified);

    void overlayUnnamed15Show() {}
    READABLE_FUNCTION(overlayUnnamed15Show, Verified);
}
