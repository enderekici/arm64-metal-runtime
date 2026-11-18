.text
.globl _main
_main:
    sub sp, sp, #64
    mov w15, #42
    mov x11, sp
    add x11, x11, #32

    mov w9, #128
    strb w9, [sp]
    ldapursb x10, [sp]
    mov x12, #-128
    cmp x10, x12
    csel w15, w15, wzr, eq
    ldapurb w10, [x11, #-32]
    mov w12, #128
    cmp w10, w12
    csel w15, w15, wzr, eq
    mov w9, #127
    strb w9, [sp, #1]
    ldapursb w10, [sp, #1]
    mov w12, #127
    cmp w10, w12
    csel w15, w15, wzr, eq

    mov w9, #0x8000
    strh w9, [sp, #2]
    ldapursh w10, [sp, #2]
    mov w12, #-32768
    cmp w10, w12
    csel w15, w15, wzr, eq
    ldapursh x10, [sp, #2]
    mov x12, #-32768
    cmp x10, x12
    csel w15, w15, wzr, eq
    ldapurh w10, [sp, #2]
    mov w12, #0x8000
    cmp w10, w12
    csel w15, w15, wzr, eq

    movz w9, #0x8000, lsl #16
    str w9, [sp, #4]
    ldapursw x10, [sp, #4]
    mov x12, #-2147483648
    cmp x10, x12
    csel w15, w15, wzr, eq
    ldapur w10, [sp, #4]
    movz w12, #0x8000, lsl #16
    cmp w10, w12
    csel w15, w15, wzr, eq

    movz x9, #0x89ab
    movk x9, #0xcdef, lsl #16
    movk x9, #0x4567, lsl #32
    movk x9, #0x0123, lsl #48
    stlur x9, [sp, #8]
    ldapur x10, [sp, #8]
    cmp x10, x9
    csel w15, w15, wzr, eq

    mov w9, #0xa5
    stlurb w9, [sp, #-1]
    ldrb w10, [sp, #-1]
    cmp w10, w9
    csel w15, w15, wzr, eq
    mov w9, #0x5a
    stlurh w9, [sp, #16]
    ldrh w10, [sp, #16]
    cmp w10, w9
    csel w15, w15, wzr, eq
    mov w9, #0x1234
    stlur w9, [sp, #20]
    ldr w10, [sp, #20]
    cmp w10, w9
    csel w15, w15, wzr, eq

    stlur xzr, [sp, #24]
    ldr x10, [sp, #24]
    cmp x10, xzr
    csel w15, w15, wzr, eq
    add sp, sp, #64
    mov x0, x15
    ret
