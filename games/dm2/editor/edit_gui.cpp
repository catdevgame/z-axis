// DM2's park editor GUI (edit_gui.h): ./src/com/editor/editGui.c, DM2's own
// code.

#include "edit_gui.h"
#include "gap_tool.h"
#include "edit_core.h"
#include "edit_objects.h"
#include "edit_display.h"
#include "edit_grid.h"
#include "edit_lights.h"

#include "ee/abi.h"
#include "ee/plain.h"
#include "ee/readable.h"
#include "zge/math/vec.h"
#include "zge/platform/tasks.h"
#include "zge/render/primitives.h"
#include "zge/zgui/script.h"
#include "zge/zgui/widget.h"

#include <cstdint>
#include <cstring>
#include "sym/dm2/editor/edit_gui.cpp.h"

namespace game::dm2
{
    namespace
    {
        // zgui's root node (Zgui +0x0C, as AI's g_uiRoot).
        inline ee::Global<ee::Ptr<zge::zgui::Node>, sym::g_uiRoot> g_uiRoot;
        inline ee::Global<EditGuiWidgets, sym::g_editGuiWidgets> g_editGuiWidgets;
        // The GUI's draw task, 0 while there is none.
        inline ee::Global<ee::Ptr<zge::Task>, sym::g_editGuiTask> g_editGuiTask;
        // The park's capacity: what the placed pieces use, the limit, and the
        // estimate the bar shows beside what is used.
        inline ee::Global<ee::Float, sym::g_parkCapacityUsed> g_parkCapacityUsed;
        inline ee::Global<ee::Float, sym::g_parkCapacity> g_parkCapacity;
        inline ee::Global<ee::Float, sym::g_parkCapacityEstimate> g_parkCapacityEstimate;

        constexpr ee::Ptr<const char> kTextText{sym::textText};             // "Text"
        constexpr ee::Ptr<const char> kTextWidth{sym::textWidth};           // "Width"
        constexpr ee::Ptr<const char> kTextPosition{sym::textPosition};     // "Position"
        constexpr ee::Ptr<const char> kTextTextFormat{sym::textTextFormat}; // "TextFormat"
        constexpr ee::Ptr<const char> kTapeFormat{sym::textTapeFormat};     // "%2.0fM"

        // DM2's findNode takes no kind: the shared declaration's is passed as 0.
        ee::Ptr<zge::zgui::Node> editWidget(uint32_t path)
        {
            return zge::zgui::findNode(*g_uiRoot, ee::Ptr<const char>(path), 0);
        }

        constexpr ee::Ptr<const char> kTextPlace{sym::textPlace};               // "Place"
        constexpr ee::Ptr<const char> kTextRotate{sym::textRotate};             // "Rotate"
        constexpr ee::Ptr<const char> kTextDelete{sym::textDelete};             // "Delete"
        constexpr ee::Ptr<const char> kTextCopy{sym::textCopy};                 // "Copy"
        constexpr ee::Ptr<const char> kTextPaste{sym::textPaste};               // "Paste"
        constexpr ee::Ptr<const char> kTextCancel{sym::textCancel};             // "Cancel"
        constexpr ee::Ptr<const char> kTextSelect{sym::textSelect};             // "Select"
        constexpr ee::Ptr<const char> kTextDrop{sym::textDrop};                 // "Drop"
        constexpr ee::Ptr<const char> kTextBackup{sym::textBackup};             // "Backup"
        constexpr ee::Ptr<const char> kTextNotAvailable{sym::textNotAvailable}; // "N/A"
        constexpr ee::Ptr<const char> kTextTerrainTool{sym::textTerrainTool};   // "Terrain Tool"
        constexpr ee::Ptr<const char> kTextRailTool{sym::textRailTool};         // "Rail Tool"
        constexpr ee::Ptr<const char> kTextStartTool{sym::textStartTool};       // "Start Tool"
        constexpr ee::Ptr<const char> kTextGapTool{sym::textGapTool};           // "Gap Tool"
        constexpr ee::Ptr<const char> kTextCopyTool{sym::textCopyTool};         // "Copy Tool"
        constexpr ee::Ptr<const char> kTextNoHelp{sym::textNoHelp};             // ""

