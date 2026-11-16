.text
.globl _main
.p2align 2
_main:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    adr x8, Ltarget
    // BLRAAZ x8. Deliberately skip PACIZA so the guest must reject the target.
    .long 0xd63f091f
    mov w0, #42
    ldp x29, x30, [sp], #16
    ret
Ltarget:
    ret
