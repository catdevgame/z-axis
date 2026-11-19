# MIPS (R5900) code for the Aggressive Inline (USA, SLUS-20327, CRC 67DB3ED8)
# PCSX2 / cheat-device mods in mods/pcsx2/SLUS-20327_67DB3ED8.pnach.
# Assembled by tools/build_mods_pnach.py with the decomp's binutils; the
# addresses come from the native PC port's research (docs/BRINGUP.md,
# docs/DEVTOOLS.md, PS2Recomp game_overrides/aggressive_inline_*.cpp).
#
# Code cave: 0x00219A80-0x0021B5B7 is libkernl's file-system API
# (sceMkdir, sceFormat, sceAddDrv, sceDopen ... sceReadlink). The game never
# calls any of it: no jal/j targets it, and no word in the ELF and no
# lui/addiu pair points into it (checked over the whole loaded image). The
# three parts below sit at fixed addresses inside it (see build_mods_pnach.py)
# so that every pnach section that needs a part writes identical words.
#
# Variables: 0x002728C0-0x002728FF is the zero padding between .rodata
# (ends 0x272890) and .lit4 (0x272900). BloodRaynare's widescreen patch uses
# 0x272894-0x2728AF of the same gap for its frustum code. Section flags are
# written there by the sections that use them; with a section off its flag
# stays 0, so a shared part does nothing for it.
#
# Registers: only $1-$3, $12-$15, $24, $25 (and $5-$11 in the leaf squeeze
# hook, whose original reads only a0) and $f0-$f5 are used, all caller-saved
# temporaries in the PS2 EABI.

        .set    noreorder
        .set    noat

        # ---- game addresses ------------------------------------------------
        .set    PAD_HELD,       0x0024CAD8  # player 1 buttons held (game bits)
        .set    PAD_PRESSED,    0x0024CADC  # player 1 buttons pressed this frame
        .set    GAME_TYPE,      0x0024C9B3  # u8: 1 Career, 2 Freeskate, 3 Timed Run, 4 split screen
        .set    LEVEL_FRAME,    0x00231CAC  # frames since the level started
        .set    LOADER_DELAY,   0x00276DD5  # u8: frames the loader waits
        .set    LOADER_TARGET,  0x00276DD8  # state the loader enters (0 = level)
        .set    LEVEL_UPDATE,   0x001ACA68  # game mode 2 (play) update
        .set    SWITCH_STATE,   0x001BCD38  # top-level state switch (2 = loader)
        .set    PLAY_AGAIN,     0x001A7460  # Timed Run restart (pause menu Play Again)
        .set    SET_MODE,       0x001A70F0  # game mode switch (6 = debug menu)
        .set    GUI_DRAW_BODY,  0x00171CB0  # zgui widget draw, after its first 2 words
        .set    CLOSE_UPK_BODY, 0x00210F30  # sceVif1PkCloseUpkCode, after its first 2 words
        .set    PACKET_2D,      0x0027A848  # the 2D (screen-space) VIF packet
        .set    SCREEN_WIDTH,   0x0023190C  # float: 640 front end, 512 in levels
        .set    GAME_MODE,      0x00276FE4  # input mode: 1 front end, 2 play, 3 menus in a level

        # ---- variables in the .rodata padding --------------------------------
        .set    VARS,           0x002728C0
        .set    V_RESTART,      0           # 1 = [Quick Restart] on
        .set    V_DEBUG,        4           # 1 = [Developer Debug Menu] on
        .set    V_HUDKEY,       8           # 1 = [Hide HUD Toggle] on
        .set    V_HIDDEN,       12          # state: HUD hidden (toggled)
        .set    V_PREV,         16          # state: buttons held at the last update
        .set    V_SCALE,        20          # float: 2D squeeze (0.75 at 16:9), 0 = off
        .set    V_SHIFT,        24          # float: HUD shift / width (1/6 at 16:9), 0 = off

        # ---- button combos (game bits: Select 0x8000, L3 0x1000, R3 0x2000)
        .set    COMBO_RESTART,  0x3000      # L3 + R3
        .set    COMBO_DEBUG,    0x9000      # L3 + SELECT
        .set    COMBO_HUD,      0xA000      # R3 + SELECT

        .text

