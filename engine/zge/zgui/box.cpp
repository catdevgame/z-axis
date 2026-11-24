// zgui's box widget (widget.h): a nine-slice frame of up to nine textures
// (corners, edges, centre) drawn as textured quads over the widget's size,
// aligned on its position. A missing corner or edge borrows its partner's
// texture mirrored (negative texture coordinates); the edges and the
// centre stretch their texture over what the corners leave.

#include "zge/zgui/widget_parts.h"
#include "zge/zgui/widget_kinds.h"

#include "ee/abi.h"
#include "ee/dispatch.h"
#include "ee/readable.h"
#include "ee/plain.h"
#include "zge/math/matrix.h"
#include "zge/math/vec.h"
#include "zge/render/primitives.h"
#include "zge/render/texture.h"

#include <cstddef>
#include <cstring>
#include "sym/zge/zgui/box.cpp.h"

namespace zge::zgui
{
    namespace
    {
        constexpr uint32_t kTexturedQuad = 0x02000004u;   // its kind: a quad with texture and colours

        // The nine textures, in reading order.
        enum Piece : uint32_t
        {
            TopLeft, Top, TopRight, Left, Centre, Right, BottomLeft, Bottom, BottomRight,
        };

        // The 2D API's vertex and a quad's record (as drawQuadWidget's).
        struct Vertex
        {
            ee::Float x, y, z;
            uint32_t _0c;
            ee::Float u, v;
            uint8_t colour[4];
            uint32_t _1c;
        };
        static_assert(sizeof(Vertex) == 0x20);
        struct QuadRecord
        {
            uint32_t picture;   // +0x00
            ee::Ptr<Vertex> corner[4]; // +0x04 the vertices: bottom-left, bottom-right, top-right, top-left
            uint32_t _14[3];
            Vertex topLeft;     // +0x20
            Vertex topRight;    // +0x40
            Vertex bottomRight; // +0x60
            Vertex bottomLeft;  // +0x80
        };
        static_assert(offsetof(QuadRecord, topLeft) == 0x20 && offsetof(QuadRecord, bottomLeft) == 0x80);

        // The texture coordinates a piece's picture is drawn with, set when
        // it is picked; the ones a stretched piece sets itself are left.
        enum class Uvs
        {
            Corner, CornerMirrorX, CornerMirrorY, CornerMirrorXY,
            Across, AcrossMirrorY, // top and bottom: u of the right side stretched
            Down, DownMirrorX,     // left and right: v of the bottom stretched
            Centre,                // both stretched
        };
        struct Candidate
        {
            Piece piece;
            Uvs uvs;
        };

        const ee::Float kZero = 0.0f;
        const ee::Float kNegativeZero = -0.0f;

        void setUvs(QuadRecord &q, Uvs uvs, bool &mirrored)
        {
            const ee::Float one = 1.0f, minusOne = -1.0f;
            auto corner = [&](ee::Float u0, ee::Float u1, ee::Float v0, ee::Float v1) {
                q.topLeft.u = u0, q.topLeft.v = v0, q.topRight.u = u1, q.topRight.v = v0;
                q.bottomRight.u = u1, q.bottomRight.v = v1, q.bottomLeft.u = u0, q.bottomLeft.v = v1;
            };
            switch (uvs)
            {
            case Uvs::Corner: corner(kZero, one, kZero, one), mirrored = false; break;
            case Uvs::CornerMirrorX: corner(kNegativeZero, minusOne, kZero, one), mirrored = true; break;
            case Uvs::CornerMirrorY: corner(kZero, one, kNegativeZero, minusOne), mirrored = false; break;
            case Uvs::CornerMirrorXY: corner(kNegativeZero, minusOne, kNegativeZero, minusOne), mirrored = true; break;
            case Uvs::Across:
                q.topLeft.u = kZero, q.topLeft.v = kZero, q.topRight.v = kZero;
                q.bottomRight.v = one, q.bottomLeft.u = kZero, q.bottomLeft.v = one, mirrored = false;
                break;
            case Uvs::AcrossMirrorY:
                q.topLeft.u = kZero, q.topLeft.v = kNegativeZero, q.topRight.v = kNegativeZero;
                q.bottomRight.v = minusOne, q.bottomLeft.u = kZero, q.bottomLeft.v = minusOne, mirrored = false;
                break;
            case Uvs::Down:
                q.topLeft.u = kZero, q.topLeft.v = kZero, q.topRight.u = one, q.topRight.v = kZero;
                q.bottomRight.u = one, q.bottomLeft.u = kZero, mirrored = false;
                break;
            case Uvs::DownMirrorX:
                q.topLeft.u = kNegativeZero, q.topLeft.v = kZero, q.topRight.u = minusOne, q.topRight.v = kZero;
                q.bottomRight.u = minusOne, q.bottomLeft.u = kNegativeZero, mirrored = true;
                break;
            case Uvs::Centre:
                q.topLeft.u = kZero, q.topLeft.v = kZero, q.topRight.v = kZero, q.bottomLeft.u = kZero;
                break;
            }
        }

