.text
.p2align 2
.globl _main
_main:
    movz x1, #7
    movz x2, #9
    subs xzr, x1, #7
    csel x3, x1, x2, eq
    cmp x3, #7
    b.ne .Lfail
    csel w22, w1, w2, eq
    cmp w22, #7
    b.ne .Lfail

    cmp x1, #8
    csinc w4, w1, wzr, eq
    b.eq .Lfail
    cmp w4, #1
    b.ne .Lfail
    cmp x1, #8
    csinc x22, x1, x2, eq
    cmp x22, #10
    b.ne .Lfail

    cmp x1, #8
    csinv x5, x1, xzr, eq
    b.eq .Lfail
    movn x6, #0
    cmn x5, #1
    b.ne .Lfail
    cmp x1, #8
    csinv w23, w1, w2, eq
    movn w24, #9
    cmn w23, #10
    b.ne .Lfail

    cmp x1, #8
    csneg w7, w1, w1, eq
    b.eq .Lfail
    movz w8, #0xfff9
    movk w8, #0xffff, lsl #16
    cmn w7, #7
    b.ne .Lfail

    cmp x1, #8
    csneg x9, x1, x1, eq
    b.eq .Lfail
    movn x10, #6
    cmn x9, #7
    b.ne .Lfail

    movz x11, #1, lsl #48
    movz w12, #0xffff
    movk w12, #0xffff, lsl #16
    cmp x1, #8
    csinc w11, w1, w12, eq
    b.eq .Lfail
    cbnz x11, .Lfail

    csel x13, x1, x2, al
    cmp x13, #7
    b.ne .Lfail
    csel x14, x1, x2, nv
    cmp x14, #7
    b.ne .Lfail
    cset x15, eq
    cmp x15, #1
    b.ne .Lfail
    csetm x16, eq
    cmn x16, #1
    b.ne .Lfail

    cmp x1, #7
    cinc x17, x1, eq
    cmp x17, #8
    b.ne .Lfail
    cmp x1, #7
    cinv x18, x1, eq
    movn x20, #7
    cmn x18, #8
    b.ne .Lfail
    cmp x1, #7
    cneg x21, x1, eq
    cmn x21, #7
    b.ne .Lfail

    cmp x1, #8
    csel xzr, x2, x1, eq
    b.eq .Lfail
    movz w0, #42
    ret
.Lfail:
    movz w0, #99
    ret