        // The cursor's colours (red, green, blue, alpha).
        inline ee::Global<zge::VertexColour, sym::cursorBlue> g_cursorBlue;         // 50, 50, 250
        inline ee::Global<zge::VertexColour, sym::cursorDarkBlue> g_cursorDarkBlue; // 50, 64, 100
        inline ee::Global<zge::VertexColour, sym::cursorYellow> g_cursorYellow;     // 255, 255, 0
        inline ee::Global<zge::VertexColour, sym::cursorRed> g_cursorRed;           // 200, 32, 32
        // The pulse: the cosine swings a colour part by 80 around its middle.
        constexpr int32_t kCursorPulseRate = 0x400;
        constexpr ee::Float kCursorPulseSwing = 80.0f;
        constexpr ee::Float kCursorBlueMiddle = 100.0f;
        constexpr ee::Float kCursorYellowMiddle = 148.0f;

        // Scaffolding (address-call): the editor core's and the gap tool's
        // functions (zgeif/zoverlay.c and the code after this block) have no
        // readable bodies yet.
        inline ee::Global<ee::Float, sym::heldBoxHeightOffset> g_heldBoxHeightOffset;
        // Scaffolding (frame-exact): the corner addresses drawHeldObjectBox's
        // compiler kept in its frame; the lines' ends are read back from them.
        struct KeptCorners
        {
            ee::Ptr<zge::PrimitiveVertex> low2, high2, low3, high3, high0, low1;
        };
        // A quad as drawPrimitiveQuads takes it.
        struct PrimitiveQuad
        {
            uint32_t texture;
            ee::Ptr<zge::PrimitiveVertex> vertices[4];
        };
        static_assert(sizeof(PrimitiveQuad) == 0x14);

        inline ee::Global<uint32_t, sym::g_uiDefaultMaterial> g_uiDefaultMaterial;
        // The cursor is drawn a little off the ground.
        inline ee::Global<ee::Float, sym::cursorCellsHeightOffset> g_cursorCellsHeightOffset;
        inline ee::Global<ee::Float, sym::cursorCrossHeightOffset> g_cursorCrossHeightOffset;
        // The cells' alpha swings by 64 around 128.
        constexpr ee::Float kCellAlphaSwing = 64.0f;
        constexpr ee::Float kCellAlphaMiddle = 128.0f;
        // The cross: its lines reach two cells each way, then fade over one.
        constexpr ee::Float kCrossArm = 10.0f;
        constexpr zge::VertexColour kCrossColour{0xC8, 0xC8, 0xF0, 0x80};

        void setSelectionTitle(ee::Ptr<const char> title)
        {
            zge::zgui::setVariable(editWidget(sym::textSelectionPath), kTextText, title);
        }
        void clearHelp()
        {
            zge::zgui::setVariable(editWidget(sym::textHelpPath), kTextText, kTextNoHelp);
        }

        // The editor draws after the scene, in the tasks' stage 1.
        constexpr uint32_t kEditGuiStage = 1;
        constexpr uint32_t kEditGuiTaskLine = 57; // editGui.c's line, as the task records it

        // The capacity bar's full width on screen.
        constexpr ee::Float kCapacityBarWidth = 576.0f;

    }

    int32_t startEditGui(ee::Ptr<ParkEditor> editor)
    {
        *g_editGuiTask = zge::newTask(kEditGuiStage, sym::drawEditGui, editor, 1, sym::editGuiSourcePath, kEditGuiTaskLine);
        const ee::Ptr<zge::zgui::Node> tape = editWidget(sym::textTapePath);
        return game::tail(zge::zgui::setVariable, tape, kTextTextFormat, kTapeFormat);
    }
    READABLE_FUNCTION(startEditGui, Verified);

    void stopEditGui()
    {
        if (*g_editGuiTask)
        {
            zge::deleteTask(*g_editGuiTask);
            *g_editGuiTask = ee::Ptr<zge::Task>();
        }
    }
    READABLE_FUNCTION(stopEditGui, Verified);

    int32_t setInfoText(uint32_t slot, ee::Ptr<const char> text)
    {
        if (slot >= kEditInfoTextCount)
            return 0;
        return game::tail(zge::zgui::setVariable, g_editGuiWidgets->info[slot], kTextText, text);
    }
    READABLE_FUNCTION(setInfoText, Verified);