        // A level's size in pixels as the game converts it (an unsigned count).
        ee::Float pixels(uint32_t count)
        {
            ee::Float f;
            if (static_cast<int32_t>(count) >= 0)
                f = ee::toFloat(static_cast<int32_t>(count));
            else
            {
                const ee::Float half = ee::toFloat(static_cast<int32_t>((count >> 1) | (count & 1u)));
                f = half + half;
            }
            return f;
        }

        struct Size
        {
            ee::Float width, height;
        };

        // A piece's picture: the first candidate whose texture has one, with
        // its size and texture coordinates; none: zgui's default picture,
        // size 0. `mirrored` is left as it was then.
        Size pickPicture(const BoxWidget &box, QuadRecord &quad, std::initializer_list<Candidate> candidates, bool &mirrored)
        {
            for (const Candidate &c : candidates)
            {
                const ee::Ptr<UiAsset> texture = box.texture[c.piece];
                if (!texture)
                    continue;
                const uint32_t picture = texture->object;
                quad.picture = picture;
                if (!picture)
                    continue;
                Size size;
                size.width = pixels(levelWidth(ee::Ptr<Texture>(pictureLevel(ee::Ptr<Material>(picture), 0))));
                size.height = pixels(levelHeight(ee::Ptr<Texture>(pictureLevel(ee::Ptr<Material>(picture), 0))));
                setUvs(quad, c.uvs, mirrored);
                return size;
            }
            quad.picture = g_zgui->defaultTexture;
            return {kZero, kZero};
        }

        ee::Float whole(ee::Float f) { return ee::toFloat(ee::toInt(f)); }

        // The quad's corners (pixel centres: -0.5), at z 0.
        void place(QuadRecord &q, ee::Float left, ee::Float right, ee::Float top, ee::Float bottom)
        {
            q.topLeft.x = left, q.topLeft.y = top, q.topLeft.z = kZero;
            q.topRight.x = right, q.topRight.y = top, q.topRight.z = kZero;
            q.bottomRight.x = right, q.bottomRight.y = bottom, q.bottomRight.z = kZero;
            q.bottomLeft.x = left, q.bottomLeft.y = bottom, q.bottomLeft.z = kZero;
        }

        void setColour(uint8_t (&out)[4], const uint8_t (&in)[4], ee::Float alpha)
        {
            std::memcpy(out, in, 4);
            for (int c = 0; c < 3; ++c)
                out[c] = static_cast<uint8_t>(ee::toInt(ee::toFloat(out[c]) * alpha));
        }

        // A span over a picture's size, as a texture coordinate. ZGE4 (BMX
        // XXX) answers 0 for a picture of no size; ZGE3 divides.
        ee::Float spanRatio(ee::Float span, ee::Float size)
        {
            if constexpr (game::readable::kIsBmx)
                if (size == ee::Float(0.0f))
                    return ee::Float(0.0f);
            return span / size;
        }

        // A right-hand piece starting before the left-hand one ends starts
        // where it ends, its texture cut by as much (from the mirrored side
        // when mirrored). Returns the piece's width after the cut.
        ee::Float clipLeft(QuadRecord &q, ee::Float &x, ee::Float width, ee::Float leftEnd, bool mirrored)
        {
            if (!(x < leftEnd))
                return width;
            const ee::Float overlap = leftEnd - x;
            if (mirrored)
                q.topLeft.u = -overlap / width;
            else
                q.topLeft.u = overlap / width;
            q.bottomLeft.u = q.topLeft.u;
            x = leftEnd;
            return width - overlap;
        }
    }

