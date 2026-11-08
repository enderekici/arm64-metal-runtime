.text
.globl _main
_main:
    sub sp, sp, #32
    mov w15, #42

    mov w9, #-1
    str w9, [sp]
    movz w10, #0x8000, lsl #16
    ldumin w10, w11, [sp]
    mov w12, #-1
    cmp x11, x12
    csel w15, w15, wzr, eq
    ldr w13, [sp]
    cmp w13, w10
    csel w15, w15, wzr, eq

    movz w9, #0x8000, lsl #16
    str w9, [sp]
    mov w10, #-1
    ldumax w10, w11, [sp]
    cmp x11, x9
    csel w15, w15, wzr, eq
    ldr w13, [sp]
    cmp w13, w10
    csel w15, w15, wzr, eq

    mov x9, #-1
    str x9, [sp, #8]
    add x14, sp, #8
    movz x10, #0x8000, lsl #48
    ldumin x10, x11, [x14]
    mov x12, #-1
    cmp x11, x12
    csel x15, x15, xzr, eq
    ldr x13, [sp, #8]
    cmp x13, x10
    csel x15, x15, xzr, eq

    movz x9, #0x8000, lsl #48
    str x9, [sp, #8]
    add x14, sp, #8
    mov x10, #-1
    ldumax x10, x11, [x14]
    cmp x11, x9
    csel x15, x15, xzr, eq
    ldr x13, [sp, #8]
    cmp x13, x10
    csel x15, x15, xzr, eq

    mov w9, #100
    str w9, [sp, #4]
    add x14, sp, #4
    mov w10, #50
    ldumin w10, w10, [x14]
    mov w12, #100
    cmp x10, x12
    csel w15, w15, wzr, eq
    ldr w13, [sp, #4]
    mov w12, #50
    cmp w13, w12
    csel w15, w15, wzr, eq

    mov x9, #100
    str x9, [sp, #16]
    add x14, sp, #16
    mov x10, #50
    ldumin x10, x10, [x14]
    mov x12, #100
    cmp x10, x12
    csel x15, x15, xzr, eq
    ldr x13, [sp, #16]
    mov x12, #50
    cmp x13, x12
    csel x15, x15, xzr, eq

    movz w9, #0x8000, lsl #16
    str w9, [sp, #20]
    add x14, sp, #20
    mov x10, #0x7fffffff
    movk x10, #1, lsl #32
    ldumax w10, w11, [x14]
    cmp x11, x9
    csel x15, x15, xzr, eq
    ldr w13, [sp, #20]
    cmp w13, w9
    csel w15, w15, wzr, eq

    add sp, sp, #32
    mov x0, x15
    ret