    void findEditGuiWidgets()
    {
        g_editGuiWidgets->capacityBar = editWidget(sym::textCapacityBarPath);
        g_editGuiWidgets->capacityEstimate = editWidget(sym::textCapacityEstimatePath);
        g_editGuiWidgets->info[0] = editWidget(sym::textInfoAPath);
        g_editGuiWidgets->info[1] = editWidget(sym::textInfoBPath);
        g_editGuiWidgets->info[2] = editWidget(sym::textInfoCPath);
        g_editGuiWidgets->info[3] = editWidget(sym::textInfoDPath);
        g_editGuiWidgets->info[4] = editWidget(sym::textInfoEPath);
        g_editGuiWidgets->selection = editWidget(sym::textSelectionPath);
        g_editGuiWidgets->brightnessBar = editWidget(sym::textBrightnessBarPath);
    }
    READABLE_FUNCTION(findEditGuiWidgets, Verified);

    void updateCapacityBar()
    {
        ee::Local<zge::Vec4> estimatePosition; // x: where the used part ends
        ee::Local<ee::Float> usedWidth;
        ee::Local<ee::Float> estimateWidth;

        ee::Float used = *g_parkCapacityUsed / *g_parkCapacity * kCapacityBarWidth;
        const ee::Float estimate = *g_parkCapacityEstimate / *g_parkCapacity * kCapacityBarWidth;
        *usedWidth = used;
        *estimateWidth = estimate;
        if (kCapacityBarWidth < used)
        {
            *usedWidth = kCapacityBarWidth;
            used = kCapacityBarWidth;
        }
        if (used < 0.0f)
        {
            *usedWidth = 0.0f;
            used = 0.0f;
        }
        estimatePosition->x = used;
        estimatePosition->z = 0.0f;
        estimatePosition->y = 0.0f;
        zge::zgui::setVariable(g_editGuiWidgets->capacityBar, kTextWidth, usedWidth);
        zge::zgui::setVariable(g_editGuiWidgets->capacityEstimate, kTextWidth, estimateWidth);
        zge::zgui::setVariable(g_editGuiWidgets->capacityEstimate, kTextPosition, estimatePosition);
    }
    READABLE_FUNCTION(updateCapacityBar, Verified);

    void showEditToolTexts(ee::Ptr<ParkEditor> editor)
    {
        switch (editor->tool)
        {
        case kEditToolTerrain:
            if (editor->cursorState == kCursorBlocked)
            {
                setInfoText(0, kTextNotAvailable);
                setInfoText(1, kTextNotAvailable);
            }
            else
            {
                setInfoText(0, kTextSelect);
                setInfoText(1, kTextDrop);
            }
            setInfoText(2, kTextDelete);
            setInfoText(3, kTextCopy);
            setSelectionTitle(kTextTerrainTool);
            clearHelp();
            break;
        case kEditToolPiece:
            setInfoText(0, kTextPlace);
            setInfoText(1, kTextRotate);
            setInfoText(2, kTextDelete);
            setInfoText(3, kTextCopy);
            break;
        case kEditToolRail:
            setSelectionTitle(kTextRailTool);
            setInfoText(0, kTextPlace);
            setInfoText(1, kTextRotate);
            setInfoText(2, kTextDelete);
            setInfoText(3, kTextCopy);
            clearHelp();
            break;
        case kEditToolStart:
            setSelectionTitle(kTextStartTool);
            setInfoText(0, kTextPlace);
            setInfoText(1, kTextRotate);
            setInfoText(2, kTextNotAvailable);
            setInfoText(3, kTextNotAvailable);
            clearHelp();
            break;
        case kEditToolGap:
            setSelectionTitle(kTextGapTool);
            setInfoText(0, kTextPlace);
            setInfoText(1, kTextNotAvailable);
            setInfoText(2, kTextDelete);
            if (editor->toolStep == 0)
                setInfoText(3, kTextNotAvailable);
            else
                setInfoText(3, kTextBackup);
            break;
        case kEditToolCopy:
            setInfoText(0, kTextPaste);
            setInfoText(1, kTextRotate);
            setInfoText(2, kTextDelete);
            setInfoText(3, kTextCancel);
            setSelectionTitle(kTextCopyTool);
            clearHelp();
            break;
        default:
            break;
        }
        game::tail(updateCapacityBar);
    }
    READABLE_FUNCTION(showEditToolTexts, Verified);

