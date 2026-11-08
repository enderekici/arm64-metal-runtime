.text
.global _main
.p2align 2
_main:
    sub sp, sp, #32
    add x6, sp, #16
    movz x0, #0x1234
    str x0, [sp, #16]!
    mov x1, #0
    ldr x1, [sp], #-16
    cmp x1, x0
    b.ne .Lfailure
    movz w2, #0xabcd
    stur w2, [x6, #-12]
    ldur w3, [x6, #-12]
    cmp w3, w2
    b.ne .Lfailure
    str w2, [x6, #4]!
    ldr w3, [x6], #-4
    cmp w3, w2
    b.ne .Lfailure
    str w2, [x6, #-4]!
    ldr w3, [x6], #4
    cmp w3, w2
    b.ne .Lfailure
    stur x0, [x6, #-16]
    ldur x4, [x6, #-16]
    cmp x4, x0
    b.ne .Lfailure
    str wzr, [x6, #-4]!
    ldr w5, [x6], #4
    cbnz w5, .Lfailure
    add sp, sp, #32
    mov w0, #42
    ret
.Lfailure:
    add sp, sp, #32
    mov w0, #99
    ret
