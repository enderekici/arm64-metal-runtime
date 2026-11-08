.text
#ifndef ARM64_GUEST_NATIVE_TEST
.globl _main
_main:
#endif
.globl _guest_integer_bitops_main
_guest_integer_bitops_main:
    mov x1, #1
    rbit xzr, x1
    rbit x2, x1
    mov x3, #0x8000000000000000
    cmp x2, x3
    b.ne .Lfail

    movz x1, #0x5678
    movk x1, #0x1234, lsl #16
    rev wzr, w1
    rev w2, w1
    movz w3, #0x3412
    movk w3, #0x7856, lsl #16
    cmp w2, w3
    b.ne .Lfail

    movz x1, #0xcdef
    movk x1, #0x89ab, lsl #16
    movk x1, #0x4567, lsl #32
    movk x1, #0x0123, lsl #48
    rev x2, x1
    movz x3, #0x2301
    movk x3, #0x6745, lsl #16
    movk x3, #0xab89, lsl #32
    movk x3, #0xefcd, lsl #48
    cmp x2, x3
    b.ne .Lfail

    clz x4, xzr
    cmp x4, #64
    b.ne .Lfail
    mov w1, #1
    clz w4, w1
    cmp w4, #31
    b.ne .Lfail
    mvn x1, xzr
    cls x4, x1
    cmp x4, #63
    b.ne .Lfail
    mov w1, #0
    cls w4, w1
    cmp w4, #31
    b.ne .Lfail

    mov x0, #42
    ret
.Lfail:
    mov x0, #1
    ret