    int32_t drawBoxWidget(ee::Ptr<BoxWidget> widget)
    {
        ee::Local<QuadRecord> record;
        ee::Local<zge::Matrix> matrix;
        BoxWidget &box = *widget;
        QuadRecord &quad = *record;
        const ee::Float half = -0.5f;

        // Placed by a matrix: scaled, at the widget's screen position less
        // the alignment. ZGE4 (BMX XXX) never sets a screen position's
        // fourth word (placeAtAnchor), and its box gives the matrix's 0.
        matrixIdentity(matrix);
        matrixScale(matrix, matrix, &box.scale.xyz());
        zge::Vec4 &at = zge::matrixRow(*matrix, 3);
        at = box.node.screenPosition;
        if constexpr (game::readable::kIsBmx)
            at.w = kZero;
        switch (box.alignment & 0xF)
        {
        case 0: break;
        case 1:
            at.x = at.x - box.width * ee::Float(0.5f);
            break;
        case 2:
            at.x = at.x - box.width;
            break;
        default: break;
        }
        switch (box.alignment & 0xF0)
        {
        case 0: break;
        case 0x10:
            at.y = at.y - box.height * ee::Float(0.5f);
            break;
        case 0x20:
            at.y = at.y - box.height;
            break;
        default: break;
        }
        // The nine pieces' textures by name, on first use.
        for (uint32_t i = 0; i < 9; ++i)
        {
            if (box.textureName[i] && !box.texture[i])
                box.texture[i] = findGuiAsset(box.textureName[i], 0);
        }

        quad.corner[0] = &quad.bottomLeft;
        quad.corner[1] = &quad.bottomRight;
        quad.corner[2] = &quad.topRight;
        quad.corner[3] = &quad.topLeft;
        const ee::Float alpha = g_zgui->alpha;
        setColour(quad.topLeft.colour, box.colour, alpha);
        setColour(quad.topRight.colour, box.colour, alpha);
        setColour(quad.bottomRight.colour, box.colour, alpha);
        setColour(quad.bottomLeft.colour, box.colour, alpha);

        const ee::Float W = box.width, H = box.height;
        // A piece drawn when `shown`, placed by `place`.
        bool mirrored = false;
        auto draw = [&](bool shown, auto place) {
            if (shown)
            {
                place();
                drawPrimitiveQuads(kTexturedQuad, record, 1, matrix);
            }
        };
        ee::Float topHeight = 0.0f;
        ee::Float bottomHeight = 0.0f;

        // The top row: the left corner, the right one (not over the left),
        // the edge stretched between them.
        const Size topLeft = pickPicture(box, quad, {{TopLeft, Uvs::Corner}, {TopRight, Uvs::CornerMirrorX},
                                                     {BottomLeft, Uvs::CornerMirrorY}, {BottomRight, Uvs::CornerMirrorXY}}, mirrored);
        draw(kZero < topLeft.width && kZero < topLeft.height,
             [&] { place(quad, half, whole(topLeft.width) + half, half, whole(topLeft.height) + half); });
        if (topHeight < topLeft.height)
            topHeight = topLeft.height;

        const Size topRight = pickPicture(box, quad, {{TopRight, Uvs::Corner}, {TopLeft, Uvs::CornerMirrorX},
                                                      {BottomRight, Uvs::CornerMirrorY}, {BottomLeft, Uvs::CornerMirrorXY}}, mirrored);
        {
            ee::Float x = W - topRight.width;
            const ee::Float span = clipLeft(quad, x, topRight.width, topLeft.width, mirrored);
            draw(kZero < span && kZero < topRight.height,
                 [&] { place(quad, whole(x) + half, whole(x) + whole(span) + half, half, whole(topRight.height) + half); });
        }
        if (topHeight < topRight.height)
            topHeight = topRight.height;

        const Size top = pickPicture(box, quad, {{Top, Uvs::Across}, {Bottom, Uvs::AcrossMirrorY}}, mirrored);
        {
            const ee::Float span = W - (topLeft.width + topRight.width);
            quad.topRight.u = spanRatio(span, top.width);
            quad.bottomRight.u = quad.topRight.u;
            draw(kZero < span && kZero < top.height,
                 [&] {
                     place(quad, whole(topLeft.width) + half, whole(topLeft.width) + whole(span) + half, half,
                           whole(top.height) + half);
                 });
        }
        if (topHeight < top.height)
            topHeight = top.height;

        // The bottom row: the right corner, the left one, the edge.
        const Size bottomRight = pickPicture(box, quad, {{BottomRight, Uvs::Corner}, {BottomLeft, Uvs::CornerMirrorX},
                                                         {TopRight, Uvs::CornerMirrorY}, {TopLeft, Uvs::CornerMirrorXY}}, mirrored);
        draw(kZero < bottomRight.width && kZero < bottomRight.height,
             [&] {
                 const ee::Float x = whole(W - bottomRight.width);
                 const ee::Float y = whole(H - bottomRight.height);
                 place(quad, x + half, x + whole(bottomRight.width) + half, y + half, y + whole(bottomRight.height) + half);
             });
        if (bottomHeight < bottomRight.height)
            bottomHeight = bottomRight.height;

        const Size bottomLeft = pickPicture(box, quad, {{BottomLeft, Uvs::Corner}, {BottomRight, Uvs::CornerMirrorX},
                                                        {TopLeft, Uvs::CornerMirrorY}, {TopRight, Uvs::CornerMirrorXY}}, mirrored);
        draw(kZero < bottomLeft.width && kZero < bottomLeft.height,
             [&] {
                 const ee::Float y = whole(H - bottomLeft.height);
                 place(quad, half, whole(bottomLeft.width) + half, y + half, y + whole(bottomLeft.height) + half);
             });
        if (bottomHeight < bottomLeft.height)
            bottomHeight = bottomLeft.height;

        const Size bottom = pickPicture(box, quad, {{Bottom, Uvs::Across}, {Top, Uvs::AcrossMirrorY}}, mirrored);
        {
            const ee::Float span = W - (bottomLeft.width + bottomRight.width);
            quad.topRight.u = spanRatio(span, bottom.width);
            quad.bottomRight.u = quad.topRight.u;
            draw(kZero < span && kZero < bottom.height,
                 [&] {
                     const ee::Float y = whole(H - bottom.height);
                     place(quad, whole(bottomLeft.width) + half, whole(bottomLeft.width) + whole(span) + half, y + half,
                           y + whole(bottom.height) + half);
                 });
        }
        if (bottomHeight < bottom.height)
            bottomHeight = bottom.height;

        // The middle row between them: the left edge, the right one (not
        // over the left), the centre.
        const ee::Float rows = topHeight + bottomHeight;
        const Size left = pickPicture(box, quad, {{Left, Uvs::Down}, {Right, Uvs::DownMirrorX}}, mirrored);
        {
            const ee::Float span = H - rows;
            quad.bottomRight.v = spanRatio(span, left.height);
            quad.bottomLeft.v = quad.bottomRight.v;
            draw(kZero < left.width && kZero < span,
                 [&] {
                     place(quad, half, whole(left.width) + half, whole(topHeight) + half,
                           whole(topHeight) + whole(span) + half);
                 });
        }

        const Size right = pickPicture(box, quad, {{Right, Uvs::Down}, {Left, Uvs::DownMirrorX}}, mirrored);
        {
            ee::Float x = W - right.width;
            const ee::Float span = H - rows;
            const ee::Float width = clipLeft(quad, x, right.width, left.width, mirrored);
            quad.bottomRight.v = spanRatio(span, right.height);
            quad.bottomLeft.v = quad.bottomRight.v;
            draw(kZero < width && kZero < span,
                 [&] {
                     place(quad, whole(x) + half, whole(x) + whole(width) + half, whole(topHeight) + half,
                           whole(topHeight) + whole(span) + half);
                 });
        }

        const Size centre = pickPicture(box, quad, {{Centre, Uvs::Centre}}, mirrored);
        {
            const ee::Float across = W - (right.width + left.width);
            const ee::Float down = (H - topHeight) - bottomHeight;
            quad.topRight.u = spanRatio(across, centre.width);
            quad.bottomRight.v = spanRatio(down, centre.height);
            draw(kZero < across && kZero < down,
                 [&] {
                     quad.bottomRight.u = quad.topRight.u;
                     quad.bottomLeft.v = quad.bottomRight.v;
                     place(quad, whole(left.width) + half, whole(left.width) + whole(across) + half, whole(topHeight) + half,
                           whole(topHeight) + whole(down) + half);
                 });
        }
        return 1;
    }
    READABLE_FUNCTION_OWN(drawBoxWidget, Verified, dm2, drawListedFrame);

