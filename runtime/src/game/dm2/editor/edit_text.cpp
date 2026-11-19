// DM2's park editor: the editor's texts (edit_text.h). DM2's own code (its
// editor block).

#include "edit_text.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/text/strings.h"

#include <cstdint>
#include "sym/dm2/editor/edit_text.cpp.h"

namespace game::dm2
{
    const char *editorText(uint32_t key)
    {
        return zge::stringTableText(key);
    }
    READABLE_FUNCTION(editorText, Verified);
}
