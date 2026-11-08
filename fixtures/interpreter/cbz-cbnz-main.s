.text
.globl _main
_main:
    movz x0, #0
    cbz x0, .Lzero_x_ok
    movz x0, #95
.Lzero_x_ok:
    cbnz x0, .Lnonzero_x_bad
    b .Lafter_zero
.Lnonzero_x_bad:
    movz x0, #96
.Lafter_zero:
    movz x1, #1, lsl #32
    cbz w1, .Lzero_w_ok
    movz x0, #91
.Lzero_w_ok:
    cbnz w1, .Lnonzero_w_bad
    b .Lafter_w
.Lnonzero_w_bad:
    movz x0, #92
.Lafter_w:
    cbz x1, .Lzero_x_bad
    b .Lafter_x
.Lzero_x_bad:
    movz x0, #93
.Lafter_x:
    cbnz x1, .Lnonzero_x_ok
    movz x0, #94
.Lnonzero_x_ok:
    movz w2, #1
    cbnz w2, .Lnonzero_w_ok
    movz x0, #90
.Lnonzero_w_ok:
    cbz w2, .Lzero_w_bad
    b .Lafter_w2
.Lzero_w_bad:
    movz x0, #89
.Lafter_w2:
    movz x0, #42
    ret
