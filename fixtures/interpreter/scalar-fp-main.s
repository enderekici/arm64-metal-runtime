.text
.globl _main
_main:
    movz x8, #1
    movk x8, #0x200, lsl #32
    ucvtf d0, x8
    fmov d1, #0.75
    frintp d2, d1
    mov w0, #42
    ret
