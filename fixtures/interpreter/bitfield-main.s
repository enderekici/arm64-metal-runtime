.text
#ifndef ARM64_GUEST_NATIVE_TEST
.globl _main
_main:
#endif
.globl _guest_bitfield_main
_guest_bitfield_main:
    movz x1, #0xcdef
    movk x1, #0x89ab, lsl #16
    movk x1, #0x4567, lsl #32
    movk x1, #0x0123, lsl #48

    ubfm x2, x1, #60, #7
    mov x3, #0xef0
    cmp x2, x3
    b.ne .Lfail

    sbfm x4, x1, #60, #7
    movn x5, #0x10f
    cmp x4, x5
    b.ne .Lfail

    mov x6, #0x123
    bfm x6, x1, #60, #7
    mov x7, #0xef3
    cmp x6, x7
    b.ne .Lfail

    movz w8, #0xcdef
    movk w8, #0x89ab, lsl #16
    ubfm w9, w8, #28, #7
    cmp x9, #0xef0
    b.ne .Lfail

    mov x0, #42
    ret
.Lfail:
    mov x0, #1
    ret