# =========================================================================
# Part L: the play update (game mode 2). The mode table's update pointer
# 0x0024B844 (0x001ACA68) points here instead. Checks the combos on the
# frame they are completed, else continues with the game's update (j, so all
# argument registers and $f12 are untouched).
# =========================================================================
        .globl  level_update
level_update:
        lui     $12, %hi(VARS)
        addiu   $12, $12, %lo(VARS)
        lui     $13, %hi(PAD_HELD)
        lw      $13, %lo(PAD_HELD)($13)     # held now
        lw      $14, V_PREV($12)            # held at the last update
        beq     $13, $14, 1f
        nop
        sw      $13, V_PREV($12)
1:
        # ---- Quick Restart: L3 + R3 -------------------------------------
        lw      $24, V_RESTART($12)
        beq     $24, $0, 2f
        li      $15, COMBO_RESTART
        and     $24, $13, $15
        bne     $24, $15, 2f                # not both held
        and     $24, $14, $15
        beq     $24, $15, 2f                # already held last time
        nop
        lui     $24, %hi(LEVEL_FRAME)
        lw      $24, %lo(LEVEL_FRAME)($24)
        sltiu   $24, $24, 31
        bne     $24, $0, 2f                 # level just started
        lui     $24, %hi(GAME_TYPE)
        lbu     $24, %lo(GAME_TYPE)($24)
        li      $25, 4
        beq     $24, $25, 2f                # split screen: not handled
        li      $25, 3
        bne     $24, $25, 3f
        nop
        j       PLAY_AGAIN                  # Timed Run: the game's Play Again (tail call)
        nop
3:      lui     $24, %hi(LOADER_TARGET)
        sw      $0, %lo(LOADER_TARGET)($24) # the loader enters the level again
        li      $25, 1
        lui     $24, %hi(LOADER_DELAY)
        sb      $25, %lo(LOADER_DELAY)($24)
        j       SWITCH_STATE                # level exit, loader (tail call)
        li      $4, 2
2:
        # ---- Developer Debug Menu: L3 + SELECT --------------------------
        lw      $24, V_DEBUG($12)
        beq     $24, $0, 4f
        li      $15, COMBO_DEBUG
        and     $24, $13, $15
        bne     $24, $15, 4f
        and     $24, $14, $15
        beq     $24, $15, 4f
        nop
        lui     $24, %hi(PAD_PRESSED)       # the combo's SELECT does not open
        lw      $25, %lo(PAD_PRESSED)($24)  # the Challenges list
        andi    $25, $25, 0x7FFF
        sw      $25, %lo(PAD_PRESSED)($24)
        j       SET_MODE                    # game mode 6: the debug menu (tail call)
        li      $4, 6
4:
        # ---- Hide HUD toggle: R3 + SELECT -------------------------------
        lw      $24, V_HUDKEY($12)
        beq     $24, $0, 5f
        li      $15, COMBO_HUD
        and     $24, $13, $15
        bne     $24, $15, 5f
        and     $24, $14, $15
        beq     $24, $15, 5f
        nop
        lw      $24, V_HIDDEN($12)
        xori    $24, $24, 1
        sw      $24, V_HIDDEN($12)
        lui     $24, %hi(PAD_PRESSED)       # the combo's SELECT does not open
        lw      $25, %lo(PAD_PRESSED)($24)  # the Challenges list
        andi    $25, $25, 0x7FFF
        sw      $25, %lo(PAD_PRESSED)($24)
5:
        j       LEVEL_UPDATE
        nop
        .globl  level_update_end
level_update_end:

