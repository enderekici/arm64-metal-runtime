.text
.p2align 2
.globl _main
_main:
    movz x1, #0
    movz x2, #1

    // ADC consumes carry-in and preserves the flags used by the branch.
    cmp xzr, xzr
    adc x3, x1, x2
    b.eq .Ladc_flags_preserved
    b .Lfail
.Ladc_flags_preserved:
    cmp x3, #2
    b.ne .Lfail

    // SBC consumes a clear carry as a borrow and preserves the N flag.
    movz x10, #0
    subs x11, x10, #1
    sbc x4, x1, x1
    b.mi .Lsbc_flags_preserved
    b .Lfail
.Lsbc_flags_preserved:
    movn x12, #0
    cmp x4, x12
    b.ne .Lfail

    // The exact fixture instruction aliases Rd, Rn and Rm.
    movz x9, #0
    subs xzr, x10, #1
    sbcs x9, x9, x9
    b.cs .Lfail
    b.pl .Lfail
    cmp x9, x12
    b.ne .Lfail

    // ADCS consumes a clear carry, wraps signed max to signed min, and sets V.
    movn x13, #0x8000, lsl #48
    movz x14, #1
    movz x15, #0
    subs xzr, x15, #1
    adcs x16, x13, x14
    b.vs .Ladcs_overflow
    b .Lfail
.Ladcs_overflow:
    b.mi .Lpass
    b .Lfail
.Lpass:
    movz w0, #42
    ret
.Lfail:
    movz w0, #99
    ret
