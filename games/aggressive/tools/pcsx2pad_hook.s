# PCSX2 pad-injection hook for Aggressive Inline (USA) SLUS-20327, CRC 67DB3ED8.
#
# The game reads the pad in exactly one place: the per-port pad state machine
# func_00118540 calls scePadRead (0x00211A30) at 0x001188D0 with
# a2 = 0x00282494 (a 32-byte pad data buffer shared by all ports). This file
# replaces that call with `jal pad_hook`. pad_hook calls the real scePadRead,
# then, for port 0 slot 0 only and only when the mailbox is armed, clears the
# requested button bits (active-low), forces the matching pressure bytes to
# 0xFF and optionally overrides the analog sticks.
#
# build_hook.py assembles this with the decomp's binutils and writes the
# result as pnach lines. Addresses below must match pcsx2pad.py.
#
# Mailbox layout (MAILBOX, little-endian, all zero = inert):
#   +0x00 u32 magic   0x4D444150 ("PADM") arms the hook; anything else = off
#   +0x04 u16 pressed buttons, ACTIVE-HIGH (bit set = held). Same bit order as
#             the pad word: bit0 SELECT, L3, R3, START, UP, RIGHT, DOWN, LEFT,
#             L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, bit15 SQUARE
#   +0x06 u8  flags   bit0 = override analog sticks with +0x08..+0x0B
#   +0x07 u8  reserved
#   +0x08 u8  RX, RY, LX, LY (0x80 = centre, 0x00 = left/up, 0xFF = right/down)
#   +0x0C u32 poll counter: +1 on every port-0 slot-0 scePadRead that returned
#             data (counts even when not armed; proves the hook is live)
#   +0x10 u16 the button word the game finally saw (active-low), for checks
#   +0x12 u16 the button word from the real pad before injection
#   +0x14 u32 value of the game's vsync counter (0x00231CA4) at the first poll
#
# Active-high was chosen over an active-low AND mask so that zeroed or
# uninitialised memory can never hold buttons down.

        .set    noreorder
        .set    noat

        .set    SCEPADREAD, 0x00211A30
        .set    MAILBOX,    0x000F7000
        .set    VSYNC_COUNTER, 0x00231CA4   # game's VBlank count (func_00100268)
        .set    MAGIC_HI,   0x4D44
        .set    MAGIC_LO,   0x4150

        .text
        .globl  pad_hook
pad_hook:
        addiu   $sp, $sp, -32
        sd      $ra, 0($sp)
        sd      $a0, 8($sp)             # port
        sd      $a1, 16($sp)            # slot
        jal     SCEPADREAD
        sd      $a2, 24($sp)            # (delay) buffer
        ld      $ra, 0($sp)
        ld      $t0, 8($sp)
        ld      $t1, 16($sp)
        ld      $t2, 24($sp)
        addiu   $sp, $sp, 32
        or      $t0, $t0, $t1
        bne     $t0, $zero, done        # only port 0, slot 0
        nop
        blez    $v0, done               # no data this time
        nop

        lui     $t3, %hi(MAILBOX)
        addiu   $t3, $t3, %lo(MAILBOX)
        lw      $t4, 0x0C($t3)
        bne     $t4, $zero, counted
        addiu   $t4, $t4, 1             # (delay)
        lui     $t6, %hi(VSYNC_COUNTER) # first poll: remember the vsync count
        lw      $t6, %lo(VSYNC_COUNTER)($t6)
        sw      $t6, 0x14($t3)
counted:
        sw      $t4, 0x0C($t3)          # poll counter
        lhu     $t5, 2($t2)
        sh      $t5, 0x12($t3)          # raw buttons
        sh      $t5, 0x10($t3)          # seen buttons (updated below if armed)

        lw      $t4, 0x00($t3)
        lui     $t6, MAGIC_HI
        ori     $t6, $t6, MAGIC_LO
        bne     $t4, $t6, done          # not armed
        nop

        lhu     $t4, 0x04($t3)          # pressed mask (active-high)
        nor     $t6, $t4, $zero
        and     $t5, $t5, $t6           # clear pressed bits
        sh      $t5, 2($t2)
        sh      $t5, 0x10($t3)

        # Pressure bytes: buttons 4..15 have a pressure byte at buf[8..19].
        lui     $t9, %hi(pressure_offsets)
        addiu   $t9, $t9, %lo(pressure_offsets)
        addiu   $t6, $zero, 4
        addiu   $t7, $zero, 0xFF
press_loop:
        srlv    $t8, $t4, $t6
        andi    $t8, $t8, 1
        beq     $t8, $zero, press_next
        addu    $t8, $t9, $t6           # (delay) &pressure_offsets[bit]
        lbu     $t8, 0($t8)
        addu    $t8, $t2, $t8
        sb      $t7, 0($t8)
press_next:
        addiu   $t6, $t6, 1
        slti    $t8, $t6, 16
        bne     $t8, $zero, press_loop
        nop

        lbu     $t4, 0x06($t3)          # flags
        andi    $t4, $t4, 1
        beq     $t4, $zero, done
        nop
        lbu     $t4, 0x08($t3)
        sb      $t4, 4($t2)             # RX
        lbu     $t4, 0x09($t3)
        sb      $t4, 5($t2)             # RY
        lbu     $t4, 0x0A($t3)
        sb      $t4, 6($t2)             # LX
        lbu     $t4, 0x0B($t3)
        sb      $t4, 7($t2)             # LY
done:
        jr      $ra
        nop

        # buffer offset of the pressure byte for button bit n (0 = none)
        # pressure order: RIGHT LEFT UP DOWN TRIANGLE CIRCLE CROSS SQUARE L1 R1 L2 R2
pressure_offsets:
        .byte   0, 0, 0, 0              # SELECT L3 R3 START
        .byte   10, 8, 11, 9            # UP RIGHT DOWN LEFT
        .byte   18, 19, 16, 17          # L2 R2 L1 R1
        .byte   12, 13, 14, 15          # TRIANGLE CIRCLE CROSS SQUARE
