.text
.global _main
.p2align 2
_main:
    sub sp, sp, #32
    add x6, sp, #16
    movz x0, #0x123
    str x0, [sp, #16]!
    movz x1, #0
    ldr x1, [sp], #-16
    cmp x1, #0x123
    b.ne .Lfailure
    movz w2, #0xabc
    stur w2, [x6, #-12]
    ldur w3, [x6, #-12]
    cmp w3, #0xabc
    b.ne .Lfailure
    str w2, [x6, #4]!
    ldr w3, [x6], #-4
    cmp w3, #0xabc
    b.ne .Lfailure
    str w2, [x6, #-4]!
    ldr w3, [x6], #4
    cmp w3, #0xabc
    b.ne .Lfailure
    stur x0, [x6, #-16]
    ldur x4, [x6, #-16]
    cmp x4, #0x123
    b.ne .Lfailure
    str wzr, [x6, #-4]!
    ldr w5, [x6], #4
    cbnz w5, .Lfailure
    strb w2, [x6, #1]
    ldrb w3, [x6, #1]
    cmp w3, #0xbc
    b.ne .Lfailure
    strh w2, [x6, #2]
    ldrh w3, [x6, #2]
    cmp w3, #0xabc
    b.ne .Lfailure
    sturb w2, [x6, #-1]
    ldurb w3, [x6, #-1]
    cmp w3, #0xbc
    b.ne .Lfailure
    sturh w2, [x6, #-2]
    ldurh w3, [x6, #-2]
    cmp w3, #0xabc
    b.ne .Lfailure
    strb w2, [x6, #1]!
    ldrb w3, [x6], #-1
    cmp w3, #0xbc
    b.ne .Lfailure
    strh w2, [x6], #2
    ldrh w3, [x6, #-2]!
    cmp w3, #0xabc
    b.ne .Lfailure
    strb wzr, [sp, #2]
    ldrb w7, [sp, #2]
    cbnz w7, .Lfailure
    strh wzr, [sp, #4]
    ldrh w7, [sp, #4]
    cbnz w7, .Lfailure
    strb wzr, [sp, #16]!
    ldrb w7, [sp], #-16
    cbnz w7, .Lfailure
    add sp, sp, #32
    mov w0, #42
    ret
.Lfailure:
    add sp, sp, #32
    mov w0, #99
    ret
