.text
.globl _main
_main:
    sub sp, sp, #64
    movz x0, #42
    movz x1, #1
    stp x0, x1, [sp, #16]
    movz x0, #0
    movz x1, #0
    ldp x2, x3, [sp, #16]
    stp x2, x3, [sp, #-16]!
    ldp x4, x5, [sp], #16
    stp x4, x5, [sp], #16
    ldp x6, x7, [sp, #-16]!
    stp w6, w7, [sp, #8]
    movz x6, #0
    movz x7, #0
    ldp w8, w9, [sp, #8]
    stp w8, w9, [sp, #-16]!
    ldp w10, w11, [sp], #16
    stp w10, w11, [sp], #16
    ldp w12, w13, [sp, #-16]!
    cbz w13, fail
    add w0, w12, #0
    add sp, sp, #64
    ret
fail:
    movz w0, #99
    add sp, sp, #64
    ret
