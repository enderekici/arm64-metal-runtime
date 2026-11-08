.text
#ifndef ARM64_GUEST_NATIVE_TEST
.globl _main
_main:
#endif
.globl _guest_integer_divide_main
_guest_integer_divide_main:
    mov x1, #42
    mov x2, #0
    udiv x4, x1, x2
    cmp x4, #0
    b.ne .Lfail

    movz x1, #0x8000, lsl #48
    movn x2, #0
    sdiv x4, x1, x2
    cmp x4, x1
    b.ne .Lfail

    movz w1, #0x8000, lsl #16
    movn w2, #0
    sdiv w4, w1, w2
    cmp w4, w1
    b.ne .Lfail

    movn w1, #0
    mov w2, #3
    udiv w4, w1, w2
    movz w3, #0x5555
    movk w3, #0x5555, lsl #16
    cmp w4, w3
    b.ne .Lfail

    movn x1, #6
    mov x2, #3
    sdiv x4, x1, x2
    movn x3, #1
    cmp x4, x3
    b.ne .Lfail

    udiv x4, x1, xzr
    cmp x4, #0
    b.ne .Lfail
    udiv xzr, x1, x2

    mov x0, #42
    ret
.Lfail:
    mov x0, #1
    ret
