// Small engine routines ported in the readable process round (drafted with
// tools/readable/readable_draft.py, then named and typed; READABLE.md,
// "Drafts and the recording corpus"): the engine's doubly linked lists, a
// matrix scaled by a vector, a box's point test, the box sweep's end point
// moving up its axis, and the font's glyph lookup. They belong to their
// modules (scene, math, collision, text); here until those are next worked
// on. Names past the offsets are inferred from what the code does.

#include "ee/abi.h"
#include "ee/readable.h"
#include "ee/readable_waits.h"
#include "ee/plain.h"
#include "zge/collision/sweep.h"
#include "zge/container/list.h"
#include "zge/math/angle.h"
#include "zge/math/box.h"
#include "zge/math/matrix.h"
#include "zge/math/types.h"
#include "zge/math/vu0_ops.h"
#include "zge/platform/platform.h"
#include "zge/scene/scene.h"
#include "zge/text/cstring.h"
#include "zge/text/font.h"
#include "zge/text/strings.h"

#include <cstddef>
#include <cstdint>
#include "sym/zge/utilities.cpp.h"

namespace zge
{
    // ---- lists ----------------------------------------------------------------

    // The lists' functions take any list (a record that starts with a List's
    // head, tail and count: pools', handler chains', a module's own typed
    // views) and any node (a record that starts with a ListNode's links).

    // listPushFront: node put at the front of the list.
    void listPushFront(ee::Address list, ee::Address node)
    {
        const ee::Ptr<List> to(list);
        const ee::Ptr<ListNode> added(node);
        const ee::Ptr<ListNode> first = to->head;
        added->prev = ee::Ptr<ListNode>();
        added->next = first;
        to->head = added;
        if (!to->tail)
            to->tail = added;
        if (const ee::Ptr<ListNode> next = added->next)
            next->prev = added;
        ++to->count;
    }
    READABLE_FUNCTION(listPushFront, Verified);

    // listRemove: node taken out of the list (its own links are left as
    // they were).
    void listRemove(ee::Address list, ee::Address node)
    {
        const ee::Ptr<List> from(list);
        const ee::Ptr<ListNode> removed(node);
        if (const ee::Ptr<ListNode> prev = removed->prev)
            prev->next = removed->next;
        else
            from->head = removed->next;
        if (const ee::Ptr<ListNode> next = removed->next)
            next->prev = removed->prev;
        else
            from->tail = removed->prev;
        --from->count;
    }
    READABLE_FUNCTION(listRemove, Verified);

    // initList: the list made empty.
    void initList(ee::Address list)
    {
        const ee::Ptr<List> emptied(list);
        emptied->count = 0;
        emptied->tail = ee::Ptr<ListNode>();
        emptied->head = ee::Ptr<ListNode>();
    }
    READABLE_FUNCTION(initList, Verified);

    // listPush: node put at the end of the list.
    void listPush(ee::Address list, ee::Address node)
    {
        const ee::Ptr<List> to(list);
        const ee::Ptr<ListNode> added(node);
        added->next = ee::Ptr<ListNode>();
        added->prev = to->tail;
        to->tail = added;
        if (!to->head)
            to->head = added;
        if (const ee::Ptr<ListNode> prev = added->prev)
            prev->next = added;
        ++to->count;
    }
    READABLE_FUNCTION(listPush, Verified);

    // listInsertAfter (nothing calls it in AI): node put after `after`, or at
    // the front with no `after`. Kept bug: the front case counts the node
    // twice (listPushFront counts it, then this does again).
    void listInsertAfter(ee::Address list, ee::Address node, ee::Address after)
    {
        const ee::Ptr<List> to(list);
        const ee::Ptr<ListNode> added(node);
        if (const ee::Ptr<ListNode> before{after})
        {
            added->prev = before;
            added->next = before->next;
            if (const ee::Ptr<ListNode> next = before->next)
                next->prev = added;
            else
                to->tail = added;
            before->next = added;
        }
        else
        {
            listPushFront(list, node);
        }
        ++to->count;
    }
    READABLE_FUNCTION(listInsertAfter, Draft);

    // listContains: 1 if node is on the list, else 0.
    uint32_t listContains(ee::Address list, ee::Address node)
    {
        const ee::Ptr<ListNode> wanted(node);
        for (ee::Ptr<ListNode> at = ee::Ptr<List>(list)->head; at; at = at->next)
            if (at == wanted)
                return 1;
        return 0;
    }
    READABLE_FUNCTION(listContains, Verified);

    // ---- math -----------------------------------------------------------------

