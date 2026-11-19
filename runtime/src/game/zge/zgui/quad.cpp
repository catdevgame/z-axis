// zgui's rectangle widgets (widget.h): a textured quad with a colour per
// corner, aligned on the widget's position.

#include "zge/zgui/widget_parts.h"
#include "zge/zgui/widget_kinds.h"

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/render/primitives.h"

#include <cstring>
#include "sym/zge/zgui/quad.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kTexturedQuad = 0x02000004u;   // its kind: a quad with texture and colours

        // The 2D API's vertex.
        struct Vertex
        {
            ee::Float x, y, z;
            uint32_t _0c;
            ee::Float u, v;
            uint8_t colour[4];
            uint32_t _1c;
        };
        static_assert(sizeof(Vertex) == 0x20);
        // A quad's record (the original's stack frame).
        struct QuadRecord
        {
            uint32_t texture;   // +0x00
            ee::Ptr<Vertex> corner[4]; // +0x04 the vertices: bottom-left, bottom-right, top-right, top-left
            uint32_t _14[3];
            Vertex topLeft;     // +0x20
            Vertex topRight;    // +0x40
            Vertex bottomRight; // +0x60
            Vertex bottomLeft;  // +0x80
        };
        static_assert(offsetof(QuadRecord, topLeft) == 0x20 && offsetof(QuadRecord, bottomLeft) == 0x80);

        void setColour(uint8_t (&out)[4], const uint8_t (&in)[4], ee::Float alpha)
        {
            std::memcpy(out, in, 4);
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<uint8_t>(ee::toInt(ee::toFloat(out[c]) * alpha));
        }
    }

    int32_t drawQuadWidget(ee::Ptr<QuadWidget> widget)
    {
        ee::Local<QuadRecord> record;
        ee::Local<zge::Matrix> matrix;
        if (widget->width == 0.0f || widget->height == 0.0f)
            return 0;
        if (widget->textureName && !widget->texture)
            widget->texture = findGuiAsset(widget->textureName, 0);
        const ee::Ptr<UiAsset> texture = widget->texture;
        QuadRecord &quad = *record;
        const ee::Float half = 0.5f;

        // The corners about the position by the alignment (an alignment past
        // the three leaves its corners as they were).
        const uint32_t align = widget->alignment;
        const ee::Float w = widget->width;
        switch (align & 0xF)
        {
        case 0: quad.topLeft.x = 0.0f, quad.bottomRight.x = w, quad.topRight.x = w, quad.bottomLeft.x = 0.0f; break;
        case 1:
        {
            const ee::Float right = w * half, left = (-w) * half;
            quad.bottomRight.x = right, quad.topRight.x = right, quad.bottomLeft.x = left, quad.topLeft.x = left;
            break;
        }
        case 2: quad.topRight.x = 0.0f, quad.bottomRight.x = 0.0f, quad.bottomLeft.x = -w, quad.topLeft.x = -w; break;
        default: break;
        }
        const ee::Float h = widget->height;
        switch (align & 0xF0)
        {
        case 0x00: quad.topLeft.y = 0.0f, quad.bottomLeft.y = h, quad.topRight.y = 0.0f, quad.bottomRight.y = h; break;
        case 0x10:
        {
            const ee::Float bottom = h * half, top = (-h) * half;
            quad.bottomLeft.y = bottom, quad.bottomRight.y = bottom, quad.topRight.y = top, quad.topLeft.y = top;
            break;
        }
        case 0x20: quad.bottomRight.y = 0.0f, quad.bottomLeft.y = 0.0f, quad.topRight.y = -h, quad.topLeft.y = -h; break;
        default: break;
        }
        // Pixel centres.
        const ee::Float pixel = -0.5f;
        for (Vertex *v : {&quad.topLeft, &quad.topRight, &quad.bottomRight, &quad.bottomLeft})
        {
            v->x = v->x + pixel;
            v->y = v->y + pixel;
        }

        const ee::Float alpha = g_zgui->alpha;
        quad.topLeft.z = 0.0f, quad.topLeft.u = 0.0f, quad.topLeft.v = 0.0f;
        setColour(quad.topLeft.colour, widget->topLeft, alpha);
        quad.topRight.z = 0.0f, quad.topRight.u = widget->u, quad.topRight.v = 0.0f;
        setColour(quad.topRight.colour, widget->topRight, alpha);
        quad.bottomRight.z = 0.0f, quad.bottomRight.u = widget->u, quad.bottomRight.v = widget->v;
        setColour(quad.bottomRight.colour, widget->bottomRight, alpha);
        quad.bottomLeft.z = 0.0f, quad.bottomLeft.u = 0.0f, quad.bottomLeft.v = widget->v;
        setColour(quad.bottomLeft.colour, widget->bottomLeft, alpha);
        quad.corner[0] = &quad.bottomLeft;
        quad.corner[1] = &quad.bottomRight;
        quad.corner[2] = &quad.topRight;
        quad.corner[3] = &quad.topLeft;
        quad.texture = texture ? texture->object : g_zgui->defaultTexture;

        // Placed by a matrix: scaled, at the widget's screen position.
        matrixIdentity(matrix);
        matrixScale(matrix, matrix, &widget->scale.xyz());
        zge::matrixRow(*matrix, 3) = widget->widget.node.screenPosition;
        if constexpr (game::readable::kIsBmx)
            matrix->m[3][3] = 0.0f; // (ZGE4 clears the row's fourth word)
        drawPrimitiveQuads(kTexturedQuad, record, 1, matrix);
        return 1;
    }
    READABLE_FUNCTION_OWN(drawQuadWidget, Verified, dm2, drawListedImage);

    namespace
    {
        void setColour(uint8_t (&out)[4], Rgba in, ee::Float alpha)
        {
            const uint8_t channels[4] = {in.r, in.g, in.b, in.a};
            setColour(out, channels, alpha);
        }
    }

    // drawListedImage: ZGE2's drawQuadWidget, the image's Draw: the same quad
    // from the image's record, turned by the widget's orientation as well as
    // scaled. 0 for an image with no width or height, else 1.
    int32_t drawListedImage(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<QuadRecord> record;
        ee::Local<zge::Matrix> matrix;
        const ee::Ptr<ListedImage> image = messageRecord<ListedImage>(message);
        if (image->width == 0.0f || image->height == 0.0f)
            return 0;
        if (image->material && !image->asset)
            image->asset = findGuiAsset(image->material, kAssetTexture);
        QuadRecord &quad = *record;
        const ee::Float half = 0.5f;

        // The corners about the position by the alignment (an alignment past
        // the three leaves its corners as they were).
        const ee::Float w = image->width;
        switch (image->alignment & 0xF)
        {
        case 0: quad.topLeft.x = 0.0f, quad.topRight.x = w, quad.bottomRight.x = w, quad.bottomLeft.x = 0.0f; break;
        case 1:
        {
            const ee::Float right = w * half, left = (-w) * half;
            quad.topLeft.x = left, quad.topRight.x = right, quad.bottomRight.x = right, quad.bottomLeft.x = left;
            break;
        }
        case 2: quad.topRight.x = 0.0f, quad.bottomRight.x = 0.0f, quad.topLeft.x = -w, quad.bottomLeft.x = -w; break;
        default: break;
        }
        const ee::Float h = image->height;
        switch (image->alignment & 0xF0)
        {
        case 0x00: quad.topLeft.y = 0.0f, quad.topRight.y = 0.0f, quad.bottomRight.y = h, quad.bottomLeft.y = h; break;
        case 0x10:
        {
            const ee::Float bottom = h * half, top = (-h) * half;
            quad.topLeft.y = top, quad.topRight.y = top, quad.bottomRight.y = bottom, quad.bottomLeft.y = bottom;
            break;
        }
        case 0x20: quad.topLeft.y = -h, quad.bottomRight.y = 0.0f, quad.bottomLeft.y = 0.0f, quad.topRight.y = -h; break;
        default: break;
        }
        // Pixel centres.
        const ee::Float pixel = -0.5f;
        for (Vertex *v : {&quad.topLeft, &quad.topRight, &quad.bottomRight, &quad.bottomLeft})
        {
            v->x = v->x + pixel;
            v->y = v->y + pixel;
        }

        const ee::Float alpha = g_zgui->alpha;
        quad.topLeft.z = 0.0f, quad.topLeft.u = 0.0f, quad.topLeft.v = 0.0f;
        setColour(quad.topLeft.colour, image->topLeftColor, alpha);
        quad.topRight.z = 0.0f, quad.topRight.u = image->uScale, quad.topRight.v = 0.0f;
        setColour(quad.topRight.colour, image->topRightColor, alpha);
        quad.bottomRight.z = 0.0f, quad.bottomRight.u = image->uScale, quad.bottomRight.v = image->vScale;
        setColour(quad.bottomRight.colour, image->bottomRightColor, alpha);
        quad.bottomLeft.z = 0.0f, quad.bottomLeft.u = 0.0f, quad.bottomLeft.v = image->vScale;
        setColour(quad.bottomLeft.colour, image->bottomLeftColor, alpha);
        quad.corner[0] = &quad.bottomLeft;
        quad.corner[1] = &quad.bottomRight;
        quad.corner[2] = &quad.topRight;
        quad.corner[3] = &quad.topLeft;
        quad.texture = image->asset ? image->asset->object : g_zgui->defaultTexture;

        widgetMatrix(matrix, image->widget);
        zge::matrixRow(*matrix, 3) = image->widget.position;
        drawPrimitiveQuads(kTexturedQuad, record, 1, matrix);
        return 1;
    }
}
