.text
.p2align 2
.globl _main
_main:
    movz x20, #0
    add x23, sp, wzr, uxtw
    movz w1, #0x80
    add x2, x20, w1, uxtb
    cmp x2, #0x80
    b.ne .Lfail
    add x3, x20, w1, sxtb
    movn x4, #127
    cmp x3, x4
    b.ne .Lfail
    cmn x3, #128
    b.ne .Lfail

    movz w5, #0x8000, lsl #16
    add x6, x20, w5, uxtw
    add x7, x20, w5, sxtw
    movz x8, #0x8000, lsl #16
    cmp x6, x8
    b.ne .Lfail
    movn x9, #0
    movk x9, #0x8000, lsl #16
    movk x9, #0
    cmp x7, x9
    b.ne .Lfail

    movz x10, #3
    add x11, x20, x10, uxtx #4
    cmp x11, #48
    b.ne .Lfail
    add x12, x20, x10, sxtx #1
    cmp x12, #6
    b.ne .Lfail
    add w13, w20, w1, sxtb #1
    cmn w13, #256
    b.ne .Lfail

    movz x14, #7
    movz x15, #3
    adds x16, x14, w15, uxtw #1
    cmp x16, #13
    b.ne .Lfail
    subs xzr, x14, w15, uxtw
    b.cc .Lfail
    b.eq .Lfail
    cmn x14, w15, sxtw
    b.eq .Lfail
    cmp x14, w15, sxtw
    b.cc .Lfail
    b.eq .Lfail

    // Extended arithmetic reads and writes SP, including WSP.
    add x24, sp, w15, uxtw #2
    sub x24, x24, w15, uxtw #2
    cmp x24, x23
    b.ne .Lfail
    add sp, sp, w15, uxtw
    sub sp, sp, w15, uxtw
    add wsp, wsp, w15, uxtw
    mov sp, x23

    movz w0, #42
    ret
.Lfail:
    mov sp, x23
    movz w0, #99
    ret