    // matrixScale: out = m with each rotation row's x, y and z scaled by
    // scale's x, y and z (m times a diagonal scale, applied first); the rows'
    // w are left as out had them, the translation row copied (when out is not
    // m). Returns out.
    Matrix *matrixScale(Matrix *out, const Matrix *m, const Vec3 *scale)
    {
        // The original's order: a column at a time.
        for (int column = 0; column < 3; ++column)
        {
            const ee::Float s = column == 0 ? scale->x : column == 1 ? scale->y : scale->z;
            for (int row = 0; row < 3; ++row)
                out->m[row][column] = m->m[row][column] * s;
        }
        if (m != out)
            for (int i = 0; i < 4; ++i)
                out->m[3][i] = m->m[3][i];
        return out;
    }

    // scaleMatrixDiagonal: ZGE2's matrixScale: out = m (copied whole when out
    // is not m), then the diagonal's three elements scaled by scale's x, y
    // and z; the rest of the rotation is copied, not scaled. Returns out.
    Matrix *scaleMatrixDiagonal(Matrix *out, const Matrix *m, const Vec3 *scale)
    {
        if (m != out)
            copyRotationAndTranslation(ee::addressOf(out), ee::addressOf(m));
        out->m[0][0] = m->m[0][0] * scale->x;
        out->m[1][1] = m->m[1][1] * scale->y;
        out->m[2][2] = m->m[2][2] * scale->z;
        return out;
    }
    // scaleRotationRows: ZGE4's matrixScale, on VU0: out = m with its first
    // three rows' x, y and z scaled, row 0 by scale's x, row 1 by its y,
    // row 2 by its z (ZGE3 scales the columns); the rows' w and the
    // translation row copied. VU0 keeps the original's registers: vf5 the
    // scale, vf1 to vf4 out's rows. Returns out.
    Matrix *scaleRotationRows(Matrix *out, const Matrix *m, const Vec3 *scale)
    {
        vu0::Registers vu0;
        vu0.load(5, ee::addressOf(scale));
        for (int row = 0; row < 4; ++row)
            vu0.load(1 + row, ee::addressOf(m) + row * static_cast<uint32_t>(sizeof(m->m[0])));
        vu0.mulBc(vu0::kXyz, 1, 1, 5, vu0::kLaneX);
        vu0.mulBc(vu0::kXyz, 2, 2, 5, vu0::kLaneY);
        vu0.mulBc(vu0::kXyz, 3, 3, 5, vu0::kLaneZ);
        for (int row = 0; row < 4; ++row)
            vu0.store(1 + row, ee::addressOf(out) + row * static_cast<uint32_t>(sizeof(out->m[0])));
        return out;
    }
    READABLE_FUNCTION_OWN(matrixScale, Verified, dm2, scaleMatrixDiagonal, bmx, scaleRotationRows);

    // ---- collision ------------------------------------------------------------

    // boxContainsPoint: whether point lies in box (its faces included).
    bool boxContainsPoint(const Box *box, const Vec3 *point)
    {
        const bool inside = box->low.x <= point->x && point->x <= box->high.x && box->low.y <= point->y &&
                            point->y <= box->high.y && box->low.z <= point->z && point->z <= box->high.z;
        return inside;
    }
    READABLE_FUNCTION(boxContainsPoint, Verified);

    // The box sweep (zge/collision/sweep.h): an end point's moves along its axis.

    namespace
    {
        constexpr uint32_t kSweepSwapNext = sym::sweepSwapNext; // (axis list, end, next): end and the one after it swapped
    }

    void sweepSwapNext(ee::Ptr<SweepEnd> axisList, ee::Ptr<SweepEnd> end, ee::Ptr<SweepEnd> next);

    // sweepMoveUp: an end point whose value grew moved up its axis: past
    // each smaller one after it (a low end passed is told to the sweep).
    void sweepMoveUp(ee::Ptr<Sweep> sweep, ee::Ptr<SweepEnd> axisList, ee::Ptr<SweepEnd> end, uint32_t axis)
    {
        for (ee::Ptr<SweepEnd> next = end->next;; next = end->next)
        {
            if (!next)
                break;
            if (!(next->value < end->value))
                break;
            if (!next->isHigh)
                sweepPassLow(sweep, end, next, axis);
            sweepSwapNext(axisList, end, end->next);
        }
    }
    READABLE_FUNCTION(sweepMoveUp, Verified);

    // ---- text -----------------------------------------------------------------

    // (Font: zge/text/font.h.)

    namespace // the game functions below call: ee/plain.h
    {
        void memset(ee::Ptr<uint8_t> m, uint32_t c, uint32_t n) { ee::game(kMemset, m, c, n); }
    }