    void cursorColour(ee::Ptr<ParkEditor> editor, ee::Ptr<zge::VertexColour> colour)
    {
        ee::Local<zge::VertexColour> blue;
        ee::Local<zge::VertexColour> darkBlue;
        ee::Local<zge::VertexColour> yellow;
        ee::Local<zge::VertexColour> red;
        *blue = *g_cursorBlue;
        *darkBlue = *g_cursorDarkBlue;
        *yellow = *g_cursorYellow;
        *red = *g_cursorRed;

        if (editor->cursorState == kCursorBlocked)
        {
            *colour = *red;
            return;
        }
        switch (editor->toolStep)
        {
        case 0:
            *colour = *yellow;
            colour->red = static_cast<uint8_t>(ee::toInt(pulseCosine(kCursorPulseRate) * kCursorPulseSwing + kCursorYellowMiddle));
            colour->green = static_cast<uint8_t>(ee::toInt(pulseCosine(kCursorPulseRate) * kCursorPulseSwing + kCursorYellowMiddle));
            break;
        case 1:
            *colour = *blue;
            colour->blue = static_cast<uint8_t>(ee::toInt(pulseCosine(kCursorPulseRate) * kCursorPulseSwing + kCursorBlueMiddle));
            break;
        case 2:
            *colour = *darkBlue;
            colour->blue = static_cast<uint8_t>(ee::toInt(pulseCosine(kCursorPulseRate) * kCursorPulseSwing + kCursorBlueMiddle));
            break;
        default:
            break;
        }
    }
    READABLE_FUNCTION(cursorColour, Verified);

    void tintMarkedPieces(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::VertexColour> colour;
        for (uint32_t row = 0; row < editor->rows; ++row)
        {
            for (uint32_t column = 0; column < editor->columns; ++column)
            {
                const ee::Ptr<PlacedObject> piece = parkCell(editor, column, row)->object;
                if (!piece || piece->column != column || piece->row != row)
                    continue;
                if (piece->flags & kPieceUnderCursor)
                {
                    cursorColour(editor, colour);
                    if (editor->cursorState == kCursorFree && editor->tool == kEditToolTerrain)
                        setNodeColour(piece->node, colour);
                    piece->flags = kPieceTinted;
                }
                else if (piece->flags & kPieceTinted)
                {
                    lightNodeVertices(piece->node, 1);
                    piece->flags &= ~kPieceTinted;
                }
            }
        }
    }
    READABLE_FUNCTION(tintMarkedPieces, Verified);

    void drawCursorCells(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::PrimitiveVertex[4]> corners;
        ee::Local<PrimitiveQuad> quad;
        ee::Local<CellBox> box;
        ee::Local<zge::VertexColour> colour;
        ee::Local<zge::Vec4> centre;

        cursorCellBox(editor, box);
        quad->vertices[1] = &corners[2];
        quad->texture = *g_uiDefaultMaterial;
        quad->vertices[0] = &corners[3];
        quad->vertices[2] = &corners[1];
        quad->vertices[3] = &corners[0];
        cursorColour(editor, colour);
        colour->alpha = static_cast<uint8_t>(ee::toInt(pulseCosine(kCursorPulseRate) * kCellAlphaSwing + kCellAlphaMiddle));
        for (uint32_t i = 0; i < 4; ++i)
        {
            corners[i].v = 0.0f;
            corners[i].u = 0.0f;
            std::memcpy(corners[i].rgba, &*colour, sizeof(zge::VertexColour));
        }

        for (int32_t row = box->firstRow; row <= box->lastRow; ++row)
        {
            for (int32_t column = box->firstColumn; column <= box->lastColumn; ++column)
            {
                cellWorldPosition(editor, column, row, centre);
                const ee::Float right = centre->x + kHalfCell;
                const ee::Float far = centre->z + kHalfCell;
                const ee::Float left = centre->x - kHalfCell;
                const ee::Float y = centre->y - *g_cursorCellsHeightOffset;
                const ee::Float near = centre->z - kHalfCell;
                corners[1].x = right;
                corners[2].z = far;
                corners[0].x = left;
                corners[0].y = y;
                corners[0].z = near;
                corners[3].x = left;
                corners[2].x = right;
                corners[1].z = near;
                corners[3].z = far;
                corners[3].y = y;
                corners[2].y = y;
                corners[1].y = y;
                zge::drawPrimitiveQuads(0, quad, 1, 0);
            }
        }
    }
    READABLE_FUNCTION(drawCursorCells, Verified);

