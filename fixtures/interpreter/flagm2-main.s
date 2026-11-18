.text
.p2align 2
.globl _main

// Compare NZCV to the architectural nibble at bit offset X13 in the table.
.macro check_table table
    cset w9, mi
    cset w10, eq
    orr w9, w10, w9, lsl #1
    cset w10, cs
    orr w9, w10, w9, lsl #1
    cset w10, vs
    orr w9, w10, w9, lsl #1
    lsrv x10, \table, x13
    and w10, w10, #0xf
    cmp w9, w10
    b.ne Lfail
.endm

_main:
    // Native MSR NZCV / AXFLAG or XAFLAG / MRS NZCV for inputs 0..15.
    movz x14, #0x4240
    movk x14, #0x4644, lsl #16
    movk x14, #0x4240, lsl #32
    movk x14, #0x4644, lsl #48
    movz x15, #0x2288
    movk x15, #0x6633, lsl #16
    movk x15, #0x2288, lsl #32
    movk x15, #0x6633, lsl #48

    mov w12, #0
Lnext:
    lsl w13, w12, #2
    rmif x12, #0, #15
    axflag
    check_table x14
    rmif x12, #0, #15
    xaflag
    check_table x15
    add w12, w12, #1
    cmp w12, #16
    b.ne Lnext
    mov x0, #42
    ret
Lfail:
    mov x0, #1
    ret
