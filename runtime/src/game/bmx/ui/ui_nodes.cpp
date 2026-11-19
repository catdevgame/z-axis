// BMX XXX's calls into zgui by a node's name (ui_nodes.h): BMX XXX's own
// code (bmx.c).

#include "ui_nodes.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/zgui/script.h"

#include <cstdint>
#include "sym/bmx/ui/ui_nodes.cpp.h"

namespace game::bmx
{
    using zge::zgui::Node;

    namespace
    {
        // The kind findNode looks for: an animation node.
        constexpr uint32_t kAnimationNode = 4;
        // The node an animation plays on, or stops on: its own.
        constexpr uint32_t kOwnTarget = 0;
        // A script run with no argument.
        constexpr uint32_t kNoArgument = 0;

        // zgui's tail calls, by address.
        // Scaffolding (address-call): the original's tail calls of zgui.
        void playWidgetTail(ee::Ptr<Node> animation, uint32_t target) { ee::tailCall<void>(sym::playWidget, animation, target); }
        void stopPlayingTail(ee::Ptr<Node> animation, uint32_t target) { ee::tailCall<void>(sym::stopPlaying, animation, target); }
        void runScriptTail(ee::Ptr<Node> node, ee::Address text, uint32_t argument) { ee::tailCall<void>(sym::runScript, node, text, argument); }
    }

    void playUiNode(ee::Ptr<Node> root, ee::Address name)
    {
        if (!root)
            return;
        const ee::Ptr<Node> animation = zge::zgui::findNode(root, name, kAnimationNode);
        if (animation)
            playWidgetTail(animation, kOwnTarget);
    }
    READABLE_FUNCTION_WAITS(playUiNode, Verified);

    void stopUiNode(ee::Ptr<Node> root, ee::Address name)
    {
        if (!root)
            return;
        const ee::Ptr<Node> animation = zge::zgui::findNode(root, name, kAnimationNode);
        if (animation)
            stopPlayingTail(animation, kOwnTarget);
    }
    READABLE_FUNCTION_WAITS(stopUiNode, Verified);

    void runUiScript(ee::Ptr<Node> node, ee::Address text)
    {
        runScriptTail(node, text, kNoArgument);
    }
    READABLE_FUNCTION_WAITS(runUiScript, Verified);
}