    void drawCursorCross(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<zge::Vec4> centre;
        ee::Local<zge::PrimitiveLine> middle;   // across the cursor's cell and its neighbours
        ee::Local<zge::PrimitiveLine> fadeAhead;
        ee::Local<zge::PrimitiveLine> fadeBehind;
        ee::Local<zge::PrimitiveVertex[6]> ends; // two a line, in the lines' order
        ee::Local<zge::VertexColour> colour;

        *colour = kCrossColour;
        cursorWorldPosition(editor, centre);
        centre->y = centre->y - *g_cursorCrossHeightOffset;
        const uint32_t material = *g_uiDefaultMaterial;
        middle->first = &ends[0];
        middle->second = &ends[1];
        fadeAhead->first = &ends[2];
        fadeAhead->second = &ends[3];
        fadeBehind->texture = material;
        fadeBehind->first = &ends[4];
        fadeBehind->second = &ends[5];
        middle->texture = material;
        fadeAhead->texture = material;
        for (uint32_t i = 0; i < 6; ++i)
        {
            std::memcpy(ends[i].rgba, &*colour, sizeof(zge::VertexColour));
            ends[i].y = centre->y;
        }
        // The fading lines end clear.
        ends[3].rgba[3] = 0;
        ends[5].rgba[3] = 0;

        // Four lines along the rows, a cell apart.
        {
            const ee::Float ahead = centre->z + kCrossArm;
            const ee::Float behind = centre->z - kCrossArm;
            ends[0].z = behind;
            ends[3].z = ahead + kCellSize;
            ends[5].z = behind - kCellSize;
            ends[1].z = ahead;
            ends[2].z = ahead;
            ends[4].z = behind;
            for (int32_t line = -1; line <= 2; ++line)
            {
                const ee::Float x = centre->x + ee::toFloat(line) * kCellSize - kHalfCell;
                for (uint32_t i = 0; i < 6; ++i)
                    ends[i].x = x;
                zge::drawLines(0, middle.address(), 1, 0);
                zge::drawLines(0, fadeAhead.address(), 1, 0);
                zge::drawLines(0, fadeBehind.address(), 1, 0);
            }
        }
        // And four along the columns.
        {
            const ee::Float arm = kCrossArm;
            const ee::Float right = centre->x + arm;
            const ee::Float left = centre->x - arm;
            ends[1].x = right;
            ends[0].x = left;
            ends[2].x = right;
            ends[3].x = right + kCellSize;
            ends[5].x = left - kCellSize;
            ends[4].x = left;
            for (int32_t line = -1; line <= 2; ++line)
            {
                const ee::Float z = centre->z + ee::toFloat(line) * kCellSize - kHalfCell;
                for (uint32_t i = 0; i < 6; ++i)
                    ends[i].z = z;
                zge::drawLines(0, middle.address(), 1, 0);
                zge::drawLines(0, fadeAhead.address(), 1, 0);
                zge::drawLines(0, fadeBehind.address(), 1, 0);
            }
        }
    }
    READABLE_FUNCTION(drawCursorCross, Verified);

