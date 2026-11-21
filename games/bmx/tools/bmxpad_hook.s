# PCSX2 pad-injection hook for BMX XXX (USA) SLUS-20415, CRC 2999BCF9: dm2pad_hook.s (DM2's) at BMX's
# addresses, with two additions: an input schedule keyed on the game's level frame counter, and the
# level frame in every seen-log entry.
#
# Pad: the game reads the pad in one place, the per-port pad function 0x001179D0, which calls
# scePadRead (0x0022B280) at 0x00117D60 with a2 = 0x002ABC94 (the 32-byte pad buffer). That call
# becomes `jal pad_hook`. pad_hook calls the real scePadRead, then for port 0 slot 0 only clears the
# requested button bits (active-low) and forces their pressure bytes to 0xFF; the requested buttons
# are the mailbox's (written over PINE, when armed) OR'ed with the schedule's.
#
# Schedule (SCHED, u32 level frame, u32 buttons active-high, sorted by frame): on every poll, every
# entry whose frame is <= the level frame (0x002A2DE0, 0 in the front end, +1 per level frame) is
# taken, and the buttons of the last one taken are held. So a ride replays on exactly the level
# frames it says, whatever the vsyncs per frame (no PINE latency).
#
# VBlank counter: as DM2's hook, an INTC 2 (VBON) handler of our own counts into MAILBOX+0x18,
# installed from the call `jal 0x00100300` (the game's sceGsSyncVCallback setup) at 0x00100364 and,
# as a fallback, on the first pad poll.
#
# Mailbox (MAILBOX, little-endian, all zero = inert): DM2's layout, plus
#   +0x30 u32 schedule entries
#   +0x34 u32 next schedule entry
#   +0x38 u32 schedule buttons held now (active-high)
# Seen log (SEENLOG, 4096 x 16 bytes, ring; count at +0x24, last logged word at +0x28): every port-0
# read whose final buttons/left stick differ from the previous one (and the first read):
#   u32 VBlank count, u32 (buttons active-low | LX << 16 | LY << 24), u32 level frame, u32 poll count.

        .set    noreorder
        .set    noat

        .set    SCEPADREAD,      0x0022B280
        .set    ADDINTCHANDLER,  0x0022C7E0   # syscall 0x10 wrapper (cause, handler, next)
        .set    ENABLEINTC,      0x0022D0C8   # DI / _EnableIntc / EI wrapper
        .set    INIT_TARGET,     0x00100300   # original target of the call at 0x00100364
        .set    LEVEL_FRAME,     0x002A2DE0
        .set    MAILBOX,         0x000F7000
        .set    MAGIC_HI,        0x4D44
        .set    MAGIC_LO,        0x4150
        .set    SEENLOG,         0x000E0000
        .set    SCHED,           0x000D0000

        .text
        .globl  pad_hook
pad_hook:
        addiu   $sp, $sp, -32
        sd      $ra, 0($sp)
        sd      $a0, 8($sp)             # port
        sd      $a1, 16($sp)            # slot
        jal     SCEPADREAD
        sd      $a2, 24($sp)            # (delay) buffer
        ld      $t0, 8($sp)
        ld      $t1, 16($sp)
        or      $t0, $t0, $t1
        bne     $t0, $zero, done        # only port 0, slot 0
        nop
        blez    $v0, done               # no data this time
        nop

        lui     $t3, %hi(MAILBOX)
        addiu   $t3, $t3, %lo(MAILBOX)
        lw      $t4, 0x0C($t3)
        bne     $t4, $zero, counted
        nop
        # first poll: make sure the VBlank counter runs, remember its value
        sd      $v0, 8($sp)             # keep scePadRead's result
        jal     install_vsync
        nop
        ld      $v0, 8($sp)
        lui     $t3, %hi(MAILBOX)
        addiu   $t3, $t3, %lo(MAILBOX)
        lw      $t6, 0x18($t3)
        sw      $t6, 0x14($t3)
        lw      $t4, 0x0C($t3)
counted:
        addiu   $t4, $t4, 1
        sw      $t4, 0x0C($t3)          # poll counter
        ld      $t2, 24($sp)            # buffer
        lhu     $t5, 2($t2)
        sh      $t5, 0x12($t3)          # raw buttons
        sh      $t5, 0x10($t3)          # seen buttons (updated below if anything is pressed)

        # Schedule: take every entry due at this level frame.
        lui     $t6, %hi(LEVEL_FRAME)
        lw      $t7, %lo(LEVEL_FRAME)($t6)      # t7 = level frame
        lw      $t8, 0x34($t3)          # next entry
        lw      $t9, 0x30($t3)          # entries
sched_loop:
        sltu    $t6, $t8, $t9
        beq     $t6, $zero, sched_done
        nop
        sll     $t6, $t8, 3
        lui     $t0, %hi(SCHED)
        addiu   $t0, $t0, %lo(SCHED)
        addu    $t6, $t6, $t0
        lw      $t0, 0($t6)             # entry frame
        beq     $t7, $zero, sched_done  # front end: no level frame yet
        sltu    $t1, $t7, $t0           # (delay) level frame < entry frame: not due
        bne     $t1, $zero, sched_done
        nop
        lw      $t0, 4($t6)
        sw      $t0, 0x38($t3)          # held buttons
        addiu   $t8, $t8, 1
        b       sched_loop
        sw      $t8, 0x34($t3)          # (delay)
