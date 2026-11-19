// zgui's tree walks: the children of a node, and drawing a widget tree.

#include "widget.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget_parts.h"
#include "zge/scene/message.h"
#include "zge/scene/scene.h"
#include "zge/math/types.h"

#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/vu0.h"
#include "ee/plain.h"
#include "zge/math/vec.h"
#include "sym/zge/zgui/draw.cpp.h"

namespace zge::zgui
{
    void forEachChild(ee::Ptr<Node> node, uint32_t visit)
    {
        // The next sibling is read before the visit, which may unlink the child.
        const ee::Callback<void(ee::Ptr<Node>)> visitChild{visit};
        for (ee::Ptr<Node> child = node->firstChild; child;)
        {
            const ee::Ptr<Node> next = child->nextSibling;
            visitChild(child);
            child = next;
        }
    }
    READABLE_FUNCTION(sym::forEachChild2, forEachChild, Verified);

    void sendMessage(ee::Ptr<Node> widget, uint32_t message, ee::Address argument)
    {
        for (ee::Ptr<Node> child = widget->firstChild; child; child = child->nextSibling)
        {
            if (child->kind() == NodeKind::Handler && child->message == message)
            {
                child->handler(widget, argument);
                return;
            }
        }
    }
    // A handler can wait (the pause menu's, in the career: run without a
    // fiber, the game stalled there), so it runs on a fiber (ee/suspend.cpp).
    READABLE_FUNCTION_WAITS(sendMessage, Verified);

    void drawWidget(ee::Ptr<Node> widget)
    {
        if (widget->kind() != NodeKind::Widget)
            return;
        if (!widget->shown)
            return;
        // Placed in the nearest widget above it (handlers and other nodes
        // between are skipped); a top-level widget keeps its screen position.
        ee::Ptr<Node> container = widget->parent;
        while (container && container->kind() != NodeKind::Widget)
            container = container->parent;
        // ZGE4 places it at its anchor in the container (placeAtAnchor).
        if (container)
        {
            if constexpr (game::readable::kIsBmx)
                placeAtAnchor(widget, container);
            else
                widget->screenPosition = container->screenPosition + widget->position;
        }
        sendMessage(widget, static_cast<uint32_t>(Message::Draw), 0u);
        // Its children through the game's function table (the HUD overrides
        // wrap this function).
        forEachChild(widget, sym::drawWidget);
        if constexpr (game::readable::kIsBmx)
            sendMessage(widget, static_cast<uint32_t>(Message::DrawAfter), 0u); // (ZGE4 calls it)
        else
            game::tail(zge::zgui::sendMessage, widget, static_cast<uint32_t>(Message::DrawAfter), 0u); // the original's jump to it
    }
    READABLE_FUNCTION_OWN(drawWidget, Verified, dm2, drawWidgetObject);

    // drawWidgetObject: ZGE2's drawWidget, a forEachAttached visitor: a shown
    // widget object is sent Draw at once, the objects hanging from it are
    // drawn through itself, then it is sent DrawAfter (each with itself as
    // the message's word). Always 1.
    uint32_t drawWidgetObject(ee::Ptr<zge::SceneObject> object, ee::Address /*context*/)
    {
        if ((object->flags & (zge::kObjectKindMask | kWidgetObjectShown)) != (kWidgetObjectKind | kWidgetObjectShown))
            return 1;
        zge::sendNewMessage(0u, object, static_cast<uint32_t>(Message::Draw), object, zge::kMessageNow);
        zge::forEachAttached(object, sym::drawWidget, 0u);
        zge::sendNewMessage(0u, object, static_cast<uint32_t>(Message::DrawAfter), object, zge::kMessageNow);
        return 1;
    }

    // drawZgui: the UI drawn: the root's screen position set to the origin, then
    // each of its widgets drawn (drawWidget). Returns 1.
    int32_t drawZgui()
    {
        const ee::Ptr<Node> root = *g_root;
        if (root)
        {
            root->screenPosition = zge::Vec4{0.0f, 0.0f, 0.0f, 1.0f};
            forEachChild(root, sym::drawWidget);
        }
        return 1;
    }
    READABLE_FUNCTION_OWN(drawZgui, Verified, dm2, drawWidgetObjects);

    // drawWidgetObjects: ZGE2's drawZgui: each widget object hanging from the
    // root drawn (drawWidgetObject; the objects have no screen position to
    // set). Returns 1.
    int32_t drawWidgetObjects()
    {
        zge::forEachAttached(ee::Ptr<zge::SceneObject>(g_root->address), sym::drawWidget, 0u);
        return 1;
    }
}