    void drawHeldObjectBox(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<CellBox> cells;
        ee::Local<zge::PrimitiveVertex[4]> low;  // the cells' corners at the box's least y
        ee::Local<zge::PrimitiveVertex[4]> high; // and at its greatest
        ee::Local<zge::PrimitiveLine> line;
        ee::Local<zge::Vec4> gridOrigin;         // the centre of cell (0, 0)
        ee::Local<zge::Box> box;
        ee::Local<KeptCorners> kept;

        const ee::Ptr<zge::Box> heldBox = heldObjectBox();
        if (!heldBox)
            return;
        *box = *heldBox;
        cursorCellBox(editor, cells);
        const uint8_t state = editor->cursorState;
        kept->low1 = &low[1];
        kept->low3 = &low[3];
        kept->low2 = &low[2];
        kept->high0 = &high[0];
        kept->high3 = &high[3];
        kept->high2 = &high[2];

        const ee::Float y = box->low.y;
        const ee::Float originZ = -(ee::toFloat(editor->rows - 1) * kHalfCell);
        const ee::Float originX = -(ee::toFloat(editor->columns - 1) * kHalfCell);
        // The height offset is stored with the origin and not used.
        gridOrigin->y = *g_heldBoxHeightOffset;
        gridOrigin->x = originX;
        gridOrigin->z = originZ;
        const ee::Float lastZ = originZ + ee::toFloat(cells->lastRow) * kCellSize + kHalfCell;
        const ee::Float firstX = originX + ee::toFloat(cells->firstColumn) * kCellSize - kHalfCell;
        const ee::Float firstZ = originZ + ee::toFloat(cells->firstRow) * kCellSize - kHalfCell;
        const ee::Float lastX = originX + ee::toFloat(cells->lastColumn) * kCellSize + kHalfCell;
        low[0].x = firstX;
        low[0].z = firstZ;
        low[1].x = lastX;
        low[1].z = firstZ;
        low[2].x = lastX;
        low[2].z = lastZ;
        low[3].x = firstX;
        low[3].z = lastZ;
        for (uint32_t i = 0; i < 4; ++i)
        {
            low[i].y = y;
            low[i].v = 0.0f;
            low[i].u = 0.0f;
            // Red where the object does not fit, else blue.
            if (state == kCursorBlocked)
            {
                low[i].rgba[0] = 0xFF;
                low[i].rgba[1] = 0x20;
                low[i].rgba[2] = 0x20;
            }
            else
            {
                low[i].rgba[0] = 0;
                low[i].rgba[1] = 0;
                low[i].rgba[2] = 0xFF;
            }
            low[i].rgba[3] = 0xFF;
        }

        const auto drawLine = [&](ee::Ptr<zge::PrimitiveVertex> first, ee::Ptr<zge::PrimitiveVertex> second)
        {
            line->first = first;
            line->second = second;
            zge::drawLines(0, line.address(), 1, 0);
        };
        line->texture = *g_uiDefaultMaterial;
        drawLine(&low[0], kept->low1);
        drawLine(&low[0], kept->low3);
        drawLine(kept->low2, kept->low3);
        drawLine(kept->low2, kept->low1);

        for (uint32_t i = 0; i < 4; ++i)
            high[i] = low[i];
        const ee::Float highY = box->high.y;
        high[3].y = highY;
        high[0].y = highY;
        high[2].y = highY;
        high[1].y = highY;
        drawLine(kept->high0, &high[1]);
        drawLine(kept->high0, kept->high3);
        drawLine(kept->high2, kept->high3);
        drawLine(kept->high2, &high[1]);

        // The four edges between them.
        drawLine(&low[0], kept->high0);
        drawLine(kept->low1, &high[1]);
        drawLine(kept->low2, kept->high2);
        drawLine(kept->low3, kept->high3);
    }
    READABLE_FUNCTION(drawHeldObjectBox, Verified);

    void markCursorPieces(ee::Ptr<ParkEditor> editor)
    {
        ee::Local<CellBox> box;
        cursorCellBox(editor, box);
        for (int32_t row = box->firstRow; row <= box->lastRow; ++row)
        {
            for (int32_t column = box->firstColumn; column <= box->lastColumn; ++column)
            {
                if (column < 0 || row < 0 || column >= editor->columns || row >= editor->rows)
                    continue;
                const ee::Ptr<PlacedObject> piece = parkCell(editor, column, row)->object;
                if (piece && piece->column == column && piece->row == row)
                    piece->flags |= kPieceUnderCursor;
            }
        }
    }
    READABLE_FUNCTION(markCursorPieces, Verified);

    uint32_t drawEditGui(ee::Ptr<ParkEditor> editor)
    {
        switch (editor->tool)
        {
        case kEditToolRail:
            if (editor->cursorState != kCursorFree)
                drawRailToolStub(editor);
            else
                drawHeldObjectBox(editor);
            break;
        case kEditToolPiece:
            drawHeldObjectBox(editor);
            break;
        case kEditToolStart:
            break;
        case kEditToolGap:
            drawGapTool(editor);
            if (!cursorFree(editor))
                drawCursorCells(editor);
            break;
        default:
            drawCursorCells(editor);
            break;
        }
        if (editor->tool != kEditToolStart && editor->selectionIsHeldObject == 0)
            drawCursorCross(editor);
        return 1;
    }
    READABLE_FUNCTION(drawEditGui, Verified);

    int32_t showHeldObjectTexts()
    {
        const ee::Ptr<const char> name = heldObjectName();
        const ee::Ptr<const char> help = heldObjectHelp();
        // With no help the result is the last call's: the name's set, or the
        // help asked for (0).
        int32_t result = static_cast<int32_t>(ee::Address(help));
        if (name)
            result = zge::zgui::setVariable(editWidget(sym::textSelectionPath), kTextText, name);
        if (help)
            return game::tail(zge::zgui::setVariable, editWidget(sym::textHelpPath), kTextText, help);
        return result;
    }
    READABLE_FUNCTION(showHeldObjectTexts, Verified);

    void drawRailToolStub(ee::Ptr<ParkEditor>)
    {
    }
    READABLE_FUNCTION(drawRailToolStub, Verified);
}