sched_done:
        lw      $t4, 0x38($t3)          # schedule buttons
        lw      $t0, 0x00($t3)
        lui     $t6, MAGIC_HI
        ori     $t6, $t6, MAGIC_LO
        bne     $t0, $t6, apply         # mailbox not armed: the schedule's alone
        nop
        lhu     $t0, 0x04($t3)          # mailbox buttons (active-high)
        or      $t4, $t4, $t0
apply:
        beq     $t4, $zero, sticks
        nop
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

sticks:
        lw      $t0, 0x00($t3)
        lui     $t6, MAGIC_HI
        ori     $t6, $t6, MAGIC_LO
        bne     $t0, $t6, log_seen      # not armed
        nop
        lbu     $t4, 0x06($t3)          # flags
        andi    $t4, $t4, 1
        beq     $t4, $zero, log_seen
        nop
        lbu     $t4, 0x08($t3)
        sb      $t4, 4($t2)             # RX
        lbu     $t4, 0x09($t3)
        sb      $t4, 5($t2)             # RY
        lbu     $t4, 0x0A($t3)
        sb      $t4, 6($t2)             # LX
        lbu     $t4, 0x0B($t3)
        sb      $t4, 7($t2)             # LY
log_seen:
        lhu     $t5, 2($t2)             # final buttons (active-low)
        lbu     $t6, 6($t2)             # LX
        lbu     $t7, 7($t2)             # LY
        sll     $t6, $t6, 16
        sll     $t7, $t7, 24
        or      $t5, $t5, $t6
        or      $t5, $t5, $t7
        lw      $t6, 0x28($t3)          # last logged
        lw      $t7, 0x24($t3)          # count
        beq     $t7, $zero, 2f          # always log the first read
        nop
        beq     $t5, $t6, done
        nop
2:
        sw      $t5, 0x28($t3)
        andi    $t8, $t7, 0xFFF
        sll     $t8, $t8, 4
        lui     $t9, %hi(SEENLOG)
        addiu   $t9, $t9, %lo(SEENLOG)
        addu    $t8, $t8, $t9
        lw      $t6, 0x18($t3)          # VBlank count
        sw      $t6, 0($t8)
        sw      $t5, 4($t8)
        lui     $t6, %hi(LEVEL_FRAME)
        lw      $t6, %lo(LEVEL_FRAME)($t6)
        sw      $t6, 8($t8)
        lw      $t6, 0x0C($t3)          # polls
        sw      $t6, 12($t8)
        addiu   $t7, $t7, 1
        sw      $t7, 0x24($t3)
done:
        ld      $ra, 0($sp)
        jr      $ra
        addiu   $sp, $sp, 32            # (delay)

# Replaces `jal 0x00100300` at 0x00100364: install the VBlank counter, then tail-call the original
# (the game's VBlank callback setup, no arguments).
        .globl  init_hook
init_hook:
        addiu   $sp, $sp, -16
        sd      $ra, 0($sp)
        jal     install_vsync
        nop
        ld      $ra, 0($sp)
        j       INIT_TARGET
        addiu   $sp, $sp, 16            # (delay)

# Registers vsync_handler for INTC 2 (VBON) and enables it, once.
install_vsync:
        lui     $t0, %hi(MAILBOX)
        lw      $t1, %lo(MAILBOX + 0x1C)($t0)
        bne     $t1, $zero, 1f
        nop
        addiu   $sp, $sp, -16
        sd      $ra, 0($sp)
        addiu   $a0, $zero, 2           # INTC_VBLANK_S
        lui     $a1, %hi(vsync_handler)
        addiu   $a1, $a1, %lo(vsync_handler)
        jal     ADDINTCHANDLER
        addiu   $a2, $zero, -1          # (delay) next = -1
        lui     $t0, %hi(MAILBOX)
        sw      $v0, %lo(MAILBOX + 0x20)($t0)
        addiu   $t1, $zero, 1
        sw      $t1, %lo(MAILBOX + 0x1C)($t0)
        jal     ENABLEINTC
        addiu   $a0, $zero, 2           # (delay)
        ld      $ra, 0($sp)
        addiu   $sp, $sp, 16
1:
        jr      $ra
        nop

# INTC handler: count VBlanks, return 0.
vsync_handler:
        lui     $t0, %hi(MAILBOX)
        lw      $t1, %lo(MAILBOX + 0x18)($t0)
        addiu   $t1, $t1, 1
        sw      $t1, %lo(MAILBOX + 0x18)($t0)
        jr      $ra
        move    $v0, $zero              # (delay)

        # buffer offset of the pressure byte for button bit n (0 = none)
        # pressure order: RIGHT LEFT UP DOWN TRIANGLE CIRCLE CROSS SQUARE L1 R1 L2 R2
pressure_offsets:
        .byte   0, 0, 0, 0              # SELECT L3 R3 START
        .byte   10, 8, 11, 9            # UP RIGHT DOWN LEFT
        .byte   18, 19, 16, 17          # L2 R2 L1 R1
        .byte   12, 13, 14, 15          # TRIANGLE CIRCLE CROSS SQUARE
