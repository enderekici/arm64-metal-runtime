.text
.p2align 2
.globl _main
_main:
    movz x1, #4
    movk x1, #0x8000, lsl #48
    movz x2, #1

    // Variable shifts preserve NZCV and use the low six amount bits for X registers.
    cmp x1, #4
    lslv x3, x1, x2
    b.cc .Lfail
    cmp x3, #8
    b.ne .Lfail
    lsrv x4, x1, x2
    movz x5, #2
    movk x5, #0x4000, lsl #48
    cmp x4, x5
    b.ne .Lfail
    asrv x6, x1, x2
    movz x5, #2
    movk x5, #0xc000, lsl #48
    cmp x6, x5
    b.ne .Lfail
    rorv x7, x1, x2
    movz x5, #2
    movk x5, #0x4000, lsl #48
    cmp x7, x5
    b.ne .Lfail

    movz x8, #0x1001
    lsrv x9, x1, x8
    cmp x9, x5
    b.ne .Lfail
    rorv x10, x1, xzr
    cmp x10, x1
    b.ne .Lfail

    // W forms mask the amount to five bits and zero-extend the result to X.
    movz w11, #0x8000, lsl #16
    movz w12, #1
    lslv w13, w11, w12
    cmp x13, #0
    b.ne .Lfail
    lsrv w14, w11, w12
    movz w15, #0x4000, lsl #16
    cmp x14, x15
    b.ne .Lfail
    asrv w16, w11, w12
    movz w15, #0xc000, lsl #16
    cmp x16, x15
    b.ne .Lfail
    rorv w17, w11, w12
    movz w15, #0x4000, lsl #16
    cmp x17, x15
    b.ne .Lfail
    movz w18, #0x21
    lsrv w19, w11, w18
    cmp x19, x15
    b.ne .Lfail

    // Zero-register values and zero shift amounts leave the source unchanged or zero.
    asrv x20, xzr, xzr
    cmp x20, #0
    b.ne .Lfail
    lslv xzr, x1, xzr
    cmp x1, x10
    b.ne .Lfail

    movz w0, #42
    ret
.Lfail:
    movz w0, #99
    ret
