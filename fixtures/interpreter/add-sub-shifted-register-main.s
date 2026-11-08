.text
.p2align 2
.globl _main
_main:
    movz x1, #0x1234
    movz x2, #3
    add x3, x1, x2, lsl #2
    movz x20, #0x1240
    cmp x3, x20
    b.ne .Lfail
    sub x4, x3, x2, lsr #1
    movz x21, #0x123f
    cmp x4, x21
    b.ne .Lfail

    // ASR uses the sign bit and W destinations zero-extend.
    movn w5, #0
    add w6, w5, w5, asr #31
    cmn w6, #2
    b.ne .Lfail
    adds xzr, x6, xzr
    b.mi .Lfail
    movn x7, #0
    add w7, wzr, w5
    adds xzr, x7, xzr
    b.mi .Lfail

    // Register 31 is ZR in every shifted-register operand position.
    add x8, xzr, x2, lsl #1
    cmp x8, #6
    b.ne .Lfail
    movz x9, #5
    add xzr, x9, x2
    cmp x9, #5
    b.ne .Lfail

    // A non-flag-setting form preserves NZCV.
    cmp x1, #8
    add x10, x1, x2
    b.eq .Lfail

    // ADDS overflow and SUBS carry/borrow are consumed by typed B.cond.
    movz x11, #0xffff
    movk x11, #0xffff, lsl #16
    movk x11, #0xffff, lsl #32
    movk x11, #0x7fff, lsl #48
    adds x12, x11, x2
    b.vs .Loverflow
    b .Lfail
.Loverflow:
    b.pl .Lfail
    subs xzr, x2, x1
    b.cs .Lfail
    b.mi .Lborrow
    b .Lfail
.Lborrow:
    cmp x3, x4
    b.cc .Lfail
    b.eq .Lfail

    // CMP/CMN shifted-register aliases write only NZCV.
    movn x13, #0
    cmn x13, x2
    b.cc .Lfail
    b.eq .Lfail
    cmp x3, x4, lsl #0
    b.ne .Lcontinue
    b .Lfail
.Lcontinue:
    // W-sized boundary shifts truncate to 32 bits.
    add w14, wzr, w2, lsl #31
    cmn w14, wzr
    b.pl .Lfail
    movz w22, #1
    cmp w22, w2
    b.cs .Lfail
    b.mi .Lwborrow
    b .Lfail
.Lwborrow:
    movz w15, #0x8000, lsl #16
    add w16, wzr, w15, lsr #31
    cmp w16, #1
    b.ne .Lfail
    movz x17, #1
    add x18, xzr, x17, lsl #63
    cmp x18, #0
    b.eq .Lfail

    movz w0, #42
    ret
.Lfail:
    movz w0, #99
    ret