    // fontGlyph: the font's glyph for a character code; a letter the font
    // lacks is looked up in upper case, then in lower case. Null if none.
    ee::Ptr<Glyph> fontGlyph(ee::Ptr<Font> font, uint8_t code)
    {
        if (const ee::Ptr<Glyph> glyph = font->glyphs[code])
            return glyph;
        // The one-letter text the case changes work on.
        ee::Local<uint8_t> letter;
        memset(letter, 0, 2);
        *letter = code;
        stringToUpper(letter);
        if (const ee::Ptr<Glyph> glyph = font->glyphs[*letter])
            return glyph;
        stringToLower(letter);
        return font->glyphs[*letter];
    }
    READABLE_FUNCTION(fontGlyph, Verified);
}

namespace zge
{
    // ---- the box sweep, continued -----------------------------------------------

    namespace
    {
        // The other three moves of an end point along its axis list (inferred
        // from their order around sweepMoveUp; each (sweep, axis list, end,
        // pass) where pass is 2 * axis + 0 or 1).
        constexpr uint32_t kSweepLowUp = sym::sweepLowUp;
        constexpr uint32_t kSweepLowDown = sym::sweepLowDown;
    }

    namespace
    {
        // Low ends moved down (passes 0, 2, 4) or up (1, 3, 5) may leave the
        // axis's first end point elsewhere: the sweep's head for the axis
        // walked back to the list's start.
        void rewindHead(ee::Ptr<SweepEnd> &head)
        {
            ee::Ptr<SweepEnd> first = head;
            ee::Ptr<SweepEnd> before = first->prev;
            if (before)
            {
                for (;;)
                {
                    first = before;
                    before = first->prev;
                    if (!before)
                        break;
                }
            }
            head = first;
        }

    }

    // sweepSwapNext (kSweepSwapNext): an end point and the one after it on
    // the axis swapped (the axis list itself is not used).
    void sweepSwapNext(ee::Ptr<SweepEnd>, ee::Ptr<SweepEnd> end, ee::Ptr<SweepEnd> next)
    {
        if (end->prev)
            end->prev->next = next;
        if (next->next)
            next->next->prev = end;
        next->prev = end->prev;
        end->prev = next;
        end->next = next->next;
        next->next = end;
    }
    READABLE_FUNCTION(sweepSwapNext, Verified);

    // sweepLowUp (kSweepLowUp): a box's low end whose value grew moved up
    // its axis past each smaller end point after it, up to its own box's
    // high end (a high end passed is told to the sweep: the boxes part);
    // then the axis's head found again.
    void sweepLowUp(ee::Ptr<Sweep> sweep, ee::Ptr<SweepEnd> axisList, ee::Ptr<SweepEnd> end, uint32_t pass)
    {
        for (ee::Ptr<SweepEnd> next = end->next;;)
        {
            if (!next || next->box == end->box || !(next->value < end->value))
                break;
            if (next->isHigh == 1)
                sweepPassLow(sweep, end, next, pass);
            sweepSwapNext(axisList, end, end->next);
            next = end->next;
        }
        if (pass == 3)
            rewindHead(sweep->axes[1]);
        else if (static_cast<int32_t>(pass) < 4)
        {
            if (pass != 1)
                return;
            rewindHead(sweep->axes[0]);
        }
        else
        {
            if (pass != 5)
                return;
            rewindHead(sweep->axes[2]);
        }
    }
    READABLE_FUNCTION_WAITS(sweepLowUp, Verified);

    // sweepLowDown (kSweepLowDown): a box's low end whose value shrank moved
    // down its axis past each larger end point before it (a high end passed
    // is told to the sweep: the boxes meet); then the axis's head found
    // again.
    void sweepLowDown(ee::Ptr<Sweep> sweep, ee::Ptr<SweepEnd> axisList, ee::Ptr<SweepEnd> end, uint32_t pass)
    {
        for (ee::Ptr<SweepEnd> before = end->prev;;)
        {
            if (!before || !(end->value < before->value))
                break;
            if (before->isHigh == 1)
                sweepPassLow(sweep, end, before, pass);
            sweepSwapNext(axisList, end->prev, end);
            before = end->prev;
        }
        if (pass == 2)
            rewindHead(sweep->axes[1]);
        else if (static_cast<int32_t>(pass) < 3)
        {
            if (pass != 0)
                return;
            rewindHead(sweep->axes[0]);
        }
        else
        {
            if (pass != 4)
                return;
            rewindHead(sweep->axes[2]);
        }
    }
    READABLE_FUNCTION_WAITS(sweepLowDown, Verified);

    // sweepHighDown (kSweepHighDown): a box's high end whose value shrank
    // moved down its axis past each larger end point before it, down to its
    // own box's low end (a low end passed is told to the sweep: the boxes
    // part).
    void sweepHighDown(ee::Ptr<Sweep> sweep, ee::Ptr<SweepEnd> axisList, ee::Ptr<SweepEnd> end, uint32_t pass)
    {
        for (ee::Ptr<SweepEnd> before = end->prev;;)
        {
            if (!before)
                break;
            if (before->box == end->box || !(end->value < before->value))
                break;
            if (before->isHigh == 0)
                sweepPassLow(sweep, end, before, pass);
            sweepSwapNext(axisList, end->prev, end);
            before = end->prev;
        }
    }
    READABLE_FUNCTION_WAITS(sweepHighDown, Verified);

