.text
.p2align 2
.globl _main
_main:
    movz x1, #0x1234
    and x2, x1, #0xff
    subs xzr, x2, #0x34
    b.ne .Lfail
    orr x3, xzr, #0x8000000000000001
    eor x4, x3, #0x8000000000000001
    cbnz x4, .Lfail
    ands x11, x3, #0x8000000000000000
    cbz x11, .Lfail
    orr x9, xzr, #0xff00ff00ff00ff00
    b.mi .Lnegative
    b .Lfail
.Lnegative:
    movz w5, #0x00ab
    eor w6, w5, #0x00ff00ff
    ands wzr, w6, #8
    b.eq .Lzero
    b .Lfail
.Lzero:
    orr w7, wzr, #0x00ff00ff
    tst w7, #0x00ff00ff
    b.ne .Lnonzero
    b .Lfail
.Lnonzero:
    and w8, w7, #0xff
    subs wzr, w8, #0xff
    b.ne .Lfail
    and x10, xzr, #1
    cbnz x10, .Lfail
    ands xzr, xzr, #1
    b.eq .Lsuccess
    b .Lfail
.Lsuccess:
    movz w0, #42
    ret
.Lfail:
    movz w0, #99
    ret
