# PCSX2 replay/record hook for Aggressive Inline (USA) SLUS-20327, CRC 67DB3ED8.
#
# Same call site as pcsx2pad_hook.s (the game's only scePadRead call, at
# 0x001188D0), but the input comes from a schedule in EE RAM instead of a
# mailbox written over PINE, so a recorded input sequence replays on exactly
# the vsyncs it says, with no PINE latency. On every port-0 slot-0 poll the
# hook also appends a record (vsync, poll count, then a list of watched
# guest words) to a ring buffer, and it can freeze the game at a given vsync
# (spin in the hook until the host releases it), so the host can read
# a consistent RAM image of exactly that frame.
#
# pcsx2replay.py assembles this, installs the pnach in its own PCSX2 data
# dir and drives it. Addresses below must match pcsx2replay.py.
#
# Control block (CTRL, little-endian):
#   +0x00 u32 magic 0x594C5052 ("RPLY") arms the hook; anything else = inert
#   +0x04 u32 schedule entries
#   +0x08 u32 next schedule entry
#   +0x0C u32 buttons currently held (active-high, pad bit order)
#   +0x10 u32 log records written (total; slot = total % capacity)
#   +0x14 u32 log capacity in records (0 = no log)
#   +0x18 u32 log record size in bytes (8 + 4 * watched words)
#   +0x1C u32 freeze vsync: on the first poll with vsync >= this, spin until
#             the host writes another value here (the next freeze vsync, or
#             0 = no freeze)
#   +0x20 u32 vsync the hook froze at
#   +0x24 u32 watch entries
#   +0x28 u32 polls seen while armed
#   +0x40 watch list: (u32 address, u32 words) per entry
# Schedule (SCHED): (u32 vsync, u32 buttons) per entry, sorted by vsync; the
#   buttons of the last entry with vsync <= the current vsync are held.
# Log (LOG): per record u32 vsync, u32 poll count, then the watched words.

        .set    noreorder
        .set    noat

        .set    SCEPADREAD, 0x00211A30
        .set    CTRL,       0x000F6800
        .set    SCHED,      0x00096000
        .set    LOG,        0x000A0000
        .set    VSYNC_COUNTER, 0x00231CA4
        .set    MAGIC_HI,   0x594C
        .set    MAGIC_LO,   0x5052

        .text
        .globl  replay_hook
replay_hook:
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

        lui     $t3, %hi(CTRL)
        addiu   $t3, $t3, %lo(CTRL)
        lw      $t4, 0x00($t3)
        lui     $t6, MAGIC_HI
        ori     $t6, $t6, MAGIC_LO
        bne     $t4, $t6, done          # not armed
        nop
        lui     $t6, %hi(VSYNC_COUNTER)
        lw      $t7, %lo(VSYNC_COUNTER)($t6)    # t7 = vsync
        lw      $t4, 0x28($t3)
        addiu   $t4, $t4, 1
        sw      $t4, 0x28($t3)

        # Schedule: advance over every entry that is due.
        lw      $t4, 0x08($t3)          # next entry
        lw      $t5, 0x04($t3)          # entries
        lw      $t6, 0x0C($t3)          # held buttons
        lui     $t9, %hi(SCHED)
        addiu   $t9, $t9, %lo(SCHED)
sched_loop:
        sltu    $t8, $t4, $t5
        beq     $t8, $zero, sched_done
        sll     $t8, $t4, 3             # (delay)
        addu    $t8, $t9, $t8
        lw      $t0, 0($t8)
        sltu    $t1, $t7, $t0
        bne     $t1, $zero, sched_done  # not due yet
        nop
        lw      $t6, 4($t8)
        b       sched_loop
        addiu   $t4, $t4, 1             # (delay)
sched_done:
        sw      $t4, 0x08($t3)
        sw      $t6, 0x0C($t3)

        # Apply: clear the held bits (the pad word is active-low) and force
        # their pressure bytes to 0xFF.
        lhu     $t5, 2($t2)
        nor     $t0, $t6, $zero
        and     $t5, $t5, $t0
        sh      $t5, 2($t2)
        lui     $t9, %hi(pressure_offsets)
        addiu   $t9, $t9, %lo(pressure_offsets)
        addiu   $t4, $zero, 4
        addiu   $t1, $zero, 0xFF
press_loop:
        srlv    $t8, $t6, $t4
        andi    $t8, $t8, 1
        beq     $t8, $zero, press_next
        addu    $t8, $t9, $t4           # (delay) &pressure_offsets[bit]
        lbu     $t8, 0($t8)
        addu    $t8, $t2, $t8
        sb      $t1, 0($t8)
press_next:
        addiu   $t4, $t4, 1
        slti    $t8, $t4, 16
        bne     $t8, $zero, press_loop
        nop

        # Log record.
        lw      $t4, 0x14($t3)          # capacity
        beq     $t4, $zero, log_done
        nop
        lw      $t5, 0x10($t3)          # records written
        divu    $zero, $t5, $t4
        mfhi    $t0                     # slot
        lw      $t1, 0x18($t3)          # record size
        mult    $zero, $t0, $t1
        mflo    $t0
        lui     $t8, %hi(LOG)
        addiu   $t8, $t8, %lo(LOG)
        addu    $t8, $t8, $t0
        sw      $t7, 0($t8)
        lw      $t0, 0x28($t3)
        sw      $t0, 4($t8)
        addiu   $t8, $t8, 8
        lw      $t4, 0x24($t3)          # watch entries
        addiu   $t9, $t3, 0x40
watch_loop:
        blez    $t4, watch_done
        nop
        lw      $t0, 0($t9)             # address
        lw      $t1, 4($t9)             # words
copy_loop:
        blez    $t1, copy_done
        nop
        lw      $t6, 0($t0)
        sw      $t6, 0($t8)
        addiu   $t0, $t0, 4
        addiu   $t8, $t8, 4
        b       copy_loop
        addiu   $t1, $t1, -1            # (delay)
copy_done:
        addiu   $t9, $t9, 8
        b       watch_loop
        addiu   $t4, $t4, -1            # (delay)
watch_done:
        addiu   $t5, $t5, 1
        sw      $t5, 0x10($t3)
log_done:

        # Freeze.
        lw      $t4, 0x1C($t3)
        beq     $t4, $zero, done
        sltu    $t5, $t7, $t4           # (delay)
        bne     $t5, $zero, done
        nop
        sw      $t7, 0x20($t3)
spin:
        lw      $t5, 0x1C($t3)
        beq     $t5, $t4, spin          # until the host writes another value
        nop
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