    // sweepUpdateBox: a box whose bounds changed moved into place on each axis:
    // its high end up, its low end up and down, its high end down (the last
    // the original's tail call). box: its three low end points (each low
    // end's +0x14 is its high end).
    void sweepUpdateBox(ee::Ptr<Sweep> sweep, ee::Ptr<SweepBox> box)
    {
        // The lists and end points are read again for every call, as the
        // original's unrolled code does.
        for (int axis = 0; axis < 3; ++axis)
        {
            const int32_t pass = 2 * axis;
            sweepMoveUp(sweep, sweep->axes[axis], box->lowEnds[axis]->other, static_cast<uint32_t>(pass));
            sweepLowUp(sweep, sweep->axes[axis], box->lowEnds[axis], pass + 1);
            sweepLowDown(sweep, sweep->axes[axis], box->lowEnds[axis], pass);
            if (axis < 2)
                sweepHighDown(sweep, sweep->axes[axis], box->lowEnds[axis]->other, pass + 1);
            else
                game::tail(sweepHighDown, sweep, sweep->axes[axis], box->lowEnds[axis]->other, pass + 1);
        }
    }
    READABLE_FUNCTION(sweepUpdateBox, Verified);

    // ---- an object's matrices ---------------------------------------------------

    namespace
    {
        constexpr uint32_t kCopyMatrix = sym::matrixTranspose3; // (out, in) (inferred: a 4x4 copy)
        constexpr uint32_t kRotateY = sym::matrixRotateY;    // (out, in, angle) by a 16-bit angle (engine)
    }

    // The four matrices an object keeps (0x40 bytes each from +0x70), made
    // when first asked for: a bit each in +0x170.
    struct MatrixCache
    {
        uint8_t _00[0x70];
        Matrix matrices[4]; // +0x70; 2: matrix 0 without its translation, 3: a turn about y facing matrix 0's z axis
        uint32_t valid;     // +0x170
    };
    static_assert(offsetof(MatrixCache, matrices) == 0x70 && offsetof(MatrixCache, valid) == 0x170);

    // cachedMatrix: the object's matrix `which` (0 .. 3), made first if it is
    // not yet; null (0) for another `which`.
    ee::Ptr<Matrix> cachedMatrix(ee::Address objectAddress, uint32_t which)
    {
        const ee::Ptr<MatrixCache> object(objectAddress); // a scene's or a camera's matrices (from +0x70)
        const ee::Ptr<Matrix> matrices = &object->matrices[0];
        if (!(static_cast<int32_t>(object->valid) >> (which & 31u) & 1))
        {
            if (which == 2)
            {
                cachedMatrix(object, 1); // matrix 1 made first (its result unused)
                const ee::Ptr<Matrix> out = matrices + 2;
                matrixTranspose3(out, matrices); // from matrix 0, not the 1 just made
                matrixClearTranslation(out);
            }
            else if (which == 3)
            {
                cachedMatrix(object, 1); // matrix 1 made first (its result unused)
                const ee::Ptr<Matrix> out = matrices + 3;
                const Matrix &m = object->matrices[0]; // matrix 0's z axis, not 1's
                const int32_t heading = static_cast<int32_t>(static_cast<short>(angleAtan2(m.m[2][0], m.m[2][2])));
                matrixIdentity(out);
                const int32_t turn = static_cast<int32_t>(static_cast<int16_t>(-heading));
                matrixRotateY(out, out, turn);
            }
            else if (which > 3)
                return ee::Ptr<Matrix>();
            object->valid = object->valid | 1u << (which & 31u);
        }
        return matrices + which;
    }

    // ZGE2 keeps an object's two matrices made (from +0x60), and no others.
    struct KeptMatrices
    {
        uint8_t _00[0x60];
        Matrix matrices[2]; // +0x60
    };
    static_assert(offsetof(KeptMatrices, matrices) == 0x60);

    // keptMatrix: ZGE2's cachedMatrix: the object's matrix 0 or 1; null (0)
    // for another `which`.
    ee::Ptr<Matrix> keptMatrix(ee::Address objectAddress, uint32_t which)
    {
        const ee::Ptr<KeptMatrices> object(objectAddress);
        if (which == 0)
            return ee::Ptr<Matrix>(&object->matrices[0]);
        if (which == 1)
            return ee::Ptr<Matrix>(&object->matrices[1]);
        return ee::Ptr<Matrix>();
    }
    READABLE_FUNCTION_OWN(cachedMatrix, Verified, dm2, keptMatrix);
}
