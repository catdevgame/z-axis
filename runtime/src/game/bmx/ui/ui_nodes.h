#pragma once
// BMX XXX's calls into zgui by a node's name (bmx.c, ui_nodes.cpp): an
// animation node under a root played or stopped, a script run on a node.
// BMX XXX's own code.

#include "ee/ee.h"
#include "zge/zgui/widget.h"

#include <cstdint>

namespace game::bmx
{
    // The animation node `name` under `root` played, or stopped; nothing if
    // there is no root or no such node.
    void playUiNode(ee::Ptr<zge::zgui::Node> root, ee::Address name);
    void stopUiNode(ee::Ptr<zge::zgui::Node> root, ee::Address name);
    // A zgui script's text run on a node.
    void runUiScript(ee::Ptr<zge::zgui::Node> node, ee::Address text);
}
