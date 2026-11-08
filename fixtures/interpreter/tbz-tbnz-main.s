.text
.globl _main
_main:
    movz x0, #0
    movz w1, #0x8000, lsl #16
    tbz w1, #31, .Lbad
    tbnz w1, #31, .Lwbit_ok
.Lbad:
    movz x0, #91
    ret
.Lwbit_ok:
    movz x2, #1, lsl #32
    tbz w2, #0, .Lzero_w
    movz x0, #92
    ret
.Lzero_w:
    movz x3, #1, lsl #48
    tbz x3, #48, .Lbad
    tbnz x3, #48, .Lxbit_ok
    movz x0, #93
    ret
.Lxbit_ok:
    tbz xzr, #63, .Lzero_xzr
    movz x0, #94
    ret
.Lzero_xzr:
    tbnz xzr, #63, .Lbad
    movz x0, #42
    ret