    namespace
    {
        // A piece's picture for ZGE2's frame: as pickPicture, over its record.
        Size pickFramePicture(const ListedFrame &frame, QuadRecord &quad, std::initializer_list<Candidate> candidates)
        {
            bool mirrored = false; // (ZGE2 clips no piece: it has no use for it)
            for (const Candidate &c : candidates)
            {
                const ee::Ptr<UiAsset> texture = frame.assets[c.piece];
                if (!texture)
                    continue;
                const uint32_t picture = texture->object;
                if (!picture)
                    continue;
                quad.picture = picture;
                Size size;
                size.width = pixels(levelWidth(ee::Ptr<Texture>(pictureLevel(ee::Ptr<Material>(picture), 0))));
                size.height = pixels(levelHeight(ee::Ptr<Texture>(pictureLevel(ee::Ptr<Material>(picture), 0))));
                setUvs(quad, c.uvs, mirrored);
                return size;
            }
            quad.picture = g_zgui->defaultTexture;
            return {kZero, kZero};
        }
    }

    // drawListedFrame: ZGE2's drawBoxWidget, the frame's Draw: the same nine
    // pieces in the same order from the ZGE2 record, turned by the widget's
    // orientation as well as scaled. ZGE2's is the plainer one: every piece
    // is drawn whatever its size, the right-hand pieces are not cut where
    // they would overlap the left-hand ones, the left edge hangs below the
    // top left corner's width (not its height), the right edge below the top
    // right corner's height, and the centre starts below the lower of the
    // two top corners and ends above the lower of the two bottom ones. 1.
    int32_t drawListedFrame(ee::Ptr<zge::EngineMessage> message)
    {
        ee::Local<QuadRecord> record;
        ee::Local<zge::Matrix> matrix;
        ListedFrame &frame = *messageRecord<ListedFrame>(message);
        QuadRecord &quad = *record;
        const ee::Float half = -0.5f;

        // Placed by a matrix: turned and scaled, at the widget's position
        // less the alignment. Unlike the other widgets' (widgetMatrix), the
        // matrix is not made the identity first: the orientation's matrix is
        // written over what the frame held.
        zge::quaternionToMatrix(matrix, &frame.widget.orientation);
        zge::matrixScale(matrix, matrix, &frame.widget.scale.xyz());
        zge::Vec4 &at = zge::matrixRow(*matrix, 3);
        at = frame.widget.position;
        switch (frame.alignment & 0xF)
        {
        case 1: at.x = at.x - frame.width * ee::Float(0.5f); break;
        case 2: at.x = at.x - frame.width; break;
        default: break;
        }
        switch (frame.alignment & 0xF0)
        {
        case 0x10: at.y = at.y - frame.height * ee::Float(0.5f); break;
        case 0x20: at.y = at.y - frame.height; break;
        default: break;
        }
        // The nine pieces' textures by name, on first use.
        for (uint32_t i = 0; i < 9; ++i)
        {
            if (frame.materials[i] && !frame.assets[i])
                frame.assets[i] = findGuiAsset(frame.materials[i], 0);
        }

        quad.corner[0] = &quad.bottomLeft;
        quad.corner[1] = &quad.bottomRight;
        quad.corner[2] = &quad.topRight;
        quad.corner[3] = &quad.topLeft;
        const ee::Float alpha = g_zgui->alpha;
        const uint8_t colour[4] = {frame.color.r, frame.color.g, frame.color.b, frame.color.a};
        setColour(quad.topLeft.colour, colour, alpha);
        setColour(quad.topRight.colour, colour, alpha);
        setColour(quad.bottomRight.colour, colour, alpha);
        setColour(quad.bottomLeft.colour, colour, alpha);

        const auto draw = [&] { drawPrimitiveQuads(kTexturedQuad, record, 1, matrix); };

        // The top row: the left corner, the right one, the edge stretched
        // between them.
        const Size topLeft = pickFramePicture(frame, quad, {{TopLeft, Uvs::Corner}, {TopRight, Uvs::CornerMirrorX},
                                                            {BottomLeft, Uvs::CornerMirrorY}, {BottomRight, Uvs::CornerMirrorXY}});
        place(quad, half, whole(topLeft.width) + half, half, whole(topLeft.height) + half);
        draw();

        const Size topRight = pickFramePicture(frame, quad, {{TopRight, Uvs::Corner}, {TopLeft, Uvs::CornerMirrorX},
                                                             {BottomRight, Uvs::CornerMirrorY}, {BottomLeft, Uvs::CornerMirrorXY}});
        {
            const ee::Float x = whole(frame.width - topRight.width);
            place(quad, x + half, x + whole(topRight.width) + half, half, whole(topRight.height) + half);
            draw();
        }

        const Size top = pickFramePicture(frame, quad, {{Top, Uvs::Across}, {Bottom, Uvs::AcrossMirrorY}});
        {
            const ee::Float span = frame.width - (topLeft.width + topRight.width);
            quad.topRight.u = spanRatio(span, top.width);
            quad.bottomRight.u = quad.topRight.u;
            place(quad, whole(topLeft.width) + half, whole(topLeft.width) + whole(span) + half, half, whole(top.height) + half);
            draw();
        }

        // The bottom row: the right corner, the left one, the edge.
        const Size bottomRight = pickFramePicture(frame, quad, {{BottomRight, Uvs::Corner}, {BottomLeft, Uvs::CornerMirrorX},
                                                                {TopRight, Uvs::CornerMirrorY}, {TopLeft, Uvs::CornerMirrorXY}});
        {
            const ee::Float x = whole(frame.width - bottomRight.width);
            const ee::Float y = whole(frame.height - bottomRight.height);
            place(quad, x + half, x + whole(bottomRight.width) + half, y + half, y + whole(bottomRight.height) + half);
            draw();
        }

        const Size bottomLeft = pickFramePicture(frame, quad, {{BottomLeft, Uvs::Corner}, {BottomRight, Uvs::CornerMirrorX},
                                                               {TopLeft, Uvs::CornerMirrorY}, {TopRight, Uvs::CornerMirrorXY}});
        {
            const ee::Float y = whole(frame.height - bottomLeft.height);
            place(quad, half, whole(bottomLeft.width) + half, y + half, y + whole(bottomLeft.height) + half);
            draw();
        }

        const Size bottom = pickFramePicture(frame, quad, {{Bottom, Uvs::Across}, {Top, Uvs::AcrossMirrorY}});
        {
            const ee::Float span = frame.width - (bottomLeft.width + bottomRight.width);
            quad.bottomRight.u = spanRatio(span, bottom.width);
            quad.topRight.u = quad.bottomRight.u;
            const ee::Float y = whole(frame.height - bottom.height);
            place(quad, whole(bottomLeft.width) + half, whole(bottomLeft.width) + whole(span) + half, y + half,
                  y + whole(bottom.height) + half);
            draw();
        }

        // The middle row: the left edge, the right one, the centre.
        const Size left = pickFramePicture(frame, quad, {{Left, Uvs::Down}, {Right, Uvs::DownMirrorX}});
        {
            const ee::Float span = frame.height - (topLeft.height + bottomLeft.height);
            quad.bottomLeft.v = spanRatio(span, left.height);
            quad.bottomRight.v = quad.bottomLeft.v;
            const ee::Float y = whole(topLeft.width); // (the corner's width: the original's)
            place(quad, half, whole(left.width) + half, y + half, y + whole(span) + half);
            draw();
        }

        const Size right = pickFramePicture(frame, quad, {{Right, Uvs::Down}, {Left, Uvs::DownMirrorX}});
        {
            const ee::Float span = frame.height - (topRight.height + bottomRight.height);
            quad.bottomLeft.v = spanRatio(span, right.height);
            quad.bottomRight.v = quad.bottomLeft.v;
            const ee::Float x = whole(frame.width - right.width);
            const ee::Float y = whole(topRight.height);
            place(quad, x + half, x + whole(right.width) + half, y + half, y + whole(span) + half);
            draw();
        }

        const Size centre = pickFramePicture(frame, quad, {{Centre, Uvs::Corner}});
        {
            const ee::Float topHeight = ee::min(topLeft.height, topRight.height);
            const ee::Float across = frame.width - (right.width + left.width);
            const ee::Float down = frame.height - topHeight - ee::min(bottomLeft.height, bottomRight.height);
            quad.bottomRight.u = spanRatio(across, centre.width);
            quad.bottomLeft.v = spanRatio(down, centre.height);
            quad.topRight.u = quad.bottomRight.u;
            quad.bottomRight.v = quad.bottomLeft.v;
            place(quad, whole(left.width) + half, whole(left.width) + whole(across) + half, whole(topHeight) + half,
                  whole(topHeight) + whole(down) + half);
            draw();
        }
        return 1;
    }
}