# =========================================================================
# Part G: zgui widget draw 0x00171CA8 (a0 = widget; it draws the widget and
# then its children through the same function pointer). Its first two words
# become `j gui_draw; nop`.
#  - Hide HUD: while hidden, InGameUI's / MultiplayerUI's Overlay and
#    OverlayToggle groups (score, juice, trick list, balance, timer,
#    messages) are not drawn.
#  - Widescreen: a direct child of InGameUI/Overlay or InGameUI/OverlayToggle
#    (or the game-over score PauseMenus/Death/Score) in the left or right
#    third of the screen is moved out by width * V_SHIFT while it is drawn,
#    so after the 2D squeeze it sits at the wide picture's edge.
# Widget: +0x08 parent, +0x0C type (low 14 bits 1 = drawable), +0x14 name,
# +0x20 position, +0x30 screen position.
# =========================================================================
        .align  4
        .globl  gui_draw
gui_draw:
        addiu   $sp, $sp, -32
        sd      $31, 0($sp)
        sw      $4, 8($sp)                  # widget
        lui     $12, %hi(VARS)
        addiu   $12, $12, %lo(VARS)
        # ---- hide HUD --------------------------------------------------
        lw      $13, V_HUDKEY($12)
        beq     $13, $0, g_wide
        lw      $13, V_HIDDEN($12)
        beq     $13, $0, g_wide
        move    $13, $4
        la      $14, s_overlay
        jal     name_is
        nop
        bne     $2, $0, 1f
        move    $13, $4
        la      $14, s_overlay_toggle
        jal     name_is
        nop
        beq     $2, $0, g_wide
        nop
1:      lw      $13, 8($4)                  # parent
        la      $14, s_ingameui
        jal     name_is
        nop
        bne     $2, $0, g_skip
        lw      $13, 8($4)
        la      $14, s_multiplayerui
        jal     name_is
        nop
        beq     $2, $0, g_wide
        nop
g_skip: ld      $31, 0($sp)                 # not drawn (nor its children)
        jr      $31
        addiu   $sp, $sp, 32

        # ---- widescreen HUD shift -----------------------------------------
g_wide: lw      $13, V_SHIFT($12)
        beq     $13, $0, g_tail
        lw      $13, 0x0C($4)
        andi    $13, $13, 0x3FFF
        li      $14, 1
        bne     $13, $14, g_tail
        lw      $13, 8($4)                  # parent
        beq     $13, $0, g_tail
        sw      $13, 16($sp)
        lw      $14, 8($13)                 # grandparent
        beq     $14, $0, g_tail
        sw      $14, 20($sp)
        la      $14, s_overlay
        jal     name_is
        nop
        bne     $2, $0, 2f
        lw      $13, 16($sp)
        la      $14, s_overlay_toggle
        jal     name_is
        nop
        beq     $2, $0, 3f
        nop
2:      lw      $13, 20($sp)
        la      $14, s_ingameui
        jal     name_is
        nop
        bne     $2, $0, g_shift
        nop
3:      lw      $13, 8($sp)                 # or PauseMenus/Death/Score
        la      $14, s_score
        jal     name_is
        nop
        beq     $2, $0, g_tail
        lw      $13, 16($sp)
        la      $14, s_death
        jal     name_is
        nop
        beq     $2, $0, g_tail
        lw      $13, 20($sp)
        la      $14, s_pausemenus
        jal     name_is
        nop
        beq     $2, $0, g_tail
        nop
g_shift:
        lw      $4, 8($sp)
        lw      $13, 16($sp)
        lwc1    $f0, 0x30($13)              # parent's screen x
        lwc1    $f1, 0x20($4)               # own x
        add.s   $f0, $f0, $f1               # x
        lui     $14, %hi(SCREEN_WIDTH)
        lwc1    $f1, %lo(SCREEN_WIDTH)($14) # width
        lwc1    $f2, V_SHIFT($12)
        mul.s   $f2, $f1, $f2               # shift = width * V_SHIFT
        li      $14, 0x3EAAAAAB             # 1/3
        mtc1    $14, $f3
        mul.s   $f3, $f1, $f3               # width / 3
        c.lt.s  $f0, $f3
        nop
        bc1t    4f                          # left third: move left
        add.s   $f3, $f3, $f3               # (delay) 2 width / 3
        c.lt.s  $f3, $f0
        nop
        bc1f    g_tail                      # middle third: as it is
        nop
        b       5f
        nop
4:      neg.s   $f2, $f2
5:      swc1    $f2, 12($sp)
        lwc1    $f1, 0x20($4)
        add.s   $f1, $f1, $f2
        swc1    $f1, 0x20($4)
        jal     gui_draw_original
        nop
        lw      $4, 8($sp)                  # undone as a move
        lwc1    $f1, 0x20($4)
        lwc1    $f2, 12($sp)
        sub.s   $f1, $f1, $f2
        swc1    $f1, 0x20($4)
        ld      $31, 0($sp)
        jr      $31
        addiu   $sp, $sp, 32

g_tail: lw      $4, 8($sp)
        ld      $31, 0($sp)
        addiu   $sp, $sp, 32
gui_draw_original:                          # the two words the hook replaced
        addiu   $sp, $sp, -48
        j       GUI_DRAW_BODY
        li      $3, 1

# $13 = widget (may be 0), $14 = string: $2 = 1 if the widget's name is it.
# Uses $2, $14, $15, $24, $25.
name_is:
        beq     $13, $0, 9f
        move    $2, $0
        lw      $24, 0x14($13)
        beq     $24, $0, 9f
        nop
1:      lbu     $25, 0($24)
        lbu     $15, 0($14)
        bne     $25, $15, 9f
        addiu   $24, $24, 1
        bne     $15, $0, 1b
        addiu   $14, $14, 1
        li      $2, 1
9:      jr      $31
        nop

s_overlay:          .asciz  "Overlay"
s_overlay_toggle:   .asciz  "OverlayToggle"
s_ingameui:         .asciz  "InGameUI"
s_multiplayerui:    .asciz  "MultiplayerUI"
s_score:            .asciz  "Score"
s_death:            .asciz  "Death"
s_pausemenus:       .asciz  "PauseMenus"
        .align  2
        .globl  gui_draw_end
gui_draw_end:

# =========================================================================
# Part S: sceVif1PkCloseUpkCode 0x00210F28 (a0 = packet; a leaf). Its first
# two words become `j squeeze; nop`. For the 2D packet, a finished primitive
# (one UNPACK V4-32 to VU1 address 8: a PACKED GIF tag, then per vertex nreg
# qwords, XYZ2/XYZF2 as float x, y, z in screen pixels; bit 0 of x is the
# ADC flag) has its x squeezed about the screen centre by V_SCALE, so 2D
# keeps its 4:3 shape in a 16:9 picture. In a level (0x276FE4 = 2 or 3)
# primitives over the whole width (fades, darkened menu backgrounds,
# letterbox) are left alone; elsewhere (front end, movies) all 2D is
# squeezed, so those screens show as 4:3 between bars. (The level loading
# screen does not go through this path and stays stretched.)
# =========================================================================
        .align  4
        .globl  squeeze
squeeze:
        lui     $12, %hi(PACKET_2D)
        addiu   $12, $12, %lo(PACKET_2D)
        bne     $4, $12, s_orig
        lui     $12, %hi(VARS)
        addiu   $12, $12, %lo(VARS)
        lw      $13, V_SCALE($12)
        beq     $13, $0, s_orig
        mtc1    $13, $f3                    # scale
        lw      $13, 0($4)                  # packet current pointer
        lw      $14, 0x0C($4)               # open UNPACK code
        beq     $14, $0, s_orig
        lw      $15, 0($14)
        srl     $24, $15, 24
        andi    $24, $24, 0x6F
        li      $25, 0x6C                   # UNPACK V4-32 (mask bit ignored)
        bne     $24, $25, s_orig
        andi    $24, $15, 0x3FF
        li      $25, 8                      # to VU1 address 8
        bne     $24, $25, s_orig
        addiu   $14, $14, 4                 # data
        sltu    $24, $14, $13
        beq     $24, $0, s_orig
        subu    $15, $13, $14
        andi    $24, $15, 15
        bne     $24, $0, s_orig
        srl     $15, $15, 4                 # qwords
        lw      $5, 0($14)
        andi    $5, $5, 0x7FFF              # NLOOP
        beq     $5, $0, s_orig
        lw      $24, 4($14)
        srl     $6, $24, 28                 # NREG
        beq     $6, $0, s_orig
        srl     $24, $24, 26
        andi    $24, $24, 3                 # FLG must be PACKED
        bne     $24, $0, s_orig
        sltiu   $24, $6, 9                  # REGS must fit in 32 bits
        beq     $24, $0, s_orig
        mult    $7, $5, $6
        addiu   $7, $7, 1
        bne     $7, $15, s_orig             # the tag covers exactly the data
        lw      $8, 8($14)                  # REGS
        move    $9, $0
        li      $10, -1
6:      sll     $11, $9, 2
        srlv    $11, $8, $11
        andi    $11, $11, 0xF
        addiu   $11, $11, -4                # XYZF2 = 4, XYZ2 = 5
        sltiu   $11, $11, 2
        bne     $11, $0, 7f
        nop
        addiu   $9, $9, 1
        bne     $9, $6, 6b
        nop
        b       s_orig
        nop
7:      move    $10, $9
        sll     $11, $6, 4                  # stride
        sll     $12, $10, 4
        addu    $12, $12, $14
        addiu   $12, $12, 16                # first vertex's x
        move    $13, $12
        move    $9, $5
        lwc1    $f0, 0($13)                 # lo
        mov.s   $f1, $f0                    # hi
8:      lwc1    $f2, 0($13)
        min.s   $f0, $f0, $f2
        max.s   $f1, $f1, $f2
        addiu   $9, $9, -1
        bne     $9, $0, 8b
        addu    $13, $13, $11
        lui     $24, %hi(SCREEN_WIDTH)
        lwc1    $f2, %lo(SCREEN_WIDTH)($24)
        lui     $24, %hi(GAME_MODE)         # outside a level (front end and
        lw      $24, %lo(GAME_MODE)($24)    # movies) everything is squeezed: the
        addiu   $24, $24, -2                # front end shows as 4:3 with bars
        sltiu   $24, $24, 2
        beq     $24, $0, 1f
        lui     $24, 0x4080                 # 4.0
        mtc1    $24, $f4
        c.le.s  $f0, $f4
        nop
        bc1f    1f                          # starts right of the left edge
        sub.s   $f5, $f2, $f4
        c.le.s  $f5, $f1
        nop
        bc1t    s_orig                      # full width: left alone
        nop
1:      lui     $24, 0x3F00                 # 0.5
        mtc1    $24, $f4
        mul.s   $f4, $f2, $f4               # centre
        move    $13, $12
        move    $9, $5
2:      lw      $24, 0($13)
        mtc1    $24, $f5
        sub.s   $f5, $f5, $f4
        mul.s   $f5, $f5, $f3
        add.s   $f5, $f5, $f4
        mfc1    $25, $f5
        srl     $25, $25, 1
        sll     $25, $25, 1
        andi    $24, $24, 1                 # keep the ADC flag
        or      $25, $25, $24
        sw      $25, 0($13)
        addiu   $9, $9, -1
        bne     $9, $0, 2b
        addu    $13, $13, $11
s_orig: lw      $2, 0($4)                   # the two words the hook replaced
        j       CLOSE_UPK_BODY
        lui     $3, 0xFFFF
        .globl  squeeze_end
squeeze_end:
